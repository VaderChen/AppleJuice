#include "x64arm/windows_services.hpp"
#include "x64arm/sspi_digest.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
namespace digest=sspi_digest;
constexpr std::uint32_t no_memory=0x80090300,invalid_parameter=0x8009035d,small_buffer=0x80090321;
void require(bool value,std::uint32_t status=digest::invalid_token){if(!value)throw digest::Error{status};}
std::string lower(std::string value){for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return value;}
struct Buffer {std::uint64_t at{},pointer{};std::uint32_t size{},type{};};
std::vector<Buffer> buffers(GuestMemory& memory,std::uint64_t at,bool output=false){
    require(at);const bool small=memory.execution_mode()==ExecutionMode::X86;const unsigned unit=small?4U:8U,size=small?12U:16U;
    memory.check(GuestAddress{at},8+unit,Access::Read);require(memory.load(GuestAddress{at},32)==0);
    const auto count=memory.load(GuestAddress{at+4},32),array=memory.load(GuestAddress{at+8},static_cast<std::uint8_t>(unit*8));require(count && count<=64 && array);
    memory.check(GuestAddress{array},count*size,output?Access::Write:Access::Read);std::vector<Buffer> out;out.reserve(count);
    for(std::uint64_t i=0;i<count;++i){const auto item=array+i*size;out.push_back({item,memory.load(GuestAddress{item+8},static_cast<std::uint8_t>(unit*8)),static_cast<std::uint32_t>(memory.load(GuestAddress{item},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{item+4},32))});}
    return out;
}
std::string contents(const GuestMemory& memory,const Buffer& b,std::uint64_t limit=65536){
    require(b.size<=limit);std::string value(b.size,'\0');if(b.size){require(b.pointer);memory.read(GuestAddress{b.pointer},{reinterpret_cast<std::uint8_t*>(value.data()),value.size()});}
    while(!value.empty() && value.back()=='\0')value.pop_back();require(value.find('\0')==value.npos);return value;
}
Buffer token(const std::vector<Buffer>& values){
    const auto it=std::find_if(values.begin(),values.end(),[](const auto& b){return (b.type&0xfffffffU)==2;});require(it!=values.end());
    require(std::count_if(values.begin(),values.end(),[](const auto& b){return (b.type&0xfffffffU)==2;})==1);return *it;
}
std::uint64_t handle(const GuestMemory& memory,std::uint64_t at){
    if(!at)return 0;const unsigned unit=memory.execution_mode()==ExecutionMode::X86?4U:8U;
    memory.check(GuestAddress{at},unit*2,Access::Read);
    if(memory.load(GuestAddress{at+unit},static_cast<std::uint8_t>(unit*8))!=(unit==4?static_cast<std::uint32_t>(digest::cookie):digest::cookie))return 0;
    return memory.load(GuestAddress{at},static_cast<std::uint8_t>(unit*8));
}
void handle(GuestMemory& memory,std::uint64_t at,std::uint64_t id){
    const unsigned unit=memory.execution_mode()==ExecutionMode::X86?4U:8U;
    memory.store(GuestAddress{at},static_cast<std::uint8_t>(unit*8),id);memory.store(GuestAddress{at+unit},static_cast<std::uint8_t>(unit*8),digest::cookie);
}
struct Input {std::string token,method,entity,realm;};
Input read_input(GuestMemory& memory,const std::vector<Buffer>& values,bool server,bool complete=false){
    Input out;out.token=contents(memory,token(values));std::vector<Buffer> params;CryptoHash body("MD5",false,{});bool has_body=false;
    for(const auto& b:values){const auto type=b.type&0xfffffffU;
        if(type==3)params.push_back(b);
        else if(type==1 && complete){require(b.size<=WindowsServices::output_limit);std::vector<std::uint8_t> bytes(b.size);if(b.size)memory.read(GuestAddress{b.pointer},bytes);body.update(bytes);has_body=true;}
        else if(type==14 || type==16)throw digest::Error{0x80090346}; // 未驗證 channel binding 時不能降級成功。
        else if(type!=2 && type!=0 && !(complete && type==1))throw digest::Error{digest::invalid_token};
    }
    if(!params.empty())out.method=contents(memory,params[0]);
    const auto entity_index=server?2U:1U;if(params.size()>entity_index)out.entity=contents(memory,params[entity_index]);
    if(server && params.size()>3){const auto& b=params[3];require(!(b.size&1) && b.size<=8192);out.realm=utf16_to_utf8(read_utf16_counted(memory,GuestAddress{b.pointer},b.size/2));while(!out.realm.empty() && !out.realm.back())out.realm.pop_back();}
    if(has_body){constexpr char digits[]="0123456789abcdef";std::string result;for(auto byte:body.digest()){result+=digits[byte>>4];result+=digits[byte&15];}if(!out.entity.empty())require(digest::equal(lower(out.entity),result),digest::altered);out.entity=std::move(result);}
    return out;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_digest(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    namespace digest=sspi_digest;const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& all=runtime_.tls.digest;std::uint32_t result=0;
    std::uint32_t bad_memory=invalid_parameter;
    const unsigned unit=x86_?4U:8U;const auto text=[&](std::uint64_t at,bool wide){require(at,invalid_parameter);return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{at},4096)):utf16_to_utf8(cp1252_to_utf16(read_ansi(memory,at,4096)));};
    const auto output_sites=[&](std::uint64_t context,std::uint64_t attributes,std::uint64_t expiry){require(context,invalid_parameter);memory.check(GuestAddress{context},unit*2,Access::Write);if(attributes)memory.check(GuestAddress{attributes},4,Access::Write);if(expiry)memory.check(GuestAddress{expiry},8,Access::Write);};
    const auto emit_token=[&](const Buffer& b,std::string_view value,bool allocate){
        require(!(b.type&0xf0000000U),invalid_parameter);memory.check(GuestAddress{b.at},x86_?12U:16U,Access::Write);
        auto pointer=b.pointer;if(allocate){pointer=value.empty()?0:allocate_heap(memory,value.size());if(!value.empty() && !pointer)throw digest::Error{no_memory};if(pointer)runtime_.tls.buffers.insert(pointer);}
        else {if(value.size()>b.size){memory.store(GuestAddress{b.at},32,value.size());throw digest::Error{small_buffer};}if(!value.empty()){require(pointer);memory.check(GuestAddress{pointer},value.size(),Access::Write);}}
        if(!value.empty())memory.write(GuestAddress{pointer},{reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});
        memory.store(GuestAddress{b.at},32,value.size());memory.store(GuestAddress{b.at+4},32,2);memory.store(GuestAddress{b.at+8},static_cast<std::uint8_t>(unit*8),pointer);
    };
    const auto package_info=[&](bool wide,bool list,std::string_view name){
        struct Package {const char* name;const char* comment;std::uint32_t flags;};
        constexpr std::array<Package,2> packages{{{"Microsoft Unified Security Protocol Provider","TLS client via OpenSSL",0x472},{"WDigest","Digest authentication via shared crypto",0x14024}}};
        std::vector<Package> selected;if(list)selected.assign(packages.begin(),packages.end());else {const auto folded=lower(std::string(name));if(folded=="schannel" || folded=="microsoft unified security protocol provider")selected.push_back(packages[0]);else if(folded=="wdigest" || folded=="digest")selected.push_back(packages[1]);else throw digest::Error{0x80090305};}
        const unsigned size=x86_?20U:32U,offset=x86_?12U:16U;std::vector<std::uint8_t> bytes(size*selected.size());
        const auto put=[&](std::size_t at,unsigned extent,std::uint64_t value){for(unsigned i=0;i<extent;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        std::vector<std::pair<std::size_t,std::size_t>> strings;
        for(std::size_t i=0;i<selected.size();++i){
            const auto append=[&](std::string_view s){const auto begin=bytes.size();if(wide){const auto out=utf16_bytes(utf8_to_utf16(s));bytes.insert(bytes.end(),out.begin(),out.end());}else {bytes.insert(bytes.end(),s.begin(),s.end());bytes.push_back(0);}return begin;};
            const auto name_at=append(selected[i].name),comment_at=append(selected[i].comment);strings.emplace_back(name_at,comment_at);put(i*size,4,selected[i].flags);put(i*size+4,2,1);put(i*size+6,2,0xffff);put(i*size+8,4,65536);
        }
        const auto pointer=allocate_heap(memory,bytes.size());require(pointer,no_memory);
        for(std::size_t i=0;i<selected.size();++i){put(i*size+offset,unit,pointer+strings[i].first);put(i*size+offset+unit,unit,pointer+strings[i].second);}
        memory.write(GuestAddress{pointer},bytes);runtime_.tls.buffers.insert(pointer);return std::pair{pointer,static_cast<std::uint32_t>(selected.size())};
    };
    try{
        if(symbol.starts_with("QuerySecurityPackageInfo") || symbol.starts_with("EnumerateSecurityPackages")){
            const bool list=symbol.starts_with("Enumerate"),wide=symbol.ends_with('W');require(arg(0) && arg(1),invalid_parameter);
            memory.check(GuestAddress{arg(1)},unit,Access::Write);if(list)memory.check(GuestAddress{arg(0)},4,Access::Write);
            memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(unit*8),0);if(list)memory.store(GuestAddress{arg(0)},32,0);
            const auto [pointer,count]=package_info(wide,list,list?"":text(arg(0),wide));memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(unit*8),pointer);if(list)memory.store(GuestAddress{arg(0)},32,count);
        }else if(symbol.starts_with("AcquireCredentialsHandle")){
            const auto use=static_cast<std::uint32_t>(arg(2));require(use>=1 && use<=3 && !arg(3) && !arg(5) && !arg(6),digest::unsupported);
            output_sites(arg(7),0,arg(8));require(arg(4),digest::no_credentials);require(all.credentials.size()<1024 && all.next<=UINT32_MAX,no_memory);
            const auto at=arg(4);memory.check(GuestAddress{at},x86_?28U:48U,Access::Read);const auto flags=memory.load(GuestAddress{at+(x86_?24U:44U)},32);require(flags==1 || flags==2,invalid_parameter);
            const auto counted=[&](unsigned index){const unsigned offset=x86_?index*8:index*16;const auto pointer=memory.load(GuestAddress{at+offset},static_cast<std::uint8_t>(unit*8)),length=memory.load(GuestAddress{at+offset+unit},32);require(length<=4096 && (!length || pointer),invalid_parameter);
                if(!length)return std::string{};if(flags==2)return utf16_to_utf8(read_utf16_counted(memory,GuestAddress{pointer},length));std::string value(length,'\0');memory.read(GuestAddress{pointer},{reinterpret_cast<std::uint8_t*>(value.data()),value.size()});return utf16_to_utf8(cp1252_to_utf16(value));};
            digest::Credentials credentials;credentials.user=counted(0);credentials.domain=counted(1);require(!credentials.user.empty() && credentials.user.find('\0')==credentials.user.npos && credentials.domain.find('\0')==credentials.domain.npos,invalid_parameter);
            auto password=counted(2);require(password.find('\0')==password.npos,invalid_parameter);credentials.password=std::make_shared<digest::Secret>(std::move(password));credentials.use=use;
            if(arg(0))require(text(arg(0),symbol.ends_with('W'))==credentials.user,0x80090306);
            const auto id=all.next;all.credentials.emplace(id,std::move(credentials));++all.next;handle(memory,arg(7),id);if(arg(8))memory.store(GuestAddress{arg(8)},64,INT64_MAX);
        }else if(symbol=="FreeCredentialsHandle"){
            require(all.credentials.erase(handle(memory,arg(0))),digest::invalid_handle);
        }else if(symbol.starts_with("InitializeSecurityContext") || symbol=="AcceptSecurityContext"){
            const bool server=symbol=="AcceptSecurityContext";const auto flags=static_cast<std::uint32_t>(arg(3));
            const auto input_pointer=arg(server?2:6),output_pointer=arg(server?6:9),new_context=arg(server?5:8),attributes=arg(server?7:10),expiry=arg(server?8:11);
            const auto supported=0x1000090eU|(server?0x28000U:0x14000U);require(!(flags&~supported) && !arg(server?4:5),0x80090332);
            if(!server)require(!arg(4) && !arg(7),invalid_parameter);
            const bool http=(flags&0x10000000U)!=0;require(http || !(flags&(server?0x2000cU:0x1000cU)),digest::unsupported);
            output_sites(new_context,attributes,expiry);require(attributes,invalid_parameter);const auto out=token(buffers(memory,output_pointer,true));
            const auto id=handle(memory,arg(1));auto found=all.contexts.find(id);digest::Context context;bool creating=!arg(1);
            if(creating){require(all.contexts.size()<1024 && all.next<=UINT32_MAX,no_memory);const auto credential=all.credentials.find(handle(memory,arg(0)));require(credential!=all.credentials.end(),digest::invalid_handle);require((credential->second.use&(server?1U:2U))!=0,digest::no_credentials);context.credentials=credential->second;context.server=server;context.http=http;context.flags=flags;}
            else {require(found!=all.contexts.end(),digest::invalid_handle);context=found->second;require(context.server==server && context.http==http && (context.flags&~0x100U)==(flags&~0x100U),digest::out_of_sequence);}
            Input incoming;if(input_pointer)incoming=read_input(memory,buffers(memory,input_pointer),server);
            std::string random;if(context.phase==digest::Context::Phase::Initial || context.phase==digest::Context::Phase::Challenge){std::array<std::uint8_t,16> bytes{};require(random_bytes(bytes),no_memory);constexpr char digits[]="0123456789abcdef";for(auto c:bytes){random+=digits[c>>4];random+=digits[c&15];}}
            digest::Result reply;if(server)reply=digest::server(context,incoming.token,incoming.realm,incoming.method,incoming.entity,random);
            else reply=digest::client(context,incoming.token,text(arg(2),symbol.ends_with('W')),incoming.method,incoming.entity,random);
            const auto committed=creating?all.next:id;
            if(creating){
                const auto reserved=all.contexts.emplace(committed,digest::Context{}).first;
                try{emit_token(out,reply.token,(flags&0x100)!=0);}catch(...){all.contexts.erase(reserved);throw;}
                reserved->second=std::move(context);++all.next;
            }else {emit_token(out,reply.token,(flags&0x100)!=0);found->second=std::move(context);}
            handle(memory,new_context,committed);memory.store(GuestAddress{attributes},32,flags&~(server?0x8000U:0x4000U));if(expiry)memory.store(GuestAddress{expiry},64,INT64_MAX);result=reply.status;
        }else {
            bad_memory=digest::invalid_handle;const auto id=handle(memory,arg(0));const auto found=all.contexts.find(id);
            if(found==all.contexts.end()){
                if(symbol=="CompleteAuthToken" && arg(0)){
                    // 有效 Schannel context 沒有 Digest 的完成步驟；不能把它冒充 Digest。
                    const auto cookie_at=arg(0)+unit;if(memory.load(GuestAddress{cookie_at},static_cast<std::uint8_t>(unit*8))==(x86_?UINT64_C(0x4e4e454c):UINT64_C(0x534348414e4e454c)) && runtime_.tls.sessions.contains(memory.load(GuestAddress{arg(0)},static_cast<std::uint8_t>(unit*8))))throw digest::Error{digest::unsupported};
                }throw digest::Error{digest::invalid_handle};
            }
            if(symbol=="DeleteSecurityContext")all.contexts.erase(found);
            else if(symbol=="CompleteAuthToken"){
                bad_memory=digest::invalid_token;
                const auto values=buffers(memory,arg(1),true);const auto outgoing=token(values);auto incoming=read_input(memory,values,found->second.server,true);auto context=found->second;
                const auto reply=digest::complete(context,incoming.token,incoming.method,incoming.entity);emit_token(outgoing,reply.token,false);found->second=std::move(context);
            }else if(symbol.starts_with("QueryContextAttributes")){
                const auto kind=static_cast<std::uint32_t>(arg(1));const auto at=arg(2);require(at,invalid_parameter);const auto& context=found->second;const bool established=context.phase==digest::Context::Phase::Established;
                if(kind==10){memory.check(GuestAddress{at},unit,Access::Write);const auto [pointer,count]=package_info(symbol.ends_with('W'),false,"WDigest");(void)count;memory.store(GuestAddress{at},static_cast<std::uint8_t>(unit*8),pointer);}
                else {require(established,digest::out_of_sequence);
                    if(kind==0){memory.check(GuestAddress{at},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{at+i*4},32,i==0?65536:i==2?1:0);}
                    else if(kind==1){memory.check(GuestAddress{at},unit,Access::Write);std::vector<std::uint8_t> bytes;if(symbol.ends_with('W'))bytes=utf16_bytes(utf8_to_utf16(context.credentials.user));else {bool replaced=false;const auto ansi=utf16_to_cp1252(utf8_to_utf16(context.credentials.user),'?',replaced);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}const auto pointer=allocate_heap(memory,bytes.size());require(pointer,no_memory);memory.write(GuestAddress{pointer},bytes);runtime_.tls.buffers.insert(pointer);memory.store(GuestAddress{at},static_cast<std::uint8_t>(unit*8),pointer);}
                    else if(kind==2){memory.check(GuestAddress{at},16,Access::Write);memory.store(GuestAddress{at},64,0);memory.store(GuestAddress{at+8},64,INT64_MAX);}
                    else if(kind==14){memory.check(GuestAddress{at},4,Access::Write);memory.store(GuestAddress{at},32,context.flags&~(context.server?0x8000U:0x4000U));}
                    else throw digest::Error{digest::unsupported};
                }
            }else throw digest::Error{digest::unsupported};
        }
    }catch(const digest::Error& error){result=error.status;}catch(const MemoryFault&){result=bad_memory;}catch(const std::bad_alloc&){result=no_memory;}catch(const std::invalid_argument&){result=digest::invalid_token;}catch(const std::runtime_error&){result=0x80090304;}
    arg.return_to(state,result);return {};
}
}
