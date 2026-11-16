// wldap32 擴充家族：寫入／比較／更名／延伸操作、非同步訊息與結果解析、參照、控制項（分頁、排序、VLV）、
// 分頁搜尋、DN 與篩選字串工具、字串／berval 清單與版本資訊。全部以系統 OpenLDAP 執行協定，客體結構依 WinLDAP 版面轉換。
const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
const auto free_message=[&](std::uint64_t handle){
    const auto found=data.messages.find(handle);if(found==data.messages.end())return false;const auto message=found->second;
    for(auto it=data.entries.begin();it!=data.entries.end();)if(it->second.message==message){(void)free_heap(memory,it->first,state);it=data.entries.erase(it);}else ++it;
    for(auto it=data.cursors.begin();it!=data.cursors.end();)if(it->second->message==message){(void)free_heap(memory,it->first,state);it=data.cursors.erase(it);}else ++it;
    data.messages.erase(found);(void)free_heap(memory,handle,state);return true;
};
struct LdapFailure {unsigned code;};
const auto read_bytes=[&](std::uint64_t at,std::uint64_t size){
    if(size>output_limit)throw LdapFailure{90};std::string bytes(static_cast<std::size_t>(size),'\0');
    if(size){if(!at)throw LdapFailure{89};memory.read(GuestAddress{at},std::span(reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()));}return bytes;
};
// WinLDAP berval：{ULONG bv_len; PCHAR bv_val}，指標對齊。
const auto read_berval=[&](std::uint64_t at){return read_bytes(ptr(at+pointer),memory.load(GuestAddress{at},32));};
const auto read_strings=[&](std::uint64_t at){std::vector<std::string> list;if(!at)return list;for(unsigned i=0;;++i){if(i>=4096)throw LdapFailure{89};const auto p=ptr(at+i*pointer);if(!p)break;list.push_back(text(p));}return list;};
const auto read_bervals=[&](std::uint64_t at){std::vector<std::string> list;if(!at)return list;for(unsigned i=0;;++i){if(i>=4096)throw LdapFailure{89};const auto p=ptr(at+i*pointer);if(!p)break;list.push_back(read_berval(p));}return list;};
const auto encode_text=[&](const std::string& value){std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(utf8_to_utf16_lossy(value));else {bytes.assign(value.begin(),value.end());bytes.push_back(0);}return bytes;};
// 字串清單：一個區塊內含 (n+1) 個指標與字串，以 ldap_value_free 釋放。
const auto string_list=[&](const std::vector<std::string>& items)->std::uint64_t{
    std::vector<std::vector<std::uint8_t>> encoded;std::uint64_t size=(items.size()+1)*pointer;for(const auto& item:items){encoded.push_back(encode_text(item));size+=encoded.back().size();}
    if(size>output_limit)throw LdapFailure{90};const auto block=allocate(size);std::uint64_t cursor=block+(items.size()+1)*pointer;
    for(std::size_t i=0;i<encoded.size();++i){memory.store(GuestAddress{block+i*pointer},bits,cursor);memory.write(GuestAddress{cursor},encoded[i]);cursor+=encoded[i].size();}
    memory.store(GuestAddress{block+items.size()*pointer},bits,0);data.lists.insert(block);return block;
};
const auto make_berval=[&](std::string_view bytes)->std::uint64_t{
    const auto header=2*pointer;const auto block=allocate(header+bytes.size()+1);memory.fill(GuestAddress{block},0,header+bytes.size()+1);
    memory.store(GuestAddress{block},32,bytes.size());memory.store(GuestAddress{block+pointer},bits,block+header);
    if(!bytes.empty())memory.write(GuestAddress{block+header},std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));data.bervals.insert(block);return block;
};
// 控制項：客體 LDAPControl {PCHAR ldctl_oid; berval ldctl_value; BOOLEAN ldctl_iscritical} → 原生陣列（擁有字串）。
struct Controls {std::vector<std::string> oids,values;std::vector<LDAPControl> items;std::vector<LDAPControl*> array;LDAPControl** get(){return array.empty()?nullptr:array.data();}};
const auto read_control=[&](std::uint64_t at,Controls& controls){
    controls.oids.push_back(text(ptr(at)));controls.values.push_back(read_bytes(ptr(at+2*pointer),memory.load(GuestAddress{at+pointer},32)));
    LDAPControl control{};control.ldctl_oid=controls.oids.back().data();control.ldctl_value.bv_len=controls.values.back().size();control.ldctl_value.bv_val=controls.values.back().data();
    control.ldctl_iscritical=static_cast<char>(memory.load(GuestAddress{at+3*pointer},8)?1:0);controls.items.push_back(control);
};
const auto read_controls=[&](std::uint64_t at,Controls& controls){
    if(!at)return;std::vector<std::uint64_t> pointers;for(unsigned i=0;;++i){if(i>=256)throw LdapFailure{89};const auto p=ptr(at+i*pointer);if(!p)break;pointers.push_back(p);}
    controls.oids.reserve(pointers.size());controls.values.reserve(pointers.size());controls.items.reserve(pointers.size());
    for(const auto p:pointers)read_control(p,controls);for(auto& item:controls.items)controls.array.push_back(&item);controls.array.push_back(nullptr);
};
// 原生控制項 → 客體：單一控制項（ldap_control_free）或陣列（ldap_controls_free），結構、OID 與值放在同一區塊。
const auto control_size=4*pointer;
const auto write_control_at=[&](std::uint64_t at,std::uint64_t& cursor,const LDAPControl& control){
    const auto oid=encode_text(control.ldctl_oid?control.ldctl_oid:"");memory.store(GuestAddress{at},bits,cursor);memory.write(GuestAddress{cursor},oid);cursor+=oid.size();
    memory.store(GuestAddress{at+pointer},32,control.ldctl_value.bv_len);memory.store(GuestAddress{at+2*pointer},bits,control.ldctl_value.bv_len?cursor:0);
    if(control.ldctl_value.bv_len){memory.write(GuestAddress{cursor},std::span(reinterpret_cast<const std::uint8_t*>(control.ldctl_value.bv_val),control.ldctl_value.bv_len));cursor+=control.ldctl_value.bv_len;}
    memory.store(GuestAddress{at+3*pointer},8,control.ldctl_iscritical?1:0);cursor=(cursor+pointer-1)/pointer*pointer;
};
const auto control_bytes=[&](const LDAPControl& control){return encode_text(control.ldctl_oid?control.ldctl_oid:"").size()+control.ldctl_value.bv_len+pointer;};
const auto write_control=[&](const LDAPControl& control){
    const auto size=control_size+control_bytes(control);const auto block=allocate(size);memory.fill(GuestAddress{block},0,size);
    std::uint64_t cursor=block+control_size;write_control_at(block,cursor,control);data.controls.insert(block);return block;
};
const auto write_controls=[&](LDAPControl** controls)->std::uint64_t{
    if(!controls || !controls[0])return 0;std::size_t count=0;std::uint64_t size=0;for(;controls[count];++count)size+=control_size+control_bytes(*controls[count]);
    size+=(count+1)*pointer;if(size>output_limit)throw LdapFailure{90};const auto block=allocate(size);memory.fill(GuestAddress{block},0,size);
    std::uint64_t cursor=block+(count+1)*pointer+count*control_size;
    for(std::size_t i=0;i<count;++i){const auto at=block+(count+1)*pointer+i*control_size;memory.store(GuestAddress{block+i*pointer},bits,at);write_control_at(at,cursor,*controls[i]);}
    data.control_arrays.insert(block);return block;
};
// LDAPMod {ULONG mod_op; PCHAR mod_type; PCHAR* 或 berval** mod_vals}；原生一律以 LDAP_MOD_BVALUES 傳遞。
struct Mods {std::vector<std::string> types;std::vector<std::vector<std::string>> data;std::vector<std::vector<berval>> values;std::vector<std::vector<berval*>> lists;std::vector<LDAPMod> items;std::vector<LDAPMod*> array;};
const auto read_mods=[&](std::uint64_t at,Mods& mods){
    if(!at)throw LdapFailure{89};std::vector<std::uint64_t> pointers;for(unsigned i=0;;++i){if(i>=4096)throw LdapFailure{89};const auto p=ptr(at+i*pointer);if(!p)break;pointers.push_back(p);}
    const auto count=pointers.size();mods.types.reserve(count);mods.data.reserve(count);mods.values.reserve(count);mods.lists.reserve(count);mods.items.reserve(count);
    for(const auto p:pointers){
        const auto op=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));if((op&~0x80U)>2)throw LdapFailure{89};
        mods.types.push_back(text(ptr(p+pointer)));const auto list=ptr(p+2*pointer);
        mods.data.push_back((op&0x80U)?read_bervals(list):read_strings(list));auto& raw=mods.data.back();
        mods.values.emplace_back();for(auto& value:raw)mods.values.back().push_back(berval{value.size(),value.data()});
        mods.lists.emplace_back();for(auto& value:mods.values.back())mods.lists.back().push_back(&value);mods.lists.back().push_back(nullptr);
        LDAPMod mod{};mod.mod_op=static_cast<int>(op&~0x80U)|LDAP_MOD_BVALUES;mod.mod_type=mods.types.back().data();mod.mod_bvalues=list?mods.lists.back().data():nullptr;mods.items.push_back(mod);
    }
    for(auto& item:mods.items)mods.array.push_back(&item);mods.array.push_back(nullptr);
};
const auto native_timeout=[&](std::uint64_t at)->std::optional<timeval>{if(!at)return std::nullopt;return timeval{static_cast<std::int32_t>(memory.load(GuestAddress{at},32)),static_cast<std::int32_t>(memory.load(GuestAddress{at+4},32))};};
const auto connection_of=[&](std::uint64_t handle){const auto found=data.connections.find(handle);if(found==data.connections.end() || !found->second->native)throw LdapFailure{89};return found->second;};
const auto message_of=[&](std::uint64_t handle){const auto found=data.messages.find(handle);if(found==data.messages.end())throw LdapFailure{89};return found->second;};
const auto store_message=[&](const std::shared_ptr<LdapConnection>& connection,LDAPMessage* native,std::uint64_t at){
    auto message=std::make_shared<LdapMessage>();message->connection=connection;message->native=native;const auto handle=allocate(16);data.messages.emplace(handle,std::move(message));if(at)memory.store(GuestAddress{at},bits,handle);return handle;
};
const auto entry_handle=[&](LDAPMessage* native,const std::shared_ptr<LdapMessage>& owner)->std::uint64_t{
    if(!native)return 0;for(const auto& [handle,entry]:data.entries)if(entry.native==native && entry.message==owner)return handle;
    const auto handle=allocate(16);data.entries.emplace(handle,LdapState::Entry{native,owner});return handle;
};
const auto connection_handle=[&](const std::shared_ptr<LdapConnection>& connection)->std::uint64_t{for(const auto& [handle,item]:data.connections)if(item==connection)return handle;return 0;};
const auto open_connection=[&](const std::string& host,std::uint32_t port,bool secure,bool connect)->std::uint64_t{
    if(data.connections.size()>=8192)throw LdapFailure{90};
    if(host.empty() || host.find_first_of("/?#@\\\r\n\t")!=host.npos || port>65535)throw LdapFailure{89};
    auto connection=std::make_shared<LdapConnection>();timeval timeout{5,0};
    if(connect){
        // ldap_open 建立連線物件並立即連線（不繫結）；先設定程式庫預設的網路逾時，避免無法連線時長時間阻塞。
        ldap_set_option(nullptr,LDAP_OPT_NETWORK_TIMEOUT,&timeout);connection->native=ldap_open(host.c_str(),static_cast<int>(port?port:389U));
        if(!connection->native)throw LdapFailure{0x51};
    }else{
        // 主機可為空白分隔清單，每個項目可帶 :port；secure 時使用 ldaps。
        std::string uri;std::size_t begin=0;
        while(begin<host.size()){const auto end=std::min(host.find(' ',begin),host.size());const auto item=host.substr(begin,end-begin);begin=end+1;if(item.empty())continue;
            if(!uri.empty())uri+=' ';uri+=(secure?"ldaps://":"ldap://")+item+(item.find(':')==item.npos?":"+std::to_string(port?port:(secure?636U:389U)):"");}
        if(uri.empty())throw LdapFailure{89};
        const auto status=ldap_initialize(&connection->native,uri.c_str());if(status)throw LdapFailure{ldap_status(status)};
    }
    ldap_set_option(connection->native,LDAP_OPT_NETWORK_TIMEOUT,&timeout);ldap_set_option(connection->native,LDAP_OPT_TIMEOUT,&timeout);
    int version=LDAP_VERSION3;ldap_set_option(connection->native,LDAP_OPT_PROTOCOL_VERSION,&version);
    const auto handle=allocate(16);data.connections.emplace(handle,std::move(connection));return handle;
};
const auto first_control=[&](LDAPControl** controls,const char* oid)->LDAPControl*{if(!controls)return nullptr;for(auto** it=controls;*it;++it)if((*it)->ldctl_oid && std::string_view((*it)->ldctl_oid)==oid)return *it;return nullptr;};
// 搜尋共用：(base, scope, filter, attrs, attrsonly)。
struct SearchArguments {std::string base,filter;int scope{},attributes_only{};std::vector<std::string> attributes;std::vector<char*> pointers;bool has_attributes{};char** list(){return has_attributes?pointers.data():nullptr;}};
const auto read_search=[&](unsigned first,SearchArguments& search){
    search.base=text(arg(first));search.scope=static_cast<int>(u32(first+1));if(search.scope<0 || search.scope>2)throw LdapFailure{89};
    search.filter=arg(first+2)?text(arg(first+2)):std::string("(objectClass=*)");search.has_attributes=arg(first+3)!=0;search.attributes=read_strings(arg(first+3));
    for(auto& item:search.attributes)search.pointers.push_back(item.data());search.pointers.push_back(nullptr);search.attributes_only=u32(first+4)?1:0;
};
// LdapMapErrorToWin32：結果碼對應 winerror.h 的 ERROR_DS_* 群組，用戶端錯誤對應一般 Win32 錯誤。
const auto win32_error=[](std::uint32_t code)->std::uint32_t{
    switch(code){
    case 0:return 0;case 1:return 8224;case 2:return 8225;case 3:return 8226;case 4:return 8227;case 5:return 8229;case 6:return 8230;case 7:return 8231;case 8:return 8232;
    case 9:case 10:return 8235;case 11:return 8228;case 12:return 8236;case 13:return 8237;case 14:return 234;case 16:return 8202;case 17:return 8204;case 18:return 8238;
    case 19:return 8239;case 20:return 8205;case 21:return 8203;case 32:return 8240;case 33:return 8241;case 34:return 8242;case 35:return 8243;case 36:return 8244;
    case 48:return 8233;case 49:return 1326;case 50:return 5;case 51:return 8206;case 52:return 8207;case 53:return 8245;case 54:return 8246;case 64:return 8247;
    case 65:return 8212;case 66:return 8213;case 67:return 8214;case 68:return 5010;case 69:return 8215;case 70:return 8248;case 71:return 8249;case 80:return 8341;
    case 0x51:return 8250;case 0x52:return 8251;case 0x53:return 8252;case 0x54:return 8253;case 0x55:return 1460;case 0x56:return 8234;case 0x57:return 8254;
    case 0x58:return 1223;case 0x59:return 87;case 0x5a:return 8;case 0x5b:return 1225;case 0x5c:return 50;case 0x5d:return 8258;case 0x5e:return 8257;case 0x5f:return 234;
    case 0x60:return 8259;case 0x61:return 8260;default:return 8341;
    }
};
// RFC 4515 篩選語法檢查（WinLDAP 也接受最外層不加括號的單一項目）。
const auto valid_filter=[](std::string_view filter){
    std::size_t at=0;
    const std::function<bool(bool)> parse=[&](bool outer)->bool{
        if(at<filter.size() && filter[at]=='('){
            ++at;if(at>=filter.size())return false;
            if(filter[at]=='&' || filter[at]=='|'){++at;std::size_t count=0;while(at<filter.size() && filter[at]=='('){if(!parse(false))return false;++count;}if(!count)return false;}
            else if(filter[at]=='!'){++at;if(!parse(false))return false;}
            else {const auto close=filter.find(')',at);if(close==filter.npos)return false;const auto item=filter.substr(at,close-at);at=close;
                const auto equal=item.find('=');if(equal==item.npos || equal==0)return false;auto attribute=item.substr(0,equal);if(attribute.back()=='~' || attribute.back()=='>' || attribute.back()=='<' || attribute.back()==':')attribute.remove_suffix(1);
                if(attribute.empty() || attribute.find_first_of("()*\\ ")!=attribute.npos)return false;for(std::size_t i=equal+1;i<item.size();++i)if(item[i]=='\\'){if(i+2>=item.size())return false;if(!std::isxdigit(static_cast<unsigned char>(item[i+1])) || !std::isxdigit(static_cast<unsigned char>(item[i+2])))return false;i+=2;}}
            if(at>=filter.size() || filter[at]!=')')return false;++at;return true;
        }
        if(!outer)return false;const auto equal=filter.find('=');if(equal==filter.npos || equal==0 || filter.find_first_of("()")!=filter.npos)return false;at=filter.size();return true;
    };
    return parse(true) && at==filter.size();
};
const auto pointer_function=name.starts_with("ldap_open") || name.starts_with("ldap_sslinit") || name.starts_with("ldap_explode_dn") || name.starts_with("ldap_dn2ufn") || (name.starts_with("ldap_get_values") && !name.starts_with("ldap_get_values_len")) ||
    name=="ldap_first_reference" || name=="ldap_next_reference" || name=="ldap_conn_from_msg" || name.starts_with("ldap_search_init_page") || name=="ber_bvdup";
