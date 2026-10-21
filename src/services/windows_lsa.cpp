#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_sddl.hpp"
#include <algorithm>
#include <chrono>
#if !defined(_WIN32)
#include <unistd.h>
#endif
namespace x64arm {
std::span<const WindowsServiceDescriptor> lsa_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"secur32.dll","LsaConnectUntrusted",1},{"secur32.dll","LsaDeregisterLogonProcess",1},
        {"secur32.dll","LsaFreeReturnBuffer",1},{"secur32.dll","LsaLookupAuthenticationPackage",3},
        {"secur32.dll","LsaLogonUser",14},{"secur32.dll","LsaGetLogonSessionData",2},
        {"secur32.dll","LsaEnumerateLogonSessions",2}
    };return table;
}
namespace authentication {
std::uint32_t winerror(std::uint32_t status){
    switch(status){
    case 0:return 0;case 0xc0000008:return 6;case 0xc000000d:return 87;case 0xc0000017:return 8;
    case 0xc0000022:return 5;case 0xc000005e:return 1311;case 0xc000005f:return 1312;case 0xc0000061:return 1314;
    case 0xc0000064:return 1317;case 0xc000006a:return 1323;case 0xc000006d:return 1326;case 0xc000006e:return 1327;
    case 0xc000006f:return 1328;case 0xc0000070:return 1329;case 0xc0000071:return 1330;case 0xc0000072:return 1331;
    case 0xc00000a7:return 1348;case 0xc0000106:return 206;case 0xc00000bb:return 50;case 0xc00000fe:return 1364;case 0xc0000103:return 267;case 0xc0000193:return 1793;
    case 0xc0000224:return 1907;case 0xc0000234:return 1909;case 0xc000009a:return 1450;
    case 0xc0000005:return 998;default:return 317;
    }
}
}
namespace {
using namespace security;
constexpr std::u16string_view package=u"MICROSOFT_AUTHENTICATION_PACKAGE_V1_0";
struct Failure {std::uint32_t status;};
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));}
std::uint64_t now(){return 116444736000000000ULL+static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()/100);}
// 有長度的字串不可轉成宿主指標；驗證整個 MaximumLength 範圍。
std::u16string counted(GuestMemory& memory,std::uint64_t at,bool small,std::uint64_t base=0,std::uint64_t extent=0){
    const unsigned size=small?8:16,ptr=small?4:8;memory.check(GuestAddress{at},size,Access::Read);
    const auto length=memory.load(GuestAddress{at},16),maximum=memory.load(GuestAddress{at+2},16),buffer=memory.load(GuestAddress{at+ptr},small?32:64);
    if(length>maximum || (length&1) || (maximum&1) || (!buffer && maximum))throw Failure{0xc000000dU};
    if(base && maximum && (buffer<base || buffer-base>extent || maximum>extent-(buffer-base)))throw Failure{0xc000000dU};
    if(maximum)memory.check(GuestAddress{buffer},maximum,Access::Read);std::u16string out(static_cast<std::size_t>(length/2),0);
    for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<char16_t>(memory.load(GuestAddress{buffer+i*2},16));return out;
}
std::string ansi_counted(GuestMemory& memory,std::uint64_t at,bool small){
    const unsigned size=small?8:16,ptr=small?4:8;if(!at)throw Failure{0xc000000dU};memory.check(GuestAddress{at},size,Access::Read);
    const auto n=memory.load(GuestAddress{at},16),cap=memory.load(GuestAddress{at+2},16),p=memory.load(GuestAddress{at+ptr},small?32:64);
    if(n>cap || (!p && cap))throw Failure{0xc000000dU};if(cap)memory.check(GuestAddress{p},cap,Access::Read);
    std::string text;for(unsigned i=0;i<n;++i)text.push_back(static_cast<char>(memory.load(GuestAddress{p+i},8)));if(text.find('\0')!=std::string::npos)throw Failure{0xc000000dU};return text;
}
}
std::pair<std::uint64_t,std::uint32_t> WindowsServices::logon_token(std::uint32_t type,const authentication::Identity& identity,const std::array<std::uint8_t,8>& source,std::uint64_t source_id){
    initialize_tokens();collect_tokens();if(runtime_.token_handles.size()>=1024 || runtime_.lsa.sessions.size()>=1024)return {0,0xc000009aU};
    const auto [luid,error]=allocate_luid();if(error)return {0,error==8?0xc0000017U:0xc000009aU};
    VirtualToken token;bool current=false;
#if !defined(_WIN32)
    current=identity.uid==static_cast<std::uint32_t>(::geteuid());
#endif
    token.user.sid=current?runtime_.token_objects.at(1).user.sid:unix_sid(false,identity.uid);token.owner=token.user.sid;
    token.primary_group=current?runtime_.token_objects.at(1).primary_group:unix_sid(true,identity.gid);
    token.groups={{sddl_sid("S-1-1-0"),7},{sddl_sid("S-1-5-11"),7},{sddl_sid(type==3?"S-1-5-2":type==4?"S-1-5-3":type==5?"S-1-5-6":"S-1-5-4"),7}};
    if(current)token.groups.push_back({sddl_sid("S-1-5-32-545"),7});
    for(auto group:identity.groups)token.groups.push_back({unix_sid(true,group),7});
    token.groups.push_back({sddl_sid("S-1-5-5-"+std::to_string(luid>>32)+"-"+std::to_string(static_cast<std::uint32_t>(luid))),0xc0000007U});
    token.type=type==3?2:1;token.authentication_id=luid;token.source=source;token.source_id=source_id;
    token.default_dacl.resize(8);token.default_dacl[0]=2;const auto ace=encode_ace({0,0,0x10000000U,token.user.sid});token.default_dacl.insert(token.default_dacl.end(),ace.begin(),ace.end());put(token.default_dacl,2,token.default_dacl.size(),2);put(token.default_dacl,4,1,2);
    SecurityDescriptor descriptor;descriptor.control=4;descriptor.data[0]=token.owner;descriptor.data[1]=token.primary_group;descriptor.data[3]=token.default_dacl;
    if(!current){const auto creator=encode_ace({0,0,0xf01ff,runtime_.token_objects.at(1).user.sid});descriptor.data[3].insert(descriptor.data[3].end(),creator.begin(),creator.end());put(descriptor.data[3],2,descriptor.data[3].size(),2);put(descriptor.data[3],4,2,2);}
    token.object_descriptor=relative_descriptor(descriptor);
    const auto object=runtime_.next_token_object++,handle=runtime_.allocate_wait_handle();
    try{runtime_.token_objects.emplace(object,token);runtime_.token_handles.emplace(handle,0xf01ff);runtime_.token_handle_objects.emplace(handle,object);runtime_.handle_flags.emplace(handle,0);
        runtime_.lsa.sessions.emplace(luid,LsaSession{luid,now(),type,token.session,identity.name,u"X64ARM",identity.full_name,token.user.sid});
    }catch(...){runtime_.token_objects.erase(object);runtime_.token_handles.erase(handle);runtime_.token_handle_objects.erase(handle);runtime_.handle_flags.erase(handle);runtime_.lsa.sessions.erase(luid);throw;}
    return {handle,0};
}
std::optional<std::uint32_t> WindowsServices::invoke_lsa(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool logon32=name=="LogonUserA" || name=="LogonUserW";
    const unsigned unit=x86_?4:8,stride=x86_?8:16;const std::uint8_t bits=x86_?32:64;std::uint32_t status=0;
    const auto out=[&](std::uint64_t p,unsigned size){if(!p)throw Failure{0xc000000dU};memory.check(GuestAddress{p},size,Access::Write);};
    const auto connection=[&](){if(!runtime_.lsa.connections.contains(arg(0)))throw Failure{0xc0000008U};};
    const auto write=[&](std::uint64_t p,std::uint64_t value,std::uint8_t width=0){memory.store(GuestAddress{p},width?width:bits,value);};
    // 配置與 UNICODE_STRING 內指標一起完成；回傳區塊只能由 LsaFreeReturnBuffer 釋放。
    const auto buffer=[&](std::vector<std::uint8_t> bytes,const std::vector<std::pair<unsigned,std::u16string>>& strings){
        if(runtime_.lsa.buffers.size()>=4096)throw Failure{0xc000009aU};std::vector<std::pair<unsigned,std::size_t>> offsets;
        for(const auto& [at,text]:strings){if(text.size()>32766)throw Failure{0xc000000dU};if(text.empty())continue;
            offsets.emplace_back(at,bytes.size());put(bytes,at,text.size()*2,2);put(bytes,at+2,text.size()*2+2,2);const auto encoded=utf16_bytes(text);bytes.insert(bytes.end(),encoded.begin(),encoded.end());}
        const auto p=allocate_heap(memory,bytes.size());if(!p)throw Failure{0xc0000017U};
        try{for(const auto& [at,offset]:offsets)put(bytes,at+unit,p+offset,unit);memory.write(GuestAddress{p},bytes);runtime_.lsa.buffers.insert(p);}catch(...){(void)free_heap(memory,p,state);throw;}
        return std::pair{p,static_cast<std::uint32_t>(bytes.size())};
    };
    std::uint64_t owned_buffer=0,new_token=0;
    try {
        if(name=="LsaConnectUntrusted"){
            out(arg(0),unit);write(arg(0),0);if(runtime_.lsa.connections.size()>=1024)throw Failure{0xc000009aU};const auto h=runtime_.allocate_wait_handle();runtime_.lsa.connections.insert(h);write(arg(0),h);
        }else if(name=="LsaDeregisterLogonProcess"){
            connection();runtime_.lsa.connections.erase(arg(0));
        }else if(name=="LsaFreeReturnBuffer"){
            if(!runtime_.lsa.buffers.contains(arg(0)))throw Failure{0xc000000dU};if(!free_heap(memory,arg(0),state))throw Failure{0xc000000dU};runtime_.lsa.buffers.erase(arg(0));
        }else if(name=="LsaLookupAuthenticationPackage"){
            connection();out(arg(2),4);auto text=ansi_counted(memory,arg(1),x86_);if(text.size()>127)throw Failure{0xc0000106U};for(auto& c:text)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');if(text!=utf16_to_utf8(package))throw Failure{0xc00000feU};write(arg(2),0,32);
        }else if(name=="LsaEnumerateLogonSessions"){
            out(arg(0),4);out(arg(1),unit);write(arg(0),0,32);write(arg(1),0);collect_tokens();
            if(!runtime_.lsa.sessions.empty()){std::vector<std::uint8_t> bytes(runtime_.lsa.sessions.size()*8);std::size_t offset=0;for(const auto& [id,item]:runtime_.lsa.sessions){(void)item;put(bytes,offset,id,8);offset+=8;}
                const auto [p,n]=buffer(std::move(bytes),{});(void)n;owned_buffer=p;write(arg(0),runtime_.lsa.sessions.size(),32);write(arg(1),p);}
        }else if(name=="LsaGetLogonSessionData"){
            out(arg(1),unit);if(!arg(0))throw Failure{0xc000000dU};const auto id=memory.load(GuestAddress{arg(0)},64);write(arg(1),0);collect_tokens();
            const auto found=runtime_.lsa.sessions.find(id);if(found==runtime_.lsa.sessions.end())throw Failure{0xc000005fU};const auto& item=found->second;initialize_tokens();if(item.sid!=runtime_.token_objects.at(effective_token_id()).user.sid)throw Failure{0xc0000022U};
            // Windows XP 共同前綴；Size 讓新版客體辨識可用欄位，後續擴充不宣稱已提供。
            const unsigned length=x86_?80:136,user=x86_?12:16,kind=user+3*stride,sid=x86_?44:72,time=x86_?48:80,server=x86_?56:88;
            std::vector<std::uint8_t> bytes(length);put(bytes,0,length);put(bytes,4,id,8);put(bytes,kind,item.type);put(bytes,kind+4,item.session);put(bytes,time,item.time,8);
            const auto sid_offset=bytes.size();bytes.insert(bytes.end(),item.sid.begin(),item.sid.end());
            const auto [p,n]=buffer(std::move(bytes),{{user,item.user},{user+stride,item.domain},{user+2*stride,std::u16string(package)},{server,u"X64ARM"}});(void)n;owned_buffer=p;write(p+sid,p+sid_offset);write(arg(1),p);
        }else if(name=="LsaLogonUser" || logon32){
            authentication::Secret password;std::u16string user,domain;std::uint32_t type{};std::array<std::uint8_t,8> source{'L','o','g','o','n','U','s','r'};std::uint64_t source_id=0;
            if(logon32){
                if(!arg(0) || !arg(5) || static_cast<std::uint32_t>(arg(4))>3)throw Failure{0xc000000dU};out(arg(5),unit);write(arg(5),0);type=static_cast<std::uint32_t>(arg(3));
                if(type!=2 && type!=3 && type!=4 && type!=5 && type!=7 && type!=8 && type!=9)throw Failure{0xc000000dU};
                // NULL 密碼的內建服務帳號、網路保留密碼與 new-credentials 尚無原生對應。
                if(!arg(2) || type==8 || type==9)throw Failure{0xc00000bbU};
                const auto text=[&](std::uint64_t p){return name.ends_with('W')?read_utf16(memory,GuestAddress{p}):utf8_to_utf16_lossy(read_ansi(memory,p,32767));};
                user=text(arg(0));if(arg(1))domain=text(arg(1));
                if(name.ends_with('W'))password.value=read_utf16(memory,GuestAddress{arg(2)});else{authentication::ByteSecret raw;raw.value=read_ansi(memory,arg(2),131068);try{password.value=utf8_to_utf16(raw.value);}catch(const std::invalid_argument&){throw Failure{0xc000000dU};}}
            }else{
                connection();for(const auto index:{8U,11U})out(arg(index),unit);for(const auto index:{9U,13U})out(arg(index),4);out(arg(10),8);out(arg(12),x86_?32:48);
                if(!arg(1) || !arg(7))throw Failure{0xc000000dU};const auto origin=ansi_counted(memory,arg(1),x86_);(void)origin;
                memory.check(GuestAddress{arg(7)},16,Access::Read);for(unsigned i=0;i<8;++i)source[i]=static_cast<std::uint8_t>(memory.load(GuestAddress{arg(7)+i},8));source_id=memory.load(GuestAddress{arg(7)+8},64);
                write(arg(8),0);write(arg(9),0,32);write(arg(10),0,64);write(arg(11),0);write(arg(13),0,32);
                if(arg(6))throw Failure{0xc0000061U};if(static_cast<std::uint32_t>(arg(3))!=0)throw Failure{0xc00000feU};type=static_cast<std::uint32_t>(arg(2));
                if(type!=2 && type!=3 && type!=4 && type!=5 && type!=7)throw Failure{0xc000000dU};
                const auto base=arg(4);const auto extent=static_cast<std::uint32_t>(arg(5));const unsigned first=x86_?4:8,minimum=first+3*stride;
                if(!base || extent<minimum || extent>1048576 || base>UINT64_MAX-extent)throw Failure{0xc000000dU};memory.check(GuestAddress{base},extent,Access::Read);
                if(memory.load(GuestAddress{base},32)!=2)throw Failure{0xc00000a7U};domain=counted(memory,base+first,x86_,base,extent);user=counted(memory,base+first+stride,x86_,base,extent);password.value=counted(memory,base+first+2*stride,x86_,base,extent);
            }
            if(!authentication_backend_ && !runtime_.network.external_io)throw HostCallFault("比較模式不允許原生認證 I/O");
            auto backend=authentication_backend_?authentication_backend_:authentication::native_backend();const auto verified=backend->verify(user,domain,password.value);
            if(verified.status){if(!logon32)write(arg(13),verified.substatus,32);throw Failure{logon32 && verified.substatus?verified.substatus:verified.status};}
            if(!verified.identity || verified.identity->name.empty() || verified.identity->groups.size()>1024)throw Failure{0xc000006dU};
            const auto [handle,error]=logon_token(type,*verified.identity,source,source_id);if(error)throw Failure{error};new_token=handle;
            if(logon32){write(arg(5),handle);}else{
                const auto& session=runtime_.lsa.sessions.at(runtime_.token_objects.at(runtime_.token_handle_objects.at(handle)).authentication_id);
                std::vector<std::uint8_t> profile(x86_?112:160);put(profile,0,2);put(profile,4,1,2);put(profile,8,session.time,8);put(profile,16,INT64_MAX,8);put(profile,24,INT64_MAX,8);
                const auto [p,length]=buffer(std::move(profile),{{56+2*stride,verified.identity->full_name},{56+5*stride,u"X64ARM"}});owned_buffer=p;
                std::vector<std::uint8_t> quotas(x86_?32:48);put(quotas,3*unit,memory.limit(),unit);put(quotas,4*unit,memory.limit(),unit);put(quotas,x86_?24:40,UINT64_MAX,8);memory.write(GuestAddress{arg(12)},quotas);
                write(arg(8),p);write(arg(9),length,32);write(arg(10),session.id,64);write(arg(11),handle);
            }
        }else throw HostCallFault("未知的 LSA 服務");
    }catch(const Failure& error){status=error.status;}catch(const MemoryFault&){status=0xc0000005U;}catch(const std::bad_alloc&){status=0xc0000017U;}
    if(status){
        if(owned_buffer){runtime_.lsa.buffers.erase(owned_buffer);(void)free_heap(memory,owned_buffer,state);}
        if(new_token){runtime_.token_handles.erase(new_token);runtime_.token_handle_objects.erase(new_token);runtime_.handle_flags.erase(new_token);collect_tokens();}
    }
    if(logon32){if(status)last_error_=authentication::winerror(status);arg.return_to(state,status?0:1);}else arg.return_to(state,status);
    return {};
}
}
