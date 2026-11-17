// shell32 公用函式（以既有 COM、設定檔、登錄與 ShellExecuteEx 服務轉接；HRESULT 依 Windows 文件）：
// - SHCreateStdEnumFmtEtc：複製 FORMATETC 陣列（含 ptd 的 DVTARGETDEVICE）成 IEnumFORMATETC，與 OLE 剪貼簿資料物件共用同一個列舉物件。
// - SHCLSIDFromString：CLSIDFromString。SHCoCreateInstance：字串 CLSID 優先、否則用 pclsid，以 CLSCTX_INPROC_SERVER 呼叫 CoCreateInstance。
// - SHGetLocalizedName／SHSetLocalizedName／SHRemoveLocalizedName：資料夾 desktop.ini 的 [.ShellClassInfo] LocalizedResourceName＝@模組,-資源識別碼；
//   設定時 desktop.ini 設為隱藏＋系統、資料夾加上唯讀屬性（PathMakeSystemFolder 的預設）；沒有名稱時 HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)。
// - SHSetUnreadMailCountW／SHGetUnreadMailCountW／SHEnumerateUnreadMailAccountsW：HKCU（或指定的使用者機碼）\Software\Microsoft\Windows\CurrentVersion\UnreadMail\<帳號>
//   的 MessageCount（DWORD）、TimeStamp（FILETIME 二進位）與 Application（字串）。不指定帳號時取得的是時間戳晚於 pFileTime 的帳號未讀數總和。
// - ShellExec_RunDLL（A／W；無後綴為 ANSI）：rundll32 進入點；命令列可先接「?十六進位旗標?」作為 SEE_MASK 旗標，接著是檔案（可加引號）與參數，以 ShellExecuteExW 執行。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <bit>
#include <ctime>

