#include "x64arm/windows_services.hpp"
#include "x64arm/native_vulkan.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <vector>
#if defined(__APPLE__)
#include <dlfcn.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_metal.h>
#endif

namespace x64arm {
// 內建 vulkan-1.dll：客體呼叫轉給宿主 Vulkan loader（MoltenVK）。
// x64 共用宿主結構佈局，x86 依登錄檔產生 Win32 結構逐欄轉換，再共用指標修復流程；
// 結構中介資料與命令處理常式由 scripts/generate_vulkan_abi.py 自 vk.xml 產生（vulkan_generated.inc）。
// 對映記憶體以 map_shared 直接對映到客體位址，客體寫入即為裝置可見的 coherent 記憶體。
// 參考：https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html
#if defined(__APPLE__)
namespace vulkan_detail {
enum VulkanMemberKind : std::uint8_t {K_INLINE=1,K_PNEXT,K_STRUCT,K_SCALARS,K_STRING,K_STRINGS,K_OPAQUE,K_UNSUPPORTED};
enum VulkanArrayKind : std::uint8_t {A_BYTES,A_SIZE,A_DISPATCH};
struct VulkanMember {std::size_t offset;std::uint8_t kind,writable;std::uint32_t element;std::int32_t target;std::size_t len_offset;std::uint8_t len_width,len_mode;std::uint32_t len_add,len_div;std::uint8_t scalar_kind{};};
struct VulkanStruct {const char* name;std::size_t size,size32;VkStructureType stype;std::uint32_t first,count;};
struct VulkanCommandName {std::string_view name;unsigned index;};
#define X64ARM_VULKAN_STRUCTS
#define X64ARM_VULKAN_DESCRIPTORS
#include "vulkan_generated.inc"
#undef X64ARM_VULKAN_STRUCTS
#undef X64ARM_VULKAN_DESCRIPTORS
struct VulkanFunctions {
#define X64ARM_VULKAN_FIELD(name) PFN_##name name{};
    X64ARM_VULKAN_FUNCTIONS(X64ARM_VULKAN_FIELD)
#undef X64ARM_VULKAN_FIELD
};
}
struct VulkanState {
    void* library{};PFN_vkGetInstanceProcAddr loader{};bool attempted{},instance_loaded{};
    // 指標欄位位移表在建立狀態後第一次呼叫時一次算完（之後唯讀），worker 上的並行呼叫不會寫入。
    bool offsets_prepared{};
    vulkan_detail::VulkanFunctions fn;
    std::unordered_map<std::string_view,unsigned> commands;
    std::unordered_map<std::uint32_t,int> by_stype;
    std::unordered_map<std::string_view,int> by_name;
    std::vector<std::vector<std::size_t>> pointer_offsets;std::vector<bool> pointer_offsets_ready;
    struct Device {VkPhysicalDevice physical{};};
    std::unordered_map<std::uint64_t,Device> devices;
    struct Mapped {std::uint64_t device{},guest{};std::size_t size{};};
    std::unordered_map<std::uint64_t,Mapped> mapped;
    std::unordered_map<std::uint64_t,VkDeviceSize> allocations;
    struct Dispatch {std::uintptr_t host{};std::uint32_t parent{};};
    std::unordered_map<std::uint32_t,Dispatch> dispatch32;
    std::unordered_map<std::uintptr_t,std::uint32_t> reverse32;
    std::uint32_t next_dispatch32{0x76000000};
    struct Instance {
        PFN_vkCreateMetalSurfaceEXT create_surface{};
        bool win32{};
        std::shared_ptr<NativeVulkanSurface> probe_layer;
        VkSurfaceKHR probe{};
    };
    struct Surface {VkInstance instance{};std::uint64_t window{};void* view{};std::shared_ptr<NativeVulkanSurface> layer;};
    struct Swapchain {VkDevice device{};VkSurfaceKHR surface{};};
    std::unordered_map<VkInstance,Instance> instances;
    std::unordered_map<VkPhysicalDevice,VkInstance> physical_instances;
    std::unordered_map<VkSurfaceKHR,Surface> surfaces;
    std::unordered_map<VkSwapchainKHR,Swapchain> swapchains;
};
namespace vulkan_detail {
constexpr std::size_t host_page=16384,max_copy=std::size_t{256}<<20;
class VulkanCall;
void vulkan_unpack32(VulkanCall&,int,const std::uint8_t*,std::uint8_t*);
void vulkan_pack32(VulkanCall&,int,const std::uint8_t*,std::uint8_t*);
// 一次服務呼叫的轉換：配置宿主暫存、深層複製輸入結構，呼叫後把輸出寫回客體（保留客體原本的指標欄位）。
class VulkanCall {
public:
    struct Host {std::function<std::uint64_t(std::string_view)> resolve;std::function<std::uint64_t(void*,std::size_t)> map;std::function<void(std::uint64_t,std::size_t)> unmap;std::function<VulkanWindow(std::uint64_t)> window;};
    VulkanCall(GuestState& state,GuestMemory& memory,const WindowsCall& call,VulkanState& vulkan,Host host)
        :state_(state),memory_(memory),call_(call),vulkan_(vulkan),host_(std::move(host)),small_(memory.execution_mode()==ExecutionMode::X86) {}
    std::uint64_t arg(unsigned i) const {return call_(i);}
    std::uint32_t u32(unsigned i) const {return static_cast<std::uint32_t>(call_(i));}
    std::uint64_t u64(unsigned i) const {return call_(i);}
    float f32(unsigned i) const {
        // Win64：前四個參數的 float 在 XMM0–3，其餘在堆疊 slot 的低 32 位元。
        std::uint32_t raw=0;
        if(small_)raw=static_cast<std::uint32_t>(call_(i));
        else if(i<4)for(unsigned j=0;j<4;++j)raw|=std::uint32_t{state_.xmm[i][j]}<<(8*j);
        else raw=static_cast<std::uint32_t>(call_(i));
        float value;std::memcpy(&value,&raw,sizeof value);return value;
    }
    template<class T> T decode_handle(std::uint64_t value) const {
        if constexpr(vulkan_dispatchable<T>)if(small_ && value){const auto p=vulkan_.dispatch32.find(static_cast<std::uint32_t>(value));value=p==vulkan_.dispatch32.end()?0:p->second.host;}
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));
    }
    std::uint64_t encode_dispatch(std::uintptr_t value){
        if(!small_ || !value)return value;
        if(const auto p=vulkan_.reverse32.find(value);p!=vulkan_.reverse32.end())return p->second;
        if(!vulkan_.next_dispatch32)throw std::bad_alloc();const auto id=vulkan_.next_dispatch32++;
        const auto parent=static_cast<std::uint32_t>(arg(0));
        vulkan_.dispatch32.emplace(id,VulkanState::Dispatch{value,vulkan_.dispatch32.contains(parent)?parent:0});vulkan_.reverse32.emplace(value,id);return id;
    }
    template<class T> std::uint64_t encode_handle(T value){
        const auto raw=reinterpret_cast<std::uintptr_t>(value);if constexpr(vulkan_dispatchable<T>)return encode_dispatch(raw);else return raw;
    }
    template<class T> T handle(unsigned i) const {return decode_handle<T>(call_(i));}
    template<class T> void store_handle(std::uint64_t at,T value){if(at)memory_.store(GuestAddress{at},small_ && vulkan_dispatchable<T>?32:64,encode_handle(value));}
    void forget_dispatch(std::uint64_t guest){
        if(!small_)return;const auto id=static_cast<std::uint32_t>(guest);
        std::vector<std::uint32_t> children;for(const auto& [key,item]:vulkan_.dispatch32)if(item.parent==id)children.push_back(key);
        for(const auto child:children)forget_dispatch(child);
        if(const auto p=vulkan_.dispatch32.find(id);p!=vulkan_.dispatch32.end()){vulkan_.reverse32.erase(p->second.host);vulkan_.dispatch32.erase(p);}
    }
    VulkanFunctions& fn() {return vulkan_.fn;}
    VulkanState& vulkan() {return vulkan_;}
    GuestMemory& memory() {return memory_;}
    Host& host() {return host_;}
    std::uint64_t deref(unsigned i,unsigned width,unsigned kind=A_BYTES) const {const auto at=call_(i);return at?memory_.load(GuestAddress{at},static_cast<std::uint8_t>((small_ && kind!=A_BYTES?4:width)*8)):0;}
    std::uint64_t member(unsigned i,std::size_t offset,std::size_t offset32,unsigned width,unsigned kind=A_BYTES) const {const auto at=call_(i);return at?memory_.load(GuestAddress{at+(small_?offset32:offset)},static_cast<std::uint8_t>((small_ && kind!=A_BYTES?4:width)*8)):0;}
    void* in_bytes(unsigned i,std::size_t size) {return raw(call_(i),size,false);}
    void* out_bytes(unsigned i,std::size_t size) {return raw(call_(i),size,true);}
    void* inout_bytes(unsigned i,std::size_t size) {return raw(call_(i),size,true);}
    void* in_array(unsigned i,std::size_t count,unsigned width,unsigned kind){return array(call_(i),count,width,kind,false);}
    void* out_array(unsigned i,std::size_t count,unsigned width,unsigned kind){return array(call_(i),count,width,kind,true);}
    const char* in_string(unsigned i) {return string(call_(i));}
    void* in_struct(unsigned i,int id,std::uint64_t count) {return structure(call_(i),id,static_cast<std::size_t>(count),false);}
    void* out_struct(unsigned i,int id,std::uint64_t count) {return structure(call_(i),id,static_cast<std::size_t>(count),true);}
    std::uint8_t* allocate(std::size_t size) {blocks_.push_back(std::make_unique<std::uint8_t[]>(std::max<std::size_t>(size,1)));return blocks_.back().get();}
    void store64(std::uint64_t at,std::uint64_t value) {if(at)memory_.store(GuestAddress{at},64,value);}
    void store32(std::uint64_t at,std::uint32_t value) {if(at)memory_.store(GuestAddress{at},32,value);}
    void store_pointer(std::uint64_t at,std::uint64_t value){if(at)memory_.store(GuestAddress{at},small_?32:64,value);}
    bool small()const{return small_;}
    void finish() {
        // 輸出結構整筆寫回，但指標欄位恢復為客體原值；被指向的輸出資料各自另有寫回項目。
        for(auto& item:writebacks_){
            if(small_ && item.id>=0){
                const auto& info=vulkan_structs[item.id];
                for(std::size_t k=0;k<item.count;++k)vulkan_pack32(*this,item.id,item.host+k*info.size,item.original.data()+k*info.size32);
                memory_.write(GuestAddress{item.guest},item.original);continue;
            }
            if(small_ && item.scalar_kind!=A_BYTES){
                for(std::size_t k=0;k<item.count;++k){std::uint64_t value{};std::memcpy(&value,item.host+k*8,8);
                    if(item.scalar_kind==A_DISPATCH)value=encode_dispatch(static_cast<std::uintptr_t>(value));
                    memory_.store(GuestAddress{item.guest+k*4},32,value);}
                continue;
            }
            if(item.id>=0){
                const auto& info=vulkan_structs[item.id];
                for(std::size_t k=0;k<item.count;++k)for(const auto offset:pointer_offsets(item.id))
                    std::memcpy(item.host+k*info.size+offset,item.original.data()+k*info.size+offset,8);
            }
            memory_.write(GuestAddress{item.guest},std::span<const std::uint8_t>(item.host,item.size));
        }
        writebacks_.clear();
    }
    std::optional<std::uint32_t> done(std::uint64_t value,bool wide=false) {result_=value;call_.return_to(state_,value,wide);return std::optional<std::uint32_t>{};}
    std::uint64_t result() const {return result_;}
    int struct_id(std::string_view name) const {const auto found=vulkan_.by_name.find(name);return found==vulkan_.by_name.end()?-1:found->second;}
    void prepare_pointer_offsets(){for(int i=0;i<static_cast<int>(std::size(vulkan_structs));++i)(void)pointer_offsets(i);vulkan_.offsets_prepared=true;}
private:
    struct Writeback {std::uint64_t guest;std::uint8_t* host;std::size_t size;int id;std::size_t count;std::vector<std::uint8_t> original;unsigned scalar_kind{};};
    bool read(std::uint64_t guest,std::uint8_t* host,std::size_t size) {
        // 規格允許部分被忽略的指標為任意值（例如 VkWriteDescriptorSet 依類型只用其中一組），讀不到時視為 NULL。
        try{memory_.read(GuestAddress{guest},std::span<std::uint8_t>(host,size));return true;}catch(const MemoryFault&){return false;}
    }
    void* raw(std::uint64_t guest,std::size_t size,bool output) {
        if(!guest || size>max_copy)return nullptr;
        auto* host=allocate(size);if(size && !read(guest,host,size))return nullptr;
        if(output && size)writebacks_.push_back({guest,host,size,-1,0,{}});
        return host;
    }
    void* array(std::uint64_t guest,std::size_t count,unsigned width,unsigned kind,bool output){
        if(!width || count>max_copy/width)return nullptr;
        if(!small_ || kind==A_BYTES)return raw(guest,count*width,output);
        if(!guest || width!=8)return nullptr;
        std::vector<std::uint8_t> original(count*4);if(!read(guest,original.data(),original.size()))return nullptr;
        auto* host=allocate(count*8);
        for(std::size_t i=0;i<count;++i){std::uint32_t value{};std::memcpy(&value,original.data()+i*4,4);std::uint64_t expanded=value;
            if(kind==A_DISPATCH){const auto p=vulkan_.dispatch32.find(value);expanded=p==vulkan_.dispatch32.end()?0:p->second.host;}
            std::memcpy(host+i*8,&expanded,8);}
        if(output && count)writebacks_.push_back({guest,host,count*8,-1,count,std::move(original),kind});return host;
    }
    const char* string(std::uint64_t guest) {
        if(!guest)return nullptr;std::string text;
        for(std::uint64_t at=guest;text.size()<65536;){
            std::span<const std::uint8_t> chunk;try{chunk=memory_.readable_prefix(GuestAddress{at},256);}catch(const MemoryFault&){return nullptr;}
            if(chunk.empty())return nullptr;
            const auto end=std::find(chunk.begin(),chunk.end(),std::uint8_t{0});
            text.append(chunk.begin(),end);if(end!=chunk.end())break;at+=chunk.size();
        }
        auto* host=allocate(text.size()+1);std::memcpy(host,text.data(),text.size());return reinterpret_cast<const char*>(host);
    }
    const std::vector<std::size_t>& pointer_offsets(int id) {
        auto& offsets=vulkan_.pointer_offsets[static_cast<std::size_t>(id)];
        if(vulkan_.pointer_offsets_ready[static_cast<std::size_t>(id)])return offsets;
        vulkan_.pointer_offsets_ready[static_cast<std::size_t>(id)]=true;
        const auto& info=vulkan_structs[id];
        for(std::uint32_t m=info.first;m<info.first+info.count;++m){
            const auto& member=vulkan_members[m];
            if(member.kind!=K_INLINE){offsets.push_back(member.offset);continue;}
            const auto nested=pointer_offsets(member.target);const auto size=vulkan_structs[member.target].size;
            auto& merged=vulkan_.pointer_offsets[static_cast<std::size_t>(id)];
            for(std::uint32_t k=0;k<member.element;++k)for(const auto offset:nested)merged.push_back(member.offset+k*size+offset);
        }
        return offsets;
    }
    std::size_t length(const std::uint8_t* host,const VulkanMember& member) const {
        if(member.len_mode==0)return 1;
        if(member.len_mode==3)return member.len_offset;
        if(member.len_mode!=1)return 0;
        std::uint64_t value=0;std::memcpy(&value,host+member.len_offset,std::min<std::size_t>(member.len_width,8));
        if(member.len_width<8)value&=(UINT64_C(1)<<(member.len_width*8))-1;
        return static_cast<std::size_t>((value+member.len_add)/member.len_div);
    }
    void* chain(std::uint64_t guest,bool output) {
        // 依 sType 找到已知結構；未知（平台專用）或需客體回呼的結構略過，接續其 pNext。
        for(unsigned depth=0;guest && depth<64;++depth){
            std::uint32_t stype=0;std::uint64_t next=0;
            if(!read(guest,reinterpret_cast<std::uint8_t*>(&stype),4) || !read(guest+(small_?4:8),reinterpret_cast<std::uint8_t*>(&next),small_?4:8))return nullptr;
            const bool blocked=stype==VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT || stype==VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT ||
                stype==VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT || stype==VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
            const auto found=vulkan_.by_stype.find(stype);
            if(!blocked && found!=vulkan_.by_stype.end())return structure(guest,found->second,1,output);
            guest=next;
        }
        return nullptr;
    }
    void* structure(std::uint64_t guest,int id,std::size_t count,bool output) {
        if(!guest || id<0)return nullptr;
        const auto& info=vulkan_structs[id];if(count && info.size>max_copy/count)return nullptr;
        const auto size=info.size*count;auto* host=allocate(size);
        std::vector<std::uint8_t> original;
        if(small_){
            original.resize(info.size32*count);if(!read(guest,original.data(),original.size()))return nullptr;
            for(std::size_t k=0;k<count;++k)vulkan_unpack32(*this,id,original.data()+k*info.size32,host+k*info.size);
        }else{if(size && !read(guest,host,size))return nullptr;if(output)original.assign(host,host+size);}
        if(output && size)writebacks_.push_back({guest,host,size,id,count,std::move(original)});
        for(std::size_t k=0;k<count;++k)fix(id,host+k*info.size,output);
        return host;
    }
    void fix(int id,std::uint8_t* host,bool output) {
        const auto& info=vulkan_structs[id];
        for(std::uint32_t m=info.first;m<info.first+info.count;++m){
            const auto& member=vulkan_members[m];auto* slot=host+member.offset;
            if(member.kind==K_INLINE){for(std::uint32_t k=0;k<member.element;++k)fix(member.target,slot+k*vulkan_structs[member.target].size,output);continue;}
            if(member.kind==K_OPAQUE)continue;
            std::uint64_t guest=0;std::memcpy(&guest,slot,8);void* pointer=nullptr;
            const bool writes=output && member.writable;
            if(guest)switch(member.kind){
            case K_PNEXT:pointer=chain(guest,writes);break;
            case K_STRUCT:pointer=structure(guest,member.target,length(host,member),writes);break;
            case K_SCALARS:pointer=array(guest,length(host,member),member.element,member.scalar_kind,writes);break;
            case K_STRING:pointer=const_cast<char*>(string(guest));break;
            case K_STRINGS:{
                const auto count=length(host,member);if(count>4096)break;
                auto* names=reinterpret_cast<const char**>(allocate(count*sizeof(char*)));
                for(std::size_t k=0;k<count;++k){std::uint64_t entry=0;if(read(guest+k*(small_?4:8),reinterpret_cast<std::uint8_t*>(&entry),small_?4:8))names[k]=string(entry);}
                pointer=names;break;
            }
            default:break;
            }
            std::memcpy(slot,&pointer,8);
        }
    }
    GuestState& state_;GuestMemory& memory_;const WindowsCall& call_;VulkanState& vulkan_;Host host_;bool small_{};std::uint64_t result_{};
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks_;std::vector<Writeback> writebacks_;
};
#define X64ARM_VULKAN_CONVERSIONS32
#include "vulkan_generated.inc"
#undef X64ARM_VULKAN_CONVERSIONS32
bool load(VulkanState& vulkan) {
    if(vulkan.attempted)return vulkan.loader!=nullptr;
    vulkan.attempted=true;
    std::vector<std::string> candidates;
    if(const char* configured=std::getenv("X64ARM_VULKAN_LIBRARY"))candidates.emplace_back(configured);
    for(const char* path:{"libvulkan.1.dylib","/opt/homebrew/lib/libvulkan.1.dylib","/usr/local/lib/libvulkan.1.dylib","libMoltenVK.dylib","/opt/homebrew/lib/libMoltenVK.dylib","/usr/local/lib/libMoltenVK.dylib"})candidates.emplace_back(path);
    for(const auto& path:candidates){
        vulkan.library=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);if(!vulkan.library)continue;
        vulkan.loader=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan.library,"vkGetInstanceProcAddr"));if(vulkan.loader)break;
        dlclose(vulkan.library);vulkan.library=nullptr;
    }
    if(!vulkan.loader)return false;
    vulkan.fn.vkGetInstanceProcAddr=vulkan.loader;
    vulkan.fn.vkCreateInstance=reinterpret_cast<PFN_vkCreateInstance>(vulkan.loader(nullptr,"vkCreateInstance"));
    vulkan.fn.vkEnumerateInstanceExtensionProperties=reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(vulkan.loader(nullptr,"vkEnumerateInstanceExtensionProperties"));
    vulkan.fn.vkEnumerateInstanceLayerProperties=reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(vulkan.loader(nullptr,"vkEnumerateInstanceLayerProperties"));
    vulkan.fn.vkEnumerateInstanceVersion=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vulkan.loader(nullptr,"vkEnumerateInstanceVersion"));
    return vulkan.fn.vkCreateInstance!=nullptr;
}
void load_instance(VulkanState& vulkan,VkInstance instance) {
    // 裝置層級函式也經 loader 取得 trampoline，依 handle 轉送；同一張表適用所有 instance／device。
#define X64ARM_VULKAN_LOAD(name) if(auto loaded=reinterpret_cast<PFN_##name>(vulkan.loader(instance,#name)))vulkan.fn.name=loaded;
    X64ARM_VULKAN_FUNCTIONS(X64ARM_VULKAN_LOAD)
#undef X64ARM_VULKAN_LOAD
    vulkan.instance_loaded=true;
}
template<class Range> bool listed(const Range& range,std::string_view name) {return std::find(std::begin(range),std::end(range),name)!=std::end(range);}
std::vector<VkExtensionProperties> host_instance_extensions(VulkanState& vulkan) {
    std::vector<VkExtensionProperties> list;if(!vulkan.fn.vkEnumerateInstanceExtensionProperties)return list;
    std::uint32_t count=0;vulkan.fn.vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr);list.resize(count);
    vulkan.fn.vkEnumerateInstanceExtensionProperties(nullptr,&count,list.data());list.resize(count);return list;
}
std::vector<VkExtensionProperties> host_device_extensions(VulkanState& vulkan,VkPhysicalDevice physical) {
    std::vector<VkExtensionProperties> list;if(!vulkan.fn.vkEnumerateDeviceExtensionProperties)return list;
    std::uint32_t count=0;vulkan.fn.vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr);list.resize(count);
    vulkan.fn.vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,list.data());list.resize(count);return list;
}
bool has(const std::vector<VkExtensionProperties>& list,std::string_view name) {
    return std::any_of(list.begin(),list.end(),[&](const VkExtensionProperties& p){return name==p.extensionName;});
}
// 列舉語義：pProperties 為 NULL 時回報數量；不足時寫入可容納部分並回傳 VK_INCOMPLETE。
std::optional<std::uint32_t> enumerate(VulkanCall& c,std::uint64_t count_at,std::uint64_t items_at,const std::vector<VkExtensionProperties>& items) {
    if(!count_at)return c.done(static_cast<std::uint32_t>(VK_ERROR_INITIALIZATION_FAILED));
    if(!items_at){c.store32(count_at,static_cast<std::uint32_t>(items.size()));return c.done(VK_SUCCESS);}
    const auto capacity=static_cast<std::uint32_t>(c.memory().load(GuestAddress{count_at},32));
    const auto written=std::min<std::size_t>(capacity,items.size());
    if(written)c.memory().write(GuestAddress{items_at},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(items.data()),written*sizeof(VkExtensionProperties)));
    c.store32(count_at,static_cast<std::uint32_t>(written));
    return c.done(static_cast<std::uint32_t>(written<items.size()?VK_INCOMPLETE:VK_SUCCESS));
}
std::vector<VkExtensionProperties> guest_instance_extensions(VulkanState& vulkan) {
    std::vector<VkExtensionProperties> list;
    const auto host=host_instance_extensions(vulkan);
    for(const auto& item:host)if(listed(vulkan_instance_extensions,item.extensionName))list.push_back(item);
    if(has(host,"VK_KHR_surface") && has(host,"VK_EXT_metal_surface")){
        VkExtensionProperties item{};std::strcpy(item.extensionName,"VK_KHR_win32_surface");item.specVersion=6;list.push_back(item);
    }
    return list;
}
// X64ARM_VULKAN_COOPMAT_PROBE=1：宿主不支援 VK_KHR_cooperative_matrix 時仍向客體公告（Apple simdgroup 的 8×8 組合），
// 建立 device 時自宿主請求移除；只用來觀察客體會不會選用及其 SPIR-V，著色器本身仍需宿主支援才能建立管線。
bool cooperative_matrix_probe() {
    static const bool probe=[]{const char* value=std::getenv("X64ARM_VULKAN_COOPMAT_PROBE");return value && std::string_view(value)=="1";}();
    return probe;
}
std::vector<VkExtensionProperties> guest_device_extensions(VulkanState& vulkan,VkPhysicalDevice physical) {
    std::vector<VkExtensionProperties> list;const auto host=host_device_extensions(vulkan,physical);
    for(const auto& item:host)if(listed(vulkan_device_extensions,item.extensionName))list.push_back(item);
    if(cooperative_matrix_probe() && !has(host,"VK_KHR_cooperative_matrix")){VkExtensionProperties item{};std::strcpy(item.extensionName,"VK_KHR_cooperative_matrix");item.specVersion=2;list.push_back(item);}
    return list;
}
std::optional<std::uint32_t> failure(VulkanCall& c,VkResult result) {return c.done(static_cast<std::uint32_t>(result));}

