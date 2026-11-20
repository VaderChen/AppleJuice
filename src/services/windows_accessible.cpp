#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include "builtin_version.hpp"
#include <algorithm>
namespace x64arm {
namespace {
// MSAA 角色（ROLE_SYSTEM_*，1～0x40）與狀態位元（STATE_SYSTEM_*）的英文顯示名稱；0 號狀態為 normal。
constexpr std::u16string_view role_names[]{u"",u"title bar",u"menu bar",u"scroll bar",u"grip",u"sound",u"cursor",u"caret",u"alert",u"window",u"client",u"popup menu",u"menu item",
    u"tool tip",u"application",u"document",u"pane",u"chart",u"dialog",u"border",u"grouping",u"separator",u"tool bar",u"status bar",u"table",u"column header",u"row header",
    u"column",u"row",u"cell",u"link",u"help balloon",u"character",u"list",u"list item",u"outline",u"outline item",u"page tab",u"property page",u"indicator",u"graphic",
    u"text",u"editable text",u"push button",u"check box",u"radio button",u"combo box",u"drop down",u"progress bar",u"dial",u"hot key field",u"slider",u"spin box",
    u"diagram",u"animation",u"equation",u"drop down button",u"menu button",u"grid drop down button",u"white space",u"page tab list",u"clock",u"split button",u"IP address",u"outline button"};
constexpr std::u16string_view state_names[]{u"unavailable",u"selected",u"focused",u"pressed",u"checked",u"mixed",u"read only",u"hot tracked",u"default",u"expanded",u"collapsed",
    u"busy",u"floating",u"marqueed",u"animated",u"invisible",u"offscreen",u"sizeable",u"moveable",u"self voicing",u"focusable",u"selectable",u"linked",u"traversed",
    u"multiple selectable",u"extended selectable",u"alert low",u"alert medium",u"alert high",u"protected",u"has popup"};
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
        {"oleacc.dll","GetRoleTextA",3},{"oleacc.dll","GetRoleTextW",3},{"oleacc.dll","GetStateTextA",3},{"oleacc.dll","GetStateTextW",3},
        {"oleacc.dll","GetOleaccVersionInfo",2},{"oleacc.dll","GetProcessHandleFromHwnd",1},{"oleacc.dll","AccessibleObjectFromEvent",5,0,0,0x6},{"oleacc.dll","AccessibleObjectFromPoint",3,0x1},
        {"uiautomationcore.dll","UiaGetReservedMixedAttributeValue",1},{"uiautomationcore.dll","UiaGetReservedNotSupportedValue",1},
        {"uiautomationcore.dll","UiaHostProviderFromHwnd",2},{"uiautomationcore.dll","UiaReturnRawElementProvider",4},
        {"uiautomationcore.dll","UiaRaiseAutomationEvent",2},{"uiautomationcore.dll","UiaRaiseAutomationPropertyChangedEvent",4,0,12},
        {"uiautomationcore.dll","UiaRaiseStructureChangedEvent",4},{"uiautomationcore.dll","UiaRaiseNotificationEvent",5},{"uiautomationcore.dll","UiaClientsAreListening",0},{"uiautomationcore.dll","UiaDisconnectProvider",1},{"uiautomationcore.dll","UiaDisconnectAllProviders",0}};
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
    if(name=="AccessibleObjectFromEvent" || name=="AccessibleObjectFromPoint"){
        // Event：(hwnd, dwObjectID, dwChildID, ppacc, pvarChild)：以 AccessibleObjectFromWindow 取得物件；dwChildID 不是 CHILDID_SELF 時以 get_accChild 取子物件，
        //   子項是物件則回傳子物件與 CHILDID_SELF，否則回傳父物件與子項 ID。
        // Point：(POINT ptScreen, ppacc, pvarChild)：WindowFromPoint 的視窗之工作區物件（OBJID_CLIENT），再以 accHitTest 逐層往下（最多 64 層）到最內層的物件或子項；
        //   點不在任何視窗上時 E_FAIL。
        return nested_continuation(state,memory,"oleacc 物件查詢",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool from_point=service.symbol=="AccessibleObjectFromPoint";const unsigned size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(size*8);
            const unsigned variant_bytes=x86_?16U:24U;const auto out=call(from_point?1:3),child_out=call(from_point?2:4);
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto entry=[&](std::string_view dll,std::string_view target){const auto stub=host_service_stub(dll,target);return stub.value?stub.value:resolve(dll,target).value;};
            const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},width)+index*size},width);};
            const auto set_child=[&](std::uint32_t id){m.fill(GuestAddress{child_out},0,variant_bytes);m.store(GuestAddress{child_out},16,3);m.store(GuestAddress{child_out+8},32,id);};
            if(!out || !child_out)co_return finish(pointer_error);m.check(GuestAddress{out},size,Access::Write);m.check(GuestAddress{child_out},variant_bytes,Access::Write);
            m.store(GuestAddress{out},width,0);m.fill(GuestAddress{child_out},0,variant_bytes);
            GuestScratch temporary(*this,m,s);const auto iid=temporary.allocate(16),slot=temporary.allocate(size),value=temporary.allocate(variant_bytes);if(!iid || !slot || !value)co_return finish(0x8007000eU);
            m.write(GuestAddress{iid},accessible_iid);
            std::uint64_t window=0;std::uint32_t object_id=0xfffffffcU,child_id=0;
            if(from_point){window=co_await service_callback::call_value(entry("user32.dll","WindowFromPoint"),x86_?std::vector<std::uint64_t>{call(0)&UINT32_MAX,call(0)>>32}:std::vector<std::uint64_t>{call(0)});
                if(x86_)window&=UINT32_MAX;if(!window)co_return finish(0x80004005U);}
            else{window=call(0);object_id=static_cast<std::uint32_t>(call(1));child_id=static_cast<std::uint32_t>(call(2));}
            m.store(GuestAddress{slot},width,0);
            auto status=static_cast<std::uint32_t>(co_await service_callback::call(entry("oleacc.dll","AccessibleObjectFromWindow"),{window,object_id,iid,slot}));
            if(status&0x80000000U)co_return finish(status);auto object=m.load(GuestAddress{slot},width);if(!object)co_return finish(0x80004005U);
            // get_accChild 的 VARIANT 參數：x86 以 16 位元組值傳遞（四個堆疊格），x64 傳指向複本的指標。
            const auto variant_args=[&](std::uint32_t id){m.fill(GuestAddress{value},0,variant_bytes);m.store(GuestAddress{value},16,3);m.store(GuestAddress{value+8},32,id);
                if(x86_)return std::vector<std::uint64_t>{m.load(GuestAddress{value},32),m.load(GuestAddress{value+4},32),m.load(GuestAddress{value+8},32),m.load(GuestAddress{value+12},32)};
                return std::vector<std::uint64_t>{value};};
            const auto as_accessible=[&](std::uint64_t dispatch)->service_callback::Task<std::uint64_t>{
                m.store(GuestAddress{slot},width,0);const auto hr=static_cast<std::uint32_t>(co_await service_callback::call(method(dispatch,0),{dispatch,iid,slot}));
                (void)co_await service_callback::call(method(dispatch,2),{dispatch});co_return hr?0:m.load(GuestAddress{slot},width);};
            if(!from_point){
                if(!child_id){m.store(GuestAddress{out},width,object);set_child(0);co_return finish(0);}
                std::vector<std::uint64_t> args{object};const auto variant=variant_args(child_id);args.insert(args.end(),variant.begin(),variant.end());args.push_back(slot);
                m.store(GuestAddress{slot},width,0);status=static_cast<std::uint32_t>(co_await service_callback::call(method(object,9),std::move(args))); // get_accChild
                const auto dispatch=m.load(GuestAddress{slot},width);
                if(!status && dispatch){if(const auto child=co_await as_accessible(dispatch)){(void)co_await service_callback::call(method(object,2),{object});m.store(GuestAddress{out},width,child);set_child(0);co_return finish(0);}}
                m.store(GuestAddress{out},width,object);set_child(child_id);co_return finish(0);
            }
            const auto x=static_cast<std::uint32_t>(call(0)),y=static_cast<std::uint32_t>(call(0)>>32);
            for(unsigned depth=0;depth<64;++depth){
                m.fill(GuestAddress{value},0,variant_bytes);
                status=static_cast<std::uint32_t>(co_await service_callback::call(method(object,24),{object,x,y,value})); // accHitTest
                const auto type=m.load(GuestAddress{value},16);
                if(!status && type==9){const auto dispatch=m.load(GuestAddress{value+8},width);const auto next=dispatch?co_await as_accessible(dispatch):0;
                    if(next && next!=object){(void)co_await service_callback::call(method(object,2),{object});object=next;continue;}
                    if(next)(void)co_await service_callback::call(method(next,2),{next});}
                m.store(GuestAddress{out},width,object);set_child(!status && type==3?static_cast<std::uint32_t>(m.load(GuestAddress{value+8},32)):0U);co_return finish(0);
            }
            m.store(GuestAddress{out},width,object);set_child(0);co_return finish(0);
        });
    }
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
        if(name.starts_with("GetRoleText") || name.starts_with("GetStateText")){
            // (id, buffer, capacity)：回傳字元數（不含 NUL）；buffer 為 NULL 時回傳所需長度，容量不足時截斷並保留 NUL；未知角色或非單一狀態位元回 0。
            const bool role=name.starts_with("GetRole"),wide=name.ends_with('W');const auto id=static_cast<std::uint32_t>(arg(0));std::u16string_view text;
            if(role){if(id>0 && id<std::size(role_names))text=role_names[id];}
            else if(!id)text=u"normal";
            else if(!(id&(id-1))){const auto bit=static_cast<unsigned>(std::countr_zero(id));if(bit<std::size(state_names))text=state_names[bit];}
            if(text.empty()){if(arg(1) && static_cast<std::uint32_t>(arg(2)))memory.store(GuestAddress{arg(1)},wide?16:8,0);return finish(0);}
            if(!arg(1))return finish(text.size());const auto capacity=static_cast<std::uint32_t>(arg(2));if(!capacity)return finish(0);
            const auto count=std::min<std::size_t>(text.size(),capacity-1);
            if(wide){memory.write(GuestAddress{arg(1)},utf16_bytes(text.substr(0,count)));}
            else{std::vector<std::uint8_t> bytes;for(std::size_t i=0;i<count;++i)bytes.push_back(static_cast<std::uint8_t>(text[i]));bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}
            return finish(count);
        }
        if(name=="GetOleaccVersionInfo"){
            // 版本與內建 oleacc.dll 的版本資源一致：pVer＝主.次、pBuild＝組建.修訂（高字組／低字組）。
            if(!arg(0) || !arg(1))return finish(0);const auto version=builtin_file_version("oleacc.dll");
            memory.store(GuestAddress{arg(0)},32,(version[0]<<16)|(version[1]&0xffffU));memory.store(GuestAddress{arg(1)},32,(version[2]<<16)|(version[3]&0xffffU));return finish(0);
        }
        if(name=="GetProcessHandleFromHwnd"){
            // 視窗所屬程序，存取權為 PROCESS_DUP_HANDLE｜VM_OPERATION｜VM_READ｜VM_WRITE｜SYNCHRONIZE；無效視窗回 NULL。
            const auto saved=last_error_;
            const auto handle=nested_service(state,memory,"win32u.dll","NtUserGetWindowProcessHandle",{arg(0),0x100078U});
            if(handle)last_error_=saved;return finish(handle);
        }
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
