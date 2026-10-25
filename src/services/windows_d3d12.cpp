#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/d3d12_abi.hpp"
#include "x64arm/unicode.hpp"
#include "windows_display_identity.hpp"
#include <vkd3d_shader.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace x64arm {
// Direct3D 12：與 D3D9／10／11 共用同一 Metal 圖形後端（NativeGraphics）。
// 命令清單於錄製時保存參數，ExecuteCommandLists 依序重播為 NativeGraphics 呼叫後同步完成，佇列 Signal 隨後生效。
// UPLOAD／READBACK 堆積的資源以客體記憶體為後備，Map 直接回傳該位址；DEFAULT 堆積資源位於 GPU。
// 根簽章以 vkd3d-shader 序列化／解析；SM5.0 著色器沿用 HLSLcc，SM5.1／DXIL 經 SPIR-V 轉 MSL。
// 運算、繪圖、交換鏈、間接命令、Bundle、條件執行與 UAV 清除均使用共用 NativeGraphics／Metal 後端。
// 參考：https://learn.microsoft.com/windows/win32/direct3d12/directx-12-programming-guide
struct D3D12State {
    enum class Kind {Device,Queue,Allocator,List,Fence,Resource,DescriptorHeap,RootSignature,Pipeline,QueryHeap,Heap,Blob,CommandSignature,SwapChain,PipelineLibrary};
    struct Object {Kind kind{};unsigned references{1};std::uint64_t device{};std::map<std::array<std::uint8_t,16>,std::vector<std::uint8_t>> data;};
    struct Resource {
        std::uint32_t dimension{},format{},height{1},depth{1},levels{1},samples{1},flags{},heap_type{1};std::uint64_t width{};
        std::uint64_t address{},cpu{},cpu_size{};std::shared_ptr<GraphicsResource> gpu;std::vector<std::uint8_t> desc;std::uint32_t structure{};
        bool owned_cpu{true}; // placed 資源的 CPU 後備屬於堆積，不由資源釋放。
        std::uint32_t cpu_page{}; // CUSTOM 堆積的 D3D12_CPU_PAGE_PROPERTY（1 不可存取、2 write-combine、3 write-back）。
        // UMA 上 CPU 可見的 CUSTOM 堆積同時具備 UPLOAD（GPU 使用前更新）與 READBACK（GPU 寫入後回寫）語意。
        bool uploads() const {return heap_type==2 || (heap_type==4 && cpu_page>1);}
        bool reads_back() const {return heap_type==3 || (heap_type==4 && cpu_page>1);}
    };
    struct Descriptor {
        enum class Type {Empty,Constant,Shader,Unordered,Sampler,Target,Depth} type{Type::Empty};
        GraphicsView view;std::uint64_t resource{},address{};std::uint32_t size{};std::shared_ptr<GraphicsSampler> sampler;
    };
    // shader-visible CBV_SRV_UAV 堆積另有 GPU 鏡像供描述元表（bindless）使用：mirrors[0] 每項 8 位元組資源 ID，
    // mirrors[1] 每項 16 位元組緩衝位址＋長度；dirty 標記經 CPU handle 寫入過的項目，下次綁定前更新。
    struct Heap {
        std::uint32_t type{},count{};bool visible{};std::uint64_t cpu{},gpu{};std::vector<Descriptor> descriptors;
        std::array<std::shared_ptr<GraphicsResource>,2> mirrors;std::array<std::vector<std::uint8_t>,2> shadow;
        std::vector<std::shared_ptr<void>> keep;std::vector<std::uint8_t> dirty;bool any_dirty{};
    };
    struct Range {std::uint32_t type{},count{},base{},space{},offset{};};
    struct Parameter {std::uint32_t type{},reg{},space{},values{};std::vector<Range> ranges;};
    struct RootSignature {std::vector<Parameter> parameters;std::vector<std::pair<std::uint32_t,std::shared_ptr<GraphicsSampler>>> samplers;};
    struct Pipeline {
        bool compute{};std::uint64_t root{};std::shared_ptr<GraphicsShader> shader,pixel;
        std::vector<GraphicsElement> layout;std::vector<std::uint8_t> blend,depth,raster;std::uint32_t sample_mask{UINT32_MAX};
    };
    // views：錄製時即解析的 RTV／DSV（D3D12 於呼叫當下讀取 CPU 描述元）。
    struct Command {std::string op;std::vector<std::uint64_t> values;std::vector<std::uint8_t> data;std::vector<GraphicsView> views;};
    struct List {std::uint32_t type{};std::uint64_t allocator{};bool open{true},invalid_recording{};std::vector<Command> commands;std::vector<D3D12Replay> callbacks;};
    struct IndirectArgument {std::uint32_t type{},index{},offset{},count{},size{};};
    struct CommandSignature {std::uint32_t stride{},operation{};std::uint64_t root{};std::vector<IndirectArgument> arguments;};
    std::map<std::uint64_t,CommandSignature> signatures;
    struct Fence {std::uint64_t value{};std::vector<std::pair<std::uint64_t,std::uint64_t>> events;};
    struct Device {std::shared_ptr<NativeOpenGlContext> native;std::uint32_t level{0xc000};};
    struct MemoryHeap {std::uint64_t size{},cpu{};std::uint32_t type{},cpu_page{};std::vector<std::uint8_t> desc;};
    // 查詢堆積：TIMESTAMP 存 GPU 時戳；OCCLUSION 每個索引一個 visibility 緩衝，期間每次繪製各佔一格。
    struct QueryHeap {
        std::uint32_t type{},count{};std::vector<std::uint64_t> timestamps;
        struct Occlusion {std::shared_ptr<GraphicsResource> buffer;std::uint32_t slots{};};std::vector<Occlusion> occlusion;
    };
    // 翻轉模型交換鏈：背景緩衝為裝置上的 D3D12 資源，呈現由獨立 Metal context 接手。
    struct SwapChain {
        std::uint64_t queue{},device{},window{},factory{};std::vector<std::uint64_t> buffers;std::shared_ptr<NativeOpenGlContext> native;
        std::uint32_t width{},height{},format{},flags{},index{},interval{};std::uint64_t frames{};std::vector<std::uint8_t> desc1;
    };
    std::map<std::uint64_t,Object> objects;
    std::map<std::uint64_t,Device> devices;std::map<std::uint64_t,Resource> resources;std::map<std::uint64_t,Heap> heaps;
    std::map<std::uint64_t,RootSignature> roots;std::map<std::uint64_t,Pipeline> pipelines;std::map<std::uint64_t,List> lists;
    std::map<std::uint64_t,Fence> fences;std::map<std::uint64_t,std::vector<std::uint8_t>> blobs;std::map<std::uint64_t,std::uint32_t> queues;std::map<std::uint64_t,SwapChain> swaps;
    std::map<std::uint64_t,MemoryHeap> memory_heaps;std::map<std::uint64_t,QueryHeap> query_heaps;
    // 管線庫：名稱對應已儲存的 PSO（持有參照）；由序列化資料還原的名稱沒有現存 PSO（值為 0），載入時依呼叫端描述重新建立。
    std::map<std::uint64_t,std::map<std::u16string,std::uint64_t>> libraries;
    std::map<Kind,std::uint64_t> tables;
    std::uint64_t next_address{UINT64_C(0x0000100000000000)},next_descriptor{UINT64_C(0x0000700000000000)};
    std::set<std::string> reported;
};
namespace {
using Kind=D3D12State::Kind;
constexpr std::uint32_t ok=0,false_ok=1,not_implemented=0x80004001U,no_interface=0x80004002U,pointer_error=0x80004003U,fail=0x80004005U,
    invalid=0x80070057U,no_memory=0x8007000eU,unsupported=0x887a0004U,not_found=0x887a0002U;
constexpr std::uint64_t descriptor_size=32;
// 管線庫序列化格式：識別字串＋格式版本＋名稱數，之後每筆為 UTF-16 長度與內容。識別不符即視為其他驅動版本產生。
constexpr std::array<std::uint8_t,16> library_magic{'X','6','4','A','R','M','-','P','S','O','L','I','B',0,0,1};
constexpr std::uint32_t driver_version_mismatch=0x887e0002U;
struct KindInfo {Kind kind;std::string_view prefix;std::string_view top;std::vector<std::string_view> chain;};
const std::vector<KindInfo>& kinds(){
    static const std::vector<KindInfo> list{
        {Kind::Device,"Device.","ID3D12Device14",{"ID3D12Object","ID3D12Device","ID3D12Device1","ID3D12Device2","ID3D12Device3","ID3D12Device4","ID3D12Device5","ID3D12Device6","ID3D12Device7","ID3D12Device8","ID3D12Device9","ID3D12Device10","ID3D12Device11","ID3D12Device12","ID3D12Device13","ID3D12Device14"}},
        {Kind::Queue,"Queue.","ID3D12CommandQueue",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12CommandQueue"}},
        {Kind::Allocator,"Allocator.","ID3D12CommandAllocator",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12CommandAllocator"}},
        {Kind::List,"List.","ID3D12GraphicsCommandList10",{"ID3D12Object","ID3D12DeviceChild","ID3D12CommandList","ID3D12GraphicsCommandList","ID3D12GraphicsCommandList1","ID3D12GraphicsCommandList2","ID3D12GraphicsCommandList3","ID3D12GraphicsCommandList4","ID3D12GraphicsCommandList5","ID3D12GraphicsCommandList6","ID3D12GraphicsCommandList7","ID3D12GraphicsCommandList8","ID3D12GraphicsCommandList9","ID3D12GraphicsCommandList10"}},
        {Kind::Fence,"Fence.","ID3D12Fence1",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12Fence","ID3D12Fence1"}},
        {Kind::Resource,"Resource.","ID3D12Resource2",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12Resource","ID3D12Resource1","ID3D12Resource2"}},
        {Kind::DescriptorHeap,"DescriptorHeap.","ID3D12DescriptorHeap",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12DescriptorHeap"}},
        {Kind::RootSignature,"RootSignature.","ID3D12RootSignature",{"ID3D12Object","ID3D12DeviceChild","ID3D12RootSignature"}},
        {Kind::Pipeline,"Pipeline.","ID3D12PipelineState",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12PipelineState"}},
        {Kind::CommandSignature,"CommandSignature.","ID3D12CommandSignature",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12CommandSignature"}},
        {Kind::QueryHeap,"QueryHeap.","ID3D12QueryHeap",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12QueryHeap"}},
        {Kind::Heap,"Heap.","ID3D12Heap1",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12Heap","ID3D12Heap1"}},
        {Kind::PipelineLibrary,"PipelineLibrary.","ID3D12PipelineLibrary1",{"ID3D12Object","ID3D12DeviceChild","ID3D12PipelineLibrary","ID3D12PipelineLibrary1"}},
    };
    return list;
}
const KindInfo* info_of(Kind kind){for(const auto& item:kinds())if(item.kind==kind)return &item;return nullptr;}
// IDXGISwapChain4 完整 vtable 順序與 Win32 參數數量（不含 This）。
constexpr std::pair<std::string_view,unsigned> swap_methods[]{
    {"QueryInterface",2},{"AddRef",0},{"Release",0},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"GetPrivateData",3},{"GetParent",2},{"GetDevice",2},
    {"Present",2},{"GetBuffer",3},{"SetFullscreenState",2},{"GetFullscreenState",2},{"GetDesc",1},{"ResizeBuffers",5},{"ResizeTarget",1},{"GetContainingOutput",1},
    {"GetFrameStatistics",1},{"GetLastPresentCount",1},{"GetDesc1",1},{"GetFullscreenDesc",1},{"GetHwnd",1},{"GetCoreWindow",2},{"Present1",3},{"IsTemporaryMonoSupported",0},
    {"GetRestrictToOutput",1},{"SetBackgroundColor",1},{"GetBackgroundColor",1},{"SetRotation",1},{"GetRotation",1},{"SetSourceSize",2},{"GetSourceSize",2},
    {"SetMaximumFrameLatency",1},{"GetMaximumFrameLatency",1},{"GetFrameLatencyWaitableObject",0},{"SetMatrixTransform",1},{"GetMatrixTransform",1},
    {"GetCurrentBackBufferIndex",0},{"CheckColorSpaceSupport",2},{"SetColorSpace1",1},{"ResizeBuffers1",7},{"SetHDRMetaData",3},
};
// IDXGIObject、IDXGIDeviceSubObject、IDXGISwapChain–IDXGISwapChain4。
constexpr std::array<std::array<std::uint8_t,16>,7> swap_iids{{
    {0xb8,0x2f,0xc2,0xae,0xf3,0x76,0x39,0x46,0x9b,0xe0,0x28,0xeb,0x43,0xa6,0x7a,0x2e},{0x79,0x03,0x3e,0x3d,0xde,0xf9,0x58,0x4d,0xbb,0x6c,0x18,0xd6,0x29,0x92,0xf1,0xa6},
    {0xa0,0x36,0x0d,0x31,0xe7,0xd2,0x0a,0x4c,0xaa,0x04,0x6a,0x9d,0x23,0xb8,0x88,0x6a},{0xf7,0x45,0x0a,0x79,0x42,0x0d,0x76,0x48,0x98,0x3a,0x0a,0x55,0xcf,0xe6,0xf4,0xaa},
    {0xc4,0x2a,0xbe,0xa8,0xf8,0x19,0x46,0x49,0xb3,0x31,0x79,0x59,0x9f,0xb9,0x8d,0xe7},{0xdb,0x9b,0xd9,0x94,0xf8,0xf1,0xb0,0x4a,0xb2,0x36,0x7d,0xa0,0x17,0x0e,0xda,0xb1},
    {0x5a,0x5d,0x58,0x3d,0xbd,0x4a,0x9e,0x48,0xb1,0xf4,0x3d,0xbc,0xb6,0x45,0x2f,0xfb}}};
constexpr std::array<std::uint8_t,16> iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::array<std::uint8_t,16> iid_blob{0x08,0xfb,0xa5,0x8b,0x95,0x51,0xe2,0x40,0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02}; // ID3D10Blob。
std::array<std::uint8_t,16> read_guid(const GuestMemory& memory,std::uint64_t at){std::array<std::uint8_t,16> value{};memory.read(GuestAddress{at},value);return value;}
}
std::span<const WindowsServiceDescriptor> d3d12_export_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"d3d12.dll","D3D12CreateDevice",4},{"d3d12.dll","#101",4},{"d3d12.dll","D3D12GetDebugInterface",2},{"d3d12.dll","D3D12SerializeRootSignature",4},
        {"d3d12.dll","D3D12SerializeVersionedRootSignature",3},{"d3d12.dll","D3D12CreateRootSignatureDeserializer",4},
        {"d3d12.dll","D3D12CreateVersionedRootSignatureDeserializer",4},{"d3d12.dll","D3D12EnableExperimentalFeatures",4},{"d3d12.dll","D3D12GetInterface",3},
    };
    return table;
}
std::span<const WindowsServiceDescriptor> d3d12_object_service_descriptors(){
    static const auto descriptors=[]{
        static std::vector<std::string> names;std::vector<std::pair<std::size_t,unsigned>> rows;
        for(const auto& item:kinds()){const auto* abi=d3d12abi::find_interface(item.top);for(const auto& method:abi->methods){rows.push_back({names.size(),method.arguments+1});names.push_back(std::string(item.prefix)+std::string(method.name));}}
        for(const auto& [name,count]:swap_methods){rows.push_back({names.size(),count+1});names.push_back("Swap."+std::string(name));}
        rows.push_back({names.size(),4});names.push_back("DXGI.CreateSwapChain");rows.push_back({names.size(),7});names.push_back("DXGI.CreateSwapChainForHwnd");
        for(const auto* name:{"Blob.QueryInterface","Blob.AddRef","Blob.Release","Blob.GetBufferPointer","Blob.GetBufferSize"})names.push_back(name);
        std::vector<WindowsServiceDescriptor> out;
        for(const auto& [index,count]:rows)out.push_back(WindowsServiceDescriptor{"internal-d3d12",names[index],count});
        const auto base=rows.size();
        for(unsigned i=0;i<5;++i)out.push_back(WindowsServiceDescriptor{"internal-d3d12",names[base+i],i==0?3U:1U});
        return out;
    }();
    return descriptors;
}
bool WindowsServices::d3d12_queue(std::uint64_t object) const{return d3d12_ && d3d12_->queues.contains(object);}
bool WindowsServices::d3d12_record(std::uint64_t list,D3D12Replay replay){
    if(!d3d12_)return false;const auto found=d3d12_->lists.find(list);if(found==d3d12_->lists.end() || !found->second.open)return false;
    found->second.commands.push_back({"Callback",{found->second.callbacks.size()},{},{}});found->second.callbacks.push_back(std::move(replay));return true;
}
std::uint64_t WindowsServices::d3d12_device_of(std::uint64_t object) const{
    if(!d3d12_)return 0;if(d3d12_->devices.contains(object))return object;const auto found=d3d12_->objects.find(object);return found==d3d12_->objects.end()?0:found->second.device;
}
bool WindowsServices::d3d12_resource_width(std::uint64_t resource,std::uint64_t& width) const{
    if(!d3d12_)return false;const auto found=d3d12_->resources.find(resource);if(found==d3d12_->resources.end())return false;width=found->second.width;return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_d3d12(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    // d3d12.lib 以序號 101 匯入 D3D12CreateDevice。
    const std::string_view symbol=service.symbol=="#101"?"D3D12CreateDevice":service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(x86_)return finish(service.family=="d3d12.dll"?unsupported:not_implemented);
    if(!d3d12_)d3d12_=std::make_shared<D3D12State>();
    auto& bridge=*d3d12_;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto load=[&](std::uint64_t at,unsigned bits){return memory.load(GuestAddress{at},static_cast<std::uint8_t>(bits));};
    const auto store=[&](std::uint64_t at,unsigned bits,std::uint64_t value){memory.store(GuestAddress{at},static_cast<std::uint8_t>(bits),value);};
    const auto bytes=[&](std::uint64_t at,std::size_t size){std::vector<std::uint8_t> out(size);if(size)memory.read(GuestAddress{at},out);return out;};
    const auto report=[&](std::string_view name){if(bridge.reported.insert(std::string(name)).second)runtime_.module_events.push_back("d3d12_note=尚未支援 "+std::string(name));};
    const auto create=[&](Kind kind,std::uint64_t device)->std::uint64_t{
        auto& table=bridge.tables[kind];
        if(!table){
            if(kind==Kind::SwapChain){
                table=runtime_allocate(memory,{},std::size(swap_methods)*8);std::size_t i=0;
                for(const auto& [name,count]:swap_methods){(void)count;store(table+8*i++,64,resolve("internal-d3d12","Swap."+std::string(name)).value);}
            }else if(kind==Kind::Blob){
                table=runtime_allocate(memory,{},5*8);std::size_t i=0;
                for(const auto* name:{"Blob.QueryInterface","Blob.AddRef","Blob.Release","Blob.GetBufferPointer","Blob.GetBufferSize"})store(table+8*i++,64,resolve("internal-d3d12",name).value);
            }else{
                const auto* item=info_of(kind);const auto* abi=d3d12abi::find_interface(item->top);table=runtime_allocate(memory,{},abi->methods.size()*8);
                for(std::size_t i=0;i<abi->methods.size();++i)store(table+i*8,64,resolve("internal-d3d12",std::string(item->prefix)+std::string(abi->methods[i].name)).value);
            }
        }
        const auto pointer=allocate_heap(memory,16);if(!pointer)throw std::bad_alloc();
        store(pointer,64,table);store(pointer+8,64,0);
        D3D12State::Object object;object.kind=kind;object.device=device;bridge.objects.emplace(pointer,std::move(object));
        if(device)++bridge.objects.at(device).references;
        return pointer;
    };
    const auto blob=[&](std::vector<std::uint8_t> data){const auto object=create(Kind::Blob,0);const auto copy=allocate_heap(memory,std::max<std::size_t>(data.size(),1));if(!copy)throw std::bad_alloc();
        if(!data.empty())memory.write(GuestAddress{copy},data);store(object+8,64,copy);bridge.blobs[object]=std::move(data);return object;};
    // QueryInterface 取得物件：IID 屬於該種類介面鏈才成功。
    const auto query=[&](std::uint64_t self,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        if(!out)return pointer_error;store(out,64,0);if(!riid)return invalid;
        const auto found=bridge.objects.find(self);if(found==bridge.objects.end())return invalid;
        const auto iid=read_guid(memory,riid);bool match=iid==iid_unknown || (found->second.kind==Kind::Blob && iid==iid_blob) ||
            (found->second.kind==Kind::SwapChain && std::find(swap_iids.begin(),swap_iids.end(),iid)!=swap_iids.end());
        if(const auto* item=info_of(found->second.kind))for(const auto name:item->chain)if(const auto* abi=d3d12abi::find_interface(name);abi && abi->iid==iid)match=true;
        if(!match)return no_interface;
        ++found->second.references;store(out,64,self);return ok;
    };
    const auto give=[&](std::uint64_t object,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        // 新物件參照由呼叫端取得；介面不符時釋放剛建立的物件。
        const auto result=query(object,riid,out);
        auto& entry=bridge.objects.at(object);--entry.references;return result;
    };
    // D3D12CreateDevice：功能層級最高 12_1；Metal 後端由 D3D11 相同工廠建立。
    if(symbol=="D3D12CreateDevice"){
        // 1_0_GENERIC（0x100）／1_0_CORE（0x1000）為計算專用層級，完整裝置亦滿足。
        const auto level=u32(1);
        if(level!=0x100 && level!=0x1000 && (level<0xb000 || level>0xc100))return finish(level>0xc100?unsupported:invalid);
        if(arg(0) && !arg(3))return finish(false_ok);
        std::shared_ptr<NativeOpenGlContext> native;
        try{native=make_metal_opengl(nullptr,1,1);}catch(const std::exception&){native.reset();}
        if(!native)return finish(unsupported);
        if(!arg(3))return finish(false_ok);
        const auto device=create(Kind::Device,0);bridge.devices[device]=D3D12State::Device{std::move(native),0xc100};
        return finish(give(device,arg(2),arg(3)));
    }
    if(symbol=="D3D12GetDebugInterface" || symbol=="D3D12GetInterface" || symbol=="D3D12EnableExperimentalFeatures"){
        if(symbol!="D3D12EnableExperimentalFeatures" && arg(symbol=="D3D12GetInterface"?2:1))store(arg(symbol=="D3D12GetInterface"?2:1),64,0);
        return finish(no_interface);
    }
    // 根簽章：客體 D3D12 描述轉成 vkd3d 1.1 描述後序列化為 DXBC RTS0。
    if(symbol=="D3D12SerializeRootSignature" || symbol=="D3D12SerializeVersionedRootSignature"){
        const bool versioned=symbol=="D3D12SerializeVersionedRootSignature";const auto out=arg(versioned?1:2),error_out=arg(versioned?2:3);
        if(!out)return finish(pointer_error);store(out,64,0);if(error_out)store(error_out,64,0);
        if(!arg(0))return finish(invalid);
        std::uint32_t version=versioned?static_cast<std::uint32_t>(load(arg(0),32)):u32(1);const auto desc=versioned?arg(0)+8:arg(0);
        if(version!=1 && version!=2)return finish(invalid);
        const bool v11=version==2;
        vkd3d_shader_versioned_root_signature_desc root{};root.version=VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1;
        std::vector<vkd3d_shader_root_parameter1> parameters(static_cast<std::size_t>(load(desc,32)));
        std::vector<std::vector<vkd3d_shader_descriptor_range1>> ranges(parameters.size());
        std::vector<vkd3d_shader_static_sampler_desc> samplers(static_cast<std::size_t>(load(desc+16,32)));
        if(parameters.size()>64 || samplers.size()>2032)return finish(invalid);
        const auto parameter_at=load(desc+8,64),sampler_at=load(desc+24,64);
        for(std::size_t i=0;i<parameters.size();++i){
            const auto at=parameter_at+i*32;auto& p=parameters[i];p.parameter_type=static_cast<vkd3d_shader_root_parameter_type>(load(at,32));p.shader_visibility=static_cast<vkd3d_shader_visibility>(load(at+24,32));
            if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
                const auto count=load(at+8,32),list=load(at+16,64);if(count>4096)return finish(invalid);
                for(std::uint64_t j=0;j<count;++j){const auto r=list+j*(v11?24:20);vkd3d_shader_descriptor_range1 range{};
                    range.range_type=static_cast<vkd3d_shader_descriptor_type>(load(r,32));range.descriptor_count=static_cast<unsigned>(load(r+4,32));range.base_shader_register=static_cast<unsigned>(load(r+8,32));
                    range.register_space=static_cast<unsigned>(load(r+12,32));range.flags=v11?static_cast<vkd3d_shader_descriptor_range_flags>(load(r+16,32)):VKD3D_SHADER_DESCRIPTOR_RANGE_FLAG_NONE;
                    range.descriptor_table_offset=static_cast<unsigned>(load(r+(v11?20:16),32));ranges[i].push_back(range);}
                p.u.descriptor_table.descriptor_range_count=static_cast<unsigned>(ranges[i].size());p.u.descriptor_table.descriptor_ranges=ranges[i].data();
            }else if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){
                p.u.constants.shader_register=static_cast<unsigned>(load(at+8,32));p.u.constants.register_space=static_cast<unsigned>(load(at+12,32));p.u.constants.value_count=static_cast<unsigned>(load(at+16,32));
            }else{
                p.u.descriptor.shader_register=static_cast<unsigned>(load(at+8,32));p.u.descriptor.register_space=static_cast<unsigned>(load(at+12,32));
                p.u.descriptor.flags=v11?static_cast<vkd3d_shader_root_descriptor_flags>(load(at+16,32)):VKD3D_SHADER_ROOT_DESCRIPTOR_FLAG_NONE;
            }
        }
        for(std::size_t i=0;i<samplers.size();++i){
            const auto at=sampler_at+i*52;auto& s=samplers[i];
            s.filter=static_cast<vkd3d_shader_filter>(load(at,32));s.address_u=static_cast<vkd3d_shader_texture_address_mode>(load(at+4,32));
            s.address_v=static_cast<vkd3d_shader_texture_address_mode>(load(at+8,32));s.address_w=static_cast<vkd3d_shader_texture_address_mode>(load(at+12,32));
            s.mip_lod_bias=std::bit_cast<float>(static_cast<std::uint32_t>(load(at+16,32)));s.max_anisotropy=static_cast<unsigned>(load(at+20,32));
            s.comparison_func=static_cast<vkd3d_shader_comparison_func>(load(at+24,32));s.border_colour=static_cast<vkd3d_shader_static_border_colour>(load(at+28,32));
            s.min_lod=std::bit_cast<float>(static_cast<std::uint32_t>(load(at+32,32)));s.max_lod=std::bit_cast<float>(static_cast<std::uint32_t>(load(at+36,32)));
            s.shader_register=static_cast<unsigned>(load(at+40,32));s.register_space=static_cast<unsigned>(load(at+44,32));s.shader_visibility=static_cast<vkd3d_shader_visibility>(load(at+48,32));
        }
        root.u.v_1_1.parameter_count=static_cast<unsigned>(parameters.size());root.u.v_1_1.parameters=parameters.data();
        root.u.v_1_1.static_sampler_count=static_cast<unsigned>(samplers.size());root.u.v_1_1.static_samplers=samplers.data();
        root.u.v_1_1.flags=static_cast<vkd3d_shader_root_signature_flags>(load(desc+32,32));
        vkd3d_shader_versioned_root_signature_desc converted{};const vkd3d_shader_versioned_root_signature_desc* serialized=&root;
        if(!v11){if(vkd3d_shader_convert_root_signature(&converted,VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_0,&root)<0)return finish(invalid);serialized=&converted;}
        vkd3d_shader_code code{};char* messages=nullptr;const auto status=vkd3d_shader_serialize_root_signature(serialized,&code,&messages);
        if(!v11)vkd3d_shader_free_root_signature(&converted);
        if(status<0){if(error_out){const std::string text=messages?messages:"";store(error_out,64,blob(std::vector<std::uint8_t>(text.begin(),text.end()+0)));}vkd3d_shader_free_messages(messages);return finish(invalid);}
        vkd3d_shader_free_messages(messages);
        std::vector<std::uint8_t> data(static_cast<const std::uint8_t*>(code.code),static_cast<const std::uint8_t*>(code.code)+code.size);vkd3d_shader_free_shader_code(&code);
        store(out,64,blob(std::move(data)));return finish(ok);
    }
    // 交換鏈背景緩衝：裝置上可作 RTV／SRV 的 2D 紋理，包成 ID3D12Resource。
    const auto swap_buffers=[&](D3D12State::SwapChain& swap,std::uint32_t count){
        auto& native=*bridge.devices.at(swap.device).native;
        for(const auto buffer:swap.buffers){bridge.resources.erase(buffer);bridge.objects.erase(buffer);}
        swap.buffers.clear();swap.index=0;
        for(std::uint32_t i=0;i<count;++i){
            D3D12State::Resource r;r.dimension=3;r.width=swap.width;r.height=swap.height;r.format=swap.format;r.flags=1;r.heap_type=1;r.desc.assign(56,0);
            const auto put=[&](unsigned at,std::uint64_t value,unsigned size){std::memcpy(r.desc.data()+at,&value,size);};
            put(0,3,4);put(16,swap.width,8);put(24,swap.height,4);put(28,1|(1U<<16),4);put(32,swap.format,4);put(36,1,4);put(48,1,4);
            GraphicsResourceDesc d;d.dimension=3;d.width=swap.width;d.height=swap.height;d.format=swap.format;d.bind=0x28;r.gpu=native.graphics_resource(d);
            const auto object=create(Kind::Resource,swap.device);bridge.resources[object]=std::move(r);swap.buffers.push_back(object);
        }
        // 無原生 GUI（headless）時以離屏表面呈現，與 D3D11 交換鏈相同。
        if(gui_.windows.contains(swap.window))swap.native->drawable(native_gui_?native_gui_->graphics_view(swap.window):nullptr,swap.width,swap.height);
    };
    // DXGI 工廠收到 D3D12 佇列時轉入此處；參數位置與原 API 相同（D3D12 只允許翻轉模型）。
    if(symbol=="DXGI.CreateSwapChain" || symbol=="DXGI.CreateSwapChainForHwnd"){
        const bool modern=symbol=="DXGI.CreateSwapChainForHwnd";const auto queue=arg(1),out=arg(modern?6:3),desc=arg(modern?3:2);
        if(!out || !desc)return finish(pointer_error);store(out,64,0);
        if(!bridge.queues.contains(queue))return finish(invalid);
        D3D12State::SwapChain swap;std::uint32_t count=0,effect=0,samples=0;
        if(modern){swap.window=arg(2);swap.width=static_cast<std::uint32_t>(load(desc,32));swap.height=static_cast<std::uint32_t>(load(desc+4,32));swap.format=static_cast<std::uint32_t>(load(desc+8,32));
            samples=static_cast<std::uint32_t>(load(desc+16,32));count=static_cast<std::uint32_t>(load(desc+28,32));effect=static_cast<std::uint32_t>(load(desc+36,32));swap.flags=static_cast<std::uint32_t>(load(desc+44,32));swap.desc1=bytes(desc,48);}
        else{swap.width=static_cast<std::uint32_t>(load(desc,32));swap.height=static_cast<std::uint32_t>(load(desc+4,32));swap.format=static_cast<std::uint32_t>(load(desc+16,32));
            samples=static_cast<std::uint32_t>(load(desc+28,32));count=static_cast<std::uint32_t>(load(desc+40,32));swap.window=load(desc+48,64);effect=static_cast<std::uint32_t>(load(desc+60,32));swap.flags=static_cast<std::uint32_t>(load(desc+64,32));
            swap.desc1.assign(48,0);const auto put=[&](unsigned at,std::uint32_t value){std::memcpy(swap.desc1.data()+at,&value,4);};
            put(8,swap.format);put(16,samples);put(24,static_cast<std::uint32_t>(load(desc+36,32)));put(28,count);put(36,effect);put(44,swap.flags);}
        const auto window=gui_.windows.find(swap.window);if(window==gui_.windows.end())return finish(invalid);
        if(!swap.width)swap.width=window->second.width;if(!swap.height)swap.height=window->second.height;
        if((effect!=3 && effect!=4) || count<2 || count>16 || samples!=1 || !swap.width || !swap.height)return finish(invalid);
        swap.queue=queue;swap.device=bridge.objects.at(queue).device;swap.factory=arg(0);
        try{swap.native=make_metal_opengl(nullptr,1,1);swap_buffers(swap,count);}
        catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=CreateSwapChain 失敗：")+error.what());return finish(invalid);}
        const auto object=create(Kind::SwapChain,swap.device);bridge.swaps[object]=std::move(swap);store(out,64,object);return finish(ok);
    }
    if(symbol=="D3D12CreateRootSignatureDeserializer" || symbol=="D3D12CreateVersionedRootSignatureDeserializer"){if(arg(3))store(arg(3),64,0);report(symbol);return finish(not_implemented);}
    const auto dot=symbol.find('.');if(dot==std::string_view::npos)return finish(not_implemented);
    const auto member=symbol.substr(dot+1);const auto self=arg(0);
    const auto found=bridge.objects.find(self);if(found==bridge.objects.end())return finish(invalid);
    const auto kind=found->second.kind;
    if(member=="QueryInterface")return finish(query(self,arg(1),arg(2)));
    if(member=="AddRef")return finish(++found->second.references);
    if(member=="Release"){
        // 物件與宿主資源在最後一個參照釋放時移除；客體記憶體區塊保留，避免懸置的 vtable 指標。
        if(found->second.references)--found->second.references;
        const auto remaining=found->second.references;
        if(!remaining){
            const auto device=found->second.device;
            if(kind==Kind::Resource){if(const auto r=bridge.resources.find(self);r!=bridge.resources.end()){if(r->second.cpu && r->second.owned_cpu)(void)free_heap(memory,r->second.cpu,state);bridge.resources.erase(r);}}
            if(kind==Kind::Heap){if(const auto h=bridge.memory_heaps.find(self);h!=bridge.memory_heaps.end()){if(h->second.cpu)(void)free_heap(memory,h->second.cpu,state);bridge.memory_heaps.erase(h);}}
            bridge.signatures.erase(self);bridge.query_heaps.erase(self);
            bridge.heaps.erase(self);bridge.roots.erase(self);bridge.pipelines.erase(self);bridge.lists.erase(self);bridge.fences.erase(self);bridge.queues.erase(self);
            if(const auto library=bridge.libraries.find(self);library!=bridge.libraries.end()){
                for(const auto& [name,pipeline]:library->second)if(const auto entry=bridge.objects.find(pipeline);pipeline && entry!=bridge.objects.end() && entry->second.references && !--entry->second.references){
                    const auto owner=entry->second.device;bridge.pipelines.erase(pipeline);bridge.objects.erase(entry);
                    if(const auto parent=bridge.objects.find(owner);parent!=bridge.objects.end() && parent->second.references)--parent->second.references;}
                bridge.libraries.erase(library);
            }
            if(const auto swap=bridge.swaps.find(self);swap!=bridge.swaps.end()){for(const auto buffer:swap->second.buffers){bridge.resources.erase(buffer);bridge.objects.erase(buffer);}bridge.swaps.erase(swap);}
            if(kind==Kind::Blob){if(const auto data=load(self+8,64))(void)free_heap(memory,data,state);bridge.blobs.erase(self);}
            if(kind==Kind::Device)bridge.devices.erase(self);
            bridge.objects.erase(found);
            if(device)if(const auto parent=bridge.objects.find(device);parent!=bridge.objects.end() && parent->second.references)--parent->second.references;
        }
        return finish(remaining);
    }
    if(kind==Kind::Blob){
        if(member=="GetBufferPointer")return finish(load(self+8,64));
        if(member=="GetBufferSize"){const auto data=bridge.blobs.find(self);return finish(data==bridge.blobs.end()?0:data->second.size());}
        return finish(not_implemented);
    }
    // ID3D12Object 共用方法。
    if(member=="SetName" || member=="SetPrivateDataInterface")return finish(ok);
    if(member=="SetPrivateData"){
        if(!arg(1))return finish(invalid);const auto guid=read_guid(memory,arg(1));
        if(!arg(3)){found->second.data.erase(guid);return finish(ok);}
        found->second.data[guid]=bytes(arg(3),u32(2));return finish(ok);
    }
    if(member=="GetPrivateData"){
        if(!arg(1) || !arg(2))return finish(invalid);const auto item=found->second.data.find(read_guid(memory,arg(1)));
        if(item==found->second.data.end()){store(arg(2),32,0);return finish(not_found);}
        const auto capacity=load(arg(2),32);store(arg(2),32,item->second.size());
        if(!arg(3))return finish(ok);if(capacity<item->second.size())return finish(0x887a0003U); // DXGI_ERROR_MORE_DATA。
        memory.write(GuestAddress{arg(3)},item->second);return finish(ok);
    }
    if(member=="GetDevice"){const auto device=kind==Kind::Device?self:found->second.device;return finish(query(device,arg(1),arg(2)));}
    const auto device_of=[&](std::uint64_t object){const auto& o=bridge.objects.at(object);return o.kind==Kind::Device?object:o.device;};
    auto& device=bridge.devices.at(device_of(self));auto& native=*device.native;
    // 描述元控制代碼：CPU／GPU 位址皆為堆積基底加索引×32。
    const auto descriptor=[&](std::uint64_t handle,bool gpu)->D3D12State::Descriptor*{
        for(auto& [object,heap]:bridge.heaps){(void)object;const auto base=gpu?heap.gpu:heap.cpu;
            if(base && handle>=base && handle<base+heap.count*descriptor_size && !((handle-base)%descriptor_size)){
                const auto index=(handle-base)/descriptor_size;
                // 經 CPU handle 取得的項目可能被寫入：標記鏡像待更新。
                if(!gpu && !heap.dirty.empty()){heap.dirty[index]=1;heap.any_dirty=true;}
                return &heap.descriptors[index];
            }}
        return nullptr;
    };
    const auto resource_at=[&](std::uint64_t address)->std::pair<std::uint64_t,std::uint64_t>{
        for(const auto& [object,resource]:bridge.resources)if(resource.address && address>=resource.address && address<resource.address+std::max<std::uint64_t>(resource.width,1))return {object,address-resource.address};
        return {0,0};
    };
    // UPLOAD 堆積：GPU 讀取前以客體後備記憶體更新影子資源；READBACK：GPU 寫入後回寫。
    const auto gpu_resource=[&](std::uint64_t object)->std::shared_ptr<GraphicsResource>{
        auto& r=bridge.resources.at(object);
        if(!r.gpu){GraphicsResourceDesc d;d.dimension=1;d.width=static_cast<std::uint32_t>(r.width);d.bind=0x8c;r.gpu=native.graphics_resource(d);}
        if(r.cpu && r.uploads())native.graphics_write(GraphicsView{r.gpu},bytes(r.cpu,static_cast<std::size_t>(r.width)),static_cast<std::uint32_t>(r.width),static_cast<std::uint32_t>(r.width));
        return r.gpu;
    };
    const auto read_back=[&](std::uint64_t object){
        auto& r=bridge.resources.at(object);if(!r.cpu || !r.gpu)return;
        const auto data=native.graphics_read(GraphicsView{r.gpu});memory.write(GuestAddress{r.cpu},std::span<const std::uint8_t>(data).first(std::min<std::size_t>(data.size(),static_cast<std::size_t>(r.width))));
    };
    if(kind==Kind::SwapChain){
        auto& swap=bridge.swaps.at(self);
        if(member=="Present" || member=="Present1"){
            if(u32(2)&1)return finish(ok); // DXGI_PRESENT_TEST。
            auto& buffer=bridge.resources.at(swap.buffers[swap.index]);GraphicsView view{buffer.gpu};
            try{
                native.graphics_handoff(*swap.native,view);swap.native->swap_interval(u32(1)?1:0);
                if(gui_.windows.contains(swap.window))swap.native->graphics_present(view);
                swap.native->graphics_handoff(native,view);
            }catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=Present 失敗：")+error.what());return finish(fail);}
            swap.index=(swap.index+1)%static_cast<std::uint32_t>(swap.buffers.size());++swap.frames;return finish(ok);
        }
        if(member=="GetBuffer"){if(!arg(3))return finish(pointer_error);store(arg(3),64,0);if(u32(1)>=swap.buffers.size())return finish(invalid);return finish(query(swap.buffers[u32(1)],arg(2),arg(3)));}
        if(member=="GetCurrentBackBufferIndex")return finish(swap.index);
        if(member=="GetDevice")return finish(query(swap.queue,arg(1),arg(2)));
        if(member=="GetHwnd"){if(!arg(1))return finish(pointer_error);store(arg(1),64,swap.window);return finish(ok);}
        if(member=="GetDesc1"){if(!arg(1))return finish(pointer_error);auto desc=swap.desc1;std::memcpy(desc.data(),&swap.width,4);std::memcpy(desc.data()+4,&swap.height,4);std::memcpy(desc.data()+8,&swap.format,4);memory.write(GuestAddress{arg(1)},desc);return finish(ok);}
        if(member=="GetDesc"){
            if(!arg(1))return finish(pointer_error);memory.fill(GuestAddress{arg(1)},0,72);
            const auto word=[&](unsigned at){std::uint32_t value;std::memcpy(&value,swap.desc1.data()+at,4);return value;};
            store(arg(1),32,swap.width);store(arg(1)+4,32,swap.height);store(arg(1)+8,32,60);store(arg(1)+12,32,1);store(arg(1)+16,32,swap.format);store(arg(1)+28,32,1);
            store(arg(1)+36,32,word(24));store(arg(1)+40,32,static_cast<std::uint32_t>(swap.buffers.size()));store(arg(1)+48,64,swap.window);store(arg(1)+56,32,1);store(arg(1)+60,32,word(36));store(arg(1)+64,32,swap.flags);return finish(ok);
        }
        if(member=="ResizeBuffers" || member=="ResizeBuffers1"){
            const auto count=u32(1)?u32(1):static_cast<std::uint32_t>(swap.buffers.size());const auto window=gui_.windows.find(swap.window);
            swap.width=u32(2)?u32(2):window!=gui_.windows.end()?window->second.width:swap.width;swap.height=u32(3)?u32(3):window!=gui_.windows.end()?window->second.height:swap.height;
            if(u32(4))swap.format=u32(4);if(count<2 || count>16)return finish(invalid);
            std::memcpy(swap.desc1.data()+28,&count,4);
            try{swap_buffers(swap,count);}catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=ResizeBuffers 失敗：")+error.what());return finish(invalid);}
            return finish(ok);
        }
        if(member=="GetFullscreenState"){if(arg(1))store(arg(1),32,0);if(arg(2))store(arg(2),64,0);return finish(ok);}
        if(member=="SetFullscreenState"){if(u32(1))report("SetFullscreenState(TRUE)");return finish(ok);}
        if(member=="GetLastPresentCount"){if(!arg(1))return finish(pointer_error);store(arg(1),32,swap.frames);return finish(ok);}
        if(member=="GetFrameStatistics")return finish(0x887a000bU); // DXGI_ERROR_FRAME_STATISTICS_DISJOINT。
        if(member=="GetContainingOutput" || member=="GetParent" || member=="GetRestrictToOutput" || member=="GetCoreWindow"){if(arg(member=="GetParent" || member=="GetCoreWindow"?2:1))store(arg(member=="GetParent" || member=="GetCoreWindow"?2:1),64,0);return finish(member=="GetParent"?no_interface:unsupported);}
        if(member=="CheckColorSpaceSupport"){if(!arg(2))return finish(pointer_error);store(arg(2),32,u32(1)==0?1:0);return finish(ok);}
        if(member=="SetColorSpace1")return finish(u32(1)==0?ok:invalid);
        if(member=="SetMaximumFrameLatency" || member=="SetBackgroundColor" || member=="SetRotation" || member=="SetSourceSize" || member=="SetMatrixTransform" || member=="SetHDRMetaData" || member=="ResizeTarget")return finish(ok);
        if(member=="GetMaximumFrameLatency"){if(arg(1))store(arg(1),32,3);return finish(ok);}
        if(member=="GetSourceSize"){if(arg(1))store(arg(1),32,swap.width);if(arg(2))store(arg(2),32,swap.height);return finish(ok);}
        if(member=="GetRotation"){if(arg(1))store(arg(1),32,1);return finish(ok);}
        if(member=="IsTemporaryMonoSupported")return finish(0);
        report("Swap."+std::string(member));return finish(not_implemented);
    }
    if(kind==Kind::Device){
        if(member=="CreatePipelineLibrary"){
            // 空資料建立新庫；資料須為本實作 Serialize 的格式，其他驅動或版本的快取回 DRIVER_VERSION_MISMATCH，損毀回 E_INVALIDARG。
            std::map<std::u16string,std::uint64_t> names;const auto size=arg(2);
            if(size){
                if(!arg(1))return finish(invalid);if(size<20)return finish(driver_version_mismatch);
                const auto data=bytes(arg(1),static_cast<std::size_t>(std::min<std::uint64_t>(size,UINT64_C(64)*1024*1024)));
                if(!std::equal(library_magic.begin(),library_magic.end(),data.begin()))return finish(driver_version_mismatch);
                std::uint32_t count;std::memcpy(&count,data.data()+16,4);std::size_t at=20;
                for(std::uint32_t i=0;i<count;++i){
                    if(at+4>data.size())return finish(invalid);std::uint32_t length;std::memcpy(&length,data.data()+at,4);at+=4;
                    if(length>32768 || at+std::size_t{length}*2>data.size())return finish(invalid);
                    std::u16string name(length,0);std::memcpy(name.data(),data.data()+at,std::size_t{length}*2);at+=std::size_t{length}*2;names.emplace(std::move(name),0);
                }
            }
            if(!arg(4))return finish(false_ok);
            const auto object=create(Kind::PipelineLibrary,self);bridge.libraries[object]=std::move(names);return finish(give(object,arg(3),arg(4)));
        }
        // 駐留管理：Apple Silicon 為統一記憶體，沒有可被換出的顯示記憶體預算，所有 pageable 物件恆駐留；
        // EnqueueMakeResident 因此立即把 fence 推進到要求的值。物件須屬於本裝置的 pageable 種類。
        if(member=="MakeResident" || member=="Evict" || member=="SetResidencyPriority" || member=="EnqueueMakeResident"){
            const bool enqueue=member=="EnqueueMakeResident";const std::uint32_t count=u32(enqueue?2:1);const std::uint64_t list=arg(enqueue?3:2);
            if(enqueue && (u32(1)&~1U))return finish(invalid);if(count && !list)return finish(invalid);
            for(std::uint32_t i=0;i<count;++i){const auto object=bridge.objects.find(load(list+i*8,64));
                if(object==bridge.objects.end() || object->second.kind==Kind::Device || object->second.kind==Kind::Blob || object->second.kind==Kind::SwapChain)return finish(invalid);}
            if(enqueue){
                const auto fence=bridge.fences.find(arg(4));if(fence==bridge.fences.end())return finish(invalid);
                fence->second.value=std::max(fence->second.value,arg(5));
                std::erase_if(fence->second.events,[&](const auto& item){if(item.first>fence->second.value)return false;if(const auto event=runtime_.wait_objects.find(item.second);event!=runtime_.wait_objects.end())event->second.set_signal(true);return true;});
            }
            return finish(ok);
        }
        if(member=="GetNodeCount")return finish(1);
        if(member=="GetDescriptorHandleIncrementSize")return finish(descriptor_size);
        if(member=="GetDeviceRemovedReason")return finish(ok);
        if(member=="GetAdapterLuid"){if(arg(1))store(arg(1),64,display_identity::adapter_luid);return finish(arg(1));}
        if(member=="CheckFeatureSupport"){
            const auto feature=u32(1);const auto data=arg(2);const auto size=u32(3);if(!data)return finish(invalid);
            const auto zero=[&](std::uint32_t expected){if(size!=expected)return false;memory.fill(GuestAddress{data},0,expected);return true;};
            switch(feature){
            case 0:if(!zero(60))return finish(invalid);store(data+16,32,2);store(data+36,32,40);store(data+56,32,1);return finish(ok); // OPTIONS：ResourceBindingTier 2、VA 40 位元、ResourceHeapTier 1。
            case 1:if(!zero(16))return finish(invalid);store(data+4,32,1);store(data+8,32,1);store(data+12,32,1);return finish(ok); // ARCHITECTURE：TBDR、UMA、cache coherent。
            case 2:{if(size!=24)return finish(invalid);const auto count=load(data,32),list=load(data+8,64);std::uint32_t best=0;
                for(std::uint64_t i=0;i<count;++i){const auto level=static_cast<std::uint32_t>(load(list+i*4,32));if(level<=0xc100)best=std::max(best,level);}store(data+16,32,best);return finish(best?ok:unsupported);}
            case 3:{
                // FORMAT_SUPPORT：共用後端可對應的格式（含 BC 區塊壓縮）回報支援；其餘格式回報 0（不擲出例外）。
                if(size!=12)return finish(invalid);const auto format=static_cast<std::uint32_t>(load(data,32));bool known=graphics_format_bc_family(format)!=0;
                if(!known)try{known=graphics_format_bytes(format)!=0;}catch(const std::invalid_argument&){known=false;}
                store(data+4,32,known?0x0fffffffU:0U);store(data+8,32,0);return finish(ok);}
            case 4:if(size!=16)return finish(invalid);store(data+12,32,load(data+4,32)==1 || native.graphics_samples_supported(static_cast<std::uint32_t>(load(data,32)),static_cast<std::uint32_t>(load(data+4,32)))?1:0);return finish(ok);
            case 5:if(size!=8)return finish(invalid);store(data+4,32,1);return finish(ok);
            case 6:if(!zero(8))return finish(invalid);store(data,32,40);store(data+4,32,40);return finish(ok);
            case 7:if(size!=4)return finish(invalid);store(data,32,std::min<std::uint64_t>(load(data,32),0x60));return finish(ok); // 最高 SM6.0。
            case 12:if(size!=4)return finish(invalid);store(data,32,std::min<std::uint64_t>(load(data,32),2));return finish(ok);
            case 16:if(size!=20)return finish(invalid);store(data+4,32,1);store(data+8,32,1);store(data+12,32,1);store(data+16,32,1);return finish(ok);
            case 8:if(!zero(24))return finish(invalid);store(data,32,1);store(data+4,32,32);store(data+8,32,32);store(data+12,32,1024);store(data+16,32,1);store(data+20,32,1);return finish(ok); // OPTIONS1：wave 寬 32（Metal SIMD group）、ExpandedComputeResourceStates、64 位元整數。
            case 20:if(size!=12)return finish(invalid);store(data+8,32,load(data+4,32)==0 || load(data+4,32)==100?1:0);return finish(ok); // 佇列優先權：NORMAL／HIGH。
            // 其餘選項結構：未支援的能力回報 FALSE／tier 0（大小依 d3d12.h）。
            case 18:case 39:if(!zero(8))return finish(invalid);return finish(ok);
            case 19:case 22:case 36:case 40:if(!zero(4))return finish(invalid);return finish(ok);
            case 21:case 30:if(!zero(20))return finish(invalid);return finish(ok);
            case 23:case 27:case 41:if(!zero(12))return finish(invalid);return finish(ok);
            case 32:if(!zero(8))return finish(invalid);return finish(ok);
            case 37:case 42:if(!zero(24))return finish(invalid);return finish(ok);
            default:report("CheckFeatureSupport("+std::to_string(feature)+")");return finish(invalid);
            }
        }
        if(member=="CreateCommandQueue"){
            if(!arg(1))return finish(invalid);const auto type=static_cast<std::uint32_t>(load(arg(1),32));if(type>3 && type!=7)return finish(invalid);
            if(!arg(3))return finish(false_ok);
            const auto queue=create(Kind::Queue,self);bridge.queues[queue]=type;return finish(give(queue,arg(2),arg(3)));
        }
        if(member=="CreateCommandAllocator"){if(!arg(3))return finish(false_ok);return finish(give(create(Kind::Allocator,self),arg(2),arg(3)));}
        if(member=="CreateCommandList" || member=="CreateCommandList1"){
            const bool closed=member=="CreateCommandList1";const auto out=arg(closed?5:6),riid=arg(closed?4:5);
            if(!out)return finish(false_ok);
            const auto list=create(Kind::List,self);D3D12State::List record;record.type=u32(2);record.allocator=closed?0:arg(3);record.open=!closed;
            if(!closed && arg(4))record.commands.push_back({"SetPipelineState",{arg(4)},{},{}});
            bridge.lists[list]=std::move(record);return finish(give(list,riid,out));
        }
        if(member=="CreateFence"){
            if(!arg(4))return finish(false_ok);
            const auto fence=create(Kind::Fence,self);bridge.fences[fence].value=arg(1);return finish(give(fence,arg(3),arg(4)));
        }
        if(member=="CreateDescriptorHeap"){
            if(!arg(1))return finish(invalid);
            D3D12State::Heap heap;heap.type=static_cast<std::uint32_t>(load(arg(1),32));heap.count=static_cast<std::uint32_t>(load(arg(1)+4,32));heap.visible=(load(arg(1)+8,32)&1)!=0;
            if(heap.type>3 || !heap.count || heap.count>1000000)return finish(invalid);
            if(!arg(3))return finish(false_ok);
            heap.cpu=bridge.next_descriptor;bridge.next_descriptor+=(std::uint64_t{heap.count}*descriptor_size+0xffffU)&~UINT64_C(0xffff);
            if(heap.visible){heap.gpu=bridge.next_descriptor;bridge.next_descriptor+=(std::uint64_t{heap.count}*descriptor_size+0xffffU)&~UINT64_C(0xffff);}
            heap.descriptors.resize(heap.count);
            if(heap.visible && heap.type==0){heap.dirty.assign(heap.count,1);heap.any_dirty=true;}
            const auto object=create(Kind::DescriptorHeap,self);bridge.heaps[object]=std::move(heap);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="CreateRootSignature"){
            if(!arg(2) || !arg(3))return finish(invalid);
            const auto data=bytes(arg(2),static_cast<std::size_t>(std::min<std::uint64_t>(arg(3),16U*1024U*1024U)));
            vkd3d_shader_code code{data.data(),data.size()};vkd3d_shader_versioned_root_signature_desc parsed{},converted{};char* messages=nullptr;
            if(vkd3d_shader_parse_root_signature(&code,&parsed,&messages)<0){vkd3d_shader_free_messages(messages);return finish(invalid);}
            vkd3d_shader_free_messages(messages);
            const auto* desc=&parsed;if(parsed.version!=VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1){if(vkd3d_shader_convert_root_signature(&converted,VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1,&parsed)<0){vkd3d_shader_free_root_signature(&parsed);return finish(invalid);}desc=&converted;}
            D3D12State::RootSignature root;
            for(unsigned i=0;i<desc->u.v_1_1.parameter_count;++i){
                const auto& p=desc->u.v_1_1.parameters[i];D3D12State::Parameter parameter;parameter.type=p.parameter_type;
                if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
                    std::uint32_t offset=0;
                    for(unsigned j=0;j<p.u.descriptor_table.descriptor_range_count;++j){
                        const auto& r=p.u.descriptor_table.descriptor_ranges[j];
                        if(r.descriptor_table_offset!=0xffffffffU)offset=r.descriptor_table_offset;
                        parameter.ranges.push_back({static_cast<std::uint32_t>(r.range_type),r.descriptor_count,r.base_shader_register,r.register_space,offset});
                        if(r.descriptor_count!=~0U)offset+=r.descriptor_count;
                    }
                }else if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){parameter.reg=p.u.constants.shader_register;parameter.space=p.u.constants.register_space;parameter.values=p.u.constants.value_count;}
                else {parameter.reg=p.u.descriptor.shader_register;parameter.space=p.u.descriptor.register_space;}
                root.parameters.push_back(std::move(parameter));
            }
            for(unsigned i=0;i<desc->u.v_1_1.static_sampler_count;++i){
                const auto& s=desc->u.v_1_1.static_samplers[i];std::vector<std::uint8_t> sampler(52);
                const auto put=[&](unsigned at,std::uint32_t value){std::memcpy(sampler.data()+at,&value,4);};
                put(0,s.filter);put(4,s.address_u);put(8,s.address_v);put(12,s.address_w);put(16,std::bit_cast<std::uint32_t>(s.mip_lod_bias));put(20,s.max_anisotropy);put(24,s.comparison_func);
                const float border=s.border_colour==VKD3D_SHADER_STATIC_BORDER_COLOUR_TRANSPARENT_BLACK?0.0F:s.border_colour==VKD3D_SHADER_STATIC_BORDER_COLOUR_OPAQUE_BLACK?0.0F:1.0F;
                const float alpha=s.border_colour==VKD3D_SHADER_STATIC_BORDER_COLOUR_TRANSPARENT_BLACK?0.0F:1.0F;
                put(28,std::bit_cast<std::uint32_t>(border));put(32,std::bit_cast<std::uint32_t>(border));put(36,std::bit_cast<std::uint32_t>(border));put(40,std::bit_cast<std::uint32_t>(alpha));
                put(44,std::bit_cast<std::uint32_t>(s.min_lod));put(48,std::bit_cast<std::uint32_t>(s.max_lod));
                root.samplers.push_back({s.shader_register,native.graphics_sampler(sampler)});
            }
            vkd3d_shader_free_root_signature(&parsed);if(desc==&converted)vkd3d_shader_free_root_signature(&converted);
            if(!arg(5))return finish(false_ok);
            const auto object=create(Kind::RootSignature,self);bridge.roots[object]=std::move(root);return finish(give(object,arg(4),arg(5)));
        }
        if(member=="CreateGraphicsPipelineState"){
            // D3D12_GRAPHICS_PIPELINE_STATE_DESC（x64，656 bytes）轉為共用後端的 D3D11 狀態格式。
            if(!arg(1))return finish(invalid);
            const auto desc=bytes(arg(1),656);
            const auto word=[&](std::size_t at){std::uint32_t value;std::memcpy(&value,desc.data()+at,4);return value;};
            const auto quad=[&](std::size_t at){std::uint64_t value;std::memcpy(&value,desc.data()+at,8);return value;};
            for(const auto at:{40U,56U,72U})if(quad(at) && quad(at+8)){report("CreateGraphicsPipelineState(GS／HS／DS)");return finish(not_implemented);}
            if(quad(88) || !quad(8) || !quad(16))return finish(invalid);
            D3D12State::Pipeline pipeline;pipeline.root=quad(0);
            try{
                pipeline.shader=native.graphics_shader(0,bytes(quad(8),static_cast<std::size_t>(std::min<std::uint64_t>(quad(16),16U*1024U*1024U))));
                if(quad(24) && quad(32))pipeline.pixel=native.graphics_shader(1,bytes(quad(24),static_cast<std::size_t>(std::min<std::uint64_t>(quad(32),16U*1024U*1024U))));
            }catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=CreateGraphicsPipelineState 失敗：")+error.what());return finish(invalid);}
            // D3D12 render target blend（40 bytes）→ D3D11（32 bytes）。
            pipeline.blend.assign(8+8*32,0);std::memcpy(pipeline.blend.data(),desc.data()+120,8);
            for(unsigned i=0;i<8;++i){const auto from=128+i*40,to=8+i*32;std::memcpy(pipeline.blend.data()+to,desc.data()+from,4);std::memcpy(pipeline.blend.data()+to+4,desc.data()+from+8,24);pipeline.blend[to+28]=desc[from+36];}
            pipeline.sample_mask=word(448);
            // D3D12 rasterizer：ScissorEnable 固定開啟，其後為多重取樣與反鋸齒線。
            pipeline.raster.assign(40,0);std::memcpy(pipeline.raster.data(),desc.data()+452,28);const std::uint32_t scissor=1;std::memcpy(pipeline.raster.data()+28,&scissor,4);std::memcpy(pipeline.raster.data()+32,desc.data()+480,8);
            pipeline.depth.assign(desc.begin()+496,desc.begin()+548);
            std::map<std::uint32_t,std::uint32_t> append;
            for(std::uint32_t i=0;i<word(560);++i){
                const auto at=quad(552)+i*32;GraphicsElement element;element.semantic=[&]{std::string text;for(std::uint64_t n=load(at,64);text.size()<256;++n){const auto c=static_cast<char>(load(n,8));if(!c)break;text.push_back(c);}return text;}();
                element.index=static_cast<std::uint32_t>(load(at+8,32));element.format=static_cast<std::uint32_t>(load(at+12,32));element.slot=static_cast<std::uint32_t>(load(at+16,32));
                element.offset=static_cast<std::uint32_t>(load(at+20,32));element.classification=static_cast<std::uint32_t>(load(at+24,32));element.step=static_cast<std::uint32_t>(load(at+28,32));
                if(element.offset==UINT32_MAX)element.offset=append[element.slot];
                append[element.slot]=element.offset+std::max(1U,graphics_format_bytes(element.format));pipeline.layout.push_back(std::move(element));
            }
            if(!arg(3))return finish(false_ok);
            const auto object=create(Kind::Pipeline,self);bridge.pipelines[object]=std::move(pipeline);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="CreateRenderTargetView" || member=="CreateDepthStencilView"){
            const bool depth=member=="CreateDepthStencilView";const auto target=descriptor(arg(3),false);if(!target)return finish(0);*target={};
            const auto r=bridge.resources.find(arg(1));if(r==bridge.resources.end() || !r->second.gpu)return finish(0);
            target->type=depth?D3D12State::Descriptor::Type::Depth:D3D12State::Descriptor::Type::Target;target->resource=arg(1);
            GraphicsView v{r->second.gpu};v.format=r->second.format;v.slices=1;
            if(arg(2)){
                const auto format=static_cast<std::uint32_t>(load(arg(2),32));if(format)v.format=format;const auto dimension=static_cast<std::uint32_t>(load(arg(2)+4,32));
                const auto base=depth?12U:8U;if(depth)v.flags=static_cast<std::uint32_t>(load(arg(2)+8,32))&3U;
                v.level=static_cast<std::uint32_t>(load(arg(2)+base,32));
                const bool array=depth?(dimension==3 || dimension==5):(dimension==3 || dimension==5);
                if(array){v.slice=static_cast<std::uint32_t>(load(arg(2)+base+4,32));v.slices=static_cast<std::uint32_t>(load(arg(2)+base+8,32));}
            }
            target->view=v;return finish(0);
        }
        if(member=="CreateComputePipelineState"){
            if(!arg(1))return finish(invalid);
            const auto root=load(arg(1),64),code=load(arg(1)+8,64),size=load(arg(1)+16,64);
            if(!code || !size || size>16U*1024U*1024U)return finish(invalid);
            std::shared_ptr<GraphicsShader> shader;
            try{shader=native.graphics_shader(5,bytes(code,static_cast<std::size_t>(size)));}
            catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=CreateComputePipelineState 失敗：")+error.what());return finish(invalid);}
            if(!arg(3))return finish(false_ok);
            const auto object=create(Kind::Pipeline,self);D3D12State::Pipeline compute;compute.compute=true;compute.root=root;compute.shader=std::move(shader);bridge.pipelines[object]=std::move(compute);return finish(give(object,arg(2),arg(3)));
        }
        // 建立資源：cpu_base 為 placed 緩衝在 CPU 可見堆積中的位址（0 表示自行配置）。
        const auto make_resource=[&](std::uint32_t heap_type,std::uint32_t cpu_page,std::uint64_t desc_at,std::uint64_t cpu_base,std::uint64_t cpu_limit,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
            D3D12State::Resource r;r.heap_type=heap_type;r.cpu_page=heap_type==4?cpu_page:0;r.desc=bytes(desc_at,56);
            if(heap_type==4 && (!cpu_page || cpu_page>3))return invalid;
            const auto word=[&](unsigned at){std::uint32_t value;std::memcpy(&value,r.desc.data()+at,4);return value;};
            r.dimension=word(0);std::memcpy(&r.width,r.desc.data()+16,8);r.height=word(24);r.depth=word(28)&0xffffU;r.levels=word(28)>>16;r.format=word(32);r.samples=word(36);r.flags=word(48);
            if(r.heap_type<1 || r.heap_type>4 || r.dimension<1 || r.dimension>4 || !r.width)return invalid;
            if(r.dimension==1){r.height=1;r.depth=1;r.levels=1;r.samples=1;if(r.width>(UINT64_C(1)<<31))return no_memory;}
            if(!r.levels){r.levels=1;for(auto size=std::max<std::uint64_t>({r.width,r.height,r.dimension==4?r.depth:1});size>1;size>>=1)++r.levels;}
            if(!out)return false_ok;
            if(r.dimension==1 && (r.uploads() || r.reads_back())){
                // CPU 可見緩衝：客體記憶體為後備，GPU 使用時再建立影子資源。
                if(cpu_base){if(r.width>cpu_limit)return invalid;r.cpu=cpu_base;r.owned_cpu=false;}
                else {r.cpu=allocate_heap(memory,r.width);if(!r.cpu)return no_memory;memory.fill(GuestAddress{r.cpu},0,static_cast<std::size_t>(r.width));}
                r.cpu_size=r.width;
            }else if(r.dimension==1){
                GraphicsResourceDesc d;d.dimension=1;d.width=static_cast<std::uint32_t>(r.width);d.bind=0x0c|((r.flags&4)?0x80U:0U);r.gpu=native.graphics_resource(d);
            }else{
                if(r.heap_type!=1 && !(r.heap_type==4 && r.cpu_page==1))return invalid;
                GraphicsResourceDesc d;d.dimension=r.dimension==2?2U:r.dimension==3?3U:4U;d.width=static_cast<std::uint32_t>(r.width);d.height=r.height;
                d.depth=r.dimension==4?r.depth:1;d.layers=r.dimension==4?1:r.depth;d.levels=r.levels;d.format=r.format;d.samples=std::max(1U,r.samples);
                d.bind=((r.flags&8)?0U:8U)|((r.flags&1)?0x20U:0U)|((r.flags&2)?0x40U:0U)|((r.flags&4)?0x80U:0U);
                try{r.gpu=native.graphics_resource(d);}catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=建立資源失敗：")+error.what());return invalid;}
            }
            if(r.dimension==1){r.address=bridge.next_address;bridge.next_address+=(r.width+0xffffU)&~UINT64_C(0xffff);}
            const auto object=create(Kind::Resource,self);bridge.resources[object]=std::move(r);return give(object,riid,out);
        };
        if(member=="CreateCommittedResource" || member=="CreateCommittedResource1" || member=="CreateCommittedResource2" || member=="CreateCommittedResource3"){
            // CreateCommittedResource(heapProps, heapFlags, desc, initialState, clear, riid, out)。
            // 1／2 版多 ProtectedSession；3 版改以 layout 與可轉型格式清單取代初始狀態。
            const unsigned extra=member=="CreateCommittedResource"?0:member=="CreateCommittedResource3"?3:1;
            if(!arg(1) || !arg(3))return finish(invalid);
            return finish(make_resource(static_cast<std::uint32_t>(load(arg(1),32)),static_cast<std::uint32_t>(load(arg(1)+4,32)),arg(3),0,0,arg(6+extra),arg(7+extra)));
        }
        if(member=="CreateHeap" || member=="CreateHeap1"){
            // D3D12_HEAP_DESC：大小、堆積屬性、對齊與旗標（48 bytes）。DEFAULT 堆積的 placed 資源各自配置 GPU 記憶體，不模擬別名。
            const unsigned extra=member=="CreateHeap1"?1:0;if(!arg(1))return finish(invalid);
            D3D12State::MemoryHeap heap;heap.desc=bytes(arg(1),48);std::memcpy(&heap.size,heap.desc.data(),8);std::memcpy(&heap.type,heap.desc.data()+8,4);std::memcpy(&heap.cpu_page,heap.desc.data()+12,4);
            if(!heap.size || heap.type<1 || heap.type>4 || heap.size>(UINT64_C(1)<<31))return finish(heap.size>(UINT64_C(1)<<31)?no_memory:invalid);
            if(!arg(3+extra))return finish(false_ok);
            if(heap.type==2 || heap.type==3 || (heap.type==4 && heap.cpu_page>1)){heap.cpu=allocate_heap(memory,heap.size);if(!heap.cpu)return finish(no_memory);memory.fill(GuestAddress{heap.cpu},0,static_cast<std::size_t>(heap.size));}
            const auto object=create(Kind::Heap,self);bridge.memory_heaps[object]=std::move(heap);return finish(give(object,arg(2+extra),arg(3+extra)));
        }
        if(member=="CreatePlacedResource" || member=="CreatePlacedResource1" || member=="CreatePlacedResource2"){
            // CreatePlacedResource(heap, offset, desc, state, clear, riid, out)；2 版多可轉型格式清單。
            const unsigned extra=member=="CreatePlacedResource2"?2:0;
            const auto heap=bridge.memory_heaps.find(arg(1));if(heap==bridge.memory_heaps.end() || !arg(3))return finish(invalid);
            if(arg(2)>=heap->second.size)return finish(invalid);
            return finish(make_resource(heap->second.type,heap->second.cpu_page,arg(3),heap->second.cpu?heap->second.cpu+arg(2):0,heap->second.size-arg(2),arg(6+extra),arg(7+extra)));
        }
        if(member=="CreateCommandSignature"){
            if(arg(4))store(arg(4),64,0);
            if(!arg(1) || !arg(3))return finish(invalid);
            D3D12State::CommandSignature signature;signature.stride=static_cast<std::uint32_t>(load(arg(1),32));
            const auto count=load(arg(1)+4,32),descs=load(arg(1)+8,64),node=load(arg(1)+16,32);
            if(!count || count>128 || !descs || !signature.stride || (signature.stride&3) || node>1)return finish(invalid);
            std::uint64_t packed=0;bool root_arguments=false;
            for(std::uint64_t i=0;i<count;++i){
                const auto at=descs+i*16;D3D12State::IndirectArgument a;
                a.type=static_cast<std::uint32_t>(load(at,32));a.index=static_cast<std::uint32_t>(load(at+4,32));
                a.offset=static_cast<std::uint32_t>(load(at+8,32));a.count=static_cast<std::uint32_t>(load(at+12,32));
                if(a.type<=2){if(i+1!=count)return finish(invalid);signature.operation=a.type;a.size=a.type==0?16:a.type==1?20:12;}
                else if(a.type==3){if(a.index>=32)return finish(invalid);a.size=16;}
                else if(a.type==4)a.size=16;
                else if(a.type<=8){
                    root_arguments=true;const auto root=bridge.roots.find(arg(2));
                    if(root==bridge.roots.end() || bridge.objects.at(arg(2)).device!=self || a.index>=root->second.parameters.size())return finish(invalid);
                    const auto& parameter=root->second.parameters[a.index];
                    const auto expected=a.type==5?1U:a.type-4;
                    if(parameter.type!=expected)return finish(invalid);
                    if(a.type==5){if(!a.count || a.offset>parameter.values || a.count>parameter.values-a.offset)return finish(invalid);a.size=a.count*4;}
                    else a.size=8;
                }else return finish(invalid);
                packed+=a.size;signature.arguments.push_back(a);
            }
            if(signature.arguments.back().type>2 || packed>signature.stride || (!root_arguments && arg(2)))return finish(invalid);
            if(signature.operation==2)for(const auto& a:signature.arguments)if(a.type==3 || a.type==4)return finish(invalid);
            signature.root=root_arguments?arg(2):0;
            const auto* abi=d3d12abi::find_interface("ID3D12CommandSignature");
            if(read_guid(memory,arg(3))!=abi->iid && read_guid(memory,arg(3))!=iid_unknown)return finish(no_interface);
            if(!arg(4))return finish(false_ok);
            const auto object=create(Kind::CommandSignature,self);bridge.signatures[object]=std::move(signature);
            return finish(give(object,arg(3),arg(4)));
        }
        if(member=="CreateQueryHeap"){
            if(!arg(1))return finish(invalid);
            D3D12State::QueryHeap heap;heap.type=static_cast<std::uint32_t>(load(arg(1),32));heap.count=static_cast<std::uint32_t>(load(arg(1)+4,32));
            if(!heap.count || heap.count>65536)return finish(invalid);
            if(heap.type!=0 && heap.type!=1 && heap.type!=2){report("CreateQueryHeap(type="+std::to_string(heap.type)+")");return finish(not_implemented);}
            if(!arg(3))return finish(false_ok);
            heap.timestamps.resize(heap.count);heap.occlusion.resize(heap.count);
            const auto object=create(Kind::QueryHeap,self);bridge.query_heaps[object]=std::move(heap);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="GetResourceAllocationInfo"){
            // GetResourceAllocationInfo(RetVal, visibleMask, count, descs)。
            std::uint64_t total=0;
            for(std::uint64_t i=0;i<arg(3);++i){const auto d=arg(4)+i*56;const auto width=load(d+16,64),height=load(d+24,32),layers=load(d+28,32)&0xffffU;
                const auto dimension=load(d,32);const auto format=static_cast<std::uint32_t>(load(d+32,32));
                // 依列距（BC 以 4×4 區塊計）估計含 mip 鏈的大小（約為最上層的兩倍）。
                const auto pitch=dimension==1?std::array<std::uint32_t,2>{1U,1U}:graphics_format_pitch(format,static_cast<std::uint32_t>(std::min<std::uint64_t>(width,UINT32_MAX)),static_cast<std::uint32_t>(std::max<std::uint64_t>(height,1)));
                const auto size=dimension==1?width:std::uint64_t{pitch[0]}*pitch[1]*std::max<std::uint64_t>(layers,1)*2;
                total+=(size+0xffffU)&~UINT64_C(0xffff);}
            if(arg(1)){store(arg(1),64,total);store(arg(1)+8,64,0x10000);}return finish(arg(1));
        }
        if(member=="GetCopyableFootprints"){
            // (desc, first, count, base, layouts, rows, row_sizes, total)：列距 256、子資源偏移 512 對齊。
            if(!arg(1))return finish(0);
            const auto d=bytes(arg(1),56);const auto word=[&](unsigned at){std::uint32_t value;std::memcpy(&value,d.data()+at,4);return value;};
            std::uint64_t width;std::memcpy(&width,d.data()+16,8);const auto dimension=word(0),height=dimension==1?1U:word(24),depth_or_layers=dimension==1?1U:(word(28)&0xffffU),format=word(32);
            auto levels=dimension==1?1U:(word(28)>>16);if(!levels){levels=1;for(auto size=std::max<std::uint64_t>({width,height,dimension==4?depth_or_layers:1U});size>1;size>>=1)++levels;}
            std::uint64_t offset=arg(4),total=0;
            for(std::uint32_t i=0;i<u32(3);++i){
                const auto sub=u32(2)+i;const auto level=sub%levels;
                const std::uint64_t w=std::max<std::uint64_t>(1,width>>level);const std::uint32_t h=std::max(1U,height>>level),z=dimension==4?std::max(1U,depth_or_layers>>level):1U;
                const auto pitch=dimension==1?std::array<std::uint32_t,2>{static_cast<std::uint32_t>(w),1U}:graphics_format_pitch(format,static_cast<std::uint32_t>(w),h);
                const auto row=(std::uint64_t{pitch[0]}+255U)&~UINT64_C(255);offset=(offset+511U)&~UINT64_C(511);
                if(arg(5)){const auto at=arg(5)+i*32;store(at,64,offset);store(at+8,32,format);store(at+12,32,w);store(at+16,32,h);store(at+20,32,z);store(at+24,32,row);}
                if(arg(6))store(arg(6)+i*4,32,pitch[1]);if(arg(7))store(arg(7)+i*8,64,pitch[0]);
                const auto size=row*(std::uint64_t{pitch[1]}*z-1)+pitch[0];total=offset+size-arg(4);offset+=row*pitch[1]*z;
            }
            if(arg(8))store(arg(8),64,total);return finish(0);
        }
        // 描述元建立：寫入目的堆積的對應槽。
        if(member=="CreateConstantBufferView"){
            const auto target=descriptor(arg(2),false);if(!target || !arg(1))return finish(0);
            *target={};target->type=D3D12State::Descriptor::Type::Constant;target->address=load(arg(1),64);target->size=static_cast<std::uint32_t>(load(arg(1)+8,32));return finish(0);
        }
        if(member=="CreateShaderResourceView" || member=="CreateUnorderedAccessView"){
            const bool uav=member=="CreateUnorderedAccessView";const auto handle=arg(uav?4:3),desc=arg(uav?3:2);
            const auto target=descriptor(handle,false);if(!target)return finish(0);*target={};
            if(!arg(1)){target->type=uav?D3D12State::Descriptor::Type::Unordered:D3D12State::Descriptor::Type::Shader;return finish(0);} // 空描述元。
            const auto r=bridge.resources.find(arg(1));if(r==bridge.resources.end())return finish(0);
            auto& resource=r->second;target->type=uav?D3D12State::Descriptor::Type::Unordered:D3D12State::Descriptor::Type::Shader;target->resource=arg(1);
            GraphicsView v;v.format=resource.format;v.levels=resource.levels;v.slices=resource.dimension==4?1:resource.depth;
            if(desc){
                v.format=static_cast<std::uint32_t>(load(desc,32));if(!v.format)v.format=resource.format;
                const auto view_dimension=static_cast<std::uint32_t>(load(desc+4,32));v.dimension=view_dimension;
                if(view_dimension==1){
                    const auto base=uav?8U:16U;const std::uint64_t first=load(desc+base,64),count=load(desc+base+8,32);const std::uint32_t stride=static_cast<std::uint32_t>(load(desc+base+12,32)),flags=static_cast<std::uint32_t>(load(desc+base+(uav?24:16),32));
                    const auto element=stride?stride:(flags&1)?4U:std::max(1U,graphics_format_bytes(v.format));
                    v.first=static_cast<std::uint32_t>(first*element);v.count=static_cast<std::uint32_t>(count*element);v.flags=flags&1;if(stride)resource.structure=stride;
                    // 結構化／原始緩衝在 SPIR-V 路徑為 uint 紋理緩衝；HLSLcc 路徑以裝置緩衝綁定、不使用格式。
                    if(stride || (flags&1) || v.format==39)v.format=42;
                }else if(!uav){
                    v.level=static_cast<std::uint32_t>(load(desc+16,32));const auto levels=static_cast<std::uint32_t>(load(desc+20,32));v.levels=levels==UINT32_MAX?resource.levels-v.level:std::max(1U,levels);
                    if(view_dimension==3 || view_dimension==5 || view_dimension==10){v.slice=static_cast<std::uint32_t>(load(desc+24,32));v.slices=static_cast<std::uint32_t>(load(desc+28,32));}
                }else{
                    v.level=static_cast<std::uint32_t>(load(desc+8,32));v.levels=1;
                    if(view_dimension==3 || view_dimension==5 || view_dimension==8){v.slice=static_cast<std::uint32_t>(load(desc+12,32));v.slices=static_cast<std::uint32_t>(load(desc+16,32));
                        if(view_dimension==8 && v.slices==UINT32_MAX){const auto depth=std::max(1U,resource.depth>>std::min(v.level,31U));v.slices=v.slice<depth?depth-v.slice:0;}}
                }
            }else if(resource.dimension==1){v.first=0;v.count=static_cast<std::uint32_t>(resource.width);v.levels=1;v.format=42;}
            target->view=v;return finish(0);
        }
        if(member=="CreateSampler"){
            const auto target=descriptor(arg(2),false);if(!target || !arg(1))return finish(0);
            *target={};target->type=D3D12State::Descriptor::Type::Sampler;target->sampler=native.graphics_sampler(bytes(arg(1),52));return finish(0);
        }
        if(member=="CopyDescriptorsSimple"){
            for(std::uint32_t i=0;i<u32(1);++i){const auto dst=descriptor(arg(2)+i*descriptor_size,false),src=descriptor(arg(3)+i*descriptor_size,false);if(dst && src)*dst=*src;}
            return finish(0);
        }
        if(member=="CopyDescriptors"){
            // (dstRanges, dstStarts, dstSizes, srcRanges, srcStarts, srcSizes, type)。
            std::vector<D3D12State::Descriptor> sources;
            for(std::uint32_t i=0;i<u32(4);++i){const auto start=load(arg(5)+i*8,64),count=arg(6)?load(arg(6)+i*4,32):1;for(std::uint64_t j=0;j<count;++j){const auto src=descriptor(start+j*descriptor_size,false);sources.push_back(src?*src:D3D12State::Descriptor{});}}
            std::size_t at=0;
            for(std::uint32_t i=0;i<u32(1);++i){const auto start=load(arg(2)+i*8,64),count=arg(3)?load(arg(3)+i*4,32):1;for(std::uint64_t j=0;j<count && at<sources.size();++j){if(const auto dst=descriptor(start+j*descriptor_size,false))*dst=sources[at];++at;}}
            return finish(0);
        }
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::DescriptorHeap){
        auto& heap=bridge.heaps.at(self);
        if(member=="GetDesc"){if(arg(1)){store(arg(1),32,heap.type);store(arg(1)+4,32,heap.count);store(arg(1)+8,32,heap.visible?1:0);store(arg(1)+12,32,0);}return finish(arg(1));}
        if(member=="GetCPUDescriptorHandleForHeapStart"){if(arg(1))store(arg(1),64,heap.cpu);return finish(arg(1));}
        if(member=="GetGPUDescriptorHandleForHeapStart"){if(arg(1))store(arg(1),64,heap.gpu);return finish(arg(1));}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Resource){
        auto& r=bridge.resources.at(self);
        if(member=="GetGPUVirtualAddress")return finish(r.address);
        if(member=="GetDesc"){if(arg(1))memory.write(GuestAddress{arg(1)},r.desc);return finish(arg(1));}
        if(member=="Map"){
            if(!r.cpu)return finish(invalid);
            // READBACK：映射時取得 GPU 最新內容。
            if(r.reads_back() && r.gpu)read_back(self);
            if(arg(3))store(arg(3),64,r.cpu);return finish(ok);
        }
        if(member=="Unmap")return finish(0);
        if(member=="GetHeapProperties"){if(arg(1)){store(arg(1),32,r.heap_type);store(arg(1)+4,32,r.cpu_page);store(arg(1)+8,32,r.heap_type==4?1:0);store(arg(1)+12,32,1);store(arg(1)+16,32,1);}if(arg(2))store(arg(2),32,0);return finish(ok);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Fence){
        auto& fence=bridge.fences.at(self);
        const auto signal=[&](std::uint64_t value){
            fence.value=value;
            std::erase_if(fence.events,[&](const auto& item){if(item.first>fence.value)return false;if(const auto event=runtime_.wait_objects.find(item.second);event!=runtime_.wait_objects.end())event->second.set_signal(true);return true;});
        };
        if(member=="GetCompletedValue")return finish(fence.value);
        if(member=="Signal"){signal(arg(1));return finish(ok);}
        if(member=="SetEventOnCompletion"){
            if(!arg(2)){if(fence.value>=arg(1))return finish(ok);report("SetEventOnCompletion(NULL) 阻塞等候");return finish(fail);}
            fence.events.push_back({arg(1),arg(2)});signal(fence.value);return finish(ok);
        }
        if(member=="GetCreationFlags")return finish(0);
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Allocator){if(member=="Reset")return finish(ok);report(symbol);return finish(not_implemented);}
    if(kind==Kind::PipelineLibrary){
        auto& library=bridge.libraries.at(self);
        const auto text=[&](std::uint64_t at){return read_utf16(memory,GuestAddress{at},32768);};
        if(member=="StorePipeline"){
            // 名稱不可重複；庫持有 PSO 參照直到釋放。
            if(!arg(1) || !arg(2))return finish(invalid);const auto pipeline=bridge.objects.find(arg(2));
            if(pipeline==bridge.objects.end() || pipeline->second.kind!=Kind::Pipeline)return finish(invalid);
            const auto name=text(arg(1));if(library.contains(name))return finish(invalid);
            ++pipeline->second.references;library.emplace(name,arg(2));return finish(ok);
        }
        if(member=="LoadGraphicsPipeline" || member=="LoadComputePipeline" || member=="LoadPipeline"){
            // 找不到名稱回 E_INVALIDARG；找到時依呼叫端描述建立 PSO（與驅動重用已編譯結果相同，回傳等價物件）。
            if(!arg(1) || !arg(2))return finish(invalid);if(arg(4))store(arg(4),64,0);
            if(!library.contains(text(arg(1))))return finish(invalid);
            const auto owner=found->second.device,desc=arg(2),riid=arg(3),out=arg(4);
            state.reg(Gpr::Rcx)=owner;state.reg(Gpr::Rdx)=desc;state.reg(Gpr::R8)=riid;state.reg(Gpr::R9)=out;
            const auto target=member=="LoadGraphicsPipeline"?"Device.CreateGraphicsPipelineState":member=="LoadComputePipeline"?"Device.CreateComputePipelineState":"Device.CreatePipelineState";
            return invoke_d3d12(state,memory,WindowsServiceDescriptor{"internal-d3d12",target,4});
        }
        std::vector<std::uint8_t> serialized(library_magic.begin(),library_magic.end());
        const auto append=[&](std::uint32_t value){for(unsigned b=0;b<4;++b)serialized.push_back(static_cast<std::uint8_t>(value>>(8*b)));};
        append(static_cast<std::uint32_t>(library.size()));
        for(const auto& [name,pipeline]:library){(void)pipeline;append(static_cast<std::uint32_t>(name.size()));for(const auto c:name){serialized.push_back(static_cast<std::uint8_t>(c));serialized.push_back(static_cast<std::uint8_t>(c>>8));}}
        if(member=="GetSerializedSize")return finish(serialized.size());
        if(member=="Serialize"){if(!arg(1) || arg(2)<serialized.size())return finish(invalid);memory.write(GuestAddress{arg(1)},serialized);return finish(ok);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Heap){
        auto& heap=bridge.memory_heaps.at(self);
        if(member=="GetDesc"){if(arg(1))memory.write(GuestAddress{arg(1)},heap.desc);return finish(arg(1));}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::RootSignature || kind==Kind::Pipeline || kind==Kind::QueryHeap){
        if(member=="GetCachedBlob"){if(arg(1))store(arg(1),64,0);return finish(not_implemented);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::List){
        auto& list=bridge.lists.at(self);
        if(member=="GetType")return finish(list.type);
        if(member=="Close"){if(!list.open)return finish(fail);list.open=false;return finish(list.invalid_recording?invalid:ok);}
        if(member=="Reset"){list.commands.clear();list.callbacks.clear();list.open=true;list.invalid_recording=false;list.allocator=arg(1);if(arg(2))list.commands.push_back({"SetPipelineState",{arg(2)},{},{}});return finish(ok);}
        if(!list.open)return finish(0);
        if(member=="ExecuteBundle"){
            const auto bundle=bridge.lists.find(arg(1));
            if(list.type!=0 || bundle==bridge.lists.end() || bundle->second.type!=1 || bundle->second.open || bundle->second.invalid_recording || bridge.objects.at(arg(1)).device!=found->second.device){list.invalid_recording=true;return finish(0);}
        }
        if(list.type==1 && member=="SetPredication"){list.invalid_recording=true;return finish(0);}
        if(list.type==1 && member=="ExecuteIndirect"){
            const auto signature=bridge.signatures.find(arg(1));
            if(arg(5) || signature==bridge.signatures.end() || signature->second.arguments.size()!=1 || signature->second.operation==2){list.invalid_recording=true;return finish(0);}
        }
        // 錄製：保留原始參數與需要立即讀取的客體資料。
        D3D12State::Command command;command.op=std::string(member);
        for(unsigned i=1;i<service.arguments;++i)command.values.push_back(arg(i));
        if(member=="SetComputeRoot32BitConstants" || member=="SetGraphicsRoot32BitConstants")command.data=bytes(arg(3),std::size_t{u32(2)}*4);
        else if(member=="SetDescriptorHeaps")for(std::uint32_t i=0;i<u32(1);++i)command.values.push_back(load(arg(2)+i*8,64));
        else if(member=="CopyTextureRegion"){command.data=bytes(arg(1),48);const auto src=bytes(arg(5),48);command.data.insert(command.data.end(),src.begin(),src.end());if(arg(6)){const auto box=bytes(arg(6),24);command.data.insert(command.data.end(),box.begin(),box.end());}}
        else if(member=="ClearUnorderedAccessViewUint" || member=="ClearUnorderedAccessViewFloat"){
            const auto entry=descriptor(arg(2),false);
            if(!entry || entry->type!=D3D12State::Descriptor::Type::Unordered || entry->resource!=arg(3) || u32(5)>65536 || (u32(5) && !arg(6))){list.invalid_recording=true;return finish(0);}
            command.views.push_back(entry->view);command.data=bytes(arg(4),16);
            if(u32(5)){const auto rects=bytes(arg(6),std::size_t{u32(5)}*16);command.data.insert(command.data.end(),rects.begin(),rects.end());}
        }
        else if(member=="IASetVertexBuffers"){if(arg(3))command.data=bytes(arg(3),std::size_t{u32(2)}*16);}
        else if(member=="IASetIndexBuffer"){if(arg(1))command.data=bytes(arg(1),16);}
        else if(member=="RSSetViewports")command.data=bytes(arg(2),std::size_t{std::min(u32(1),16U)}*24);
        else if(member=="RSSetScissorRects")command.data=bytes(arg(2),std::size_t{std::min(u32(1),16U)}*16);
        else if(member=="OMSetBlendFactor"){if(arg(1))command.data=bytes(arg(1),16);else{const float one=1.0F;command.data.resize(16);for(unsigned i=0;i<4;++i)std::memcpy(command.data.data()+i*4,&one,4);}}
        else if(member=="OMSetRenderTargets"){
            // (count, handles, single_range, dsv)：單一範圍時 handles 指向第一個控制代碼。
            const auto count=std::min(u32(1),8U);command.data={static_cast<std::uint8_t>(count),static_cast<std::uint8_t>(arg(4)?1:0)};
            for(std::uint32_t i=0;i<count;++i){const auto handle=u32(3)?load(arg(2),64)+i*descriptor_size:load(arg(2)+i*8,64);const auto entry=descriptor(handle,false);command.views.push_back(entry?entry->view:GraphicsView{});}
            if(arg(4)){const auto entry=descriptor(load(arg(4),64),false);command.views.push_back(entry?entry->view:GraphicsView{});}
        }
        else if(member=="ClearRenderTargetView"){const auto entry=descriptor(arg(1),false);command.views.push_back(entry?entry->view:GraphicsView{});command.data=bytes(arg(2),16);}
        else if(member=="ClearDepthStencilView"){
            // x64：Depth 為第 4 個參數，以 XMM3 傳遞。
            const auto entry=descriptor(arg(1),false);command.views.push_back(entry?entry->view:GraphicsView{});command.data.assign(state.xmm[3].begin(),state.xmm[3].begin()+4);
        }
        list.commands.push_back(std::move(command));return finish(0);
    }
    if(kind==Kind::Queue){
        if(member=="GetTimestampFrequency"){
            // 與 EndQuery(TIMESTAMP) 使用同一時基：後端支援 GPU 時戳時取其頻率，否則為宿主奈秒。
            if(!arg(1))return finish(pointer_error);std::uint64_t frequency=1000000000;
            try{if(native.graphics_timestamp_supported())frequency=native.graphics_timestamp().frequency;}catch(const std::exception&){}
            store(arg(1),64,frequency);return finish(ok);
        }
        if(member=="GetDesc"){if(arg(1)){store(arg(1),32,bridge.queues.at(self));store(arg(1)+4,32,0);store(arg(1)+8,32,0);store(arg(1)+12,32,0);}return finish(arg(1));}
        if(member=="Wait")return finish(ok);
        if(member=="Signal"){
            const auto fence=bridge.fences.find(arg(1));if(fence==bridge.fences.end())return finish(invalid);
            fence->second.value=arg(2);
            std::erase_if(fence->second.events,[&](const auto& item){if(item.first>fence->second.value)return false;if(const auto event=runtime_.wait_objects.find(item.second);event!=runtime_.wait_objects.end())event->second.set_signal(true);return true;});
            return finish(ok);
        }
        if(member=="ExecuteCommandLists"){
            std::set<std::uint64_t> written;
            try{
                for(std::uint32_t l=0;l<u32(1);++l){
                    const auto handle=load(arg(2)+l*8,64);const auto found_list=bridge.lists.find(handle);if(found_list==bridge.lists.end())continue;
                    // 重播狀態：運算與繪圖各自的根簽章與根參數（D3D12 兩者獨立），以及管線、IA／RS／OM 狀態。
                    struct Roots {std::uint64_t root{};std::map<std::uint32_t,std::uint64_t> tables,views;std::map<std::uint32_t,std::vector<std::uint8_t>> constants;};
                    Roots compute_roots,graphics_roots;std::uint64_t pipeline=0;GraphicsDraw draw;std::uint32_t target_count=0;
                    std::uint64_t occlusion_heap=0;std::uint32_t occlusion_index=0;constexpr std::uint32_t occlusion_slots=4096;
                    const auto constant_buffer=[&](std::uint64_t address,std::uint32_t size)->std::shared_ptr<GraphicsResource>{
                        const auto [object,offset]=resource_at(address);if(!object)return {};auto& resource=bridge.resources.at(object);
                        if(!offset && (!size || size>=resource.width))return gpu_resource(object);
                        // 子範圍 CBV：建立對應內容的常數緩衝。
                        const auto length=static_cast<std::uint32_t>(std::min<std::uint64_t>(size?size:resource.width-offset,resource.width-offset));
                        std::vector<std::uint8_t> data;
                        if(resource.cpu)data=bytes(resource.cpu+offset,length);else{auto all=native.graphics_read(GraphicsView{resource.gpu});data.assign(all.begin()+static_cast<std::ptrdiff_t>(offset),all.begin()+static_cast<std::ptrdiff_t>(offset+length));}
                        GraphicsResourceDesc d;d.dimension=1;d.width=(length+15U)&~15U;d.bind=4;auto buffer=native.graphics_resource(d);data.resize(d.width);native.graphics_write(GraphicsView{buffer},data,d.width,d.width);return buffer;
                    };
                    // 描述元表鏡像：把待更新的描述元換成後端的資源 ID／緩衝位址，寫入 GPU 鏡像。
                    const auto refresh_mirror=[&](D3D12State::Heap& heap){
                        if(heap.dirty.empty() || (!heap.any_dirty && heap.mirrors[0]))return;
                        for(unsigned m=0;m<2;++m)if(!heap.mirrors[m]){
                            GraphicsResourceDesc d;d.dimension=1;d.width=heap.count*(m?16U:8U);d.bind=0x8c;heap.mirrors[m]=native.graphics_resource(d);heap.shadow[m].assign(d.width,0);}
                        if(heap.keep.size()<heap.count)heap.keep.resize(heap.count);
                        const auto put=[](std::vector<std::uint8_t>& out,std::size_t at,std::uint64_t value){for(unsigned b=0;b<8;++b)out[at+b]=static_cast<std::uint8_t>(value>>(8*b));};
                        for(std::uint32_t i=0;i<heap.count;++i){
                            if(!heap.dirty[i])continue;heap.dirty[i]=0;
                            const auto& entry=heap.descriptors[i];const bool uav=entry.type==D3D12State::Descriptor::Type::Unordered;GraphicsView view;
                            if((entry.type==D3D12State::Descriptor::Type::Shader || uav) && entry.resource){
                                const auto& resource=bridge.resources.at(entry.resource);view=entry.view;view.resource=resource.dimension==1?gpu_resource(entry.resource):resource.gpu;}
                            std::array<std::uint64_t,2> texture{},buffer{};std::shared_ptr<void> keep,unused;
                            if(!native.graphics_descriptor(view,uav?7U:6U,texture,keep))native.graphics_descriptor(GraphicsView{},6,texture,keep);
                            if(!native.graphics_descriptor(view,uav?9U:8U,buffer,unused))buffer={};
                            put(heap.shadow[0],std::size_t{i}*8,texture[0]);put(heap.shadow[1],std::size_t{i}*16,buffer[0]);put(heap.shadow[1],std::size_t{i}*16+8,buffer[1]&UINT32_MAX);
                            heap.keep[i]=std::move(keep);
                        }
                        for(unsigned m=0;m<2;++m){const auto width=static_cast<std::uint32_t>(heap.shadow[m].size());native.graphics_write(GraphicsView{heap.mirrors[m]},heap.shadow[m],width,width);}
                        heap.any_dirty=false;
                    };
                    // 描述元表：依著色器的 (類型, space, register) 找到根簽章中涵蓋它的描述元表範圍，換算成鏡像位置。
                    const auto bind_tables=[&](GraphicsStage& stage,Roots& roots,const D3D12State::RootSignature& signature){
                        stage.tables.clear();if(!stage.shader)return;
                        for(const auto& b:stage.shader->bindings){
                            if(b.type<6 || b.type>9)continue;
                            const std::uint32_t range_type=b.type==6 || b.type==8?VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;bool bound=false;
                            for(std::uint32_t index=0;index<signature.parameters.size() && !bound;++index){
                                const auto& parameter=signature.parameters[index];
                                if(parameter.type!=VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)continue;
                                const auto base=roots.tables[index];if(!base)continue;
                                for(const auto& range:parameter.ranges){
                                    if(range.type!=range_type || range.space!=b.space || b.slot<range.base || (range.count!=~0U && b.slot-range.base>=range.count))continue;
                                    for(auto& [object,heap]:bridge.heaps){(void)object;
                                        if(!heap.gpu || heap.dirty.empty() || base<heap.gpu || base>=heap.gpu+std::uint64_t{heap.count}*descriptor_size)continue;
                                        const auto first=(base-heap.gpu)/descriptor_size+range.offset+(b.slot-range.base);
                                        if(first>=heap.count)throw std::invalid_argument("描述元表超出堆積範圍");
                                        refresh_mirror(heap);const bool buffer_table=b.type>=8;
                                        stage.tables.push_back({b.type,b.space,b.slot,heap.mirrors[buffer_table?1:0],first*(buffer_table?16U:8U)});bound=true;break;
                                    }
                                    if(bound)break;
                                }
                            }
                            if(!bound)throw std::invalid_argument("著色器描述元表在根簽章中沒有對應範圍");
                        }
                    };
                    // 根參數 → 共用後端的 b／t／u／s 暫存器槽（register space 0）；非零 space 與大陣列走描述元表。
                    const auto bind=[&](GraphicsStage& stage,Roots& roots,const D3D12State::RootSignature& signature){
                        bind_tables(stage,roots,signature);
                        for(std::uint32_t index=0;index<signature.parameters.size();++index){
                            const auto& parameter=signature.parameters[index];
                            if(parameter.space)throw std::invalid_argument("根參數使用非零 register space");
                            if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){
                                auto data=roots.constants[index];data.resize((std::max<std::size_t>(parameter.values*4,data.size())+15U)&~std::size_t{15});
                                GraphicsResourceDesc d;d.dimension=1;d.width=static_cast<std::uint32_t>(data.size());d.bind=4;auto buffer=native.graphics_resource(d);native.graphics_write(GraphicsView{buffer},data,d.width,d.width);
                                if(parameter.reg<stage.constants.size())stage.constants[parameter.reg]=buffer;
                            }else if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_CBV){
                                if(const auto address=roots.views[index];address && parameter.reg<stage.constants.size())stage.constants[parameter.reg]=constant_buffer(address,0);
                            }else if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_SRV || parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_UAV){
                                const auto [object,offset]=resource_at(roots.views[index]);if(!object)continue;auto& resource=bridge.resources.at(object);
                                GraphicsView view{gpu_resource(object)};view.first=static_cast<std::uint32_t>(offset);view.count=static_cast<std::uint32_t>(resource.width-offset);view.format=42;
                                if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_UAV){if(parameter.reg<stage.unordered.size())stage.unordered[parameter.reg]=view;written.insert(object);}
                                else if(parameter.reg<stage.resources.size())stage.resources[parameter.reg]=view;
                            }else{
                                const auto base=roots.tables[index];if(!base)continue;
                                for(const auto& range:parameter.ranges){
                                    if(range.space)continue;
                                    // 共用後端最多 128 個 SRV 槽；更大的陣列只能經描述元表存取。
                                    const auto count=range.count==~0U?1U:std::min<std::uint32_t>(range.count,128);
                                    for(std::uint32_t i=0;i<count;++i){
                                        const auto entry=descriptor(base+(std::uint64_t{range.offset}+i)*descriptor_size,true);if(!entry)continue;const auto reg=range.base+i;
                                        if(range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER){if(reg<stage.samplers.size())stage.samplers[reg]=entry->sampler;continue;}
                                        if(range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV){if(reg<stage.constants.size() && entry->address)stage.constants[reg]=constant_buffer(entry->address,entry->size);continue;}
                                        if(!entry->resource)continue;auto view=entry->view;auto& resource=bridge.resources.at(entry->resource);view.resource=resource.dimension==1?gpu_resource(entry->resource):resource.gpu;
                                        if(range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV){if(reg<stage.unordered.size())stage.unordered[reg]=view;written.insert(entry->resource);}
                                        else if(reg<stage.resources.size())stage.resources[reg]=view;
                                    }
                                }
                            }
                        }
                        for(const auto& [reg,sampler]:signature.samplers)if(reg<stage.samplers.size())stage.samplers[reg]=sampler;
                    };
                    const auto signature_of=[&](const Roots& roots)->const D3D12State::RootSignature&{
                        const auto p=bridge.pipelines.find(pipeline);const auto r=bridge.roots.find(roots.root?roots.root:p!=bridge.pipelines.end()?p->second.root:0);
                        if(r==bridge.roots.end())throw std::invalid_argument("缺少根簽章");return r->second;
                    };
                    const auto read_buffer=[&](std::uint64_t object,std::uint64_t offset,std::uint64_t length){
                        const auto& r=bridge.resources.at(object);
                        if(r.dimension!=1 || offset>r.width || length>r.width-offset)throw std::invalid_argument("ExecuteIndirect 緩衝範圍無效");
                        if(r.cpu)return bytes(r.cpu+offset,static_cast<std::size_t>(length));
                        native.graphics_flush(true);auto data=native.graphics_read(GraphicsView{r.gpu});
                        if(offset>data.size() || length>data.size()-offset)throw std::invalid_argument("ExecuteIndirect GPU 回讀範圍無效");
                        return std::vector<std::uint8_t>(data.begin()+static_cast<std::ptrdiff_t>(offset),data.begin()+static_cast<std::ptrdiff_t>(offset+length));
                    };
                    bool predicate_skip=false,in_bundle=false;
                    // 間接命令在執行時讀取，前面的 GPU 寫入／複製因此可產生命令與計數。
                    std::deque<D3D12State::Command> pending(found_list->second.commands.begin(),found_list->second.commands.end());
                    while(!pending.empty()){
                        auto c=std::move(pending.front());pending.pop_front();
                        const auto& v=c.values;const bool graphics=c.op.find("Graphics")!=std::string::npos;auto& roots=graphics?graphics_roots:compute_roots;
                        const auto ends=[&](std::string_view suffix){return c.op.ends_with(suffix);};
                        if(predicate_skip && (c.op=="DrawInstanced" || c.op=="DrawIndexedInstanced" || c.op=="Dispatch" ||
                            c.op=="CopyTextureRegion" || c.op=="CopyBufferRegion" || c.op=="CopyResource" || c.op=="CopyTiles" ||
                            c.op=="ResolveSubresource" || c.op=="ClearDepthStencilView" || c.op=="ClearRenderTargetView" ||
                            c.op=="ClearUnorderedAccessViewUint" || c.op=="ClearUnorderedAccessViewFloat" || c.op=="ExecuteIndirect"))continue;
                        if(c.op=="BundleEnd")in_bundle=false;
                        else if(c.op=="ExecuteBundle"){
                            const auto& bundle=bridge.lists.at(v[0]);
                            if(in_bundle || found_list->second.type!=0 || bundle.type!=1 || bundle.open || bundle.invalid_recording)throw std::invalid_argument("ExecuteBundle 目標無效");
                            pipeline=0;draw.topology=0;in_bundle=true;
                            pending.push_front({"BundleEnd",{},{},{}});
                            for(auto i=bundle.commands.rbegin();i!=bundle.commands.rend();++i)pending.push_front(*i);
                        }
                        else if(c.op=="SetPredication"){
                            predicate_skip=false;
                            if(v[0]){
                                if((v[1]&7) || v[2]>1 || found_list->second.type==1)throw std::invalid_argument("SetPredication 參數無效");
                                const auto data=read_buffer(v[0],v[1],8);std::uint64_t value;std::memcpy(&value,data.data(),8);
                                // 在 SetPredication 執行當下快照 64 位值，後續緩衝變動不改變判斷。
                                predicate_skip=v[2]==0?value==0:value!=0;
                            }
                        }
                        else if(c.op=="ExecuteIndirect"){
                            const auto& signature=bridge.signatures.at(v[0]);
                            if(found_list->second.type!=0 && !(found_list->second.type==2 && signature.operation==2))throw std::invalid_argument("ExecuteIndirect 命令清單種類不符");
                            if(in_bundle && (v[4] || signature.arguments.size()!=1 || signature.operation==2))throw std::invalid_argument("Bundle 間接命令不允許修改繫結或使用計數緩衝");
                            const auto& active=signature.operation==2?compute_roots:graphics_roots;
                            if(signature.root && active.root!=signature.root)throw std::invalid_argument("ExecuteIndirect 根簽章不符");
                            if((v[3]&3) || (v[5]&3))throw std::invalid_argument("ExecuteIndirect 位移未對齊");
                            const auto& argument_resource=bridge.resources.at(v[2]);
                            if(argument_resource.dimension!=1 || v[3]>argument_resource.width || std::uint64_t{static_cast<std::uint32_t>(v[1])}*signature.stride>argument_resource.width-v[3])throw std::invalid_argument("ExecuteIndirect 最大命令範圍無效");
                            auto count=static_cast<std::uint32_t>(v[1]);
                            if(v[4]){const auto data=read_buffer(v[4],v[5],4);std::uint32_t limit;std::memcpy(&limit,data.data(),4);count=std::min(count,limit);}
                            const auto data=read_buffer(v[2],v[3],std::uint64_t{count}*signature.stride);
                            const std::string prefix=signature.operation==2?"SetComputeRoot":"SetGraphicsRoot";
                            std::vector<D3D12State::Command> expanded;
                            const auto decode=[&](const D3D12State::IndirectArgument& a,const std::uint8_t* data,bool reset){
                                D3D12State::Command next;const auto word=[&](unsigned at){std::uint32_t n=0;if(!reset)std::memcpy(&n,data+at,4);return n;};
                                if(a.type<=2){if(reset)return;next.op=a.type==0?"DrawInstanced":a.type==1?"DrawIndexedInstanced":"Dispatch";for(unsigned at=0;at<a.size;at+=4)next.values.push_back(word(at));}
                                else if(a.type==3 || a.type==4){next.op=a.type==3?"IASetVertexBuffers":"IASetIndexBuffer";next.values={a.index,1,0};next.data.resize(16);if(!reset)std::memcpy(next.data.data(),data,16);}
                                else if(a.type==5){next.op=prefix+"32BitConstants";next.values={a.index,a.count,0,a.offset};next.data.resize(a.size);if(!reset)std::memcpy(next.data.data(),data,a.size);}
                                else{next.op=prefix+(a.type==6?"ConstantBufferView":a.type==7?"ShaderResourceView":"UnorderedAccessView");std::uint64_t address=0;if(!reset)std::memcpy(&address,data,8);next.values={a.index,address};}
                                expanded.push_back(std::move(next));
                            };
                            for(std::uint32_t i=0;i<count;++i){std::size_t offset=std::size_t{i}*signature.stride;for(const auto& a:signature.arguments){decode(a,data.data()+offset,false);offset+=a.size;}}
                            // 契約要求被簽章修改的 IA／根參數在呼叫後歸零，未涉及的狀態維持原值。
                            for(const auto& a:signature.arguments)decode(a,nullptr,true);
                            for(auto i=expanded.rbegin();i!=expanded.rend();++i)pending.push_front(std::move(*i));
                        }
                        else if(c.op=="SetPipelineState")pipeline=v[0];
                        else if(ends("RootSignature"))roots.root=v[0];
                        else if(ends("RootDescriptorTable"))roots.tables[static_cast<std::uint32_t>(v[0])]=v[1];
                        else if(ends("RootConstantBufferView") || ends("RootShaderResourceView") || ends("RootUnorderedAccessView"))roots.views[static_cast<std::uint32_t>(v[0])]=v[1];
                        else if(ends("Root32BitConstant")){auto& data=roots.constants[static_cast<std::uint32_t>(v[0])];const auto offset=static_cast<std::uint32_t>(v[2])*4;if(data.size()<offset+4)data.resize(offset+4);const auto value=static_cast<std::uint32_t>(v[1]);std::memcpy(data.data()+offset,&value,4);}
                        else if(ends("Root32BitConstants")){auto& data=roots.constants[static_cast<std::uint32_t>(v[0])];const auto offset=static_cast<std::uint32_t>(v[3])*4;if(data.size()<offset+c.data.size())data.resize(offset+c.data.size());std::memcpy(data.data()+offset,c.data.data(),c.data.size());}
                        else if(c.op=="IASetPrimitiveTopology")draw.topology=static_cast<std::uint32_t>(v[0]);
                        else if(c.op=="IASetVertexBuffers"){
                            for(std::size_t i=0;i*16<c.data.size() && v[0]+i<draw.vertices.size();++i){
                                std::uint64_t location;std::uint32_t stride;std::memcpy(&location,c.data.data()+i*16,8);std::memcpy(&stride,c.data.data()+i*16+12,4);
                                const auto [object,offset]=resource_at(location);
                                draw.vertices[v[0]+i]=object?GraphicsVertexBuffer{gpu_resource(object),stride,static_cast<std::uint32_t>(offset)}:GraphicsVertexBuffer{};
                            }
                        }
                        else if(c.op=="IASetIndexBuffer"){
                            if(c.data.size()<16){draw.indices.reset();continue;}
                            std::uint64_t location;std::uint32_t format;std::memcpy(&location,c.data.data(),8);std::memcpy(&format,c.data.data()+12,4);
                            const auto [object,offset]=resource_at(location);draw.indices=object?gpu_resource(object):nullptr;draw.index_offset=static_cast<std::uint32_t>(offset);draw.index_format=format;
                        }
                        else if(c.op=="RSSetViewports"){draw.viewports.clear();for(std::size_t i=0;i+24<=c.data.size();i+=24){std::array<float,6> viewport;std::memcpy(viewport.data(),c.data.data()+i,24);draw.viewports.push_back(viewport);}}
                        else if(c.op=="RSSetScissorRects"){draw.scissors.clear();for(std::size_t i=0;i+16<=c.data.size();i+=16){std::array<std::int32_t,4> rect;std::memcpy(rect.data(),c.data.data()+i,16);draw.scissors.push_back(rect);}}
                        else if(c.op=="OMSetBlendFactor")std::memcpy(draw.blend_factor.data(),c.data.data(),16);
                        else if(c.op=="OMSetStencilRef")draw.stencil_reference=static_cast<std::uint32_t>(v[0]);
                        else if(c.op=="OMSetRenderTargets"){
                            target_count=c.data[0];for(auto& target:draw.targets)target={};
                            for(std::uint32_t i=0;i<target_count;++i)draw.targets[i]=c.views[i];
                            draw.depth=c.data[1]?c.views[target_count]:GraphicsView{};
                        }
                        else if(c.op=="ClearUnorderedAccessViewUint" || c.op=="ClearUnorderedAccessViewFloat"){
                            auto view=c.views.at(0);view.resource=gpu_resource(v[2]);std::array<std::uint32_t,4> values;std::memcpy(values.data(),c.data.data(),16);
                            const bool floating=c.op.ends_with("Float");const auto& desc=view.resource->desc;
                            if(!v[4]){
                                if(!native.graphics_clear_unordered(view,values,floating))throw std::invalid_argument("UAV 清除值與格式不相容");
                            }else{
                                // 共用 Metal 清除與複製：矩形只影響指定區域，其餘像素保持原值。
                                GraphicsView cleared=view;
                                if(desc.dimension!=1){cleared.resource=native.graphics_resource(desc);if(!native.graphics_clear_unordered(cleared,values,floating))throw std::invalid_argument("UAV 清除格式不相容");}
                                const auto element=std::max(1U,graphics_format_bytes(view.format?view.format:desc.format));
                                const auto width=desc.dimension==1?view.count/element:std::max(1U,desc.width>>view.level),height=desc.dimension==1?1U:std::max(1U,desc.height>>view.level);
                                for(std::size_t at=16;at<c.data.size();at+=16){
                                    std::array<std::int32_t,4> rect;std::memcpy(rect.data(),c.data.data()+at,16);
                                    const auto x=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[0],0,width)),y=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[1],0,height));
                                    const auto right=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[2],0,width)),bottom=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[3],0,height));
                                    if(right<=x || bottom<=y)continue;
                                    if(desc.dimension==1){auto region=view;region.first+=x*element;region.count=(right-x)*element;if(!native.graphics_clear_unordered(region,values,floating))throw std::invalid_argument("UAV 清除格式不相容");}
                                    else if(desc.dimension==4)native.graphics_copy(view,cleared,{x,y,view.slice},{x,y,view.slice,right,bottom,view.slice+view.slices});
                                    else for(std::uint32_t slice=0;slice<view.slices;++slice){auto to=view,from=cleared;to.slice+=slice;from.slice+=slice;native.graphics_copy(to,from,{x,y,0},{x,y,0,right,bottom,1});}
                                }
                            }
                            written.insert(v[2]);
                        }
                        else if(c.op=="ClearRenderTargetView"){std::array<float,4> color;std::memcpy(color.data(),c.data.data(),16);if(c.views[0].resource)native.graphics_clear(c.views[0],color,0);}
                        else if(c.op=="ClearDepthStencilView"){
                            float depth;std::memcpy(&depth,c.data.data(),4);const std::array<float,4> values{depth,static_cast<float>(v[3]&0xffU),0,0};
                            if(c.views[0].resource)native.graphics_clear(c.views[0],values,static_cast<std::uint32_t>(v[1]));
                        }
                        else if(c.op=="DrawInstanced" || c.op=="DrawIndexedInstanced"){
                            const auto p=bridge.pipelines.find(pipeline);if(p==bridge.pipelines.end() || p->second.compute)throw std::invalid_argument("Draw 缺少繪圖管線");
                            const auto& signature=signature_of(graphics_roots);GraphicsDraw d=draw;
                            d.stages[0].shader=p->second.shader;bind(d.stages[0],graphics_roots,signature);
                            if(p->second.pixel){d.stages[1].shader=p->second.pixel;bind(d.stages[1],graphics_roots,signature);}
                            d.layout=p->second.layout;d.blend=p->second.blend;d.depth_state=p->second.depth;d.raster=p->second.raster;d.sample_mask=p->second.sample_mask;
                            const bool indexed=c.op=="DrawIndexedInstanced";d.indexed=indexed;d.count=static_cast<std::uint32_t>(v[0]);d.instances=static_cast<std::uint32_t>(v[1]);d.start=static_cast<std::uint32_t>(v[2]);
                            if(indexed){d.base_vertex=static_cast<std::int32_t>(static_cast<std::uint32_t>(v[3]));d.start_instance=static_cast<std::uint32_t>(v[4]);}else d.start_instance=static_cast<std::uint32_t>(v[3]);
                            if(occlusion_heap){
                                auto& occlusion=bridge.query_heaps.at(occlusion_heap).occlusion[occlusion_index];
                                if(occlusion.slots>=occlusion_slots)throw std::invalid_argument("單一遮蔽查詢內的繪製次數超過上限");
                                d.visibility=occlusion.buffer;d.visibility_offset=occlusion.slots++*8;
                            }
                            native.graphics_draw(d);
                        }
                        else if(c.op=="BeginQuery" || c.op=="EndQuery"){
                            const auto heap=bridge.query_heaps.find(v[0]);const auto type=static_cast<std::uint32_t>(v[1]),index=static_cast<std::uint32_t>(v[2]);
                            if(heap==bridge.query_heaps.end() || index>=heap->second.count)throw std::invalid_argument("查詢堆積或索引無效");
                            if(type==2){
                                // TIMESTAMP：取樣當下 GPU 時戳（與 GetTimestampFrequency 同一時基）。
                                std::uint64_t ticks=0;
                                if(native.graphics_timestamp_supported())ticks=native.graphics_timestamp().ticks;
                                else ticks=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                                heap->second.timestamps[index]=ticks;
                            }else if(c.op=="BeginQuery"){
                                auto& occlusion=heap->second.occlusion[index];
                                GraphicsResourceDesc d;d.dimension=1;d.width=occlusion_slots*8;d.bind=0x80;occlusion.buffer=native.graphics_resource(d);
                                native.graphics_write(GraphicsView{occlusion.buffer},std::vector<std::uint8_t>(d.width),d.width,d.width);occlusion.slots=0;
                                occlusion_heap=v[0];occlusion_index=index;
                            }else if(occlusion_heap==v[0] && occlusion_index==index)occlusion_heap=0;
                        }
                        else if(c.op=="ResolveQueryData"){
                            // (heap, type, start, count, destination, offset)：每個結果 8 bytes。
                            const auto heap=bridge.query_heaps.find(v[0]);const auto type=static_cast<std::uint32_t>(v[1]),start=static_cast<std::uint32_t>(v[2]),count=static_cast<std::uint32_t>(v[3]);
                            if(heap==bridge.query_heaps.end() || std::uint64_t{start}+count>heap->second.count)throw std::invalid_argument("ResolveQueryData 範圍無效");
                            std::vector<std::uint8_t> data(std::size_t{count}*8);
                            for(std::uint32_t i=0;i<count;++i){
                                std::uint64_t value=0;
                                if(type==2)value=heap->second.timestamps[start+i];
                                else if(const auto& occlusion=heap->second.occlusion[start+i];occlusion.buffer){
                                    const auto samples=native.graphics_read(GraphicsView{occlusion.buffer});
                                    for(std::uint32_t slot=0;slot<occlusion.slots;++slot){std::uint64_t part;std::memcpy(&part,samples.data()+slot*8,8);value+=part;}
                                    if(type==1)value=value?1:0;
                                }
                                std::memcpy(data.data()+i*8,&value,8);
                            }
                            auto& destination=bridge.resources.at(v[4]);
                            if(destination.cpu)memory.write(GuestAddress{destination.cpu+v[5]},data);
                            else native.graphics_write_region(GraphicsView{destination.gpu},data,static_cast<std::uint32_t>(data.size()),static_cast<std::uint32_t>(data.size()),{static_cast<std::uint32_t>(v[5]),0,0,static_cast<std::uint32_t>(v[5]+data.size()),1,1});
                        }
                        else if(c.op=="Callback"){
                            // 建立在 D3D12 上的 API（DirectML）：以同一共用後端與資源對應執行；寫入的資源視同 UAV 寫入。
                            found_list->second.callbacks.at(static_cast<std::size_t>(v[0]))(native,[&](std::uint64_t object)->std::shared_ptr<GraphicsResource>{
                                if(!bridge.resources.contains(object))return {};written.insert(object);return gpu_resource(object);});
                        }
                        else if(c.op=="Dispatch"){
                            const auto p=bridge.pipelines.find(pipeline);if(p==bridge.pipelines.end() || !p->second.compute)throw std::invalid_argument("Dispatch 缺少運算管線");
                            GraphicsStage stage;stage.shader=p->second.shader;bind(stage,compute_roots,signature_of(compute_roots));
                            native.graphics_dispatch(stage,{static_cast<std::uint32_t>(v[0]),static_cast<std::uint32_t>(v[1]),static_cast<std::uint32_t>(v[2])});
                        }
                        else if(c.op=="CopyBufferRegion" || c.op=="CopyResource"){
                            const bool whole=c.op=="CopyResource";const auto dst=v[0],src=whole?v[1]:v[2];
                            auto& d=bridge.resources.at(dst);auto& s=bridge.resources.at(src);
                            const auto dst_offset=whole?0:v[1],src_offset=whole?0:v[3],size=whole?std::min(d.width,s.width):v[4];
                            if(d.dimension!=1 || s.dimension!=1){
                                if(!whole || !d.gpu || !s.gpu)throw std::invalid_argument("CopyResource 紋理需兩端皆為 GPU 資源");
                                for(std::uint32_t level=0;level<s.levels;++level)for(std::uint32_t layer=0;layer<(s.dimension==4?1:s.depth);++layer){
                                    GraphicsView from{s.gpu};from.level=level;from.slice=layer;GraphicsView to{d.gpu};to.level=level;to.slice=layer;
                                    native.graphics_copy(to,from,{0,0,0},{0,0,0,std::max(1U,static_cast<std::uint32_t>(s.width>>level)),std::max(1U,s.height>>level),s.dimension==4?std::max(1U,s.depth>>level):1U});
                                }
                                continue;
                            }
                            if(s.cpu && d.cpu){const auto data=bytes(s.cpu+src_offset,static_cast<std::size_t>(size));memory.write(GuestAddress{d.cpu+dst_offset},data);continue;}
                            const auto data=s.cpu?bytes(s.cpu+src_offset,static_cast<std::size_t>(size)):[&]{auto all=native.graphics_read(GraphicsView{s.gpu});return std::vector<std::uint8_t>(all.begin()+static_cast<std::ptrdiff_t>(src_offset),all.begin()+static_cast<std::ptrdiff_t>(src_offset+size));}();
                            if(d.cpu){memory.write(GuestAddress{d.cpu+dst_offset},data);continue;}
                            native.graphics_write_region(GraphicsView{d.gpu},data,static_cast<std::uint32_t>(size),static_cast<std::uint32_t>(size),{static_cast<std::uint32_t>(dst_offset),0,0,static_cast<std::uint32_t>(dst_offset+size),1,1});
                        }
                        else if(c.op=="CopyTextureRegion"){
                            // 足跡（緩衝）↔ 紋理子資源；兩端皆為紋理時以 GPU 複製。
                            const auto location=[&](std::size_t at){std::uint64_t resource;std::memcpy(&resource,c.data.data()+at,8);std::uint32_t type,sub;std::memcpy(&type,c.data.data()+at+8,4);std::memcpy(&sub,c.data.data()+at+16,4);return std::tuple{resource,type,sub};};
                            const auto [dst,dst_type,dst_sub]=location(0);const auto [src,src_type,src_sub]=location(48);
                            const auto footprint=[&](std::size_t at){std::uint64_t offset;std::memcpy(&offset,c.data.data()+at+16,8);std::array<std::uint32_t,5> f;std::memcpy(f.data(),c.data.data()+at+24,20);return std::pair{offset,f};};
                            auto& d=bridge.resources.at(dst);auto& s=bridge.resources.at(src);
                            const auto x=static_cast<std::uint32_t>(v[1]),y=static_cast<std::uint32_t>(v[2]),z=static_cast<std::uint32_t>(v[3]);
                            if(dst_type==0 && src_type==1){
                                const auto [offset,f]=footprint(48);const auto level=dst_sub%d.levels,layer=dst_sub/d.levels;
                                const auto pitch=graphics_format_pitch(f[0],f[1],f[2]);std::vector<std::uint8_t> data(std::size_t{pitch[0]}*pitch[1]*f[3]);
                                for(std::uint32_t slice=0;slice<f[3];++slice)for(std::uint32_t row=0;row<pitch[1];++row){
                                    const auto from=offset+(std::uint64_t{slice}*pitch[1]+row)*f[4];
                                    const auto line=s.cpu?bytes(s.cpu+from,pitch[0]):std::vector<std::uint8_t>(pitch[0]);
                                    std::memcpy(data.data()+(std::size_t{slice}*pitch[1]+row)*pitch[0],line.data(),pitch[0]);
                                }
                                GraphicsView view{d.gpu};view.level=level;view.slice=layer;
                                native.graphics_write_region(view,data,pitch[0],pitch[0]*pitch[1],{x,y,z,x+f[1],y+f[2],z+f[3]});
                            }else if(dst_type==1 && src_type==0){
                                const auto [offset,f]=footprint(0);const auto level=src_sub%s.levels,layer=src_sub/s.levels;
                                GraphicsView view{s.gpu};view.level=level;view.slice=layer;const auto data=native.graphics_read(view);
                                const auto w=std::max(1U,static_cast<std::uint32_t>(s.width>>level)),h=std::max(1U,s.height>>level);const auto pitch=graphics_format_pitch(s.format,w,h);
                                for(std::uint32_t row=0;row<pitch[1] && std::size_t{row+1}*pitch[0]<=data.size();++row)if(d.cpu)memory.write(GuestAddress{d.cpu+offset+std::uint64_t{row}*f[4]},std::span<const std::uint8_t>(data).subspan(std::size_t{row}*pitch[0],pitch[0]));
                            }else if(dst_type==0 && src_type==0){
                                GraphicsView to{d.gpu};to.level=dst_sub%d.levels;to.slice=dst_sub/d.levels;GraphicsView from{s.gpu};from.level=src_sub%s.levels;from.slice=src_sub/s.levels;
                                std::array<std::uint32_t,6> box{0,0,0,std::max(1U,static_cast<std::uint32_t>(s.width>>from.level)),std::max(1U,s.height>>from.level),1};
                                if(c.data.size()>=120)std::memcpy(box.data(),c.data.data()+96,24);
                                native.graphics_copy(to,from,{x,y,z},box);
                            }
                        }
                        else if(c.op=="ResourceBarrier" || c.op=="SetDescriptorHeaps" || c.op=="SetName" || c.op=="SetMarker" || c.op=="BeginEvent" || c.op=="EndEvent"){}
                        else report("List."+c.op);
                    }
                }
                native.graphics_flush(true);
                for(const auto object:written)if(bridge.resources.at(object).reads_back())read_back(object);
            }catch(const std::exception& error){
                runtime_.module_events.push_back(std::string("d3d12_note=ExecuteCommandLists 失敗：")+error.what());
            }
            return finish(0);
        }
        report(symbol);return finish(not_implemented);
    }
    report(symbol);return finish(not_implemented);
}
}
