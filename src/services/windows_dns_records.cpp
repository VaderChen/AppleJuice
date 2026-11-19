#include "service_callback_task.hpp"
#include "x64arm/windows_services.hpp"
#include "dns_query.hpp"
#include "x64arm/network.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <chrono>
#include <deque>
#include <future>
#include <dns_sd.h>
#include <map>
#include <optional>
#include <poll.h>
#include <cstring>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>

// dnsapi 的宿主原生實作：DNS_RECORD 與宿主 RR 模型（dns_detail::Record）互轉，比較、複製、拆分記錄集；
// 訊息編碼／解析與 DnsQuery 共用 resolver 的 RR 解析器；快取與 DnsQuery 共用同一份。
namespace x64arm {
namespace {
using dns_detail::Error;
using dns_detail::Record;
constexpr std::uint32_t invalid_parameter=87,invalid_name=123,non_rfc_name=9556,invalid_name_char=9560,numeric_name=9561;
bool name_record(std::uint16_t type){switch(type){case 2:case 3:case 4:case 5:case 7:case 8:case 9:case 12:case 39:return true;default:return false;}}
bool text_record(std::uint16_t type){return type==13 || type==16 || type==19 || type==20;}
// DnsValidateName：依序判定結構（空名稱、空標籤、長度、格式要求的標籤數）、無效字元、全數字、非 RFC 字元。
// RFC 字元為英數與連字號；底線與非 ASCII（UTF-8）為非 RFC；萬用字元格式的第一個標籤必須是 "*"，SRV 格式的標籤可用底線開頭。
std::uint32_t validate_name(std::string name,std::uint32_t format){
    if(format>6)return invalid_parameter;
    if(name.empty())return invalid_name;
    if(name==".")return format==0?0:invalid_name;
    if(name.back()=='.')name.pop_back();
    if(name.empty() || name.size()>255)return invalid_name;
    std::vector<std::string_view> labels;std::string_view rest=name;
    for(;;){const auto dot=rest.find('.');const auto label=rest.substr(0,dot);if(label.empty() || label.size()>63)return invalid_name;labels.push_back(label);if(dot==std::string_view::npos)break;rest.remove_prefix(dot+1);}
    if((format==1 || format==3) && labels.size()!=1)return invalid_name;
    if(format==4 && labels.front()!="*")return invalid_name;
    bool invalid=false,relaxed=false;
    for(std::size_t i=0;i<labels.size();++i)for(std::size_t at=0;at<labels[i].size();++at){
        const auto c=static_cast<unsigned char>(labels[i][at]);
        if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-')continue;
        if(c=='*' && format==4 && i==0)continue;
        if(c=='_'){if(!(format==5 && at==0))relaxed=true;continue;}
        if(c>=0x80){relaxed=true;continue;}
        invalid=true;
    }
    if(invalid)return invalid_name_char;
    const auto numeric=[](std::string_view label){return std::all_of(label.begin(),label.end(),[](unsigned char c){return c>='0' && c<='9';});};
    if(format==3 && numeric(labels.front()))return numeric_name;
    if(format==2 && std::all_of(labels.begin(),labels.end(),numeric))return numeric_name;
    if(format==6 && numeric(labels.back()))return numeric_name;
    return relaxed?non_rfc_name:0;
}
bool same_record(const Record& a,const Record& b){
    if(a.type!=b.type || !dns_detail::same_name(a.name,b.name) || a.raw!=b.raw || a.bytes!=b.bytes || a.numbers!=b.numbers || a.names.size()!=b.names.size())return false;
    for(std::size_t i=0;i<a.names.size();++i)if(text_record(a.type)?a.names[i]!=b.names[i]:!dns_detail::same_name(a.names[i],b.names[i]))return false;
    return true;
}
}

// DNS_SERVICE_INSTANCE 的宿主端內容（DnsServiceConstructInstance 與 DNS-SD 回呼共用同一個版面產生器）。
struct DnsServiceInstanceData {
    std::optional<std::u16string> name,host;std::optional<std::array<std::uint8_t,4>> ip4;std::optional<std::array<std::uint8_t,16>> ip6;
    std::uint16_t port{},priority{},weight{};std::vector<std::pair<std::optional<std::u16string>,std::optional<std::u16string>>> properties;std::uint32_t interface_index{};
};
// DNS-SD 操作：Bonjour 回呼（在 DNSServiceProcessResult 內、即處理該操作的客體執行緒上）把結果放進 events，
// internal-dnssd Worker 在執行緒池逐一轉成客體結構並呼叫客體回呼；delivering 為正在回呼的客體執行緒。
struct DnsServiceState {
    enum class Kind {BrowseV1,BrowseV2,Resolve,Register,DeRegister,Multicast};
    struct Event {std::uint32_t status{};std::vector<dns_detail::Record> records;std::optional<DnsServiceInstanceData> instance;};
    struct Operation {
        Kind kind{};DNSServiceRef ref{},address_ref{},connection{};std::vector<DNSRecordRef> address_records;
        std::uint64_t callback{},context{},handle{},delivering{};std::uint32_t interface_index{};
        std::deque<Event> events;std::vector<dns_detail::Record> pending; // 批次中的 mDNS 回應（kDNSServiceFlagsMoreComing）
        DnsServiceInstanceData instance; // 解析中或已註冊的服務
        std::chrono::steady_clock::time_point deadline{};bool canceled{},done{},idle{},address_started{};
        ~Operation(){if(connection)DNSServiceRefDeallocate(connection);if(address_ref)DNSServiceRefDeallocate(address_ref);if(ref)DNSServiceRefDeallocate(ref);}
    };
    std::map<std::uint64_t,std::unique_ptr<Operation>> operations;std::uint64_t next{0x6f000000};
    struct Context {std::u16string user,domain;};
    std::map<std::uint64_t,Context> contexts;std::uint64_t next_context{0x6f800000};
};
namespace {
// DNS 名稱表示法：去掉結尾的點，\DDD 轉回字元（保留 \. 與 \\）。
std::string dnssd_name(std::string_view text){
    std::string out;
    for(std::size_t i=0;i<text.size();++i){
        if(text[i]=='\\' && i+3<text.size() && std::isdigit(static_cast<unsigned char>(text[i+1])) && std::isdigit(static_cast<unsigned char>(text[i+2])) && std::isdigit(static_cast<unsigned char>(text[i+3]))){
            const auto value=(text[i+1]-'0')*100+(text[i+2]-'0')*10+(text[i+3]-'0');
            if(value=='.' || value=='\\'){out.push_back('\\');}out.push_back(static_cast<char>(value));i+=3;continue;
        }
        out.push_back(text[i]);
    }
    if(!out.empty() && out.back()=='.' && (out.size()<2 || out[out.size()-2]!='\\'))out.pop_back();
    return out;
}
// "Instance._svc._tcp.local" → (instance, "_svc._tcp", "local")；browse 名稱沒有實例部分。
std::optional<std::array<std::string,3>> dnssd_split(const std::string& name,bool instance){
    std::size_t at=0;std::string label;
    if(instance){
        const auto marker=name.find("._");if(marker==std::string::npos || !marker)return std::nullopt;
        for(std::size_t i=0;i<marker;++i){if(name[i]=='\\' && i+1<marker)++i;label.push_back(name[i]);}at=marker+1;
    }
    const auto first=name.find('.',at);if(first==std::string::npos || name[at]!='_')return std::nullopt;
    const auto second=name.find('.',first+1);const auto type_end=second==std::string::npos?name.size():second;
    if(first+1>=name.size() || name[first+1]!='_')return std::nullopt;
    std::string domain=second==std::string::npos?std::string{}:name.substr(second+1);if(!domain.empty() && domain.back()=='.')domain.pop_back();
    return std::array<std::string,3>{label,name.substr(at,type_end-at),domain};
}
std::uint32_t dnssd_status(DNSServiceErrorType error){
    switch(error){
        case kDNSServiceErr_NoError:return 0;
        case kDNSServiceErr_NoSuchName:case kDNSServiceErr_NoSuchRecord:return 9003;   // DNS_ERROR_RCODE_NAME_ERROR
        case kDNSServiceErr_NameConflict:return 9007;                                   // DNS_ERROR_RCODE_YXDOMAIN
        case kDNSServiceErr_BadParam:case kDNSServiceErr_BadFlags:case kDNSServiceErr_BadInterfaceIndex:case kDNSServiceErr_BadReference:return 87;
        case kDNSServiceErr_NoMemory:return 14;
        case kDNSServiceErr_Timeout:return 1460;
        case kDNSServiceErr_NoAuth:case kDNSServiceErr_NotPermitted:case kDNSServiceErr_PolicyDenied:return 5;
        case kDNSServiceErr_ServiceNotRunning:return 1062;
        default:return 9002;                                                              // DNS_ERROR_RCODE_SERVER_FAILURE
    }
}
// mDNS 回應的 RDATA（名稱不壓縮）轉成共用 RR 模型。
std::optional<dns_detail::Record> dnssd_record(const char* name,std::uint16_t type,std::uint32_t ttl,const std::uint8_t* data,std::uint16_t size){
    dns_detail::Record record;record.name=dnssd_name(name);record.type=type;record.ttl=ttl;record.section=1;
    const auto* cursor=data;const auto* end=data+size;
    const auto number=[&](unsigned width)->std::optional<std::uint32_t>{if(static_cast<std::size_t>(end-cursor)<width)return std::nullopt;std::uint32_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|*cursor++;return value;};
    const auto domain=[&]()->bool{std::array<char,NS_MAXDNAME> expanded{};const auto used=::dn_expand(data,end,cursor,expanded.data(),static_cast<int>(expanded.size()));if(used<0)return false;cursor+=used;record.names.emplace_back(expanded.data());return true;};
    switch(type){
        case 1:case 28:if(size!=(type==1?4:16))return std::nullopt;record.bytes.assign(data,end);break;
        case 2:case 5:case 12:case 39:if(!domain())return std::nullopt;break;
        case 33:for(unsigned i=0;i<3;++i){const auto value=number(2);if(!value)return std::nullopt;record.numbers.push_back(*value);}if(!domain())return std::nullopt;break;
        case 16:while(cursor<end){const auto length=*cursor++;if(length>end-cursor)return std::nullopt;record.names.emplace_back(reinterpret_cast<const char*>(cursor),length);cursor+=length;}break;
        default:record.bytes.assign(data,end);break;
    }
    return record;
}
// TXT 記錄 ↔ DNS_SERVICE_INSTANCE 的 keys／values（"key=value"；沒有等號的鍵值為 NULL）。
std::vector<std::uint8_t> dnssd_txt(const DnsServiceInstanceData& data){
    std::vector<std::uint8_t> bytes;
    for(const auto& [key,value]:data.properties){if(!key)continue;auto item=utf16_to_utf8(*key);if(value){item+='=';item+=utf16_to_utf8(*value);}if(item.size()>255)item.resize(255);bytes.push_back(static_cast<std::uint8_t>(item.size()));bytes.insert(bytes.end(),item.begin(),item.end());}
    if(bytes.empty())bytes.push_back(0);
    return bytes;
}
void dnssd_properties(DnsServiceInstanceData& data,const unsigned char* txt,std::uint16_t size){
    data.properties.clear();
    for(std::uint16_t at=0;at<size;){const auto length=txt[at++];if(length>size-at)break;const std::string item(reinterpret_cast<const char*>(txt+at),length);at+=length;if(item.empty())continue;
        const auto equal=item.find('=');if(equal==std::string::npos)data.properties.push_back({utf8_to_utf16_lossy(item),std::nullopt});
        else data.properties.push_back({utf8_to_utf16_lossy(item.substr(0,equal)),utf8_to_utf16_lossy(item.substr(equal+1))});}
}
// DNS_SERVICE_INSTANCE 與其字串、位址、屬性陣列放在同一區塊；回傳區塊（指標欄位為區塊內偏移）與需要重定位的欄位。
std::pair<std::vector<std::uint8_t>,std::vector<std::size_t>> dnssd_instance_layout(const DnsServiceInstanceData& data,bool x86){
    const unsigned unit=x86?4U:8U,size=x86?40U:72U;std::vector<std::uint8_t> block(size);std::vector<std::size_t> pointers;
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)block[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto align=[&]{block.resize((block.size()+unit-1)/unit*unit);};
    const auto attach=[&](std::size_t field,const std::vector<std::uint8_t>& bytes){align();put(field,block.size(),unit);pointers.push_back(field);block.insert(block.end(),bytes.begin(),bytes.end());};
    if(data.name)attach(0,utf16_bytes(*data.name));if(data.host)attach(unit,utf16_bytes(*data.host));
    if(data.ip4)attach(2*unit,{data.ip4->begin(),data.ip4->end()});if(data.ip6)attach(3*unit,{data.ip6->begin(),data.ip6->end()});
    put(4*unit,data.port,2);put(4*unit+2,data.priority,2);put(4*unit+4,data.weight,2);const auto count=static_cast<std::uint32_t>(data.properties.size());put(x86?24:40,count,4);put(x86?36:64,data.interface_index,4);
    if(count){
        align();const auto keys=block.size();put(x86?28:48,keys,unit);pointers.push_back(x86?28:48);block.resize(keys+count*unit);
        align();const auto values=block.size();put(x86?32:56,values,unit);pointers.push_back(x86?32:56);block.resize(values+count*unit);
        for(std::uint32_t i=0;i<count;++i){if(data.properties[i].first)attach(keys+i*unit,utf16_bytes(*data.properties[i].first));if(data.properties[i].second)attach(values+i*unit,utf16_bytes(*data.properties[i].second));}
    }
    return {std::move(block),std::move(pointers)};
}
using DnsOperation=DnsServiceState::Operation;
void DNSSD_API dnssd_browse_reply(DNSServiceRef,DNSServiceFlags flags,std::uint32_t,DNSServiceErrorType error,const char* name,const char* type,const char* domain,void* context){
    auto& operation=*static_cast<DnsOperation*>(context);
    if(error){operation.events.push_back({dnssd_status(error),{},std::nullopt});return;}
    // 每個實例一筆 PTR：服務型別 → 實例全名；移除時 TTL 為 0。
    dns_detail::Record record;record.type=12;record.section=1;record.ttl=(flags&kDNSServiceFlagsAdd)?4500U:0U;
    std::string service=type?type:"";if(!service.empty() && service.back()=='.')service.pop_back();std::string zone=domain?domain:"";if(!zone.empty() && zone.back()=='.')zone.pop_back();
    record.name=service+(zone.empty()?"":"."+zone);std::string label;for(const char* c=name;c && *c;++c){if(*c=='.' || *c=='\\')label.push_back('\\');label.push_back(*c);}
    record.names.push_back(label+"."+record.name);operation.pending.push_back(std::move(record));
    if(!(flags&kDNSServiceFlagsMoreComing)){operation.events.push_back({0,std::move(operation.pending),std::nullopt});operation.pending.clear();}
}
void DNSSD_API dnssd_address_reply(DNSServiceRef,DNSServiceFlags flags,std::uint32_t,DNSServiceErrorType error,const char*,const sockaddr* address,std::uint32_t,void* context){
    auto& operation=*static_cast<DnsOperation*>(context);
    if(error){operation.events.push_back({dnssd_status(error),{},std::nullopt});operation.done=true;return;}
    if(address && address->sa_family==AF_INET && !operation.instance.ip4){std::array<std::uint8_t,4> ip{};std::memcpy(ip.data(),&reinterpret_cast<const sockaddr_in*>(address)->sin_addr,4);operation.instance.ip4=ip;}
    if(address && address->sa_family==AF_INET6 && !operation.instance.ip6){std::array<std::uint8_t,16> ip{};std::memcpy(ip.data(),&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr,16);operation.instance.ip6=ip;}
    if(!(flags&kDNSServiceFlagsMoreComing) && (operation.instance.ip4 || operation.instance.ip6) && !operation.done){operation.events.push_back({0,{},operation.instance});operation.done=true;}
}
void DNSSD_API dnssd_resolve_reply(DNSServiceRef,DNSServiceFlags,std::uint32_t index,DNSServiceErrorType error,const char*,const char* target,std::uint16_t port,std::uint16_t size,const unsigned char* txt,void* context){
    auto& operation=*static_cast<DnsOperation*>(context);
    if(error){operation.events.push_back({dnssd_status(error),{},std::nullopt});operation.done=true;return;}
    if(operation.address_started)return;operation.address_started=true;
    operation.instance.host=utf8_to_utf16_lossy(dnssd_name(target?target:""));operation.instance.port=ntohs(port);dnssd_properties(operation.instance,txt,size);
    operation.instance.interface_index=operation.interface_index;
    // 主機位址：同一操作再以 DNSServiceGetAddrInfo 取得 IPv4／IPv6。
    const auto status=DNSServiceGetAddrInfo(&operation.address_ref,0,index,kDNSServiceProtocol_IPv4|kDNSServiceProtocol_IPv6,target,dnssd_address_reply,&operation);
    if(status){operation.address_ref=nullptr;operation.events.push_back({dnssd_status(status),{},std::nullopt});operation.done=true;}
}
void DNSSD_API dnssd_register_reply(DNSServiceRef,DNSServiceFlags,DNSServiceErrorType error,const char* name,const char* type,const char* domain,void* context){
    auto& operation=*static_cast<DnsOperation*>(context);
    if(error){operation.events.push_back({dnssd_status(error),{},std::nullopt});return;}
    // 名稱可能因衝突被改名；回報實際註冊的全名。
    std::string service=type?type:"";if(!service.empty() && service.back()=='.')service.pop_back();std::string zone=domain?domain:"";if(!zone.empty() && zone.back()=='.')zone.pop_back();
    std::string label;for(const char* c=name;c && *c;++c){if(*c=='.' || *c=='\\')label.push_back('\\');label.push_back(*c);}
    operation.instance.name=utf8_to_utf16_lossy(label+"."+service+(zone.empty()?"":"."+zone));operation.events.push_back({0,{},operation.instance});
}
void DNSSD_API dnssd_record_reply(DNSServiceRef,DNSRecordRef,DNSServiceFlags,DNSServiceErrorType error,void* context){
    if(error)static_cast<DnsOperation*>(context)->events.push_back({dnssd_status(error),{},std::nullopt});
}
void DNSSD_API dnssd_query_reply(DNSServiceRef,DNSServiceFlags flags,std::uint32_t,DNSServiceErrorType error,const char* name,std::uint16_t type,std::uint16_t,std::uint16_t size,const void* data,std::uint32_t ttl,void* context){
    auto& operation=*static_cast<DnsOperation*>(context);
    if(error){operation.events.push_back({dnssd_status(error),{},std::nullopt});return;}
    if(auto record=dnssd_record(name,type,(flags&kDNSServiceFlagsAdd)?ttl:0U,static_cast<const std::uint8_t*>(data),size))operation.pending.push_back(std::move(*record));
    if(!(flags&kDNSServiceFlagsMoreComing) && !operation.pending.empty()){operation.events.push_back({0,std::move(operation.pending),std::nullopt});operation.pending.clear();}
}
}

std::uint64_t WindowsServices::dns_store_instance(GuestMemory& memory,const DnsServiceInstanceData& data){
    auto [block,pointers]=dnssd_instance_layout(data,x86_);const unsigned unit=x86_?4U:8U;
    const auto address=allocate_heap(memory,block.size());if(!address)return 0;
    for(const auto field:pointers){std::uint64_t relative=0;for(unsigned i=0;i<unit;++i)relative|=std::uint64_t{block[field+i]}<<(i*8);const auto pointer=address+relative;for(unsigned i=0;i<unit;++i)block[field+i]=static_cast<std::uint8_t>(pointer>>(i*8));}
    memory.write(GuestAddress{address},block);runtime_.network.addresses.insert(address);return address;
}
std::uint64_t WindowsServices::dns_store_records(GuestState& state,GuestMemory& memory,const std::vector<Record>& records,unsigned charset,const std::vector<unsigned>* charsets){
    const unsigned unit=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(unit*8);std::vector<std::uint64_t> allocated;
    try{
        for(std::size_t index=0;index<records.size();++index){
            auto blob=dns_detail::pack(records[index],unit,charsets?charsets->at(index):charset);
            const auto address=allocate_heap(memory,blob.bytes.size());if(!address)throw Error{8};allocated.push_back(address);
            for(const auto offset:blob.pointers){std::uint64_t relative=0;for(unsigned i=0;i<unit;++i)relative|=std::uint64_t{blob.bytes[offset+i]}<<(i*8);const auto pointer=address+relative;for(unsigned i=0;i<unit;++i)blob.bytes[offset+i]=static_cast<std::uint8_t>(pointer>>(i*8));}
            memory.write(GuestAddress{address},blob.bytes);runtime_.network.addresses.insert(address);
            if(allocated.size()>1)memory.store(GuestAddress{allocated[allocated.size()-2]},width,address);
        }
    }catch(...){for(const auto address:allocated){runtime_.network.addresses.erase(address);(void)free_heap(memory,address,state);}throw;}
    return allocated.empty()?0:allocated.front();
}

std::optional<std::uint32_t> WindowsServices::invoke_dns_records(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto symbol=service.symbol;const unsigned unit=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(unit*8);
    const auto load=[&](std::uint64_t address,unsigned bits){return memory.load(GuestAddress{address},static_cast<std::uint8_t>(bits));};
    const auto suffix_charset=[&]{return symbol.ends_with("_W")?1U:2U;};
    const auto text=[&](std::uint64_t pointer,unsigned charset)->std::string{if(!pointer)return {};return charset==1?utf16_to_utf8(read_utf16(memory,GuestAddress{pointer},1024)):read_ansi(memory,pointer,1024);};
    // DNS_RECORD：pNext、pName、wType、wDataLength、Flags（Section 0～1、Delete 2、CharSet 3～4）、dwTtl、dwReserved，資料自 2×指標＋16 起。
    const unsigned header=2*unit+16;
    const auto record_charset=[&](std::uint64_t address){const auto value=(load(address+2*unit+4,32)>>3)&3U;return value?static_cast<unsigned>(value):1U;};
    const auto unpack=[&](std::uint64_t address,unsigned charset){
        Record record;record.type=static_cast<std::uint16_t>(load(address+2*unit,16));const auto length=static_cast<unsigned>(load(address+2*unit+2,16));const auto flags=static_cast<std::uint32_t>(load(address+2*unit+4,32));
        record.name=text(load(address+unit,width),charset);record.section=flags&3U;record.flags=flags&~0x1bU;record.ttl=static_cast<std::uint32_t>(load(address+2*unit+8,32));
        const auto data=address+header;const auto need=[&](unsigned size){if(length<size)throw Error{invalid_parameter};};
        const auto pointer_text=[&](std::uint64_t at){record.names.push_back(text(load(at,width),charset));};
        if(name_record(record.type)){need(unit);pointer_text(data);}
        else if(record.type==6){need(2*unit+20);pointer_text(data);pointer_text(data+unit);for(unsigned i=0;i<5;++i)record.numbers.push_back(static_cast<std::uint32_t>(load(data+2*unit+i*4,32)));}
        else if(record.type==14 || record.type==17){need(2*unit);pointer_text(data);pointer_text(data+unit);}
        else if(record.type==15 || record.type==18 || record.type==21 || record.type==33){const unsigned count=record.type==33?3:1;need(unit+count*2);pointer_text(data);for(unsigned i=0;i<count;++i)record.numbers.push_back(static_cast<std::uint32_t>(load(data+unit+i*2,16)));}
        else if(text_record(record.type)){need(unit);const auto count=static_cast<unsigned>(load(data,32));if(count>255 || length<unit+count*unit)throw Error{invalid_parameter};for(unsigned i=0;i<count;++i)pointer_text(data+unit+i*unit);}
        else {
            // 沒有內嵌指標的型別（A／AAAA／WKS、NULL 與本引擎以位元組保存的其他型別）依 wDataLength 原樣複製。
            record.raw=true;record.bytes.resize(length);if(length)memory.read(GuestAddress{data},record.bytes);
        }
        return record;
    };
    const auto chain=[&](std::uint64_t head){std::vector<std::uint64_t> items;std::set<std::uint64_t> seen;for(auto at=head;at;at=load(at,width)){if(!seen.insert(at).second || items.size()>=65536)throw Error{invalid_parameter};items.push_back(at);}return items;};
    const auto valid_charset=[](std::uint64_t value){return value>=1 && value<=3;};
#include "windows_dns_service.inc"
    std::uint64_t result=0;
    try{
        if(symbol=="DnsNameCompare_A" || symbol=="DnsNameCompare_W"){
            const unsigned charset=symbol.ends_with("W")?1:2;result=call(0) && call(1) && dns_detail::same_name(text(call(0),charset),text(call(1),charset));
        }else if(symbol.starts_with("DnsValidateName_")){
            const unsigned charset=symbol.ends_with("_W")?1:2;result=call(0)?validate_name(text(call(0),charset),static_cast<std::uint32_t>(call(1))):invalid_name;
        }else if(symbol=="DnsRecordCompare"){
            result=call(0) && call(1) && record_charset(call(0))==record_charset(call(1)) && same_record(unpack(call(0),record_charset(call(0))),unpack(call(1),record_charset(call(1))));
        }else if(symbol=="DnsRecordSetCompare"){
            // 不同字元集的記錄視為不同；差異清單是新配置的複本，以 DnsRecordListFree 釋放。
            for(unsigned i=2;i<4;++i)if(call(i)){memory.check(GuestAddress{call(i)},unit,Access::Write);memory.store(GuestAddress{call(i)},width,0);}
            std::array<std::vector<Record>,2> sets;std::array<std::vector<unsigned>,2> charsets;
            for(unsigned side=0;side<2;++side)for(const auto at:chain(call(side))){charsets[side].push_back(record_charset(at));sets[side].push_back(unpack(at,charsets[side].back()));}
            std::array<std::vector<Record>,2> diff;std::array<std::vector<unsigned>,2> diff_charsets;
            for(unsigned side=0;side<2;++side)for(std::size_t i=0;i<sets[side].size();++i){
                const auto& other=sets[1-side];bool found=false;for(std::size_t j=0;j<other.size() && !found;++j)found=charsets[side][i]==charsets[1-side][j] && same_record(sets[side][i],other[j]);
                if(!found){diff[side].push_back(sets[side][i]);diff_charsets[side].push_back(charsets[side][i]);}
            }
            std::array<std::uint64_t,2> heads{};
            for(unsigned side=0;side<2;++side)if(call(side+2) && !diff[side].empty()){try{heads[side]=dns_store_records(state,memory,diff[side],1,&diff_charsets[side]);}catch(...){if(side && heads[0]){for(const auto at:chain(heads[0])){runtime_.network.addresses.erase(at);(void)free_heap(memory,at,state);}}throw;}}
            for(unsigned side=0;side<2;++side)if(call(side+2))memory.store(GuestAddress{call(side+2)},width,heads[side]);
            result=diff[0].empty() && diff[1].empty();
        }else if(symbol=="DnsRecordCopyEx" || symbol=="DnsRecordSetCopyEx"){
            if(!call(0) || !valid_charset(call(1)) || !valid_charset(call(2))){last_error_=invalid_parameter;result=0;}
            else {
                std::vector<Record> records;const auto items=symbol=="DnsRecordCopyEx"?std::vector<std::uint64_t>{call(0)}:chain(call(0));
                for(const auto at:items)records.push_back(unpack(at,static_cast<unsigned>(call(1))));
                result=dns_store_records(state,memory,records,static_cast<unsigned>(call(2)));
            }
        }else if(symbol=="DnsRecordSetDetach"){
            // 第一個記錄集＝開頭同名同型別的連續記錄；截斷後回傳其後的清單。
            const auto first=call(0);
            if(first){
                const auto charset=record_charset(first);const auto name=text(load(first+unit,width),charset);const auto type=load(first+2*unit,16);
                auto last=first;std::size_t steps=0;
                for(auto next=load(last,width);next && ++steps<65536;next=load(last,width)){if(load(next+2*unit,16)!=type || record_charset(next)!=charset || !dns_detail::same_name(text(load(next+unit,width),charset),name))break;last=next;}
                result=load(last,width);memory.store(GuestAddress{last},width,0);
            }
        }else if(symbol=="DnsWriteQuestionToBuffer_W" || symbol=="DnsWriteQuestionToBuffer_UTF8"){
            // XID 原樣寫入（不翻轉），問題數以網路位元組順序；不足時回傳所需大小並設 ERROR_INSUFFICIENT_BUFFER。
            if(!call(1) || !call(2)){last_error_=invalid_parameter;result=0;}
            else {
                const auto name=dns_detail::encode_name(text(call(2),suffix_charset()));const auto xid=static_cast<std::uint16_t>(call(4));const auto type=static_cast<std::uint16_t>(call(3));
                std::vector<std::uint8_t> message(12);message[0]=static_cast<std::uint8_t>(xid);message[1]=static_cast<std::uint8_t>(xid>>8);message[2]=call(5)?1:0;message[5]=1;
                message.insert(message.end(),name.begin(),name.end());message.insert(message.end(),{static_cast<std::uint8_t>(type>>8),static_cast<std::uint8_t>(type),0,1});
                const auto capacity=load(call(1),32);memory.check(GuestAddress{call(1)},4,Access::Write);
                if(!call(0) || capacity<message.size()){memory.store(GuestAddress{call(1)},32,message.size());last_error_=122;result=0;}
                else {memory.write(GuestAddress{call(0)},message);memory.store(GuestAddress{call(1)},32,message.size());result=1;}
            }
        }else if(symbol=="DnsExtractRecordsFromMessage_W" || symbol=="DnsExtractRecordsFromMessage_UTF8"){
            // 呼叫端依 DNS_BYTE_FLIP_HEADER_COUNTS 把標頭計數轉成主機順序；此處轉回網路順序後交給共用解析器。
            const auto length=static_cast<unsigned>(call(1)&0xffff);
            if(!call(0) || !call(2))result=invalid_parameter;
            else {
                memory.check(GuestAddress{call(2)},unit,Access::Write);memory.store(GuestAddress{call(2)},width,0);
                if(length<12)result=9502;
                else {
                    std::vector<std::uint8_t> message(length);memory.read(GuestAddress{call(0)},message);for(unsigned i=4;i<12;i+=2)std::swap(message[i],message[i+1]);
                    const auto records=dns_detail::parse_message(message.data(),static_cast<int>(message.size()));
                    if(std::none_of(records.begin(),records.end(),[](const auto& record){return record.section==1;}))result=9501;
                    else memory.store(GuestAddress{call(2)},width,dns_store_records(state,memory,records,suffix_charset()));
                }
            }
        }else if(symbol=="DnsFlushResolverCache"){
            auto& cache=dns_detail::cache_state();std::lock_guard lock(cache.mutex);cache.entries.clear();result=1;
        }else if(symbol.starts_with("DnsFlushResolverCacheEntry_")){
            if(!call(0))result=0;
            else {
                auto name=dns_detail::lower(text(call(0),symbol.ends_with("_W")?1U:2U));if(name.size()>1 && name.back()=='.')name.pop_back();
                auto& cache=dns_detail::cache_state();std::lock_guard lock(cache.mutex);std::erase_if(cache.entries,[&](const auto& item){return item.first.first==name;});result=1;
            }
        }else if(symbol=="DnsGetCacheDataTable"){
            // DNS_CACHE_ENTRY：pNext、pszName（UTF-16）、wType、wDataLength、dwFlags；每個項目與名稱各自配置，以 DnsFree(…, DnsFreeFlat) 釋放。
            if(!call(0))result=0;
            else {
                memory.check(GuestAddress{call(0)},unit,Access::Write);memory.store(GuestAddress{call(0)},width,0);
                std::vector<std::pair<std::string,std::uint16_t>> keys;
                {auto& cache=dns_detail::cache_state();std::lock_guard lock(cache.mutex);const auto now=dns_detail::Clock::now();
                    for(const auto& [key,value]:cache.entries)if(std::chrono::duration_cast<std::chrono::seconds>(now-value.created).count()<value.ttl)keys.push_back(key);}
                std::vector<std::uint64_t> allocated;const unsigned size=x86_?16U:24U;std::uint64_t previous=0,head=0;
                try{
                    for(const auto& [name,type]:keys){
                        const auto string=utf16_bytes(utf8_to_utf16_lossy(name));const auto entry=allocate_heap(memory,size),text_block=allocate_heap(memory,string.size());
                        if(entry)allocated.push_back(entry);if(text_block)allocated.push_back(text_block);if(!entry || !text_block)throw Error{8};
                        memory.write(GuestAddress{text_block},string);std::vector<std::uint8_t> bytes(size);
                        for(unsigned i=0;i<unit;++i)bytes[unit+i]=static_cast<std::uint8_t>(text_block>>(i*8));bytes[2*unit]=static_cast<std::uint8_t>(type);bytes[2*unit+1]=static_cast<std::uint8_t>(type>>8);
                        memory.write(GuestAddress{entry},bytes);if(previous)memory.store(GuestAddress{previous},width,entry);else head=entry;previous=entry;
                    }
                }catch(...){for(const auto at:allocated)(void)free_heap(memory,at,state);throw;}
                for(const auto at:allocated)runtime_.network.addresses.insert(at);
                memory.store(GuestAddress{call(0)},width,head);result=1;
            }
        }else if(symbol=="DnsQueryConfig"){
            // (Config, Flag, pwsAdapterName, pReserved, pBuffer, pBufLen)；DNS_CONFIG_FLAG_ALLOC 以 LocalAlloc 相容配置回傳指標。
            const auto type=static_cast<std::uint32_t>(call(0));const bool allocate=call(1)&1;
            if(!call(5))result=invalid_parameter;
            else {
                auto& config=*runtime_.network.configuration;
                if(runtime_.network.external_io)(void)snapshot_network_configuration(config);
                if(!config.captured)throw HostCallFault("比較模式不讀取宿主 DNS 設定");
                const auto string=[&](const std::string& value,unsigned charset){std::vector<std::uint8_t> bytes;if(charset==1)bytes=utf16_bytes(utf8_to_utf16_lossy(value));else {bytes.assign(value.begin(),value.end());bytes.push_back(0);}return bytes;};
                const auto dword=[](std::uint32_t value){return std::vector<std::uint8_t>{static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)};};
                std::vector<std::uint8_t> data;std::uint32_t status=0;
                const auto full=config.domain_name.empty()?config.host_name:config.host_name+"."+config.domain_name;
                const auto charset_of=[](std::uint32_t base,std::uint32_t value){return value-base==0?1U:value-base==1?3U:2U;};
                if(type<=2)data=string(config.domain_name,charset_of(0,type));
                else if(type<=5)data=string(config.domain_name,charset_of(3,type));
                else if(type==6){
                    std::vector<std::array<std::uint8_t,4>> list;for(const auto& address:config.dns)if(address.size()>=8 && address[0]==2){std::array<std::uint8_t,4> ip{};std::copy_n(address.begin()+4,4,ip.begin());if(std::find(list.begin(),list.end(),ip)==list.end())list.push_back(ip);}
                    if(list.empty())status=9852;else {data=dword(static_cast<std::uint32_t>(list.size()));for(const auto& ip:list)data.insert(data.end(),ip.begin(),ip.end());}
                }else if(type==9)data=dword(1);
                else if(type==10)data=dword(0);
                else if(type==11)data=dword(1);
                else if(type>=12 && type<=14)data=string(config.host_name,charset_of(12,type));
                else if(type>=15 && type<=17)data=string(full,charset_of(15,type));
                else if(type==18){
                    // DNS_ADDR_ARRAY：32 位元組標頭＋每筆 64 位元組（SOCKADDR 與長度）。
                    std::vector<std::vector<std::uint8_t>> list;for(const auto& address:config.dns)if(address.size()>=8 && (address[0]==2 || address[0]==23) && std::find(list.begin(),list.end(),address)==list.end())list.push_back(address);
                    if(list.empty())status=9852;
                    else {
                        const bool v4=std::all_of(list.begin(),list.end(),[](const auto& a){return a[0]==2;}),v6=std::all_of(list.begin(),list.end(),[](const auto& a){return a[0]==23;});
                        data.resize(32+list.size()*64);const auto put=[&](std::size_t at,std::uint32_t value,unsigned size){for(unsigned i=0;i<size;++i)data[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                        put(0,static_cast<std::uint32_t>(list.size()),4);put(4,static_cast<std::uint32_t>(list.size()),4);put(12,v4?2U:v6?23U:0U,2);
                        for(std::size_t i=0;i<list.size();++i){const auto& sa=list[i];const unsigned sa_size=sa[0]==2?16U:28U;std::copy_n(sa.begin(),std::min<std::size_t>(sa.size(),sa_size),data.begin()+static_cast<std::ptrdiff_t>(32+i*64));put(32+i*64+32,sa_size,4);}
                    }
                }else if(type==7 || type==8)status=50; // DnsConfigSearchList／AdapterInfo 的資料版面未公開。
                else status=invalid_parameter;
                if(!status){
                    if(allocate){
                        if(!call(4))status=invalid_parameter;
                        else {const auto block=allocate_heap(memory,data.size());if(!block)status=8;else {memory.write(GuestAddress{block},data);runtime_.local_allocations.insert(block);memory.store(GuestAddress{call(4)},width,block);memory.store(GuestAddress{call(5)},32,data.size());}}
                    }else {
                        const auto capacity=load(call(5),32);memory.store(GuestAddress{call(5)},32,data.size());
                        if(!call(4) || capacity<data.size())status=234;else memory.write(GuestAddress{call(4)},data);
                    }
                }
                result=status;
            }
        }else if(symbol=="DnsQueryEx"){
            // (pQueryRequest, pQueryResults, pCancelHandle)：DNS_QUERY_REQUEST v1～v3（不支援自訂 DoH／UDP 伺服器清單），
            // 有完成常式時查詢仍在呼叫端執行緒完成，回 DNS_REQUEST_PENDING 並把 (pQueryContext, pQueryResults) 排入執行緒池。
            const auto request=call(0),results=call(1);
            if(!request || !results)result=invalid_parameter;
            else {
                const auto version=static_cast<std::uint32_t>(load(request,32));const auto name_pointer=load(request+unit,width);const auto type=static_cast<std::uint16_t>(load(request+2*unit,16));
                const auto options=load(request+(x86_?16:24),64);const auto server_list=load(request+(x86_?24:32),width);
                const auto routine=load(request+(x86_?32:48),width),context=load(request+(x86_?36:56),width);
                if(version<1 || version>3 || !name_pointer || load(results,32)!=1)result=invalid_parameter;
                else if(version>=2 && load(request+(x86_?48:72),32))result=50;
                else {
                    std::vector<std::vector<std::uint8_t>> servers;
                    if(server_list){
                        const auto count=static_cast<std::uint32_t>(load(server_list+4,32));if(count>64)throw Error{invalid_parameter};
                        for(std::uint32_t i=0;i<count;++i){
                            std::vector<std::uint8_t> sa(28);memory.read(GuestAddress{server_list+32+i*64},sa);const unsigned family=sa[0]|unsigned(sa[1])<<8;const std::uint16_t port=(sa[2]|sa[3])?static_cast<std::uint16_t>(sa[2]<<8|sa[3]):53;
                            if(family==2){sockaddr_in native{};
#if defined(__APPLE__)
                                native.sin_len=sizeof(native);
#endif
                                native.sin_family=AF_INET;native.sin_port=htons(port);std::memcpy(&native.sin_addr,sa.data()+4,4);servers.emplace_back(reinterpret_cast<const std::uint8_t*>(&native),reinterpret_cast<const std::uint8_t*>(&native)+sizeof(native));}
                            else if(family==23){sockaddr_in6 native{};
#if defined(__APPLE__)
                                native.sin6_len=sizeof(native);
#endif
                                native.sin6_family=AF_INET6;native.sin6_port=htons(port);std::memcpy(&native.sin6_addr,sa.data()+8,16);std::uint32_t scope=0;std::memcpy(&scope,sa.data()+24,4);native.sin6_scope_id=scope;servers.emplace_back(reinterpret_cast<const std::uint8_t*>(&native),reinterpret_cast<const std::uint8_t*>(&native)+sizeof(native));}
                            else throw Error{invalid_parameter};
                        }
                    }
                    if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行外部 DNS 查詢");
                    std::uint32_t status=0;std::uint64_t head=0;
                    try{const auto records=dns_detail::query(text(name_pointer,1),type,static_cast<std::uint32_t>(options),servers.empty()?nullptr:&servers);head=dns_store_records(state,memory,records,1);}
                    catch(const Error& error){status=error.code;}
                    memory.store(GuestAddress{results+4},32,status);memory.store(GuestAddress{results+8},64,options);memory.store(GuestAddress{results+16},width,head);
                    result=status;
                    if(routine){
                        if(const auto error=queue_pool_callback(state,memory,{routine,{context,results}})){
                            if(head)for(const auto at:chain(head)){runtime_.network.addresses.erase(at);(void)free_heap(memory,at,state);}
                            memory.store(GuestAddress{results+16},width,0);result=error;
                        }else result=9506;
                    }
                }
            }
        }else if(symbol.starts_with("DnsModifyRecordsInSet_") || symbol.starts_with("DnsReplaceRecordSet")){
            // DnsModifyRecordsInSet(pAddRecords, pDeleteRecords, Options, hContext, pServerList, pReserved)／DnsReplaceRecordSet(pNewSet, Options, hContext, pServerList, pReserved)：
            // RFC 2136 動態更新（dns_detail::update）。所有記錄須同名同型別；pServerList 為 IP4_ARRAY（連接埠 53），沒有時用系統 DNS 伺服器。
            // hContext 為 DnsAcquireContextHandle 的 handle；沒有 GSS-TSIG，DNS_UPDATE_SECURITY_ONLY 回 ERROR_NOT_SUPPORTED，其他安全選項以非安全更新進行。
            const bool replace=symbol.starts_with("DnsReplaceRecordSet");
            const unsigned charset=symbol.ends_with("W")?1U:symbol.ends_with("UTF8")?2U:3U;
            const auto add_head=call(0),delete_head=replace?0:call(1);const auto options=static_cast<std::uint32_t>(call(replace?1:2));
            const auto context=call(replace?2:3),server_list=call(replace?3:4),reserved=call(replace?4:5);
            if((!add_head && !delete_head) || reserved || (options&0xffff0000U))throw Error{invalid_parameter};
            if(options&0x100U)throw Error{50};
            if(context && (!dns_services_ || !dns_services_->contexts.contains(context)))throw Error{6};
            std::vector<Record> adds,deletes;
            for(const auto at:chain(add_head))adds.push_back(unpack(at,charset));
            for(const auto at:chain(delete_head))deletes.push_back(unpack(at,charset));
            const auto& sample=!adds.empty()?adds.front():deletes.front();
            for(const auto* set:{&adds,&deletes})for(const auto& record:*set)if(!record.type || record.name.empty() || record.type!=sample.type || !dns_detail::same_name(record.name,sample.name))throw Error{invalid_parameter};
            std::vector<std::vector<std::uint8_t>> servers;
            if(server_list){
                const auto count=static_cast<std::uint32_t>(load(server_list,32));if(!count || count>64)throw Error{invalid_parameter};
                for(std::uint32_t i=0;i<count;++i){sockaddr_in native{};
#if defined(__APPLE__)
                    native.sin_len=sizeof(native);
#endif
                    native.sin_family=AF_INET;native.sin_port=htons(53);const auto address=static_cast<std::uint32_t>(load(server_list+4+i*4,32));std::memcpy(&native.sin_addr,&address,4);
                    servers.emplace_back(reinterpret_cast<const std::uint8_t*>(&native),reinterpret_cast<const std::uint8_t*>(&native)+sizeof(native));}
            }
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行 DNS 動態更新");
            // 網路往返在宿主執行緒進行，呼叫執行緒以 Sleep(1) 等待，不佔住其他客體執行緒的服務（同一程序內的 DNS 伺服器也能回應）。
            // 宿主工作只在續行建立時啟動；等待後重新進入由同一續行取回結果，不會重送 UPDATE。
            return nested_continuation(state,memory,"DNS 動態更新",[this,service,adds,deletes,replace,servers](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall frame(s,m,service);
                auto pending=std::async(std::launch::async,[adds,deletes,replace,servers]()->std::uint32_t{
                    try{return dns_detail::update(adds,deletes,replace,servers.empty()?nullptr:&servers);}catch(const Error& error){return error.code;}catch(...){return 9002U;}});
                const auto sleep=resolve("kernel32.dll","Sleep").value;
                while(pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready)(void)co_await service_callback::call(sleep,{1});
                frame.return_to(s,pending.get());co_return std::optional<std::uint32_t>{};
            });
        }else if(symbol=="DnsServiceConstructInstance"){
            // DNS_SERVICE_INSTANCE 與其字串、位址及屬性陣列配置在同一區塊（與 DNS-SD 回呼共用 dnssd_instance_layout），由 DnsServiceFreeInstance 釋放。
            const auto count=static_cast<std::uint32_t>(call(7));
            if(count>4096 || (count && (!call(8) || !call(9)))){last_error_=invalid_parameter;result=0;}
            else {
                DnsServiceInstanceData data;const auto wide=[&](std::uint64_t pointer){return read_utf16(memory,GuestAddress{pointer},1024);};
                if(call(0))data.name=wide(call(0));if(call(1))data.host=wide(call(1));
                if(call(2)){std::array<std::uint8_t,4> ip{};memory.read(GuestAddress{call(2)},ip);data.ip4=ip;}
                if(call(3)){std::array<std::uint8_t,16> ip{};memory.read(GuestAddress{call(3)},ip);data.ip6=ip;}
                data.port=static_cast<std::uint16_t>(call(4));data.priority=static_cast<std::uint16_t>(call(5));data.weight=static_cast<std::uint16_t>(call(6));
                for(std::uint32_t i=0;i<count;++i){const auto key=load(call(8)+i*unit,width),value=load(call(9)+i*unit,width);
                    data.properties.push_back({key?std::optional{wide(key)}:std::nullopt,value?std::optional{wide(value)}:std::nullopt});}
                result=dns_store_instance(memory,data);if(!result)last_error_=8;
            }
        }else if(symbol=="DnsServiceFreeInstance"){
            if(call(0) && runtime_.network.addresses.erase(call(0)))(void)free_heap(memory,call(0),state);
        }else return std::nullopt;
    }catch(const Error& error){
        // 回傳 BOOL／指標的函式以 last error 報告；DNS_STATUS 函式直接回傳錯誤碼。
        const bool status=symbol.starts_with("DnsValidateName_") || symbol=="DnsQueryConfig" || symbol=="DnsQueryEx" || symbol.starts_with("DnsExtractRecordsFromMessage_") || symbol.starts_with("DnsModifyRecordsInSet_") || symbol.starts_with("DnsReplaceRecordSet");
        last_error_=error.code;result=status?error.code:0;
    }catch(const MemoryFault&){
        const bool status=symbol=="DnsQueryConfig" || symbol=="DnsQueryEx" || symbol.starts_with("DnsExtractRecordsFromMessage_") || symbol.starts_with("DnsModifyRecordsInSet_") || symbol.starts_with("DnsReplaceRecordSet");
        last_error_=invalid_parameter;result=status?invalid_parameter:0;
    }catch(const std::bad_alloc&){last_error_=8;result=symbol=="DnsQueryConfig" || symbol=="DnsQueryEx" || symbol.starts_with("DnsModifyRecordsInSet_") || symbol.starts_with("DnsReplaceRecordSet")?8:0;}
    call.return_to(state,result);return std::nullopt;
}
}
