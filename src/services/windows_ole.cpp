#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/registry_store.hpp"
#include "automation_value.hpp"
#include "builtin_export_aliases.hpp"
#include "registry_text_view.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace x64arm {
std::span<const WindowsServiceDescriptor> ole_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","IUnknown_QueryService",4},{"shcore.dll","IUnknown_QueryService",4},
        {"shlwapi.dll","QISearch",4},
        {"ole32.dll","StringFromGUID2",3},{"ole32.dll","StringFromCLSID",2},{"ole32.dll","StringFromIID",2},{"ole32.dll","CLSIDFromString",2},{"ole32.dll","CLSIDFromProgID",2},{"ole32.dll","IIDFromString",2},
        {"ole32.dll","ProgIDFromCLSID",2},
        {"rpcrt4.dll","UuidFromStringA",2},{"rpcrt4.dll","UuidFromStringW",2},{"rpcrt4.dll","UuidCompare",3},{"rpcrt4.dll","UuidEqual",3},{"rpcrt4.dll","UuidIsNil",2},
        {"oleaut32.dll","SysAllocString",1},{"oleaut32.dll","SysReAllocString",2},
        {"oleaut32.dll","SysAllocStringLen",2},{"oleaut32.dll","SysReAllocStringLen",3},
        {"oleaut32.dll","SysFreeString",1},{"oleaut32.dll","SysStringLen",1},
        {"oleaut32.dll","SysStringByteLen",1},{"oleaut32.dll","SysAllocStringByteLen",2},
        {"oleaut32.dll","VarBstrCat",3},{"oleaut32.dll","VarBstrCmp",4,0,0,0xc},
        {"oleaut32.dll","VariantTimeToSystemTime",2,1},{"oleaut32.dll","SystemTimeToVariantTime",2},
        {"oleaut32.dll","VariantTimeToDosDateTime",3,1},{"oleaut32.dll","DosDateTimeToVariantTime",3},
        {"oleaut32.dll","VariantInit",1},{"oleaut32.dll","VariantClear",1},{"oleaut32.dll","VariantCopy",2},
        {"oleaut32.dll","VariantCopyInd",2},{"oleaut32.dll","VariantChangeType",4},{"oleaut32.dll","VariantChangeTypeEx",5},{"oleaut32.dll","VarCmp",4},
        {"oleaut32.dll","VarAdd",3},{"oleaut32.dll","VarSub",3},{"oleaut32.dll","VarMul",3},{"oleaut32.dll","VarDiv",3},
        {"oleaut32.dll","VarIdiv",3},{"oleaut32.dll","VarMod",3},{"oleaut32.dll","VarNeg",2},{"oleaut32.dll","VarNot",2},
        {"oleaut32.dll","VarAnd",3},{"oleaut32.dll","VarOr",3},{"oleaut32.dll","VarXor",3},
        {"ole32.dll","CoInitializeSecurity",9},{"ole32.dll","OleInitialize",1},{"ole32.dll","OleUninitialize",0},{"ole32.dll","ReleaseStgMedium",1},
        {"ole32.dll","OleRun",1},{"ole32.dll","OleIsRunning",1},{"ole32.dll","OleLockRunning",3},{"ole32.dll","OleSetContainedObject",2},
        {"ole32.dll","OleSetClipboard",1},{"ole32.dll","OleGetClipboard",1},{"ole32.dll","OleFlushClipboard",0},{"ole32.dll","OleIsCurrentClipboard",1},
        {"internal-ole-clipboard","Render",1},{"internal-ole-clipboard","Empty",0},{"internal-ole-clipboard","WindowProc",4},
        {"internal-clipboard-data","QueryInterface",3},{"internal-clipboard-data","AddRef",1},{"internal-clipboard-data","Release",1},
        {"internal-clipboard-data","GetData",3},{"internal-clipboard-data","GetDataHere",3},{"internal-clipboard-data","QueryGetData",2},
        {"internal-clipboard-data","GetCanonicalFormatEtc",3},{"internal-clipboard-data","SetData",4},{"internal-clipboard-data","EnumFormatEtc",3},
        {"internal-clipboard-data","DAdvise",5},{"internal-clipboard-data","DUnadvise",2},{"internal-clipboard-data","EnumDAdvise",2},
        {"internal-clipboard-enum","QueryInterface",3},{"internal-clipboard-enum","AddRef",1},{"internal-clipboard-enum","Release",1},
        {"internal-clipboard-enum","Next",4},{"internal-clipboard-enum","Skip",2},{"internal-clipboard-enum","Reset",1},{"internal-clipboard-enum","Clone",2},
        {"ole32.dll","CoRegisterMessageFilter",2},{"ole32.dll","CoFreeUnusedLibraries",0},
        {"ole32.dll","CoRegisterClassObject",5},{"ole32.dll","CoRevokeClassObject",1},
        {"ole32.dll","CoAddRefServerProcess",0},{"ole32.dll","CoReleaseServerProcess",0},
        {"ole32.dll","CoSuspendClassObjects",0},{"ole32.dll","CoResumeClassObjects",0},
        {"ole32.dll","RegisterDragDrop",2},{"ole32.dll","RevokeDragDrop",1}
    };return table;
}
std::string_view ole_ordinal_name(std::uint16_t ordinal){
    const auto& entries=detail::ole_ordinal_exports;
    const auto found=std::lower_bound(std::begin(entries),std::end(entries),ordinal,
        [](const auto& entry,auto value){return entry.first<value;});
    return found!=std::end(entries) && found->first==ordinal?found->second:std::string_view{};
}
namespace {
constexpr auto guid_order=windows_guid_order;
using OleGuid=WindowsGuid;
constexpr std::uint32_t e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,e_notimpl=0x80004001U,disp_badtype=0x80020008U;
constexpr std::uint16_t vt_bstr=8,vt_dispatch=9,vt_unknown=13,vt_array=0x2000,vt_byref=0x4000;
// COM 指標與 vtable slot 使用客體 ABI，不能套用宿主指標寬度。
std::uint64_t com_method(const GuestMemory& memory,std::uint64_t object,unsigned index){
    const std::uint8_t bits=memory.execution_mode()==ExecutionMode::X86?32:64;
    const auto width=bits/8U,offset=index*width;
    const auto table=memory.load(GuestAddress{object},bits);
    if(table>UINT64_MAX-offset)throw MemoryFault(GuestAddress{table},Access::Read,width,"COM vtable 位址溢位");
    return memory.load(GuestAddress{table+offset},bits);
}
std::uint32_t variant_status(std::uint16_t type){
    if(type&0x9000U)return disp_badtype; // VT_VECTOR／reserved 不屬於 Automation VARIANT。
    const auto base=type&0xfffU;
    if(base==15 || (base>23 && base!=36))return disp_badtype;
    if(base==36)return e_notimpl; // IRecordInfo 生命週期尚未提供。
    if((type&(vt_byref|vt_array)) && base<=1)return disp_badtype;
    if(base==12 && !(type&(vt_byref|vt_array)))return disp_badtype;
    if((type&vt_array) && !(type&vt_byref))return e_notimpl;
    return 0;
}
std::uint32_t bstr_length(const GuestMemory& memory,std::uint64_t pointer){
    if(!pointer)return 0;
    if(pointer<4)throw MemoryFault(GuestAddress{pointer},Access::Read,4,"BSTR 長度前綴位址不合法");
    return static_cast<std::uint32_t>(memory.load(GuestAddress{pointer-4},32));
}
std::uint64_t terminated_bytes(const GuestMemory& memory,std::uint64_t pointer){
    if(!pointer)return 0;
    for(std::uint64_t bytes=0;bytes<=UINT32_MAX-2U;bytes+=2){
        if(pointer>UINT64_MAX-bytes)throw MemoryFault(GuestAddress{pointer},Access::Read,2,"BSTR 來源位址溢位");
        if(!memory.load(GuestAddress{pointer+bytes},16))return bytes;
    }
    return UINT64_MAX;
}
}
std::vector<GuestCallback> WindowsServices::revoke_gui_drop_targets(GuestMemory& memory,std::uint64_t handle){
    std::vector<GuestCallback> callbacks;std::vector<std::uint64_t> registrations;
    for(const auto& [window,drop]:gui_.drop_targets){
        auto parent=window;bool belongs=false;
        for(std::size_t depth=0;parent && depth<=gui_.windows.size();++depth){
            if(parent==handle){belongs=true;break;}
            const auto found=gui_.windows.find(parent);if(found==gui_.windows.end())break;parent=found->second.parent;
        }
        if(!belongs)continue;
        const auto release=com_method(memory,drop.object,2);if(!handles(GuestAddress{release}))memory.check(GuestAddress{release},1,Access::Execute);
        callbacks.push_back({release,{drop.object}});registrations.push_back(window);
    }
    for(const auto window:registrations)gui_.drop_targets.erase(window);
    return callbacks;
}
std::vector<GuestCallback> WindowsServices::revoke_com_message_filter(GuestMemory& memory,std::uint64_t thread){
    const auto found=gui_.com_message_filters.find(thread);if(found==gui_.com_message_filters.end())return {};
    const auto object=found->second,release=com_method(memory,object,2);if(!handles(GuestAddress{release}))memory.check(GuestAddress{release},1,Access::Execute);
    gui_.com_message_filters.erase(found);return {{release,{object}}};
}
std::optional<std::uint32_t> WindowsServices::invoke_ole(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family.starts_with("internal-clipboard-") || service.family=="internal-ole-clipboard" ||
       service.symbol=="OleSetClipboard" || service.symbol=="OleGetClipboard" || service.symbol=="OleFlushClipboard" || service.symbol=="OleIsCurrentClipboard")
        return invoke_ole_clipboard(state,memory,service);
    if(service.symbol=="CoRegisterClassObject" || service.symbol=="CoRevokeClassObject" ||
       service.symbol=="CoAddRefServerProcess" || service.symbol=="CoReleaseServerProcess" ||
       service.symbol=="CoSuspendClassObjects" || service.symbol=="CoResumeClassObjects" ||
       service.symbol=="OleRun" || service.symbol=="OleIsRunning" || service.symbol=="OleLockRunning" || service.symbol=="OleSetContainedObject")
        return invoke_com_class(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const std::uint8_t bits=x86_?32:64;const unsigned width=bits/8U,variant_size=x86_?16:24;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(name=="VariantCopyInd" || name=="VariantChangeType" || name=="VariantChangeTypeEx" || name=="VarCmp" ||
       name=="VarAdd" || name=="VarSub" || name=="VarMul" || name=="VarDiv" || name=="VarIdiv" || name=="VarMod" ||
       name=="VarNeg" || name=="VarNot" || name=="VarAnd" || name=="VarOr" || name=="VarXor")return invoke_automation(state,memory,service);
    if(name=="QISearch"){
        if(!arg(3))return done(0x80004003U);
        try {
            memory.store(GuestAddress{arg(3)},bits,0);
            if(!arg(0) || !arg(1) || !arg(2))return done(0x80070057U);
            OleGuid requested{};memory.read(GuestAddress{arg(2)},requested);
            constexpr OleGuid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            const auto stride=x86_?8U:16U;
            for(auto entry=arg(1);;){
                const auto iid=memory.load(GuestAddress{entry},bits);
                if(!iid)return done(0x80004002U);
                OleGuid candidate{};if(requested!=unknown)memory.read(GuestAddress{iid},candidate);
                if(requested==candidate || requested==unknown){
                    const auto offset=memory.load(GuestAddress{entry+width},32);
                    if(arg(0)>(x86_?UINT32_MAX:UINT64_MAX)-offset)return done(0x80004003U);
                    const auto object=arg(0)+offset;
                    const auto table=memory.load(GuestAddress{object},bits);
                    const auto addref=memory.load(GuestAddress{table+width},bits);
                    memory.store(GuestAddress{arg(3)},bits,object);
                    start_callbacks(state,memory,{{addref,{object}}},Continuation::Return,0);return std::nullopt;
                }
                if(entry>(x86_?UINT32_MAX:UINT64_MAX)-stride)return done(0x80004003U);
                entry+=stride;
            }
        }catch(const MemoryFault&){return done(0x80004003U);}
    }
    if(name=="IUnknown_QueryService"){
        return invoke_com_class(state,memory,service);
    }
    if(name=="VariantTimeToSystemTime" || name=="VariantTimeToDosDateTime"){
        std::uint64_t raw=arg(0);if(!x86_){raw=0;for(unsigned i=0;i<8;++i)raw|=static_cast<std::uint64_t>(state.xmm[0][i])<<(i*8);}
        const auto fields=automation::time_fields(std::bit_cast<double>(raw));if(!fields || !arg(1))return done(0);
        if(name=="VariantTimeToSystemTime"){memory.check(GuestAddress{arg(1)},16,Access::Write);for(unsigned i=0;i<8;++i)memory.store(GuestAddress{arg(1)+i*2},16,(*fields)[i]);}
        else {if(!arg(2) || (*fields)[0]<1980 || (*fields)[0]>2107)return done(0);memory.check(GuestAddress{arg(1)},2,Access::Write);memory.check(GuestAddress{arg(2)},2,Access::Write);memory.store(GuestAddress{arg(1)},16,(((*fields)[0]-1980)<<9)|((*fields)[1]<<5)|(*fields)[3]);memory.store(GuestAddress{arg(2)},16,((*fields)[4]<<11)|((*fields)[5]<<5)|((*fields)[6]/2));}
        return done(1);
    }
    if(name=="SystemTimeToVariantTime" || name=="DosDateTimeToVariantTime"){
        std::array<unsigned,8> fields{};std::uint64_t output=0;
        if(name=="SystemTimeToVariantTime"){if(!arg(0))return done(0);for(unsigned i=0;i<8;++i)fields[i]=static_cast<unsigned>(memory.load(GuestAddress{arg(0)+i*2},16));output=arg(1);}
        else {const auto date=static_cast<unsigned>(arg(0))&0xffffU,time=static_cast<unsigned>(arg(1))&0xffffU;fields={(date>>9)+1980,(date>>5)&15,0,date&31,time>>11,(time>>5)&63,(time&31)*2,0};output=arg(2);}
        if(!output)return done(0);const auto value=automation::date_value(fields);if(!value)return done(0);memory.store(GuestAddress{output},64,std::bit_cast<std::uint64_t>(*value));return done(1);
    }
    if(name=="StringFromGUID2" || name=="StringFromCLSID" || name=="StringFromIID"){
        const bool caller_buffer=name=="StringFromGUID2";
        if(!arg(0) || !arg(1))return done(caller_buffer?0:e_invalidarg);
        if(caller_buffer && static_cast<std::int32_t>(arg(2))<39)return done(0);
        OleGuid value{};memory.read(GuestAddress{arg(0)},value);const auto bytes=utf16_bytes(windows_guid_text(value));
        if(caller_buffer){memory.write(GuestAddress{arg(1)},bytes);return done(39);}
        memory.store(GuestAddress{arg(1)},bits,0);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)return done(e_outofmemory);memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{arg(1)},bits,pointer);return done(0);
    }
    if(name=="ProgIDFromCLSID"){
        if(!arg(1))return done(e_invalidarg);
        memory.store(GuestAddress{arg(1)},bits,0);if(!arg(0))return done(e_invalidarg);
        OleGuid value{};memory.read(GuestAddress{arg(0)},value);
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return done(0x80040150U);
        initialize_tokens();RegistryTextView lookup{runtime_.registry,class_registry_roots(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1)),registry_view(0,x86_),false};
        const auto path=u"clsid\\"+windows_guid_text(value)+u"\\progid";std::optional<std::u16string> registered;
        for(const auto& root:lookup.roots){
            const auto key=runtime_.registry.keys.find(lookup.path_key(root+u"\\"+path));
            if(key==runtime_.registry.keys.end() || !key->second.contains(u""))continue;
            registered=lookup.value_at(root+u"\\"+path);if(!registered)return done(0x80040150U);break;
        }
        if(!registered)return done(0x80040154U);
        const auto bytes=utf16_bytes(*registered);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)return done(e_outofmemory);
        memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{arg(1)},bits,pointer);return done(0);
    }
    if(name=="CLSIDFromString" || name=="CLSIDFromProgID" || name=="IIDFromString" || name.starts_with("UuidFromString")){
        const bool rpc=name.starts_with("Uuid");if(!arg(1))return done(rpc?87:e_invalidarg);memory.check(GuestAddress{arg(1)},16,Access::Write);
        if(!arg(0)){if(name=="CLSIDFromProgID")return done(e_invalidarg);memory.write(GuestAddress{arg(1)},OleGuid{});return done(0);}
        auto text=name=="UuidFromStringA"?utf8_to_utf16(read_ansi(memory,arg(0))):read_utf16(memory,GuestAddress{arg(0)});auto value=name=="CLSIDFromProgID"?std::optional<OleGuid>{}:parse_windows_guid(text,!rpc);
        if(!value && (name=="CLSIDFromProgID" || (name=="CLSIDFromString" && !text.empty() && text.front()!=u'{'))){
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return done(0x80040150U);
            initialize_tokens();RegistryTextView lookup{runtime_.registry,class_registry_roots(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1)),registry_view(0,x86_),false};
            if(!text.empty())if(const auto registered=lookup.value(text+u"\\clsid"))value=parse_windows_guid(*registered,true);
        }
        if(!value)return done(rpc?1705:name=="IIDFromString"?e_invalidarg:0x800401f3U);memory.write(GuestAddress{arg(1)},*value);return done(0);
    }
    if(name=="UuidCompare" || name=="UuidEqual" || name=="UuidIsNil"){
        OleGuid left{},right{};if(arg(0))memory.read(GuestAddress{arg(0)},left);const auto status=arg(name=="UuidIsNil"?1:2);if(status)memory.store(GuestAddress{status},32,0);
        if(name!="UuidIsNil" && arg(1))memory.read(GuestAddress{arg(1)},right);int comparison=0;for(const auto i:guid_order)if(left[i]!=right[i]){comparison=left[i]<right[i]?-1:1;break;}
        return done(name=="UuidCompare"?static_cast<std::uint32_t>(comparison):comparison==0);
    }
    // OLE 與 CoInitialize 共用執行緒 apartment 與成功呼叫的 reference count。
    if(name=="CoInitializeSecurity"){
        // 只提供程序本地 COM policy；遠端 activation 仍未開放，不宣稱具備 DCOM provider。
        const auto count=static_cast<std::uint32_t>(arg(1)),authentication=static_cast<std::uint32_t>(arg(4)),impersonation=static_cast<std::uint32_t>(arg(5)),capabilities=static_cast<std::uint32_t>(arg(7));
        std::uint32_t status=0;
        if(runtime_.com_security)status=0x80010119U; // RPC_E_TOO_LATE
        else if(arg(3) || arg(8) || authentication>6 || impersonation>4)status=0x80070057U;
        else if(arg(0) || arg(2) || arg(6) || (count!=0 && count!=UINT32_MAX) || (capabilities&~0x3000U))status=0x80004001U;
        else runtime_.com_security=std::array{authentication,impersonation,capabilities};
        arg.return_to(state,status);return std::nullopt;
    }
    if(name=="OleInitialize"){
        const auto result=invoke_gui_support(state,memory,{"ole32.dll","CoInitialize",1});
        if(state.reg(Gpr::Rax)<=1)++gui_.ole_initializations[threads_.current];return result;
    }
    if(name=="OleUninitialize"){
        auto& count=gui_.ole_initializations[threads_.current];if(count){
            --count;if(!count){auto callbacks=retire_ole_clipboard(state,memory,threads_.current);
                if(!callbacks.empty()){callbacks.push_back({resolve("ole32.dll","CoUninitialize").value,{}});start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;}}
            return invoke_gui_support(state,memory,{"ole32.dll","CoUninitialize",0});}
        arg.return_to(state,0);return std::nullopt;
    }
    if(name=="CoFreeUnusedLibraries"){
        // 只有啟用載入器持有的 DLL 參與 DllCanUnloadNow；不減少一般 LoadLibrary 的引用。
        return release_unused_winrt_modules(state,memory);
    }
    if(name=="CoRegisterMessageFilter"){
        const auto initialized=gui_.com_initializations.find(threads_.current);
        if(initialized==gui_.com_initializations.end() || initialized->second<16 || !(initialized->second&2)){
            arg.return_to(state,1);return std::nullopt; // 只有已初始化的 STA 具有 message filter。
        }
        if(arg(1))memory.check(GuestAddress{arg(1)},width,Access::Write);
        const auto found=gui_.com_message_filters.find(threads_.current);
        const auto previous=found==gui_.com_message_filters.end()?0:found->second;
        std::vector<GuestCallback> callbacks;
        if(arg(0)){
            const auto addref=com_method(memory,arg(0),1),release=com_method(memory,arg(0),2);
            for(const auto target:{addref,release})if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
            callbacks.push_back({addref,{arg(0)}});
        }
        if(!arg(1)){auto releases=revoke_com_message_filter(memory,threads_.current);callbacks.insert(callbacks.end(),releases.begin(),releases.end());}
        if(arg(0))gui_.com_message_filters[threads_.current]=arg(0);else gui_.com_message_filters.erase(threads_.current);
        // 舊 filter 的持有權移交給 out 參數；沒有 out 時由 OLE 釋放。
        if(arg(1))memory.store(GuestAddress{arg(1)},bits,previous);
        if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;}
        arg.return_to(state,0);return std::nullopt;
    }
    if(name=="RegisterDragDrop" || name=="RevokeDragDrop"){
        constexpr std::uint32_t not_registered=0x80040100U,already_registered=0x80040101U,invalid_window=0x80040102U;
        const auto window=gui_.windows.find(arg(0));std::uint32_t error=0;
        if(window==gui_.windows.end() || (name=="RegisterDragDrop" && window->second.destroying))error=invalid_window;
        else if(window->second.thread!=threads_.current)error=0x8001010eU;
        else if(name=="RegisterDragDrop"){
            if(!gui_.ole_initializations[threads_.current])error=e_outofmemory;
            else if(gui_.drop_targets.contains(arg(0)))error=already_registered;
            else if(!arg(1))error=e_invalidarg;
            else {
                const auto addref=com_method(memory,arg(1),1),release=com_method(memory,arg(1),2);
                for(const auto target:{addref,release})if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
                gui_.drop_targets.emplace(arg(0),GuiDropTarget{arg(1),threads_.current,gui_.next_object++});
                start_callbacks(state,memory,{{addref,{arg(1)}}},Continuation::Return,0);return std::nullopt;
            }
        }else {
            const auto found=gui_.drop_targets.find(arg(0));if(found==gui_.drop_targets.end())error=not_registered;
            else if(found->second.thread!=threads_.current)error=0x8001010eU;
            else {
                const auto object=found->second.object,release=com_method(memory,object,2);if(!handles(GuestAddress{release}))memory.check(GuestAddress{release},1,Access::Execute);
                gui_.drop_targets.erase(found);start_callbacks(state,memory,{{release,{object}}},Continuation::Return,0);return std::nullopt;
            }
        }
        arg.return_to(state,error);return std::nullopt;
    }
    if(name=="ReleaseStgMedium"){
        const auto medium=arg(0);memory.check(GuestAddress{medium},3*width,Access::Read);memory.check(GuestAddress{medium},4,Access::Write);
        const auto type=memory.load(GuestAddress{medium},32),value=memory.load(GuestAddress{medium+width},bits),owner=memory.load(GuestAddress{medium+2*width},bits);
        std::vector<GuestCallback> callbacks;
        const auto release=[&](std::uint64_t object){
            if(!object)return;const auto target=com_method(memory,object,2);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
            callbacks.push_back({target,{object}});
        };
        if(type!=0 && type!=1 && type!=2 && type!=4 && type!=8 && type!=16 && type!=32 && type!=64)throw HostCallFault("STGMEDIUM tymed 不合法");
        if(type==4 || type==8)release(value);
        release(owner); // IStream／IStorage 與 pUnkForRelease 各自持有一份 reference。
        const auto nested_call=[&](auto invoke,const WindowsServiceDescriptor& descriptor){
            auto nested=state;
            if(x86_){
                const auto sp=state.reg(Gpr::Rsp)-8;
                memory.store(GuestAddress{sp},32,0);memory.store(GuestAddress{sp+4},32,value);nested.reg(Gpr::Rsp)=sp;
            }else nested.reg(Gpr::Rcx)=value;
            (void)(this->*invoke)(nested,memory,descriptor);return nested.reg(Gpr::Rax);
        };
        if(type==1 && !owner){
            if(nested_call(&WindowsServices::invoke_gui_extra,{"kernel32.dll","GlobalFree",1}))throw HostCallFault("STGMEDIUM HGLOBAL 無法釋放");
        }else if(type==16 && !owner){
            const auto deleted=nested_call(&WindowsServices::invoke_gdi,{"gdi32.dll","DeleteObject",1});
            if(value && !deleted)throw HostCallFault("STGMEDIUM GDI object 無法釋放");
        }else if(type==64 && !owner){
            if(value && !nested_call(&WindowsServices::invoke_metafile,{"gdi32.dll","DeleteEnhMetaFile",1}))throw HostCallFault("STGMEDIUM EMF 無法釋放");
        }else if(type==32 && !owner){
            const auto global=gui_.global_memory.find(value);
            if(global==gui_.global_memory.end() || !global->second.address || global->second.size<(x86_?16U:24U))throw HostCallFault("STGMEDIUM METAFILEPICT 不合法");
            const auto metafile=memory.load(GuestAddress{global->second.address+(x86_?12U:16U)},bits);gui_.metafiles.erase(metafile);
            if(nested_call(&WindowsServices::invoke_gui_extra,{"kernel32.dll","GlobalFree",1}))throw HostCallFault("STGMEDIUM METAFILEPICT 無法釋放");
        }else if(type==2 && value){
            if(!allocations_.contains(value))throw HostCallFault("STGMEDIUM 檔名不是有效的 CoTaskMem 配置");
            if(!owner)(void)nested_call(&WindowsServices::invoke_file,{"kernel32.dll","DeleteFileW",1});
            gui_.shell_pidls.erase(value);if(!free_heap(memory,value,state))throw HostCallFault("STGMEDIUM 檔名配置已失效");
        }
        if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::OleReleaseStgMedium);return std::nullopt;}
        memory.store(GuestAddress{medium},32,0);arg.return_to(state,0);return std::nullopt;
    }
    const auto owned_bstr=[&](std::uint64_t pointer){check_bstr(memory,pointer);};
    const auto free_bstr=[&](std::uint64_t pointer){this->free_bstr(state,memory,pointer);};
    const auto allocate=[&](std::uint64_t bytes,std::span<const std::uint8_t> data){return allocate_bstr(memory,bytes,data);};
    const auto copy_from=[&](std::uint64_t source,std::uint64_t bytes)->std::uint64_t{
        if(bytes>UINT32_MAX || bytes+10>memory.remaining_capacity())return 0;
        std::vector<std::uint8_t> contents;
        if(source && bytes){memory.check(GuestAddress{source},static_cast<std::size_t>(bytes),Access::Read);try{contents.resize(static_cast<std::size_t>(bytes));}catch(const std::bad_alloc&){return 0;}memory.read(GuestAddress{source},contents);}
        return allocate(bytes,contents);
    };
    std::uint64_t result=0;
    if(name=="VarBstrCat"){
        if(!arg(2))result=e_invalidarg;
        else {
            memory.check(GuestAddress{arg(2)},width,Access::Write);
            const auto left=bstr_length(memory,arg(0)),right=bstr_length(memory,arg(1));const auto total=std::uint64_t{left}+right;
            std::vector<std::uint8_t> contents;
            if(total<=UINT32_MAX && total+10<=memory.remaining_capacity()){
                if(left)memory.check(GuestAddress{arg(0)},left,Access::Read);if(right)memory.check(GuestAddress{arg(1)},right,Access::Read);
                try{contents.resize(static_cast<std::size_t>(total));}catch(const std::bad_alloc&){}
            }
            std::uint64_t replacement=0;
            if(contents.size()==total){if(left)memory.read(GuestAddress{arg(0)},std::span(contents).first(left));if(right)memory.read(GuestAddress{arg(1)},std::span(contents).subspan(left,right));replacement=allocate(total,contents);}
            memory.store(GuestAddress{arg(2)},bits,replacement);result=replacement?0U:e_outofmemory;
        }
    }else if(name=="VarBstrCmp"){
        const auto left=bstr_length(memory,arg(0)),right=bstr_length(memory,arg(1));
        if(left)memory.check(GuestAddress{arg(0)},left,Access::Read);if(right)memory.check(GuestAddress{arg(1)},right,Access::Read);
        try {
        if(!arg(2)){
            std::vector<std::uint8_t> a(left),b(right);if(left)memory.read(GuestAddress{arg(0)},a);if(right)memory.read(GuestAddress{arg(1)},b);
            result=a==b?1U:std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end())?0U:2U;
        }else {
            const auto text=[&](std::uint64_t pointer,std::uint32_t length){std::u16string value(length/2,0);for(std::size_t i=0;i<value.size();++i)value[i]=static_cast<char16_t>(memory.load(GuestAddress{pointer+2*i},16));return value;};
            result=automation::compare_strings(text(arg(0),left),text(arg(1),right),static_cast<std::uint32_t>(arg(2)),static_cast<std::uint32_t>(arg(3)));
        }
        }catch(const std::bad_alloc&){result=e_outofmemory;}
    }else if(name=="SysStringLen" || name=="SysStringByteLen")result=bstr_length(memory,arg(0))/(name=="SysStringLen"?2U:1U);
    else if(name=="SysFreeString")free_bstr(arg(0));
    else if(name=="SysAllocString"){
        if(arg(0))result=copy_from(arg(0),terminated_bytes(memory,arg(0)));
    }else if(name=="SysAllocStringLen" || name=="SysAllocStringByteLen"){
        const auto bytes=static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(1)))*(name=="SysAllocStringLen"?2U:1U);result=copy_from(arg(0),bytes);
    }else if(name=="SysReAllocString" || name=="SysReAllocStringLen"){
        memory.check(GuestAddress{arg(0)},width,Access::Write);const auto previous=memory.load(GuestAddress{arg(0)},bits);owned_bstr(previous);
        if(name=="SysReAllocString" && !arg(1)){free_bstr(previous);memory.store(GuestAddress{arg(0)},bits,0);result=1;}
        else {
            const auto bytes=name=="SysReAllocString"?terminated_bytes(memory,arg(1)):static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(2)))*2;
            const auto replacement=copy_from(arg(1),bytes);
            if(replacement){free_bstr(previous);memory.store(GuestAddress{arg(0)},bits,replacement);result=1;}
        }
    }else if(name=="VariantInit")memory.store(GuestAddress{arg(0)},16,0);
    else if(name=="VariantClear" || name=="VariantCopy"){
        const bool copying=name=="VariantCopy";const auto destination=arg(0),source=copying?arg(1):0;
        if(!destination || (copying && !source)){arg.return_to(state,e_invalidarg);return std::nullopt;}
        memory.check(GuestAddress{destination},variant_size,Access::Read);memory.check(GuestAddress{destination},variant_size,Access::Write);
        if(copying && destination==source){arg.return_to(state,0);return std::nullopt;}
        std::array<std::uint8_t,24> replacement{};std::uint16_t source_type=0;std::uint64_t source_value=0;
        if(copying){memory.read(GuestAddress{source},std::span{replacement}.first(variant_size));source_type=static_cast<std::uint16_t>(memory.load(GuestAddress{source},16));source_value=memory.load(GuestAddress{source+8},bits);}
        const auto destination_type=static_cast<std::uint16_t>(memory.load(GuestAddress{destination},16));
        if(((source_type|destination_type)&vt_array)!=0)return invoke_safearray(state,memory,service);
        if(const auto status=variant_status(source_type)){arg.return_to(state,status);return std::nullopt;}
        if(const auto status=variant_status(destination_type)){arg.return_to(state,status);return std::nullopt;}
        const auto destination_value=memory.load(GuestAddress{destination+8},bits);
        if(destination_type==vt_bstr)owned_bstr(destination_value);
        const auto method=[&](std::uint64_t object,unsigned index){
            const auto target=com_method(memory,object,index);
            if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);return target;
        };
        std::uint64_t addref=0;
        if((source_type==vt_unknown || source_type==vt_dispatch) && source_value)addref=method(source_value,1);
        if((destination_type==vt_unknown || destination_type==vt_dispatch) && destination_value){
            start_callbacks(state,memory,{{method(destination_value,2),{destination_value}}},copying?Continuation::OleVariantCopy:Continuation::OleVariantClear);return std::nullopt;
        }
        std::vector<std::uint8_t> string_copy;
        if(source_type==vt_bstr && source_value){
            const auto bytes=bstr_length(memory,source_value);memory.check(GuestAddress{source_value},bytes,Access::Read);
            try{string_copy.resize(bytes);}catch(const std::bad_alloc&){arg.return_to(state,e_outofmemory);return std::nullopt;}
            memory.read(GuestAddress{source_value},string_copy);
        }
        if(destination_type==vt_bstr)free_bstr(destination_value);
        memory.store(GuestAddress{destination},16,0);
        if(copying){
            if(source_type==vt_bstr && source_value){
                const auto pointer=allocate(string_copy.size(),string_copy);
                if(!pointer){arg.return_to(state,e_outofmemory);return std::nullopt;}
                for(unsigned i=0;i<width;++i)replacement[8+i]=static_cast<std::uint8_t>(pointer>>(i*8U));
            }
            memory.write(GuestAddress{destination},std::span{replacement}.first(variant_size));
            if(addref){start_callbacks(state,memory,{{addref,{source_value}}},Continuation::Return,0);return std::nullopt;}
        }
    }else throw HostCallFault("未實作的 OLEAUT32 服務");
    arg.return_to(state,result);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_ole(GuestState& state,GuestMemory& memory,const CallbackFrame& frame){
    if(frame.continuation==Continuation::OleReleaseStgMedium){const WindowsCall arg(state,memory,1);memory.store(GuestAddress{arg(0)},32,0);arg.return_to(state,0);return std::nullopt;}
    const bool copying=frame.continuation==Continuation::OleVariantCopy;const WindowsCall arg(state,memory,copying?2:1);
    memory.store(GuestAddress{arg(0)},16,0);
    if(copying)return invoke_ole(state,memory,{"oleaut32.dll","VariantCopy",2});
    arg.return_to(state,0);return std::nullopt;
}
#include "windows_ole_clipboard.inc"

}
