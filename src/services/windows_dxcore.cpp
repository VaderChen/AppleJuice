#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/gpu_memory.hpp"
#include "windows_display_identity.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <vector>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
enum class Kind {Factory,List,Adapter};
constexpr Guid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid factory_iid{0x45,0x59,0xee,0x78,0x6e,0xc3,0x13,0x4b,0xa6,0x69,0,0x5d,0xd1,0x1c,0x0f,0x06};
constexpr Guid list_iid{0x76,0x77,0x6c,0x52,0xe9,0x40,0x9b,0x45,0xb7,0x11,0xf3,0x2a,0xd7,0x6d,0xfc,0x28};
constexpr Guid adapter_iid{0x7f,0x4c,0xdb,0xf0,0x5a,0xfe,0xa2,0x42,0xbd,0x62,0xf2,0xa6,0xcf,0x6f,0xc8,0x3e};
constexpr Guid graphics11{0x6b,0x86,0x47,0x8c,0x83,0x75,0x0d,0x45,0xf0,0xf0,0x6b,0xad,0xa8,0x95,0xaf,0x4b};
constexpr Guid graphics12{0x4d,0xce,0x9e,0x0c,0x6e,0x2f,0x01,0x4f,0x8c,0x96,0xe8,0x9e,0x33,0x1b,0x47,0xb1};
constexpr Guid compute12{0x00,0x28,0x8e,0x24,0x93,0xa7,0x24,0x47,0xab,0xaa,0x23,0xa6,0xde,0x1b,0xe0,0x90};
constexpr Guid hardware_gpu{0x19,0xb2,0x9e,0xb6,0xed,0x3d,0x64,0x44,0x97,0x9f,0xa0,0x0b,0xd4,0x68,0x70,0x06};
constexpr std::uint32_t invalid=0x80070057,invalid_call=0x887a0001,no_interface=0x80004002,
    pointer_error=0x80004003,unsupported=0x887a0004,not_found=0x887a0002;
