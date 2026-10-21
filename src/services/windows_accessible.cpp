#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid accessible_iid{0xe0,0x36,0x87,0x61,0x3d,0x3c,0xcf,0x11,0x81,0x0c,0,0xaa,0,0x38,0x9b,0x71};
constexpr Guid dispatch_iid{0,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},unknown_iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,not_implemented=0x80004001U;
constexpr WindowsServiceDescriptor methods[]{
    {"internal-accessible","IAccessible.QueryInterface",3},{"internal-accessible","IAccessible.AddRef",1},{"internal-accessible","IAccessible.Release",1},
    {"internal-accessible","IAccessible.GetTypeInfoCount",2},{"internal-accessible","IAccessible.GetTypeInfo",4},{"internal-accessible","IAccessible.GetIDsOfNames",6},{"internal-accessible","IAccessible.Invoke",9},
    {"internal-accessible","IAccessible.get_accParent",2},{"internal-accessible","IAccessible.get_accChildCount",2},
    {"internal-accessible","IAccessible.get_accChild",3,0,2},{"internal-accessible","IAccessible.get_accName",3,0,2},{"internal-accessible","IAccessible.get_accValue",3,0,2},
    {"internal-accessible","IAccessible.get_accDescription",3,0,2},{"internal-accessible","IAccessible.get_accRole",3,0,2},{"internal-accessible","IAccessible.get_accState",3,0,2},
    {"internal-accessible","IAccessible.get_accHelp",3,0,2},{"internal-accessible","IAccessible.get_accHelpTopic",4,0,4},{"internal-accessible","IAccessible.get_accKeyboardShortcut",3,0,2},
    {"internal-accessible","IAccessible.get_accFocus",2},{"internal-accessible","IAccessible.get_accSelection",2},{"internal-accessible","IAccessible.get_accDefaultAction",3,0,2},
    {"internal-accessible","IAccessible.accSelect",3,0,4},{"internal-accessible","IAccessible.accLocation",6,0,32},{"internal-accessible","IAccessible.accNavigate",4,0,4},
    {"internal-accessible","IAccessible.accHitTest",4},{"internal-accessible","IAccessible.accDoDefaultAction",2,0,2},{"internal-accessible","IAccessible.put_accName",3,0,2},{"internal-accessible","IAccessible.put_accValue",3,0,2}
};
struct Object {std::uint64_t window{};std::uint32_t id{},references{1};};
constexpr Guid uia_simple_iid{0xd1,0x68,0xdd,0xd6,0xfd,0x86,0x32,0x43,0x86,0x66,0x9a,0xbe,0xde,0xa2,0xd2,0x4c};
constexpr WindowsServiceDescriptor uia_methods[]{
    {"internal-accessible","UIA.QueryInterface",3},{"internal-accessible","UIA.AddRef",1},{"internal-accessible","UIA.Release",1},
    {"internal-accessible","UIA.ProviderOptions",2},{"internal-accessible","UIA.GetPatternProvider",3},
    {"internal-accessible","UIA.GetPropertyValue",3},{"internal-accessible","UIA.HostRawElementProvider",2}
};
}
struct AccessibleState {
    struct Transfer {std::uint64_t object{},wparam{};};
    std::map<std::uint64_t,Object> objects;
    std::map<std::uint64_t,Transfer> transfers;
    std::uint64_t table{},next_transfer{0x20000000};
    std::map<std::uint64_t,Object> uia_objects;
    std::array<std::uint64_t,2> uia_reserved{};
    std::uint64_t uia_table{};
    struct UiaProvider {std::uint64_t window{},token{};};
    std::map<std::uint64_t,UiaProvider> uia_providers;
};
std::span<const WindowsServiceDescriptor> accessible_service_descriptors(){
    static const auto descriptors=[] {std::vector<WindowsServiceDescriptor> list{{"oleacc.dll","CreateStdAccessibleObject",4},{"oleacc.dll","AccessibleObjectFromWindow",4},{"oleacc.dll","AccessibleChildren",5},{"oleacc.dll","WindowFromAccessibleObject",2},{"oleacc.dll","LresultFromObject",3},{"oleacc.dll","ObjectFromLresult",4},
        {"uiautomationcore.dll","UiaGetReservedMixedAttributeValue",1},{"uiautomationcore.dll","UiaGetReservedNotSupportedValue",1},
        {"uiautomationcore.dll","UiaHostProviderFromHwnd",2},{"uiautomationcore.dll","UiaReturnRawElementProvider",4},
        {"uiautomationcore.dll","UiaRaiseAutomationEvent",2},{"uiautomationcore.dll","UiaRaiseAutomationPropertyChangedEvent",4,0,12},
        {"uiautomationcore.dll","UiaRaiseStructureChangedEvent",4},{"uiautomationcore.dll","UiaRaiseNotificationEvent",5},{"uiautomationcore.dll","UiaClientsAreListening",0}};
        list.insert(list.end(),std::begin(methods),std::end(methods));list.insert(list.end(),std::begin(uia_methods),std::end(uia_methods));return list;}();return descriptors;
}
std::uint64_t WindowsServices::create_accessible(GuestMemory& memory,std::uint64_t window,std::uint32_t id){
    if(!accessible_)accessible_=std::make_shared<AccessibleState>();auto& bridge=*accessible_;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(bridge.objects.size()>=65536)throw std::bad_alloc{};
    if(!bridge.table){bridge.table=runtime_allocate(memory,{},std::size(methods)*unit);for(unsigned i=0;i<std::size(methods);++i)memory.store(GuestAddress{bridge.table+i*unit},bits,resolve(methods[i].family,methods[i].symbol).value);}
    const auto at=allocate_heap(memory,unit);if(!at)throw std::bad_alloc{};memory.store(GuestAddress{at},bits,bridge.table);bridge.objects.emplace(at,Object{window,id,1});return at;
}
std::optional<std::uint32_t> WindowsServices::resume_accessible(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint64_t result){
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);auto& bridge=*accessible_;
    const auto read=[&](unsigned offset){return memory.load(GuestAddress{scratch+offset},64);};
    const auto operation=read(0),object=read(8),out=read(16),wparam=read(56);
    const auto done=[&](std::uint64_t status){(void)free_heap(memory,scratch,state);complete_call(state,memory,operation==1 && (status&0x80000000U)?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(status))):status);return std::optional<std::uint32_t>{};};
    const auto method=[&](std::uint64_t pointer,unsigned index){const auto table=memory.load(GuestAddress{pointer},bits),target=memory.load(GuestAddress{table+index*unit},bits);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);return target;};
    const auto release=[&](std::uint64_t pointer,std::uint64_t status){const auto target=method(pointer,2);(void)free_heap(memory,scratch,state);start_callbacks(state,memory,{{target,{pointer}}},Continuation::Return,operation==1 && (status&0x80000000U)?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(status))):status);return std::optional<std::uint32_t>{};};
    const auto next_child=[&](){
        const auto original=read(64),child=read(48)+read(24)+1;memory.store(GuestAddress{scratch+8},64,0);memory.store(GuestAddress{scratch},64,11);
        memory.write(GuestAddress{scratch+80},std::vector<std::uint8_t>(x86_?16:24));memory.store(GuestAddress{scratch+80},16,3);memory.store(GuestAddress{scratch+88},32,child);
        std::vector<std::uint64_t> arguments=x86_?std::vector<std::uint64_t>{original,3,0,child,0,scratch+8}:std::vector<std::uint64_t>{original,scratch+80,scratch+8};
        start_callbacks(state,memory,{{method(original,9),std::move(arguments)}},Continuation::AccessibleOperation,scratch);return std::optional<std::uint32_t>{};
    };
    try{
        if(operation==12)return done(read(16));
        if(operation==10 || operation==11){
            if(result&0x80000000U)return done(static_cast<std::uint32_t>(result));
            if(operation==10){
                const auto total=static_cast<std::int32_t>(memory.load(GuestAddress{scratch+72},32));if(total<0)return done(0x8000ffffU);
                const auto count=read(48)<static_cast<std::uint64_t>(total)?std::min(read(40),static_cast<std::uint64_t>(total)-read(48)):0;
                memory.store(GuestAddress{scratch+24},64,0);memory.store(GuestAddress{scratch+32},64,count);if(!count)return done(read(40)?1:0);return next_child();
            }
            const auto index=read(24),pointer=memory.load(GuestAddress{scratch+8},bits),at=out+index*(x86_?16:24);
            memory.write(GuestAddress{at},std::vector<std::uint8_t>(x86_?16:24));memory.store(GuestAddress{at},16,pointer?9:3);memory.store(GuestAddress{at+8},pointer?bits:32,pointer?pointer:read(48)+index+1);
            memory.store(GuestAddress{read(56)},32,index+1);memory.store(GuestAddress{scratch+24},64,index+1);
            if(index+1==read(32))return done(index+1==read(40)?0:1);return next_child();
        }
        if(operation==1){
            if(result&0x80000000U)return done(static_cast<std::uint32_t>(result));const auto queried=memory.load(GuestAddress{scratch+8},bits);if(!queried)return done(0x8000ffffU);
            if(bridge.transfers.size()>=4096 || bridge.next_transfer>0x7fffffffU)return release(queried,0x8007000eU);
            const auto token=bridge.next_transfer++;bridge.transfers.emplace(token,AccessibleState::Transfer{queried,wparam});return done(token);
        }
        if(operation==2 || operation==5)return release(object,static_cast<std::uint32_t>(result));
        if(operation==4){
            if(result&0x80000000U)return done(static_cast<std::uint32_t>(result));const auto queried=memory.load(GuestAddress{scratch+8},bits);if(!queried)return done(0x8000ffffU);
            memory.store(GuestAddress{scratch+8},64,queried);memory.store(GuestAddress{scratch},64,5);
            start_callbacks(state,memory,{{method(queried,3),{queried,out}}},Continuation::AccessibleOperation,scratch);return {};
        }
        if(operation>=6 && operation<=9){
            const auto status=static_cast<std::uint32_t>(result);
            if(operation==6){
                if(status==no_interface){const auto original=read(64);memory.store(GuestAddress{scratch},64,10);start_callbacks(state,memory,{{method(original,8),{original,scratch+72}}},Continuation::AccessibleOperation,scratch);return {};}
                if(status&0x80000000U)return done(status);const auto queried=memory.load(GuestAddress{scratch+8},bits);if(!queried)return done(0x8000ffffU);
                memory.store(GuestAddress{scratch+8},64,queried);memory.store(GuestAddress{scratch},64,7);
                start_callbacks(state,memory,{{method(queried,5),{queried}}},Continuation::AccessibleOperation,scratch);return {};
            }
            if(status&0x80000000U)return release(object,status);
            if(operation==9){const auto obtained=memory.load(GuestAddress{read(56)},32);return release(object,obtained>read(40)?0x8000ffffU:obtained==read(40)?0U:1U);}
            if(operation==8 && status==1)return release(object,1);
            if(operation==7 && read(48)){
                memory.store(GuestAddress{scratch},64,8);start_callbacks(state,memory,{{method(object,4),{object,read(48)}}},Continuation::AccessibleOperation,scratch);return {};
            }
            memory.store(GuestAddress{scratch},64,9);start_callbacks(state,memory,{{method(object,3),{object,read(40),out,read(56)}}},Continuation::AccessibleOperation,scratch);return {};
        }
        if(operation==3){
            const auto found=bridge.transfers.find(result);
            if(found!=bridge.transfers.end() && found->second.wparam==wparam){
                const auto pointer=found->second.object;const auto target=method(pointer,0);bridge.transfers.erase(found);
                memory.store(GuestAddress{scratch},64,2);memory.store(GuestAddress{scratch+8},64,pointer);
                start_callbacks(state,memory,{{target,{pointer,scratch+24,out}}},Continuation::AccessibleOperation,scratch);return {};
            }
            if(result)return done((result&0x80000000U)?static_cast<std::uint32_t>(result):invalid);
            const auto window=read(40);const auto id=static_cast<std::uint32_t>(read(48));if(!gui_.windows.contains(window))return done(0x80070578U);
            if(id!=0 && id!=0xfffffffcU)return done(not_implemented);Guid iid{};memory.read(GuestAddress{scratch+24},iid);
            if(iid!=unknown_iid && iid!=dispatch_iid && iid!=accessible_iid)return done(no_interface);
            memory.store(GuestAddress{out},bits,create_accessible(memory,window,id));return done(0);
        }
        return done(0x8000ffffU);
    }catch(const std::bad_alloc&){return done(0x8007000eU);}
}
std::optional<std::uint32_t> WindowsServices::invoke_accessible(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=x86_?4:8,variant_size=x86_?16:24;
    const auto finish=[&](std::uint64_t value){if(name=="LresultFromObject" && (value&0x80000000U))value=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(!accessible_)accessible_=std::make_shared<AccessibleState>();auto& bridge=*accessible_;
    const auto guid=[&](std::uint64_t at){Guid v{};memory.read(GuestAddress{at},v);return v;};
    const auto known=[](const Guid& iid){return iid==accessible_iid || iid==dispatch_iid || iid==unknown_iid;};
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto create=[&](std::uint64_t window,std::uint32_t id){return create_accessible(memory,window,id);};
    const auto variant=[&](std::uint64_t at,std::uint16_t type,std::uint64_t value){memory.check(GuestAddress{at},variant_size,Access::Write);memory.write(GuestAddress{at},std::vector<std::uint8_t>(variant_size));memory.store(GuestAddress{at},16,type);if(type==9)put(at+8,value);else if(type)memory.store(GuestAddress{at+8},32,value);};
    const auto bstr=[&](std::u16string_view text){const auto bytes=utf16_bytes(text);const auto at=allocate_heap(memory,bytes.size()+8);if(!at)throw std::bad_alloc{};memory.store(GuestAddress{at},32,0x52545342U);memory.store(GuestAddress{at+4},32,bytes.size()-2);memory.write(GuestAddress{at+8},bytes);return at+8;};
    try{
        const auto method=[&](std::uint64_t object,unsigned index){const auto table=memory.load(GuestAddress{object},bits),target=memory.load(GuestAddress{table+index*unit},bits);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);return target;};
        const auto scratch_for=[&](unsigned operation){const auto at=allocate_heap(memory,112);if(!at)throw std::bad_alloc{};memory.write(GuestAddress{at},std::vector<std::uint8_t>(112));memory.store(GuestAddress{at},64,operation);return at;};
#include "windows_uia_provider.inc"
        if(name=="LresultFromObject"){
            if(!arg(0) || !arg(2))return finish(invalid);if(bridge.transfers.size()>=4096 || bridge.next_transfer>0x7fffffffU)return finish(0x8007000eU);
            const auto iid=guid(arg(0));const auto target=method(arg(2),0),scratch=scratch_for(1);memory.write(GuestAddress{scratch+24},iid);memory.store(GuestAddress{scratch+56},64,arg(1));
            start_callbacks(state,memory,{{target,{arg(2),scratch+24,scratch+8}}},Continuation::AccessibleOperation,scratch);return {};
        }
        if(name=="ObjectFromLresult"){
            if(!arg(3))return finish(pointer_error);put(arg(3),0);if(!arg(1))return finish(invalid);
            const auto found=bridge.transfers.find(arg(0));if(found==bridge.transfers.end() || found->second.wparam!=arg(2))return finish(invalid);
            const auto iid=guid(arg(1));const auto object=found->second.object,target=method(object,0),scratch=scratch_for(2);
            memory.store(GuestAddress{scratch+8},64,object);memory.write(GuestAddress{scratch+24},iid);bridge.transfers.erase(found);
            start_callbacks(state,memory,{{target,{object,scratch+24,arg(3)}}},Continuation::AccessibleOperation,scratch);return {};
        }
        if(name=="AccessibleObjectFromWindow"){
            if(!arg(3))return finish(pointer_error);put(arg(3),0);if(!arg(2))return finish(pointer_error);
            const auto found=gui_.windows.find(arg(0));if(found==gui_.windows.end())return finish(0x80070578U);
            const auto& window=found->second;if(window.thread!=threads_.current)return finish(0x80070032U);
            const auto iid=guid(arg(2));const auto scratch=scratch_for(3);memory.store(GuestAddress{scratch+16},64,arg(3));memory.write(GuestAddress{scratch+24},iid);memory.store(GuestAddress{scratch+40},64,arg(0));memory.store(GuestAddress{scratch+48},64,static_cast<std::uint32_t>(arg(1)));
            if(!window.procedure || !stock_gui_class(window.procedure).empty())return resume_accessible(state,memory,scratch,0);
            start_callbacks(state,memory,{{window.procedure,{arg(0),0x3d,0,static_cast<std::uint32_t>(arg(1))}}},Continuation::AccessibleOperation,scratch);return {};
        }
        if(name=="WindowFromAccessibleObject"){
            if(!arg(0) || !arg(1))return finish(invalid);put(arg(1),0);const auto found=bridge.objects.find(arg(0));
            if(found!=bridge.objects.end()){if(!gui_.windows.contains(found->second.window))return finish(0x80070578U);put(arg(1),found->second.window);return finish(0);}
            constexpr Guid ole_window{0x14,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            const auto target=method(arg(0),0),scratch=scratch_for(4);memory.write(GuestAddress{scratch+24},ole_window);memory.store(GuestAddress{scratch+16},64,arg(1));
            start_callbacks(state,memory,{{target,{arg(0),scratch+24,scratch+8}}},Continuation::AccessibleOperation,scratch);return {};
        }
        const auto children=[&](const Object& parent){std::vector<Object> out;if(!parent.id)out.push_back({parent.window,0xfffffffcU,1});else for(const auto& [h,w]:gui_.windows)if(w.parent==parent.window)out.push_back({h,0xfffffffcU,1});return out;};
        if(name=="AccessibleChildren"){
            if(!arg(4))return finish(invalid);memory.store(GuestAddress{arg(4)},32,0);
            const auto start=static_cast<std::int32_t>(arg(1)),count=static_cast<std::int32_t>(arg(2));if(!arg(0) || !arg(3) || start<0 || count<0)return finish(invalid);
            if(count>65536)return finish(0x8007000eU);memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(count)*variant_size,Access::Write);
            const auto found=bridge.objects.find(arg(0));
            if(found==bridge.objects.end()){
                constexpr Guid enum_variant{4,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                const auto target=method(arg(0),0),scratch=scratch_for(6);memory.store(GuestAddress{scratch+64},64,arg(0));memory.write(GuestAddress{scratch+24},enum_variant);memory.store(GuestAddress{scratch+16},64,arg(3));memory.store(GuestAddress{scratch+40},64,static_cast<std::uint32_t>(count));memory.store(GuestAddress{scratch+48},64,static_cast<std::uint32_t>(start));memory.store(GuestAddress{scratch+56},64,arg(4));
                start_callbacks(state,memory,{{target,{arg(0),scratch+24,scratch+8}}},Continuation::AccessibleOperation,scratch);return {};
            }
            if(!gui_.windows.contains(found->second.window))return finish(0x80070578U);
            const auto list=children(found->second);const auto first=std::min<std::size_t>(static_cast<std::size_t>(start),list.size()),size=std::min<std::size_t>(static_cast<std::size_t>(count),list.size()-first);
            std::vector<std::uint64_t> allocated;allocated.reserve(size);
            try{for(std::size_t i=0;i<size;++i)allocated.push_back(create(list[first+i].window,list[first+i].id));}
            catch(...){for(const auto address:allocated){bridge.objects.erase(address);(void)free_heap(memory,address,state);}throw;}
            for(std::size_t i=0;i<size;++i)variant(arg(3)+i*variant_size,9,allocated[i]);memory.store(GuestAddress{arg(4)},32,size);return finish(size==static_cast<std::size_t>(count)?0:1);
        }
        if(name=="CreateStdAccessibleObject"){
            if(!arg(3))return finish(pointer_error);put(arg(3),0);if(!arg(2))return finish(pointer_error);
            if(!gui_.windows.contains(arg(0)))return finish(0x80070578U);
            const auto id=static_cast<std::uint32_t>(arg(1));if(id!=0 && id!=0xfffffffcU)return finish(not_implemented);
            if(!known(guid(arg(2))))return finish(no_interface);put(arg(3),create(arg(0),id));return finish(0);
        }
        const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())throw HostCallFault("無效的 IAccessible 介面");const auto object=found->second;
        if(name=="IAccessible.QueryInterface"){
            if(!arg(2))return finish(pointer_error);put(arg(2),0);if(!arg(1))return finish(pointer_error);
            if(!known(guid(arg(1))))return finish(no_interface);++found->second.references;put(arg(2),arg(0));return finish(0);
        }
        if(name=="IAccessible.AddRef")return finish(++found->second.references);
        if(name=="IAccessible.Release"){const auto refs=--found->second.references;if(!refs){bridge.objects.erase(found);(void)free_heap(memory,arg(0),state);}return finish(refs);}
        if(name=="IAccessible.GetTypeInfoCount"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,0);return finish(0);}
        if(name=="IAccessible.GetTypeInfo"){if(!arg(3))return finish(pointer_error);put(arg(3),0);return finish(0x8002000bU);}
        if(name=="IAccessible.GetIDsOfNames" || name=="IAccessible.Invoke")return finish(not_implemented);
        if(!gui_.windows.contains(object.window))return finish(0x80070578U);
        const auto list=children(object);
        if(name=="IAccessible.get_accChildCount"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,list.size());return finish(0);}
        if(name=="IAccessible.get_accParent"){
            if(!arg(1))return finish(pointer_error);put(arg(1),0);
            if(object.id){put(arg(1),create(object.window,0));return finish(0);}
            const auto parent=gui_.windows.at(object.window).parent;if(!gui_.windows.contains(parent))return finish(1);put(arg(1),create(parent,0xfffffffcU));return finish(0);
        }
        if(name=="IAccessible.get_accFocus"){
            if(!arg(1))return finish(pointer_error);variant(arg(1),0,0);
            if(gui_.focus==object.window){variant(arg(1),3,0);return finish(0);}
            for(const auto& child:list)if(child.window==gui_.focus){variant(arg(1),9,create(child.window,child.id));return finish(0);}return finish(1);
        }
        if(name=="IAccessible.get_accSelection")return finish(not_implemented);
        Object target=object;
        if(service.aggregate_arguments){
            const auto index=static_cast<unsigned>(std::countr_zero(service.aggregate_arguments));const auto at=arg(index);
            if(!at || memory.load(GuestAddress{at},16)!=3)return finish(invalid);const auto child=static_cast<std::int32_t>(memory.load(GuestAddress{at+8},32));
            if(child<0 || static_cast<std::size_t>(child)>list.size())return finish(invalid);if(child)target=list[static_cast<std::size_t>(child)-1];
        }
        const auto& w=gui_.windows.at(target.window);const auto& cls=gui_control_class(w);
        if(name=="IAccessible.get_accChild"){
            if(!arg(2))return finish(pointer_error);put(arg(2),0);if(!memory.load(GuestAddress{arg(1)+8},32))return finish(invalid);put(arg(2),create(target.window,target.id));return finish(0);
        }
        if(name=="IAccessible.get_accName" || name=="IAccessible.get_accValue" || name=="IAccessible.get_accDescription" || name=="IAccessible.get_accHelp" || name=="IAccessible.get_accKeyboardShortcut" || name=="IAccessible.get_accDefaultAction"){
            if(!arg(2))return finish(pointer_error);put(arg(2),0);
            if(name=="IAccessible.get_accName"){if(cls==u"edit" || w.title.empty())return finish(1);put(arg(2),bstr(w.title));return finish(0);}
            if(name=="IAccessible.get_accValue" && cls==u"edit"){if(w.style&0x20U)return finish(0x80070005U);put(arg(2),bstr(w.title));return finish(0);}
            return finish(1);
        }
        if(name=="IAccessible.get_accRole" || name=="IAccessible.get_accState"){
            if(!arg(2))return finish(pointer_error);std::uint32_t value{};
            if(name=="IAccessible.get_accRole")value=!target.id?9U:cls==u"edit"?42U:cls==u"static"?41U:cls==u"button"?43U:cls==u"listbox"?33U:cls==u"combobox"?46U:cls==u"msctls_statusbar32"?23U:cls==u"toolbarwindow32"?22U:10U;
            else {value=(!w.enabled?1U:0U)|(gui_.focus==target.window?4U:0U)|(!w.visible?0x8000U:0U)|((w.style&0x10000U)?0x100000U:0U);if(cls==u"edit" && (w.style&0x20U))value|=0x20000000U;}
            variant(arg(2),3,value);return finish(0);
        }
        const auto rectangle=[&](const Object& value){const auto& window=gui_.windows.at(value.window);auto origin=gui_client_origin(gui_,value.id?value.window:window.parent);const auto outer=gui_outer_size(window);if(!value.id){origin[0]+=window.x;origin[1]+=window.y;}return std::array<std::int64_t,4>{origin[0],origin[1],value.id?window.width:outer[0],value.id?window.height:outer[1]};};
        if(name=="IAccessible.accLocation"){
            const auto rect=rectangle(target);for(unsigned i=1;i<=4;++i){if(!arg(i))return finish(pointer_error);memory.check(GuestAddress{arg(i)},4,Access::Write);}for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(i+1)},32,static_cast<std::uint32_t>(rect[i]));return finish(0);
        }
        if(name=="IAccessible.accHitTest"){
            if(!arg(3))return finish(pointer_error);variant(arg(3),0,0);const auto x=static_cast<std::int32_t>(arg(1)),y=static_cast<std::int32_t>(arg(2));
            const auto contains=[&](const Object& item){const auto r=rectangle(item);return gui_.windows.at(item.window).visible && x>=r[0] && y>=r[1] && x<r[0]+r[2] && y<r[1]+r[3];};
            if(!contains(object))return finish(1);for(auto it=list.rbegin();it!=list.rend();++it)if(contains(*it)){variant(arg(3),9,create(it->window,it->id));return finish(0);}variant(arg(3),3,0);return finish(0);
        }
        return finish(not_implemented);
    }catch(const std::bad_alloc&){return finish(0x8007000eU);}
}
}
