// 由 windows_wintrust.cpp 引入：Subject Interface Package（SIP）。內建 PE SIP 的簽章讀寫沿用 imagehlp 的憑證表服務，間接資料的雜湊沿用
// Authenticode 的 PE 映像雜湊（catalog_file_hash）；其他主體類型依登錄（HKLM\Software\Microsoft\Cryptography\OID\EncodingType 0\CryptSIPDll*\{GUID}）
// 載入提供者 DLL，以協程呼叫其 IsMyFileType／IsMyFileType2 與各入口。提供者 DLL 與位元數相關，登錄走程序預設檢視（x86 為 WOW6432Node）。檔案內容與 WinVerifyTrust 共用 trust_file_contents。
namespace {
using SipGuid=std::array<std::uint8_t,16>;
constexpr SipGuid sip_pe{0xb8,0xaa,0x89,0xc6,0x78,0x8e,0xd0,0x11,0x8c,0x47,0x00,0xc0,0x4f,0xc2,0x95,0xee};      // {C689AAB8-8E78-11D0-8C47-00C04FC295EE}
constexpr SipGuid sip_java{0xb9,0xaa,0x89,0xc6,0x78,0x8e,0xd0,0x11,0x8c,0x47,0x00,0xc0,0x4f,0xc2,0x95,0xee};    // {C689AAB9-...}
constexpr SipGuid sip_cab{0xba,0xaa,0x89,0xc6,0x78,0x8e,0xd0,0x11,0x8c,0x47,0x00,0xc0,0x4f,0xc2,0x95,0xee};     // {C689AABA-...}
constexpr SipGuid sip_flat{0x42,0x1a,0x35,0xde,0x59,0x8e,0xd0,0x11,0x8c,0x47,0x00,0xc0,0x4f,0xc2,0x95,0xee};    // {DE351A42-8E59-11D0-8C47-00C04FC295EE}
constexpr SipGuid sip_catalog{0x43,0x1a,0x35,0xde,0x59,0x8e,0xd0,0x11,0x8c,0x47,0x00,0xc0,0x4f,0xc2,0x95,0xee}; // {DE351A43-...}
constexpr SipGuid sip_ctl{0x3f,0x1d,0xa6,0x9b,0x3a,0xe7,0xd0,0x11,0x8c,0xd2,0x00,0xc0,0x4f,0xc2,0x95,0xee};     // {9BA61D3F-E73A-11D0-8CD2-00C04FC295EE}
constexpr std::uint32_t trust_subject_unknown=0x800b0003U,trust_no_signature=0x800b0100U,trust_bad_digest=0x80096010U,bad_algorithm=0x80090008U;
std::uint32_t le32(std::span<const std::uint8_t> bytes,std::size_t at){return std::uint32_t{bytes[at]}|std::uint32_t{bytes[at+1]}<<8|std::uint32_t{bytes[at+2]}<<16|std::uint32_t{bytes[at+3]}<<24;}
bool contains_bytes(std::span<const std::uint8_t> bytes,std::span<const std::uint8_t> needle,std::size_t limit){
    const auto end=std::min(bytes.size(),limit);if(needle.size()>end)return false;
    for(std::size_t i=0;i+needle.size()<=end;++i)if(std::equal(needle.begin(),needle.end(),bytes.begin()+static_cast<std::ptrdiff_t>(i)))return true;return false;
}
bool is_pe(std::span<const std::uint8_t> bytes){
    if(bytes.size()<0x40 || bytes[0]!='M' || bytes[1]!='Z')return false;const auto nt=le32(bytes,0x3c);return std::uint64_t{nt}+24<=bytes.size() && le32(bytes,nt)==0x4550U;
}
// 內建主體類型：PE、CAB（MSCF）、Java class、PKCS#7 簽署的 CTL（含目錄清單用途時為目錄檔）。
std::optional<SipGuid> builtin_subject(std::span<const std::uint8_t> bytes){
    if(is_pe(bytes))return sip_pe;
    if(bytes.size()>=4 && bytes[0]=='M' && bytes[1]=='S' && bytes[2]=='C' && bytes[3]=='F')return sip_cab;
    if(bytes.size()>=4 && bytes[0]==0xca && bytes[1]==0xfe && bytes[2]==0xba && bytes[3]==0xbe)return sip_java;
    constexpr std::array<std::uint8_t,11> signed_data{0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x07,0x02};      // 1.2.840.113549.1.7.2
    constexpr std::array<std::uint8_t,11> ctl{0x06,0x09,0x2b,0x06,0x01,0x04,0x01,0x82,0x37,0x0a,0x01};             // 1.3.6.1.4.1.311.10.1
    constexpr std::array<std::uint8_t,12> catalog_list{0x06,0x0a,0x2b,0x06,0x01,0x04,0x01,0x82,0x37,0x0c,0x01,0x01}; // 1.3.6.1.4.1.311.12.1.1
    if(bytes.size()>=2 && bytes[0]==0x30 && contains_bytes(bytes,signed_data,64) && contains_bytes(bytes,ctl,128))return contains_bytes(bytes,catalog_list,256)?sip_catalog:sip_ctl;
    return std::nullopt;
}
std::string sip_guid_text(const SipGuid& g){
    char text[39];std::snprintf(text,sizeof(text),"{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",g[3],g[2],g[1],g[0],g[5],g[4],g[7],g[6],g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);return text;
}
std::optional<SipGuid> sip_guid_parse(std::u16string_view text){
    const auto narrow=utf16_to_ansi(text);unsigned v[11]{};
    if(narrow.size()!=38 || std::sscanf(narrow.c_str(),"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10])!=11)return std::nullopt;
    SipGuid g{};for(unsigned i=0;i<4;++i)g[i]=static_cast<std::uint8_t>(v[0]>>(i*8));g[4]=static_cast<std::uint8_t>(v[1]);g[5]=static_cast<std::uint8_t>(v[1]>>8);
    g[6]=static_cast<std::uint8_t>(v[2]);g[7]=static_cast<std::uint8_t>(v[2]>>8);for(unsigned i=0;i<8;++i)g[8+i]=static_cast<std::uint8_t>(v[3+i]);return g;
}
// PE 憑證表（資料目錄 4，偏移為檔案位置）：WIN_CERTIFICATE {dwLength, wRevision, wCertificateType, bCertificate}，各項以 8 位元組對齊。
struct SipCertificate {std::size_t offset,length;std::uint16_t type;};
std::vector<SipCertificate> sip_certificates(std::span<const std::uint8_t> bytes){
    std::vector<SipCertificate> result;if(!is_pe(bytes))return result;
    const auto optional=std::size_t{le32(bytes,0x3c)}+24;if(optional+2>bytes.size())return result;
    const bool plus=(bytes[optional]|bytes[optional+1]<<8)==0x20b;const auto directory=optional+(plus?112U:96U)+4*8;
    if(directory+8>bytes.size())return result;const auto start=std::size_t{le32(bytes,directory)},size=std::size_t{le32(bytes,directory+4)};
    if(!start || !size || start+size>bytes.size())return result;
    for(auto at=start;at+8<=start+size;){
        const auto length=std::size_t{le32(bytes,at)};if(length<8 || at+length>start+size)break;
        result.push_back({at,length,static_cast<std::uint16_t>(bytes[at+6]|bytes[at+7]<<8)});at+=(length+7)&~std::size_t{7};
    }
    return result;
}
// SIP_SUBJECTINFO 的雜湊 OID → EVP 名稱。
std::optional<std::string> sip_digest_name(std::string_view oid){
    if(oid=="1.3.14.3.2.26")return "SHA1";if(oid=="2.16.840.1.101.3.4.2.1")return "SHA256";if(oid=="2.16.840.1.101.3.4.2.2")return "SHA384";
    if(oid=="2.16.840.1.101.3.4.2.3")return "SHA512";if(oid=="1.2.840.113549.2.5")return "MD5";return std::nullopt;
}
// SpcPeImageData：flags 為空的 BIT STRING，file 為 SpcLink [2] SpcString [0] BMPString "<<<Obsolete>>>"（與 Windows 簽章工具相同的編碼）。
std::vector<std::uint8_t> spc_pe_image_data(){
    std::vector<std::uint8_t> value{0x30,0x25,0x03,0x01,0x00,0xa0,0x20,0xa2,0x1e,0x80,0x1c};
    for(const char c:std::string_view("<<<Obsolete>>>")){value.push_back(0);value.push_back(static_cast<std::uint8_t>(c));}return value;
}
}
std::vector<std::uint8_t> WindowsServices::trust_file_contents(GuestMemory& memory,std::uint64_t handle,std::uint64_t path){
    std::shared_ptr<HostFile> native;std::shared_ptr<const std::vector<std::uint8_t>> snapshot;
    if(handle && handle!=(x86_?UINT32_MAX:UINT64_MAX)){
        const auto found=files_.find(handle);require(found!=files_.end(),6);require((found->second.access&0x80000001U)!=0,5);
        require(!found->second.directory->loaded && !(found->second.metadata.attributes&0x10) && !found->second.pipe && !found->second.serial && !found->second.hid,6);native=found->second.native;snapshot=found->second.bytes;
    }else{
        require(path);if(!runtime_.network.external_io)throw HostCallFault("compare 不允許 WinTrust 檔案 I/O");
        const auto opened=filesystem_.open_reader(utf16_to_utf8(read_utf16(memory,GuestAddress{path},32768)));require(!opened.error,opened.error);native=opened.file;
    }
    if(native){
        if(!runtime_.network.external_io)throw HostCallFault("compare 不允許 WinTrust 原生檔案 I/O");
        const auto info=native->information();require(!info.error,info.error);require(!(info.metadata.attributes&0x10),5);require(info.metadata.size<=256ULL*1024*1024,8);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.metadata.size));const auto read=native->read(bytes,0);require(!read.error,read.error);require(read.bytes==bytes.size(),38);return bytes;
    }
    require(bool(snapshot),6);return *snapshot;
}
std::optional<std::uint32_t> WindowsServices::invoke_sip(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](bool ok,std::uint32_t error=0){if(!ok)last_error_=error;arg.return_to(state,ok?1U:0U);return std::optional<std::uint32_t>{};};
    const auto load=[&](std::uint64_t base,unsigned at,unsigned width=32){return memory.load(GuestAddress{base+at},static_cast<std::uint8_t>(width));};
    const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> values){return nested_service(state,memory,family,symbol,values);};
    const auto invalid_handle=x86_?UINT64_C(0xffffffff):UINT64_MAX;
    GuestScratch scratch(*this,memory,state);
    const std::u16string sip_root=u"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDll";
    // 登錄的提供者：(DLL, 函式名稱)。
    const auto registered=[&](std::u16string_view function,const SipGuid& guid)->std::optional<std::pair<std::u16string,std::u16string>>{
        const auto key=scratch.text(sip_root+std::u16string(function)+u"\\"+utf8_to_utf16(sip_guid_text(guid)));
        const auto dll_name=scratch.text(u"Dll"),func_name=scratch.text(u"FuncName"),data=scratch.allocate(1040),size=scratch.allocate(4);
        if(!key || !dll_name || !func_name || !data || !size)throw std::bad_alloc();
        const auto read=[&](std::uint64_t field)->std::optional<std::u16string>{memory.store(GuestAddress{size},32,1040);
            if(host("advapi32.dll","RegGetValueW",{0x80000002U,key,field,2,0,data,size})&UINT32_MAX)return std::nullopt;return read_utf16(memory,GuestAddress{data},519);};
        const auto dll=read(dll_name),func=read(func_name);if(!dll || !func)return std::nullopt;return std::pair{*dll,*func};
    };
    // SIP_SUBJECTINFO：pgSubjectType、hFile、pwsFileName、DigestAlgorithm.pszObjId。
    const auto subject_bytes=[&](std::uint64_t info){require(info);return trust_file_contents(memory,load(info,x86_?8:16,bits),load(info,x86_?12:24,bits));};
    const auto subject_guid=[&](std::uint64_t info)->SipGuid{const auto pointer=load(info,x86_?4:8,bits);require(pointer);SipGuid g{};memory.read(GuestAddress{pointer},g);return g;};
    try{
        if(name=="CryptSIPRetrieveSubjectGuid" || name=="CryptSIPRetrieveSubjectGuidForCatalogFile"){
            // (FileName, hFileIn, pgSubject)：目錄雜湊用的版本只分 PE 與一般（flat）檔案。
            if(!arg(2) || (!arg(0) && (!arg(1) || arg(1)==invalid_handle)))return finish(false,87);
            const auto bytes=trust_file_contents(memory,arg(1)==invalid_handle?0:arg(1),arg(0));
            if(name.ends_with("CatalogFile")){memory.write(GuestAddress{arg(2)},is_pe(bytes)?sip_pe:sip_flat);return finish(true);}
            if(const auto builtin=builtin_subject(bytes)){memory.write(GuestAddress{arg(2)},*builtin);return finish(true);}
            // 其他類型：依序詢問登錄的 IsMyFileType2（檔名）與 IsMyFileType（handle）。
            std::vector<std::tuple<SipGuid,std::u16string,std::u16string,bool>> candidates;
            for(const auto [function,by_name]:{std::pair{std::u16string_view(u"IsMyFileType2"),true},std::pair{std::u16string_view(u"IsMyFileType"),false}}){
                const auto path=scratch.text(sip_root+std::u16string(function)),key=scratch.allocate(8),buffer=scratch.allocate(520),length=scratch.allocate(4);
                if(!path || !key || !buffer || !length)throw std::bad_alloc();
                if(host("advapi32.dll","RegOpenKeyExW",{0x80000002U,path,0,0x20019,key})&UINT32_MAX)continue;
                const auto handle=load(key,0,bits);
                for(std::uint32_t index=0;index<256;++index){memory.store(GuestAddress{length},32,260);
                    if(host("advapi32.dll","RegEnumKeyExW",{handle,index,buffer,length,0,0,0,0})&UINT32_MAX)break;
                    if(const auto guid=sip_guid_parse(read_utf16(memory,GuestAddress{buffer},260)))if(const auto provider=registered(function,*guid))candidates.emplace_back(*guid,provider->first,provider->second,by_name);}
                (void)host("advapi32.dll","RegCloseKey",{handle});
            }
            if(candidates.empty())return finish(false,trust_subject_unknown);
            const auto file_name=arg(0),file_handle=arg(1)==invalid_handle?0:arg(1),output=arg(2);
            return nested_continuation(state,memory,"SipSubject",
                [this,service,candidates,file_name,file_handle,output,invalid_handle](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall inner(s,m,service);const auto api=[&](std::string_view module,std::string_view symbol){return resolve(module,symbol).value;};
                    const auto guid_buffer=allocate_heap(m,16);if(!guid_buffer){last_error_=8;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                    std::uint64_t opened=0;
                    for(const auto& [guid,dll,function,by_name]:candidates){
                        if(by_name && !file_name)continue;
                        if(!by_name && !file_handle && !opened && file_name)opened=co_await service_callback::call_value(api("kernel32.dll","CreateFileW"),{file_name,0x80000000U,3,0,3,0,0});
                        const auto handle=file_handle?file_handle:opened;if(!by_name && (!handle || handle==invalid_handle))continue;
                        auto dll_bytes=utf16_bytes(dll);const auto dll_at=allocate_heap(m,dll_bytes.size());if(!dll_at)continue;m.write(GuestAddress{dll_at},dll_bytes);
                        const auto module=co_await service_callback::call_value(api("kernel32.dll","LoadLibraryW"),{dll_at});(void)free_heap(m,dll_at,s);if(!module)continue;
                        auto narrow=utf16_to_ansi(function);std::vector<std::uint8_t> name_bytes(narrow.begin(),narrow.end());name_bytes.push_back(0);
                        const auto name_at=allocate_heap(m,name_bytes.size());if(!name_at)continue;m.write(GuestAddress{name_at},name_bytes);
                        const auto entry=co_await service_callback::call_value(api("kernel32.dll","GetProcAddress"),{module,name_at});(void)free_heap(m,name_at,s);if(!entry)continue;
                        m.write(GuestAddress{guid_buffer},std::array<std::uint8_t,16>{});
                        if(co_await service_callback::call(entry,{by_name?file_name:handle,guid_buffer})){
                            std::array<std::uint8_t,16> reported{};m.read(GuestAddress{guid_buffer},reported);m.write(GuestAddress{output},reported==std::array<std::uint8_t,16>{}?guid:reported);
                            if(opened && opened!=invalid_handle)(void)co_await service_callback::call(api("kernel32.dll","CloseHandle"),{opened});
                            (void)free_heap(m,guid_buffer,s);inner.return_to(s,1);co_return std::optional<std::uint32_t>{};
                        }
                    }
                    if(opened && opened!=invalid_handle)(void)co_await service_callback::call(api("kernel32.dll","CloseHandle"),{opened});
                    (void)free_heap(m,guid_buffer,s);last_error_=trust_subject_unknown;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};
                });
        }
        if(name=="CryptSIPLoad"){
            // (pgSubject, dwFlags, SIP_DISPATCH_INFO*)：{cbSize, hSIP, pfGet, pfPut, pfCreate, pfVerify, pfRemove}。PE 使用內建入口，其他類型載入登錄的提供者。
            if(!arg(0) || !arg(2) || static_cast<std::uint32_t>(arg(1)))return finish(false,87);
            const auto dispatch=arg(2);require(load(dispatch,0)>=unit*7,87);
            SipGuid guid{};memory.read(GuestAddress{arg(0)},guid);
            if(guid==sip_pe){
                memory.store(GuestAddress{dispatch+unit},bits,0);unsigned slot=2;
                // wintrust 已載入時取其匯出位址（與 GetProcAddress 相同），否則使用服務入口。
                const auto module=find_module(u"wintrust.dll");std::vector<std::uint64_t> created;
                for(const auto* entry:{"CryptSIPGetSignedDataMsg","CryptSIPPutSignedDataMsg","CryptSIPCreateIndirectData","CryptSIPVerifyIndirectData","CryptSIPRemoveSignedDataMsg"})
                    memory.store(GuestAddress{dispatch+unit*slot++},bits,module?module_export(memory,module,entry,std::nullopt,created,0,&state):resolve("wintrust.dll",entry).value);
                return finish(true);
            }
            std::vector<std::pair<std::u16string,std::u16string>> entries;
            for(const auto* function:{u"GetSignedDataMsg",u"PutSignedDataMsg",u"CreateIndirectData",u"VerifyIndirectData",u"RemoveSignedDataMsg"}){
                const auto provider=registered(function,guid);if(!provider)return finish(false,trust_subject_unknown);entries.push_back(*provider);}
            return nested_continuation(state,memory,"SipLoad",[this,service,entries,dispatch,unit,bits](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall inner(s,m,service);const auto api=[&](std::string_view symbol){return resolve("kernel32.dll",symbol).value;};
                std::uint64_t first_module=0;unsigned slot=2;
                for(const auto& [dll,function]:entries){
                    const auto dll_bytes=utf16_bytes(dll);const auto dll_at=allocate_heap(m,dll_bytes.size());if(!dll_at){last_error_=8;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                    m.write(GuestAddress{dll_at},dll_bytes);const auto module=co_await service_callback::call_value(api("LoadLibraryW"),{dll_at});(void)free_heap(m,dll_at,s);
                    if(!module){inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}if(!first_module)first_module=module;
                    const auto narrow=utf16_to_ansi(function);std::vector<std::uint8_t> name_bytes(narrow.begin(),narrow.end());name_bytes.push_back(0);
                    const auto name_at=allocate_heap(m,name_bytes.size());if(!name_at){last_error_=8;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                    m.write(GuestAddress{name_at},name_bytes);const auto entry=co_await service_callback::call_value(api("GetProcAddress"),{module,name_at});(void)free_heap(m,name_at,s);
                    if(!entry){inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                    m.store(GuestAddress{dispatch+unit*slot++},bits,entry);
                }
                m.store(GuestAddress{dispatch+unit},bits,first_module);inner.return_to(s,1);co_return std::optional<std::uint32_t>{};
            });
        }
        if(name=="CryptSIPAddProvider" || name=="CryptSIPRemoveProvider"){
            // SIP_ADD_NEWPROVIDER {cbStruct, pgSubject, pwszDLLFileName, pwszMagicNumber, pwszIsFunctionName, Get, Put, Create, Verify, Remove, IsFunctionNameFmt2, GetCap}。
            constexpr std::array<std::u16string_view,8> functions{u"IsMyFileType",u"GetSignedDataMsg",u"PutSignedDataMsg",u"CreateIndirectData",u"VerifyIndirectData",u"RemoveSignedDataMsg",u"IsMyFileType2",u"GetCaps"};
            constexpr std::array<unsigned,8> slots{4,5,6,7,8,9,10,11};
            const bool adding=name=="CryptSIPAddProvider";const auto guid_at=adding?(arg(0)?load(arg(0),unit,bits):0):arg(0);
            if(!arg(0) || !guid_at)return finish(false,87);SipGuid guid{};memory.read(GuestAddress{guid_at},guid);const auto guid_text=utf8_to_utf16(sip_guid_text(guid));
            if(!adding){
                for(const auto function:functions){const auto key=scratch.text(sip_root+std::u16string(function)+u"\\"+guid_text);if(!key)throw std::bad_alloc();(void)host("advapi32.dll","RegDeleteKeyW",{0x80000002U,key});}
                return finish(true);
            }
            const auto size=static_cast<std::uint32_t>(load(arg(0),0));require(size>=unit*10,87);
            const auto text_at=[&](unsigned slot)->std::optional<std::u16string>{if(unit*(slot+1)>size)return std::nullopt;const auto pointer=load(arg(0),unit*slot,bits);
                if(!pointer)return std::nullopt;return read_utf16(memory,GuestAddress{pointer},520);};
            const auto dll=text_at(2);if(!dll || dll->empty())return finish(false,87);
            for(const unsigned required:{5U,6U,7U,8U,9U})if(!text_at(required))return finish(false,87);
            for(std::size_t i=0;i<functions.size();++i){
                const auto function=text_at(slots[i]);if(!function)continue;
                const auto key=scratch.text(sip_root+std::u16string(functions[i])+u"\\"+guid_text),handle=scratch.allocate(8),dll_name=scratch.text(u"Dll"),func_name=scratch.text(u"FuncName");
                if(!key || !handle || !dll_name || !func_name)throw std::bad_alloc();
                if(const auto status=host("advapi32.dll","RegCreateKeyExW",{0x80000002U,key,0,0,0,0xf003fU,0,handle,0})&UINT32_MAX)return finish(false,static_cast<std::uint32_t>(status));
                const auto opened=load(handle,0,bits);
                for(const auto& [field,value]:{std::pair{dll_name,*dll},std::pair{func_name,*function}}){
                    const auto bytes=utf16_bytes(value);const auto data=scratch.allocate(bytes.size());if(!data)throw std::bad_alloc();memory.write(GuestAddress{data},bytes);
                    (void)host("advapi32.dll","RegSetValueExW",{opened,field,0,1,data,bytes.size()});
                }
                (void)host("advapi32.dll","RegCloseKey",{opened});
            }
            return finish(true);
        }
        // 以下為 SIP 的五個入口：PE 主體使用內建實作；crypt32 的通用入口對其他主體轉送給登錄的提供者（wintrust 的入口只處理 PE）。
        const auto info=arg(0);require(info,87);require(load(info,0)>=(x86_?36U:64U),87);
        if(const auto guid=subject_guid(info);guid!=sip_pe){
            if(service.family!="crypt32.dll")return finish(false,trust_subject_unknown);
            const auto provider=registered(utf8_to_utf16(std::string(name.substr(8))),guid);if(!provider)return finish(false,trust_subject_unknown);
            std::vector<std::uint64_t> forwarded;for(unsigned i=0;i<service.arguments;++i)forwarded.push_back(arg(i));
            return nested_continuation(state,memory,"SipForward",[this,service,provider=*provider,forwarded](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall inner(s,m,service);const auto api=[&](std::string_view symbol){return resolve("kernel32.dll",symbol).value;};
                const auto dll_bytes=utf16_bytes(provider.first);const auto dll_at=allocate_heap(m,dll_bytes.size());if(!dll_at){last_error_=8;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                m.write(GuestAddress{dll_at},dll_bytes);const auto module=co_await service_callback::call_value(api("LoadLibraryW"),{dll_at});(void)free_heap(m,dll_at,s);
                if(!module){inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                const auto narrow=utf16_to_ansi(provider.second);std::vector<std::uint8_t> name_bytes(narrow.begin(),narrow.end());name_bytes.push_back(0);
                const auto name_at=allocate_heap(m,name_bytes.size());if(!name_at){last_error_=8;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                m.write(GuestAddress{name_at},name_bytes);const auto entry=co_await service_callback::call_value(api("GetProcAddress"),{module,name_at});(void)free_heap(m,name_at,s);
                if(!entry){last_error_=trust_subject_unknown;inner.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                const auto result=co_await service_callback::call(entry,forwarded);inner.return_to(s,result);co_return std::optional<std::uint32_t>{};
            });
        }
        if(name=="CryptSIPGetSignedDataMsg"){
            // (SubjectInfo, pdwEncodingType, dwIndex, pcbSignedDataMsg, pbSignedDataMsg)：取憑證表第 dwIndex 項的 PKCS#7；緩衝為 NULL 時只回報大小。
            require(arg(1) && arg(3),87);const auto bytes=subject_bytes(info);const auto certificates=sip_certificates(bytes);const auto index=static_cast<std::uint32_t>(arg(2));
            if(index>=certificates.size())return finish(false,trust_no_signature);const auto& entry=certificates[index];if(entry.type!=2)return finish(false,trust_no_signature);
            const auto length=static_cast<std::uint32_t>(entry.length-8);memory.store(GuestAddress{arg(1)},32,0x10001U);
            const auto capacity=static_cast<std::uint32_t>(load(arg(3),0));memory.store(GuestAddress{arg(3)},32,length);
            if(!arg(4))return finish(true);if(capacity<length)return finish(false,122);
            memory.write(GuestAddress{arg(4)},std::span(bytes).subspan(entry.offset+8,length));return finish(true);
        }
        // Put／Remove：交給 imagehlp 的憑證表服務；主體只有檔名時以讀寫權限開啟後關閉。
        const auto with_handle=[&](auto&& action)->std::optional<std::uint32_t>{
            auto handle=load(info,x86_?8:16,bits);std::uint64_t opened=0;
            if(!handle || handle==invalid_handle){const auto path=load(info,x86_?12:24,bits);require(path,87);
                opened=host("kernel32.dll","CreateFileW",{path,0xc0000000U,1,0,3,0x80,0});if(opened==invalid_handle || !opened)return finish(false,last_error_);handle=opened;}
            const auto ok=action(handle);const auto error=last_error_;
            if(opened)(void)host("kernel32.dll","CloseHandle",{opened});last_error_=error;return finish(ok,error);
        };
        if(name=="CryptSIPPutSignedDataMsg"){
            // (SubjectInfo, dwEncodingType, pdwIndex, cbSignedDataMsg, pbSignedDataMsg)：加入 WIN_CERTIFICATE（修訂 0x0200、類型 PKCS_SIGNED_DATA）。
            const auto length=static_cast<std::uint32_t>(arg(3));require(arg(2) && arg(4) && length && length<=16U*1024*1024,87);
            const auto certificate=scratch.allocate(8U+length);require(certificate,8);
            memory.store(GuestAddress{certificate},32,8U+length);memory.store(GuestAddress{certificate+4},16,0x0200);memory.store(GuestAddress{certificate+6},16,2);
            std::vector<std::uint8_t> data(length);memory.read(GuestAddress{arg(4)},data);memory.write(GuestAddress{certificate+8},data);
            return with_handle([&](std::uint64_t handle){return (host("imagehlp.dll","ImageAddCertificate",{handle,certificate,arg(2)})&UINT32_MAX)!=0;});
        }
        if(name=="CryptSIPRemoveSignedDataMsg")return with_handle([&](std::uint64_t handle){return (host("imagehlp.dll","ImageRemoveCertificate",{handle,static_cast<std::uint32_t>(arg(1))})&UINT32_MAX)!=0;});
        // 間接資料：SIP_INDIRECT_DATA {Data {pszObjId, Value}, DigestAlgorithm {pszObjId, Parameters}, Digest}。
        const unsigned struct_size=x86_?32U:64U;
        if(name=="CryptSIPCreateIndirectData"){
            // (SubjectInfo, pcbIndirectData, pIndirectData)：以 SubjectInfo 的 DigestAlgorithm 計算 PE 映像雜湊；pIndirectData 為 NULL 時只回報大小。
            require(arg(1),87);const auto oid_at=load(info,x86_?32:56,bits);require(oid_at,87);const auto oid=read_ansi(memory,oid_at,128);
            const auto algorithm=sip_digest_name(oid);if(!algorithm)return finish(false,bad_algorithm);
            const auto digest=catalog_file_hash(subject_bytes(info),*algorithm);const auto value=spc_pe_image_data();const std::string data_oid="1.3.6.1.4.1.311.2.1.15";
            const auto align=[](std::size_t value){return (value+7)&~std::size_t{7};};
            const std::size_t data_oid_at=struct_size,digest_oid_at=align(data_oid_at+data_oid.size()+1),value_at=align(digest_oid_at+oid.size()+1),digest_at=align(value_at+value.size());
            const auto required=static_cast<std::uint32_t>(digest_at+digest.size());
            const auto capacity=static_cast<std::uint32_t>(load(arg(1),0));memory.store(GuestAddress{arg(1)},32,required);
            if(!arg(2))return finish(true);if(capacity<required)return finish(false,122);
            const auto out=arg(2);std::vector<std::uint8_t> bytes(required);
            const auto put=[&](std::size_t at,unsigned width,std::uint64_t number){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(number>>(i*8));};
            std::copy(data_oid.begin(),data_oid.end(),bytes.begin()+static_cast<std::ptrdiff_t>(data_oid_at));std::copy(oid.begin(),oid.end(),bytes.begin()+static_cast<std::ptrdiff_t>(digest_oid_at));
            std::copy(value.begin(),value.end(),bytes.begin()+static_cast<std::ptrdiff_t>(value_at));std::copy(digest.begin(),digest.end(),bytes.begin()+static_cast<std::ptrdiff_t>(digest_at));
            put(0,unit,out+data_oid_at);put(unit,4,value.size());put(unit*2,unit,out+value_at);
            put(unit*3,unit,out+digest_oid_at);put(unit*4,4,0);put(unit*5,unit,0);
            put(unit*6,4,digest.size());put(unit*7,unit,out+digest_at);
            memory.write(GuestAddress{out},bytes);return finish(true);
        }
        if(name=="CryptSIPVerifyIndirectData"){
            // (SubjectInfo, pIndirectData)：以間接資料的雜湊演算法重算並比對。
            const auto data=arg(1);require(data,87);const auto oid_at=load(data,unit*3,bits);require(oid_at,87);
            const auto algorithm=sip_digest_name(read_ansi(memory,oid_at,128));if(!algorithm)return finish(false,bad_algorithm);
            const auto expected_size=static_cast<std::uint32_t>(load(data,unit*6));const auto expected_at=load(data,unit*7,bits);require(expected_at || !expected_size,87);
            std::vector<std::uint8_t> expected(expected_size);if(expected_size)memory.read(GuestAddress{expected_at},expected);
            return finish(catalog_file_hash(subject_bytes(info),*algorithm)==expected,trust_bad_digest);
        }
        throw HostCallFault("未知的 SIP 服務");
    }catch(const Error& error){return finish(false,error.code);}
    catch(const MemoryFault&){return finish(false,87);}
    catch(const std::bad_alloc&){return finish(false,8);}
    catch(const std::invalid_argument&){return finish(false,bad_algorithm);}
    catch(const std::runtime_error&){ERR_clear_error();return finish(false,trust_subject_unknown);} // PE 結構不合法
}
