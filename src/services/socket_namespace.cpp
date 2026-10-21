#include "x64arm/socket_namespace.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/network.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace x64arm::socket_namespace {
namespace {
constexpr std::size_t limit=8*1024*1024;
const auto index=std::u16string(1,u'\1')+u"\\software\\x64arm\\winsock-namespace";
void number(std::vector<std::uint8_t>& bytes,std::uint64_t n,unsigned size){for(unsigned i=0;i<size;++i)bytes.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
void data(std::vector<std::uint8_t>& bytes,std::span<const std::uint8_t> value){if(value.size()>1024*1024)throw std::length_error("服務資料過大");number(bytes,value.size(),4);bytes.insert(bytes.end(),value.begin(),value.end());}
void text(std::vector<std::uint8_t>& bytes,std::u16string_view value){if(value.size()>4096)throw std::length_error("服務字串過長");number(bytes,value.size(),4);for(auto c:value)number(bytes,c,2);}
struct Reader {
    std::span<const std::uint8_t> bytes;std::size_t at{};
    std::uint64_t number(unsigned size){if(size>bytes.size()-at)throw std::invalid_argument("服務目錄截斷");std::uint64_t n=0;for(unsigned i=0;i<size;++i)n|=std::uint64_t{bytes[at++]}<<(8*i);return n;}
    std::vector<std::uint8_t> data(std::size_t maximum){const auto size=number(4);if(size>maximum || size>bytes.size()-at)throw std::invalid_argument("服務目錄資料無效");std::vector<std::uint8_t> out(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;return out;}
    std::u16string text(){const auto size=number(4);if(size>4096 || size>(bytes.size()-at)/2)throw std::invalid_argument("服務目錄字串無效");std::u16string out;for(std::size_t i=0;i<size;++i)out.push_back(static_cast<char16_t>(number(2)));return out;}
};
bool identity(const Record& a,const Record& b){return a.service_class==b.service_class && fold(a.name)==fold(b.name) && fold(a.context)==fold(b.context);}
}
std::u16string fold(std::u16string_view text){std::u16string result(text);for(auto& c:result)c=unicode_case_unit(c,false);return result;}
bool valid(const Address& a){
    const auto valid=[](const auto& bytes){return bytes.empty() || (bytes.size()==16 && bytes[0]==2 && !bytes[1]) || (bytes.size()==28 && bytes[0]==23 && !bytes[1]);};
    return valid(a.local) && valid(a.remote) && (!a.local.empty() || !a.remote.empty()) && a.type>=1 && a.type<=5 && a.protocol<=INT32_MAX && (a.local.empty() || a.remote.empty() || a.local[0]==a.remote[0]);
}
std::vector<Record> load(const RegistryState& state){
    const auto key=state.keys.find(index);if(key==state.keys.end())return {};const auto value=key->second.find(u"catalog");if(value==key->second.end())return {};
    if(value->second.type!=3 || value->second.bytes.size()>limit)throw std::invalid_argument("服務目錄型別無效");Reader r{value->second.bytes};
    if(r.number(8)!=UINT64_C(0x31534e5741523458))throw std::invalid_argument("服務目錄版本無效");const auto count=r.number(4);if(count>1024)throw std::invalid_argument("服務目錄過大");std::vector<Record> records;
    for(std::size_t i=0;i<count;++i){Record record;for(auto& c:record.service_class)c=static_cast<std::uint8_t>(r.number(1));record.name=r.text();record.context=r.text();record.comment=r.text();const auto flags=r.number(4);if(flags&~7U || record.name.empty())throw std::invalid_argument("服務目錄旗標無效");record.registered=(flags&1)!=0;
        if(flags&2)record.version=std::array{static_cast<std::uint32_t>(r.number(4)),static_cast<std::uint32_t>(r.number(4))};if(flags&4)record.blob=r.data(1024*1024);
        const auto n=r.number(4);if(n>256)throw std::invalid_argument("服務地址過多");for(std::size_t j=0;j<n;++j){Address a;a.type=static_cast<std::uint32_t>(r.number(4));a.protocol=static_cast<std::uint32_t>(r.number(4));a.local=r.data(128);a.remote=r.data(128);if(!valid(a))throw std::invalid_argument("服務地址無效");record.addresses.push_back(std::move(a));}
        if(std::any_of(records.begin(),records.end(),[&](const auto& item){return identity(item,record);}))throw std::invalid_argument("服務目錄身分重複");records.push_back(std::move(record));
    }if(r.at!=r.bytes.size())throw std::invalid_argument("服務目錄有多餘資料");return records;
}
void save(RegistryState& state,const std::vector<Record>& records){
    if(records.size()>1024)throw std::length_error("服務目錄容量不足");std::vector<std::uint8_t> bytes;number(bytes,UINT64_C(0x31534e5741523458),8);number(bytes,records.size(),4);
    for(const auto& r:records){bytes.insert(bytes.end(),r.service_class.begin(),r.service_class.end());text(bytes,r.name);text(bytes,r.context);text(bytes,r.comment);number(bytes,(r.registered?1U:0U)|(r.version?2U:0U)|(r.blob?4U:0U),4);if(r.version)for(auto n:*r.version)number(bytes,n,4);if(r.blob)data(bytes,*r.blob);
        if(r.addresses.size()>256)throw std::length_error("服務地址過多");number(bytes,r.addresses.size(),4);for(const auto& a:r.addresses){if(!valid(a))throw std::invalid_argument("服務地址無效");number(bytes,a.type,4);number(bytes,a.protocol,4);data(bytes,a.local);data(bytes,a.remote);}if(bytes.size()>limit)throw std::length_error("服務目錄容量不足");}
    auto& values=state.keys[index];const auto old=values.find(u"catalog");const auto previous=old==values.end()?0:old->second.bytes.size();if(state.bytes<previous || state.bytes-previous+bytes.size()>16*1024*1024 || (old==values.end() && state.values>=4096))throw std::length_error("登錄檔容量不足");
    state.bytes=state.bytes-previous+bytes.size();if(old==values.end())++state.values;values[u"catalog"]={3,std::move(bytes),{}};state.modified_times[index]=UINT64_C(116444736000000000)+static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()/100);
}
std::uint32_t change(std::vector<Record>& records,Record request,std::uint32_t operation,bool multiple){
    const auto found=std::find_if(records.begin(),records.end(),[&](const auto& record){return identity(record,request);});
    if(!operation){request.registered=true;std::vector<Address> addresses=multiple && found!=records.end()?found->addresses:std::vector<Address>{};for(auto& a:request.addresses)if(std::find(addresses.begin(),addresses.end(),a)==addresses.end())addresses.push_back(std::move(a));if(addresses.size()>256)return 8;request.addresses=std::move(addresses);if(found==records.end()){if(records.size()>=1024)return 8;records.push_back(std::move(request));}else *found=std::move(request);return 0;}
    if(operation>2)return 10022;if(found==records.end())return 10108;
    if(!multiple){if(operation==2)records.erase(found);else {found->addresses.clear();found->registered=false;}return 0;}
    std::erase_if(found->addresses,[&](const auto& a){return std::find(request.addresses.begin(),request.addresses.end(),a)!=request.addresses.end();});
    if(found->addresses.empty()){if(operation==2)records.erase(found);else found->registered=false;}return 0;
}
std::vector<std::uint8_t> information(const Record& r,bool wide,bool x86,std::uint64_t base,std::uint32_t flags){
    const unsigned unit=x86?4:8,fixed=x86?60:120;std::vector<std::uint8_t> bytes(fixed);
    const auto offset=[&](unsigned field){return field*(x86?4:8);};
    const auto set=[&](std::size_t at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    const auto append=[&](std::span<const std::uint8_t> data,unsigned alignment){while(bytes.size()%alignment)bytes.push_back(0);const auto at=bytes.size();bytes.insert(bytes.end(),data.begin(),data.end());return at;};
    const auto string=[&](unsigned cell,std::u16string_view value){auto data=wide?utf16_bytes(value):std::vector<std::uint8_t>{};if(!wide){const auto s=utf16_to_utf8(value);data.assign(s.begin(),s.end());data.push_back(0);}const auto at=append(data,wide?2:1);set(offset(cell),base+at,unit);};
    set(0,fixed);set(offset(5),r.namespace_id);const auto provider=append(r.namespace_id==nla_namespace?nla_provider_id:provider_id,4);set(offset(6),base+provider,unit);if(!r.context.empty())string(7,r.context);
    if(flags&16)string(1,r.name);if(flags&32){const auto at=append(r.service_class,4);set(offset(2),base+at,unit);}if((flags&64) && r.version){std::vector<std::uint8_t> version;for(auto n:*r.version)number(version,n,4);const auto at=append(version,4);set(offset(3),base+at,unit);}if((flags&128) && !r.comment.empty())string(4,r.comment);
    if((flags&256) && !r.addresses.empty()){
        const unsigned size=x86?24:40;const auto at=append(std::vector<std::uint8_t>(size*r.addresses.size()),unit);set(offset(11),r.addresses.size());set(offset(12),base+at,unit);
        std::vector<std::pair<std::uint32_t,std::uint32_t>> protocols;
        for(std::size_t i=0;i<r.addresses.size();++i){const auto& a=r.addresses[i];auto local=a.local,remote=a.remote;if(local.empty())local=remote;if(remote.empty())remote=local;if(flags&0x8000U)std::swap(local,remote);const auto entry=at+i*size;
            for(unsigned part=0;part<2;++part){const auto& data=part?remote:local;const auto address=append(data,4);set(entry+part*2*unit,base+address,unit);set(entry+part*2*unit+unit,data.size());}set(entry+4*unit,a.type);set(entry+4*unit+4,a.protocol);
            const auto& address=remote.empty()?local:remote;const auto family=static_cast<std::uint32_t>(address[0]|static_cast<unsigned>(address[1])<<8);const auto pair=std::pair{family,a.protocol};if(std::find(protocols.begin(),protocols.end(),pair)==protocols.end())protocols.push_back(pair);
        }std::vector<std::uint8_t> data;for(const auto& p:protocols){number(data,p.first,4);number(data,p.second,4);}const auto p=append(data,4);set(offset(8),protocols.size());set(offset(9),base+p,unit);
    }
    if((flags&512) && r.blob){const auto at=append(std::vector<std::uint8_t>(2*unit),unit);set(offset(14),base+at,unit);set(at,r.blob->size());if(!r.blob->empty()){const auto content=append(*r.blob,1);set(at+unit,base+content,unit);}}
    return bytes;
}
std::vector<std::uint8_t> providers(bool wide,bool x86,std::uint64_t base){
    const unsigned fixed=x86?32:40,unit=x86?4:8;std::vector<std::uint8_t> bytes(2*fixed);
    const auto set=[&](unsigned at,std::uint64_t n,unsigned size=4){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(n>>(8*i));};
    for(unsigned i=0;i<2;++i){const auto at=i*fixed;const auto& id=i?nla_provider_id:provider_id;std::copy(id.begin(),id.end(),bytes.begin()+at);set(at+16,i?nla_namespace:local_namespace);set(at+20,1);set(at+24,1);set(at+(x86?28:32),base+bytes.size(),unit);const auto name=i?u"MacWin 網路位置介面":u"MacWin 本機 TCP/IP 服務目錄";
        if(wide){const auto value=utf16_bytes(name);bytes.insert(bytes.end(),value.begin(),value.end());}else{const auto value=utf16_to_utf8(name);bytes.insert(bytes.end(),value.begin(),value.end());bytes.push_back(0);}}
    return bytes;
}
std::vector<Record> nla_records(const NetworkConfiguration& config){
    std::vector<Record> records;
    for(const auto& interface:config.interfaces){
        if(interface.status!=1 || interface.type==24 || interface.addresses.empty())continue;
        Record record;record.namespace_id=nla_namespace;record.service_class=nla_class_id;record.name=utf8_to_utf16(interface.name);record.comment=utf8_to_utf16(interface.description);record.registered=true;
        std::vector<std::uint8_t> blob;
        const auto size=static_cast<unsigned>((20+interface.name.size()+1+3)&~std::size_t{3});
        number(blob,1,4);number(blob,size,4);number(blob,size,4);number(blob,interface.type,4);number(blob,std::min<std::uint64_t>(interface.speed,UINT32_MAX),4);blob.insert(blob.end(),interface.name.begin(),interface.name.end());blob.resize(size);
        number(blob,3,4);number(blob,20,4);number(blob,0,4);number(blob,3,4);number(blob,0,4); // 未測量網際網路可達性，依契約回報 UNKNOWN。
        record.blob=std::move(blob);records.push_back(std::move(record));
    }
    return records;
}
}
