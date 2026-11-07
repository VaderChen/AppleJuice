// Windows 認證管理員（advapi32 Cred*）。依專案規定認證只留在 sandbox 的 C: 磁碟，依虛擬使用者 SID 隔離，
// 不使用 macOS 鑰匙圈或宿主家目錄；內容以 DPAPI 同一套 sandbox 金鑰加密後放進私鑰儲存。
// CRED_PERSIST_SESSION 只存在本程序的記憶體（真實系統是登入工作階段，本專案沒有跨程序的工作階段服務）。
#include "x64arm/windows_services.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
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
}
std::span<const WindowsServiceDescriptor> credential_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","CredReadW",4},{"advapi32.dll","CredReadA",4},{"advapi32.dll","CredWriteW",2},{"advapi32.dll","CredWriteA",2},
        {"advapi32.dll","CredDeleteW",3},{"advapi32.dll","CredDeleteA",3},{"advapi32.dll","CredEnumerateW",4},{"advapi32.dll","CredEnumerateA",4},
        {"advapi32.dll","CredFree",1}};
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
        if(symbol.starts_with("CredWrite")){
            if(!arg(0) || static_cast<std::uint32_t>(arg(1))&~1U)return finish(invalid_parameter);
            const auto base=arg(0);memory.check(GuestAddress{base},record_size,Access::Read);
            Credential item;item.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{base},32));item.type=static_cast<std::uint32_t>(memory.load(GuestAddress{base+4},32));
            item.target=read_text(memory.load(GuestAddress{base+o_target},bits));item.comment=read_text(memory.load(GuestAddress{base+o_comment},bits));
            item.alias=read_text(memory.load(GuestAddress{base+o_alias},bits));item.user=read_text(memory.load(GuestAddress{base+o_user},bits));
            item.persist=static_cast<std::uint32_t>(memory.load(GuestAddress{base+o_persist},32));
            const auto blob_size=memory.load(GuestAddress{base+o_blob_size},32),blob=memory.load(GuestAddress{base+o_blob},bits);
            const auto count=memory.load(GuestAddress{base+o_count},32),attributes=memory.load(GuestAddress{base+o_attributes},bits);
            if(item.target.empty() || item.type<1 || item.type>7 || item.persist<1 || item.persist>3 || blob_size>max_blob || (blob_size && !blob) || count>max_attributes || (count && !attributes))return finish(invalid_parameter);
            item.blob.resize(blob_size);if(blob_size)memory.read(GuestAddress{blob},item.blob);
            item.attributes.resize(count);
            for(std::uint64_t i=0;i<count;++i){
                const auto entry=attributes+i*attribute_size;auto& attribute=item.attributes[i];
                attribute.keyword=read_text(memory.load(GuestAddress{entry},bits));attribute.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+a_flags},32));
                const auto size=memory.load(GuestAddress{entry+a_size},32),value=memory.load(GuestAddress{entry+a_value},bits);
                if(attribute.keyword.empty() || size>max_attribute_value || (size && !value))return finish(invalid_parameter);
                attribute.value.resize(size);if(size)memory.read(GuestAddress{value},attribute.value);
            }
            item.flags&=~UINT32_C(1);item.last_written=runtime_.system_filetime();
            const auto name=store_name(item.type,item.target);
            session_credentials().erase(name);
            if(item.persist==1){session_credentials()[name]=item;(void)private_key_store_delete(context,store_type,name,false);return finish(0);}
            const auto protect=protect_sandbox_data(context,serialize(item),u"Credential",false);
            // CredWrite 覆寫同名認證：儲存層 create 模式遇到既有項目回 NTE_EXISTS，改走更新。
            auto status=private_key_store_write(context,store_type,name,false,protect,true);
            if(status==0x8009000fU)status=private_key_store_write(context,store_type,name,false,protect,false);
            return finish(status);
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
        std::uint64_t total=items.size()*unit;for(const auto& item:items)total+=block_size(item);
        const auto at=allocate_heap(memory,total);if(!at)return finish(8);
        auto next=at+items.size()*unit;
        for(std::size_t i=0;i<items.size();++i){memory.store(GuestAddress{at+i*unit},bits,next);next=write_record(items[i],next);}
        memory.store(GuestAddress{arg(2)},32,items.size());memory.store(GuestAddress{arg(3)},bits,at);
        return finish(0);
    }catch(const CredentialError& error){return finish(error.code);}
    catch(const DataProtectionError&){return finish(13);}
    catch(const std::bad_alloc&){return finish(8);}
}
}
