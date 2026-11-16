#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_guid.hpp"
#include "service_callback_task.hpp"
#include "x64arm/registry_store.hpp"
#include "registry_text_view.hpp"
#include <algorithm>

// ole32 類別登錄與輔助家族：CoGetTreatAsClass／CoTreatAsClass、OleGetAutoConvert／OleSetAutoConvert、CoGetPSClsid／CoRegisterPSClsid、CoIsOle1Class、
// OleRegGetUserType、OleRegGetMiscStatus、GetClassFile、IsEqualGUID、IsValidInterface、CoGetCurrentLogicalThreadId、CoIsHandlerConnected、
// CoLoadLibrary／CoFreeLibrary／CoFreeAllLibraries／CoFreeUnusedLibrariesEx、CoGetState／CoSetState、CoMarshalHresult／CoUnmarshalHresult、
// CoInitializeWOW／OleInitializeWOW、ReadFmtUserTypeStg／WriteFmtUserTypeStg（MS-OLEDS CompObj 串流）。
// 登錄一律經 advapi32 讀寫 HKEY_CLASSES_ROOT（使用者與機器類別的合併檢視）；需要呼叫客體程式碼（DllMain、COM 方法）者經 nested_continuation。
namespace x64arm {
namespace {
constexpr std::uint32_t e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,e_pointer=0x80004003U;
constexpr std::uint32_t regdb_read=0x80040150U,regdb_keymissing=0x80040152U,regdb_classnotreg=0x80040154U,regdb_iidnotreg=0x80040155U;
}
std::span<const WindowsServiceDescriptor> ole_misc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CoGetTreatAsClass",2},{"ole32.dll","CoTreatAsClass",2},{"ole32.dll","OleGetAutoConvert",2},{"ole32.dll","OleSetAutoConvert",2},
        {"ole32.dll","CoGetPSClsid",2},{"ole32.dll","CoRegisterPSClsid",2},{"ole32.dll","CoIsOle1Class",1},{"ole32.dll","OleRegGetUserType",3,0,0,0x2},
        {"ole32.dll","OleRegGetMiscStatus",3,0,0,0x2},{"ole32.dll","GetClassFile",2},{"ole32.dll","IsEqualGUID",2},{"ole32.dll","IsValidInterface",1},
        {"ole32.dll","CoGetCurrentLogicalThreadId",1},{"ole32.dll","CoIsHandlerConnected",1},
        {"ole32.dll","CoLoadLibrary",2,0,0,0x2},{"ole32.dll","CoFreeLibrary",1},{"ole32.dll","CoFreeAllLibraries",0},{"ole32.dll","CoFreeUnusedLibrariesEx",2,0,0,0x3},
        {"ole32.dll","CoGetState",1},{"ole32.dll","CoSetState",1},{"ole32.dll","CoMarshalHresult",2,0,0,0x2},{"ole32.dll","CoUnmarshalHresult",2},
        {"ole32.dll","CoInitializeWOW",2,0,0,0x3},{"ole32.dll","OleInitializeWOW",2,0,0,0x3},
        {"ole32.dll","ReadFmtUserTypeStg",3},{"ole32.dll","WriteFmtUserTypeStg",3,0,0,0x2},
    };return table;
}
std::optional<std::u16string> WindowsServices::class_registry_value(const std::u16string& key,const std::u16string& name){
    if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return std::nullopt;initialize_tokens();
    const RegistryTextView lookup{runtime_.registry,class_registry_roots(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1)),registry_view(0,x86_)};
    return lookup.value(key,name);
}
int WindowsServices::class_registry_root(const std::u16string& key){
    if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return -1;initialize_tokens();
    const auto roots=class_registry_roots(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1));const RegistryTextView lookup{runtime_.registry,roots,registry_view(0,x86_)};
    for(std::size_t i=0;i<roots.size();++i)if(runtime_.registry.keys.contains(lookup.path_key(roots[i]+u"\\"+key)))return static_cast<int>(i);return -1;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const std::uint64_t classes_root=x86_?0x80000000ULL:0xffffffff80000000ULL;
    const auto read_guid=[&](std::uint64_t at){WindowsGuid value{};memory.read(GuestAddress{at},value);return value;};
    const auto host=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,dll,symbol,args)&UINT32_MAX;};
    const auto class_value=[&](const std::u16string& key,const std::u16string& value){return class_registry_value(key,value);};
    const auto class_exists=[&](const std::u16string& key){return class_registry_root(key)>=0;};
    // 寫入／刪除落在類別機碼所在的根：使用者 → HKCU\Software\Classes，HKCR 本身，機器 → HKLM\Software\Classes。
    const auto root_of=[&](int which,const std::u16string& key)->std::pair<std::uint64_t,std::u16string>{
        if(which==0)return {x86_?0x80000001ULL:0xffffffff80000001ULL,u"Software\\Classes\\"+key};if(which==2)return {x86_?0x80000002ULL:0xffffffff80000002ULL,u"Software\\Classes\\"+key};return {classes_root,key};};
    const auto set_class_value=[&](int which,const std::u16string& key,const std::u16string& text)->std::uint32_t{
        const auto [hive,path]=root_of(which,key);GuestScratch temporary(*this,memory,state);const auto key_text=temporary.text(path),data=temporary.text(text);if(!key_text || !data)return e_outofmemory;
        const auto status=host("advapi32.dll","RegSetKeyValueW",{hive,key_text,0,1,data,(text.size()+1)*2});return status?0x80070000U|(status&0xffffU):0;};
    const auto delete_class_key=[&](const std::u16string& key){const auto which=class_registry_root(key);if(which<0)return;const auto [hive,path]=root_of(which,key);
        GuestScratch temporary(*this,memory,state);if(const auto key_text=temporary.text(path))(void)host("advapi32.dll","RegDeleteKeyW",{hive,key_text});};
    const auto clsid_key=[&](const WindowsGuid& id){return u"CLSID\\"+windows_guid_text(id);};
    const auto task_text=[&](const std::u16string& text)->std::uint64_t{const auto bytes=utf16_bytes(text);auto block=nested_service(state,memory,"ole32.dll","CoTaskMemAlloc",{bytes.size()});if(x86_)block&=UINT32_MAX;if(block)memory.write(GuestAddress{block},bytes);return block;};
    constexpr WindowsGuid null_guid{};
    try{
        if(name=="IsEqualGUID"){if(!arg(0) || !arg(1))return done(0);return done(read_guid(arg(0))==read_guid(arg(1))?1U:0U);}
        if(name=="IsValidInterface"){
            // 物件、vtable 與 IUnknown 三個方法位址可讀且非空即視為有效介面。
            if(!arg(0))return done(0);try{const auto table=memory.load(GuestAddress{arg(0)},bits);if(!table)return done(0);for(unsigned i=0;i<3;++i)if(!memory.load(GuestAddress{table+i*unit},bits))return done(0);return done(1);}
            catch(const MemoryFault&){return done(0);}
        }
        if(name=="CoIsHandlerConnected")return done(1); // 同程序物件不是遠端處理常式，一律視為已連線
        if(name=="CoGetCurrentLogicalThreadId"){
            // 每條執行緒一個隨機邏輯執行緒 GUID，第一次查詢時建立。
            if(!arg(0))return done(e_invalidarg);auto found=com_logical_threads_.find(threads_.current);
            if(found==com_logical_threads_.end()){GuestScratch temporary(*this,memory,state);const auto slot=temporary.allocate(16);if(!slot)return done(e_outofmemory);
                if(host("ole32.dll","CoCreateGuid",{slot}))return done(e_outofmemory);found=com_logical_threads_.emplace(threads_.current,read_guid(slot)).first;}
            memory.write(GuestAddress{arg(0)},found->second);return done(0);
        }
        if(name=="CoGetTreatAsClass" || name=="OleGetAutoConvert"){
            // TreatAs：沒有時輸出原 CLSID 並回 S_FALSE；AutoConvertTo：沒有時回 REGDB_E_KEYMISSING。
            if(!arg(0) || !arg(1))return done(e_invalidarg);const auto id=read_guid(arg(0));const bool treat=name=="CoGetTreatAsClass";
            const auto text=class_value(clsid_key(id)+(treat?u"\\TreatAs":u"\\AutoConvertTo"),u"");const auto parsed=text?parse_windows_guid(*text,true):std::nullopt;
            if(!parsed){if(treat){memory.write(GuestAddress{arg(1)},id);return done(1);}memory.write(GuestAddress{arg(1)},null_guid);return done(regdb_keymissing);}
            memory.write(GuestAddress{arg(1)},*parsed);return done(0);
        }
        if(name=="CoTreatAsClass" || name=="OleSetAutoConvert"){
            // 新 CLSID 為 CLSID_NULL（或 TreatAs 與原 CLSID 相同）時移除設定；舊類別必須已登錄。
            if(!arg(0) || !arg(1))return done(e_invalidarg);const auto old_id=read_guid(arg(0)),new_id=read_guid(arg(1));const bool treat=name=="CoTreatAsClass";
            const auto which=class_registry_root(clsid_key(old_id));if(which<0)return done(regdb_classnotreg);const auto key=clsid_key(old_id)+(treat?u"\\TreatAs":u"\\AutoConvertTo");
            if(new_id==null_guid || (treat && new_id==old_id)){delete_class_key(key);return done(0);}
            return done(set_class_value(which,key,windows_guid_text(new_id)));
        }
        if(name=="CoRegisterPSClsid"){if(!arg(0) || !arg(1))return done(e_invalidarg);com_ps_clsids_[read_guid(arg(0))]=read_guid(arg(1));return done(0);}
        if(name=="CoGetPSClsid"){
            // 先查本程序以 CoRegisterPSClsid 登錄的對應，再查 Interface\{IID}\ProxyStubClsid32。
            if(!arg(0) || !arg(1))return done(e_invalidarg);const auto iid=read_guid(arg(0));
            if(const auto found=com_ps_clsids_.find(iid);found!=com_ps_clsids_.end()){memory.write(GuestAddress{arg(1)},found->second);return done(0);}
            const auto text=class_value(u"Interface\\"+windows_guid_text(iid)+u"\\ProxyStubClsid32",u"");const auto parsed=text?parse_windows_guid(*text,true):std::nullopt;
            if(!parsed)return done(regdb_iidnotreg);memory.write(GuestAddress{arg(1)},*parsed);return done(0);
        }
        if(name=="CoIsOle1Class"){if(!arg(0))return done(0);return done(class_exists(clsid_key(read_guid(arg(0)))+u"\\Ole1Class")?1U:0U);}
        if(name=="OleRegGetUserType"){
            // USERCLASSTYPE_FULL 1 為 CLSID 預設值；SHORT 2／APPNAME 3 先查 AuxUserType\n，沒有時退回完整名稱。
            if(!arg(2))return done(e_invalidarg);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(0))return done(e_invalidarg);const auto form=static_cast<std::uint32_t>(arg(1));
            if(form<1 || form>3)return done(e_invalidarg);const auto key=clsid_key(read_guid(arg(0)));if(!class_exists(key))return done(regdb_classnotreg);
            std::optional<std::u16string> text;if(form!=1)text=class_value(key+u"\\AuxUserType\\"+(form==2?u"2":u"3"),u"");if(!text || text->empty())text=class_value(key,u"");
            if(!text || text->empty())return done(regdb_read);const auto block=task_text(*text);if(!block)return done(e_outofmemory);memory.store(GuestAddress{arg(2)},bits,block);return done(0);
        }
        if(name=="OleRegGetMiscStatus"){
            // MiscStatus\<aspect> 優先，其次 MiscStatus 預設值（十進位文字）；類別已登錄但沒有 MiscStatus 時為 0。
            if(!arg(2))return done(e_invalidarg);memory.store(GuestAddress{arg(2)},32,0);if(!arg(0))return done(e_invalidarg);const auto key=clsid_key(read_guid(arg(0)));
            if(!class_exists(key))return done(regdb_classnotreg);
            auto text=class_value(key+u"\\MiscStatus\\"+utf8_to_utf16_lossy(std::to_string(static_cast<std::uint32_t>(arg(1)))),u"");if(!text)text=class_value(key+u"\\MiscStatus",u"");
            std::uint32_t value=0;if(text)for(const auto c:*text){if(c<u'0' || c>u'9')break;value=value*10+static_cast<std::uint32_t>(c-u'0');}
            memory.store(GuestAddress{arg(2)},32,value);return done(0);
        }
        if(name=="CoMarshalHresult" || name=="CoUnmarshalHresult" || name=="CoGetState" || name=="CoSetState" || name=="CoLoadLibrary" || name=="CoFreeLibrary" ||
           name=="CoFreeAllLibraries" || name=="CoFreeUnusedLibrariesEx" || name=="CoInitializeWOW" || name=="OleInitializeWOW" ||
           name=="ReadFmtUserTypeStg" || name=="WriteFmtUserTypeStg" || name=="GetClassFile"){
            return nested_continuation(state,memory,"OleMisc",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const auto symbol=service.symbol;const unsigned size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(size*8);
                const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
                const auto entry=[&](std::string_view dll,std::string_view target){const auto stub=host_service_stub(dll,target);return stub.value?stub.value:resolve(dll,target).value;};
                const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},width)+index*size},width);};
                const auto failed=[](std::uint32_t status){return (status&0x80000000U)!=0;};
                GuestScratch temporary(*this,m,s);const auto slot=temporary.allocate(16);if(!slot)co_return finish(e_outofmemory);
                if(symbol=="CoMarshalHresult" || symbol=="CoUnmarshalHresult"){
                    // HRESULT 以 4 位元組寫入／讀出資料流。
                    if(!call(0) || (symbol=="CoUnmarshalHresult" && !call(1)))co_return finish(e_invalidarg);const bool writing=symbol=="CoMarshalHresult";
                    m.store(GuestAddress{slot},32,writing?static_cast<std::uint32_t>(call(1)):0U);m.store(GuestAddress{slot+8},32,0);
                    const auto status=static_cast<std::uint32_t>(co_await service_callback::call(method(call(0),writing?4:3),{call(0),slot,4,slot+8}));if(failed(status))co_return finish(status);
                    if(m.load(GuestAddress{slot+8},32)!=4)co_return finish(writing?0x80030070U:0x8003001eU); // STG_E_MEDIUMFULL／STG_E_READFAULT
                    if(!writing)m.store(GuestAddress{call(1)},32,m.load(GuestAddress{slot},32));co_return finish(0);
                }
                if(symbol=="CoGetState"){
                    // 每執行緒的狀態物件：取得時 AddRef；沒有時 S_FALSE。
                    if(!call(0))co_return finish(e_pointer);m.store(GuestAddress{call(0)},width,0);const auto found=com_thread_states_.find(threads_.current);
                    if(found==com_thread_states_.end() || !found->second)co_return finish(1);const auto object=found->second;
                    (void)co_await service_callback::call(method(object,1),{object});m.store(GuestAddress{call(0)},width,object);co_return finish(0);
                }
                if(symbol=="CoSetState"){
                    const auto replacement=call(0);if(replacement)(void)co_await service_callback::call(method(replacement,1),{replacement});
                    std::uint64_t previous=0;if(const auto found=com_thread_states_.find(threads_.current);found!=com_thread_states_.end())previous=found->second;
                    if(replacement)com_thread_states_[threads_.current]=replacement;else com_thread_states_.erase(threads_.current);
                    if(previous)(void)co_await service_callback::call(method(previous,2),{previous});co_return finish(0);
                }
                if(symbol=="CoLoadLibrary"){
                    // LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH)；bAutoFree 時記錄，供 CoFreeAllLibraries 釋放。
                    if(!call(0))co_return finish(0);const auto module=co_await service_callback::call_value(entry("kernel32.dll","LoadLibraryExW"),{call(0),0,8});
                    const auto handle=x86_?module&UINT32_MAX:module;if(handle && static_cast<std::uint32_t>(call(1)))com_loaded_libraries_.insert(handle);co_return finish(handle);
                }
                if(symbol=="CoFreeLibrary"){com_loaded_libraries_.erase(call(0));(void)co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{call(0)});co_return finish(0);}
                if(symbol=="CoFreeAllLibraries"){const auto modules=com_loaded_libraries_;com_loaded_libraries_.clear();
                    for(const auto module:modules)(void)co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{module});co_return finish(0);}
                if(symbol=="CoFreeUnusedLibrariesEx"){(void)co_await service_callback::call(entry("ole32.dll","CoFreeUnusedLibraries"),{});co_return finish(0);}
                if(symbol=="CoInitializeWOW")co_return finish(co_await service_callback::call(entry("ole32.dll","CoInitializeEx"),{0,2}));
                if(symbol=="OleInitializeWOW")co_return finish(co_await service_callback::call(entry("ole32.dll","OleInitialize"),{0}));
                const auto stream_name=temporary.text(u"\x0001" u"CompObj");if(!stream_name)co_return finish(e_outofmemory);
                if(symbol=="WriteFmtUserTypeStg"){
                    // CompObj 串流：標頭（0xFFFE0001、版本 0x0A03、0xFFFFFFFF＋類別 CLSID）、ANSI 使用者類型、ANSI 剪貼簿格式、保留字串、
                    // Unicode 標記 0x71B239F4、Unicode 使用者類型、Unicode 剪貼簿格式、保留字串。標準格式寫成 0xFFFFFFFE＋格式值，已登錄格式寫名稱。
                    if(!call(0))co_return finish(e_invalidarg);const auto stat=temporary.allocate(96);if(!stat)co_return finish(e_outofmemory);
                    WindowsGuid clsid{};if(!failed(static_cast<std::uint32_t>(co_await service_callback::call(method(call(0),17),{call(0),stat,1}))))m.read(GuestAddress{stat+(x86_?48U:56U)},clsid);
                    const auto user=call(2)?read_utf16(m,GuestAddress{call(2)},32767):std::u16string{};const auto format=static_cast<std::uint32_t>(call(1))&0xffffU;std::u16string format_name;
                    if(format>=0xc000){const auto buffer=temporary.allocate(2*256);if(buffer){const auto length=static_cast<std::uint32_t>(co_await service_callback::call(entry("user32.dll","GetClipboardFormatNameW"),{format,buffer,256}));if(length)format_name=read_utf16(m,GuestAddress{buffer},255);}}
                    std::vector<std::uint8_t> data;const auto put=[&](std::uint32_t value){for(unsigned i=0;i<4;++i)data.push_back(static_cast<std::uint8_t>(value>>(i*8)));};
                    const auto ansi=[&](const std::u16string& text){if(text.empty()){put(0);return;}const auto bytes=utf16_to_utf8_lossy(text);put(static_cast<std::uint32_t>(bytes.size()+1));data.insert(data.end(),bytes.begin(),bytes.end());data.push_back(0);};
                    const auto unicode=[&](const std::u16string& text){if(text.empty()){put(0);return;}put(static_cast<std::uint32_t>(text.size()+1));for(const auto c:text){data.push_back(static_cast<std::uint8_t>(c));data.push_back(static_cast<std::uint8_t>(c>>8));}data.push_back(0);data.push_back(0);};
                    const auto clipboard=[&](bool wide_text){if(!format)put(0);else if(format<0xc000){put(0xfffffffeU);put(format);}else if(wide_text)unicode(format_name);else ansi(format_name);};
                    put(0xfffe0001U);put(0x00000a03U);put(0xffffffffU);data.insert(data.end(),clsid.begin(),clsid.end());
                    ansi(user);clipboard(false);put(0);put(0x71b239f4U);unicode(user);clipboard(true);put(0);
                    m.store(GuestAddress{slot},width,0);auto status=static_cast<std::uint32_t>(co_await service_callback::call(method(call(0),3),{call(0),stream_name,0x1011,0,0,slot})); // CREATE｜SHARE_EXCLUSIVE｜WRITE
                    if(failed(status))co_return finish(status);const auto stream=m.load(GuestAddress{slot},width);const auto buffer=temporary.allocate(data.size());
                    if(!buffer){(void)co_await service_callback::call(method(stream,2),{stream});co_return finish(e_outofmemory);}m.write(GuestAddress{buffer},data);m.store(GuestAddress{slot+8},32,0);
                    status=static_cast<std::uint32_t>(co_await service_callback::call(method(stream,4),{stream,buffer,data.size(),slot+8}));(void)co_await service_callback::call(method(stream,2),{stream});
                    if(failed(status))co_return finish(status);co_return finish(m.load(GuestAddress{slot+8},32)==data.size()?0U:0x80030070U);
                }
                if(symbol=="ReadFmtUserTypeStg"){
                    // 有 Unicode 標記時採 Unicode 欄位，否則採 ANSI（ACP）欄位；已登錄格式名稱以 RegisterClipboardFormat 取回格式值。
                    if(call(1))m.store(GuestAddress{call(1)},16,0);if(call(2))m.store(GuestAddress{call(2)},width,0);if(!call(0))co_return finish(e_invalidarg);
                    m.store(GuestAddress{slot},width,0);auto status=static_cast<std::uint32_t>(co_await service_callback::call(method(call(0),4),{call(0),stream_name,0,0x10,0,slot})); // READ｜SHARE_EXCLUSIVE
                    if(failed(status))co_return finish(status);const auto stream=m.load(GuestAddress{slot},width);const auto buffer=temporary.allocate(65536);
                    if(!buffer){(void)co_await service_callback::call(method(stream,2),{stream});co_return finish(e_outofmemory);}m.store(GuestAddress{slot+8},32,0);
                    status=static_cast<std::uint32_t>(co_await service_callback::call(method(stream,3),{stream,buffer,65536,slot+8}));(void)co_await service_callback::call(method(stream,2),{stream});if(failed(status))co_return finish(status);
                    std::vector<std::uint8_t> data(m.load(GuestAddress{slot+8},32));m.read(GuestAddress{buffer},data);std::size_t at=28;bool bad=data.size()<28;
                    const auto get=[&]()->std::uint32_t{if(at+4>data.size()){bad=true;return 0;}std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{data[at+i]}<<(i*8);at+=4;return value;};
                    const auto ansi=[&]()->std::u16string{const auto length=get();if(!length || bad)return {};if(at+length>data.size()){bad=true;return {};}std::string bytes(data.begin()+static_cast<std::ptrdiff_t>(at),data.begin()+static_cast<std::ptrdiff_t>(at+length));at+=length;
                        while(!bytes.empty() && !bytes.back())bytes.pop_back();return utf8_to_utf16_lossy(bytes);};
                    const auto unicode=[&]()->std::u16string{const auto length=get();if(!length || bad)return {};if(at+length*2ULL>data.size()){bad=true;return {};}std::u16string text;for(std::uint32_t i=0;i<length;++i)text.push_back(static_cast<char16_t>(data[at+i*2]|(data[at+i*2+1]<<8)));at+=length*2ULL;
                        while(!text.empty() && !text.back())text.pop_back();return text;};
                    struct Format {std::uint32_t value{};std::u16string name;};
                    const auto clipboard=[&](bool wide_text)->Format{const auto marker=get();if(bad || !marker)return {};if(marker==0xffffffffU || marker==0xfffffffeU)return {get(),{}};at-=4;return {0,wide_text?unicode():ansi()};};
                    auto user=ansi();auto format=clipboard(false);(void)ansi();
                    if(!bad && at+4<=data.size()){const auto saved=at;if(get()==0x71b239f4U){const auto wide_user=unicode();const auto wide_format=clipboard(true);if(!bad){user=wide_user;format=wide_format;}}else at=saved;}
                    if(data.size()<28)co_return finish(0x800300fbU); // 標頭不足：STG_E_INVALIDHEADER
                    std::uint32_t value=format.value;
                    if(!format.name.empty()){const auto text=temporary.text(format.name);if(text)value=static_cast<std::uint32_t>(co_await service_callback::call(entry("user32.dll","RegisterClipboardFormatW"),{text}));}
                    if(call(1))m.store(GuestAddress{call(1)},16,value&0xffffU);
                    if(call(2) && !user.empty()){const auto bytes=utf16_bytes(user);const auto block=co_await service_callback::call_value(entry("ole32.dll","CoTaskMemAlloc"),{bytes.size()});const auto handle=x86_?block&UINT32_MAX:block;
                        if(!handle)co_return finish(e_outofmemory);m.write(GuestAddress{handle},bytes);m.store(GuestAddress{call(2)},width,handle);}
                    co_return finish(0);
                }
                // GetClassFile：複合文件取儲存的類別；其他檔案依副檔名的 ProgID／CLSID 登錄。檔案不存在 MK_E_CANTOPENFILE，沒有對應 MK_E_INVALIDEXTENSION。
                if(!call(0) || !call(1))co_return finish(e_invalidarg);m.write(GuestAddress{call(1)},WindowsGuid{});const auto path=read_utf16(m,GuestAddress{call(0)},32767);
                if(static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetFileAttributesW"),{call(0)}))==UINT32_MAX)co_return finish(0x800401eaU);
                if(!static_cast<std::uint32_t>(co_await service_callback::call(entry("ole32.dll","StgIsStorageFile"),{call(0)}))){
                    m.store(GuestAddress{slot},width,0);if(!failed(static_cast<std::uint32_t>(co_await service_callback::call(entry("ole32.dll","StgOpenStorage"),{call(0),0,0x20,0,0,slot})))){ // READ｜SHARE_DENY_WRITE
                        const auto storage=m.load(GuestAddress{slot},width);const auto stat=temporary.allocate(96);std::uint32_t status=e_outofmemory;
                        if(stat){status=static_cast<std::uint32_t>(co_await service_callback::call(method(storage,17),{storage,stat,1}));if(!failed(status)){WindowsGuid clsid{};m.read(GuestAddress{stat+(x86_?48U:56U)},clsid);m.write(GuestAddress{call(1)},clsid);}}
                        (void)co_await service_callback::call(method(storage,2),{storage});co_return finish(failed(status)?status:0U);}
                }
                const auto slash=path.find_last_of(u"\\/");const auto dot=path.rfind(u'.');if(dot==path.npos || (slash!=path.npos && dot<slash))co_return finish(0x800401e6U);
                const auto lookup=[&](const std::u16string& key){return class_registry_value(key,u"");};
                const auto progid=lookup(path.substr(dot));if(progid && !progid->empty())if(const auto text=lookup(*progid+u"\\CLSID"))if(const auto parsed=parse_windows_guid(*text,true)){m.write(GuestAddress{call(1)},*parsed);co_return finish(0);}
                co_return finish(0x800401e6U);
            });
        }
    }catch(const MemoryFault&){return done(e_invalidarg);}
    throw HostCallFault("未知的 ole32 雜項服務");
}
}