const auto message_function=name=="ldap_add" || name=="ldap_addA" || name=="ldap_addW" || name=="ldap_modify" || name=="ldap_modifyA" || name=="ldap_modifyW" || name=="ldap_delete" || name=="ldap_deleteA" || name=="ldap_deleteW" ||
    name=="ldap_compare" || name=="ldap_compareA" || name=="ldap_compareW" || name=="ldap_modrdn" || name=="ldap_modrdnA" || name=="ldap_modrdnW" || name=="ldap_modrdn2" || name=="ldap_modrdn2A" || name=="ldap_modrdn2W" ||
    name=="ldap_search" || name=="ldap_searchA" || name=="ldap_searchW" || name=="ldap_result" || name=="ber_printf" || name=="ber_scanf" || name=="ber_flatten";
const auto extended_name=[&](std::string_view base){return name==base || (name.size()==base.size()+1 && name.starts_with(base) && (name.back()=='A' || name.back()=='W'));};
try{
    if(name=="LdapMapErrorToWin32")return done(win32_error(u32(0)));
    if(name=="LdapUTF8ToUnicode" || name=="LdapUnicodeToUTF8"){
        // (lpSrcStr, cchSrc, lpDestStr, cchDest)：cchSrc 為 -1 時包含結尾 NUL；cchDest 為 0 時回傳所需字元數。
        const bool to_unicode=name=="LdapUTF8ToUnicode";const auto source=arg(0);auto count=static_cast<std::int32_t>(u32(1));const auto dest=arg(2);const auto capacity=static_cast<std::int32_t>(u32(3));
        if(!source || count<-1 || capacity<0)return error(87,true);
        std::u16string unicode;
        if(to_unicode){std::string input=count==-1?read_ansi(memory,source)+std::string(1,'\0'):read_bytes(source,static_cast<std::uint64_t>(count));unicode=utf8_to_utf16_lossy(input);}
        else {if(count==-1){unicode=read_utf16(memory,GuestAddress{source});unicode.push_back(0);}else {for(std::int32_t i=0;i<count;++i)unicode.push_back(static_cast<char16_t>(memory.load(GuestAddress{source+static_cast<std::uint64_t>(i)*2},16)));}}
        if(to_unicode){const auto required=static_cast<std::int32_t>(unicode.size());if(!capacity)return done(static_cast<std::uint32_t>(required));if(!dest || capacity<required)return error(122,true);
            for(std::size_t i=0;i<unicode.size();++i)memory.store(GuestAddress{dest+i*2},16,unicode[i]);return done(static_cast<std::uint32_t>(required));}
        const auto utf8=utf16_to_utf8(unicode);const auto required=static_cast<std::int32_t>(utf8.size());if(!capacity)return done(static_cast<std::uint32_t>(required));if(!dest || capacity<required)return error(122,true);
        if(!utf8.empty())memory.write(GuestAddress{dest},std::span(reinterpret_cast<const std::uint8_t*>(utf8.data()),utf8.size()));return done(static_cast<std::uint32_t>(required));
    }
    if(extended_name("ldap_count_values")){if(!arg(0))return done(0);unsigned count=0;while(ptr(arg(0)+count*pointer)){if(++count>65536)return error(89);}return done(count);}
    if(extended_name("ldap_value_free")){if(arg(0) && data.lists.erase(arg(0)))(void)free_heap(memory,arg(0),state);return done(0);}
    // OpenLDAP 把非 ASCII 位元組寫成 \XX；WinLDAP 回傳可讀的 UTF-8／Unicode，因此只還原 0x80 以上的跳脫，ASCII 特殊字元維持跳脫。
    const auto readable=[](std::string value){
        std::string out;const auto hex=[](char c)->int{return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;};
        for(std::size_t i=0;i<value.size();++i){if(value[i]=='\\' && i+2<value.size()){const auto high=hex(value[i+1]),low=hex(value[i+2]);if(high>=8 && low>=0){out+=static_cast<char>(high*16+low);i+=2;continue;}}out+=value[i];}
        return out;
    };
    if(extended_name("ldap_explode_dn")){
        if(!arg(0))return error(89,true);const auto dn=text(arg(0));char** parts=ldap_explode_dn(dn.c_str(),u32(1)?1:0);if(!parts)return error(0x22,true);
        std::vector<std::string> items;for(auto** it=parts;*it;++it)items.push_back(readable(*it));ldap_value_free(parts);return done(string_list(items));
    }
    if(extended_name("ldap_dn2ufn")){
        if(!arg(0))return error(89,true);const auto dn=text(arg(0));char* ufn=ldap_dn2ufn(dn.c_str());if(!ufn)return error(0x22,true);std::unique_ptr<char,decltype(&ldap_memfree)> holder(ufn,ldap_memfree);return done(string(readable(ufn).c_str()));
    }
    if(extended_name("ldap_escape_filter_element")){
        // (source, sourceLength, dest, destLength)：RFC 4515 的特殊字元與非可列印位元組改為 \xx；dest 為 NULL 時回傳所需位元組數。
        const auto source=read_bytes(arg(0),u32(1));std::string escaped;constexpr char hex[]="0123456789abcdef";
        for(const auto c:source){const auto byte=static_cast<unsigned char>(c);if(byte=='*' || byte=='(' || byte==')' || byte=='\\' || byte<0x20 || byte>=0x7f){escaped+='\\';escaped+=hex[byte>>4];escaped+=hex[byte&15];}else escaped+=c;}
        const auto bytes=encode_text(escaped);if(!arg(2))return done(bytes.size());if(u32(3)<bytes.size())return error(89);memory.write(GuestAddress{arg(2)},bytes);return done(0);
    }
    if(extended_name("ldap_control_free")){if(arg(0) && data.controls.erase(arg(0)))(void)free_heap(memory,arg(0),state);return done(0);}
    if(extended_name("ldap_controls_free") || extended_name("ldap_free_controls")){if(arg(0) && data.control_arrays.erase(arg(0)))(void)free_heap(memory,arg(0),state);return done(0);}
    if(name=="ber_bvfree"){if(arg(0) && data.bervals.erase(arg(0)))(void)free_heap(memory,arg(0),state);return done(0);}
    if(name=="ber_bvecfree"){if(arg(0)){for(unsigned i=0;i<65536;++i){const auto item=ptr(arg(0)+i*pointer);if(!item)break;if(data.bervals.erase(item))(void)free_heap(memory,item,state);}if(data.lists.erase(arg(0)))(void)free_heap(memory,arg(0),state);}return done(0);}
    if(name=="ber_bvdup"){if(!arg(0))return error(89,true);return done(make_berval(read_berval(arg(0))));}
    // BER 編解碼：客體 BerElement handle 對應原生 lber 元素；標記值以 32 位元回傳（LBER_DEFAULT＝0xffffffff）。
    if(name=="ber_alloc_t" || name=="ber_init"){
        auto element=std::make_shared<LdapBer>();
        if(name=="ber_alloc_t")element->native=ber_alloc_t(static_cast<int>(u32(0)));
        else {if(!arg(0))return error(89,true);auto bytes=read_berval(arg(0));berval value{bytes.size(),bytes.data()};element->native=ber_init(&value);}
        if(!element->native)return error(90,true);const auto handle=allocate(16);data.bers.emplace(handle,std::move(element));return done(handle);
    }
    if(name=="ber_free"){
        // 只處理 ber_alloc_t／ber_init 的元素；ldap_first_attribute 的游標由下方既有分支釋放。
        if(const auto found=data.bers.find(arg(0));found!=data.bers.end()){data.bers.erase(found);(void)free_heap(memory,arg(0),state);return done(0);}
    }
    if(name.starts_with("ber_") && name!="ber_free" && name!="ber_bvfree" && name!="ber_bvecfree" && name!="ber_bvdup"){
        const auto found=data.bers.find(arg(0));
        const bool tag_result=name=="ber_first_element" || name=="ber_next_element" || name=="ber_peek_tag" || name=="ber_skip_tag" || name=="ber_scanf";
        (void)tag_result;if(found==data.bers.end()){last_error_=89;return done(0xffffffffU);}
        auto& element=*found->second;auto* ber=element.native;
        const auto guest_tag=[](ber_tag_t tag){return tag==LBER_DEFAULT?0xffffffffU:static_cast<std::uint32_t>(tag);};
        if(name=="ber_flatten"){
            if(!arg(1))return done(0xffffffffU);memory.store(GuestAddress{arg(1)},bits,0);berval* flat=nullptr;if(ber_flatten(ber,&flat)==-1 || !flat)return done(0xffffffffU);
            const auto copy=make_berval({flat->bv_val?flat->bv_val:"",flat->bv_len});ber_bvfree(flat);memory.store(GuestAddress{arg(1)},bits,copy);return done(0);
        }
        if(name=="ber_peek_tag" || name=="ber_skip_tag"){
            ber_len_t length=0;const auto tag=name=="ber_peek_tag"?ber_peek_tag(ber,&length):ber_skip_tag(ber,&length);if(arg(1))memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(length));return done(guest_tag(tag));
        }
        if(name=="ber_first_element" || name=="ber_next_element"){
            // ber_first_element 的 opaque 輸出為 CHAR**，ber_next_element 收回該值（CHAR*）。
            ber_len_t length=0;ber_tag_t tag=LBER_DEFAULT;
            if(name=="ber_first_element"){char* last=nullptr;tag=ber_first_element(ber,&length,&last);if(element.marks.size()>=4096)element.marks.clear();element.marks.push_back(last);if(arg(2))memory.store(GuestAddress{arg(2)},bits,element.marks.size());}
            else {const auto mark=arg(2);if(!mark || mark>element.marks.size())return done(0xffffffffU);tag=ber_next_element(ber,&length,element.marks[static_cast<std::size_t>(mark-1)]);}
            if(arg(1))memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(length));return done(guest_tag(tag));
        }
        const bool encode=name=="ber_printf";if(!arg(1))return done(0xffffffffU);const auto format=read_ansi(memory,arg(1),1024);unsigned next=2;
        const auto take=[&]{return arg(next++);};
        if(encode){
            // ber_printf：逐一元素呼叫原生編碼；t 指定的標記併入下一個元素的同一次呼叫。X 為以位元計長的 bit string。
            std::optional<ber_tag_t> tag;
            const auto put=[&](const char* single,auto... values){int rc;if(tag){const char spec[]={'t',single[0],0};rc=ber_printf(ber,spec,*tag,values...);tag.reset();}else rc=ber_printf(ber,single,values...);if(rc==-1)throw LdapFailure{0x53};};
            for(const auto c:format){
                switch(c){
                case 't':tag=static_cast<ber_tag_t>(static_cast<std::uint32_t>(take()));break;
                case 'b':case 'e':case 'i':{const char single[]={c,0};put(single,static_cast<ber_int_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(take()))));break;}
                case 'n':put("n");break;
                case 'o':{const auto at=take();const auto size=static_cast<std::uint32_t>(take());auto bytes=read_bytes(at,size);put("o",bytes.data(),static_cast<ber_len_t>(bytes.size()));break;}
                case 's':{const auto at=take();if(!at)throw LdapFailure{89};auto value=read_ansi(memory,at,1U<<20);put("s",value.data());break;}
                case 'X':{const auto at=take();const auto bit_length=static_cast<std::uint32_t>(take());auto bytes=read_bytes(at,(std::uint64_t{bit_length}+7)/8);put("B",bytes.data(),static_cast<ber_len_t>(bit_length));break;}
                case 'v':{auto items=read_strings(take());std::vector<char*> list;for(auto& item:items)list.push_back(item.data());list.push_back(nullptr);put("v",list.data());break;}
                case 'V':{auto items=read_bervals(take());std::vector<berval> values;values.reserve(items.size());for(auto& item:items)values.push_back(berval{item.size(),item.data()});std::vector<berval*> list;for(auto& value:values)list.push_back(&value);list.push_back(nullptr);put("V",list.data());break;}
                case '{':case '}':case '[':case ']':{const char single[]={c,0};put(single);break;}
                case ' ':break;
                default:throw LdapFailure{89};
                }
            }
            return done(0);
        }
        // ber_scanf：a→char*（ldap_memfree）、O→berval*（ber_bvfree）、v→char**（ldap_value_free）、V→berval**（ber_bvecfree）、B→bit string 與位元數。
        // {v／{V 交給同一次原生解碼（序列包裝由 v／V 處理）。
        const auto fail_scan=[&]{last_error_=0x54;return done(0xffffffffU);};
        for(std::size_t i=0;i<format.size();++i){
            const auto c=format[i];
            if((c=='{' || c=='[') && i+1<format.size() && (format[i+1]=='v' || format[i+1]=='V'))continue;
            const bool wrapped=i>0 && (format[i-1]=='{' || format[i-1]=='[') && (c=='v' || c=='V');
            const char spec[]={wrapped?format[i-1]:c,wrapped?c:'\0',0};
            switch(c){
            case 'a':{char* value=nullptr;const auto out=take();if(ber_scanf(ber,"a",&value)==LBER_ERROR)return fail_scan();if(out)memory.store(GuestAddress{out},bits,string(value));ber_memfree(value);break;}
            case 'O':{berval* value=nullptr;const auto out=take();if(ber_scanf(ber,"O",&value)==LBER_ERROR || !value)return fail_scan();if(out)memory.store(GuestAddress{out},bits,make_berval({value->bv_val?value->bv_val:"",value->bv_len}));ber_bvfree(value);break;}
            case 'b':case 'e':case 'i':{ber_int_t value=0;const auto out=take();const char single[]={c,0};if(ber_scanf(ber,single,&value)==LBER_ERROR)return fail_scan();if(out)memory.store(GuestAddress{out},32,static_cast<std::uint32_t>(value));break;}
            case 'B':{char* value=nullptr;ber_len_t bit_length=0;const auto out=take(),length_out=take();if(ber_scanf(ber,"B",&value,&bit_length)==LBER_ERROR)return fail_scan();
                const auto bytes=static_cast<std::size_t>((bit_length+7)/8);const auto block=allocate(bytes+1);memory.fill(GuestAddress{block},0,bytes+1);if(bytes)memory.write(GuestAddress{block},std::span(reinterpret_cast<const std::uint8_t*>(value),bytes));data.strings.insert(block);ber_memfree(value);
                if(out)memory.store(GuestAddress{out},bits,block);if(length_out)memory.store(GuestAddress{length_out},32,static_cast<std::uint32_t>(bit_length));break;}
            case 'n':case 'x':case '{':case '}':case '[':case ']':{const char single[]={c,0};if(ber_scanf(ber,single)==LBER_ERROR)return fail_scan();break;}
            case 't':{ber_tag_t value=0;const auto out=take();if(ber_scanf(ber,"t",&value)==LBER_ERROR)return fail_scan();if(out)memory.store(GuestAddress{out},32,guest_tag(value));break;}
            case 'v':{char** values=nullptr;const auto out=take();if(ber_scanf(ber,spec,&values)==LBER_ERROR)return fail_scan();std::vector<std::string> items;if(values){for(auto** it=values;*it;++it)items.emplace_back(*it);ber_memvfree(reinterpret_cast<void**>(values));}
                if(out)memory.store(GuestAddress{out},bits,string_list(items));break;}
            case 'V':{berval** values=nullptr;const auto out=take();if(ber_scanf(ber,spec,&values)==LBER_ERROR)return fail_scan();std::vector<std::uint64_t> copies;
                if(values){for(auto** it=values;*it;++it)copies.push_back(make_berval({(*it)->bv_val?(*it)->bv_val:"",(*it)->bv_len}));ber_bvecfree(values);}
                const auto list=allocate((copies.size()+1)*pointer);for(std::size_t k=0;k<copies.size();++k)memory.store(GuestAddress{list+k*pointer},bits,copies[k]);memory.store(GuestAddress{list+copies.size()*pointer},bits,0);data.lists.insert(list);
                if(out)memory.store(GuestAddress{out},bits,list);break;}
            case ' ':break;
            default:return fail_scan();
            }
        }
        return done(0);
    }
    if(name=="ldap_startup"){
        // (LDAP_VERSION_INFO* version, HANDLE* instance)：回報 LDAP 協定 3.0，instance 由 ldap_cleanup 釋放。
        if(arg(0)){if(memory.load(GuestAddress{arg(0)},32)<12)return error(89);memory.store(GuestAddress{arg(0)+4},32,3);memory.store(GuestAddress{arg(0)+8},32,0);}
        if(arg(1)){const auto instance=allocate(16);data.instances.insert(instance);memory.store(GuestAddress{arg(1)},bits,instance);}return done(0);
    }
    if(name=="ldap_cleanup"){if(arg(0) && data.instances.erase(arg(0)))(void)free_heap(memory,arg(0),state);return done(0);}
    if(name=="ldap_set_dbg_flags"){data.debug_flags=u32(0);return done(data.debug_flags);}
    if(name=="ldap_perror")return done(0); // Microsoft 文件註明此函式不受支援；不輸出任何資料。
    if(extended_name("ldap_open") || extended_name("ldap_sslinit")){
        const bool ssl=name.starts_with("ldap_sslinit");return done(open_connection(text(arg(0)),u32(1),ssl && u32(2)!=0,!ssl));
    }
    if(name=="ldap_conn_from_msg"){
        if(!arg(1))return done(data.connections.contains(arg(0))?arg(0):0);const auto found=data.messages.find(arg(1));if(found==data.messages.end())return error(89,true);return done(connection_handle(found->second->connection));
    }
    if(extended_name("ldap_check_filter")){(void)connection_of(arg(0));if(!arg(1))return error(89);return error(valid_filter(text(arg(1)))?0:0x57);}
    if(name=="ldap_result"){
        // (ld, msgid, all, timeout, res)：回傳訊息型別，逾時為 0，失敗為 (ULONG)-1。
        const auto connection=connection_of(arg(0));if(!arg(4)){last_error_=89;return done(0xffffffffU);}memory.store(GuestAddress{arg(4)},bits,0);auto timeout=native_timeout(arg(3));
        LDAPMessage* result=nullptr;const auto type=ldap_result(connection->native,static_cast<int>(u32(1)),static_cast<int>(u32(2)),timeout?&*timeout:nullptr,&result);
        if(type<0){int code=0;ldap_get_option(connection->native,LDAP_OPT_RESULT_CODE,&code);last_error_=ldap_status(code?code:LDAP_SERVER_DOWN);return done(0xffffffffU);}
        if(type==0){last_error_=0x55;return done(0);}
        store_message(connection,result,arg(4));last_error_=0;return done(static_cast<std::uint32_t>(type));
    }
    if(name=="ldap_abandon" || name=="ldap_close_extended_op"){
        const auto connection=connection_of(arg(0));const auto status=ldap_abandon_ext(connection->native,static_cast<int>(u32(1)),nullptr,nullptr);
        if(name=="ldap_abandon"){last_error_=ldap_status(status);return done(status?0xffffffffU:0U);}return error(ldap_status(status));
    }
    if(extended_name("ldap_parse_result") || name=="ldap_result2error" || extended_name("ldap_parse_extended_result") || extended_name("ldap_parse_reference")){
        const auto connection=connection_of(arg(0));
        if(extended_name("ldap_parse_reference")){
            // (Connection, ResultMessage, Referrals)：ResultMessage 為 ldap_first_reference／next 取得的參照項目。
            const auto found=data.entries.find(arg(1));if(found==data.entries.end() || !arg(2))return error(89);memory.store(GuestAddress{arg(2)},bits,0);
            char** referrals=nullptr;const auto status=ldap_parse_reference(connection->native,found->second.native,&referrals,nullptr,0);
            if(referrals){std::vector<std::string> items;for(auto** it=referrals;*it;++it)items.emplace_back(*it);ldap_memvfree(reinterpret_cast<void**>(referrals));memory.store(GuestAddress{arg(2)},bits,string_list(items));}
            return error(ldap_status(status));
        }
        const auto message=message_of(arg(1));
        if(name=="ldap_result2error"){const auto status=ldap_result2error(connection->native,message->native,0);if(u32(2))free_message(arg(1));return error(ldap_status(status));}
        if(extended_name("ldap_parse_extended_result")){
            // (Connection, ResultMessage, ResultOID, ResultData, Freeit)
            if(arg(2))memory.store(GuestAddress{arg(2)},bits,0);if(arg(3))memory.store(GuestAddress{arg(3)},bits,0);char* oid=nullptr;berval* value=nullptr;
            const auto status=ldap_parse_extended_result(connection->native,message->native,arg(2)?&oid:nullptr,arg(3)?&value:nullptr,0);
            if(oid){memory.store(GuestAddress{arg(2)},bits,string(oid));ldap_memfree(oid);}if(value){memory.store(GuestAddress{arg(3)},bits,make_berval({value->bv_val,value->bv_len}));ber_bvfree(value);}
            if(static_cast<std::uint8_t>(arg(4)))free_message(arg(1));return error(ldap_status(status));
        }
        // ldap_parse_result (Connection, ResultMessage, ReturnCode, MatchedDNs, ErrorMessage, Referrals, ServerControls, Freeit)
        int code=0;char* matched=nullptr;char* message_text=nullptr;char** referrals=nullptr;LDAPControl** controls=nullptr;
        for(unsigned i=3;i<7;++i)if(arg(i))memory.store(GuestAddress{arg(i)},bits,0);
        const auto status=ldap_parse_result(connection->native,message->native,&code,arg(3)?&matched:nullptr,arg(4)?&message_text:nullptr,arg(5)?&referrals:nullptr,arg(6)?&controls:nullptr,0);
        if(arg(2))memory.store(GuestAddress{arg(2)},32,ldap_status(code));
        if(matched){memory.store(GuestAddress{arg(3)},bits,string(matched));ldap_memfree(matched);}
        if(message_text){memory.store(GuestAddress{arg(4)},bits,string(message_text));ldap_memfree(message_text);}
        if(referrals){std::vector<std::string> items;for(auto** it=referrals;*it;++it)items.emplace_back(*it);ldap_memvfree(reinterpret_cast<void**>(referrals));memory.store(GuestAddress{arg(5)},bits,string_list(items));}
        if(controls){const auto copy=write_controls(controls);ldap_controls_free(controls);memory.store(GuestAddress{arg(6)},bits,copy);}
        if(static_cast<std::uint8_t>(arg(7)))free_message(arg(1));return error(ldap_status(status));
    }
    if(name=="ldap_first_reference" || name=="ldap_next_reference" || name=="ldap_count_references"){
        const auto connection=connection_of(arg(0));
        if(name=="ldap_next_reference"){const auto found=data.entries.find(arg(1));if(found==data.entries.end())return error(89,true);const auto owner=found->second.message;return done(entry_handle(ldap_next_reference(connection->native,found->second.native),owner));}
        const auto message=message_of(arg(1));if(name=="ldap_count_references")return done(static_cast<std::uint32_t>(std::max(0,ldap_count_references(connection->native,message->native))));
        return done(entry_handle(ldap_first_reference(connection->native,message->native),message));
    }
    if(extended_name("ldap_get_values")){
        const auto connection=connection_of(arg(0));const auto found=data.entries.find(arg(1));if(found==data.entries.end() || !arg(2))return error(89,true);
        const auto attribute=text(arg(2));auto** values=ldap_get_values_len(connection->native,found->second.native,attribute.c_str());if(!values)return error(0x10,true);
        std::vector<std::string> items;for(auto** it=values;*it;++it)items.emplace_back((*it)->bv_val,(*it)->bv_len);ldap_value_free_len(values);return done(string_list(items));
    }
    // 同步與非同步的寫入、比較、更名、延伸操作與搜尋。
    const bool add=name.starts_with("ldap_add"),modify=name.starts_with("ldap_modify"),remove=name.starts_with("ldap_delete"),compare=name.starts_with("ldap_compare"),
        modrdn=name.starts_with("ldap_modrdn"),rename=name.starts_with("ldap_rename_ext"),extended=name.starts_with("ldap_extended_operation"),
        search=extended_name("ldap_search") || extended_name("ldap_search_ext") || extended_name("ldap_search_ext_s") || extended_name("ldap_search_st");
    if(add || modify || remove || compare || modrdn || rename || extended || search){
        const auto connection=connection_of(arg(0));auto* native=connection->native;
        const bool sync=name.ends_with("_s") || name.ends_with("_sA") || name.ends_with("_sW");
        const bool ext=name.find("_ext")!=name.npos;
        Controls server,client;int msgid=-1;int status=LDAP_SUCCESS;
        const auto finish_async=[&](unsigned number_at)->std::optional<std::uint32_t>{
            if(ext){if(!status && arg(number_at))memory.store(GuestAddress{arg(number_at)},32,static_cast<std::uint32_t>(msgid));return error(ldap_status(status));}
            last_error_=ldap_status(status);return done(status?0xffffffffU:static_cast<std::uint32_t>(msgid));
        };
        if(search){
            SearchArguments arguments;read_search(1,arguments);
            if(extended_name("ldap_search")){status=ldap_search_ext(native,arguments.base.c_str(),arguments.scope,arguments.filter.c_str(),arguments.list(),arguments.attributes_only,nullptr,nullptr,nullptr,0,&msgid);return finish_async(0);}
            if(extended_name("ldap_search_ext")){
                // (ld, base, scope, filter, attrs, attrsonly, ServerControls, ClientControls, TimeLimit, SizeLimit, MessageNumber)
                read_controls(arg(6),server);read_controls(arg(7),client);timeval limit{static_cast<std::int32_t>(u32(8)),0};
                status=ldap_search_ext(native,arguments.base.c_str(),arguments.scope,arguments.filter.c_str(),arguments.list(),arguments.attributes_only,server.get(),client.get(),u32(8)?&limit:nullptr,static_cast<int>(u32(9)),&msgid);
                if(!arg(10))return error(89);return finish_async(10);
            }
            const bool st=extended_name("ldap_search_st");const auto out_at=arg(st?7:10);if(!out_at)return error(89);memory.store(GuestAddress{out_at},bits,0);
            if(!st){read_controls(arg(6),server);read_controls(arg(7),client);}auto timeout=native_timeout(arg(st?6:8));
            LDAPMessage* result=nullptr;status=ldap_search_ext_s(native,arguments.base.c_str(),arguments.scope,arguments.filter.c_str(),arguments.list(),arguments.attributes_only,server.get(),client.get(),timeout?&*timeout:nullptr,st?0:static_cast<int>(u32(9)),&result);
            if(result)store_message(connection,result,out_at);return error(ldap_status(status));
        }
        const auto dn=text(arg(1));
        if(add || modify){
            // (ld, dn, mods[, ServerControls, ClientControls[, MessageNumber]])
            Mods mods;read_mods(arg(2),mods);if(ext){read_controls(arg(3),server);read_controls(arg(4),client);}
            if(sync)status=add?ldap_add_ext_s(native,dn.c_str(),mods.array.data(),server.get(),client.get()):ldap_modify_ext_s(native,dn.c_str(),mods.array.data(),server.get(),client.get());
            else status=add?ldap_add_ext(native,dn.c_str(),mods.array.data(),server.get(),client.get(),&msgid):ldap_modify_ext(native,dn.c_str(),mods.array.data(),server.get(),client.get(),&msgid);
            if(sync)return error(ldap_status(status));if(ext && !arg(5))return error(89);return finish_async(5);
        }
        if(remove){
            if(ext){read_controls(arg(2),server);read_controls(arg(3),client);}
            if(sync){status=ldap_delete_ext_s(native,dn.c_str(),server.get(),client.get());return error(ldap_status(status));}
            status=ldap_delete_ext(native,dn.c_str(),server.get(),client.get(),&msgid);if(ext && !arg(4))return error(89);return finish_async(4);
        }
        if(compare){
            // (ld, dn, attr, value) 或 _ext：(ld, dn, Attr, Value, Data, ServerControls, ClientControls[, MessageNumber])；Data 不為 NULL 時以二進位比較。
            const auto attribute=text(arg(2));std::string value=ext && arg(4)?read_berval(arg(4)):text(arg(3));berval assertion{value.size(),value.data()};
            if(ext){read_controls(arg(5),server);read_controls(arg(6),client);}
            if(sync){status=ldap_compare_ext_s(native,dn.c_str(),attribute.c_str(),&assertion,server.get(),client.get());return error(ldap_status(status));}
            status=ldap_compare_ext(native,dn.c_str(),attribute.c_str(),&assertion,server.get(),client.get(),&msgid);if(ext && !arg(7))return error(89);return finish_async(7);
        }
        if(modrdn || rename){
            // modrdn：(ld, dn, newrdn) 刪除舊 RDN；modrdn2：(ld, dn, newrdn, DeleteOldRdn)；rename_ext：(ld, dn, NewRDN, NewParent, DeleteOldRdn, ServerControls, ClientControls[, MessageNumber])。
            const auto newrdn=text(arg(2));const bool two=name.starts_with("ldap_modrdn2");std::string parent;bool has_parent=false;int delete_old=1;
            if(two)delete_old=u32(3)?1:0;
            if(rename){has_parent=arg(3)!=0;parent=text(arg(3));delete_old=u32(4)?1:0;read_controls(arg(5),server);read_controls(arg(6),client);}
            if(sync){status=ldap_rename_s(native,dn.c_str(),newrdn.c_str(),has_parent?parent.c_str():nullptr,delete_old,server.get(),client.get());return error(ldap_status(status));}
            status=ldap_rename(native,dn.c_str(),newrdn.c_str(),has_parent?parent.c_str():nullptr,delete_old,server.get(),client.get(),&msgid);if(rename && !arg(7))return error(89);return finish_async(7);
        }
        // 延伸操作：(ld, Oid, Data, ServerControls, ClientControls, MessageNumber) 或 _s：(…, ReturnedOid, ReturnedData)。
        if(!arg(1))return error(89);const auto oid=text(arg(1));std::string payload=arg(2)?read_berval(arg(2)):std::string{};berval request{payload.size(),payload.data()};
        read_controls(arg(3),server);read_controls(arg(4),client);
        if(name.starts_with("ldap_extended_operation_s")){
            if(arg(5))memory.store(GuestAddress{arg(5)},bits,0);if(arg(6))memory.store(GuestAddress{arg(6)},bits,0);char* returned_oid=nullptr;berval* returned=nullptr;
            status=ldap_extended_operation_s(native,oid.c_str(),arg(2)?&request:nullptr,server.get(),client.get(),&returned_oid,&returned);
            if(returned_oid){if(arg(5))memory.store(GuestAddress{arg(5)},bits,string(returned_oid));ldap_memfree(returned_oid);}
            if(returned){if(arg(6))memory.store(GuestAddress{arg(6)},bits,make_berval({returned->bv_val,returned->bv_len}));ber_bvfree(returned);}
            return error(ldap_status(status));
        }
        status=ldap_extended_operation(native,oid.c_str(),arg(2)?&request:nullptr,server.get(),client.get(),&msgid);if(!arg(5))return error(89);
        if(!status)memory.store(GuestAddress{arg(5)},32,static_cast<std::uint32_t>(msgid));return error(ldap_status(status));
    }
    if(extended_name("ldap_create_page_control") || extended_name("ldap_create_sort_control") || extended_name("ldap_create_vlv_control") || extended_name("ldap_encode_sort_control")){
        const auto connection=connection_of(arg(0));LDAPControl* control=nullptr;int status=LDAP_SUCCESS;
        if(extended_name("ldap_create_page_control")){
            // (ld, PageSize, Cookie, IsCritical, Control)
            if(!arg(4))return error(89);memory.store(GuestAddress{arg(4)},bits,0);std::string cookie=arg(2)?read_berval(arg(2)):std::string{};berval value{cookie.size(),cookie.data()};
            status=ldap_create_page_control(connection->native,static_cast<ber_int_t>(u32(1)),&value,static_cast<std::uint8_t>(arg(3))?1:0,&control);
            if(control){memory.store(GuestAddress{arg(4)},bits,write_control(*control));ldap_control_free(control);}return error(ldap_status(status));
        }
        if(extended_name("ldap_create_vlv_control")){
            // (ld, VlvInfo, IsCritical, Control)：LDAPVLVInfo {版本, 前, 後, 位移, 數量, attrvalue*, context*, extradata}。
            if(!arg(1) || !arg(3))return error(89);memory.store(GuestAddress{arg(3)},bits,0);const auto info=arg(1);
            LDAPVLVInfo vlv{};vlv.ldvlv_version=static_cast<ber_int_t>(memory.load(GuestAddress{info},32));vlv.ldvlv_before_count=static_cast<ber_int_t>(memory.load(GuestAddress{info+4},32));
            vlv.ldvlv_after_count=static_cast<ber_int_t>(memory.load(GuestAddress{info+8},32));vlv.ldvlv_offset=static_cast<ber_int_t>(memory.load(GuestAddress{info+12},32));vlv.ldvlv_count=static_cast<ber_int_t>(memory.load(GuestAddress{info+16},32));
            const auto values=info+(x86_?20:24);std::string attribute,context;berval attribute_value{},context_value{};
            if(const auto p=ptr(values)){attribute=read_berval(p);attribute_value={attribute.size(),attribute.data()};vlv.ldvlv_attrvalue=&attribute_value;}
            if(const auto p=ptr(values+pointer)){context=read_berval(p);context_value={context.size(),context.data()};vlv.ldvlv_context=&context_value;}
            status=ldap_create_vlv_control(connection->native,&vlv,&control);
            if(control){control->ldctl_iscritical=static_cast<char>(static_cast<std::uint8_t>(arg(2))?1:0);memory.store(GuestAddress{arg(3)},bits,write_control(*control));ldap_control_free(control);}return error(ldap_status(status));
        }
        // 排序：LDAPSortKey {sk_attrtype, sk_matchruleoid, BOOLEAN sk_reverseorder}。
        if(!arg(1) || !arg(3))return error(89);std::vector<std::uint64_t> pointers;for(unsigned i=0;;++i){if(i>=256)return error(89);const auto p=ptr(arg(1)+i*pointer);if(!p)break;pointers.push_back(p);}
        std::vector<std::string> types,rules;types.reserve(pointers.size());rules.reserve(pointers.size());std::vector<LDAPSortKey> keys;keys.reserve(pointers.size());std::vector<LDAPSortKey*> list;
        for(const auto p:pointers){types.push_back(text(ptr(p)));const auto rule=ptr(p+pointer);rules.push_back(text(rule));
            LDAPSortKey key{};key.attributeType=types.back().data();key.orderingRule=rule?rules.back().data():nullptr;key.reverseOrder=memory.load(GuestAddress{p+2*pointer},8)?1:0;keys.push_back(key);}
        for(auto& key:keys)list.push_back(&key);list.push_back(nullptr);
        const bool encode=extended_name("ldap_encode_sort_control");const auto critical=static_cast<std::uint8_t>(arg(encode?3:2))?1:0;
        status=ldap_create_sort_control(connection->native,list.data(),critical,&control);if(!control)return error(ldap_status(status?status:LDAP_ENCODING_ERROR));
        std::unique_ptr<LDAPControl,decltype(&ldap_control_free)> holder(control,ldap_control_free);
        if(!encode){memory.store(GuestAddress{arg(3)},bits,0);memory.store(GuestAddress{arg(3)},bits,write_control(*control));return error(ldap_status(status));}
        // ldap_encode_sort_control 填入呼叫端的 LDAPControl；OID 與值以 ldap_memfree 釋放。
        const auto value=allocate(control->ldctl_value.bv_len+1);memory.fill(GuestAddress{value},0,control->ldctl_value.bv_len+1);
        if(control->ldctl_value.bv_len)memory.write(GuestAddress{value},std::span(reinterpret_cast<const std::uint8_t*>(control->ldctl_value.bv_val),control->ldctl_value.bv_len));data.strings.insert(value);
        memory.store(GuestAddress{arg(2)},bits,string(control->ldctl_oid));memory.store(GuestAddress{arg(2)+pointer},32,control->ldctl_value.bv_len);memory.store(GuestAddress{arg(2)+2*pointer},bits,value);
        memory.store(GuestAddress{arg(2)+3*pointer},8,static_cast<std::uint64_t>(critical));return error(0);
    }
    if(extended_name("ldap_parse_page_control") || extended_name("ldap_parse_sort_control") || extended_name("ldap_parse_vlv_control")){
        // 從伺服器控制項陣列找出對應的回應控制項並解碼。
        const auto connection=connection_of(arg(0));Controls controls;read_controls(arg(1),controls);
        if(extended_name("ldap_parse_page_control")){
            // (ld, ServerControls, TotalCount, Cookie)
            if(arg(3))memory.store(GuestAddress{arg(3)},bits,0);auto* control=first_control(controls.get(),"1.2.840.113556.1.4.319");if(!control)return error(0x5d);
            ber_int_t count=0;berval cookie{};const auto status=ldap_parse_pageresponse_control(connection->native,control,&count,&cookie);
            if(!status){if(arg(2))memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(count));if(arg(3))memory.store(GuestAddress{arg(3)},bits,make_berval({cookie.bv_val?cookie.bv_val:"",cookie.bv_len}));}
            if(cookie.bv_val)ber_memfree(cookie.bv_val);return error(ldap_status(status));
        }
        if(extended_name("ldap_parse_sort_control")){
            // (ld, Control, Result, Attribute)
            if(arg(3))memory.store(GuestAddress{arg(3)},bits,0);auto* control=first_control(controls.get(),"1.2.840.113556.1.4.474");if(!control)return error(0x5d);
            ber_int_t result=0;char* attribute=nullptr;const auto status=ldap_parse_sortresponse_control(connection->native,control,&result,&attribute);
            if(!status && arg(2))memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(result));if(attribute){if(arg(3))memory.store(GuestAddress{arg(3)},bits,string(attribute));ldap_memfree(attribute);}
            return error(ldap_status(status));
        }
        // ldap_parse_vlv_control (ld, Control, TargetPos, ListCount, Context, ErrCode)
        if(arg(4))memory.store(GuestAddress{arg(4)},bits,0);auto* control=first_control(controls.get(),"2.16.840.1.113730.3.4.10");if(!control)return error(0x5d);
        ber_int_t target=0,count=0,code=0;berval* context=nullptr;const auto status=ldap_parse_vlvresponse_control(connection->native,control,&target,&count,arg(4)?&context:nullptr,&code);
        if(!status){if(arg(2))memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(target));if(arg(3))memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(count));if(arg(5))memory.store(GuestAddress{arg(5)},32,static_cast<std::uint32_t>(code));}
        if(context){if(arg(4))memory.store(GuestAddress{arg(4)},bits,make_berval({context->bv_val,context->bv_len}));ber_bvfree(context);}return error(ldap_status(status));
    }
    if(extended_name("ldap_start_tls_s")){
        // (ld, ServerReturnValue, result, ServerControls, ClientControls)
        const auto connection=connection_of(arg(0));Controls server,client;read_controls(arg(3),server);read_controls(arg(4),client);if(arg(2))memory.store(GuestAddress{arg(2)},bits,0);
        const auto status=ldap_start_tls_s(connection->native,server.get(),client.get());if(arg(1))memory.store(GuestAddress{arg(1)},32,ldap_status(status));return error(ldap_status(status));
    }
    if(extended_name("ldap_search_init_page")){
        // (ld, base, scope, filter, attrs, attrsonly, ServerControls, ClientControls, PageTimeLimit, TotalSizeLimit, SortKeys)：建立分頁搜尋狀態，排序鍵轉為伺服器控制項。
        auto search_state=std::make_shared<LdapPagedSearch>();search_state->connection=connection_of(arg(0));SearchArguments arguments;read_search(1,arguments);
        search_state->base=arguments.base;search_state->filter=arguments.filter;search_state->scope=arguments.scope;search_state->attributes_only=arguments.attributes_only;search_state->has_attributes=arguments.has_attributes;search_state->attributes=arguments.attributes;
        Controls server,client;read_controls(arg(6),server);read_controls(arg(7),client);
        for(std::size_t i=0;i<server.items.size();++i)search_state->server_controls.push_back({server.oids[i],{server.values[i],server.items[i].ldctl_iscritical!=0}});
        for(std::size_t i=0;i<client.items.size();++i)search_state->client_controls.push_back({client.oids[i],{client.values[i],client.items[i].ldctl_iscritical!=0}});
        search_state->page_time_limit=u32(8);search_state->total_size_limit=u32(9);
        if(arg(10)){
            std::vector<std::string> types,rules;std::vector<LDAPSortKey> keys;std::vector<std::uint64_t> pointers;for(unsigned i=0;;++i){if(i>=256)return error(89,true);const auto p=ptr(arg(10)+i*pointer);if(!p)break;pointers.push_back(p);}
            types.reserve(pointers.size());rules.reserve(pointers.size());keys.reserve(pointers.size());
            for(const auto p:pointers){types.push_back(text(ptr(p)));const auto rule=ptr(p+pointer);rules.push_back(text(rule));LDAPSortKey key{};key.attributeType=types.back().data();key.orderingRule=rule?rules.back().data():nullptr;key.reverseOrder=memory.load(GuestAddress{p+2*pointer},8)?1:0;keys.push_back(key);}
            std::vector<LDAPSortKey*> list;for(auto& key:keys)list.push_back(&key);list.push_back(nullptr);LDAPControl* control=nullptr;
            const auto status=ldap_create_sort_control(search_state->connection->native,list.data(),0,&control);if(!control)return error(ldap_status(status?status:LDAP_ENCODING_ERROR),true);
            search_state->server_controls.push_back({control->ldctl_oid,{std::string(control->ldctl_value.bv_val,control->ldctl_value.bv_len),false}});ldap_control_free(control);
        }
        const auto handle=allocate(16);data.searches.emplace(handle,std::move(search_state));return done(handle);
    }
    if(name=="ldap_get_next_page" || name=="ldap_get_next_page_s" || name=="ldap_get_paged_count" || name=="ldap_search_abandon_page"){
        const auto connection=connection_of(arg(0));const auto found=data.searches.find(arg(1));if(found==data.searches.end() || found->second->connection!=connection)return error(89);auto& paged=*found->second;
        if(name=="ldap_search_abandon_page"){if(paged.pending>=0)(void)ldap_abandon_ext(connection->native,paged.pending,nullptr,nullptr);data.searches.erase(found);(void)free_heap(memory,arg(1),state);return error(0);}
        // 由結果訊息的分頁回應控制項更新 cookie 與總數；cookie 為空表示最後一頁。
        const auto update=[&](LDAPMessage* result){
            LDAPControl** controls=nullptr;int code=0;const auto status=ldap_parse_result(connection->native,result,&code,nullptr,nullptr,nullptr,&controls,0);if(status)return static_cast<int>(status);
            auto* control=first_control(controls,"1.2.840.113556.1.4.319");
            if(control){ber_int_t count=0;berval cookie{};if(!ldap_parse_pageresponse_control(connection->native,control,&count,&cookie)){paged.total=static_cast<std::uint32_t>(count);paged.cookie.assign(cookie.bv_val?cookie.bv_val:"",cookie.bv_len);}if(cookie.bv_val)ber_memfree(cookie.bv_val);}
            else paged.cookie.clear();
            if(controls)ldap_controls_free(controls);if(paged.cookie.empty())paged.finished=true;paged.pending=-1;return code;
        };
        if(name=="ldap_get_paged_count"){
            // (ld, SearchBlock, TotalCount, Results)
            const auto message=message_of(arg(3));const auto code=update(message->native);if(arg(2))memory.store(GuestAddress{arg(2)},32,paged.total);return error(ldap_status(code));
        }
        const bool sync=name=="ldap_get_next_page_s";const auto page_size=u32(sync?3:2);
        if(sync){if(!arg(5))return error(89);memory.store(GuestAddress{arg(5)},bits,0);}
        if(paged.finished)return error(0x5e);
        Controls server;server.oids.reserve(paged.server_controls.size()+1);server.values.reserve(paged.server_controls.size()+1);server.items.reserve(paged.server_controls.size()+1);
        for(const auto& [oid,value]:paged.server_controls){server.oids.push_back(oid);server.values.push_back(value.first);LDAPControl control{};control.ldctl_oid=server.oids.back().data();control.ldctl_value={server.values.back().size(),server.values.back().data()};control.ldctl_iscritical=value.second?1:0;server.items.push_back(control);}
        berval cookie{paged.cookie.size(),paged.cookie.data()};LDAPControl* page=nullptr;
        const auto created=ldap_create_page_control(connection->native,static_cast<ber_int_t>(page_size),&cookie,0,&page);if(!page)return error(ldap_status(created?created:LDAP_ENCODING_ERROR));
        std::unique_ptr<LDAPControl,decltype(&ldap_control_free)> holder(page,ldap_control_free);server.items.push_back(*page);for(auto& item:server.items)server.array.push_back(&item);server.array.push_back(nullptr);
        Controls client;client.oids.reserve(paged.client_controls.size());client.values.reserve(paged.client_controls.size());client.items.reserve(paged.client_controls.size());
        for(const auto& [oid,value]:paged.client_controls){client.oids.push_back(oid);client.values.push_back(value.first);LDAPControl control{};control.ldctl_oid=client.oids.back().data();control.ldctl_value={client.values.back().size(),client.values.back().data()};control.ldctl_iscritical=value.second?1:0;client.items.push_back(control);}
        for(auto& item:client.items)client.array.push_back(&item);if(!client.array.empty())client.array.push_back(nullptr);
        std::vector<char*> attributes;for(auto& item:paged.attributes)attributes.push_back(item.data());attributes.push_back(nullptr);
        timeval limit{static_cast<std::int32_t>(paged.page_time_limit),0};
        if(!sync){
            int msgid=-1;const auto status=ldap_search_ext(connection->native,paged.base.c_str(),paged.scope,paged.filter.c_str(),paged.has_attributes?attributes.data():nullptr,paged.attributes_only,server.get(),client.get(),paged.page_time_limit?&limit:nullptr,static_cast<int>(paged.total_size_limit),&msgid);
            if(!status){paged.pending=msgid;if(arg(3))memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(msgid));}return error(ldap_status(status));
        }
        // ldap_get_next_page_s (ld, SearchBlock, timeout, PageSize, TotalCount, Results)
        auto timeout=native_timeout(arg(2));LDAPMessage* result=nullptr;
        const auto status=ldap_search_ext_s(connection->native,paged.base.c_str(),paged.scope,paged.filter.c_str(),paged.has_attributes?attributes.data():nullptr,paged.attributes_only,server.get(),client.get(),timeout?&*timeout:(paged.page_time_limit?&limit:nullptr),static_cast<int>(paged.total_size_limit),&result);
        if(result){const auto code=update(result);store_message(connection,result,arg(5));if(arg(4))memory.store(GuestAddress{arg(4)},32,paged.total);if(!status && code)return error(ldap_status(code));}
        return error(ldap_status(status));
    }
}catch(const LdapFailure& failure){
    last_error_=failure.code;return done(pointer_function?0:message_function?0xffffffffU:failure.code);
}catch(const MemoryFault&){last_error_=89;return done(pointer_function?0:message_function?0xffffffffU:89U);}