// 依 Microsoft DirectX-Headers dxcore_interface.h 的 ABI 排列，不含 This 的參數由下方 descriptor 定義。
constexpr std::string_view factory_methods[]{"QueryInterface","AddRef","Release","CreateAdapterList","GetAdapterByLuid","IsNotificationTypeSupported","RegisterEventNotification","UnregisterEventNotification"};
constexpr std::string_view list_methods[]{"QueryInterface","AddRef","Release","GetAdapter","GetAdapterCount","IsStale","GetFactory","Sort","IsAdapterPreferenceSupported"};
constexpr std::string_view adapter_methods[]{"QueryInterface","AddRef","Release","IsValid","IsAttributeSupported","IsPropertySupported","GetProperty","GetPropertySize","IsQueryStateSupported","QueryState","IsSetStateSupported","SetState","GetFactory"};
bool supports(Kind kind,const Guid& iid){return iid==unknown || iid==(kind==Kind::Factory?factory_iid:kind==Kind::List?list_iid:adapter_iid);}
}
struct DXCoreState {
    struct Object {Kind kind{};std::uint32_t references{1};std::uint64_t factory{};std::vector<std::uint64_t> adapters;};
    std::map<std::uint64_t,Object> objects;
    std::map<Kind,std::uint64_t> tables;
    std::uint64_t factory{},adapter{}; // 弱快取；最後一個 COM 參照釋放時一併清除。
    bool probed{};
    bool available{};
};
std::span<const WindowsServiceDescriptor> dxcore_export_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"dxcore.dll","DXCoreCreateAdapterFactory",2}};return table;
}
std::span<const WindowsServiceDescriptor> dxcore_object_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"internal-dxcore","QueryInterface",3},{"internal-dxcore","AddRef",1},{"internal-dxcore","Release",1},
        {"internal-dxcore","CreateAdapterList",5},{"internal-dxcore","GetAdapterByLuid",4},
        {"internal-dxcore","IsNotificationTypeSupported",2},{"internal-dxcore","RegisterEventNotification",6},{"internal-dxcore","UnregisterEventNotification",2},
        {"internal-dxcore","GetAdapter",4},{"internal-dxcore","GetAdapterCount",1},{"internal-dxcore","IsStale",1},
        {"internal-dxcore","GetFactory",3},{"internal-dxcore","Sort",3},{"internal-dxcore","IsAdapterPreferenceSupported",2},
        {"internal-dxcore","IsValid",1},{"internal-dxcore","IsAttributeSupported",2},{"internal-dxcore","IsPropertySupported",2},
        {"internal-dxcore","GetProperty",4},{"internal-dxcore","GetPropertySize",3},
        {"internal-dxcore","IsQueryStateSupported",2},{"internal-dxcore","QueryState",6},
        {"internal-dxcore","IsSetStateSupported",2},{"internal-dxcore","SetState",6}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dxcore(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const std::uint8_t bits=x86_?32:64;const unsigned pointer_size=bits/8;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto guid=[&](std::uint64_t at){Guid value{};memory.read(GuestAddress{at},value);return value;};
    const auto store_pointer=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    if(!dxcore_)dxcore_=std::make_shared<DXCoreState>();auto& bridge=*dxcore_;
    const auto create=[&](Kind kind,std::uint64_t factory){
        auto& table=bridge.tables[kind];
        if(!table){
            const std::span<const std::string_view> methods=kind==Kind::Factory?std::span<const std::string_view>(factory_methods):kind==Kind::List?std::span<const std::string_view>(list_methods):std::span<const std::string_view>(adapter_methods);
            table=runtime_allocate(memory,{},methods.size()*pointer_size);
            for(std::size_t i=0;i<methods.size();++i)store_pointer(table+i*pointer_size,resolve("internal-dxcore",methods[i]).value);
        }
        DXCoreState::Object object;object.kind=kind;object.factory=factory;
        const auto pointer=allocate_heap(memory,pointer_size);if(!pointer)throw std::bad_alloc{};
        try{store_pointer(pointer,table);bridge.objects.emplace(pointer,std::move(object));}
        catch(...){(void)free_heap(memory,pointer,state);throw;}
        if(factory)++bridge.objects.at(factory).references;
        return pointer;
    };
    const auto query=[&](std::uint64_t self,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        if(!out)return pointer_error;store_pointer(out,0);
        if(!riid)return invalid;
        auto& object=bridge.objects.at(self);if(!supports(object.kind,guid(riid)))return no_interface;
        ++object.references;store_pointer(out,self);return 0;
    };
    std::function<std::uint32_t(std::uint64_t)> release=[&](std::uint64_t self){
        auto& object=bridge.objects.at(self);if(--object.references)return object.references;
        const auto factory=object.factory;const auto adapters=std::move(object.adapters);
        bridge.objects.erase(self);if(bridge.factory==self)bridge.factory=0;if(bridge.adapter==self)bridge.adapter=0;
        (void)free_heap(memory,self,state);
        for(const auto adapter:adapters)release(adapter);
        if(factory)release(factory);return std::uint32_t{0};
    };
    const auto has_attribute=[&](const Guid& attribute){
        return bridge.available && (attribute==graphics11 || attribute==hardware_gpu || (!x86_ && (attribute==graphics12 || attribute==compute12)));
    };
    const auto adapter=[&](){
        if(!bridge.adapter)bridge.adapter=create(Kind::Adapter,bridge.factory);
        else ++bridge.objects.at(bridge.adapter).references;
        return bridge.adapter;
    };
    try{
        if(symbol=="DXCoreCreateAdapterFactory"){
            if(!arg(1))return finish(pointer_error);store_pointer(arg(1),0);
            if(!arg(0))return finish(invalid);if(!supports(Kind::Factory,guid(arg(0))))return finish(no_interface);
            if(!bridge.probed){
                if(!runtime_.network.external_io)throw HostCallFault("原生硬體裝置不能在 compare 模式建立或使用");
                // native 只用來判斷 Metal 裝置是否可用，屬性皆取自 display_identity。單一加速器可由 IOKit 確認時
                // 不建立 Metal 裝置（首次約 60 ms）；無法確認才建立裝置判斷。
                bridge.available=metal_opengl_identity().has_value();
                if(!bridge.available)try{bridge.available=bool(make_metal_opengl(nullptr,1,1));}catch(const std::exception&){bridge.available=false;}
                bridge.probed=true;
            }
            if(!bridge.factory)bridge.factory=create(Kind::Factory,0);else ++bridge.objects.at(bridge.factory).references;
            store_pointer(arg(1),bridge.factory);return finish(0);
        }
        const auto at=bridge.objects.find(arg(0));if(at==bridge.objects.end())return finish(invalid);
        auto& object=at->second;
        if(symbol=="QueryInterface")return finish(query(arg(0),arg(1),arg(2)));
        if(symbol=="AddRef")return finish(++object.references);
        if(symbol=="Release")return finish(release(arg(0)));
        if(symbol=="GetFactory")return finish(object.factory?query(object.factory,arg(1),arg(2)):invalid);
        if(object.kind==Kind::Factory){
            if(symbol=="CreateAdapterList"){
                if(!arg(4))return finish(pointer_error);store_pointer(arg(4),0);
                if(!u32(1) || !arg(2) || !arg(3))return finish(invalid);
                if(!supports(Kind::List,guid(arg(3))))return finish(no_interface);
                memory.check(GuestAddress{arg(2)},std::uint64_t{u32(1)}*16,Access::Read);
                bool match=true;for(std::uint32_t i=0;i<u32(1);++i)if(!has_attribute(guid(arg(2)+std::uint64_t{i}*16)))match=false;
                const auto list=create(Kind::List,arg(0));
                try{if(match){bridge.objects.at(list).adapters.reserve(1);bridge.objects.at(list).adapters.push_back(adapter());}}
                catch(...){release(list);throw;}
                store_pointer(arg(4),list);return finish(0);
            }
            if(symbol=="GetAdapterByLuid"){
                if(!arg(3))return finish(pointer_error);store_pointer(arg(3),0);
                if(!arg(1) || !arg(2))return finish(invalid);
                if(!bridge.available || memory.load(GuestAddress{arg(1)},64)!=display_identity::adapter_luid)return finish(not_found);
                if(!supports(Kind::Adapter,guid(arg(2))))return finish(no_interface);
                store_pointer(arg(3),adapter());return finish(0);
            }
            // 此虛擬配接器沒有熱插拔／驅動更新來源；不要宣告無法派送的通知能力。
            if(symbol=="IsNotificationTypeSupported")return finish(0);
            if(symbol=="RegisterEventNotification"){
                if(!arg(5))return finish(pointer_error);memory.store(GuestAddress{arg(5)},32,0);return finish(unsupported);
            }
            if(symbol=="UnregisterEventNotification")return finish(invalid);
        }
        if(object.kind==Kind::List){
            if(symbol=="GetAdapterCount")return finish(object.adapters.size());
            if(symbol=="IsStale")return finish(0); // 固定的 Metal 配接器集合。
            if(symbol=="GetAdapter"){
                if(!arg(3))return finish(pointer_error);store_pointer(arg(3),0);
                return finish(u32(1)<object.adapters.size()?query(object.adapters[u32(1)],arg(2),arg(3)):invalid);
            }
            if(symbol=="IsAdapterPreferenceSupported")return finish(u32(1)<=2);
            if(symbol=="Sort"){
                if(!u32(1) || !arg(2))return finish(invalid);
                memory.check(GuestAddress{arg(2)},std::uint64_t{u32(1)}*4,Access::Read);
                for(std::uint32_t i=0;i<u32(1);++i)if(memory.load(GuestAddress{arg(2)+std::uint64_t{i}*4},32)>2)return finish(invalid);
                return finish(0); // 零或一個硬體配接器，所有支援的排序均保持同一順序。
            }
        }
        if(object.kind==Kind::Adapter){
            if(symbol=="IsValid")return finish(bridge.available);
            if(symbol=="IsAttributeSupported")return finish(arg(1) && has_attribute(guid(arg(1))));
            if(symbol=="IsQueryStateSupported")return finish(u32(1)<=1);
            if(symbol=="IsSetStateSupported")return finish(u32(1)==1);
            if(symbol=="QueryState" || symbol=="SetState"){
                const bool set=symbol=="SetState";
                if(!arg(5))return finish(pointer_error);
                if(!set && u32(1)==0){
                    if(arg(2) || arg(3) || arg(4)<1)return finish(invalid);
                    memory.store(GuestAddress{arg(5)},8,0);return finish(0);
                }
                if(u32(1)!=1)return finish(unsupported);
                if(arg(2)!=8 || !arg(3) || arg(4)<(set?8U:32U))return finish(invalid);
                const auto node=memory.load(GuestAddress{arg(3)},32),segment=memory.load(GuestAddress{arg(3)+4},32);
                if(node || segment>1)return finish(invalid);
                if(set)return finish(set_gpu_memory_reservation(segment==0,memory.load(GuestAddress{arg(5)},64))?0:invalid);
                const auto budget=gpu_memory_budget(segment==0);
                const std::array<std::uint64_t,4> values{budget.budget,budget.usage,budget.available,budget.reservation};
                memory.check(GuestAddress{arg(5)},32,Access::Write);
                for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(5)+i*8},64,values[i]);
                return finish(0);
            }
            if(symbol=="IsPropertySupported" || symbol=="GetPropertySize" || symbol=="GetProperty"){
                std::vector<std::uint8_t> data;
                const auto number=[&](std::uint64_t value,unsigned size){data.resize(size);for(unsigned i=0;i<size;++i)data[i]=static_cast<std::uint8_t>(value>>(8*i));};
                switch(u32(1)){
                case 0:number(display_identity::adapter_luid,8);break;
                case 1:number(display_identity::version_number,8);break;
                case 2:data.assign(display_identity::adapter_name_utf8.begin(),display_identity::adapter_name_utf8.end());data.push_back(0);break;
                case 3:case 14:number(display_identity::vendor_id,4);data.resize(u32(1)==3?16:20);break;
                // 不冒充 WDDM 驅動，也不宣告未提供的搶占／引擎遙測屬性。
                case 7:case 8:number(0,8);break;
                case 9:number(gpu_memory_stats().limit,8);break;
                case 10:case 13:number(0,1);break;
                case 11:case 12:number(1,1);break;
                case 15:number(1,4);break;
                default:break;
                }
                if(symbol=="IsPropertySupported")return finish(!data.empty());
                if(!arg(symbol=="GetProperty"?3:2))return finish(pointer_error);
                if(data.empty())return finish(u32(1)>17?invalid_call:unsupported);
                if(symbol=="GetPropertySize")store_pointer(arg(2),data.size());
                else{
                    if(arg(2)<data.size())return finish(invalid);
                    memory.check(GuestAddress{arg(3)},arg(2),Access::Write);
                    // DXCore 契約要求把大於實際屬性的剩餘 buffer 也清零。
                    const std::array<std::uint8_t,256> zeros{};
                    for(std::uint64_t offset=data.size();offset<arg(2);){const auto count=std::min<std::uint64_t>(zeros.size(),arg(2)-offset);memory.write(GuestAddress{arg(3)+offset},std::span(zeros).first(count));offset+=count;}
                    memory.write(GuestAddress{arg(3)},data);
                }
                return finish(0);
            }
        }
        return finish(invalid_call);
    }catch(const std::bad_alloc&){return finish(0x8007000e);}
}
}
