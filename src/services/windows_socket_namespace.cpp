#include "x64arm/windows_services.hpp"
#include "iphelper_legacy.hpp"
#include "x64arm/socket_namespace.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
namespace {
using namespace socket_namespace;
struct Error{std::uint32_t code;};
void require(std::uint32_t error){if(error)throw Error{error};}
std::uint32_t registry_error(std::uint32_t code){return code==5?10013:code==8?8:code?10107:0;}
struct Transaction {
    RegistryState& state;RegistryTransaction native;RegistryState previous;bool committed{};
    Transaction(RegistryState& s,const std::filesystem::path& path):state(s),native(s,path),previous(s){require(registry_error(native.error));}
    ~Transaction(){if(!committed)state=std::move(previous);}
    void commit(){require(registry_error(native.commit()));committed=true;}
};
constexpr std::uint32_t query_flags=0xbfff; // 回傳欄位、階層、清快取及服務地址；未安裝外部 provider 不虛構結果。
}
std::optional<std::uint32_t> WindowsServices::invoke_socket_namespace(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& descriptor){
    const WindowsCall call(state,memory,descriptor);const auto symbol=descriptor.symbol;auto& network=runtime_.network;const bool wide=symbol.ends_with('W');const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;std::uint64_t result=0;
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(call(n));};
    const auto text=[&](std::uint64_t pointer){return !pointer?std::u16string{}:wide?read_utf16(memory,GuestAddress{pointer},4096):utf8_to_utf16(read_ansi(memory,pointer,16384));};
    const auto read=[&](std::uint64_t pointer,unsigned size){std::vector<std::uint8_t> data(size);memory.read(GuestAddress{pointer},data);return data;};
    const auto parse=[&](std::uint64_t base,bool setting,Record& record,std::vector<std::pair<std::uint32_t,std::uint32_t>>& protocols){
        if(!base)throw Error{10014};memory.check(GuestAddress{base},x86_?60:120,Access::Read);if(memory.load(GuestAddress{base},32)!=(x86_?60:120))throw Error{10022};
        const auto value=[&](unsigned field,bool pointer=true){return memory.load(GuestAddress{base+field*unit},pointer?bits:32);};
        if(!setting && std::getenv("X64ARM_TRACE_NETWORK_ERRORS"))std::fprintf(stderr,"[namespace_query] pid=%u namespace=%u flags=%x service_class=%llx provider=%llx\n",process_id(),static_cast<std::uint32_t>(value(5,false)),d(1),static_cast<unsigned long long>(value(2)),static_cast<unsigned long long>(value(6)));
        // 指定 provider 時忽略 namespace；未指定則 NS_ALL 或本機 TCP/IP。
        if(value(6)){const auto provider=read(value(6),16);if(provider==std::vector<std::uint8_t>(nla_provider_id.begin(),nla_provider_id.end()))record.namespace_id=nla_namespace;else if(provider!=std::vector<std::uint8_t>(provider_id.begin(),provider_id.end()))throw Error{10022};}
        else if(value(5,false)==nla_namespace)record.namespace_id=nla_namespace;
        else if(value(5,false)!=0 && value(5,false)!=local_namespace)throw Error{10045};
        if(value(2)){const auto id=read(value(2),16);std::copy(id.begin(),id.end(),record.service_class.begin());if(!value(6) && value(5,false)==0 && record.service_class==nla_class_id)record.namespace_id=nla_namespace;}
        else if(!setting && record.namespace_id==nla_namespace)record.service_class=nla_class_id;
        else throw Error{10022};
        if(record.namespace_id==nla_namespace){if(setting)throw Error{10045};if(record.service_class!=nla_class_id)throw Error{10108};if(d(1)&2)throw Error{11004};}
        record.name=text(value(1));record.context=text(value(7));if(setting && record.name.empty())throw Error{10022};
        if(value(3)){record.version=std::array{static_cast<std::uint32_t>(memory.load(GuestAddress{value(3)},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{value(3)+4},32))};if((*record.version)[1]>1)throw Error{10022};}
        if(setting)record.comment=text(value(4));
        if(value(14)){const auto blob=value(14);memory.check(GuestAddress{blob},2*unit,Access::Read);const auto size=memory.load(GuestAddress{blob},32),pointer=memory.load(GuestAddress{blob+unit},bits);if(size>1024*1024)throw Error{10022};if(size && !pointer)throw Error{10014};record.blob=size?read(pointer,static_cast<unsigned>(size)):std::vector<std::uint8_t>{};}
        if(setting){const auto count=value(11,false),array=value(12);if(count>256)throw Error{10022};if(count && !array)throw Error{10014};const unsigned fixed=x86_?24:40;if(count)memory.check(GuestAddress{array},static_cast<std::size_t>(count*fixed),Access::Read);
            for(std::uint64_t i=0;i<count;++i){const auto at=array+i*fixed;Address address;for(unsigned part=0;part<2;++part){const auto pointer=memory.load(GuestAddress{at+part*2*unit},bits),size=memory.load(GuestAddress{at+part*2*unit+unit},32);if(size>128 || (size && !pointer))throw Error{10014};auto data=size?read(pointer,static_cast<unsigned>(size)):std::vector<std::uint8_t>{};if(!data.empty()){if(data.size()<2)throw Error{10022};const auto family=data[0]|static_cast<unsigned>(data[1])<<8;if(family!=2 && family!=23)throw Error{10047};if(data.size()!=(family==2?16U:28U))throw Error{10022};}if(part)address.remote=std::move(data);else address.local=std::move(data);}
                address.type=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4*unit},32));address.protocol=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4*unit+4},32));if((address.local.empty() && address.remote.empty()) || address.type<1 || address.type>5 || address.protocol>INT32_MAX || (!address.local.empty() && !address.remote.empty() && address.local[0]!=address.remote[0]))throw Error{10022};record.addresses.push_back(std::move(address));}
        }else{const auto count=value(8,false),array=value(9);if(count>64)throw Error{10022};if(count && !array)throw Error{10014};for(std::uint64_t i=0;i<count;++i)protocols.emplace_back(static_cast<std::uint32_t>(memory.load(GuestAddress{array+8*i},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{array+8*i+4},32)));if(value(10) && !text(value(10)).empty())throw Error{10045};}
    };
    try{
        if(!network.startups)throw Error{10093};
        if(symbol=="WSAEnumNameSpaceProvidersA" || symbol=="WSAEnumNameSpaceProvidersW"){
            if(!call(0))throw Error{10014};const auto capacity=memory.load(GuestAddress{call(0)},32);memory.check(GuestAddress{call(0)},4,Access::Write);const auto bytes=providers(wide,x86_,call(1));
            if(!call(1) || capacity<bytes.size()){memory.store(GuestAddress{call(0)},32,bytes.size());throw Error{10014};}memory.check(GuestAddress{call(1)},bytes.size(),Access::Write);memory.write(GuestAddress{call(1)},bytes);result=2;
        }else if(symbol=="WSASetServiceA" || symbol=="WSASetServiceW"){
            if(d(1)>2 || (d(2)&~1U))throw Error{10022};Record request;std::vector<std::pair<std::uint32_t,std::uint32_t>> ignored;parse(call(0),true,request,ignored);
            if(!registry_file_.empty() && !network.external_io)throw HostCallFault("比較模式不寫入持久 Winsock 服務目錄");
            Transaction transaction(runtime_.registry,registry_file_);auto records=load(runtime_.registry);require(change(records,std::move(request),d(1),(d(2)&1)!=0));save(runtime_.registry,records);transaction.commit();
        }else if(symbol.starts_with("WSALookupServiceBegin")){
            if(!call(2))throw Error{10014};memory.check(GuestAddress{call(2)},unit,Access::Write);if(d(1)&~query_flags)throw Error{10022};Record query;std::vector<std::pair<std::uint32_t,std::uint32_t>> protocols;parse(call(0),false,query,protocols);
            std::vector<Record> records;
            if(query.namespace_id==nla_namespace){if(!network.external_io)throw HostCallFault("比較模式不查詢宿主網路位置");if(!snapshot_network_configuration(*network.configuration))throw Error{10091};if(!network.configuration->descriptions_loaded){iphelper::capture_interface_descriptions(*network.configuration);network.configuration->descriptions_loaded=true;}records=nla_records(*network.configuration);}
            else {if(!registry_file_.empty() && !network.external_io)throw HostCallFault("比較模式不讀取持久 Winsock 服務目錄");Transaction transaction(runtime_.registry,registry_file_);records=load(runtime_.registry);}
            Lookup lookup;lookup.flags=d(1);
            for(auto record:records){if(!record.registered || record.service_class!=query.service_class || (!query.name.empty() && fold(record.name)!=fold(query.name)) || (query.blob && record.blob!=query.blob) || (d(1)&2))continue;
                const auto context=fold(record.context),asked=fold(query.context);if(context!=asked && (!(d(1)&1) || (!asked.empty() && !context.starts_with(asked+u"/"))))continue;
                if(query.version && (!record.version || ((*query.version)[1]?(*record.version)[0]<(*query.version)[0]:(*record.version)[0]!=(*query.version)[0])))continue;
                if(!protocols.empty() && query.namespace_id!=nla_namespace){std::erase_if(record.addresses,[&](const auto& a){const auto& addr=a.remote.empty()?a.local:a.remote;const auto family=addr[0]|static_cast<unsigned>(addr[1])<<8;return std::none_of(protocols.begin(),protocols.end(),[&](const auto& p){return (!p.first || p.first==family) && (!p.second || p.second==a.protocol);});});if(record.addresses.empty())continue;}lookup.records.push_back(std::move(record));
            }if(lookup.records.empty() && query.namespace_id!=nla_namespace)throw Error{10108};if(network.namespace_lookups.size()>=1024)throw Error{8};std::sort(lookup.records.begin(),lookup.records.end(),[](const auto& a,const auto& b){return std::tie(a.context,a.name)<std::tie(b.context,b.name);});const auto handle=network.next_lookup;network.namespace_lookups.emplace(handle,std::move(lookup));++network.next_lookup;memory.store(GuestAddress{call(2)},bits,handle);
        }else if(symbol.starts_with("WSALookupServiceNext")){
            const auto found=network.namespace_lookups.find(call(0));if(found==network.namespace_lookups.end())throw Error{6};if(d(1)&~query_flags)throw Error{10022};if(!call(2))throw Error{10014};const auto capacity=memory.load(GuestAddress{call(2)},32);memory.check(GuestAddress{call(2)},4,Access::Write);auto& lookup=found->second;
            if((d(1)&0x1000) && lookup.too_small){++lookup.cursor;lookup.too_small=false;}if(lookup.cursor>=lookup.records.size())throw Error{10110};if(d(1)&2)throw Error{10110};
            const auto flags=(lookup.flags&d(1)&0xff0U)|((lookup.flags|d(1))&0x8000U);const auto bytes=information(lookup.records[lookup.cursor],wide,x86_,call(3),flags);
            if(!call(3) || capacity<bytes.size()){memory.store(GuestAddress{call(2)},32,bytes.size());lookup.too_small=true;throw Error{10014};}if(x86_ && call(3)>UINT32_MAX-bytes.size())throw Error{10014};memory.check(GuestAddress{call(3)},bytes.size(),Access::Write);memory.write(GuestAddress{call(3)},bytes);++lookup.cursor;lookup.too_small=false;
        }else if(symbol=="WSALookupServiceEnd"){if(!network.namespace_lookups.erase(call(0)))throw Error{6};}
    }catch(const Error& error){last_error_=error.code;result=UINT32_MAX;}catch(const MemoryFault&){last_error_=10014;result=UINT32_MAX;}catch(const std::bad_alloc&){last_error_=8;result=UINT32_MAX;}catch(const std::length_error&){last_error_=8;result=UINT32_MAX;}catch(const std::invalid_argument&){last_error_=10022;result=UINT32_MAX;}
    call.return_to(state,result);return std::nullopt;
}
}
