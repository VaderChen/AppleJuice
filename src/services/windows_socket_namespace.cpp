#include "x64arm/windows_services.hpp"
#include "iphelper_legacy.hpp"
#include "x64arm/socket_namespace.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include "token_elevation.hpp"
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
    if(symbol=="WSCGetApplicationCategory" || symbol=="WSCSetApplicationCategory"){
        // (Path, PathLength, Extra, ExtraLength, Get: pPermittedLspCategories｜Set: PermittedLspCategories, pPrevPermLspCat, lpErrno)：不需要 WSAStartup。
        // 路徑與引數以字元數給定（可不含 NUL），不分大小寫比對；設定需要已提升的系統管理員（否則 WSANO_RECOVERY）；未分類的應用程式類別為 0。
        const bool setting=symbol=="WSCSetApplicationCategory";const auto error_at=call(setting?6:5);std::uint32_t error=0;
        try{
            const auto path_length=d(1),extra_length=d(3);
            if(!call(0) || !path_length || path_length>32767 || extra_length>32767 || (extra_length && !call(2)))throw Error{10022};
            const auto exact=[&](std::uint64_t at,std::uint32_t length){std::u16string out;for(std::uint32_t i=0;i<length;++i)out.push_back(static_cast<char16_t>(memory.load(GuestAddress{at+std::uint64_t{i}*2},16)));while(!out.empty() && !out.back())out.pop_back();return fold(out);};
            const auto path=exact(call(0),path_length),extra=extra_length?exact(call(2),extra_length):std::u16string{};if(path.empty())throw Error{10022};
            const auto same=[&](const ApplicationCategory& item){return item.path==path && item.extra==extra;};
            if(!registry_file_.empty() && !network.external_io)throw HostCallFault("比較模式不使用持久 Winsock 目錄");
            if(setting){
                const auto categories=d(4);if(categories&~0x800000ffU)throw Error{10022};
                initialize_tokens();if(!elevated_administrator(runtime_.token_objects.at(effective_token_id())))throw Error{11003};
                Transaction transaction(runtime_.registry,registry_file_);auto items=load_categories(runtime_.registry);
                const auto found=std::find_if(items.begin(),items.end(),same);const auto previous=found==items.end()?0U:found->categories;
                if(found!=items.end())found->categories=categories;else items.push_back({path,extra,categories});
                save_categories(runtime_.registry,items);transaction.commit();if(call(5))memory.store(GuestAddress{call(5)},32,previous);
            }else{
                if(!call(4))throw Error{10022};memory.check(GuestAddress{call(4)},4,Access::Write);
                Transaction transaction(runtime_.registry,registry_file_);const auto items=load_categories(runtime_.registry);
                const auto found=std::find_if(items.begin(),items.end(),same);memory.store(GuestAddress{call(4)},32,found==items.end()?0U:found->categories);
            }
        }catch(const Error& failure){error=failure.code;}catch(const MemoryFault&){error=10014;}catch(const std::bad_alloc&){error=8;}catch(const std::length_error&){error=10055;}catch(const std::invalid_argument&){error=11003;}
        if(error){try{if(error_at)memory.store(GuestAddress{error_at},32,error);}catch(const MemoryFault&){}}
        call.return_to(state,error?UINT32_MAX:0U);return std::nullopt;
    }
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
        else if(symbol.starts_with("WSAInstallServiceClass")){
            // WSASERVICECLASSINFO：lpServiceClassId、lpszServiceClassName、dwCount、lpClassInfos；同一個類別 ID 已安裝時 WSAEALREADY。
            const auto info=call(0);if(!info)throw Error{10022};const unsigned fixed=x86_?16:32,entry=x86_?20:32;memory.check(GuestAddress{info},fixed,Access::Read);
            const auto id_at=memory.load(GuestAddress{info},bits),name_at=memory.load(GuestAddress{info+unit},bits),list=memory.load(GuestAddress{info+3*unit},bits);
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{info+2*unit},32));if(!id_at || !name_at || count>256 || (count && !list))throw Error{10022};
            ServiceClass item;const auto id=read(id_at,16);std::copy(id.begin(),id.end(),item.id.begin());item.name=text(name_at);if(item.name.empty())throw Error{10022};
            for(std::uint32_t i=0;i<count;++i){
                const auto at=list+std::uint64_t{i}*entry;memory.check(GuestAddress{at},entry,Access::Read);const auto info_name=memory.load(GuestAddress{at},bits);if(!info_name)throw Error{10022};
                ClassInfo value;value.name=text(info_name);if(value.name.empty())throw Error{10022};value.namespace_id=static_cast<std::uint32_t>(memory.load(GuestAddress{at+unit},32));
                value.value_type=static_cast<std::uint32_t>(memory.load(GuestAddress{at+unit+4},32));const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{at+unit+8},32));
                const auto pointer=memory.load(GuestAddress{at+(x86_?16U:24U)},bits);if(size>65536)throw Error{10022};if(size && !pointer)throw Error{10014};if(size)value.value=read(pointer,size);
                item.infos.push_back(std::move(value));
            }
            if(!registry_file_.empty() && !network.external_io)throw HostCallFault("比較模式不寫入持久 Winsock 服務目錄");
            Transaction transaction(runtime_.registry,registry_file_);auto classes=load_classes(runtime_.registry);
            if(std::any_of(classes.begin(),classes.end(),[&](const auto& other){return other.id==item.id;}))throw Error{10037};
            classes.push_back(std::move(item));save_classes(runtime_.registry,classes);transaction.commit();
        }else if(symbol=="WSARemoveServiceClass"){
            if(!call(0))throw Error{10022};std::array<std::uint8_t,16> id{};const auto bytes=read(call(0),16);std::copy(bytes.begin(),bytes.end(),id.begin());
            if(!registry_file_.empty() && !network.external_io)throw HostCallFault("比較模式不寫入持久 Winsock 服務目錄");
            Transaction transaction(runtime_.registry,registry_file_);auto classes=load_classes(runtime_.registry);
            if(!std::erase_if(classes,[&](const auto& item){return item.id==id;}))throw Error{10109};save_classes(runtime_.registry,classes);transaction.commit();
        }else if(symbol.starts_with("WSAGetServiceClassInfo") || symbol.starts_with("WSAGetServiceClassNameByClassId")){
            // ClassInfo：(lpProviderId, lpServiceClassId, lpdwBufSize, lpServiceClassInfo)，只列該 provider 名稱空間與 NS_ALL 的結構描述；
            // NameByClassId：(lpServiceClassId, lpszServiceClassName, lpdwBufferLength)，長度以位元組計（含結尾 NUL）。緩衝區不足時 WSAEFAULT 並回報所需大小。
            const bool by_id=symbol.starts_with("WSAGetServiceClassNameByClassId");const auto class_at=call(by_id?0:1),size_at=call(by_id?2:2),buffer=call(by_id?1:3);
            std::uint32_t namespace_id=0;
            if(!by_id){if(!call(0))throw Error{10022};const auto provider=read(call(0),16);
                if(std::equal(provider.begin(),provider.end(),provider_id.begin()))namespace_id=local_namespace;else if(std::equal(provider.begin(),provider.end(),nla_provider_id.begin()))namespace_id=nla_namespace;else throw Error{10022};}
            if(!class_at)throw Error{10022};if(!size_at)throw Error{10014};memory.check(GuestAddress{size_at},4,Access::Write);
            std::array<std::uint8_t,16> id{};const auto bytes=read(class_at,16);std::copy(bytes.begin(),bytes.end(),id.begin());
            if(!registry_file_.empty() && !network.external_io)throw HostCallFault("比較模式不讀取持久 Winsock 服務目錄");
            std::vector<ServiceClass> classes;{Transaction transaction(runtime_.registry,registry_file_);classes=load_classes(runtime_.registry);}
            const auto found=std::find_if(classes.begin(),classes.end(),[&](const auto& item){return item.id==id;});if(found==classes.end())throw Error{10109};
            std::vector<std::uint8_t> output;
            if(by_id){if(wide)output=utf16_bytes(found->name);else {const auto narrow=utf16_to_utf8(found->name);output.assign(narrow.begin(),narrow.end());output.push_back(0);}}
            else output=class_information(*found,wide,x86_,buffer,namespace_id);
            const auto capacity=memory.load(GuestAddress{size_at},32);memory.store(GuestAddress{size_at},32,output.size());
            if(!buffer || capacity<output.size())throw Error{10014};if(x86_ && buffer>UINT32_MAX-output.size())throw Error{10014};
            memory.check(GuestAddress{buffer},output.size(),Access::Write);memory.write(GuestAddress{buffer},output);
        }
    }catch(const Error& error){last_error_=error.code;result=UINT32_MAX;}catch(const MemoryFault&){last_error_=10014;result=UINT32_MAX;}catch(const std::bad_alloc&){last_error_=8;result=UINT32_MAX;}catch(const std::length_error&){last_error_=8;result=UINT32_MAX;}catch(const std::invalid_argument&){last_error_=10022;result=UINT32_MAX;}
    call.return_to(state,result);return std::nullopt;
}
}
