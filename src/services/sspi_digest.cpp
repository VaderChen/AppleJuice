#include "x64arm/sspi_digest.hpp"
#include "x64arm/crypto.hpp"
#include "x64arm/unicode.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <charconv>
#include <cstdio>

namespace x64arm::sspi_digest {
namespace {
void require(bool value,std::uint32_t status=invalid_token){if(!value)throw Error{status};}
std::string lower(std::string_view value){std::string out(value);for(auto& c:out)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return out;}
bool token_char(unsigned char c){return c>32 && c<127 && std::string_view("()<>@,;:\\\"/[]?={} ").find(static_cast<char>(c))==std::string_view::npos;}
std::string quoted(std::string_view value){std::string out="\"";for(char raw:value){const auto c=static_cast<unsigned char>(raw);require(c>=32 && c!=127);if(c=='\\' || c=='\"')out+='\\';out+=static_cast<char>(c);}return out+'\"';}
std::string field(const Directives& values,std::string_view key,bool mandatory=false){const auto it=values.find(std::string(key));if(it==values.end()){require(!mandatory);return {};}require(!mandatory || !it->second.empty());return it->second;}
std::string hex(std::span<const std::uint8_t> bytes){constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(bytes.size()*2);for(auto c:bytes){out+=digits[c>>4];out+=digits[c&15];}return out;}
bool is_hex(std::string_view value,std::size_t count){return value.size()==count && std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');});}
std::string count(std::uint32_t value){char text[9];std::snprintf(text,sizeof(text),"%08x",value);return text;}
std::uint32_t count(std::string_view value){require(is_hex(value,8),out_of_sequence);std::uint32_t result{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result,16);require(parsed.ec==std::errc{} && result,out_of_sequence);return result;}
std::vector<std::string> choices(std::string_view value){std::vector<std::string> out;std::size_t at=0;while(at<value.size()){auto end=value.find(',',at);if(end==value.npos)end=value.size();auto part=value.substr(at,end-at);while(!part.empty() && part.front()==' ')part.remove_prefix(1);while(!part.empty() && part.back()==' ')part.remove_suffix(1);if(!part.empty())out.push_back(lower(part));at=end+1;}return out;}
std::string latin(std::string_view value,bool allow_utf8,bool sasl){
    const auto wide=utf8_to_utf16(value);const bool representable=std::all_of(wide.begin(),wide.end(),[](auto c){return c<=255;});
    if(allow_utf8 && (!sasl || !representable))return std::string(value);
    require(representable,unsupported);std::string out;for(auto c:wide)out+=static_cast<char>(c);return out;
}
std::string wire_identity(const Context& c,std::string_view value){return c.utf8?std::string(value):latin(value,false,!c.http);}
std::string from_wire(std::string_view value,bool utf8){
    if(utf8){(void)utf8_to_utf16(value);return std::string(value);}
    std::u16string wide;wide.reserve(value.size());for(char raw:value)wide+=static_cast<char16_t>(static_cast<unsigned char>(raw));return utf16_to_utf8(wide);
}
void derive(Context& c){
    require(c.credentials.password && !c.credentials.user.empty(),no_credentials);
    Secret a1(latin(c.credentials.user,c.utf8,!c.http)+":"+(!c.http && c.utf8?c.realm:latin(c.realm,c.utf8,!c.http))+":"+latin(c.credentials.password->value,c.utf8,!c.http));
    CryptoHash digest("MD5",false,{});digest.update({reinterpret_cast<const std::uint8_t*>(a1.value.data()),a1.value.size()});const auto initial=digest.digest();
    std::string key;
    if(c.algorithm=="md5-sess"){
        // SASL 的 A1 使用原始 16 bytes；HTTP MD5-sess 使用第一輪的十六進位值。
        Secret session(c.http?hex(initial):std::string(reinterpret_cast<const char*>(initial.data()),initial.size()));session.value+=":"+c.nonce+":"+c.cnonce;key=hash(session.value);
    }else {require(c.http && c.algorithm=="md5",unsupported);key=hex(initial);}
    c.key=std::make_shared<Secret>(std::move(key));
}
std::string response(const Context& c,bool proof,std::string_view method,std::string_view entity){
    require(bool(c.key),out_of_sequence);std::string a2=(proof?"":std::string(c.http?method:"AUTHENTICATE"))+":"+c.uri;
    if(c.qop=="auth-int"){
        const auto body=c.http?(entity.empty()?hash(""):lower(entity)):std::string(32,'0');require(is_hex(body,32));a2+=":"+body;
    }
    const auto digest=hash(a2);
    if(c.qop.empty())return hash(c.key->value+":"+c.nonce+":"+digest);
    return hash(c.key->value+":"+c.nonce+":"+count(c.nc)+":"+c.cnonce+":"+c.qop+":"+digest);
}
Directives authorization(const Context& c,std::string value){
    Directives out{{"username",wire_identity(c,c.credentials.user)},{"realm",wire_identity(c,c.realm)},{"nonce",c.nonce},{c.http?"uri":"digest-uri",c.uri},{"response",std::move(value)}};
    if(c.http)out["algorithm"]=c.algorithm=="md5-sess"?"MD5-sess":"MD5";
    if(!c.qop.empty()){out["qop"]=c.qop;out["nc"]=count(c.nc);out["cnonce"]=c.cnonce;}else if(c.algorithm=="md5-sess")out["cnonce"]=c.cnonce;
    if(!c.opaque.empty() && c.http)out["opaque"]=c.opaque;if(c.utf8)out["charset"]="utf-8";return out;
}
void verify_authorization(const Context& c,const Directives& values){
    require(equal(field(values,"username",true),wire_identity(c,c.credentials.user)),logon_denied);
    require(equal(field(values,"realm"),wire_identity(c,c.realm)) && equal(field(values,"nonce",true),c.nonce) && equal(field(values,c.http?"uri":"digest-uri",true),c.uri),altered);
    require(lower(field(values,"qop"))==c.qop,altered);
    if(!c.qop.empty())require(count(field(values,"nc",true))==c.nc && equal(field(values,"cnonce",true),c.cnonce),out_of_sequence);
    if(c.algorithm=="md5-sess")require(equal(field(values,"cnonce",true),c.cnonce),altered);
    if(c.http && !c.opaque.empty())require(equal(field(values,"opaque",true),c.opaque),altered);
    const auto algorithm=lower(field(values,"algorithm"));require(algorithm.empty() || algorithm==c.algorithm,altered);
    const auto charset=lower(field(values,"charset"));require(charset.empty() || (c.utf8 && charset=="utf-8"),altered);
    require(field(values,"authzid").empty(),unsupported);
}
Directives proof(const Context& c,std::string value){Directives out{{"rspauth",std::move(value)}};if(c.http && !c.qop.empty()){out["qop"]=c.qop;out["cnonce"]=c.cnonce;out["nc"]=count(c.nc);}return out;}
}
Secret::~Secret(){if(!value.empty())OPENSSL_cleanse(value.data(),value.size());}
bool Credentials::operator==(const Credentials& x)const{return user==x.user && domain==x.domain && use==x.use && (password==x.password || (password && x.password && equal(password->value,x.password->value)));}
bool Context::operator==(const Context& x)const{
    return credentials==x.credentials && phase==x.phase && server==x.server && http==x.http && utf8==x.utf8 && mutual==x.mutual && flags==x.flags && nc==x.nc && realm==x.realm && nonce==x.nonce && cnonce==x.cnonce && qop==x.qop && algorithm==x.algorithm && uri==x.uri && opaque==x.opaque && method==x.method && entity==x.entity && (key==x.key || (key && x.key && equal(key->value,x.key->value)));
}
bool equal(std::string_view a,std::string_view b){return a.size()==b.size() && (a.empty() || CRYPTO_memcmp(a.data(),b.data(),a.size())==0);}
std::string hash(std::string_view value){CryptoHash digest("MD5",false,{});digest.update({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});return hex(digest.digest());}
Directives parse(std::string_view input,bool repeated_realms){
    require(input.size()<=65536);while(!input.empty() && (input.back()=='\0' || input.back()==' ' || input.back()=='\t'))input.remove_suffix(1);
    if(input.size()>7 && lower(input.substr(0,7))=="digest ")input.remove_prefix(7);
    Directives out;std::size_t at=0,entries=0;
    const auto space=[&]{while(at<input.size() && (input[at]==' ' || input[at]=='\t'))++at;};
    while(at<input.size()){
        space();const auto start=at;while(at<input.size() && token_char(static_cast<unsigned char>(input[at])))++at;require(at>start);const auto key=lower(input.substr(start,at-start));space();require(at<input.size() && input[at++]=='=');space();std::string value;
        if(at<input.size() && input[at]=='\"'){
            ++at;bool closed=false;while(at<input.size()){auto c=input[at++];if(c=='\"'){closed=true;break;}if(c=='\\'){require(at<input.size());c=input[at++];}require(static_cast<unsigned char>(c)>=32 && c!=127);value+=c;}require(closed);
        }else{const auto begin=at;while(at<input.size() && input[at]!=',' && input[at]!=' ' && input[at]!='\t'){require(token_char(static_cast<unsigned char>(input[at])));++at;}value=std::string(input.substr(begin,at-begin));require(!value.empty());}
        require(++entries<=64);if(out.contains(key)){require(repeated_realms && key=="realm");}else out.emplace(key,std::move(value));
        space();if(at==input.size())break;require(input[at++]==',');space();require(at<input.size());
    }return out;
}
std::string encode(const Directives& values){
    std::string out;for(const auto& [key,value]:values){if(!out.empty())out+=",";out+=key+"=";
        const bool bare=key=="qop" || key=="nc" || key=="algorithm" || key=="response" || key=="rspauth" || key=="charset" || key=="stale";
        if(bare && !value.empty() && std::all_of(value.begin(),value.end(),[](unsigned char c){return token_char(c);}))out+=value;else out+=quoted(value);
    }require(out.size()<=65536);return out;
}
Result client(Context& c,std::string_view token,std::string_view target,std::string_view method,std::string_view entity,std::string_view random){
    require(!c.server,invalid_handle);
    if(c.phase==Context::Phase::PeerProof){
        const auto values=parse(token);require(equal(lower(field(values,"rspauth",true)),response(c,true,"",entity)),altered);
        if(c.http && !c.qop.empty()){require(field(values,"qop")==c.qop && field(values,"cnonce")==c.cnonce && count(field(values,"nc",true))==c.nc,altered);}
        c.phase=Context::Phase::Established;return {};
    }
    require(c.phase==Context::Phase::Initial || c.phase==Context::Phase::Challenge,out_of_sequence);
    if(token.empty()){c.phase=Context::Phase::Challenge;return {{},continuing};}
    const auto values=parse(token,true);c.realm=field(values,"realm");c.nonce=field(values,"nonce",true);c.algorithm=lower(field(values,"algorithm"));
    if(c.algorithm.empty())c.algorithm=c.http?"md5":"md5-sess";require(c.algorithm=="md5-sess" || (c.http && c.algorithm=="md5"),unsupported);
    const auto charset=lower(field(values,"charset"));require(charset.empty() || charset=="utf-8",unsupported);c.utf8=charset=="utf-8";c.realm=from_wire(c.realm,c.utf8);
    const auto options=choices(field(values,"qop"));const bool integrity=(c.flags&0x1000cU)!=0;
    c.qop.clear();if(integrity){require(c.http && std::find(options.begin(),options.end(),"auth-int")!=options.end(),unsupported);c.qop="auth-int";}
    else if(!options.empty()){if(std::find(options.begin(),options.end(),"auth")!=options.end())c.qop="auth";else if(c.http && std::find(options.begin(),options.end(),"auth-int")!=options.end())c.qop="auth-int";else throw Error{unsupported};}
    require(c.http || c.qop=="auth",unsupported);require(!target.empty() && std::all_of(target.begin(),target.end(),[](unsigned char c){return c>32 && c<127;}));
    c.uri=target;c.cnonce=random;require(!c.cnonce.empty(),0x80090304);c.nc=1;c.opaque=field(values,"opaque");c.method=method;c.entity=entity;
    if(c.http)require(!c.method.empty() && std::all_of(c.method.begin(),c.method.end(),[](unsigned char v){return token_char(v);}));
    derive(c);c.phase=Context::Phase::ClientPending;c.mutual=!c.http || (c.flags&2U)!=0;
    return {encode(authorization(c,response(c,false,c.method,c.entity))),c.mutual?complete_continue:complete_needed};
}
Result server(Context& c,std::string_view token,std::string_view realm,std::string_view method,std::string_view entity,std::string_view random){
    require(c.server,invalid_handle);
    if(c.phase==Context::Phase::ServerAck){require(token.empty(),invalid_token);c.phase=Context::Phase::Established;return {};}
    if(c.phase==Context::Phase::Initial){
        require(token.empty(),out_of_sequence);c.realm=realm.empty()?c.credentials.domain:std::string(realm);if(c.http)require(std::all_of(c.realm.begin(),c.realm.end(),[](unsigned char v){return v<128;}),unsupported);c.nonce=random;require(!c.nonce.empty(),0x80090304);
        c.qop=c.http && (c.flags&0x2000cU)?"auth-int":"auth";c.algorithm="md5-sess";c.utf8=!c.http;c.opaque=c.http?hash("opaque:"+c.nonce):"";c.phase=Context::Phase::Challenge;
        Directives values{{"realm",wire_identity(c,c.realm)},{"nonce",c.nonce},{"algorithm",c.http?"MD5-sess":"md5-sess"},{"qop",c.qop}};
        if(c.utf8)values["charset"]="utf-8";if(c.http)values["opaque"]=c.opaque;
        return {encode(values),continuing};
    }
    require(c.phase==Context::Phase::Challenge,out_of_sequence);const auto values=parse(token);
    c.uri=field(values,c.http?"uri":"digest-uri",true);c.cnonce=field(values,"cnonce",true);const auto next=count(field(values,"nc",true));require(next==c.nc+1,out_of_sequence);c.nc=next;
    c.method=method;c.entity=entity;if(c.http)require(!method.empty());verify_authorization(c,values);derive(c);
    require(equal(lower(field(values,"response",true)),response(c,false,method,entity)),altered);
    c.phase=Context::Phase::ServerPending;c.mutual=true;
    return {encode(proof(c,response(c,true,"",{}))),complete_needed};
}
Result complete(Context& c,std::string_view token,std::string_view method,std::string_view entity){
    if(c.phase==Context::Phase::ClientPending){
        auto values=parse(token);verify_authorization(c,values);if(!method.empty())c.method=method;if(!entity.empty())c.entity=entity;
        require(!c.http || (!c.method.empty() && std::all_of(c.method.begin(),c.method.end(),[](unsigned char v){return token_char(v);})));values["response"]=response(c,false,c.method,c.entity);
        c.phase=c.mutual?Context::Phase::PeerProof:Context::Phase::Established;return {encode(values),0};
    }
    require(c.phase==Context::Phase::ServerPending,out_of_sequence);auto values=parse(token);require(is_hex(field(values,"rspauth",true),32));
    if(c.http && !c.qop.empty())require(field(values,"qop")==c.qop && field(values,"cnonce")==c.cnonce && count(field(values,"nc",true))==c.nc,altered);
    values["rspauth"]=response(c,true,"",entity);c.phase=c.http?Context::Phase::Established:Context::Phase::ServerAck;return {encode(values),0};
}
}