std::optional<std::uint32_t> custom_vkEnumerateInstanceExtensionProperties(VulkanCall& c) {
    if(c.arg(0))return failure(c,VK_ERROR_LAYER_NOT_PRESENT);
    return enumerate(c,c.arg(1),c.arg(2),load(c.vulkan())?guest_instance_extensions(c.vulkan()):std::vector<VkExtensionProperties>{});
}
std::optional<std::uint32_t> custom_vkEnumerateDeviceExtensionProperties(VulkanCall& c) {
    if(c.arg(1))return failure(c,VK_ERROR_LAYER_NOT_PRESENT);
    return enumerate(c,c.arg(2),c.arg(3),guest_device_extensions(c.vulkan(),c.handle<VkPhysicalDevice>(0)));
}
std::optional<std::uint32_t> custom_vkEnumerateInstanceLayerProperties(VulkanCall& c) {c.store32(c.arg(0),0);return c.done(VK_SUCCESS);}
std::optional<std::uint32_t> custom_vkEnumerateDeviceLayerProperties(VulkanCall& c) {c.store32(c.arg(1),0);return c.done(VK_SUCCESS);}
std::optional<std::uint32_t> proc_address(VulkanCall& c,unsigned name_index) {
    const char* name=c.in_string(name_index);if(!name)return c.done(0);
    if(!c.vulkan().commands.contains(name)){
        static const bool trace=std::getenv("X64ARM_TRACE_VULKAN")!=nullptr;
        if(trace)std::fprintf(stderr,"[vulkan] 未提供 %s\n",name);
        return c.done(0);
    }
    return c.done(c.host().resolve(name));
}
std::optional<std::uint32_t> custom_vkGetInstanceProcAddr(VulkanCall& c) {return proc_address(c,1);}
std::optional<std::uint32_t> custom_vkGetDeviceProcAddr(VulkanCall& c) {return proc_address(c,1);}
std::optional<std::uint32_t> custom_vkCreateInstance(VulkanCall& c) {
    auto& vulkan=c.vulkan();
    if(!load(vulkan))return failure(c,VK_ERROR_INCOMPATIBLE_DRIVER);
    const auto* info=static_cast<const VkInstanceCreateInfo*>(c.in_struct(0,c.struct_id("VkInstanceCreateInfo"),1));
    if(!info || !c.arg(2))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    if(info->enabledLayerCount)return failure(c,VK_ERROR_LAYER_NOT_PRESENT);
    const auto host=host_instance_extensions(vulkan);
    const auto supported=guest_instance_extensions(vulkan);
    std::vector<const char*> names;
    bool win32=false;
    for(std::uint32_t i=0;i<info->enabledExtensionCount;++i){
        const char* name=info->ppEnabledExtensionNames?info->ppEnabledExtensionNames[i]:nullptr;
        if(!name || !has(supported,name))return failure(c,VK_ERROR_EXTENSION_NOT_PRESENT);
        if(std::string_view(name)=="VK_KHR_win32_surface"){names.push_back("VK_EXT_metal_surface");win32=true;}else names.push_back(name);
    }
    // MoltenVK 為 portability 實作：須啟用列舉擴充與旗標，loader 才會回報實體裝置。
    VkInstanceCreateInfo request=*info;
    if(has(host,"VK_KHR_portability_enumeration")){names.push_back("VK_KHR_portability_enumeration");request.flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;}
    // MoltenVK 預設以 Metal argument buffer 綁定 descriptor；頻繁更新 descriptor 的計算工作負載（例如類神經網路）
    // 每次 dispatch 都要逐一 useResource，GPU 計時明顯變長（Geekbench 305：0.196s → 0.109s）。以 layer setting 關閉，
    // 使用者另以 MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS 或 X64ARM_VULKAN_ARGUMENT_BUFFERS=1 指定時保留其選擇。
    const VkBool32 argument_buffers=VK_FALSE;
    const VkLayerSettingEXT setting{"MoltenVK","MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS",VK_LAYER_SETTING_TYPE_BOOL32_EXT,1,&argument_buffers};
    VkLayerSettingsCreateInfoEXT settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,request.pNext,1,&setting};
    const char* keep=std::getenv("X64ARM_VULKAN_ARGUMENT_BUFFERS");
    if(has(host,"VK_EXT_layer_settings") && !std::getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS") && !(keep && std::string_view(keep)=="1")){
        names.push_back("VK_EXT_layer_settings");request.pNext=&settings;
    }
    request.enabledLayerCount=0;request.ppEnabledLayerNames=nullptr;
    request.enabledExtensionCount=static_cast<std::uint32_t>(names.size());request.ppEnabledExtensionNames=names.data();
    VkInstance instance{};const auto result=vulkan.fn.vkCreateInstance(&request,nullptr,&instance);
    if(result==VK_SUCCESS){
        load_instance(vulkan,instance);c.store_handle(c.arg(2),instance);
        auto& entry=vulkan.instances[instance];entry.win32=win32;
        if(win32)entry.create_surface=reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(vulkan.loader(instance,"vkCreateMetalSurfaceEXT"));
        std::uint32_t count{};
        if(vulkan.fn.vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS){
            std::vector<VkPhysicalDevice> physical(count);
            if(vulkan.fn.vkEnumeratePhysicalDevices(instance,&count,physical.data())==VK_SUCCESS)
                for(std::uint32_t i=0;i<count;++i)vulkan.physical_instances[physical[i]]=instance;
        }
    }
    return failure(c,result);
}
std::optional<std::uint32_t> custom_vkDestroyInstance(VulkanCall& c) {
    const auto instance=c.handle<VkInstance>(0);auto& vulkan=c.vulkan();
    if(const auto found=vulkan.instances.find(instance);found!=vulkan.instances.end()){
        if(found->second.probe)c.fn().vkDestroySurfaceKHR(instance,found->second.probe,nullptr);
        vulkan.instances.erase(found);
    }
    std::erase_if(vulkan.physical_instances,[&](const auto& item){return item.second==instance;});
    if(c.fn().vkDestroyInstance && instance)c.fn().vkDestroyInstance(instance,nullptr);
    std::erase_if(vulkan.surfaces,[&](const auto& item){return item.second.instance==instance;});
    c.forget_dispatch(c.arg(0));
    return c.done(0);
}
std::optional<std::uint32_t> custom_vkCreateDevice(VulkanCall& c) {
    auto& vulkan=c.vulkan();const auto physical=c.handle<VkPhysicalDevice>(0);
    const auto* info=static_cast<const VkDeviceCreateInfo*>(c.in_struct(1,c.struct_id("VkDeviceCreateInfo"),1));
    if(!info || !c.arg(3))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    const auto host=host_device_extensions(vulkan,physical);
    std::vector<const char*> names;
    for(std::uint32_t i=0;i<info->enabledExtensionCount;++i){
        const char* name=info->ppEnabledExtensionNames?info->ppEnabledExtensionNames[i]:nullptr;
        if(name && std::string_view(name)=="VK_KHR_cooperative_matrix" && cooperative_matrix_probe() && !has(host,name))continue;
        if(!name || !listed(vulkan_device_extensions,name) || !has(host,name))return failure(c,VK_ERROR_EXTENSION_NOT_PRESENT);
        names.push_back(name);
    }
    // 宿主公告 VK_KHR_portability_subset 時規格要求啟用；Windows 程式不認得此擴充，由橋接層補上。
    if(has(host,"VK_KHR_portability_subset"))names.push_back("VK_KHR_portability_subset");
    VkDeviceCreateInfo request=*info;request.enabledLayerCount=0;request.ppEnabledLayerNames=nullptr;
    request.enabledExtensionCount=static_cast<std::uint32_t>(names.size());request.ppEnabledExtensionNames=names.data();
    VkDevice device{};const auto result=vulkan.fn.vkCreateDevice(physical,&request,nullptr,&device);
    if(result==VK_SUCCESS){vulkan.devices[reinterpret_cast<std::uintptr_t>(device)]={physical};c.store_handle(c.arg(3),device);}
    return failure(c,result);
}
void release_mapping(VulkanCall& c,std::uint64_t memory) {
    auto& vulkan=c.vulkan();const auto found=vulkan.mapped.find(memory);if(found==vulkan.mapped.end())return;
    c.host().unmap(found->second.guest,found->second.size);
    vulkan.fn.vkUnmapMemory(reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(found->second.device)),reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(memory)));
    vulkan.mapped.erase(found);
}
std::optional<std::uint32_t> custom_vkDestroyDevice(VulkanCall& c) {
    auto& vulkan=c.vulkan();const auto device=reinterpret_cast<std::uintptr_t>(c.handle<VkDevice>(0));if(!device)return c.done(0);
    std::vector<std::uint64_t> memories;for(const auto& [memory,entry]:vulkan.mapped)if(entry.device==device)memories.push_back(memory);
    for(const auto memory:memories){release_mapping(c,memory);vulkan.allocations.erase(memory);}
    vulkan.fn.vkDestroyDevice(c.handle<VkDevice>(0),nullptr);vulkan.devices.erase(device);
    std::erase_if(vulkan.swapchains,[&](const auto& item){return reinterpret_cast<std::uintptr_t>(item.second.device)==device;});
    c.forget_dispatch(c.arg(0));
    return c.done(0);
}
// Windows WSI 以真正的 CAMetalLayer surface 交給 MoltenVK；Vulkan 的非 dispatchable
// handle 在兩種客體架構都是 64 位元。客體 HWND 僅用於查詢自己的 NativeGui 視窗。
VkResult metal_surface(VulkanState::Instance& owner,const std::shared_ptr<NativeVulkanSurface>& layer,VkInstance instance,VkSurfaceKHR* surface){
    if(!owner.win32 || !owner.create_surface)return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,nullptr,0,static_cast<const CAMetalLayer*>(layer->layer())};
    return owner.create_surface(instance,&info,nullptr,surface);
}
bool refresh_surface(VulkanCall& c,VkSurfaceKHR surface){
    const auto found=c.vulkan().surfaces.find(surface);if(found==c.vulkan().surfaces.end())return false;
    const auto window=c.host().window(found->second.window);if(!window.view || window.view!=found->second.view)return false;
    found->second.layer->resize(window.width,window.height);return true;
}
bool refresh_swapchain(VulkanCall& c,VkSwapchainKHR swapchain){
    const auto found=c.vulkan().swapchains.find(swapchain);
    return found!=c.vulkan().swapchains.end() && refresh_surface(c,found->second.surface);
}
std::optional<std::uint32_t> custom_vkCreateWin32SurfaceKHR(VulkanCall& c){
    const auto instance=c.handle<VkInstance>(0);auto owner=c.vulkan().instances.find(instance);
    if(owner==c.vulkan().instances.end() || !owner->second.win32)return failure(c,VK_ERROR_EXTENSION_NOT_PRESENT);
    if(!c.arg(1) || !c.arg(3))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    const auto base=c.arg(1);const auto width=static_cast<std::uint8_t>(c.small()?32:64);
    if(c.memory().load(GuestAddress{base},32)!=VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR ||
       c.memory().load(GuestAddress{base+(c.small()?4:8)},width) ||
       c.memory().load(GuestAddress{base+(c.small()?8:16)},32))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    const auto hwnd=c.memory().load(GuestAddress{base+(c.small()?16:32)},width);
    const auto window=c.host().window(hwnd);if(!window.view)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    auto layer=make_native_vulkan_surface(window);VkSurfaceKHR surface{};
    const auto result=metal_surface(owner->second,layer,instance,&surface);
    if(result==VK_SUCCESS){c.vulkan().surfaces.emplace(surface,VulkanState::Surface{instance,hwnd,window.view,std::move(layer)});c.store_handle(c.arg(3),surface);}
    return failure(c,result);
}
std::optional<std::uint32_t> custom_vkGetPhysicalDeviceWin32PresentationSupportKHR(VulkanCall& c){
    const auto physical=c.handle<VkPhysicalDevice>(0);auto& vulkan=c.vulkan();
    const auto parent=vulkan.physical_instances.find(physical);if(parent==vulkan.physical_instances.end())return c.done(VK_FALSE);
    auto& owner=vulkan.instances.at(parent->second);if(!owner.win32)return c.done(VK_FALSE);
    if(!owner.probe){
        auto layer=make_native_vulkan_surface({nullptr,1,1});
        if(metal_surface(owner,layer,parent->second,&owner.probe)!=VK_SUCCESS)return c.done(VK_FALSE);
        owner.probe_layer=std::move(layer);
    }
    VkBool32 supported=VK_FALSE;
    if(c.fn().vkGetPhysicalDeviceSurfaceSupportKHR(physical,c.u32(1),owner.probe,&supported)!=VK_SUCCESS)supported=VK_FALSE;
    return c.done(supported);
}
std::optional<std::uint32_t> custom_vkDestroySurfaceKHR(VulkanCall& c){
    const auto surface=c.handle<VkSurfaceKHR>(1);
    if(surface)c.fn().vkDestroySurfaceKHR(c.handle<VkInstance>(0),surface,nullptr);
    c.vulkan().surfaces.erase(surface);return c.done(0);
}
std::optional<std::uint32_t> custom_vkGetPhysicalDevicePresentRectanglesKHR(VulkanCall& c){
    const auto surface=c.handle<VkSurfaceKHR>(1);
    if(!c.arg(2) || !refresh_surface(c,surface))return failure(c,VK_ERROR_UNKNOWN);
    // loader 對未提供此查詢的 ICD 可能回傳成功／零矩形；Win32 轉接持有實際視窗，
    // 以同一個原生 surface 的目前尺寸提供完整呈現區域，不漏掉任何須繪製的像素。
    VkSurfaceCapabilitiesKHR caps{};
    const auto result=c.fn().vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.handle<VkPhysicalDevice>(0),surface,&caps);
    if(result!=VK_SUCCESS)return failure(c,result==VK_ERROR_SURFACE_LOST_KHR?VK_ERROR_UNKNOWN:result);
    auto extent=caps.currentExtent;
    if(extent.width==UINT32_MAX || extent.height==UINT32_MAX){const auto window=c.host().window(c.vulkan().surfaces.at(surface).window);extent={window.width,window.height};}
    if(!c.arg(3)){c.store32(c.arg(2),1);return failure(c,VK_SUCCESS);}
    const auto capacity=c.memory().load(GuestAddress{c.arg(2)},32);
    if(!capacity){c.store32(c.arg(2),0);return failure(c,VK_INCOMPLETE);}
    auto* rectangle=static_cast<VkRect2D*>(c.out_struct(3,c.struct_id("VkRect2D"),1));
    if(!rectangle)return failure(c,VK_ERROR_UNKNOWN);
    *rectangle={{0,0},extent};c.store32(c.arg(2),1);c.finish();return failure(c,VK_SUCCESS);
}
std::optional<std::uint32_t> custom_vkCreateSwapchainKHR(VulkanCall& c){
    const auto* info=static_cast<const VkSwapchainCreateInfoKHR*>(c.in_struct(1,c.struct_id("VkSwapchainCreateInfoKHR"),1));
    if(!info || !c.arg(3))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    if(!refresh_surface(c,info->surface))return failure(c,VK_ERROR_SURFACE_LOST_KHR);
    VkSwapchainKHR swapchain{};const auto device=c.handle<VkDevice>(0);
    const auto result=c.fn().vkCreateSwapchainKHR(device,info,nullptr,&swapchain);
    if(result==VK_SUCCESS){c.vulkan().swapchains.emplace(swapchain,VulkanState::Swapchain{device,info->surface});c.store_handle(c.arg(3),swapchain);}
    return failure(c,result);
}
std::optional<std::uint32_t> custom_vkDestroySwapchainKHR(VulkanCall& c){
    const auto swapchain=c.handle<VkSwapchainKHR>(1);
    if(swapchain)c.fn().vkDestroySwapchainKHR(c.handle<VkDevice>(0),swapchain,nullptr);
    c.vulkan().swapchains.erase(swapchain);return c.done(0);
}
std::optional<std::uint32_t> custom_vkAcquireNextImageKHR(VulkanCall& c){
    if(!refresh_swapchain(c,c.handle<VkSwapchainKHR>(1)))return failure(c,VK_ERROR_SURFACE_LOST_KHR);
    std::uint32_t index{};
    const auto result=c.fn().vkAcquireNextImageKHR(c.handle<VkDevice>(0),c.handle<VkSwapchainKHR>(1),c.u64(2),c.handle<VkSemaphore>(3),c.handle<VkFence>(4),&index);
    if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)c.store32(c.arg(5),index);
    return failure(c,result);
}
std::optional<std::uint32_t> custom_vkAcquireNextImage2KHR(VulkanCall& c){
    const auto* info=static_cast<const VkAcquireNextImageInfoKHR*>(c.in_struct(1,c.struct_id("VkAcquireNextImageInfoKHR"),1));
    if(!info)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    if(!refresh_swapchain(c,info->swapchain))return failure(c,VK_ERROR_SURFACE_LOST_KHR);
    std::uint32_t index{};const auto result=c.fn().vkAcquireNextImage2KHR(c.handle<VkDevice>(0),info,&index);
    if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)c.store32(c.arg(2),index);
    return failure(c,result);
}
std::optional<std::uint32_t> custom_vkQueuePresentKHR(VulkanCall& c){
    // const 輸入根結構仍含可寫 pResults；讓共用轉接保留指標並回寫逐鏈結果。
    auto* info=static_cast<VkPresentInfoKHR*>(c.out_struct(1,c.struct_id("VkPresentInfoKHR"),1));
    if(!info)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    std::vector<std::uint32_t> valid;valid.reserve(info->swapchainCount);
    for(std::uint32_t i=0;i<info->swapchainCount;++i)if(refresh_swapchain(c,info->pSwapchains[i]))valid.push_back(i);
    const auto queue=c.handle<VkQueue>(0);
    if(valid.size()==info->swapchainCount){const auto result=c.fn().vkQueuePresentKHR(queue,info);c.finish();return failure(c,result);}
    // Surface lost 仍須消耗等待 semaphore；其他仍有效視窗各自呈現，不能一起略過。
    VkResult result=VK_SUCCESS;
    if(valid.empty()){
        std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,info->waitSemaphoreCount,info->pWaitSemaphores,stages.data(),0,nullptr,0,nullptr};
        result=c.fn().vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE);
        if(info->pResults)std::fill_n(info->pResults,info->swapchainCount,result==VK_SUCCESS?VK_ERROR_SURFACE_LOST_KHR:result);
    }else{
        std::vector<VkSwapchainKHR> chains;std::vector<std::uint32_t> indices,masks;std::vector<VkResult> results(valid.size(),VK_SUCCESS);
        for(const auto i:valid){chains.push_back(info->pSwapchains[i]);indices.push_back(info->pImageIndices[i]);}
        auto request=*info;request.swapchainCount=static_cast<std::uint32_t>(valid.size());request.pSwapchains=chains.data();request.pImageIndices=indices.data();request.pResults=results.data();
        // 已公告擴充中，只有 device-group 的呈現鏈另帶逐 swapchain 陣列。
        for(auto* next=const_cast<VkBaseInStructure*>(static_cast<const VkBaseInStructure*>(request.pNext));next;next=const_cast<VkBaseInStructure*>(next->pNext))
            if(next->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR){auto* group=reinterpret_cast<VkDeviceGroupPresentInfoKHR*>(next);for(const auto i:valid)masks.push_back(group->pDeviceMasks[i]);group->swapchainCount=request.swapchainCount;group->pDeviceMasks=masks.data();}
        result=c.fn().vkQueuePresentKHR(queue,&request);
        if(info->pResults){std::fill_n(info->pResults,info->swapchainCount,VK_ERROR_SURFACE_LOST_KHR);for(std::size_t i=0;i<valid.size();++i)info->pResults[valid[i]]=results[i];}
    }
    c.finish();return failure(c,result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR || result==VK_ERROR_OUT_OF_DATE_KHR?VK_ERROR_SURFACE_LOST_KHR:result);
}
std::optional<std::uint32_t> custom_vkAllocateMemory(VulkanCall& c) {
    auto& vulkan=c.vulkan();
    const auto* info=static_cast<const VkMemoryAllocateInfo*>(c.in_struct(1,c.struct_id("VkMemoryAllocateInfo"),1));
    if(!info || !c.arg(3))return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    VkDeviceMemory memory{};const auto result=vulkan.fn.vkAllocateMemory(c.handle<VkDevice>(0),info,nullptr,&memory);
    if(result==VK_SUCCESS){vulkan.allocations[reinterpret_cast<std::uintptr_t>(memory)]=info->allocationSize;c.store64(c.arg(3),reinterpret_cast<std::uintptr_t>(memory));}
    return failure(c,result);
}
std::optional<std::uint32_t> custom_vkFreeMemory(VulkanCall& c) {
    auto& vulkan=c.vulkan();const auto memory=c.arg(1);if(!memory)return c.done(0);
    release_mapping(c,memory);vulkan.allocations.erase(memory);
    vulkan.fn.vkFreeMemory(c.handle<VkDevice>(0),c.handle<VkDeviceMemory>(1),nullptr);
    return c.done(0);
}
std::optional<std::uint32_t> custom_vkMapMemory(VulkanCall& c) {
    // 整個配置只對映一次並放到客體位址空間；同一記憶體再次 Map 沿用同一段客體位址。
    auto& vulkan=c.vulkan();const auto device=reinterpret_cast<std::uintptr_t>(c.handle<VkDevice>(0));const auto memory=c.arg(1),offset=c.u64(2),out=c.arg(5);
    if(!out)return failure(c,VK_ERROR_MEMORY_MAP_FAILED);
    if(const auto found=vulkan.mapped.find(memory);found!=vulkan.mapped.end()){c.store_pointer(out,found->second.guest+offset);return c.done(VK_SUCCESS);}
    const auto allocation=vulkan.allocations.find(memory);if(allocation==vulkan.allocations.end())return failure(c,VK_ERROR_MEMORY_MAP_FAILED);
    void* host=nullptr;const auto result=vulkan.fn.vkMapMemory(c.handle<VkDevice>(0),c.handle<VkDeviceMemory>(1),0,VK_WHOLE_SIZE,0,&host);
    if(result!=VK_SUCCESS)return failure(c,result);
    const auto size=static_cast<std::size_t>((allocation->second+host_page-1)&~static_cast<VkDeviceSize>(host_page-1));
    // MoltenVK 的對映位址為 16 KiB 頁面對齊，配置以整頁保留；未對齊時無法直接對映。
    const auto guest=reinterpret_cast<std::uintptr_t>(host)%host_page==0?c.host().map(host,size):0;
    if(!guest){vulkan.fn.vkUnmapMemory(c.handle<VkDevice>(0),c.handle<VkDeviceMemory>(1));return failure(c,VK_ERROR_MEMORY_MAP_FAILED);}
    vulkan.mapped[memory]={device,guest,size};c.store_pointer(out,guest+offset);
    return c.done(VK_SUCCESS);
}
// 客體對映保留到 vkFreeMemory／vkDestroyDevice：規格禁止 Unmap 後存取該指標，MoltenVK 的對映位址固定；
// 每次 Map／Unmap 重建原生 MMU 別名的成本會落在工作負載的計時區段內（例如每次迭代反覆對映的程式）。
std::optional<std::uint32_t> custom_vkUnmapMemory(VulkanCall& c) {return c.done(0);}
std::optional<std::uint32_t> custom_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(VulkanCall& c) {
    auto& vulkan=c.vulkan();const auto physical=c.handle<VkPhysicalDevice>(0);const auto count_at=c.arg(1),items_at=c.arg(2);
    if(!count_at)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    if(vulkan.fn.vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR && has(host_device_extensions(vulkan,physical),"VK_KHR_cooperative_matrix")){
        auto* count=static_cast<std::uint32_t*>(c.inout_bytes(1,4));
        auto* items=static_cast<VkCooperativeMatrixPropertiesKHR*>(c.out_struct(2,c.struct_id("VkCooperativeMatrixPropertiesKHR"),c.deref(1,4)));
        const auto result=vulkan.fn.vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(physical,count,items);c.finish();return failure(c,result);
    }
    if(!cooperative_matrix_probe()){c.store32(count_at,0);return c.done(VK_SUCCESS);}
    // Apple simdgroup_matrix：8×8×8，half／float，subgroup 範圍。
    struct Combo {VkComponentTypeKHR a,b,c,result;};
    static constexpr Combo combos[]={{VK_COMPONENT_TYPE_FLOAT16_KHR,VK_COMPONENT_TYPE_FLOAT16_KHR,VK_COMPONENT_TYPE_FLOAT16_KHR,VK_COMPONENT_TYPE_FLOAT16_KHR},
        {VK_COMPONENT_TYPE_FLOAT16_KHR,VK_COMPONENT_TYPE_FLOAT16_KHR,VK_COMPONENT_TYPE_FLOAT32_KHR,VK_COMPONENT_TYPE_FLOAT32_KHR},
        {VK_COMPONENT_TYPE_FLOAT32_KHR,VK_COMPONENT_TYPE_FLOAT32_KHR,VK_COMPONENT_TYPE_FLOAT32_KHR,VK_COMPONENT_TYPE_FLOAT32_KHR}};
    constexpr auto total=static_cast<std::uint32_t>(std::size(combos));
    if(!items_at){c.store32(count_at,total);return c.done(VK_SUCCESS);}
    const auto capacity=static_cast<std::uint32_t>(c.memory().load(GuestAddress{count_at},32));const auto written=std::min(capacity,total);
    auto* items=static_cast<VkCooperativeMatrixPropertiesKHR*>(c.out_struct(2,c.struct_id("VkCooperativeMatrixPropertiesKHR"),written));
    if(written && !items)return failure(c,VK_ERROR_INITIALIZATION_FAILED);
    for(std::uint32_t i=0;i<written;++i){
        auto& item=items[i];item.MSize=8;item.NSize=8;item.KSize=8;item.AType=combos[i].a;item.BType=combos[i].b;item.CType=combos[i].c;
        item.ResultType=combos[i].result;item.saturatingAccumulation=VK_FALSE;item.scope=VK_SCOPE_SUBGROUP_KHR;
    }
    c.finish();
    c.store32(count_at,written);return c.done(static_cast<std::uint32_t>(written<total?VK_INCOMPLETE:VK_SUCCESS));
}
// 探測模式：宿主不認得的 cooperative matrix 功能／屬性結構，在客體輸出鏈中補上 Apple simdgroup 的能力。
void patch_cooperative_matrix_chain(GuestMemory& memory,std::uint64_t guest,bool small) {
    for(unsigned depth=0;guest && depth<64;++depth){
        const auto stype=static_cast<std::uint32_t>(memory.load(GuestAddress{guest},32));
        if(stype==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR)memory.store(GuestAddress{guest+(small?offsetof(Vk32_VkPhysicalDeviceCooperativeMatrixFeaturesKHR,cooperativeMatrix):offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR,cooperativeMatrix))},32,VK_TRUE);
        if(stype==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR)
            memory.store(GuestAddress{guest+(small?offsetof(Vk32_VkPhysicalDeviceCooperativeMatrixPropertiesKHR,cooperativeMatrixSupportedStages):offsetof(VkPhysicalDeviceCooperativeMatrixPropertiesKHR,cooperativeMatrixSupportedStages))},32,VK_SHADER_STAGE_COMPUTE_BIT);
        guest=memory.load(GuestAddress{guest+(small?4:8)},small?32:64);
    }
}
#define X64ARM_VULKAN_HANDLERS
#include "vulkan_generated.inc"
#undef X64ARM_VULKAN_HANDLERS
}
#endif