namespace x64arm {
namespace {
constexpr std::uint32_t e_invalidarg=0x80070057U,e_pointer=0x80004003U,e_outofmemory=0x8007000eU,e_fail=0x80004005U;
constexpr std::uint32_t hresult_win32(std::uint32_t error){return error?0x80070000U|(error&0xffffU):0U;}
}
std::span<const WindowsServiceDescriptor> shell_utility_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","SHCreateStdEnumFmtEtc",3,0,0,0x1},{"shell32.dll","SHCLSIDFromString",2},{"shell32.dll","SHCoCreateInstance",5},
        {"shell32.dll","SHGetLocalizedName",4,0,0,0x4},{"shell32.dll","SHSetLocalizedName",3,0,0,0x4},{"shell32.dll","SHRemoveLocalizedName",1},
        {"shell32.dll","SHSetUnreadMailCountW",3,0,0,0x2},{"shell32.dll","SHGetUnreadMailCountW",6,0,0,0x20},{"shell32.dll","SHEnumerateUnreadMailAccountsW",4,0,0,0xa},
        {"shell32.dll","ShellExec_RunDLL",4,0,0,0x8},{"shell32.dll","ShellExec_RunDLLA",4,0,0,0x8},{"shell32.dll","ShellExec_RunDLLW",4,0,0,0x8}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_utilities(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="SHCreateStdEnumFmtEtc"){
        const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto count=static_cast<std::uint32_t>(arg(0));const auto items=arg(1),out=arg(2);
        if(!out)return finish(e_invalidarg);
        try{
            memory.check(GuestAddress{out},unit,Access::Write);memory.store(GuestAddress{out},bits,0);
            if(count>65536 || (count && !items))return finish(e_invalidarg);
            // FORMATETC：cfFormat（WORD）、ptd、dwAspect、lindex、tymed；x86 20、x64 32 位元組。
            const unsigned stride=x86_?20U:32U;GuiClipboardObject enumeration;enumeration.enumerator=true;
            for(std::uint32_t i=0;i<count;++i){
                const auto at=items+std::uint64_t{i}*stride;GuiClipboardEntry entry;
                entry.format.format=static_cast<std::uint32_t>(memory.load(GuestAddress{at},16));
                const auto device=memory.load(GuestAddress{at+unit},bits);
                entry.format.aspect=static_cast<std::uint32_t>(memory.load(GuestAddress{at+2*unit},32));
                entry.format.index=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+2*unit+4},32)));
                entry.format.tymed=static_cast<std::uint32_t>(memory.load(GuestAddress{at+2*unit+8},32));
                if(device){const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{device},32));if(size<12 || size>65536)return finish(e_invalidarg);
                    entry.format.device.resize(size);memory.read(GuestAddress{device},entry.format.device);}
                enumeration.entries.push_back(std::move(entry));
            }
            const auto object=create_clipboard_object(memory,std::move(enumeration));if(!object)return finish(e_outofmemory);
            memory.store(GuestAddress{out},bits,object);return finish(0);
        }catch(const MemoryFault&){return finish(e_invalidarg);}catch(const std::bad_alloc&){return finish(e_outofmemory);}
    }
    return nested_continuation(state,memory,"shell32 公用函式",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto text=[&](std::u16string_view value){const auto bytes=utf16_bytes(value);const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));
        };
        const std::uint64_t current_user=x86_?0x80000001ULL:0xffffffff80000001ULL;
        try{
            if(name=="SHCLSIDFromString"){
                if(!call(0) || !call(1))co_return finish(e_invalidarg);
                co_return finish(static_cast<std::uint32_t>(co_await api("ole32.dll","CLSIDFromString",{call(0),call(1)})));
            }
            if(name=="SHCoCreateInstance"){
                // (pszCLSID, pclsid, pUnkOuter, riid, ppv)
                if(!call(4))co_return finish(e_pointer);m.store(GuestAddress{call(4)},bits,0);
                std::uint64_t clsid=call(1);
                if(call(0)){clsid=scratch(16);const auto parsed=static_cast<std::uint32_t>(co_await api("ole32.dll","CLSIDFromString",{call(0),clsid}));if(parsed&0x80000000U)co_return finish(parsed);}
                if(!clsid || !call(3))co_return finish(e_invalidarg);
                co_return finish(static_cast<std::uint32_t>(co_await api("ole32.dll","CoCreateInstance",{clsid,call(2),1,call(3),call(4)})));
            }
            if(name=="SHGetLocalizedName" || name=="SHSetLocalizedName" || name=="SHRemoveLocalizedName"){
                if(!call(0))co_return finish(e_invalidarg);
                auto folder=read_utf16(m,GuestAddress{call(0)},32768);while(!folder.empty() && (folder.back()==u'\\' || folder.back()==u'/'))folder.pop_back();
                if(folder.empty())co_return finish(e_invalidarg);
                const auto ini=text(folder+u"\\desktop.ini"),section=text(u".ShellClassInfo"),key=text(u"LocalizedResourceName");
                if(name=="SHGetLocalizedName"){
                    // (pszPath, pszResModule, cch, pidsRes)
                    const auto cch=static_cast<std::uint32_t>(call(2));if(!call(1) || !cch || !call(3))co_return finish(e_invalidarg);
                    const auto buffer=scratch(2*1024),empty=text(u"");
                    const auto length=static_cast<std::uint32_t>(co_await api("kernel32.dll","GetPrivateProfileStringW",{section,key,empty,buffer,1024,ini}));
                    const auto value=read_utf16(m,GuestAddress{buffer},1024);
                    if(!length || value.size()<4 || value.front()!=u'@')co_return finish(hresult_win32(2));
                    const auto comma=value.rfind(u',');if(comma==value.npos || comma<2)co_return finish(e_fail);
                    auto id_text=value.substr(comma+1);bool negative=false;if(!id_text.empty() && id_text.front()==u'-'){negative=true;id_text.erase(0,1);}
                    if(id_text.empty() || id_text.size()>9 || !std::all_of(id_text.begin(),id_text.end(),[](char16_t c){return c>=u'0' && c<=u'9';}))co_return finish(e_fail);
                    std::int32_t id=0;for(const auto c:id_text)id=id*10+(c-u'0');
                    const auto module=value.substr(1,comma-1);if(module.size()+1>cch)co_return finish(hresult_win32(122));
                    (void)negative;m.write(GuestAddress{call(1)},utf16_bytes(module));m.store(GuestAddress{call(3)},32,static_cast<std::uint32_t>(id));
                    co_return finish(0);
                }
                if(name=="SHRemoveLocalizedName"){
                    if(!(co_await api("kernel32.dll","WritePrivateProfileStringW",{section,key,0,ini})&UINT32_MAX)){
                        const auto error=static_cast<std::uint32_t>(co_await api("kernel32.dll","GetLastError",{}));co_return finish(hresult_win32(error?error:2));}
                    co_return finish(0);
                }
                // SHSetLocalizedName(pszPath, pszResModule, idsRes)
                if(!call(1))co_return finish(e_invalidarg);
                const auto module=read_utf16(m,GuestAddress{call(1)},32768);if(module.empty())co_return finish(e_invalidarg);
                const auto id=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(call(2)));
                const auto magnitude=static_cast<std::int64_t>(id<0?-static_cast<std::int64_t>(id):id);
                const auto number=std::to_string(magnitude);std::u16string value=u"@"+module+u",-"+std::u16string(number.begin(),number.end());
                if(!(co_await api("kernel32.dll","WritePrivateProfileStringW",{section,key,text(value),ini})&UINT32_MAX)){
                    const auto error=static_cast<std::uint32_t>(co_await api("kernel32.dll","GetLastError",{}));co_return finish(hresult_win32(error?error:5));}
                (void)co_await api("kernel32.dll","SetFileAttributesW",{ini,0x6}); // FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM
                // 與 PathMakeSystemFolder 的預設相同：資料夾加上 FILE_ATTRIBUTE_READONLY，殼層才會讀取 desktop.ini。
                const auto folder_text=text(folder);const auto attributes=static_cast<std::uint32_t>(co_await api("kernel32.dll","GetFileAttributesW",{folder_text}));
                if(attributes!=UINT32_MAX)(void)co_await api("kernel32.dll","SetFileAttributesW",{folder_text,attributes|0x1U});
                co_return finish(0);
            }
            if(name=="SHSetUnreadMailCountW" || name=="SHGetUnreadMailCountW" || name=="SHEnumerateUnreadMailAccountsW"){
                const auto root_path=std::u16string(u"Software\\Microsoft\\Windows\\CurrentVersion\\UnreadMail");
                const auto key_at=scratch(unit);
                if(name=="SHSetUnreadMailCountW"){
                    // (pszMailAddress, dwCount, pszShellExecuteCommand)
                    if(!call(0) || !call(2))co_return finish(e_invalidarg);
                    const auto address=read_utf16(m,GuestAddress{call(0)},1024);if(address.empty() || address.find(u'\\')!=address.npos)co_return finish(e_invalidarg);
                    const auto error=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegCreateKeyExW",{current_user,text(root_path+u"\\"+address),0,0,0,0x20006,0,key_at,0}));
                    if(error)co_return finish(hresult_win32(error));
                    const auto key=m.load(GuestAddress{key_at},bits);
                    const auto count_at=scratch(4);m.store(GuestAddress{count_at},32,static_cast<std::uint32_t>(call(1)));
                    const auto time_at=scratch(8);(void)co_await api("kernel32.dll","GetSystemTimeAsFileTime",{time_at});
                    const auto command=read_utf16(m,GuestAddress{call(2)},32768);const auto command_bytes=utf16_bytes(command);
                    std::uint32_t status=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegSetValueExW",{key,text(u"MessageCount"),0,4,count_at,4}));
                    if(!status)status=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegSetValueExW",{key,text(u"TimeStamp"),0,3,time_at,8}));
                    if(!status)status=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegSetValueExW",{key,text(u"Application"),0,1,text(command),command_bytes.size()}));
                    (void)co_await api("advapi32.dll","RegCloseKey",{key});
                    co_return finish(hresult_win32(status));
                }
                const auto user=call(0)?call(0):current_user;
                if(name=="SHEnumerateUnreadMailAccountsW"){
                    // (hKeyUser, dwIndex, pszMailAddress, cchMailAddress)
                    const auto cch=static_cast<std::int32_t>(static_cast<std::uint32_t>(call(3)));if(!call(2) || cch<=0)co_return finish(e_invalidarg);
                    const auto error=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegOpenKeyExW",{user,text(root_path),0,0x20019,key_at}));
                    if(error)co_return finish(hresult_win32(error==2?259U:error));
                    const auto key=m.load(GuestAddress{key_at},bits);const auto size_at=scratch(4);m.store(GuestAddress{size_at},32,static_cast<std::uint32_t>(cch));
                    const auto status=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegEnumKeyExW",{key,static_cast<std::uint32_t>(call(1)),call(2),size_at,0,0,0,0}));
                    (void)co_await api("advapi32.dll","RegCloseKey",{key});
                    co_return finish(hresult_win32(status));
                }
                // SHGetUnreadMailCountW(hKeyUser, pszMailAddress, pdwCount, pFileTime, pszShellExecuteCommand, cchShellExecuteCommand)
                if(!call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},32,0);
                const auto read_account=[&](std::u16string address,std::uint64_t time_out,std::uint64_t command_out,std::uint32_t command_cch)->service_callback::Task<std::optional<std::pair<std::uint32_t,std::uint64_t>>>{
                    const auto open=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegOpenKeyExW",{user,text(root_path+u"\\"+address),0,0x20019,key_at}));
                    if(open)co_return std::nullopt;
                    const auto key=m.load(GuestAddress{key_at},bits);const auto value_at=scratch(8),size_at=scratch(4);
                    m.store(GuestAddress{size_at},32,4);std::uint32_t count=0;std::uint64_t stamp=0;
                    if(!co_await api("advapi32.dll","RegQueryValueExW",{key,text(u"MessageCount"),0,0,value_at,size_at}))count=static_cast<std::uint32_t>(m.load(GuestAddress{value_at},32));
                    m.store(GuestAddress{size_at},32,8);m.store(GuestAddress{value_at},64,0);
                    if(!co_await api("advapi32.dll","RegQueryValueExW",{key,text(u"TimeStamp"),0,0,value_at,size_at}))stamp=m.load(GuestAddress{value_at},64);
                    if(time_out)m.store(GuestAddress{time_out},64,stamp);
                    if(command_out && command_cch){m.store(GuestAddress{size_at},32,command_cch*2);m.store(GuestAddress{command_out},16,0);
                        (void)co_await api("advapi32.dll","RegQueryValueExW",{key,text(u"Application"),0,0,command_out,size_at});}
                    (void)co_await api("advapi32.dll","RegCloseKey",{key});
                    co_return std::pair{count,stamp};
                };
                if(call(1)){
                    const auto address=read_utf16(m,GuestAddress{call(1)},1024);
                    const auto account=co_await read_account(address,call(3),call(4),static_cast<std::uint32_t>(call(5)));
                    if(!account)co_return finish(e_fail);
                    m.store(GuestAddress{call(2)},32,account->first);co_return finish(0);
                }
                // 不指定帳號：時間戳晚於 pFileTime（未給則全部）的帳號未讀數總和。
                const std::uint64_t since=call(3)?m.load(GuestAddress{call(3)},64):0;std::uint32_t total=0;
                const auto open=static_cast<std::uint32_t>(co_await api("advapi32.dll","RegOpenKeyExW",{user,text(root_path),0,0x20019,key_at}));
                if(!open){
                    const auto root=m.load(GuestAddress{key_at},bits);const auto name_at=scratch(2*256),size_at=scratch(4);
                    std::vector<std::u16string> accounts;
                    for(std::uint32_t index=0;index<4096;++index){m.store(GuestAddress{size_at},32,256);
                        if(co_await api("advapi32.dll","RegEnumKeyExW",{root,index,name_at,size_at,0,0,0,0}))break;accounts.push_back(read_utf16(m,GuestAddress{name_at},256));}
                    (void)co_await api("advapi32.dll","RegCloseKey",{root});
                    for(const auto& account:accounts)if(const auto value=co_await read_account(account,0,0,0);value && value->second>=since)total+=value->first;
                }
                m.store(GuestAddress{call(2)},32,total);co_return finish(0);
            }
            if(name.starts_with("ShellExec_RunDLL")){
                // (hwnd, hinst, pszCmdLine, nCmdShow)
                const bool wide=name.ends_with('W');if(!call(2))co_return finish(0);
                auto line=wide?read_utf16(m,GuestAddress{call(2)},32768):utf8_to_utf16_lossy(read_ansi(m,call(2),32768));
                std::size_t at=line.find_first_not_of(u" \t");if(at==line.npos)co_return finish(0);
                std::uint32_t mask=0;
                if(line[at]==u'?'){const auto end=line.find(u'?',at+1);if(end!=line.npos){auto flags=line.substr(at+1,end-at-1);if(flags.starts_with(u"0x") || flags.starts_with(u"0X"))flags.erase(0,2);
                        for(const auto c:flags){const unsigned digit=c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:16U;if(digit>15)break;mask=mask<<4|digit;}
                        at=line.find_first_not_of(u" \t",end+1);if(at==line.npos)co_return finish(0);}}
                std::u16string file,parameters;
                if(line[at]==u'"'){const auto end=line.find(u'"',at+1);file=line.substr(at+1,end==line.npos?line.npos:end-at-1);at=end==line.npos?line.size():end+1;}
                else{const auto end=line.find_first_of(u" \t",at);file=line.substr(at,end==line.npos?line.npos:end-at);at=end==line.npos?line.size():end;}
                if(const auto start=line.find_first_not_of(u" \t",at);start!=line.npos)parameters=line.substr(start);
                if(file.empty())co_return finish(0);
                // SHELLEXECUTEINFOW：x86 60、x64 112 位元組；SEE_MASK_FLAG_NO_UI 加上命令列指定的旗標。
                const unsigned size=x86_?60U:112U;const auto info=scratch(size);
                m.store(GuestAddress{info},32,size);m.store(GuestAddress{info+4},32,mask|0x400U);m.store(GuestAddress{info+8},bits,call(0));
                m.store(GuestAddress{info+8+2*unit},bits,text(file));if(!parameters.empty())m.store(GuestAddress{info+8+3*unit},bits,text(parameters));
                m.store(GuestAddress{info+8+5*unit},32,static_cast<std::uint32_t>(call(3)));
                (void)co_await api("shell32.dll","ShellExecuteExW",{info});
                co_return finish(0);
            }
        }catch(const MemoryFault&){co_return finish(e_invalidarg);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
        throw HostCallFault("未知的 shell32 公用函式");
    });
}
}
