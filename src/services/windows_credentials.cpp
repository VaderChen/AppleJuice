// Windows 認證管理員（advapi32 Cred*）。依專案規定認證只留在 sandbox 的 C: 磁碟，依虛擬使用者 SID 隔離，
// 不使用 macOS 鑰匙圈或宿主家目錄；內容以 DPAPI 同一套 sandbox 金鑰加密後放進私鑰儲存。
// CRED_PERSIST_SESSION 只存在本程序的記憶體（真實系統是登入工作階段，本專案沒有跨程序的工作階段服務）。
// 網域認證（CredReadDomainCredentials／CredWriteDomainCredentials）沿用同一份儲存，依 CREDENTIAL_TARGET_INFORMATION 由最具體到最廣的名稱比對；
// CRED_CACHE_TARGET_INFORMATION 快取的目標資訊與工作階段認證同樣只在本程序有效，供 CredGetTargetInfo 取回。
#include "x64arm/windows_services.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>
namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
struct Attribute {std::u16string keyword;std::uint32_t flags{};Bytes value;};
struct Credential {
    std::uint32_t flags{},type{},persist{};std::uint64_t last_written{};
    std::u16string target,comment,alias,user;Bytes blob;std::vector<Attribute> attributes;
};
struct CredentialError {std::uint32_t code;};
constexpr std::uint32_t store_type=0x10010,record_version=1,not_found=1168,invalid_parameter=87;
// 私鑰儲存以 NTE_BAD_KEYSET 回報不存在；CredRead／CredDelete 的契約是 ERROR_NOT_FOUND。
constexpr std::uint32_t credential_status(std::uint32_t status){return status==0x80090016U || status==2 || status==3?not_found:status;}
constexpr std::size_t max_blob=5*512,max_attributes=64,max_attribute_value=256;
void put32(Bytes& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
void put64(Bytes& out,std::uint64_t value){for(unsigned i=0;i<8;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
void put_bytes(Bytes& out,std::span<const std::uint8_t> value){put32(out,static_cast<std::uint32_t>(value.size()));out.insert(out.end(),value.begin(),value.end());}
void put_text(Bytes& out,std::u16string_view value){put32(out,static_cast<std::uint32_t>(value.size()));for(const auto c:value){out.push_back(static_cast<std::uint8_t>(c));out.push_back(static_cast<std::uint8_t>(c>>8));}}
struct Reader {
    std::span<const std::uint8_t> data;std::size_t at{};
    void need(std::size_t count) const {if(data.size()-at<count || at>data.size())throw CredentialError{13};}
    std::uint32_t u32(){need(4);std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{data[at+i]}<<(i*8);at+=4;return value;}
    std::uint64_t u64(){need(8);std::uint64_t value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t{data[at+i]}<<(i*8);at+=8;return value;}
    Bytes bytes(){const auto size=u32();need(size);Bytes value(data.begin()+static_cast<std::ptrdiff_t>(at),data.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;return value;}
    std::u16string text(){const auto size=u32();if(size>0x7fffffffU/2)throw CredentialError{13};need(std::size_t{size}*2);std::u16string value(size,u'\0');for(std::uint32_t i=0;i<size;++i)value[i]=static_cast<char16_t>(data[at+i*2]|(data[at+i*2+1]<<8));at+=std::size_t{size}*2;return value;}
};
Bytes serialize(const Credential& item){
    Bytes out;put32(out,record_version);put32(out,item.flags);put32(out,item.type);put32(out,item.persist);put64(out,item.last_written);
    put_text(out,item.target);put_text(out,item.comment);put_text(out,item.alias);put_text(out,item.user);put_bytes(out,item.blob);
    put32(out,static_cast<std::uint32_t>(item.attributes.size()));
    for(const auto& attribute:item.attributes){put_text(out,attribute.keyword);put32(out,attribute.flags);put_bytes(out,attribute.value);}
    return out;
}
Credential parse(std::span<const std::uint8_t> data){
    Reader in{data};if(in.u32()!=record_version)throw CredentialError{13};
    Credential item;item.flags=in.u32();item.type=in.u32();item.persist=in.u32();item.last_written=in.u64();
    item.target=in.text();item.comment=in.text();item.alias=in.text();item.user=in.text();item.blob=in.bytes();
    const auto count=in.u32();if(count>max_attributes)throw CredentialError{13};
    item.attributes.resize(count);
    for(auto& attribute:item.attributes){attribute.keyword=in.text();attribute.flags=in.u32();attribute.value=in.bytes();}
    return item;
}
// 目標名稱不分大小寫；儲存鍵由類型與折疊後的 UTF-8 名稱組成。
std::u16string folded(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::string store_name(std::uint32_t type,const std::u16string& target){return std::to_string(type)+":"+utf16_to_utf8(folded(target));}
// Filter 只支援結尾的單一 *（CredEnumerate 契約）。
bool filter_matches(const std::u16string& filter,const std::u16string& target){
    if(filter.empty())return true;
    const auto wanted=folded(filter),name=folded(target);
    if(wanted.back()==u'*')return name.compare(0,wanted.size()-1,wanted,0,wanted.size()-1)==0;
    return wanted==name;
}
// 本程序的工作階段認證（鍵與儲存鍵相同）。
std::map<std::string,Credential>& session_credentials(){static std::map<std::string,Credential> value;return value;}
// CREDENTIAL_TARGET_INFORMATION：names 依序為 TargetName、NetbiosServerName、DnsServerName、NetbiosDomainName、DnsDomainName、DnsTreeName、PackageName。
struct TargetInfo {std::array<std::u16string,7> names;std::uint32_t flags{};std::vector<std::uint32_t> types;};
// 快取的目標資訊，最新者在前；同一目標（TargetName，沒有時以伺服器名稱）只留一筆。
std::vector<TargetInfo>& target_cache(){static std::vector<TargetInfo> value;return value;}
constexpr std::size_t max_cached_targets=64,max_target_types=64;
bool domain_type(std::uint32_t type){return type==2 || type==3 || type==4 || type==6;}
// 由最具體到最廣的候選名稱：DFS 形式的 TargetName、DnsServerName、NetbiosServerName、TargetName、DnsServerName 的萬用伺服器名稱（後綴長者優先）、
// <DnsDomainName>\*、<NetbiosDomainName>\*、CRED_SESSION_WILDCARD_NAME、「*」。
std::vector<std::u16string> target_candidates(const TargetInfo& info){
    std::vector<std::u16string> out;const auto add=[&](std::u16string name){if(!name.empty())out.push_back(std::move(name));};
    const auto& target=info.names[0];const auto& dns=info.names[2];
    if(target.find(u'\\')!=std::u16string::npos)add(target);
    add(dns);add(info.names[1]);add(target);
    for(auto dot=dns.find(u'.');dot!=std::u16string::npos && dot+1<dns.size();dot=dns.find(u'.',dot+1))add(u"*"+dns.substr(dot));
    if(!info.names[4].empty())add(info.names[4]+u"\\*");
    if(!info.names[3].empty())add(info.names[3]+u"\\*");
    add(u"*Session");add(u"*");return out;
}
}
std::span<const WindowsServiceDescriptor> credential_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","CredReadW",4},{"advapi32.dll","CredReadA",4},{"advapi32.dll","CredWriteW",2},{"advapi32.dll","CredWriteA",2},
        {"advapi32.dll","CredDeleteW",3},{"advapi32.dll","CredDeleteA",3},{"advapi32.dll","CredEnumerateW",4},{"advapi32.dll","CredEnumerateA",4},
        {"advapi32.dll","CredFree",1},
        {"advapi32.dll","CredMarshalCredentialA",3,0,0,1},{"advapi32.dll","CredMarshalCredentialW",3,0,0,1},{"advapi32.dll","CredUnmarshalCredentialA",3},{"advapi32.dll","CredUnmarshalCredentialW",3},
        {"advapi32.dll","CredIsMarshaledCredentialA",1},{"advapi32.dll","CredIsMarshaledCredentialW",1},{"advapi32.dll","CredGetSessionTypes",2,0,0,1},
        {"advapi32.dll","CredReadDomainCredentialsW",4},{"advapi32.dll","CredReadDomainCredentialsA",4},{"advapi32.dll","CredWriteDomainCredentialsW",3},{"advapi32.dll","CredWriteDomainCredentialsA",3},
        {"advapi32.dll","CredGetTargetInfoW",3},{"advapi32.dll","CredGetTargetInfoA",3}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_credentials(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with("W");
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    // CREDENTIAL 的欄位位移（Win32／x64）；FILETIME 以 4 位元組對齊。
    const unsigned record_size=x86_?52:80,o_target=8,o_comment=x86_?12:16,o_written=x86_?16:24,o_blob_size=x86_?24:32,o_blob=x86_?28:40,o_persist=x86_?32:48,o_count=x86_?36:52,o_attributes=x86_?40:56,o_alias=x86_?44:64,o_user=x86_?48:72;
    const unsigned attribute_size=x86_?16:24,a_flags=unit,a_size=unit+4,a_value=unit+8;
    const auto read_text=[&](std::uint64_t pointer){
        if(!pointer)return std::u16string{};
        return wide?read_utf16(memory,GuestAddress{pointer},32767):utf8_to_utf16(read_ansi(memory,pointer,32767));
    };
    const auto finish=[&](std::uint32_t error){last_error_=error;arg.return_to(state,error?0:1);return std::optional<std::uint32_t>{};};
    if(symbol=="CredFree"){
        if(arg(0) && !free_heap(memory,arg(0),state))throw HostCallFault("CredFree 收到無效的緩衝區");
        arg.return_to(state,0);return std::nullopt;
    }
    // CredMarshalCredential／CredUnmarshalCredential／CredIsMarshaledCredential：「@@」＋類型字元（'A'＋類型）＋位元流的 64 字元編碼（每個位元組自低位元起，每 6 位元一字）。
    // CertCredential（1）：SHA-1 雜湊 20 位元組（固定 27 字）；UsernameTargetCredential（2）：4 位元組長度＋UTF-16 名稱；BinaryBlobCredential（3）：4 位元組長度＋資料。
    // 輸出以 CredFree 釋放。CredGetSessionTypes：各認證類型（1～7）可用的最高保存範圍；本機儲存支援到 CRED_PERSIST_ENTERPRISE。
    if(symbol.starts_with("CredMarshalCredential") || symbol.starts_with("CredUnmarshalCredential") || symbol.starts_with("CredIsMarshaledCredential") || symbol=="CredGetSessionTypes"){
        static constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789#-";
        const auto encode=[](std::span<const std::uint8_t> bytes){
            std::u16string out;std::uint32_t value=0;unsigned count=0;
            for(const auto byte:bytes){value|=static_cast<std::uint32_t>(byte)<<count;count+=8;while(count>=6){out.push_back(static_cast<char16_t>(alphabet[value&63]));value>>=6;count-=6;}}
            if(count)out.push_back(static_cast<char16_t>(alphabet[value&63]));return out;};
        const auto parse=[](std::u16string_view text)->std::optional<std::pair<unsigned,Bytes>>{
            if(text.size()<3 || text[0]!=u'@' || text[1]!=u'@' || text[2]<u'B' || text[2]>u'D')return std::nullopt;
            const unsigned type=text[2]-u'A';const auto body=text.substr(3);Bytes bytes;std::uint32_t value=0;unsigned count=0;
            for(const auto c:body){const auto at=c<128?alphabet.find(static_cast<char>(c)):std::string_view::npos;if(at==std::string_view::npos)return std::nullopt;
                value|=static_cast<std::uint32_t>(at)<<count;count+=6;if(count>=8){bytes.push_back(static_cast<std::uint8_t>(value));value>>=8;count-=8;}}
            if(type==1){if(body.size()!=27)return std::nullopt;bytes.resize(20);return std::pair{type,bytes};}
            if(bytes.size()<4)return std::nullopt;const auto length=static_cast<std::uint64_t>(bytes[0])|static_cast<std::uint64_t>(bytes[1])<<8|static_cast<std::uint64_t>(bytes[2])<<16|static_cast<std::uint64_t>(bytes[3])<<24;
            if(4+length>bytes.size() || (type==2 && (!length || length%2)) || body.size()!=((4+length)*8+5)/6)return std::nullopt;bytes.resize(static_cast<std::size_t>(4+length));return std::pair{type,bytes};};
        const auto marshaled_text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer},32767):utf8_to_utf16(read_ansi(memory,pointer,32767));};
        try{
            if(symbol=="CredGetSessionTypes"){
                // (MaximumPersistCount, MaximumPersist)
                const auto count=static_cast<std::uint32_t>(arg(0));if(count && !arg(1))return finish(87);if(count>1024)return finish(87);
                for(std::uint32_t i=0;i<count;++i)memory.store(GuestAddress{arg(1)+4*i},32,i>=1 && i<=7?3U:0U);return finish(0);
            }
            if(symbol.starts_with("CredIsMarshaledCredential")){arg.return_to(state,arg(0) && parse(marshaled_text(arg(0)))?1U:0U);return std::nullopt;}
            if(symbol.starts_with("CredMarshalCredential")){
                // (CredType, Credential, MarshaledCredential)
                if(!arg(1) || !arg(2))return finish(87);memory.store(GuestAddress{arg(2)},bits,0);const auto type=static_cast<std::uint32_t>(arg(0));Bytes payload;
                const auto put_length=[&](std::uint64_t length){for(unsigned i=0;i<4;++i)payload.push_back(static_cast<std::uint8_t>(length>>(8*i)));};
                if(type==1){if(memory.load(GuestAddress{arg(1)},32)<24)return finish(87);payload.resize(20);memory.read(GuestAddress{arg(1)+4},payload);}
                else if(type==2){const auto user=memory.load(GuestAddress{arg(1)},bits);if(!user)return finish(87);const auto name=read_utf16(memory,GuestAddress{user},32767);if(name.empty())return finish(87);
                    auto bytes=utf16_bytes(name);bytes.resize(bytes.size()-2);put_length(bytes.size());payload.insert(payload.end(),bytes.begin(),bytes.end());}
                else if(type==3){const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));const auto blob=memory.load(GuestAddress{arg(1)+unit},bits);
                    if(size && !blob)return finish(87);if(size>16U*1024U*1024U)return finish(87);put_length(size);const auto start=payload.size();payload.resize(start+size);if(size)memory.read(GuestAddress{blob},std::span(payload).subspan(start));}
                else return finish(87);
                const auto text=u"@@"+std::u16string(1,static_cast<char16_t>(u'A'+type))+encode(payload);Bytes bytes;if(wide)bytes=utf16_bytes(text);else {const auto narrow=utf16_to_utf8(text);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
                const auto at=allocate_heap(memory,bytes.size());if(!at)return finish(8);memory.write(GuestAddress{at},bytes);memory.store(GuestAddress{arg(2)},bits,at);return finish(0);
            }
            // CredUnmarshalCredential(MarshaledCredential, CredType, Credential)
            if(!arg(0) || !arg(1) || !arg(2))return finish(87);memory.store(GuestAddress{arg(2)},bits,0);const auto parsed=parse(marshaled_text(arg(0)));if(!parsed)return finish(87);
            const auto& [type,payload]=*parsed;std::uint64_t at=0;
            if(type==1){at=allocate_heap(memory,24);if(!at)return finish(8);Bytes block(24);block[0]=24;std::copy(payload.begin(),payload.end(),block.begin()+4);memory.write(GuestAddress{at},block);}
            else if(type==2){Bytes name(payload.begin()+4,payload.end());name.push_back(0);name.push_back(0);at=allocate_heap(memory,unit+name.size());if(!at)return finish(8);
                memory.store(GuestAddress{at},bits,at+unit);memory.write(GuestAddress{at+unit},name);}
            else {const std::uint64_t size=payload.size()-4,header=x86_?8U:16U;at=allocate_heap(memory,header+size);if(!at)return finish(8);memory.store(GuestAddress{at},32,size);memory.store(GuestAddress{at+unit},bits,size?at+header:0);
                if(size)memory.write(GuestAddress{at+header},std::span(payload).subspan(4));}
            memory.store(GuestAddress{arg(1)},32,type);memory.store(GuestAddress{arg(2)},bits,at);return finish(0);
        }catch(const MemoryFault&){return finish(998);}
    }
    // 把一或多筆認證放進同一塊配置，呼叫端只需 CredFree 一次。
    const auto encode_text=[&](const std::u16string& text){
        Bytes out;
        if(wide){out=utf16_bytes(text);}
        else {const auto narrow=utf16_to_utf8(text);out.assign(narrow.begin(),narrow.end());out.push_back(0);}
        return out;
    };
    const auto block_size=[&](const Credential& item){
        std::uint64_t size=record_size+item.blob.size()+item.attributes.size()*attribute_size;
        for(const auto* text:{&item.target,&item.comment,&item.alias,&item.user})if(!text->empty())size+=encode_text(*text).size();
        for(const auto& attribute:item.attributes)size+=encode_text(attribute.keyword).size()+attribute.value.size();
        return size;
    };
    const auto write_record=[&](const Credential& item,std::uint64_t at){
        const auto base=at;auto next=at+record_size;
        const auto place=[&](const Bytes& bytes){const auto where=next;if(!bytes.empty())memory.write(GuestAddress{where},bytes);next+=bytes.size();return where;};
        const auto text=[&](unsigned offset,const std::u16string& value){memory.store(GuestAddress{base+offset},bits,value.empty()?0:place(encode_text(value)));};
        memory.store(GuestAddress{base},32,item.flags);memory.store(GuestAddress{base+4},32,item.type);
        text(o_target,item.target);text(o_comment,item.comment);text(o_alias,item.alias);text(o_user,item.user);
        memory.store(GuestAddress{base+o_written},64,item.last_written);memory.store(GuestAddress{base+o_blob_size},32,item.blob.size());
        memory.store(GuestAddress{base+o_blob},bits,item.blob.empty()?0:place(item.blob));
        memory.store(GuestAddress{base+o_persist},32,item.persist);memory.store(GuestAddress{base+o_count},32,item.attributes.size());
        if(item.attributes.empty())memory.store(GuestAddress{base+o_attributes},bits,0);
        else {
            const auto table=next;next+=item.attributes.size()*attribute_size;memory.store(GuestAddress{base+o_attributes},bits,table);
            for(std::size_t i=0;i<item.attributes.size();++i){
                const auto entry=table+i*attribute_size;const auto& attribute=item.attributes[i];
                memory.store(GuestAddress{entry},bits,place(encode_text(attribute.keyword)));memory.store(GuestAddress{entry+a_flags},32,attribute.flags);
                memory.store(GuestAddress{entry+a_size},32,attribute.value.size());memory.store(GuestAddress{entry+a_value},bits,attribute.value.empty()?0:place(attribute.value));
            }
        }
        return next;
    };
    try {
        const auto context=private_key_context();
        const auto load=[&](const std::string& name,Credential& item)->std::uint32_t {
            if(const auto session=session_credentials().find(name);session!=session_credentials().end()){item=session->second;return 0;}
            Bytes stored;if(const auto status=private_key_store_read(context,store_type,name,false,stored))return credential_status(status);
            item=parse(unprotect_sandbox_data(context,stored).bytes);return 0;
        };
        // 讀出客體 CREDENTIAL 並檢查欄位（CredWrite 與 CredWriteDomainCredentials 共用）。
        const auto capture=[&](std::uint64_t base,Credential& item)->std::uint32_t {
            memory.check(GuestAddress{base},record_size,Access::Read);
            item.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{base},32));item.type=static_cast<std::uint32_t>(memory.load(GuestAddress{base+4},32));
            item.target=read_text(memory.load(GuestAddress{base+o_target},bits));item.comment=read_text(memory.load(GuestAddress{base+o_comment},bits));
            item.alias=read_text(memory.load(GuestAddress{base+o_alias},bits));item.user=read_text(memory.load(GuestAddress{base+o_user},bits));
            item.persist=static_cast<std::uint32_t>(memory.load(GuestAddress{base+o_persist},32));
            const auto blob_size=memory.load(GuestAddress{base+o_blob_size},32),blob=memory.load(GuestAddress{base+o_blob},bits);
            const auto count=memory.load(GuestAddress{base+o_count},32),attributes=memory.load(GuestAddress{base+o_attributes},bits);
            if(item.target.empty() || item.type<1 || item.type>7 || item.persist<1 || item.persist>3 || blob_size>max_blob || (blob_size && !blob) || count>max_attributes || (count && !attributes))return invalid_parameter;
            item.blob.resize(blob_size);if(blob_size)memory.read(GuestAddress{blob},item.blob);
            item.attributes.resize(count);
            for(std::uint64_t i=0;i<count;++i){
                const auto entry=attributes+i*attribute_size;auto& attribute=item.attributes[i];
                attribute.keyword=read_text(memory.load(GuestAddress{entry},bits));attribute.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+a_flags},32));
                const auto size=memory.load(GuestAddress{entry+a_size},32),value=memory.load(GuestAddress{entry+a_value},bits);
                if(attribute.keyword.empty() || size>max_attribute_value || (size && !value))return invalid_parameter;
                attribute.value.resize(size);if(size)memory.read(GuestAddress{value},attribute.value);
            }
            return 0;
        };
        // 寫入儲存；CRED_PRESERVE_CREDENTIAL_BLOB 時 CredentialBlobSize 須為 0，內容沿用同名同類型的既有認證（不存在為 ERROR_NOT_FOUND）。
        const auto store=[&](Credential item,bool preserve)->std::uint32_t {
            const auto name=store_name(item.type,item.target);
            if(preserve){if(!item.blob.empty())return invalid_parameter;Credential existing;if(const auto status=load(name,existing))return status;item.blob=std::move(existing.blob);}
            item.flags&=~UINT32_C(1);item.last_written=runtime_.system_filetime();
            session_credentials().erase(name);
            if(item.persist==1){session_credentials()[name]=item;(void)private_key_store_delete(context,store_type,name,false);return 0;}
            const auto protect=protect_sandbox_data(context,serialize(item),u"Credential",false);
            // CredWrite 覆寫同名認證：儲存層 create 模式遇到既有項目回 NTE_EXISTS，改走更新。
            auto status=private_key_store_write(context,store_type,name,false,protect,true);
            if(status==0x8009000fU)status=private_key_store_write(context,store_type,name,false,protect,false);
            return status;
        };
        // CREDENTIAL_TARGET_INFORMATION：7 個名稱指標、Flags、CredTypeCount、CredTypes。
        const unsigned target_header=7*unit+8+unit,t_flags=7*unit,t_count=7*unit+4,t_types=7*unit+8;
        const auto read_target=[&](std::uint64_t base,TargetInfo& info)->std::uint32_t {
            memory.check(GuestAddress{base},target_header,Access::Read);
            for(unsigned i=0;i<info.names.size();++i)info.names[i]=read_text(memory.load(GuestAddress{base+i*unit},bits));
            info.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{base+t_flags},32));
            const auto count=memory.load(GuestAddress{base+t_count},32),types=memory.load(GuestAddress{base+t_types},bits);
            // 至少要有一個伺服器或網域名稱；類型只接受網域認證，CRED_TYPE_DOMAIN_EXTENDED 不可與其他類型混用。
            if(std::all_of(info.names.begin()+1,info.names.begin()+6,[](const auto& name){return name.empty();}))return invalid_parameter;
            if(count>max_target_types || (count && !types))return invalid_parameter;
            for(std::uint64_t i=0;i<count;++i){const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{types+i*4},32));if(!domain_type(type))return invalid_parameter;
                if(std::find(info.types.begin(),info.types.end(),type)==info.types.end())info.types.push_back(type);}
            if(info.types.size()>1 && std::find(info.types.begin(),info.types.end(),6U)!=info.types.end())return invalid_parameter;
            return 0;
        };
        // 一或多筆認證：指標陣列與各筆內容放在同一塊配置。
        const auto return_array=[&](const std::vector<Credential>& items,std::uint64_t count_out,std::uint64_t array_out){
            std::uint64_t total=items.size()*unit;for(const auto& item:items)total+=block_size(item);
            const auto at=allocate_heap(memory,total);if(!at)return finish(8);
            auto next=at+items.size()*unit;
            for(std::size_t i=0;i<items.size();++i){memory.store(GuestAddress{at+i*unit},bits,next);next=write_record(items[i],next);}
            memory.store(GuestAddress{count_out},32,items.size());memory.store(GuestAddress{array_out},bits,at);
            return finish(0);
        };
        if(symbol.starts_with("CredWriteDomainCredentials")){
            // (TargetInfo, Credential, Flags)：認證名稱須是 TargetInfo 的候選名稱之一，類型須為網域認證。
            if(!arg(0) || !arg(1))return finish(invalid_parameter);
            if(static_cast<std::uint32_t>(arg(2))&~1U)return finish(1004);
            TargetInfo info;if(const auto status=read_target(arg(0),info))return finish(status);
            Credential item;if(const auto status=capture(arg(1),item))return finish(status);
            if(!domain_type(item.type))return finish(invalid_parameter);
            const auto candidates=target_candidates(info);const auto name=folded(item.target);
            if(std::none_of(candidates.begin(),candidates.end(),[&](const auto& candidate){return folded(candidate)==name;}))return finish(invalid_parameter);
            return finish(store(std::move(item),static_cast<std::uint32_t>(arg(2))&1U));
        }
        if(symbol.starts_with("CredReadDomainCredentials")){
            // (TargetInfo, Flags, Count, Credential)：每個類型回傳最具體的一筆，依 CredTypes 順序（未指定時為網域憑證、網域密碼）。
            if(!arg(0) || !arg(2) || !arg(3))return finish(invalid_parameter);
            if(static_cast<std::uint32_t>(arg(1))&~1U)return finish(1004);
            memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(3)},unit,Access::Write);
            memory.store(GuestAddress{arg(2)},32,0);memory.store(GuestAddress{arg(3)},bits,0);
            TargetInfo info;if(const auto status=read_target(arg(0),info))return finish(status);
            if(static_cast<std::uint32_t>(arg(1))&1U){
                auto& cache=target_cache();const auto key=[](const TargetInfo& entry){return folded(!entry.names[0].empty()?entry.names[0]:!entry.names[2].empty()?entry.names[2]:entry.names[1]);};
                std::erase_if(cache,[&](const TargetInfo& entry){return key(entry)==key(info);});cache.insert(cache.begin(),info);if(cache.size()>max_cached_targets)cache.pop_back();
            }
            const auto types=info.types.empty()?std::vector<std::uint32_t>{3,2}:info.types;const auto candidates=target_candidates(info);
            std::vector<Credential> items;
            for(const auto type:types)for(const auto& candidate:candidates){
                Credential item;const auto status=load(store_name(type,candidate),item);
                if(!status){items.push_back(std::move(item));break;}
                if(status!=not_found)return finish(status);
            }
            if(items.empty())return finish(not_found);
            return return_array(items,arg(2),arg(3));
        }
        if(symbol.starts_with("CredGetTargetInfo")){
            // (TargetName, Flags, TargetInfo)：比對快取項目的 TargetName、DnsServerName 與 NetbiosServerName。
            // CRED_ALLOW_NAME_RESOLUTION 可接受；沙箱沒有網域名稱服務，不另做名稱解析。
            if(!arg(0) || !arg(2))return finish(invalid_parameter);
            if(static_cast<std::uint32_t>(arg(1))&~1U)return finish(1004);
            memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.store(GuestAddress{arg(2)},bits,0);
            const auto name=folded(read_text(arg(0)));if(name.empty())return finish(invalid_parameter);
            const auto& cache=target_cache();
            const auto found=std::find_if(cache.begin(),cache.end(),[&](const TargetInfo& entry){return folded(entry.names[0])==name || folded(entry.names[2])==name || folded(entry.names[1])==name;});
            if(found==cache.end())return finish(not_found);
            const auto& info=*found;std::uint64_t size=target_header+info.types.size()*4;
            for(const auto& text:info.names)if(!text.empty())size+=encode_text(text).size();
            const auto at=allocate_heap(memory,size);if(!at)return finish(8);
            auto next=at+target_header;const auto types=info.types.empty()?0:next;
            for(std::size_t i=0;i<info.types.size();++i)memory.store(GuestAddress{next+i*4},32,info.types[i]);next+=info.types.size()*4;
            for(unsigned i=0;i<info.names.size();++i){
                if(info.names[i].empty()){memory.store(GuestAddress{at+i*unit},bits,0);continue;}
                const auto bytes=encode_text(info.names[i]);memory.write(GuestAddress{next},bytes);memory.store(GuestAddress{at+i*unit},bits,next);next+=bytes.size();
            }
            memory.store(GuestAddress{at+t_flags},32,info.flags);memory.store(GuestAddress{at+t_count},32,info.types.size());memory.store(GuestAddress{at+t_types},bits,types);
            memory.store(GuestAddress{arg(2)},bits,at);return finish(0);
        }
        if(symbol.starts_with("CredWrite")){
            if(!arg(0))return finish(invalid_parameter);
            if(static_cast<std::uint32_t>(arg(1))&~1U)return finish(1004);
            Credential item;if(const auto status=capture(arg(0),item))return finish(status);
            return finish(store(std::move(item),static_cast<std::uint32_t>(arg(1))&1U));
        }
        if(symbol.starts_with("CredRead")){
            if(!arg(0) || !arg(3) || static_cast<std::uint32_t>(arg(2)))return finish(invalid_parameter);
            const auto type=static_cast<std::uint32_t>(arg(1));const auto target=read_text(arg(0));
            if(target.empty() || type<1 || type>7)return finish(invalid_parameter);
            memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.store(GuestAddress{arg(3)},bits,0);
            Credential item;if(const auto status=load(store_name(type,target),item))return finish(status);
            const auto at=allocate_heap(memory,block_size(item));if(!at)return finish(8);
            (void)write_record(item,at);memory.store(GuestAddress{arg(3)},bits,at);
            return finish(0);
        }
        if(symbol.starts_with("CredDelete")){
            if(!arg(0) || static_cast<std::uint32_t>(arg(2)))return finish(invalid_parameter);
            const auto type=static_cast<std::uint32_t>(arg(1));const auto target=read_text(arg(0));
            if(target.empty() || type<1 || type>7)return finish(invalid_parameter);
            const auto name=store_name(type,target);
            if(session_credentials().erase(name))return finish(0);
            const auto status=private_key_store_delete(context,store_type,name,false);
            return finish(credential_status(status));
        }
        // CredEnumerate：回傳指標陣列，陣列與各筆內容放在同一塊配置。
        if(!arg(2) || !arg(3) || static_cast<std::uint32_t>(arg(1))&~1U)return finish(invalid_parameter);
        const auto filter=(static_cast<std::uint32_t>(arg(1))&1U)?std::u16string{}:read_text(arg(0));
        if(!filter.empty()){const auto star=filter.find(u'*');if(star!=filter.npos && star+1!=filter.size())return finish(invalid_parameter);}
        memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(3)},unit,Access::Write);
        memory.store(GuestAddress{arg(2)},32,0);memory.store(GuestAddress{arg(3)},bits,0);
        std::vector<std::string> names;
        if(const auto status=private_key_store_list(context,store_type,false,names);status && credential_status(status)!=not_found)return finish(status);
        for(const auto& [name,value]:session_credentials()){(void)value;if(std::find(names.begin(),names.end(),name)==names.end())names.push_back(name);}
        std::vector<Credential> items;
        for(const auto& name:names){Credential item;if(load(name,item))continue;if(filter_matches(filter,item.target))items.push_back(std::move(item));}
        if(items.empty())return finish(not_found);
        return return_array(items,arg(2),arg(3));
    }catch(const CredentialError& error){return finish(error.code);}
    catch(const DataProtectionError&){return finish(13);}
    catch(const std::bad_alloc&){return finish(8);}
}
}