std::span<const WindowsServiceDescriptor> vulkan_service_descriptors() {
#if defined(__APPLE__)
    return vulkan_detail::vulkan_descriptors;
#else
    static constexpr WindowsServiceDescriptor table[]={{"vulkan-1.dll","vkCreateInstance",3},{"vulkan-1.dll","vkGetInstanceProcAddr",2},
        {"vulkan-1.dll","vkEnumerateInstanceExtensionProperties",3},{"vulkan-1.dll","vkEnumerateInstanceLayerProperties",2}};
    return table;
#endif
}

bool WindowsServices::vulkan_worker_ready() const noexcept {
#if defined(__APPLE__)
    // X64ARM_TRACE_VULKAN（逐筆或統計）使用未加鎖的全域表，追蹤時全部留在協調者。
    static const bool tracing=std::getenv("X64ARM_TRACE_VULKAN")!=nullptr;
    return !tracing && vulkan_ && vulkan_->instance_loaded && vulkan_->offsets_prepared;
#else
    return false;
#endif
}
std::optional<std::uint32_t> WindowsServices::invoke_vulkan(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
#if !defined(__APPLE__)
    (void)memory;arg.return_to(state,service.symbol=="vkGetInstanceProcAddr"?0U:static_cast<std::uint32_t>(-9)); // VK_ERROR_INCOMPATIBLE_DRIVER。
    return std::optional<std::uint32_t>{};
#else
    using namespace vulkan_detail;
    if(!vulkan_){
        vulkan_=std::make_shared<VulkanState>();
        for(const auto& entry:vulkan_command_names)vulkan_->commands.emplace(entry.name,entry.index);
        for(int i=0;i<static_cast<int>(std::size(vulkan_structs));++i){
            vulkan_->by_name.emplace(vulkan_structs[i].name,i);
            if(vulkan_structs[i].stype!=VK_STRUCTURE_TYPE_MAX_ENUM)vulkan_->by_stype.emplace(static_cast<std::uint32_t>(vulkan_structs[i].stype),i);
        }
        vulkan_->pointer_offsets.resize(std::size(vulkan_structs));vulkan_->pointer_offsets_ready.resize(std::size(vulkan_structs));
    }
    auto& vulkan=*vulkan_;
    const auto command=vulkan.commands.find(service.symbol);
    if(command==vulkan.commands.end()){arg.return_to(state,static_cast<std::uint32_t>(VK_ERROR_FEATURE_NOT_PRESENT));return std::optional<std::uint32_t>{};}
    VulkanCall::Host host;
    host.resolve=[this](std::string_view name){return resolve("vulkan-1.dll",name).value;};
    host.window=[this](std::uint64_t handle){
        VulkanWindow window;const auto found=gui_.windows.find(handle);
        if(found!=gui_.windows.end() && native_gui_){window.view=native_gui_->graphics_view(handle);window.width=found->second.width;window.height=found->second.height;}
        return window;
    };
    host.map=[this,&memory](void* data,std::size_t size)->std::uint64_t{
        // 生命週期由橋接層持有（先撤除客體映射才 vkUnmapMemory／vkFreeMemory），長度固定：共用 map_host_memory 以
        // StableShared 讓原生 MMU 直接別名。X64ARM_VULKAN_CHECKED_MAPPING=1：診斷用，改回受檢外部 backing 以比較兩種映射。
        static const bool checked=[]{const char* value=std::getenv("X64ARM_VULKAN_CHECKED_MAPPING");return value && std::string_view(value)=="1";}();
        return map_host_memory(memory,std::shared_ptr<void>(data,[](void*){}),std::span<std::uint8_t>(static_cast<std::uint8_t*>(data),size),checked);
    };
    host.unmap=[&memory](std::uint64_t address,std::size_t size){try{memory.unmap(GuestAddress{address},size);}catch(const std::exception&){}};
    VulkanCall call(state,memory,arg,vulkan,std::move(host));
    if(!vulkan.offsets_prepared)call.prepare_pointer_offsets();
    // 尚未載入宿主 loader 時只允許全域命令；其他命令不可能持有有效 handle。
    static constexpr std::string_view global[]={"vkCreateInstance","vkEnumerateInstanceExtensionProperties","vkEnumerateInstanceLayerProperties","vkGetInstanceProcAddr","vkEnumerateInstanceVersion"};
    if(!vulkan.instance_loaded && !listed(global,service.symbol))return call.done(static_cast<std::uint32_t>(VK_ERROR_INITIALIZATION_FAILED));
    if(service.symbol=="vkEnumerateInstanceVersion" && !load(vulkan))return call.done(static_cast<std::uint32_t>(VK_ERROR_INCOMPATIBLE_DRIVER));
    if(service.symbol=="vkGetPhysicalDeviceSurfaceSupportKHR" || service.symbol=="vkGetPhysicalDeviceSurfaceCapabilitiesKHR" ||
       service.symbol=="vkGetPhysicalDeviceSurfaceFormatsKHR" || service.symbol=="vkGetPhysicalDeviceSurfacePresentModesKHR" ||
       service.symbol=="vkGetDeviceGroupSurfacePresentModesKHR"){
        const auto index=service.symbol=="vkGetPhysicalDeviceSurfaceSupportKHR"?2U:1U;
        if(!refresh_surface(call,call.handle<VkSurfaceKHR>(index)))return failure(call,VK_ERROR_SURFACE_LOST_KHR);
    }else if(service.symbol=="vkGetPhysicalDeviceSurfaceCapabilities2KHR" || service.symbol=="vkGetPhysicalDeviceSurfaceFormats2KHR"){
        const auto* info=static_cast<const VkPhysicalDeviceSurfaceInfo2KHR*>(call.in_struct(1,call.struct_id("VkPhysicalDeviceSurfaceInfo2KHR"),1));
        if(!info || !refresh_surface(call,info->surface))return failure(call,VK_ERROR_SURFACE_LOST_KHR);
    }
    // X64ARM_VULKAN_DUMP_SPIRV=目錄：保存客體交給 vkCreateShaderModule 的 SPIR-V，供分析使用的指令與能力。
    if(service.symbol=="vkCreateShaderModule"){
        static const char* const dump=std::getenv("X64ARM_VULKAN_DUMP_SPIRV");static unsigned serial=0;
        if(dump && arg(1)){
            const auto* info=static_cast<const VkShaderModuleCreateInfo*>(call.in_struct(1,call.struct_id("VkShaderModuleCreateInfo"),1));
            if(info && info->pCode && info->codeSize && info->codeSize<(std::size_t{64}<<20)){
                const auto path=std::string(dump)+"/shader-"+std::to_string(serial++)+".spv";
                if(auto* file=std::fopen(path.c_str(),"wb")){std::fwrite(info->pCode,1,info->codeSize,file);std::fclose(file);}
            }
        }
    }
    // X64ARM_TRACE_VULKAN_SPECIALIZATION=1：記錄 compute pipeline 的 specialization 常數（工作群組大小等），用於分析 GPU 使用率。
    if(service.symbol=="vkCreateComputePipelines"){
        static const bool spec_trace=std::getenv("X64ARM_TRACE_VULKAN_SPECIALIZATION")!=nullptr;
        if(spec_trace && arg(3)){
            const auto* info=static_cast<const VkComputePipelineCreateInfo*>(call.in_struct(3,call.struct_id("VkComputePipelineCreateInfo"),1));
            const auto module=info?reinterpret_cast<std::uintptr_t>(info->stage.module):0;
            const auto* spec=info?info->stage.pSpecializationInfo:nullptr;
            std::string text="[vulkan] spec module="+std::to_string(module);
            if(spec){
                for(std::uint32_t i=0;spec->pMapEntries && spec->pData && i<spec->mapEntryCount && i<32;++i){
                    const auto& entry=spec->pMapEntries[i];std::uint32_t value{};
                    if(entry.offset>spec->dataSize || spec->dataSize-entry.offset<4)continue;
                    std::memcpy(&value,static_cast<const std::uint8_t*>(spec->pData)+entry.offset,4);
                    text+=" "+std::to_string(entry.constantID)+"="+std::to_string(value);
                }
            }
            std::fprintf(stderr,"%s\n",text.c_str());
        }
    }
    // X64ARM_TRACE_VULKAN：逐一記錄命令與回傳值（VkResult 以有號整數顯示）；=stats 時改為每 10 秒輸出各命令次數、
    // 服務內耗時（含轉換與宿主呼叫）以及兩次服務呼叫之間的客體時間，用來區分 MoltenVK／GPU 與客體 CPU 的占比。
    static const char* const trace_mode=std::getenv("X64ARM_TRACE_VULKAN");
    static const bool trace=trace_mode && std::string_view(trace_mode)!="stats",stats=trace_mode && std::string_view(trace_mode)=="stats";
    if(cooperative_matrix_probe() && (service.symbol=="vkGetPhysicalDeviceFeatures2" || service.symbol=="vkGetPhysicalDeviceFeatures2KHR" ||
        service.symbol=="vkGetPhysicalDeviceProperties2" || service.symbol=="vkGetPhysicalDeviceProperties2KHR")){
        auto result=vulkan_dispatch(call,command->second);
        if(arg(1))patch_cooperative_matrix_chain(memory,memory.load(GuestAddress{arg(1)+(x86_?4:8)},x86_?32:64),x86_);
        return result;
    }
    if(!stats){
        auto result=vulkan_dispatch(call,command->second);
        if(trace)std::fprintf(stderr,"[vulkan] %.*s -> %d (0x%llx)\n",static_cast<int>(service.symbol.size()),service.symbol.data(),static_cast<int>(static_cast<std::int32_t>(call.result())),static_cast<unsigned long long>(call.result()));
        return result;
    }
    struct Stat {std::uint64_t count{},nanoseconds{};};
    static std::unordered_map<std::string_view,Stat> table;static std::uint64_t guest_nanoseconds{};
    static auto reported=std::chrono::steady_clock::now(),last_exit=reported;
    const auto entered=std::chrono::steady_clock::now();
    guest_nanoseconds+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(entered-last_exit).count());
    auto result=vulkan_dispatch(call,command->second);
    last_exit=std::chrono::steady_clock::now();
    auto& item=table[service.symbol];++item.count;item.nanoseconds+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(last_exit-entered).count());
    // X64ARM_TRACE_VULKAN_STATS_MS：統計區間（毫秒），預設 10 秒；縮短可只看計時迭代所在的時段。
    static const auto window=std::chrono::milliseconds([]{const char* value=std::getenv("X64ARM_TRACE_VULKAN_STATS_MS");const long parsed=value?std::strtol(value,nullptr,10):0;return parsed>0?parsed:10000L;}());
    if(last_exit-reported>window){
        std::vector<std::pair<std::string_view,Stat>> rows(table.begin(),table.end());
        std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.second.nanoseconds>b.second.nanoseconds;});
        std::uint64_t service_total=0;for(const auto& row:rows)service_total+=row.second.nanoseconds;
        std::uint64_t calls=0;for(const auto& row:rows)calls+=row.second.count;
        std::fprintf(stderr,"[vulkan] stats calls=%llu service_ms=%.1f guest_between_ms=%.1f\n",static_cast<unsigned long long>(calls),static_cast<double>(service_total)/1e6,static_cast<double>(guest_nanoseconds)/1e6);
        for(std::size_t i=0;i<rows.size() && i<15;++i)std::fprintf(stderr,"[vulkan]   %.*s count=%llu ms=%.2f\n",static_cast<int>(rows[i].first.size()),rows[i].first.data(),
            static_cast<unsigned long long>(rows[i].second.count),static_cast<double>(rows[i].second.nanoseconds)/1e6);
        table.clear();guest_nanoseconds=0;reported=last_exit;
    }
    return result;
#endif
}
}
