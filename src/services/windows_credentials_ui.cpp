#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/crypto_aead.hpp"
#include "x64arm/crypto.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <algorithm>
#include <iterator>

namespace x64arm {
// credui 認證緩衝區：CredPackAuthenticationBuffer 產生 KERB_INTERACTIVE_UNLOCK_LOGON（UNICODE_STRING 的 Buffer
// 為相對結構起點的位移），CredUnPackAuthenticationBuffer 讀回使用者、網域與密碼。
// 參考：https://learn.microsoft.com/windows/win32/api/wincred/nf-wincred-credpackauthenticationbufferw
namespace {
constexpr std::uint32_t kerb_interactive_logon=2,kerb_workstation_unlock_logon=7;
constexpr std::uint32_t credential_store_type=0x10002,missing_key=0x80090016U;
constexpr std::uint32_t no_persist=2,show_save=0x40,persist=0x1000,expect_confirmation=0x20000,generic=0x40000;
struct Credential {std::u16string domain,user,password;};
struct CredentialFailure {std::uint32_t code;};
struct SecretBytes {std::vector<std::uint8_t> bytes;~SecretBytes(){OPENSSL_cleanse(bytes.data(),bytes.size());}};
struct SecretKey {std::array<std::uint8_t,32> bytes;~SecretKey(){OPENSSL_cleanse(bytes.data(),bytes.size());}};
std::vector<std::uint8_t> credential_text(std::u16string_view text,bool wide){
    if(wide)return utf16_bytes(text);
    const auto value=utf16_to_utf8(text);std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);return bytes;
}
std::string credential_name(std::u16string target,bool is_generic){
    for(auto& c:target)c=unicode_case_unit(c,false);
    const auto bytes=utf16_to_utf8(target);CryptoHash hash("SHA256",false,{});
    hash.update({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()});
    auto name=std::string(is_generic?"generic:":"domain:");
    for(const auto byte:hash.digest()){name+="0123456789abcdef"[byte>>4];name+="0123456789abcdef"[byte&15];}return name;
}
std::optional<std::pair<std::u16string,std::u16string>> account_parts(const std::u16string& input){
    if(input.empty() || input.size()>513)return {};
    if(const auto slash=input.find(u'\\');slash!=input.npos){
        if(!slash || slash+1==input.size() || input.find(u'\\',slash+1)!=input.npos)return {};
        return std::pair{input.substr(slash+1),input.substr(0,slash)};
    }
    if(const auto at=input.rfind(u'@');at!=input.npos && (!at || at+1==input.size()))return {};
    return std::pair{input,std::u16string{}}; // UPN 保留完整使用者名稱，網域為空。
}
// KERB_INTERACTIVE_LOGON：MessageType、LogonDomainName、UserName、Password；之後是 LUID LogonId。
unsigned header_size(bool x86){return x86?36U:64U;}
unsigned string_slot(bool x86,unsigned index){return (x86?4U:8U)+index*(x86?8U:16U);}
std::vector<std::uint8_t> pack(const Credential& value,bool x86){
    std::vector<std::uint8_t> bytes(header_size(x86));
    const auto put=[&](std::size_t at,std::uint64_t number,unsigned width){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(number>>(i*8));};
    put(0,kerb_interactive_logon,4);
    const std::array<const std::u16string*,3> fields{&value.domain,&value.user,&value.password};
    for(unsigned i=0;i<3;++i){
        const auto data=utf16_bytes(*fields[i]);const auto offset=bytes.size();const auto length=fields[i]->size()*2;
        if(length>65534)throw std::invalid_argument("認證字串過長");
        const auto slot=string_slot(x86,i);put(slot,length,2);put(slot+2,length,2);put(slot+(x86?4U:8U),length?offset:0,x86?4U:8U);
        bytes.insert(bytes.end(),data.begin(),data.begin()+static_cast<std::ptrdiff_t>(length));
    }
    return bytes;
}
std::optional<Credential> unpack(std::span<const std::uint8_t> bytes,bool x86){
    if(bytes.size()<header_size(x86))return std::nullopt;
    const auto get=[&](std::size_t at,unsigned width){std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;};
    const auto type=get(0,4);if(type!=kerb_interactive_logon && type!=kerb_workstation_unlock_logon)return std::nullopt;
    Credential result;const std::array<std::u16string*,3> fields{&result.domain,&result.user,&result.password};
    for(unsigned i=0;i<3;++i){
        const auto slot=string_slot(x86,i);const auto length=get(slot,2),offset=get(slot+(x86?4U:8U),x86?4U:8U);
        if(length%2 || (length && (offset<header_size(x86) || offset>bytes.size() || length>bytes.size()-offset)))return std::nullopt;
        for(std::uint64_t at=0;at<length;at+=2)fields[i]->push_back(static_cast<char16_t>(get(static_cast<std::size_t>(offset+at),2)));
    }
    return result;
}
std::vector<std::uint8_t> seal_credential(const PrivateKeyStoreContext& store,const std::string& name,const Credential& value){
    SecretKey key{sandbox_data_protection_key(store,"credui:v1",false,true)};
    SecretBytes plain{pack(value,true)};std::array<std::uint8_t,16> tag{};
    std::vector<std::uint8_t> header{'M','W','C','R','E','D','0','1'};header.resize(20);
    if(RAND_bytes(header.data()+8,12)!=1)throw CredentialFailure{8};
    auto aad=header;aad.insert(aad.end(),name.begin(),name.end());std::vector<std::uint8_t> cipher;
    if(!crypto_aes_gcm(true,key.bytes,std::span(header).subspan(8,12),aad,plain.bytes,cipher,tag))throw CredentialFailure{13};
    header.insert(header.end(),cipher.begin(),cipher.end());header.insert(header.end(),tag.begin(),tag.end());return header;
}
std::optional<Credential> stored_credential(const PrivateKeyStoreContext& store,const std::string& name){
    std::vector<std::uint8_t> sealed;const auto status=private_key_store_read(store,credential_store_type,name,false,sealed);
    if(status==missing_key)return {};
    if(status)throw CredentialFailure{status};
    if(sealed.size()<36 || !std::equal(sealed.begin(),sealed.begin()+8,"MWCRED01"))throw CredentialFailure{13};
    SecretKey key{sandbox_data_protection_key(store,"credui:v1",false,false)};
    std::array<std::uint8_t,16> tag{};std::copy_n(sealed.end()-16,16,tag.begin());
    auto aad=std::vector<std::uint8_t>(sealed.begin(),sealed.begin()+20);aad.insert(aad.end(),name.begin(),name.end());SecretBytes plain;
    if(!crypto_aes_gcm(false,key.bytes,std::span(sealed).subspan(8,12),aad,std::span(sealed).subspan(20,sealed.size()-36),plain.bytes,tag))throw CredentialFailure{13};
    auto value=unpack(plain.bytes,true);if(!value)throw CredentialFailure{13};return value;
}
std::uint32_t store_credential(const PrivateKeyStoreContext& store,const std::string& name,std::span<const std::uint8_t> sealed){
    auto status=private_key_store_write(store,credential_store_type,name,false,sealed,false,PrivateKeyAccess::LogonUser);
    if(status==missing_key)status=private_key_store_write(store,credential_store_type,name,false,sealed,true,PrivateKeyAccess::LogonUser);
    if(status==0x8009000fU)status=private_key_store_write(store,credential_store_type,name,false,sealed,false,PrivateKeyAccess::LogonUser);
    return status;
}
}
std::span<const WindowsServiceDescriptor> credential_ui_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"credui.dll","CredPackAuthenticationBufferW",5},{"credui.dll","CredPackAuthenticationBufferA",5},
        {"credui.dll","CredUnPackAuthenticationBufferW",9},{"credui.dll","CredUnPackAuthenticationBufferA",9},
        {"credui.dll","CredUIPromptForWindowsCredentialsW",9},{"credui.dll","CredUIPromptForWindowsCredentialsA",9},
        {"credui.dll","CredUIPromptForCredentialsW",10},{"credui.dll","CredUIPromptForCredentialsA",10},
        {"credui.dll","CredUIConfirmCredentialsW",2},{"credui.dll","CredUIConfirmCredentialsA",2},
        {"credui.dll","CredUIParseUserNameW",5},{"credui.dll","CredUIParseUserNameA",5}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_credential_ui(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');
    const bool dword_result=name.starts_with("CredUI");
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;result=dword_result?error:0;};
    const auto text=[&](std::uint64_t at){return wide?read_utf16(memory,GuestAddress{at},65535):utf8_to_utf16_lossy(read_ansi(memory,at,65535));};
    try{
        if(name.starts_with("CredUIParseUserName")){
            const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
            if(!arg(0) || !arg(1) || !arg(2) || !arg(3) || !arg(4))return done(87);
            const auto parts=account_parts(text(arg(0)));if(!parts)return done(2202);
            const auto user=credential_text(parts->first,wide),domain=credential_text(parts->second,wide);const unsigned unit=wide?2U:1U;
            if(user.size()/unit>static_cast<std::uint32_t>(arg(2)) || domain.size()/unit>static_cast<std::uint32_t>(arg(4)))return done(122);
            memory.check(GuestAddress{arg(1)},user.size(),Access::Write);memory.check(GuestAddress{arg(3)},domain.size(),Access::Write);
            memory.write(GuestAddress{arg(1)},user);memory.write(GuestAddress{arg(3)},domain);return done(0);
        }
        if(name.starts_with("CredUIConfirmCredentials")){
            const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
            if(!arg(0))return done(87);const auto target=text(arg(0));if(target.empty())return done(87);
            const auto store=private_key_context();auto& queued=gui_.credential_confirmations;
            // Confirm 沒有 generic 參數，依同一 SID／目標找到最近一次待確認項目。
            const auto found=std::find_if(queued.rbegin(),queued.rend(),[&](const auto& item){return item.user_sid==store.user_sid &&
                (item.name==credential_name(target,true) || item.name==credential_name(target,false));});
            if(found==queued.rend())return done(1168);
            if(arg(1) && found->persist){
                if(!runtime_.network.external_io)return done(50);
                if(const auto error=store_credential(store,found->name,found->sealed))return done(error);
            }
            queued.erase(std::next(found).base());return done(0);
        }
        if(name.starts_with("CredUIPrompt")){
            const bool legacy=name.starts_with("CredUIPromptForCredentials");
            // 參數：pUiInfo、dwAuthError、pulAuthPackage、pvInAuthBuffer、ulInAuthBufferSize、ppvOutAuthBuffer、pulOutAuthBufferSize、pfSave、dwFlags。
            const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);const auto flags=static_cast<std::uint32_t>(arg(legacy?9:8));
            const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
            constexpr std::uint32_t known=0x1U|0x2U|0x10U|0x20U|0x100U|0x200U|0x1000U|0x2000U|0x10000000U;
            GuiCredentialPrompt prompt;prompt.flags=flags;prompt.legacy=legacy;prompt.wide=wide;
            bool checkbox=false;std::u16string user,password;
            if(legacy){
                constexpr std::uint32_t legacy_known=1U|2U|4U|8U|0x10U|0x40U|0x80U|0x100U|0x200U|0x400U|0x800U|0x1000U|0x4000U|0x20000U|0x40000U|0x80000U|0x100000U;
                if(flags&~legacy_known || ((flags&persist) && (flags&no_persist)) || ((flags&0x80U) && !(flags&generic)) ||
                   ((flags&generic) && (flags&0x80000U)) || std::popcount(flags&(8U|0x10U|0x100U))>1)return done(1004);
                if(arg(2) || !arg(4) || !arg(5) || !arg(6) || !arg(7))return done(87);
                prompt.target=arg(1)?text(arg(1)):std::u16string{};
                if(prompt.target.size()>((flags&generic)?32767U:337U))return done(87);
                prompt.user_output=arg(4);prompt.password_output=arg(6);prompt.user_capacity=static_cast<std::uint32_t>(arg(5));prompt.password_capacity=static_cast<std::uint32_t>(arg(7));
                const auto bounded=[&](std::uint64_t at,std::uint32_t capacity){
                    if(!capacity || capacity>65536U)throw CredentialFailure{87};
                    return wide?read_utf16(memory,GuestAddress{at},capacity):utf8_to_utf16_lossy(read_ansi(memory,at,capacity));
                };
                user=bounded(arg(4),prompt.user_capacity);password=bounded(arg(6),prompt.password_capacity);
                if((flags&0x200U) && user.empty())return done(87);
                checkbox=!(flags&persist) && (!(flags&no_persist) || (flags&show_save));prompt.save=arg(8);
                if(prompt.save){memory.check(GuestAddress{prompt.save},4,Access::Write);prompt.initial_save=memory.load(GuestAddress{prompt.save},32)!=0;}
                prompt.initial_save=(flags&persist) || (checkbox && prompt.initial_save);
                prompt.user_sid=private_key_context().user_sid;
                if(flags&(0x10U|0x100U))return done(50); // 需要憑證／智慧卡供應者，不能把密碼提示冒充該機制。
            }else{
                if(!arg(2) || !arg(5) || !arg(6) || (flags&~known) || ((flags&2U) && !arg(7)) || ((flags&0x20U) && !arg(3)))return done(87);
                memory.check(GuestAddress{arg(5)},unit,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);memory.store(GuestAddress{arg(5)},bits,0);memory.store(GuestAddress{arg(6)},32,0);
                prompt.output=arg(5);prompt.output_size=arg(6);prompt.save=(flags&2U)?arg(7):0;checkbox=(flags&2U)!=0;
            }
            std::uint64_t owner=0;std::u16string message,caption=u"Windows 安全性";
            if(arg(0)){
                if(memory.load(GuestAddress{arg(0)},32)!=(x86_?20U:40U))return done(87);
                owner=memory.load(GuestAddress{arg(0)+unit},bits);const auto message_at=memory.load(GuestAddress{arg(0)+2*unit},bits),caption_at=memory.load(GuestAddress{arg(0)+3*unit},bits);
                if(legacy && prompt.target.empty() && (!message_at || !caption_at))return done(87);
                if(message_at)message=text(message_at);if(caption_at)caption=text(caption_at);
                if(message.size()>32767 || caption.size()>128)return done(87);
                const auto banner=memory.load(GuestAddress{arg(0)+4*unit},bits);if(banner && !gui_.bitmaps.contains(banner))return done(87);
                if(owner && !gui_.windows.contains(owner))return done(1400);
            }
            if(legacy){
                if(prompt.target.empty() && (!(flags&no_persist) || message.empty() || caption.empty()))return done(87);
                if(!(flags&no_persist) && runtime_.network.external_io){
                    const auto cached=stored_credential(private_key_context(prompt.user_sid),credential_name(prompt.target,(flags&generic)!=0));
                    if(cached && user.empty()){user=cached->user;password=cached->password;}
                    if(cached && (flags&generic) && !(flags&0x80U) && !arg(3) && user==cached->user){
                        user=cached->user;password=cached->password;
                        const auto userbytes=credential_text(user,wide),passbytes=credential_text(password,wide);const unsigned charunit=wide?2U:1U;
                        if(userbytes.size()/charunit>prompt.user_capacity || passbytes.size()/charunit>prompt.password_capacity)return done(122);
                        memory.check(GuestAddress{prompt.user_output},userbytes.size(),Access::Write);memory.check(GuestAddress{prompt.password_output},passbytes.size(),Access::Write);
                        memory.write(GuestAddress{prompt.user_output},userbytes);memory.write(GuestAddress{prompt.password_output},passbytes);if(prompt.save)memory.store(GuestAddress{prompt.save},32,1);return done(0);
                    }
                }
            }else if(arg(3)){
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint32_t>(arg(4)),1024U*1024U)));memory.read(GuestAddress{arg(3)},bytes);
                auto value=unpack(bytes,x86_);if(!value && !x86_)value=unpack(bytes,true);if(!value)return done(87);
                user=value->domain.empty()?value->user:value->domain+u"\\"+value->user;
            }else if(flags&0x200U)user=u"X64ARM\\user"; // CREDUIWIN_ENUMERATE_CURRENT_USER：目前虛擬使用者。
            if(!native_gui_)return done(50); // 沒有互動桌面時無法提示使用者。
            if(const auto error=static_cast<std::uint32_t>(arg(legacy?3:1)))message+=(message.empty()?u"":u"\n")+std::u16string(u"上次輸入的認證無法使用（錯誤碼 ")+utf8_to_utf16_lossy(std::to_string(error))+u"）。";
            GuiWindow dialog;dialog.dpi_context=effective_dpi_context();dialog.thread=threads_.current;dialog.title=caption;dialog.class_name=u"#32770";dialog.dialog=true;dialog.modal=true;dialog.builtin=true;
            dialog.owner=owner;dialog.width=420;dialog.height=checkbox?250:220;dialog.style=0x10000000U;dialog.visible=true;dialog.extra.resize(40);dialog.default_id=1;
            dialog.procedure=resolve("user32.dll","DefDlgProcW").value;dialog.credential_prompt=std::move(prompt);
            const bool initially_checked=dialog.credential_prompt->initial_save;
            const auto handle=allocate_gui_window();std::vector<GuiWindowCreation> plans{{handle,std::move(dialog),true}};
            const auto add=[&](std::u16string_view cls,std::u16string title,std::uint32_t id,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height,std::uint32_t style){
                GuiWindow child;child.dpi_context=effective_dpi_context();child.class_name=std::u16string(cls);child.title=std::move(title);child.id=id;child.x=x;child.y=y;child.width=width;child.height=height;
                child.thread=threads_.current;child.parent=handle;child.style=style|0x50000000U;child.visible=true;child.builtin=true;child.control_class=child.class_name;child.procedure=stock_gui_proc(child.class_name,true);
                plans.push_back({allocate_gui_window(),std::move(child),true});
            };
            add(u"static",message.empty()?u"請輸入認證資訊。":message,0xffff,20,16,380,40,0);
            add(u"static",u"使用者名稱：",0xffff,20,64,100,22,0);add(u"edit",user,100,124,62,276,24,0x00810080U|(legacy && (flags&(0x100000U|0x200U))?0x800U:0U));
            add(u"static",u"密碼：",0xffff,20,98,100,22,0);add(u"edit",password,101,124,96,276,24,0x008100a0U); // ES_PASSWORD
            if(checkbox){add(u"button",u"記住我的認證",102,124,130,276,22,0x00010003U);plans.back().window.check=initially_checked?1:0;}
            const std::int32_t row=checkbox?170:140;
            add(u"button",u"確定",1,214,row,90,28,0x00010001U);add(u"button",u"取消",2,310,row,90,28,0x00010000U);
            return start_gui_creation(state,memory,std::move(plans));
        }
        const auto flags=static_cast<std::uint32_t>(arg(0));
        if(name.starts_with("CredPack")){
            // CRED_PACK_PROTECTED_CREDENTIALS(1)、WOW_BUFFER(2)、GENERIC_CREDENTIALS(4)、ID_PROVIDER_CREDENTIALS(8)。
            if((flags&~15U) || !arg(1) || !arg(2) || !arg(4))fail(87);
            else {
                Credential value;value.user=text(arg(1));value.password=text(arg(2));
                if(const auto slash=value.user.find(u'\\');slash!=value.user.npos && !(flags&4U)){value.domain=value.user.substr(0,slash);value.user.erase(0,slash+1);}
                const auto bytes=pack(value,x86_ || (flags&2U));
                memory.check(GuestAddress{arg(4)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(4)},32);
                memory.store(GuestAddress{arg(4)},32,bytes.size());
                if(!arg(3) || capacity<bytes.size())fail(122);else {memory.write(GuestAddress{arg(3)},bytes);result=1;}
            }
        }else {
            if((flags&~1U) || !arg(1) || !arg(2) || static_cast<std::uint32_t>(arg(2))>1024U*1024U)fail(87);
            else {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(static_cast<std::uint32_t>(arg(2))));memory.read(GuestAddress{arg(1)},bytes);
                auto value=unpack(bytes,x86_);if(!value && !x86_)value=unpack(bytes,true); // WOW 緩衝區使用 32-bit 結構。
                if(!value)fail(87);
                else {
                    // 沒有網域輸出時，使用者名稱以 DOMAIN\user 回傳。
                    auto user=value->user;if(!arg(5) && !value->domain.empty())user=value->domain+u"\\"+user;
                    const auto encode=[&](const std::u16string& s){if(wide)return utf16_bytes(s);auto narrow=utf16_to_utf8(s);std::vector<std::uint8_t> out(narrow.begin(),narrow.end());out.push_back(0);return out;};
                    const unsigned unit=wide?2:1;bool fits=true;
                    const std::array<std::tuple<std::u16string,std::uint64_t,std::uint64_t>,3> outputs{{{user,arg(3),arg(4)},{value->domain,arg(5),arg(6)},{value->password,arg(7),arg(8)}}};
                    for(const auto& [content,buffer,count]:outputs){
                        if(!count)continue;memory.check(GuestAddress{count},4,Access::Write);
                        const auto data=encode(content);const auto needed=data.size()/unit;const auto capacity=memory.load(GuestAddress{count},32);
                        memory.store(GuestAddress{count},32,needed);if(!buffer || capacity<needed)fits=false;
                    }
                    if(!fits)fail(122);
                    else {for(const auto& [content,buffer,count]:outputs)if(count && buffer)memory.write(GuestAddress{buffer},encode(content));result=1;}
                }
            }
        }
    }catch(const CredentialFailure& error){fail(error.code);}catch(const DataProtectionError& error){fail(error.code);}
    catch(const MemoryFault&){fail(998);}catch(const std::invalid_argument&){fail(87);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
std::uint32_t WindowsServices::finish_credential_prompt(GuestMemory& memory,std::uint64_t dialog,bool confirmed){
    const auto& prompt=*gui_.windows.at(dialog).credential_prompt;
    if(!confirmed)return 1223; // ERROR_CANCELLED：舊版字串維持原值；新版維持 NULL／0。
    Credential value;bool save=prompt.initial_save;
    for(const auto& [child,window]:gui_.windows){
        if(window.parent!=dialog)continue;
        if(window.id==100)value.user=window.title;else if(window.id==101)value.password=window.title;else if(window.id==102)save=window.check==1;
    }
    try{
        if(prompt.legacy){
            if((!(prompt.flags&generic) || (prompt.flags&0x800U)) && value.user.find(u'\\')==value.user.npos &&
               value.user.find(u'@')==value.user.npos && !prompt.target.empty())value.user=prompt.target+u"\\"+value.user;
            if((!(prompt.flags&generic) || (prompt.flags&(0x400U|0x800U))) && !account_parts(value.user))return 2202;
            const auto user=credential_text(value.user,prompt.wide),password=credential_text(value.password,prompt.wide);const unsigned unit=prompt.wide?2U:1U;
            if(user.size()/unit>prompt.user_capacity || password.size()/unit>prompt.password_capacity)return 122;
            memory.check(GuestAddress{prompt.user_output},user.size(),Access::Write);memory.check(GuestAddress{prompt.password_output},password.size(),Access::Write);
            if(prompt.save)memory.check(GuestAddress{prompt.save},4,Access::Write);
            const bool storing=save && !(prompt.flags&no_persist);
            const auto name=credential_name(prompt.target,(prompt.flags&generic)!=0);
            if(storing && !runtime_.network.external_io)return 50;
            const auto store=private_key_context(prompt.user_sid);
            auto sealed=storing?seal_credential(store,name,value):std::vector<std::uint8_t>{};
            if(prompt.flags&expect_confirmation){
                auto& queued=gui_.credential_confirmations;
                std::erase_if(queued,[&](const auto& item){return item.user_sid==store.user_sid && item.name==name;});
                if(std::count_if(queued.begin(),queued.end(),[&](const auto& item){return item.user_sid==store.user_sid;})>=5){
                    const auto oldest=std::find_if(queued.begin(),queued.end(),[&](const auto& item){return item.user_sid==store.user_sid;});queued.erase(oldest);
                }
                queued.push_back({name,store.user_sid,std::move(sealed),storing});
            }else if(storing){if(const auto error=store_credential(store,name,sealed))return error;}
            memory.write(GuestAddress{prompt.user_output},user);memory.write(GuestAddress{prompt.password_output},password);
            if(prompt.save)memory.store(GuestAddress{prompt.save},32,save?1:0);return 0;
        }
        if(const auto slash=value.user.find(u'\\');slash!=value.user.npos && !(prompt.flags&1U)){value.domain=value.user.substr(0,slash);value.user.erase(0,slash+1);}
        const auto bytes=pack(value,x86_ || (prompt.flags&0x10000000U));
        const auto buffer=allocate_heap(memory,bytes.size());if(!buffer)return 8;gui_.task_allocations.insert(buffer); // 呼叫端以 CoTaskMemFree 釋放。
        memory.write(GuestAddress{buffer},bytes);memory.store(GuestAddress{prompt.output},x86_?32:64,buffer);memory.store(GuestAddress{prompt.output_size},32,bytes.size());
        if(prompt.save)memory.store(GuestAddress{prompt.save},32,save?1:0);
    }catch(const CredentialFailure& error){return error.code;}catch(const DataProtectionError& error){return error.code;}
    catch(const std::invalid_argument&){return 87;}catch(const MemoryFault&){return 998;}catch(const std::bad_alloc&){return 8;}
    return 0;
}
}
