// 包含於 windows_nt_native.cpp：ntdll 的 Ldr*（載入器）、Tp*（執行緒池）、Dbg 中斷點、地區設定／UI 語言／IDN／Unicode 正規化、
// 路徑輔助、SID 字串與系統指標編碼。全部在宿主端直接執行既有 Win32 實作或讀寫同一份狀態（PEB 載入串列、資源目錄、執行緒池），
// 只轉換引數與 NTSTATUS；只有 LdrEnumerateLoadedModules 需要逐一呼叫客體回呼。
namespace {
constexpr std::string_view ntdll_loader_symbols[]{
    "TpAllocWork","TpAllocTimer","TpAllocWait","TpAllocPool","TpSetPoolMinThreads","TpSimpleTryPost","TpCallbackMayRunLong",
    "TpAllocCleanupGroup","TpQueryPoolStackInformation","TpSetPoolStackInformation",
    "LdrGetDllHandleEx","LdrAddRefDll","LdrDisableThreadCalloutsForDll","LdrFindEntryForAddress","LdrGetDllFullName",
    "LdrGetDllDirectory","LdrSetDllDirectory","LdrAddDllDirectory","LdrRemoveDllDirectory","LdrSetDefaultDllDirectories",
    "LdrFindResource_U","LdrAccessResource","LdrProcessRelocationBlock","LdrEnumerateLoadedModules","LdrQueryImageFileExecutionOptions",
    "DbgBreakPoint","DbgUserBreakPoint",
    "RtlLcidToLocaleName","RtlLocaleNameToLcid","RtlIsValidLocaleName","RtlGetUserPreferredUILanguages","RtlGetSystemPreferredUILanguages",
    "RtlGetThreadPreferredUILanguages","RtlIdnToAscii","RtlIdnToUnicode","RtlNormalizeString","RtlIsNormalizedString",
    "RtlGetProcessPreferredUILanguages","RtlSetProcessPreferredUILanguages","RtlSetThreadPreferredUILanguages","RtlIdnToNameprepUnicode",
    "RtlConvertSidToUnicodeString","RtlEncodeSystemPointer","RtlDecodeSystemPointer","RtlGetFullPathName_UEx","RtlGetLongestNtPathLength",
    "RtlDosPathNameToRelativeNtPathName_U","RtlDosPathNameToRelativeNtPathName_U_WithStatus","RtlReleaseRelativeName",
    "LdrGetDllPath","RtlGetSearchPath","RtlGetExePath","RtlReleasePath","LdrQueryProcessModuleInformation",
    "RtlQueryProcessPlaceholderCompatibilityMode","RtlSetProcessPlaceholderCompatibilityMode","RtlQueryThreadPlaceholderCompatibilityMode","RtlSetThreadPlaceholderCompatibilityMode"};
}
bool ntdll_loader_service(std::string_view symbol){return std::find(std::begin(ntdll_loader_symbols),std::end(ntdll_loader_symbols),symbol)!=std::end(ntdll_loader_symbols);}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_loader(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    constexpr std::uint32_t invalid_parameter=0xc000000dU,access_violation=0xc0000005U,no_memory=0xc0000017U,buffer_too_small=0xc0000023U,dll_not_found=0xc0000135U;
    const auto finish=[&](std::uint64_t value){last_error_=saved_error;call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto word=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
    const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){last_error_=0;return nested_service(state,memory,family,symbol,args);};
    const auto kernel=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return host("kernel32.dll",symbol,args);};
    const auto status_of=[&](std::uint64_t ok){return ok?0U:nt_status(last_error_);};
    const auto pointer_value=[&](std::uint64_t value){return x86_?value&UINT32_MAX:value;};
    // 暫存區：scratch 為最近一次配置的區塊；生命週期由共用 GuestScratch 管理。
    GuestScratch temporary(*this,memory,state);std::uint64_t scratch=0;
    const auto allocate=[&](std::size_t size)->bool{scratch=temporary.allocate(size);return scratch!=0;};
    // UNICODE_STRING 內容複製成 NUL 結尾字串。
    const auto terminated=[&](std::uint64_t counted)->std::uint64_t{
        const auto address=temporary.text(native_string(memory,counted,x86_));if(!address)throw NativeError{no_memory};return address;};
    const auto string_capacity=[&](std::uint64_t address){return std::uint32_t{read_counted_header(memory,address,x86_).capacity};};
    const auto string_buffer=[&](std::uint64_t address){return read_counted_header(memory,address,x86_).buffer;};
    // 寫入 UNICODE_STRING：與 Rtl 字串服務共用 store_counted_string（配置版以程序 heap 配置）。
    const auto store_string=[&](std::uint64_t address,const std::u16string& text,bool allocate_buffer,std::uint32_t overflow){
        return store_counted_string(memory,address,x86_,utf16_payload(text),2,allocate_buffer,overflow,[&](std::size_t size){return allocate_heap(memory,size);});};
    try{
        // 執行緒池：與 CreateThreadpool* 同一份物件與回呼契約。
        if(name=="TpAllocWork" || name=="TpAllocTimer" || name=="TpAllocWait"){
            if(!call(0))return finish(access_violation);
            const auto target=name=="TpAllocWork"?"CreateThreadpoolWork":name=="TpAllocTimer"?"CreateThreadpoolTimer":"CreateThreadpoolWait";
            const auto object=pointer_value(kernel(target,{call(1),call(2),call(3)}));
            if(!object)return finish(last_error_?nt_status(last_error_):no_memory);
            memory.store(GuestAddress{call(0)},bits,object);return finish(0);
        }
        if(name=="TpAllocPool"){
            if(!call(0))return finish(access_violation);
            const auto pool=pointer_value(kernel("CreateThreadpool",{call(1)}));if(!pool)return finish(last_error_?nt_status(last_error_):no_memory);
            memory.store(GuestAddress{call(0)},bits,pool);return finish(0);
        }
        if(name=="TpSetPoolMinThreads")return finish(status_of(kernel("SetThreadpoolThreadMinimum",{call(0),word(1)})));
        if(name=="TpAllocCleanupGroup"){
            if(!call(0))return finish(access_violation);const auto group=pointer_value(kernel("CreateThreadpoolCleanupGroup",{}));
            if(!group)return finish(last_error_?nt_status(last_error_):no_memory);memory.store(GuestAddress{call(0)},bits,group);return finish(0);
        }
        if(name=="TpQueryPoolStackInformation" || name=="TpSetPoolStackInformation")
            return finish(status_of(kernel(name=="TpQueryPoolStackInformation"?"QueryThreadpoolStackInformation":"SetThreadpoolStackInformation",{call(0),call(1)})));
        if(name=="TpSimpleTryPost")return finish(status_of(kernel("TrySubmitThreadpoolCallback",{call(0),call(1),call(2)})));
        if(name=="TpCallbackMayRunLong")return finish(kernel("CallbackMayRunLong",{call(0)})?0U:0xc0000129U); // STATUS_TOO_MANY_THREADS
        // 載入器
        if(name=="LdrGetDllHandleEx"){
            // (Flags, DllPath, DllCharacteristics*, DllName UNICODE_STRING*, DllHandle*)：1＝不改參考計數、2＝固定（Win32 旗標值相反）。
            const auto flags=word(0);if(flags&~3U)return finish(invalid_parameter);
            if(!call(3) || !call(4))return finish(invalid_parameter);
            const auto text=terminated(call(3));const auto out=temporary.allocate(8);if(!out)return finish(no_memory);
            const auto win32=((flags&1U)?2U:0U)|((flags&2U)?1U:0U);
            if(!kernel("GetModuleHandleExW",{win32,text,out})){memory.store(GuestAddress{call(4)},bits,0);return finish(dll_not_found);}
            memory.store(GuestAddress{call(4)},bits,memory.load(GuestAddress{out},bits));return finish(0);
        }
        if(name=="LdrAddRefDll"){
            // (Flags, DllHandle)：1＝固定在記憶體中。以位址取得模組並增加參考計數。
            const auto flags=word(0);if(flags&~1U)return finish(invalid_parameter);
            if(!allocate(8))return finish(no_memory);
            if(!kernel("GetModuleHandleExW",{4U|(flags?1U:0U),call(1),scratch}))return finish(dll_not_found);return finish(0);
        }
        if(name=="LdrDisableThreadCalloutsForDll")return finish(kernel("DisableThreadLibraryCalls",{call(0)})?0U:dll_not_found);
        if(name=="LdrFindEntryForAddress"){
            // PEB 載入串列（publish_loader_data）中包含該位址的 LDR_DATA_TABLE_ENTRY。
            if(!call(1))return finish(access_violation);
            publish_loader_data(memory);
            const auto* image=module_image(call(0),true);if(!image)return finish(0xc0000135U);
            const auto entry=runtime_.loader_entries.find(image->base);if(entry==runtime_.loader_entries.end())return finish(0x8000001aU); // STATUS_NO_MORE_ENTRIES
            memory.store(GuestAddress{call(1)},bits,entry->second);return finish(0);
        }
        if(name=="LdrGetDllFullName"){
            // (DllHandle 可為 NULL 表示主程式, FullDllName UNICODE_STRING*)
            if(!call(1))return finish(access_violation);
            const auto capacity=string_capacity(call(1))/2;
            const auto chars=static_cast<std::uint32_t>(kernel("GetModuleFileNameW",{call(0),string_buffer(call(1)),capacity}));
            if(!chars)return finish(dll_not_found);
            memory.store(GuestAddress{call(1)},16,std::min(chars,capacity)*2);
            return finish(chars>=capacity?buffer_too_small:0U);
        }
        if(name=="LdrGetDllDirectory"){
            if(!call(0))return finish(access_violation);
            const auto capacity=string_capacity(call(0))/2;
            const auto chars=static_cast<std::uint32_t>(kernel("GetDllDirectoryW",{capacity,string_buffer(call(0))}));
            if(chars>=capacity && chars){memory.store(GuestAddress{call(0)},16,(chars-1)*2);return finish(buffer_too_small);}
            memory.store(GuestAddress{call(0)},16,chars*2);return finish(0);
        }
        if(name=="LdrSetDllDirectory")return finish(status_of(kernel("SetDllDirectoryW",{call(0)?terminated(call(0)):0})));
        if(name=="LdrAddDllDirectory"){
            if(!call(0) || !call(1))return finish(invalid_parameter);
            const auto cookie=pointer_value(kernel("AddDllDirectory",{terminated(call(0))}));if(!cookie)return finish(nt_status(last_error_));
            memory.store(GuestAddress{call(1)},bits,cookie);return finish(0);
        }
        if(name=="LdrRemoveDllDirectory")return finish(status_of(kernel("RemoveDllDirectory",{call(0)})));
        if(name=="LdrSetDefaultDllDirectories")return finish(status_of(kernel("SetDefaultDllDirectories",{word(0)})));
        if(name=="LdrFindResource_U"){
            // (DllHandle, LDR_RESOURCE_INFO* {Type, Name, Language}, Level, IMAGE_RESOURCE_DATA_ENTRY**)：與 FindResourceEx 共用資源目錄走訪。
            if(!call(1) || !call(3))return finish(access_violation);
            const auto level=word(2);if(level!=3)return finish(invalid_parameter);
            const auto type=memory.load(GuestAddress{call(1)},bits),resource=memory.load(GuestAddress{call(1)+unit},bits);
            const auto language=static_cast<std::uint16_t>(memory.load(GuestAddress{call(1)+2*unit},bits));
            if(!resource_image(call(0)?call(0):runtime_.image.base))return finish(0xc0000089U); // STATUS_RESOURCE_DATA_NOT_FOUND
            auto entry=gui_resource(memory,call(0),resource,type,true,false,language?static_cast<int>(language):-1);
            // 指定語系不存在時依 Windows 的後備順序：中立、使用者預設、系統預設，最後取第一個語系。
            for(const int fallback:{0,0x400,0x800,-1}){if(entry || !language)break;entry=gui_resource(memory,call(0),resource,type,true,false,fallback);}
            if(!entry){
                if(!gui_resource(memory,call(0),resource,type,true,true,-1))return finish(0xc000008aU); // STATUS_RESOURCE_TYPE_NOT_FOUND
                return finish(gui_resource(memory,call(0),resource,type,true,false,-1)?0xc0000204U:0xc000008bU); // LANG／NAME_NOT_FOUND
            }
            memory.store(GuestAddress{call(3)},bits,entry);return finish(0);
        }
        if(name=="LdrAccessResource"){
            // (DllHandle, IMAGE_RESOURCE_DATA_ENTRY*, Resource**, Size*)：與 LoadResource／SizeofResource 相同，OffsetToData 為映像 RVA。
            const auto* image=resource_image(call(0)?call(0):runtime_.image.base);if(!image || !call(1))return finish(0xc0000089U);
            const auto rva=memory.load(GuestAddress{call(1)},32),size=memory.load(GuestAddress{call(1)+4},32);
            if(rva>image->size || size>image->size-rva)return finish(0xc0000089U);
            if(call(2))memory.store(GuestAddress{call(2)},bits,image->base+rva);if(call(3))memory.store(GuestAddress{call(3)},32,size);return finish(0);
        }
        if(name=="LdrProcessRelocationBlock"){
            // (Page, Count, Relocations USHORT*, Delta)：套用一個基底重定位區塊，回傳下一個區塊；未知型別回 NULL。
            const auto page=call(0),relocations=call(2),delta=call(3);const auto count=word(1);
            for(std::uint32_t i=0;i<count;++i){
                const auto item=static_cast<std::uint16_t>(memory.load(GuestAddress{relocations+i*2},16));const auto at=GuestAddress{page+(item&0xfffU)};
                switch(item>>12){
                case 0:break;
                case 1:memory.store(at,16,memory.load(at,16)+((delta>>16)&0xffffU));break;
                case 2:memory.store(at,16,memory.load(at,16)+(delta&0xffffU));break;
                case 3:memory.store(at,32,memory.load(at,32)+(delta&UINT32_MAX));break;
                case 10:memory.store(at,64,memory.load(at,64)+delta);break;
                default:return finish(0);
                }
            }
            return finish(relocations+std::uint64_t{count}*2);
        }
        if(name=="LdrQueryImageFileExecutionOptions"){
            // (ImageName UNICODE_STRING*, ValueName, Type, Buffer, BufferSize, ReturnedLength*)：讀 HKLM 的 IFEO 子機碼（取映像檔名部分）。
            if(!call(0) || !call(1))return finish(invalid_parameter);
            auto image=native_string(memory,call(0),x86_);if(const auto slash=image.find_last_of(u"\\/");slash!=image.npos)image.erase(0,slash+1);
            const auto path=u"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\"+image;
            if(!allocate(path.size()*2+2+32))return finish(no_memory);memory.write(GuestAddress{scratch},utf16_bytes(path));
            const auto key=scratch+path.size()*2+2,kind=key+8,length=key+16;
            const auto hklm=x86_?0x80000002ULL:0xffffffff80000002ULL;
            if(host("advapi32.dll","RegOpenKeyExW",{hklm,scratch,0,0x20019U,key}))return finish(0xc0000034U);
            const auto handle=memory.load(GuestAddress{key},bits);memory.store(GuestAddress{length},32,word(4));
            const auto error=static_cast<std::uint32_t>(host("advapi32.dll","RegQueryValueExW",{handle,call(1),0,kind,call(3),length}));
            (void)host("advapi32.dll","RegCloseKey",{handle});
            if(call(5))memory.store(GuestAddress{call(5)},32,memory.load(GuestAddress{length},32));
            if(error==2)return finish(0xc0000034U);if(error==234)return finish(buffer_too_small);if(error)return finish(nt_status(error));
            if(word(2) && memory.load(GuestAddress{kind},32)!=word(2))return finish(0xc0000024U); // STATUS_OBJECT_TYPE_MISMATCH
            return finish(0);
        }
        if(name=="LdrEnumerateLoadedModules"){
            // (Reserved, Callback(LDR_DATA_TABLE_ENTRY*, Context, BOOLEAN* Stop), Context)：依載入順序逐一回呼，Stop 設為 TRUE 時停止。
            if(call(0))return finish(invalid_parameter);if(!call(1))return finish(invalid_parameter);
            publish_loader_data(memory);
            std::vector<std::uint64_t> entries;
            for(const auto base:runtime_.loader_published)if(const auto found=runtime_.loader_entries.find(base);found!=runtime_.loader_entries.end())entries.push_back(found->second);
            const auto callback=call(1),context=call(2);
            return nested_continuation(state,memory,"LdrEnumerate",
        [this,service,entries,callback,context](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall inner(s,m,service);const auto stop=allocate_heap(m,8);
                    if(!stop){inner.return_to(s,0xc0000017U);co_return std::optional<std::uint32_t>{};}
                    for(const auto entry:entries){
                        m.store(GuestAddress{stop},8,0);(void)co_await service_callback::call(callback,{entry,context,stop});
                        if(m.load(GuestAddress{stop},8))break;
                    }
                    (void)free_heap(m,stop,s);inner.return_to(s,0);co_return std::optional<std::uint32_t>{};
                });
        }
        if(name=="DbgBreakPoint" || name=="DbgUserBreakPoint"){
            // 在呼叫者的返回位址引發 EXCEPTION_BREAKPOINT；處理常式以 EXCEPTION_CONTINUE_EXECUTION 繼續時從呼叫之後執行。
            last_error_=saved_error;auto caller=state;call.return_to(caller,0);
            begin_exception(caller,memory,0x80000003U,0,{});state=caller;return std::nullopt;
        }
        // 地區設定與 UI 語言
        if(name=="RtlLcidToLocaleName"){
            // (Lcid, LocaleName UNICODE_STRING*, Flags, BOOLEAN Allocate)：Flags 的 0x2 允許中立名稱。
            if(!call(1))return finish(invalid_parameter);
            if(!allocate(200))return finish(no_memory);
            const auto chars=static_cast<std::uint32_t>(kernel("LCIDToLocaleName",{word(0),scratch,85,(word(2)&2U)?0x08000000U:0U}));
            if(!chars)return finish(0xc00000efU); // STATUS_INVALID_PARAMETER_1
            return finish(store_string(call(1),read_utf16_counted(memory,GuestAddress{scratch},chars-1),(call(3)&0xffU)!=0,buffer_too_small));
        }
        if(name=="RtlLocaleNameToLcid"){
            // (LocaleName, Lcid*, Flags)：Flags 的 0x2 允許中立名稱。
            if(!call(0) || !call(1))return finish(invalid_parameter);
            const auto lcid=static_cast<std::uint32_t>(kernel("LocaleNameToLCID",{call(0),(word(2)&2U)?0x08000000U:0U}));
            if(!lcid)return finish(0xc00000efU);
            memory.store(GuestAddress{call(1)},32,lcid);return finish(0);
        }
        if(name=="RtlIsValidLocaleName")return finish(call(0) && kernel("IsValidLocaleName",{call(0)})?1U:0U);
        if(name=="RtlGetUserPreferredUILanguages" || name=="RtlGetSystemPreferredUILanguages" || name=="RtlGetThreadPreferredUILanguages"){
            // User／System：(Flags, Unknown, Count*, Buffer, BufferLength*)；Thread：(Flags, Count*, Buffer, BufferLength*)。
            const bool thread=name=="RtlGetThreadPreferredUILanguages";const unsigned first=thread?1U:2U;
            const auto target=thread?"GetThreadPreferredUILanguages":name=="RtlGetUserPreferredUILanguages"?"GetUserPreferredUILanguages":"GetSystemPreferredUILanguages";
            const auto ok=kernel(target,{word(0),call(first),call(first+1),call(first+2)});
            return finish(ok?0U:last_error_==122?buffer_too_small:nt_status(last_error_));
        }
        if(name=="RtlGetProcessPreferredUILanguages"){
            // (Flags, Count*, Buffer, BufferLength*)，與 kernel32 同一份程序偏好清單。
            const auto ok=kernel("GetProcessPreferredUILanguages",{word(0),call(1),call(2),call(3)});
            return finish(ok?0U:last_error_==122?buffer_too_small:nt_status(last_error_));
        }
        if(name=="RtlSetProcessPreferredUILanguages" || name=="RtlSetThreadPreferredUILanguages"){
            // (Flags, Languages, Count*)
            return finish(status_of(kernel(name.substr(3),{word(0),call(1),call(2)})));
        }
        if(name=="RtlIdnToAscii" || name=="RtlIdnToUnicode" || name=="RtlNormalizeString" || name=="RtlIdnToNameprepUnicode"){
            // IDN：(Flags, Source, SourceLength, Destination, DestinationLength*)；正規化：(Form, Source, SourceLength, Destination, DestinationLength*)。
            if(!call(4))return finish(invalid_parameter);
            const auto capacity=memory.load(GuestAddress{call(4)},32);
            const bool normalize=name=="RtlNormalizeString";
            const auto family=normalize?"kernel32.dll":"normaliz.dll";const auto target=normalize?"NormalizeString":name=="RtlIdnToAscii"?"IdnToAscii":name=="RtlIdnToNameprepUnicode"?"IdnToNameprepUnicode":"IdnToUnicode";
            auto result=static_cast<std::int32_t>(host(family,target,{word(0),call(1),word(2),capacity?call(3):0,capacity}));
            if(result>0){memory.store(GuestAddress{call(4)},32,static_cast<std::uint32_t>(result));return finish(0);}
            const auto error=last_error_;
            if(error==122){
                const auto needed=static_cast<std::int32_t>(host(family,target,{word(0),call(1),word(2),0,0}));
                memory.store(GuestAddress{call(4)},32,static_cast<std::uint32_t>(std::abs(needed)));return finish(buffer_too_small);
            }
            return finish(error==87?invalid_parameter:error==1113 || error==123?0xc0000716U:nt_status(error)); // STATUS_INVALID_IDN_NORMALIZATION
        }
        if(name=="RtlIsNormalizedString"){
            // (Form, Source, SourceLength, BOOLEAN* Normalized)
            if(!call(3))return finish(invalid_parameter);
            const auto ok=host("kernel32.dll","IsNormalizedString",{word(0),call(1),word(2)});
            if(!ok && last_error_)return finish(last_error_==87?invalid_parameter:0xc0000716U);
            memory.store(GuestAddress{call(3)},8,ok?1U:0U);return finish(0);
        }
        if(name=="RtlConvertSidToUnicodeString"){
            // (UnicodeString*, Sid, BOOLEAN Allocate)：與 ConvertSidToStringSidW 相同的 S-1-… 文字。
            if(!call(0) || !call(1))return finish(invalid_parameter);
            if(!allocate(8))return finish(no_memory);
            if(!host("advapi32.dll","ConvertSidToStringSidW",{call(1),scratch}))return finish(0xc0000078U); // STATUS_INVALID_SID
            const auto text_pointer=memory.load(GuestAddress{scratch},bits);const auto text=read_utf16(memory,GuestAddress{text_pointer},256);
            (void)host("kernel32.dll","LocalFree",{text_pointer});
            return finish(store_string(call(0),text,(call(2)&0xffU)!=0,0x80000005U));
        }
        if(name=="RtlEncodeSystemPointer" || name=="RtlDecodeSystemPointer"){
            // 系統範圍的指標編碼：以 KUSER_SHARED_DATA.Cookie（0x7FFE0330）異或並旋轉，跨程序結果一致；Decode 為逆運算。
            const auto cookie=memory.load(GuestAddress{0x7ffe0330U},32);const auto value=call(0);
            if(x86_){
                const auto shift=static_cast<int>(cookie&31U);const auto v=static_cast<std::uint32_t>(value),c=static_cast<std::uint32_t>(cookie);
                return finish(name=="RtlEncodeSystemPointer"?std::rotr(v^c,shift):std::rotl(v,shift)^c);
            }
            const auto shift=static_cast<int>(cookie&63U);
            return finish(name=="RtlEncodeSystemPointer"?std::rotr(value^cookie,shift):std::rotl(value,shift)^cookie);
        }
        if(name=="RtlGetFullPathName_UEx"){
            // (FileName, BufferLength 位元組, Buffer, FilePart*, RTL_PATH_TYPE*)：與 RtlGetFullPathName_U 同一流程，長度與錯誤改以 NTSTATUS 回報。
            if(!call(0))return finish(0xc0000033U);
            const auto bytes=static_cast<std::uint32_t>(host("ntdll.dll","RtlGetFullPathName_U",{call(0),word(1),call(2),call(3)}));
            if(call(4))memory.store(GuestAddress{call(4)},32,host("ntdll.dll","RtlDetermineDosPathNameType_U",{call(0)}));
            if(!bytes)return finish(0xc0000033U);
            return finish(bytes>word(1)?buffer_too_small:0U);
        }
        if(name=="RtlGetLongestNtPathLength")return finish(277); // "\??\UNC\" 的位元組數（16）＋ MAX_PATH（260）＋ 1
        if(name=="RtlDosPathNameToRelativeNtPathName_U" || name=="RtlDosPathNameToRelativeNtPathName_U_WithStatus"){
            // 與 RtlDosPathNameToNtPathName_U 相同的完整 NT 路徑；RelativeName 回報「不相對於目前目錄」（空名稱、無目錄 handle）。
            const bool with_status=name.ends_with("WithStatus");
            if(call(3))memory.write(GuestAddress{call(3)},std::vector<std::uint8_t>(x86_?16U:32U));
            const auto result=host("ntdll.dll",with_status?"RtlDosPathNameToNtPathName_U_WithStatus":"RtlDosPathNameToNtPathName_U",{call(0),call(1),call(2),0});
            return finish(result);
        }
        if(name=="RtlReleaseRelativeName"){
            // RTL_RELATIVE_NAME_U {UNICODE_STRING RelativeName; HANDLE ContainingDirectory; PVOID CurDirRef}
            if(call(0)){const auto handle=memory.load(GuestAddress{call(0)+2*unit},bits);if(handle)(void)kernel("CloseHandle",{handle});
                memory.write(GuestAddress{call(0)},std::vector<std::uint8_t>(x86_?16U:32U));}
            return finish(0);
        }
        // 搜尋路徑：LdrGetDllPath／RtlGetSearchPath 與載入器共用 dll_search_directories，RtlGetExePath 與 SearchPath 共用 default_search_paths，
        // 三者看到的目錄順序與實際搜尋一致。字串以程序 heap 配置（RtlReleasePath 釋放），各項去掉結尾的 \（根目錄除外），目前目錄寫成 "."。
        if(name=="LdrGetDllPath" || name=="RtlGetSearchPath" || name=="RtlGetExePath"){
            const unsigned output=name=="LdrGetDllPath"?2U:name=="RtlGetExePath"?1U:0U;
            if(!call(output))return finish(invalid_parameter);
            memory.store(GuestAddress{call(output)},bits,0);if(name=="LdrGetDllPath" && call(3))memory.store(GuestAddress{call(3)},bits,0);
            std::vector<std::u16string> entries;std::u16string environment_path;
            if(name=="RtlGetExePath"){
                // (ExecutableName, Path*)：預設順序中的目前目錄依 NeedCurrentDirectoryForExePath 決定是否保留；PATH 原樣接在最後。
                if(!call(0))return finish(invalid_parameter);
                std::size_t current=0;if(const auto error=default_search_paths(memory,entries,&current))return finish(nt_status(error));
                if(entries.size()>5){environment_path=std::move(entries.back());entries.pop_back();}
                if(executable_needs_current_directory(read_utf16(memory,GuestAddress{call(0)},32768)))entries[current].clear();
                else entries.erase(entries.begin()+static_cast<std::ptrdiff_t>(current));
            }else{
                // LdrGetDllPath(DllName, Flags, DllPath*, SearchPaths*)：Flags 為 LoadLibraryEx 的搜尋旗標；ALTERED_SEARCH_PATH 或 DLL_LOAD_DIR
                // 以 DllName 所在目錄搜尋相依 DLL；未指定搜尋旗標時套用 SetDefaultDllDirectories。RtlGetSearchPath 是傳統的標準順序。
                auto flags=name=="LdrGetDllPath"?word(1):0U;std::u16string directory;
                if(name=="LdrGetDllPath"){
                    if((flags&8U) && (flags&0x1f00U))return finish(invalid_parameter);
                    if((flags&0x108U) && call(0)){
                        auto module=read_utf16(memory,GuestAddress{call(0)},32768);std::replace(module.begin(),module.end(),u'/',u'\\');
                        if(const auto slash=module.find_last_of(u'\\');slash!=module.npos)directory=module.substr(0,slash+1);
                    }
                    if(!(flags&0x1f08U))flags|=runtime_.default_dll_directories;if(flags&0x1000U)flags=(flags&~0x1000U)|0xe00U;
                }
                entries=dll_search_directories(flags,directory);
            }
            std::u16string text;
            for(auto entry:entries){
                if(entry.empty())entry=u".";
                else if(entry.size()>1 && (entry.back()==u'\\' || entry.back()==u'/') && !(entry.size()==3 && entry[1]==u':'))entry.pop_back();
                if(!text.empty())text.push_back(u';');text+=entry;
            }
            if(!environment_path.empty()){if(!text.empty())text.push_back(u';');text+=environment_path;}
            const auto bytes=utf16_bytes(text);const auto buffer=allocate_heap(memory,bytes.size());if(!buffer)return finish(no_memory);
            memory.write(GuestAddress{buffer},bytes);memory.store(GuestAddress{call(output)},bits,buffer);return finish(0);
        }
        if(name=="RtlReleasePath"){if(call(0))(void)free_heap(memory,call(0),state);return finish(0);}
        if(name=="LdrQueryProcessModuleInformation"){
            // (RTL_PROCESS_MODULES*, Size, ReturnedSize*)：NumberOfModules 後接 RTL_PROCESS_MODULE_INFORMATION（x64 296、x86 284 位元組），
            // 依 PEB 載入順序列出映像基址、大小、LDR 旗標、載入計數、載入／初始化順序與 256 位元組 ANSI 完整路徑（OffsetToFileName 指向檔名）。
            publish_loader_data(memory);
            std::vector<std::uint64_t> entries;
            for(const auto base:runtime_.loader_published)if(const auto found=runtime_.loader_entries.find(base);found!=runtime_.loader_entries.end())entries.push_back(found->second);
            std::map<std::uint64_t,std::uint16_t> initialized;
            {const auto head=runtime_.loader_data+(x86_?0x1cU:0x30U);std::uint16_t index=0;
             for(auto node=memory.load(GuestAddress{head},bits);node!=head && index<4096;node=memory.load(GuestAddress{node},bits))initialized.emplace(node-4*unit,index++);}
            const unsigned record=x86_?284U:296U;const auto required=static_cast<std::uint32_t>(unit+entries.size()*record);
            if(call(2))memory.store(GuestAddress{call(2)},32,required);
            if(word(1)<required){if(call(0) && word(1)>=4)memory.store(GuestAddress{call(0)},32,entries.size());return finish(0xc0000004U);} // STATUS_INFO_LENGTH_MISMATCH
            if(!call(0))return finish(invalid_parameter);
            std::vector<std::uint8_t> bytes(required);
            const auto put=[&](std::size_t at,unsigned size,std::uint64_t value){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
            put(0,4,entries.size());
            for(std::size_t i=0;i<entries.size();++i){
                const auto entry=entries[i];const auto at=unit+i*record;
                const auto base=memory.load(GuestAddress{entry+6*unit},bits),size=memory.load(GuestAddress{entry+8*unit},32);
                const auto flags=memory.load(GuestAddress{entry+(x86_?0x34U:0x68U)},32),count=memory.load(GuestAddress{entry+(x86_?0x38U:0x6cU)},16);
                auto path=utf16_to_utf8(native_string(memory,entry+(x86_?0x24U:0x48U),x86_));if(path.size()>255)path.resize(255);
                const auto file=path.find_last_of("\\/");const auto file_at=file==path.npos?0U:static_cast<unsigned>(file+1);
                // Section、MappedBase、ImageBase、ImageSize、Flags、LoadOrderIndex、InitOrderIndex、LoadCount、OffsetToFileName、FullPathName。
                put(at+unit,unit,base);put(at+2*unit,unit,base);put(at+3*unit,4,size);put(at+3*unit+4,4,flags);
                const auto found=initialized.find(entry);
                put(at+3*unit+8,2,i);put(at+3*unit+10,2,found==initialized.end()?0:found->second);put(at+3*unit+12,2,count);put(at+3*unit+14,2,file_at);
                std::copy(path.begin(),path.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at+3*unit+16));
            }
            memory.write(GuestAddress{call(0)},bytes);return finish(0);
        }
        // Placeholder 相容模式（回傳 CHAR）：Set 回傳先前的模式；PHCM_ERROR_INVALID_PARAMETER＝-1、PHCM_ERROR_NO_TEB＝-2。程序可設 DISGUISE（1）
        // 或 EXPOSE（2），預設 EXPOSE；執行緒另可設 APPLICATION_DEFAULT（0，沿用程序）。沙箱檔案系統沒有雲端 placeholder，模式不改變檔案查詢結果。
        if(name.ends_with("PlaceholderCompatibilityMode")){
            const bool set=name.starts_with("RtlSet"),thread=name.find("Thread")!=name.npos;
            const auto mode=static_cast<std::int8_t>(call(0)&0xffU);
            const auto result=[&](std::int8_t value){return finish(static_cast<std::uint8_t>(value));};
            std::int8_t* target=&runtime_.placeholder_mode;
            if(thread){const auto found=threads_.items.find(threads_.current);if(found==threads_.items.end())return result(-2);target=&found->second.placeholder_mode;}
            if(!set)return result(*target);
            if(mode<(thread?0:1) || mode>2)return result(-1);
            const auto previous=*target;*target=mode;return result(previous);
        }
    }catch(const NativeError& error){return finish(error.status);}
    catch(const MemoryFault&){return finish(access_violation);}
    throw HostCallFault("未知的 ntdll 載入器服務");
}
