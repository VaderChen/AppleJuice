#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "ldap_api.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#if defined(__APPLE__) && defined(__clang__)
// WinLDAP 需要 LDAP 協定與 BER 結果，OpenDirectory 不是可替換的 ABI；限此橋接層使用系統 OpenLDAP。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef X64ARM_HAS_LDAP
#include <ldap.h>
#include <lber.h>
#include <sasl/sasl.h>
#endif

namespace x64arm {
#ifdef X64ARM_HAS_LDAP
namespace {
unsigned ldap_status(int value){return value<0?80U+static_cast<unsigned>(-value):static_cast<unsigned>(value);}
struct LdapBindIdentity {
    std::string user, domain, password;
};
int ldap_bind_prompts(LDAP*, unsigned, void* context, void* prompts) {
    const auto& identity = *static_cast<const LdapBindIdentity*>(context);
    for (auto* prompt = static_cast<sasl_interact_t*>(prompts); prompt && prompt->id != SASL_CB_LIST_END; ++prompt) {
        const std::string* field;
        switch (prompt->id) {
            case SASL_CB_AUTHNAME: field = &identity.user; break;
            case SASL_CB_USER: prompt->result = ""; prompt->len = 0; continue;
            case SASL_CB_GETREALM: field = &identity.domain; break;
            case SASL_CB_PASS: field = &identity.password; break;
            default: return LDAP_PARAM_ERROR;
        }
        prompt->result = field->data();
        prompt->len = static_cast<unsigned>(field->size());
    }
    return LDAP_SUCCESS;
}
// 先從 Root DSE 取得伺服器提供的機制，交給 SASL 從雙方支援的密碼型機制中選擇。
int ldap_password_bind(LDAP* native, LdapBindIdentity& identity) {
    char attribute[] = "supportedSASLMechanisms";
    char* attributes[]{attribute,nullptr};
    LDAPMessage* root = nullptr;
    const int status = ldap_search_ext_s(native,"",LDAP_SCOPE_BASE,"(objectClass=*)",
        attributes,0,nullptr,nullptr,nullptr,1,&root);
    std::unique_ptr<LDAPMessage,decltype(&ldap_msgfree)> response(root,ldap_msgfree);
    if(status != LDAP_SUCCESS)return status;
    auto* entry = root ? ldap_first_entry(native,root) : nullptr;
    auto** values = entry ? ldap_get_values_len(native,entry,attribute) : nullptr;
    std::unique_ptr<berval*,decltype(&ldap_value_free_len)> owner(values,ldap_value_free_len);
    std::string mechanisms;
    for(unsigned i = 0; values && values[i]; ++i){
        std::string_view name(values[i]->bv_val,values[i]->bv_len);
        if(name == "NTLM" || name == "DIGEST-MD5"){
            if(!mechanisms.empty())mechanisms += ' ';
            mechanisms += name;
        }
    }
    if(mechanisms.empty())return LDAP_AUTH_METHOD_NOT_SUPPORTED;
    return ldap_sasl_interactive_bind_s(native,nullptr,mechanisms.c_str(),nullptr,nullptr,
        LDAP_SASL_QUIET,ldap_bind_prompts,&identity);
}
struct LdapConnection {
    LDAP* native{};
    ~LdapConnection(){if(native)ldap_unbind_ext_s(native,nullptr,nullptr);}
};
struct LdapMessage {
    LDAPMessage* native{};
    std::shared_ptr<LdapConnection> connection;
    ~LdapMessage(){if(native)ldap_msgfree(native);}
};
// ber_alloc_t／ber_init 建立的編解碼元素；marks 為 ber_first_element 給客體的不透明游標（序號＋1）。
struct LdapBer {
    BerElement* native{};
    std::vector<char*> marks;
    ~LdapBer(){if(native)ber_free(native,1);}
};
struct LdapCursor {
    BerElement* native{};
    LDAPMessage* entry{};
    std::shared_ptr<LdapMessage> message;
    ~LdapCursor(){if(native)ber_free(native,0);}
};
}
struct LdapState {
    std::map<std::uint64_t,std::shared_ptr<LdapConnection>> connections;
    std::map<std::uint64_t,std::shared_ptr<LdapMessage>> messages;
    struct Entry {LDAPMessage* native{};std::shared_ptr<LdapMessage> message;};
    std::map<std::uint64_t,Entry> entries;
    std::map<std::uint64_t,std::shared_ptr<LdapCursor>> cursors;
    std::set<std::uint64_t> strings,values;
    std::map<std::pair<unsigned,bool>,std::uint64_t> errors;
    // 擴充家族：字串清單（ldap_value_free）、berval（ber_bvfree）、控制項與控制項陣列、分頁搜尋與 ldap_startup 實例。
    std::set<std::uint64_t> lists,bervals,controls,control_arrays,instances;
    std::map<std::uint64_t,std::shared_ptr<struct LdapPagedSearch>> searches;
    std::map<std::uint64_t,std::shared_ptr<LdapBer>> bers;
    std::uint32_t debug_flags{};
};
struct LdapPagedSearch {
    std::shared_ptr<LdapConnection> connection;
    std::string base,filter;int scope{},attributes_only{};bool has_attributes{};std::vector<std::string> attributes;
    // 呼叫端的伺服器／用戶端控制項（深複製）與排序控制項。
    std::vector<std::pair<std::string,std::pair<std::string,bool>>> server_controls,client_controls;
    std::uint32_t page_time_limit{},total_size_limit{};std::string cookie;bool finished{};std::uint32_t total{};int pending{-1};
};
#endif
std::optional<std::uint32_t> WindowsServices::invoke_ldap(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& descriptor){
    const WindowsCall arg(state,memory,descriptor);const auto name=ldap_api::canonical(descriptor.symbol);
    const bool pointer_result=name.starts_with("ldap_init") || name.starts_with("ldap_err2string") || name=="ldap_first_entry" || name=="ldap_next_entry" || name.starts_with("ldap_get_dn") || name.starts_with("ldap_get_values_len") || name.starts_with("ldap_first_attribute") || name.starts_with("ldap_next_attribute");
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
#ifndef X64ARM_HAS_LDAP
    last_error_=92;return done(pointer_result?0:92);
#else
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 LDAP 外部 I/O");
    if(!ldap_)ldap_=std::make_shared<LdapState>();auto& data=*ldap_;
    const bool wide=name.ends_with('W');const unsigned pointer=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const auto ptr=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    // A 版字串以程序 ACP（UTF-8，GetACP＝65001）交換；協定上一律為 UTF-8。
    const auto text=[&](std::uint64_t at){return at?(wide?utf16_to_utf8(read_utf16(memory,GuestAddress{at})):read_ansi(memory,at)):std::string{};};
    const auto error=[&](unsigned code,bool null=false){last_error_=code;return done(null?0:code);};
    const auto allocate=[&](std::size_t size){const auto p=allocate_heap(memory,size);if(!p)throw std::bad_alloc();return p;};
    const auto string=[&](const char* value){
        if(!value)return std::uint64_t{};const auto unicode=utf8_to_utf16_lossy(value);std::vector<std::uint8_t> bytes;
        if(wide)bytes=utf16_bytes(unicode);else {const auto utf8=utf16_to_utf8(unicode);bytes.assign(utf8.begin(),utf8.end());bytes.push_back(0);}
        const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);data.strings.insert(p);return p;
    };
    try {
#include "windows_ldap_extended.inc"
        if(name=="LdapGetLastError")return done(last_error_);
        if(name.starts_with("ldap_err2string")){
            const auto code=static_cast<unsigned>(arg(0));const auto key=std::make_pair(code,wide);auto& cached=data.errors[key];
            if(!cached){cached=string(ldap_err2string(code>=81 && code<=97?-static_cast<int>(code-80):static_cast<int>(code)));data.strings.erase(cached);}return done(cached);
        }
        if(name.starts_with("ldap_memfree") || name=="ldap_value_free_len"){
            auto& allocations=name=="ldap_value_free_len"?data.values:data.strings;
            if(arg(0) && allocations.erase(arg(0)))(void)free_heap(memory,arg(0),state);return done(0);
        }
        if(name=="ber_free"){
            const auto found=data.cursors.find(arg(0));if(found!=data.cursors.end()){data.cursors.erase(found);(void)free_heap(memory,arg(0),state);}return done(0);
        }
        if(name=="ldap_msgfree"){
            const auto found=data.messages.find(arg(0));if(found==data.messages.end())return error(89);
            free_message(arg(0));return done(0);
        }
        if(name=="ldap_count_values_len"){
            if(!arg(0))return done(0);if(!data.values.contains(arg(0)))return error(89);unsigned count=0;while(ptr(arg(0)+count*pointer)){if(++count>65536)throw HostCallFault("LDAP 值清單超出容量");}return done(count);
        }
        if(name.starts_with("ldap_init")){
            const auto host=text(arg(0));const auto port=static_cast<std::uint32_t>(arg(1));
            if(data.connections.size()>=8192)return error(90,true);
            if(host.empty() || host.find_first_of("/ ?#@\\\r\n\t")!=host.npos || port>65535)return error(89,true);
            const auto uri="ldap://"+host+":"+std::to_string(port?port:389);auto connection=std::make_shared<LdapConnection>();
            const auto status=ldap_initialize(&connection->native,uri.c_str());if(status)return error(ldap_status(status),true);
            timeval timeout{5,0};ldap_set_option(connection->native,LDAP_OPT_NETWORK_TIMEOUT,&timeout);ldap_set_option(connection->native,LDAP_OPT_TIMEOUT,&timeout);
            const auto handle=allocate(16);data.connections.emplace(handle,std::move(connection));return done(handle);
        }
        const auto connection=data.connections.find(arg(0));if(connection==data.connections.end())return error(89,pointer_result);
        auto* native=connection->second->native;
        if(name=="ldap_unbind_s" || name=="ldap_unbind"){
            // 關閉 TCP session，但保留尚未釋放的搜尋結果之獨立生命週期。
            const auto status=ldap_unbind_ext_s(native,nullptr,nullptr);connection->second->native=nullptr;data.connections.erase(connection);(void)free_heap(memory,arg(0),state);return error(ldap_status(status));
        }
        if(name.starts_with("ldap_set_option") || name.starts_with("ldap_get_option")){
            const bool set=name.starts_with("ldap_set");const auto option=static_cast<int>(arg(1));int value=0,status=0;
            if(option!=LDAP_OPT_PROTOCOL_VERSION && option!=LDAP_OPT_SIZELIMIT && option!=LDAP_OPT_TIMELIMIT && option!=LDAP_OPT_REFERRALS && option!=LDAP_OPT_DEREF && option!=LDAP_OPT_RESULT_CODE)return error(92);
            if(!set && !arg(2))return error(89);
            if(set && option==LDAP_OPT_RESULT_CODE)return error(92);
            if(set && option==LDAP_OPT_REFERRALS)status=ldap_set_option(native,option,arg(2)?LDAP_OPT_ON:LDAP_OPT_OFF);
            else if(set){if(!arg(2))return error(89);value=static_cast<std::int32_t>(memory.load(GuestAddress{arg(2)},32));if(value<0 || (option==LDAP_OPT_PROTOCOL_VERSION && value!=2 && value!=3))return error(89);status=ldap_set_option(native,option,&value);}
            else {status=ldap_get_option(native,option,&value);if(!status)memory.store(GuestAddress{arg(2)},32,option==LDAP_OPT_RESULT_CODE?ldap_status(value):static_cast<std::uint32_t>(value));}
            return error(status?89:0);
        }
        if(name.starts_with("ldap_bind_s") && u32(3) != 0x80U){
            if(u32(3) != 0x486U)return error(89);
            // 沙箱沒有宿主登入票證。只使用客體明示的身分與密碼型 SASL 機制。
            const auto identity_at = arg(2);
            if(!identity_at)return error(49);
            const unsigned stride = pointer * 2;
            const auto flags = memory.load(GuestAddress{identity_at + 2*stride + pointer + 4},32);
            if(flags != 1 && flags != 2)return error(89);
            LdapBindIdentity identity;
            std::array fields{&identity.user, &identity.domain, &identity.password};
            for(unsigned i = 0; i < fields.size(); ++i){
                const auto slot = identity_at + i * stride;
                const auto address = ptr(slot);
                const auto length = memory.load(GuestAddress{slot + pointer},32);
                if(length > 4096 || (length && !address))return error(89);
                if(flags == 1){
                    *fields[i] = read_bytes(address,length);
                }else{
                    std::u16string value;
                    for(unsigned n = 0; n < length; ++n)
                        value.push_back(static_cast<char16_t>(memory.load(GuestAddress{address + 2*n},16)));
                    *fields[i] = utf16_to_utf8(value);
                }
            }
            return error(ldap_status(ldap_password_bind(native,identity)));
        }
        if(name.starts_with("ldap_simple_bind_s") || name.starts_with("ldap_bind_s")){
            const auto dn=text(arg(1)),password=text(arg(2));berval credential{password.size(),const_cast<char*>(password.data())};
            return error(ldap_status(ldap_sasl_bind_s(native,arg(1)?dn.c_str():nullptr,LDAP_SASL_SIMPLE,&credential,nullptr,nullptr,nullptr)));
        }
        if(name.starts_with("ldap_search_s")){
            if(!arg(6))return error(89);memory.store(GuestAddress{arg(6)},bits,0);const auto scope=static_cast<int>(arg(2));if(scope<0 || scope>2)return error(89);
            const auto base=text(arg(1)),filter=arg(3)?text(arg(3)):std::string("(objectClass=*)");std::vector<std::string> strings;std::vector<char*> attributes;
            if(arg(4)){for(unsigned i=0;;++i){if(i>=1024)return error(89);const auto p=ptr(arg(4)+i*pointer);if(!p)break;strings.push_back(text(p));}
                for(auto& value:strings)attributes.push_back(value.data());attributes.push_back(nullptr);}
            auto message=std::make_shared<LdapMessage>();message->connection=connection->second;
            const auto status=ldap_search_ext_s(native,base.c_str(),scope,filter.c_str(),arg(4)?attributes.data():nullptr,static_cast<std::uint32_t>(arg(5))?1:0,nullptr,nullptr,nullptr,0,&message->native);
            if(message->native){const auto handle=allocate(16);data.messages.emplace(handle,std::move(message));memory.store(GuestAddress{arg(6)},bits,handle);}return error(ldap_status(status));
        }
        if(name=="ldap_first_entry" || name=="ldap_next_entry" || name=="ldap_count_entries"){
            std::shared_ptr<LdapMessage> owner;LDAPMessage* result=nullptr;
            if(name=="ldap_next_entry"){const auto found=data.entries.find(arg(1));if(found==data.entries.end())return error(89,true);owner=found->second.message;if(owner->connection!=connection->second)return error(89,true);result=ldap_next_entry(native,found->second.native);}
            else {const auto found=data.messages.find(arg(1));if(found==data.messages.end())return error(89,name!="ldap_count_entries");owner=found->second;
                if(owner->connection!=connection->second)return error(89,name!="ldap_count_entries");if(name=="ldap_count_entries")return done(static_cast<std::uint32_t>(ldap_count_entries(native,owner->native)));result=ldap_first_entry(native,owner->native);}
            if(owner->connection!=connection->second)return error(89,true);if(!result)return done(0);
            for(const auto& [handle,entry]:data.entries)if(entry.native==result && entry.message==owner)return done(handle);
            const auto handle=allocate(16);data.entries.emplace(handle,LdapState::Entry{result,std::move(owner)});return done(handle);
        }
        const auto entry=data.entries.find(arg(1));if(entry==data.entries.end() || entry->second.message->connection!=connection->second)return error(89,true);
        if(name.starts_with("ldap_get_dn")){
            auto* result=ldap_get_dn(native,entry->second.native);if(!result)return done(0);std::unique_ptr<char,decltype(&ldap_memfree)> holder(result,ldap_memfree);return done(string(result));
        }
        if(name.starts_with("ldap_first_attribute") || name.starts_with("ldap_next_attribute")){
            char* result=nullptr;
            if(name.starts_with("ldap_first")){
                if(!arg(2))return error(89,true);memory.store(GuestAddress{arg(2)},bits,0);auto cursor=std::make_shared<LdapCursor>();cursor->message=entry->second.message;cursor->entry=entry->second.native;
                result=ldap_first_attribute(native,entry->second.native,&cursor->native);
                if(cursor->native){const auto handle=allocate(16);data.cursors.emplace(handle,std::move(cursor));memory.store(GuestAddress{arg(2)},bits,handle);}
            }else {const auto cursor=data.cursors.find(arg(2));if(cursor==data.cursors.end() || cursor->second->message!=entry->second.message || cursor->second->entry!=entry->second.native)return error(89,true);result=ldap_next_attribute(native,entry->second.native,cursor->second->native);}
            if(!result)return done(0);std::unique_ptr<char,decltype(&ldap_memfree)> holder(result,ldap_memfree);return done(string(result));
        }
        if(name.starts_with("ldap_get_values_len")){
            if(!arg(2))return error(89,true);const auto attribute=text(arg(2));auto** values=ldap_get_values_len(native,entry->second.native,attribute.c_str());if(!values)return done(0);
            struct Guard {berval** values;~Guard(){ldap_value_free_len(values);}} guard{values};
            const auto native_count=ldap_count_values_len(values);if(native_count<0 || native_count>65536)return error(89,true);if(!native_count)return done(0);const auto count=static_cast<unsigned>(native_count);
            const unsigned header=x86_?8:16;std::uint64_t size=(count+1)*pointer+count*header;
            for(unsigned i=0;i<count;++i){if(values[i]->bv_len>output_limit || size+values[i]->bv_len>output_limit)return error(90,true);size+=values[i]->bv_len;}
            const auto output=allocate(size);memory.fill(GuestAddress{output},0,size);std::uint64_t bytes=output+(count+1)*pointer+count*header;
            for(unsigned i=0;i<count;++i){const auto item=output+(count+1)*pointer+i*header;memory.store(GuestAddress{output+i*pointer},bits,item);memory.store(GuestAddress{item},32,values[i]->bv_len);memory.store(GuestAddress{item+pointer},bits,bytes);
                memory.write(GuestAddress{bytes},{reinterpret_cast<const std::uint8_t*>(values[i]->bv_val),values[i]->bv_len});bytes+=values[i]->bv_len;}
            data.values.insert(output);return done(output);
        }
        throw HostCallFault("LDAP 入口沒有共用派送");
    }catch(const std::bad_alloc&){return error(90,pointer_result);}
#endif
}
}

#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif
