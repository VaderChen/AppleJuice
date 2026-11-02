#include "wlan_profile.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/unicode.hpp"
#include "windows_sddl.hpp"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <algorithm>
#include <chrono>

namespace x64arm::wlan {
namespace {
constexpr std::uint32_t profile_type=0x574c414eU,bad_profile=1206,invalid_schema=0x80001;
using Document=std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)>;
Document document(std::string_view input){
    require(input.size()<=4*1024*1024,8);Document doc(xmlReadMemory(input.data(),static_cast<int>(input.size()),"wlan-profile.xml","UTF-8",XML_PARSE_NONET|XML_PARSE_NOBLANKS|XML_PARSE_NOERROR|XML_PARSE_NOWARNING),xmlFreeDoc);
    require(doc && !doc->intSubset && !doc->extSubset,bad_profile,invalid_schema);
    const auto root=xmlDocGetRootElement(doc.get());require(root && xmlStrEqual(root->name,BAD_CAST "WLANProfile") && root->ns && xmlStrEqual(root->ns->href,BAD_CAST "http://www.microsoft.com/networking/WLAN/profile/v1"),bad_profile,invalid_schema);return doc;
}
xmlNode* child(xmlNode* parent,const char* name,bool needed=true){
    xmlNode* result=nullptr;if(parent)for(auto* n=parent->children;n;n=n->next)if(n->type==XML_ELEMENT_NODE && xmlStrEqual(n->name,BAD_CAST name)){
        require(!result,bad_profile,invalid_schema);result=n;
    }
    require(result || !needed,bad_profile,invalid_schema);return result;
}
std::string content(xmlNode* node){if(!node)return {};xmlChar* p=xmlNodeGetContent(node);std::string value=p?reinterpret_cast<const char*>(p):"";xmlFree(p);return value;}
std::string field(xmlNode* parent,const char* name,bool needed=true){return content(child(parent,name,needed));}
bool boolean(std::string_view value){require(value=="true" || value=="false" || value=="1" || value=="0",bad_profile,invalid_schema);return value=="true" || value=="1";}
std::string hex(std::span<const std::uint8_t> bytes){std::string out;out.reserve(bytes.size()*2);for(auto c:bytes){out+="0123456789ABCDEF"[c>>4];out+="0123456789ABCDEF"[c&15];}return out;}
Bytes unhex(std::string_view s){
    require(!(s.size()%2),bad_profile,invalid_schema);Bytes b;
    const auto digit=[](char c)->int{if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;if(c>='A' && c<='F')return c-'A'+10;return -1;};
    for(std::size_t i=0;i<s.size();i+=2){const auto a=digit(s[i]),c=digit(s[i+1]);require(a>=0 && c>=0,bad_profile,invalid_schema);b.push_back(static_cast<std::uint8_t>((a<<4)|c));}return b;
}
std::string serialize(xmlDoc* doc){xmlChar* buffer=nullptr;int size=0;xmlDocDumpFormatMemoryEnc(doc,&buffer,&size,"UTF-8",0);require(buffer && size>=0,8);std::string result(reinterpret_cast<const char*>(buffer),static_cast<std::size_t>(size));xmlFree(buffer);return result;}
xmlNode* shared_key(xmlDoc* doc){return child(child(child(xmlDocGetRootElement(doc),"MSM"),"security"),"sharedKey",false);}
void key_valid(const Profile& p,const std::string& key){
    if(!p.connection.cipher)return;
    if(p.key_type=="passPhrase")require(key.size()>=8 && key.size()<=63 && std::all_of(key.begin(),key.end(),[](unsigned char c){return c>=32 && c<=126;}),bad_profile,0x40004);
    else if(p.key_type=="networkKey"){
        if(p.connection.cipher==0x101)require(key.size()==10 || key.size()==26,bad_profile,0x40003);
        else require(key.size()==64,bad_profile,0x40004);(void)unhex(key);
    }else throw Error{bad_profile,invalid_schema};
}
void put(Bytes& b,std::uint64_t n,unsigned size){for(unsigned i=0;i<size;++i)b.push_back(static_cast<std::uint8_t>(n>>(i*8)));}
Bytes encode(const Profile& p){Bytes bytes{'W','L','P','R',1,0,0,0};put(bytes,p.flags,4);put(bytes,p.order,8);put(bytes,p.descriptor.size(),4);put(bytes,p.xml.size(),4);bytes.insert(bytes.end(),p.descriptor.begin(),p.descriptor.end());bytes.insert(bytes.end(),p.xml.begin(),p.xml.end());return bytes;}
Profile decode(const Bytes& b){
    require(b.size()>=28 && std::equal(b.begin(),b.begin()+8,std::array<std::uint8_t,8>{'W','L','P','R',1,0,0,0}.begin()),13);
    const auto get=[&](unsigned at,unsigned size){std::uint64_t n=0;for(unsigned i=0;i<size;++i)n|=std::uint64_t{b[at+i]}<<(i*8);return n;};
    const auto sd=get(20,4),xml=get(24,4);require(sd<=65536 && xml<=4*1024*1024 && 28+sd+xml==b.size(),13);
    auto result=parse_profile(std::string_view(reinterpret_cast<const char*>(b.data()+28+sd),static_cast<std::size_t>(xml)));
    result.flags=static_cast<std::uint32_t>(get(8,4));require(result.flags<=2,13);result.order=get(12,8);result.descriptor.assign(b.begin()+28,b.begin()+static_cast<std::ptrdiff_t>(28+sd));
    (void)security::parse_relative_descriptor(result.descriptor);return result;
}
void store_check(std::uint32_t code){if(code==0x80090016U)code=1168;if(code==0x8009000fU)code=183;if(code==0x80090005U)code=13;check(code);}
}
Profile parse_profile(std::string_view input){
    auto doc=document(input);const auto root=xmlDocGetRootElement(doc.get());Profile p;p.xml=input;p.name=utf8_to_utf16(field(root,"name"));
    require(!p.name.empty() && p.name.size()<256,bad_profile,0x80003);
    const auto ssid_config=child(root,"SSIDConfig"),ssid=child(ssid_config,"SSID");const auto hex_name=field(ssid,"hex",false),name=field(ssid,"name",false);
    p.connection.ssid=hex_name.empty()?Bytes(name.begin(),name.end()):unhex(hex_name);
    require(!p.connection.ssid.empty() && p.connection.ssid.size()<=32,bad_profile,0x80013);
    if(!hex_name.empty() && !name.empty())require(p.connection.ssid==Bytes(name.begin(),name.end()),bad_profile,0x80013);
    const auto type=field(root,"connectionType"),mode=field(root,"connectionMode");require(type=="ESS" || type=="IBSS",bad_profile,0x8000d);require(mode=="auto" || mode=="manual",bad_profile,invalid_schema);
    p.connection.bss_type=type=="ESS"?1:2;p.automatic=mode=="auto";if(auto* n=child(ssid_config,"nonBroadcast",false))p.hidden=boolean(content(n));
    require(type!="IBSS" || (!p.automatic && !p.hidden),bad_profile,0x8000e);p.connection.flags=p.hidden?1U:0U;
    auto* sec=child(child(root,"MSM"),"security");auto* ae=child(sec,"authEncryption");const auto auth=field(ae,"authentication"),cipher=field(ae,"encryption");
    if(auto* n=child(ae,"useOneX",false);n && boolean(content(n)))throw Error{1169,0x40013};
    if(auto* n=child(ae,"FIPSMode",false);n && boolean(content(n)))throw Error{1169,0x40013};
    if(auth=="open")p.connection.auth=1;else if(auth=="shared")p.connection.auth=2;else if(auth=="WPAPSK")p.connection.auth=4;else if(auth=="WPA2PSK")p.connection.auth=7;else if(auth=="WPA3SAE")p.connection.auth=9;else throw Error{1169,0x40023};
    if(cipher=="none")p.connection.cipher=0;else if(cipher=="WEP")p.connection.cipher=0x101;else if(cipher=="TKIP")p.connection.cipher=2;else if(cipher=="AES")p.connection.cipher=4;else throw Error{1169,0x40024};
    require((p.connection.auth==1 && (p.connection.cipher==0 || p.connection.cipher==0x101)) || (p.connection.auth==2 && p.connection.cipher==0x101) || (p.connection.auth>=4 && (p.connection.cipher==2 || p.connection.cipher==4)),bad_profile,0x40009);
    if(auto* key=child(sec,"sharedKey",false)){p.key_type=field(key,"keyType");p.key=field(key,"keyMaterial");p.key_protected=boolean(field(key,"protected"));require(p.connection.cipher!=0,bad_profile,invalid_schema);if(!p.key_protected)key_valid(p,p.key);else (void)unhex(p.key);}
    else require(!p.connection.cipher,bad_profile,0x40004);
    return p;
}
std::string profile_key(const Profile& p,const PrivateKeyStoreContext& context){
    if(!p.key_protected)return p.key;
    auto plain=unprotect_sandbox_data(context,unhex(p.key));require(plain.bytes.size()>=2 && !(plain.bytes.size()%2) && !plain.bytes.back() && !plain.bytes[plain.bytes.size()-2],13);
    std::u16string text;for(std::size_t i=0;i+2<plain.bytes.size();i+=2)text.push_back(static_cast<char16_t>(plain.bytes[i]|std::uint16_t{plain.bytes[i+1]}<<8));
    const auto result=utf16_to_utf8(text);key_valid(p,result);return result;
}
std::string profile_xml(const Profile& p,const PrivateKeyStoreContext& context,bool plaintext,bool renew){
    if(p.key.empty())return p.xml;auto doc=document(p.xml);auto* key=shared_key(doc.get());require(key,13);
    auto material=p.key;
    if(plaintext)material=profile_key(p,context);
    else if(!p.key_protected || renew)material=hex(protect_sandbox_data(context,utf16_bytes(utf8_to_utf16(profile_key(p,context))),u"WLAN profile key",!(p.flags&2U)));
    xmlNodeSetContent(child(key,"protected"),BAD_CAST (plaintext?"false":"true"));xmlNodeSetContent(child(key,"keyMaterial"),BAD_CAST material.c_str());return serialize(doc.get());
}
std::string rename_profile_xml(const Profile& p,std::u16string_view name,bool automatic){auto doc=document(p.xml);const auto root=xmlDocGetRootElement(doc.get());const auto utf8=utf16_to_utf8(name);xmlNodeSetContent(child(root,"name"),BAD_CAST utf8.c_str());xmlNodeSetContent(child(root,"connectionMode"),BAD_CAST (automatic?"auto":"manual"));return serialize(doc.get());}
Profiles::Profiles(const ReadOnlyFiles& files,const VirtualToken& caller,std::string sid,std::string interface):files_(files),caller_(caller),sid_(std::move(sid)),interface_(std::move(interface)){
    VirtualToken service;service.user={sddl_sid("SY"),7};service.primary_group=service.user.sid;service.owner=service.user.sid;service.integrity=0x4000;
    files_.set_security_context(service,service.user.sid,service.user.sid);
}
std::string Profiles::key(std::u16string_view name)const{return interface_+":"+utf16_to_utf8(name);}
bool Profiles::administrator()const{return caller_.user.sid==sddl_sid("SY") || security::token_sid_matches(caller_,sddl_sid("BA"),false);}
std::uint32_t Profiles::access(const Profile& p,std::uint32_t desired)const{
    const auto result=security::access_check(caller_,security::parse_relative_descriptor(p.descriptor),desired,{read_access,write_access,execute_access,write_access});check(result.error);return result.granted;
}
std::optional<Profile> Profiles::load(std::u16string_view name,bool machine){
    Bytes bytes;const auto code=private_key_store_read(context(),profile_type,key(name),machine,bytes);if(code==0x80090016U)return {};
    store_check(code);auto p=decode(bytes);require(p.name==name && bool(p.flags&2U)!=machine,13);return p;
}
Profile Profiles::get(std::u16string_view name,std::uint32_t desired){
    require(!name.empty() && name.size()<256);auto value=load(name,false);if(!value)value=load(name,true);require(value.has_value(),1168);(void)access(*value,desired);return *value;
}
std::vector<Profile> Profiles::list(){
    std::vector<Profile> result;
    for(const bool machine:{false,true}){std::vector<std::string> names;store_check(private_key_store_list(context(),profile_type,machine,names));
        for(const auto& name:names)if(name.starts_with(interface_+":")){auto p=load(utf8_to_utf16(name.substr(interface_.size()+1)),machine);if(!p)continue;
            try{(void)access(*p,read_access);}catch(const Error& e){if(e.code==5)continue;throw;}
            if(std::none_of(result.begin(),result.end(),[&](const auto& other){return p->name==other.name;}))result.push_back(std::move(*p));}}
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){if(bool(a.flags&1)!=bool(b.flags&1))return bool(a.flags&1);return a.order>b.order;});return result;
}
void Profiles::save(Profile p,bool overwrite,std::optional<std::string> security_text){
    require(p.flags<=2);if(p.flags==1)require(overwrite && administrator(),5);
    auto prior=load(p.name,false);if(!prior)prior=load(p.name,true);
    if(prior){require(overwrite && p.flags==prior->flags,183);require(!(prior->flags&1U) || p.flags==1,5);(void)access(*prior,write_access);p.order=prior->order;p.descriptor=prior->descriptor;}
    else {p.order=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());p.descriptor=sddl_descriptor("O:"+sid_+"D:P(A;;0x70023;;;"+((p.flags&2U)?sid_:std::string("WD"))+")(A;;0x70023;;;SY)(A;;0x70023;;;BA)");}
    if(!(p.flags&2U) && security_text)p.descriptor=sddl_descriptor(*security_text);
    p.xml=profile_xml(p,context(),false,true);const auto bytes=encode(p);
    store_check(private_key_store_write(context(),profile_type,key(p.name),!(p.flags&2U),bytes,!prior.has_value()));
}
void Profiles::erase(std::u16string_view name){const auto p=get(name,write_access);require(!(p.flags&1U),5);store_check(private_key_store_delete(context(),profile_type,key(name),!(p.flags&2U)));}
}
