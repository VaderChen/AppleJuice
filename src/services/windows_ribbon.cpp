#include "x64arm/windows_services.hpp"
#include <array>
#include <map>

namespace x64arm {
// Windows Ribbon 框架（UIRibbon.dll，CLSID_UIRibbonFramework）。
// 框架物件與 IUIApplication 的生命週期、初始化與查詢介面依 UIRibbon.h 契約；
// Ribbon 標記（UIFILE 資源）尚無原生呈現，LoadUI 回報 E_NOTIMPL，應用程式維持傳統選單，
// GetView 在沒有已載入的 view 時失敗，GetRibbonHeight 類呼叫得到高度 0。
// 參考：https://learn.microsoft.com/windows/win32/api/uiribbon/nn-uiribbon-iuiframework
struct RibbonState {
    enum class Kind {Framework,Factory};
    struct Object {Kind kind{Kind::Framework};unsigned references{1};std::uint64_t window{},application{};};
    std::map<std::uint64_t,Object> objects;
    std::uint64_t framework_table{},factory_table{};
    unsigned server_locks{};bool reported_markup{};
};
namespace {
constexpr std::uint32_t failure=0x80004005U,no_interface=0x80004002U,pointer_error=0x80004003U,not_implemented=0x80004001U,
    unexpected=0x8000ffffU,invalid=0x80070057U,no_memory=0x8007000eU,no_aggregation=0x80040110U,class_not_registered=0x80040154U,
    not_initialized=0x800401f0U;
using Guid=std::array<std::uint8_t,16>;
constexpr Guid ribbon_clsid{0xfa,0x49,0x67,0x92,0x15,0x26,0x87,0x49,0x88,0x45,0xc3,0x3e,0x65,0xf2,0xb9,0x57};
constexpr Guid iid_unknown{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46};
constexpr Guid iid_factory{0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46};
constexpr Guid iid_framework{0x5d,0x38,0xf0,0xf4,0x72,0x68,0xa8,0x43,0xad,0x09,0x4c,0x33,0x9c,0xb3,0xf5,0xc5};
// IUIFramework 完整 vtable 順序；IClassFactory 共用 IUnknown 三格。
constexpr WindowsServiceDescriptor framework_methods[]{
    {"internal-ribbon","QueryInterface",3},{"internal-ribbon","AddRef",1},{"internal-ribbon","Release",1},
    {"internal-ribbon","Initialize",3},{"internal-ribbon","Destroy",1},{"internal-ribbon","LoadUI",3},{"internal-ribbon","GetView",4},
    {"internal-ribbon","GetUICommandProperty",4},{"internal-ribbon","SetUICommandProperty",4},{"internal-ribbon","InvalidateUICommand",4},
    {"internal-ribbon","FlushPendingInvalidations",1},{"internal-ribbon","SetModes",2},
};
constexpr WindowsServiceDescriptor factory_methods[]{
    {"internal-ribbon","QueryInterface",3},{"internal-ribbon","AddRef",1},{"internal-ribbon","Release",1},
    {"internal-ribbon","CreateInstance",4},{"internal-ribbon","LockServer",2},
};
Guid read_guid(const GuestMemory& memory,std::uint64_t address){Guid value{};memory.read(GuestAddress{address},value);return value;}
}
std::span<const WindowsServiceDescriptor> ribbon_service_descriptors(){
    static const auto descriptors=[]{
        std::vector<WindowsServiceDescriptor> out(std::begin(framework_methods),std::end(framework_methods));
        out.push_back(factory_methods[3]);out.push_back(factory_methods[4]);return out;
    }();
    return descriptors;
}
bool WindowsServices::begin_ribbon_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    if(!arg(0) || read_guid(memory,arg(0))!=ribbon_clsid)return false;
    (void)invoke_ribbon(state,memory,service);return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_ribbon(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using Kind=RibbonState::Kind;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(!ribbon_)ribbon_=std::make_shared<RibbonState>();
    else if(ribbon_.use_count()>1)ribbon_=std::make_shared<RibbonState>(*ribbon_);
    auto& bridge=*ribbon_;
    // 客體 COM 方法以真正的 vtable 呼叫；結果由 Continuation::Return 帶回原 API。
    const auto guest_call=[&](std::uint64_t object,unsigned slot,std::uint32_t value){
        const auto target=memory.load(GuestAddress{memory.load(GuestAddress{object},bits)+slot*unit},bits);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        start_callbacks(state,memory,{{target,{object}}},Continuation::Return,value);return std::optional<std::uint32_t>{};
    };
    const auto create=[&](Kind kind)->std::uint64_t{
        auto& table=kind==Kind::Framework?bridge.framework_table:bridge.factory_table;
        if(!table){
            const std::span<const WindowsServiceDescriptor> methods=kind==Kind::Framework?std::span<const WindowsServiceDescriptor>(framework_methods):std::span<const WindowsServiceDescriptor>(factory_methods);
            const auto address=runtime_allocate(memory,{},methods.size()*unit);
            for(std::size_t i=0;i<methods.size();++i)memory.store(GuestAddress{address+i*unit},bits,resolve(methods[i].family,methods[i].symbol).value);
            table=address;
        }
        const auto pointer=allocate_heap(memory,unit);if(!pointer)return 0;
        memory.store(GuestAddress{pointer},bits,table);bridge.objects.emplace(pointer,RibbonState::Object{kind});return pointer;
    };
    // CoCreateInstance／IClassFactory::CreateInstance 共用：只建立 in-process STA 框架。
    const auto instantiate=[&](std::uint64_t outer,std::uint64_t riid,std::uint64_t out){
        if(!out)return pointer_error;
        memory.store(GuestAddress{out},bits,0);
        if(!riid)return invalid;
        if(outer)return no_aggregation;
        const auto iid=read_guid(memory,riid);
        if(iid!=iid_unknown && iid!=iid_framework)return no_interface;
        const auto object=create(Kind::Framework);if(!object)return no_memory;
        memory.store(GuestAddress{out},bits,object);return 0U;
    };
    if(name=="CoCreateInstance" || name=="CoGetClassObject"){
        const bool instance=name=="CoCreateInstance";const auto out=arg(4);
        if(!out)return finish(pointer_error);
        memory.store(GuestAddress{out},bits,0);
        const auto apartment=com_apartment(threads_.current);if(!apartment)return finish(not_initialized);
        const auto context=static_cast<std::uint32_t>(arg(instance?2:1));
        if(!(context&1U))return finish(class_not_registered);
        if(!instance && arg(2))return finish(not_implemented);
        // UIRibbon 登錄為 Apartment；MTA 呼叫端需要 host STA 與 proxy，與一般 in-proc 啟用相同地拒絕。
        if(!*apartment)return finish(not_implemented);
        if(instance)return finish(instantiate(arg(1),arg(3),out));
        if(!arg(3))return finish(invalid);
        const auto iid=read_guid(memory,arg(3));
        if(iid!=iid_unknown && iid!=iid_factory)return finish(no_interface);
        const auto factory=create(Kind::Factory);if(!factory)return finish(no_memory);
        memory.store(GuestAddress{out},bits,factory);return finish(0);
    }
    const auto self=arg(0);const auto found=bridge.objects.find(self);
    if(found==bridge.objects.end())return finish(unexpected);
    auto& object=found->second;
    if(name=="QueryInterface"){
        if(!arg(2))return finish(pointer_error);
        memory.store(GuestAddress{arg(2)},bits,0);
        if(!arg(1))return finish(invalid);
        const auto iid=read_guid(memory,arg(1));
        if(iid!=iid_unknown && iid!=(object.kind==Kind::Framework?iid_framework:iid_factory))return finish(no_interface);
        ++object.references;memory.store(GuestAddress{arg(2)},bits,self);return finish(0);
    }
    if(name=="AddRef"){++object.references;return finish(object.references);}
    if(name=="Release"){
        const auto remaining=--object.references;if(remaining)return finish(remaining);
        const auto application=object.application;
        bridge.objects.erase(found);(void)free_heap(memory,self,state);
        // 最後一次 Release 等同 Destroy：釋放 Initialize 持有的 IUIApplication。
        if(application)return guest_call(application,2,0);
        return finish(0);
    }
    if(object.kind==Kind::Factory){
        if(name=="CreateInstance")return finish(instantiate(arg(1),arg(2),arg(3)));
        if(name=="LockServer"){
            if(static_cast<std::uint32_t>(arg(1)))++bridge.server_locks;else if(bridge.server_locks)--bridge.server_locks;
            return finish(0);
        }
        return finish(not_implemented);
    }
    if(name=="Initialize"){
        if(!arg(1) || !arg(2))return finish(pointer_error);
        if(object.application)return finish(unexpected);
        if(!gui_.windows.contains(arg(1)))return finish(invalid);
        object.window=arg(1);object.application=arg(2);
        return guest_call(object.application,1,0);
    }
    if(name=="Destroy"){
        if(!object.application)return finish(unexpected);
        const auto application=object.application;object.application=0;object.window=0;
        return guest_call(application,2,0);
    }
    if(!object.application)return finish(unexpected);
    if(name=="LoadUI"){
        if(!arg(1) || !arg(2))return finish(pointer_error);
        if(!bridge.reported_markup){
            bridge.reported_markup=true;
            runtime_.module_events.push_back("ribbon_note=UIRibbon LoadUI 尚未支援原生 Ribbon 標記呈現；應用程式保留傳統選單");
        }
        return finish(not_implemented);
    }
    if(name=="GetView"){
        if(!arg(3))return finish(pointer_error);
        memory.store(GuestAddress{arg(3)},bits,0);
        if(!arg(2))return finish(invalid);
        return finish(failure); // 尚未載入任何 Ribbon／ContextPopup view。
    }
    if(name=="GetUICommandProperty"){
        if(!arg(2) || !arg(3))return finish(pointer_error);
        memory.fill(GuestAddress{arg(3)},0,x86_?16U:24U); // PROPVARIANT 設為 VT_EMPTY。
        return finish(failure);
    }
    if(name=="SetUICommandProperty"){
        if(!arg(2) || !arg(3))return finish(pointer_error);
        return finish(failure);
    }
    // 未載入標記時沒有任何 command 需要更新；失效請求與模式設定不產生作用。
    if(name=="InvalidateUICommand" || name=="FlushPendingInvalidations")return finish(0);
    if(name=="SetModes")return finish(failure);
    return finish(not_implemented);
}
}
