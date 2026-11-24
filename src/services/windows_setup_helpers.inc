        // SetupAPI 的公用輔助函式（舊版私有匯出）：權限、檔案對應、登錄、檔案安全性、目錄檔安裝、INF 欄位指標、斷言與延後移動。
        // 除 AssertFail 與 InstallCatalog 為 ANSI 外都是寬字元；以 nested_service 呼叫既有的 kernel32／advapi32／wintrust 服務，沿用其驗證與副作用。
        if(name=="AssertFail" || name=="DelayedMove" || name=="DoesUserHavePrivilege" || name=="EnablePrivilege" || name=="OpenAndMapFileForRead" || name=="UnmapAndCloseFile" ||
           name=="QueryRegistryValue" || name=="RegistryDelnode" || name=="RetreiveFileSecurity" || name=="StampFileSecurity" || name=="TakeOwnershipOfFile" ||
           name=="pSetupGetField" || name=="InstallCatalog" || name=="pSetupInstallCatalog"){
            GuestScratch scratch(*this,memory,state);
            const auto call_host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> values){return nested_service(state,memory,family,symbol,values);};
            const auto guest_text=[&](const std::u16string& value){const auto at=scratch.text(value);if(!at)throw std::bad_alloc{};return at;};
            const auto block=[&](std::size_t size){const auto at=scratch.allocate(size);if(!at)throw std::bad_alloc{};memory.fill(GuestAddress{at},0,size);return at;};
            const auto wide_arg=[&](unsigned index){return arg(index)?read_utf16(memory,GuestAddress{arg(index)},32768):std::u16string{};};
            const auto ansi_arg=[&](unsigned index){return arg(index)?ansi_to_utf16_lossy(read_ansi(memory,arg(index),32768)):std::u16string{};};
            const auto error_code=[&](std::uint32_t error)->std::optional<std::uint32_t>{last_error_=error;return finish(error);};
            const auto current_process=invalid_handle;
            // 目前程序權杖（TOKEN_QUERY 8、TOKEN_ADJUST_PRIVILEGES 0x20）。
            const auto open_token=[&](std::uint32_t access)->std::uint64_t{const auto at=block(unit);if(!call_host("advapi32.dll","OpenProcessToken",{current_process,access,at}))return 0;return memory.load(GuestAddress{at},width);};
            const auto token_information=[&](std::uint64_t token,std::uint32_t kind)->std::uint64_t{
                const auto size_at=block(4);(void)call_host("advapi32.dll","GetTokenInformation",{token,kind,0,0,size_at});const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{size_at},32));
                if(!size || size>65536)return 0;const auto data=block(size);return call_host("advapi32.dll","GetTokenInformation",{token,kind,data,size,size_at})?data:0;};
            const auto privilege_luid=[&](const std::u16string& privilege)->std::optional<std::uint64_t>{
                const auto luid=block(8);if(!call_host("advapi32.dll","LookupPrivilegeValueW",{0,guest_text(privilege),luid}))return std::nullopt;return memory.load(GuestAddress{luid},64);};
            if(name=="DoesUserHavePrivilege"){
                // (PrivilegeName)：權杖的權限清單中有此權限（不論是否已啟用）時回 TRUE。
                const auto token=open_token(8);if(!token)return fail(last_error_);
                const auto luid=privilege_luid(wide_arg(0));const auto privileges=luid?token_information(token,3):0;const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{token});
                if(!luid || !privileges)return fail(error?error:1313);
                const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{privileges},32));
                for(std::uint32_t i=0;i<count;++i)if(memory.load(GuestAddress{privileges+4+i*12},64)==*luid)return succeed();
                return fail(0);
            }
            if(name=="EnablePrivilege"){
                // (PrivilegeName, Enable)：AdjustTokenPrivileges 只設定這一項；權杖沒有此權限（ERROR_NOT_ALL_ASSIGNED）時回 FALSE。
                const auto token=open_token(0x28);if(!token)return fail(last_error_);
                const auto luid=privilege_luid(wide_arg(0));if(!luid){const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{token});return fail(error);}
                const auto privileges=block(16);memory.store(GuestAddress{privileges},32,1);memory.store(GuestAddress{privileges+4},64,*luid);memory.store(GuestAddress{privileges+12},32,(arg(1)&0xffffffffU)?2U:0U);
                const bool adjusted=call_host("advapi32.dll","AdjustTokenPrivileges",{token,0,privileges,0,0,0})!=0;const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{token});
                if(!adjusted || error)return fail(adjusted?error:error?error:1300);return succeed();
            }
            if(name=="OpenAndMapFileForRead"){
                // (FileName, Size*, File*, Mapping*, Buffer*)：唯讀開啟並整個對應到記憶體，回傳 Win32 錯誤碼；以 UnmapAndCloseFile 釋放。
                if(!arg(0) || !arg(1) || !arg(2) || !arg(3) || !arg(4))return error_code(87);
                const auto file=call_host("kernel32.dll","CreateFileW",{guest_text(wide_arg(0)),0x80000000U,1,0,3,0,0});if(file==invalid_handle || !file)return error_code(last_error_);
                const auto size=call_host("kernel32.dll","GetFileSize",{file,0})&0xffffffffU;
                if(size==0xffffffffU){const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{file});return error_code(error?error:5);}
                const auto mapping=call_host("kernel32.dll","CreateFileMappingW",{file,0,2,0,0,0});
                if(!mapping){const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{file});return error_code(error);}
                const auto view=call_host("kernel32.dll","MapViewOfFile",{mapping,4,0,0,0});
                if(!view){const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{mapping});(void)call_host("kernel32.dll","CloseHandle",{file});return error_code(error);}
                memory.check(GuestAddress{arg(1)},4,Access::Write);memory.store(GuestAddress{arg(1)},32,size);
                for(const auto& [at,value]:{std::pair{arg(2),file},std::pair{arg(3),mapping},std::pair{arg(4),view}}){memory.check(GuestAddress{at},unit,Access::Write);memory.store(GuestAddress{at},width,value);}
                return error_code(0);
            }
            if(name=="UnmapAndCloseFile"){
                // (File, Mapping, Buffer)
                const bool unmapped=call_host("kernel32.dll","UnmapViewOfFile",{arg(2)})!=0;const auto first=last_error_;
                const bool mapping=call_host("kernel32.dll","CloseHandle",{arg(1)})!=0;const bool file=call_host("kernel32.dll","CloseHandle",{arg(0)})!=0;
                if(!unmapped)return fail(first);if(!mapping || !file)return fail(last_error_?last_error_:6);return succeed();
            }
            if(name=="QueryRegistryValue"){
                // (Key, ValueName, Data*, Type*, Size*)：以 MyMalloc 配置（多 2 個位元組的 NUL），回傳 Win32 錯誤碼；呼叫端以 MyFree 釋放。
                if(!arg(2) || !arg(3) || !arg(4))return error_code(87);
                const auto value_name=arg(1)?guest_text(wide_arg(1)):0;const auto type_at=block(4),size_at=block(4);
                if(const auto error=static_cast<std::uint32_t>(call_host("advapi32.dll","RegQueryValueExW",{arg(0),value_name,0,type_at,0,size_at})))return error_code(error);
                const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{size_at},32));if(size>16U*1024U*1024U)return error_code(8);
                const auto data=call_host("setupapi.dll","MyMalloc",{size+2});if(!data)return error_code(8);memory.fill(GuestAddress{data},0,size+2);
                if(const auto error=static_cast<std::uint32_t>(call_host("advapi32.dll","RegQueryValueExW",{arg(0),value_name,0,type_at,data,size_at}))){(void)call_host("setupapi.dll","MyFree",{data});return error_code(error);}
                memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.store(GuestAddress{arg(2)},width,data);
                memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(3)},32,memory.load(GuestAddress{type_at},32));
                memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,memory.load(GuestAddress{size_at},32));
                return error_code(0);
            }
            if(name=="RegistryDelnode"){
                // (RootKey, SubKeyName)：刪除子機碼及其下的全部子機碼與值；不存在時成功。
                if(!arg(1))return fail(87);const auto subkey=guest_text(wide_arg(1));
                auto error=static_cast<std::uint32_t>(call_host("advapi32.dll","RegDeleteTreeW",{arg(0),subkey}));if(error==2)error=0;
                if(!error){error=static_cast<std::uint32_t>(call_host("advapi32.dll","RegDeleteKeyW",{arg(0),subkey}));if(error==2)error=0;}
                return error?fail(error):succeed();
            }
            if(name=="RetreiveFileSecurity" || name=="StampFileSecurity" || name=="TakeOwnershipOfFile"){
                // Retreive(FileName, SECURITY_DESCRIPTOR**)：擁有者、群組與 DACL，以 MyMalloc 配置；Stamp(FileName, SECURITY_DESCRIPTOR*)：套用同三項；
                // TakeOwnershipOfFile(FileName)：擁有者改為目前權杖的使用者。三者都回傳 Win32 錯誤碼。
                if(!arg(0))return error_code(87);const auto file=guest_text(wide_arg(0));
                if(name=="StampFileSecurity"){if(!arg(1))return error_code(87);return error_code(call_host("advapi32.dll","SetFileSecurityW",{file,7,arg(1)})?0:last_error_);}
                if(name=="TakeOwnershipOfFile"){
                    const auto token=open_token(8);if(!token)return error_code(last_error_);const auto user=token_information(token,1);const auto error=last_error_;(void)call_host("kernel32.dll","CloseHandle",{token});
                    if(!user)return error_code(error?error:1312);
                    return error_code(static_cast<std::uint32_t>(call_host("advapi32.dll","SetNamedSecurityInfoW",{file,1,1,memory.load(GuestAddress{user},width),0,0,0})));
                }
                if(!arg(1))return error_code(87);const auto needed_at=block(4);
                if(call_host("advapi32.dll","GetFileSecurityW",{file,7,0,0,needed_at})|| last_error_!=122)return error_code(last_error_?last_error_:13);
                const auto needed=static_cast<std::uint32_t>(memory.load(GuestAddress{needed_at},32));if(!needed || needed>1048576)return error_code(13);
                const auto descriptor=call_host("setupapi.dll","MyMalloc",{needed});if(!descriptor)return error_code(8);
                if(!call_host("advapi32.dll","GetFileSecurityW",{file,7,descriptor,needed,needed_at})){const auto error=last_error_;(void)call_host("setupapi.dll","MyFree",{descriptor});return error_code(error);}
                memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.store(GuestAddress{arg(1)},width,descriptor);return error_code(0);
            }
            if(name=="pSetupGetField"){
                // (Context, FieldIndex)：回傳欄位文字的指標（0 為鍵；與 SetupGetStringField 同規則，字串代換後保存到 INF 關閉為止）；超出範圍回 NULL 與 ERROR_INVALID_PARAMETER。
                if(!arg(0))return fail(87);const auto context=read_context(arg(0));auto [handle,file,line]=context_line(context);const auto index=static_cast<std::uint32_t>(arg(1));
                std::optional<std::u16string> text;
                if(index==0)text=line.keyed?field_text(handle,file,line.key):line.fields.empty()?std::u16string{}:field_text(handle,file,line.fields.front());
                else if(index<=line.fields.size())text=field_text(handle,file,line.fields[index-1]);
                if(!text)return fail(87);
                const auto key=std::tuple{context.file,context.section,context.line,index};
                if(const auto found=handle.field_cache.find(key);found!=handle.field_cache.end())return succeed(found->second);
                const auto bytes=utf16_bytes(*text);const auto at=allocate_heap(memory,bytes.size());if(!at)return fail(8);memory.write(GuestAddress{at},bytes);
                handle.field_cache.emplace(key,at);return succeed(at);
            }
            if(name=="InstallCatalog" || name=="pSetupInstallCatalog"){
                // (CatalogFile, DestFileName, FullPath)：以驅動程式驗證子系統把簽署目錄加入目錄資料庫，FullPath（MAX_PATH）取得安裝後的路徑；回傳 Win32 錯誤碼。InstallCatalog 為 ANSI。
                const bool narrow=name=="InstallCatalog";if(!arg(0))return error_code(87);
                const auto catalog=narrow?ansi_arg(0):wide_arg(0),destination=narrow?ansi_arg(1):wide_arg(1);
                static constexpr std::array<std::uint8_t,16> driver_verify{0xc3,0xe6,0x50,0xf7,0xee,0x38,0xd1,0x11,0x85,0xe5,0,0xc0,0x4f,0xc2,0x95,0xee};
                const auto guid=block(16);memory.write(GuestAddress{guid},driver_verify);const auto admin_at=block(unit);
                if(!call_host("wintrust.dll","CryptCATAdminAcquireContext",{admin_at,guid,0}))return error_code(last_error_);
                const auto admin=memory.load(GuestAddress{admin_at},width);
                const auto info=call_host("wintrust.dll","CryptCATAdminAddCatalog",{admin,guest_text(catalog),destination.empty()?0:guest_text(destination),0});
                std::uint32_t error=info?0:last_error_;
                if(info && arg(2)){
                    const auto catalog_info=block(524);memory.store(GuestAddress{catalog_info},32,524);
                    if(call_host("wintrust.dll","CryptCATCatalogInfoFromContext",{info,catalog_info,0})){
                        const auto path=read_utf16(memory,GuestAddress{catalog_info+4},260);auto bytes=narrow?std::vector<std::uint8_t>{}:utf16_bytes(path);
                        if(narrow){const auto ansi=utf16_to_ansi(path);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}
                        if(bytes.size()>260U*(narrow?1U:2U))error=122;else{memory.check(GuestAddress{arg(2)},bytes.size(),Access::Write);memory.write(GuestAddress{arg(2)},bytes);}
                    }else error=last_error_;
                }
                if(info)(void)call_host("wintrust.dll","CryptCATAdminReleaseCatalogContext",{admin,info,0});
                (void)call_host("wintrust.dll","CryptCATAdminReleaseContext",{admin,0});
                return error_code(error);
            }
            if(name=="DelayedMove"){
                // (ExistingFileName, NewFileName)：在 Session Manager 的 PendingFileRenameOperations（REG_MULTI_SZ）加入「\??\來源」與「!\??\目的」（目的為 NULL 時為空字串，表示刪除）。
                // 與 MoveFileEx 的 MOVEFILE_DELAY_UNTIL_REBOOT 相同需要寫入 HKLM 的權限；沙箱沒有重新開機流程，項目保留在登錄中。
                if(!arg(0))return fail(87);
                const auto source=u"\\??\\"+full_path(wide_arg(0));const auto target=arg(1)?u"!\\??\\"+full_path(wide_arg(1)):std::u16string{};
                const auto key_at=block(unit);const std::uint64_t machine=x86_?0x80000002ULL:0xffffffff80000002ULL;
                if(const auto error=static_cast<std::uint32_t>(call_host("advapi32.dll","RegCreateKeyExW",{machine,guest_text(u"SYSTEM\\CurrentControlSet\\Control\\Session Manager"),0,0,0,0x20006,0,key_at,0})))return fail(error);
                const auto key=memory.load(GuestAddress{key_at},width);const auto value_name=guest_text(u"PendingFileRenameOperations");
                std::u16string existing;const auto size_at=block(4);
                if(!call_host("advapi32.dll","RegQueryValueExW",{key,value_name,0,0,0,size_at})){
                    const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{size_at},32));
                    if(size && size<=1048576){const auto data=block(size+2);if(!call_host("advapi32.dll","RegQueryValueExW",{key,value_name,0,0,data,size_at}))
                        for(std::uint32_t i=0;i+1<size;i+=2)existing.push_back(static_cast<char16_t>(memory.load(GuestAddress{data+i},16)));}
                }
                while(!existing.empty() && existing.back()==0)existing.pop_back();if(!existing.empty())existing.push_back(0);
                existing+=source;existing.push_back(0);existing+=target;existing.push_back(0);existing.push_back(0);
                auto bytes=utf16_bytes(existing);bytes.resize(existing.size()*2);const auto data=block(bytes.size());memory.write(GuestAddress{data},bytes);
                const auto error=static_cast<std::uint32_t>(call_host("advapi32.dll","RegSetValueExW",{key,value_name,0,7,data,bytes.size()}));(void)call_host("advapi32.dll","RegCloseKey",{key});
                return error?fail(error):succeed();
            }
            if(name=="AssertFail"){
                // (FileName, Line, Message)：輸出偵錯字串；互動模式另以 MessageBoxA（是／否）詢問是否呼叫 DebugBreak。
                const auto text=std::string("Assertion failure at line ")+std::to_string(static_cast<std::uint32_t>(arg(1)))+" in file "+(arg(0)?read_ansi(memory,arg(0),1024):std::string("(null)"))+": "+(arg(2)?read_ansi(memory,arg(2),4096):std::string{});
                const auto debug_text=scratch.allocate(text.size()+3);if(!debug_text)throw std::bad_alloc{};
                memory.write(GuestAddress{debug_text},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));memory.write(GuestAddress{debug_text+text.size()},std::array<std::uint8_t,3>{'\r','\n',0});
                (void)call_host("kernel32.dll","OutputDebugStringA",{debug_text});
                if(setup_inf_->global_flags&4U){last_error_=0;return finish(0);}
                const auto prompt=text+"\n\nCall DebugBreak()?";const auto box=resolve("user32.dll","MessageBoxA").value,brk=resolve("kernel32.dll","DebugBreak").value;
                return nested_continuation(state,memory,"SetupAssertFail",[this,service,box,brk,prompt](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall inner(s,m,service);const auto message=allocate_heap(m,prompt.size()+1),caption=allocate_heap(m,16);if(!message || !caption)throw std::bad_alloc{};
                    m.write(GuestAddress{message},std::span(reinterpret_cast<const std::uint8_t*>(prompt.data()),prompt.size()));m.store(GuestAddress{message+prompt.size()},8,0);
                    static constexpr std::array<std::uint8_t,8> title{'S','e','t','u','p','A','P','I'};m.write(GuestAddress{caption},title);m.store(GuestAddress{caption+8},8,0);
                    const auto pressed=co_await service_callback::call(box,{0,message,caption,0x00012014U}); // MB_YESNO｜MB_ICONSTOP｜MB_TASKMODAL｜MB_SETFOREGROUND
                    (void)free_heap(m,message,s);(void)free_heap(m,caption,s);
                    if(pressed==6)(void)co_await service_callback::call(brk,{});
                    inner.return_to(s,0);co_return std::optional<std::uint32_t>{};});
            }
        }
