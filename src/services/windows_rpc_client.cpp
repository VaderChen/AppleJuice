#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include "windows_sddl.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t rpc_ok=0,rpc_invalid_parameter=87,rpc_no_memory=14,rpc_invalid_string_binding=1700,rpc_wrong_kind=1715,rpc_invalid_binding=1702;
constexpr std::uint32_t rpc_protseq_not_supported=1703,rpc_invalid_string_uuid=1705,rpc_server_unavailable=1722,rpc_no_call_active=1725;
constexpr std::uint32_t rpc_unknown_authn_service=1747,rpc_unknown_authn_level=1748,ept_not_registered=1753,rpc_null_context=1775;
// winerror 的 RPC_S_WRONG_KIND_OF_BINDING（1701）、RPC_S_INVALID_RPC_PROTSEQ（1704）、RPC_S_INVALID_TIMEOUT（1709）、
// RPC_S_NOT_LISTENING（1715）、RPC_S_BINDING_HAS_NO_AUTH（1746）。
constexpr std::uint32_t rpc_wrong_kind_of_binding=1701,rpc_invalid_protseq=1704,rpc_invalid_timeout=1709,rpc_not_listening=1715,rpc_no_auth=1746;
constexpr std::u16string_view rpc_protocols[]{u"ncalrpc",u"ncacn_np",u"ncacn_ip_tcp",u"ncacn_http",u"ncadg_ip_udp",u"ncacn_hvsocket"};
std::u16string fold(std::u16string_view text){std::u16string lower(text);for(auto& c:lower)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return lower;}
bool uuid_text(std::u16string_view text){
    if(text.size()!=36)return false;
    for(std::size_t i=0;i<36;++i){const auto c=text[i];if(i==8 || i==13 || i==18 || i==23){if(c!=u'-')return false;continue;}
        if(!((c>=u'0' && c<=u'9') || (c>=u'a' && c<=u'f') || (c>=u'A' && c<=u'F')))return false;}
    return true;
}
// 字串 binding：[物件 UUID@]協定序列:[網路位址][[端點][,選項]]。
bool parse_binding(std::u16string_view text,RpcBinding& out,std::uint32_t& error){
    error=rpc_invalid_string_binding;
    std::size_t at=0;
    if(const auto mark=text.find(u'@');mark!=text.npos && mark<text.find(u':')){out.object=std::u16string(text.substr(0,mark));at=mark+1;
        if(!uuid_text(out.object)){error=rpc_invalid_string_uuid;return false;}}
    const auto colon=text.find(u':',at);if(colon==text.npos || colon==at)return false;
    out.protocol=std::u16string(text.substr(at,colon-at));at=colon+1;
    const auto open=text.find(u'[',at);
    out.address=std::u16string(text.substr(at,open==text.npos?text.npos:open-at));
    if(open!=text.npos){
        const auto close=text.find(u']',open);if(close==text.npos || close+1!=text.size())return false;
        const auto inside=text.substr(open+1,close-open-1);const auto comma=inside.find(u',');
        out.endpoint=std::u16string(inside.substr(0,comma));if(comma!=inside.npos)out.options=std::u16string(inside.substr(comma+1));
    }
    const auto protocol=fold(out.protocol);
    if(std::find(std::begin(rpc_protocols),std::end(rpc_protocols),protocol)==std::end(rpc_protocols)){error=rpc_protseq_not_supported;return false;}
    error=rpc_ok;return true;
}
}
std::span<const WindowsServiceDescriptor> rpc_client_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"rpcrt4.dll","RpcStringBindingComposeW",6},{"rpcrt4.dll","RpcStringBindingComposeA",6},
        {"rpcrt4.dll","RpcStringBindingParseW",6},{"rpcrt4.dll","RpcStringBindingParseA",6},
        {"rpcrt4.dll","RpcBindingFromStringBindingW",2},{"rpcrt4.dll","RpcBindingFromStringBindingA",2},
        {"rpcrt4.dll","RpcBindingSetAuthInfoExW",7},{"rpcrt4.dll","RpcBindingSetAuthInfoExA",7},
        {"rpcrt4.dll","RpcBindingSetAuthInfoW",6},{"rpcrt4.dll","RpcBindingSetAuthInfoA",6},
        {"rpcrt4.dll","RpcBindingFree",1},{"rpcrt4.dll","I_RpcExceptionFilter",1},
        {"rpcrt4.dll","RpcServerInqCallAttributesW",2},{"rpcrt4.dll","RpcServerInqCallAttributesA",2},
        {"rpcrt4.dll","NdrClientCall3",3,0,0,0,true},{"rpcrt4.dll","NdrClientCall2",2,0,0,0,true},
        {"rpcrt4.dll","UuidCreateNil",1},{"rpcrt4.dll","UuidHash",2},{"rpcrt4.dll","DceErrorInqTextA",2},{"rpcrt4.dll","DceErrorInqTextW",2},
        {"rpcrt4.dll","I_RpcAllocate",1},{"rpcrt4.dll","I_RpcFree",1},{"rpcrt4.dll","I_RpcMapWin32Status",1},
        {"rpcrt4.dll","RpcBindingCopy",2},{"rpcrt4.dll","RpcBindingToStringBindingA",2},{"rpcrt4.dll","RpcBindingToStringBindingW",2},
        {"rpcrt4.dll","RpcBindingInqObject",2},{"rpcrt4.dll","RpcBindingSetObject",2},{"rpcrt4.dll","RpcBindingReset",1},{"rpcrt4.dll","RpcBindingVectorFree",1},
        {"rpcrt4.dll","RpcNetworkIsProtseqValidA",1},{"rpcrt4.dll","RpcNetworkIsProtseqValidW",1},{"rpcrt4.dll","RpcNetworkInqProtseqsA",1},{"rpcrt4.dll","RpcNetworkInqProtseqsW",1},
        {"rpcrt4.dll","RpcProtseqVectorFreeA",1},{"rpcrt4.dll","RpcProtseqVectorFreeW",1},{"rpcrt4.dll","RpcIfInqId",2},{"rpcrt4.dll","RpcRaiseException",1},
        {"rpcrt4.dll","RpcMgmtSetComTimeout",2},{"rpcrt4.dll","RpcBindingSetOption",3},
        {"rpcrt4.dll","RpcBindingInqAuthInfoA",6},{"rpcrt4.dll","RpcBindingInqAuthInfoW",6},{"rpcrt4.dll","RpcBindingInqAuthInfoExA",8},{"rpcrt4.dll","RpcBindingInqAuthInfoExW",8},
        {"rpcrt4.dll","RpcImpersonateClient",1},{"rpcrt4.dll","RpcRevertToSelf",0},{"rpcrt4.dll","RpcRevertToSelfEx",1},
        {"rpcrt4.dll","RpcBindingInqAuthClientA",6},{"rpcrt4.dll","RpcBindingInqAuthClientW",6},{"rpcrt4.dll","RpcBindingInqAuthClientExA",7},{"rpcrt4.dll","RpcBindingInqAuthClientExW",7},
        {"rpcrt4.dll","RpcBindingServerFromClient",2},{"rpcrt4.dll","RpcMgmtIsServerListening",1},{"rpcrt4.dll","RpcMgmtWaitServerListen",0},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_rpc_client(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name.starts_with("NdrClientCall"))return invoke_ndr_client_call(state,memory,service);
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const bool wide=name.ends_with('W');
    const auto read=[&](std::uint64_t address)->std::u16string{
        if(!address)return {};
        if(wide)return read_utf16(memory,GuestAddress{address},32767);
        std::u16string text;for(const auto c:read_ansi(memory,address,32767))text.push_back(static_cast<unsigned char>(c));return text;
    };
    // RpcStringFree 釋放的字串：與 UuidToString 相同由客體 heap 配置，含結尾 NUL。
    const auto allocate_text=[&](std::u16string_view text)->std::uint64_t{
        const auto size=(text.size()+1)*(wide?2:1);const auto pointer=allocate_heap(memory,size);if(!pointer)return 0;
        for(std::size_t i=0;i<text.size();++i){if(wide)memory.store(GuestAddress{pointer+i*2},16,text[i]);else memory.store(GuestAddress{pointer+i},8,text[i]&0xffU);}
        if(wide)memory.store(GuestAddress{pointer+text.size()*2},16,0);else memory.store(GuestAddress{pointer+text.size()},8,0);
        return pointer;
    };
    try{
        if(name=="I_RpcExceptionFilter"){
            // 嚴重的處理器例外繼續往外搜尋（EXCEPTION_CONTINUE_SEARCH），其餘由 RpcExcept 處理（EXCEPTION_EXECUTE_HANDLER）。
            switch(static_cast<std::uint32_t>(arg(0))){
            case 0x80000002U:case 0x80000003U:case 0xc0000005U:case 0xc000001dU:case 0xc0000096U:case 0xc00000aaU:case 0xc00000fdU:case 0xc0000194U:return done(0);
            default:return done(1);
            }
        }
        if(name.starts_with("RpcStringBindingCompose")){
            const auto object=read(arg(0)),protocol=read(arg(1)),address=read(arg(2)),endpoint=read(arg(3)),options=read(arg(4));
            std::u16string text;if(!object.empty()){text+=object;text+=u'@';}
            if(!protocol.empty()){text+=protocol;text+=u':';}
            text+=address;
            if(!endpoint.empty() || !options.empty()){text+=u'[';text+=endpoint;if(!options.empty()){text+=u',';text+=options;}text+=u']';}
            if(!arg(5))return done(rpc_ok);
            const auto pointer=allocate_text(text);if(!pointer)return done(rpc_no_memory);
            memory.store(GuestAddress{arg(5)},bits,pointer);return done(rpc_ok);
        }
        if(name.starts_with("RpcStringBindingParse")){
            if(!arg(0))return done(rpc_invalid_string_binding);
            RpcBinding parsed;std::uint32_t error=0;
            if(!parse_binding(read(arg(0)),parsed,error) && error!=rpc_protseq_not_supported)return done(error);
            const std::array<std::pair<std::uint64_t,const std::u16string*>,5> outputs{{{arg(1),&parsed.object},{arg(2),&parsed.protocol},{arg(3),&parsed.address},{arg(4),&parsed.endpoint},{arg(5),&parsed.options}}};
            std::vector<std::uint64_t> allocated;
            for(const auto& [output,text]:outputs){if(!output)continue;const auto pointer=allocate_text(*text);
                if(!pointer){for(const auto value:allocated)(void)free_heap(memory,value,state);return done(rpc_no_memory);}allocated.push_back(pointer);memory.store(GuestAddress{output},bits,pointer);}
            return done(rpc_ok);
        }
        if(name.starts_with("RpcBindingFromStringBinding")){
            if(!arg(1))return done(rpc_invalid_parameter);memory.store(GuestAddress{arg(1)},bits,0);
            if(!arg(0))return done(rpc_invalid_string_binding);
            RpcBinding binding;std::uint32_t error=0;if(!parse_binding(read(arg(0)),binding,error))return done(error);
            const auto handle=allocate_heap(memory,16);if(!handle)return done(rpc_no_memory);
            runtime_.rpc_bindings.emplace(handle,std::move(binding));memory.store(GuestAddress{arg(1)},bits,handle);return done(rpc_ok);
        }
        if(name.starts_with("RpcBindingSetAuthInfo")){
            const auto found=runtime_.rpc_bindings.find(arg(0));if(found==runtime_.rpc_bindings.end())return done(rpc_invalid_binding);
            const auto level=static_cast<std::uint32_t>(arg(2)),authn=static_cast<std::uint32_t>(arg(3)),authz=static_cast<std::uint32_t>(arg(5));
            if(level>6)return done(rpc_unknown_authn_level);
            constexpr std::uint32_t services[]{0,9,10,14,16,0xffffffffU};
            if(std::find(std::begin(services),std::end(services),authn)==std::end(services))return done(rpc_unknown_authn_service);
            found->second.principal=read(arg(1));found->second.authn_level=level;found->second.authn_service=authn;found->second.authz_service=authz;
            found->second.identity=arg(4);found->second.authenticated=authn!=0;found->second.qos_set=false;
            // Ex 版本的 RPC_SECURITY_QOS｛Version, Capabilities, IdentityTracking, ImpersonationType｝保存供 RpcBindingInqAuthInfoEx 回報。
            if(name.starts_with("RpcBindingSetAuthInfoEx") && arg(6)){for(unsigned i=0;i<4;++i)found->second.qos[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(6)+i*4},32));found->second.qos_set=true;}
            return done(rpc_ok);
        }
        const auto binding_text=[&](const RpcBinding& binding){
            // 與 RpcStringBindingCompose 相同的組字規則：[物件 UUID@]協定序列:位址[端點,選項]。
            std::u16string text;if(!binding.object.empty()){text+=binding.object;text+=u'@';}
            if(!binding.protocol.empty()){text+=binding.protocol;text+=u':';}
            text+=binding.address;
            if(!binding.endpoint.empty() || !binding.options.empty()){text+=u'[';text+=binding.endpoint;if(!binding.options.empty()){text+=u',';text+=binding.options;}text+=u']';}
            return text;
        };
        if(name=="UuidCreateNil"){if(!arg(0))return done(rpc_invalid_parameter);memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(16));return done(rpc_ok);}
        if(name=="UuidHash"){
            // DCE uuid_hash：逐位元組累加兩個 mod 255 的和，NULL 視同 nil UUID。
            std::array<std::uint8_t,16> uuid{};if(arg(0))memory.read(GuestAddress{arg(0)},std::span(uuid));
            std::int32_t c0=0,c1=0;for(const auto byte:uuid){c0+=byte;c1+=c0;}
            std::int32_t x=-c1%255;if(x<0)x+=255;std::int32_t y=(c1-c0)%255;if(y<0)y+=255;
            if(arg(1))memory.store(GuestAddress{arg(1)},32,rpc_ok);return done(static_cast<std::uint32_t>(y*256+x));
        }
        if(name.starts_with("DceErrorInqText")){
            // 文字取自系統訊息表（FormatMessage），最多 DCE_C_ERROR_STRING_LEN（256）個字元；找不到時回 RPC_S_INVALID_ARG。
            if(!arg(1))return done(rpc_invalid_parameter);GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(512);if(!buffer)return done(rpc_no_memory);
            const auto length=nested_service(state,memory,"kernel32.dll","FormatMessageW",{0x1200,0,static_cast<std::uint32_t>(arg(0)),0,buffer,256,0});
            if(!length)return done(rpc_invalid_parameter);
            const auto text=read_utf16_counted(memory,GuestAddress{buffer},static_cast<std::size_t>(std::min<std::uint64_t>(length,255)));
            if(wide){auto bytes=utf16_bytes(text);memory.write(GuestAddress{arg(1)},bytes);}
            else {auto narrow=utf16_to_utf8(text);if(narrow.size()>255)narrow.resize(255);std::vector<std::uint8_t> bytes(narrow.begin(),narrow.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}
            return done(rpc_ok);
        }
        if(name=="I_RpcAllocate")return done(allocate_heap(memory,std::max<std::uint64_t>(static_cast<std::uint32_t>(arg(0)),1)));
        if(name=="I_RpcFree"){if(arg(0))(void)free_heap(memory,arg(0),state);return done(0);}
        if(name=="I_RpcMapWin32Status")return done(static_cast<std::uint32_t>(arg(0))); // RPC_STATUS 即 Win32 錯誤碼。
        if(name.starts_with("RpcNetworkIsProtseqValid")){
            const auto text=fold(read(arg(0)));if(text.empty() || text.find_first_of(u":[]@")!=text.npos)return done(rpc_invalid_protseq);
            return done(std::find(std::begin(rpc_protocols),std::end(rpc_protocols),text)!=std::end(rpc_protocols)?rpc_ok:rpc_protseq_not_supported);
        }
        if(name.starts_with("RpcNetworkInqProtseqs")){
            // RPC_PROTSEQ_VECTOR｛unsigned Count; 字串指標[Count]｝：向量與各字串都由 RpcProtseqVectorFree 釋放。
            if(!arg(0))return done(rpc_invalid_parameter);const auto count=std::size(rpc_protocols);
            const auto vector=allocate_heap(memory,unit+count*unit);if(!vector)return done(rpc_no_memory);memory.store(GuestAddress{vector},32,count);
            for(std::size_t i=0;i<count;++i){const auto text=allocate_text(rpc_protocols[i]);if(!text){for(std::size_t j=0;j<i;++j)(void)free_heap(memory,memory.load(GuestAddress{vector+unit+j*unit},bits),state);(void)free_heap(memory,vector,state);return done(rpc_no_memory);}
                memory.store(GuestAddress{vector+unit+i*unit},bits,text);}
            memory.store(GuestAddress{arg(0)},bits,vector);return done(rpc_ok);
        }
        if(name.starts_with("RpcProtseqVectorFree") || name=="RpcBindingVectorFree"){
            // RPC_PROTSEQ_VECTOR 與 RPC_BINDING_VECTOR 版面相同（計數後接指標陣列）；binding 向量另釋放每個 binding。
            if(!arg(0))return done(rpc_invalid_parameter);const auto vector=memory.load(GuestAddress{arg(0)},bits);if(!vector)return done(rpc_ok);
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{vector},32));if(count>65536)return done(rpc_invalid_parameter);
            for(std::uint32_t i=0;i<count;++i){const auto item=memory.load(GuestAddress{vector+unit+i*unit},bits);if(!item)continue;
                if(name=="RpcBindingVectorFree")runtime_.rpc_bindings.erase(item);(void)free_heap(memory,item,state);}
            (void)free_heap(memory,vector,state);memory.store(GuestAddress{arg(0)},bits,0);return done(rpc_ok);
        }
        if(name=="RpcIfInqId"){
            // RPC_SERVER_INTERFACE／RPC_CLIENT_INTERFACE 的 InterfaceId（GUID 與主／次版本）緊接在 Length 之後，即 RPC_IF_ID。
            if(!arg(0) || !arg(1))return done(rpc_invalid_parameter);std::vector<std::uint8_t> id(20);memory.read(GuestAddress{arg(0)+4},id);memory.write(GuestAddress{arg(1)},id);return done(rpc_ok);
        }
        if(name=="RpcRaiseException"){
            // 與 RpcRaiseException 相同以不可繼續的例外丟出狀態碼。
            start_callbacks(state,memory,{{resolve("kernel32.dll","RaiseException").value,{static_cast<std::uint32_t>(arg(0)),1,0,0}}},Continuation::Return,0);return std::nullopt;
        }
        // 伺服器端呼叫內容：沙箱內沒有 RPC 伺服器呼叫，NULL（目前呼叫）回 RPC_S_NO_CALL_ACTIVE，用戶端 binding 不是伺服器端 binding。
        if(name=="RpcRevertToSelf" || name=="RpcMgmtWaitServerListen")return done(name=="RpcRevertToSelf"?rpc_no_call_active:rpc_not_listening);
        if(name=="RpcImpersonateClient" || name=="RpcRevertToSelfEx" || name.starts_with("RpcBindingInqAuthClient") || name=="RpcBindingServerFromClient"){
            if(name=="RpcBindingServerFromClient" && arg(1))memory.store(GuestAddress{arg(1)},bits,0);
            if(!arg(0))return done(rpc_no_call_active);
            return done(runtime_.rpc_bindings.contains(arg(0))?rpc_wrong_kind_of_binding:rpc_invalid_binding);
        }
        if(name=="RpcMgmtIsServerListening"){
            // 本程序沒有在監聽；遠端伺服器在沙箱內不可連線，與連線失敗相同回 RPC_S_NOT_LISTENING。
            if(arg(0) && !runtime_.rpc_bindings.contains(arg(0)))return done(rpc_invalid_binding);return done(rpc_not_listening);
        }
        if(name=="RpcBindingCopy" || name.starts_with("RpcBindingToStringBinding") || name=="RpcBindingInqObject" || name=="RpcBindingSetObject" ||
           name=="RpcBindingReset" || name=="RpcMgmtSetComTimeout" || name=="RpcBindingSetOption" || name.starts_with("RpcBindingInqAuthInfo")){
            const auto found=runtime_.rpc_bindings.find(arg(0));if(found==runtime_.rpc_bindings.end())return done(rpc_invalid_binding);auto& binding=found->second;
            if(name=="RpcBindingCopy"){
                if(!arg(1))return done(rpc_invalid_parameter);const auto handle=allocate_heap(memory,16);if(!handle)return done(rpc_no_memory);
                runtime_.rpc_bindings.emplace(handle,binding);memory.store(GuestAddress{arg(1)},bits,handle);return done(rpc_ok);
            }
            if(name.starts_with("RpcBindingToStringBinding")){
                if(!arg(1))return done(rpc_invalid_parameter);const auto text=allocate_text(binding_text(binding));if(!text)return done(rpc_no_memory);
                memory.store(GuestAddress{arg(1)},bits,text);return done(rpc_ok);
            }
            if(name=="RpcBindingInqObject"){
                if(!arg(1))return done(rpc_invalid_parameter);security::Guid uuid{};if(!binding.object.empty())uuid=sddl_guid(utf16_to_utf8(binding.object));
                memory.write(GuestAddress{arg(1)},std::span(uuid));return done(rpc_ok);
            }
            if(name=="RpcBindingSetObject"){
                security::Guid uuid{};if(arg(1))memory.read(GuestAddress{arg(1)},std::span(uuid));
                binding.object=std::all_of(uuid.begin(),uuid.end(),[](std::uint8_t b){return !b;})?std::u16string{}:utf8_to_utf16(sddl_guid_text(uuid));return done(rpc_ok);
            }
            if(name=="RpcBindingReset"){binding.endpoint.clear();return done(rpc_ok);} // 動態端點於下次呼叫重新解析。
            if(name=="RpcMgmtSetComTimeout"){const auto timeout=static_cast<std::uint32_t>(arg(1));if(timeout>10)return done(rpc_invalid_timeout);binding.com_timeout=timeout;return done(rpc_ok);}
            if(name=="RpcBindingSetOption"){const auto option=static_cast<std::uint32_t>(arg(1));if(!option || option>=17)return done(rpc_invalid_parameter);binding.settings[option]=arg(2);return done(rpc_ok);}
            // RpcBindingInqAuthInfo(Ex)：回報 SetAuthInfo(Ex) 保存的認證資訊；未設定認證時回 RPC_S_BINDING_HAS_NO_AUTH。
            if(!binding.authenticated)return done(rpc_no_auth);
            if(arg(1)){const auto text=allocate_text(binding.principal);if(!text)return done(rpc_no_memory);memory.store(GuestAddress{arg(1)},bits,text);}
            if(arg(2))memory.store(GuestAddress{arg(2)},32,binding.authn_level);if(arg(3))memory.store(GuestAddress{arg(3)},32,binding.authn_service);
            if(arg(4))memory.store(GuestAddress{arg(4)},bits,binding.identity);if(arg(5))memory.store(GuestAddress{arg(5)},32,binding.authz_service);
            if(name.starts_with("RpcBindingInqAuthInfoEx") && arg(7)){
                if(static_cast<std::uint32_t>(arg(6))!=1)return done(rpc_invalid_parameter);
                const std::array<std::uint32_t,4> qos=binding.qos_set?binding.qos:std::array<std::uint32_t,4>{1,0,0,0};
                for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(7)+i*4},32,qos[i]);
            }
            return done(rpc_ok);
        }
        if(name=="RpcBindingFree"){
            if(!arg(0))return done(rpc_invalid_binding);
            const auto handle=memory.load(GuestAddress{arg(0)},bits);const auto found=runtime_.rpc_bindings.find(handle);
            if(found==runtime_.rpc_bindings.end())return done(rpc_invalid_binding);
            runtime_.rpc_bindings.erase(found);(void)free_heap(memory,handle,state);memory.store(GuestAddress{arg(0)},bits,0);return done(rpc_ok);
        }
        if(name.starts_with("RpcServerInqCallAttributes")){
            // 沙箱內沒有 RPC 伺服器呼叫：NULL 表示「目前呼叫」而不存在；用戶端 binding 屬錯誤種類。
            if(!arg(1))return done(rpc_invalid_parameter);
            if(!arg(0))return done(rpc_no_call_active);
            return done(runtime_.rpc_bindings.contains(arg(0))?rpc_wrong_kind:rpc_invalid_binding);
        }
    }catch(const MemoryFault&){return done(rpc_invalid_parameter);}
    catch(const SddlError&){return done(rpc_invalid_string_uuid);}
    throw HostCallFault("未知的 RPC 用戶端服務");
}
// NdrClientCall2／3：解析 Oicf 程序標頭取得 binding handle（明確參數或 MIDL_STUB_DESC 的隱含 handle）。沙箱內沒有 RPC 伺服器，
// 連線必定失敗：binding 有端點時為 RPC_S_SERVER_UNAVAILABLE，沒有端點（需端點對應器）時為 EPT_S_NOT_REGISTERED。
// 程序宣告了 [comm_status] 時把狀態寫入該參數（或作為回傳值），否則與 RpcRaiseException 相同以不可繼續例外丟出。
// NdrClientCall3 的 ProcFormatString 是 DCE 語法的 Oicf 字串（NDR64 另存於 pSyntaxInfo）。
std::optional<std::uint32_t> WindowsServices::invoke_ndr_client_call(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    return service_callback::invoke<std::optional<std::uint32_t>>(
        this,threads_.current,entry_state,entry_memory,"NdrClientCall",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            const bool third=service.symbol=="NdrClientCall3";
            std::uint64_t descriptor=0,format=0,procedure=0;
            if(third){
                // MIDL_STUBLESS_PROXY_INFO：pStubDesc、ProcFormatString、FormatStringOffset。
                const auto info=call(0);procedure=call(1);descriptor=memory.load(GuestAddress{info},bits);
                format=memory.load(GuestAddress{info+unit},bits)+memory.load(GuestAddress{memory.load(GuestAddress{info+2*unit},bits)+procedure*2},16);
            }else{descriptor=call(0);format=call(1);}
            // 格式字串的堆疊位移以第一個程序參數為 0：x64 的程序參數接在固定參數後以變動參數傳入（第 3 或 2 個起），
            // Win32 的 NdrClientCall2 第三個參數則是指向呼叫者堆疊上第一個程序參數的指標。
            const unsigned first=third?3U:2U;
            const auto parameter=[&](std::uint64_t offset)->std::uint64_t{
                if(!x86_)return call(static_cast<unsigned>(first+offset/8));
                return memory.load(GuestAddress{call(2)+offset},32);
            };
            const auto byte=[&](std::uint64_t at){return static_cast<std::uint8_t>(memory.load(GuestAddress{at},8));};
            const auto word=[&](std::uint64_t at){return static_cast<std::uint16_t>(memory.load(GuestAddress{at},16));};
            const auto handle_type=byte(format),oi_flags=byte(format+1);std::uint64_t at=format+2;
            if(oi_flags&0x08U)at+=4; // rpc_flags
            const auto proc_number=word(at);at+=4; // proc_num、stack_size
            if(!third)procedure=proc_number;
            std::uint32_t status=0;std::uint64_t binding=0;
            const auto stub=[&](unsigned offset){return memory.load(GuestAddress{descriptor+offset},bits);};
            const auto implicit=stub(3*unit); // IMPLICIT_HANDLE_INFO
            std::uint64_t unbind=0,unbind_object=0;
            if(handle_type==0){
                const auto kind=byte(at),flags=byte(at+1);const auto offset=word(at+2);
                if(kind==0x32U){binding=parameter(offset);if(flags&0x80U)binding=binding?memory.load(GuestAddress{binding},bits):0;}
                else if(kind==0x30U){
                    // 內容 handle 只能由成功的呼叫建立；沙箱內沒有伺服器，NULL 內容回報 RPC_X_SS_IN_NULL_CONTEXT。
                    auto context=parameter(offset);if(flags&0x80U)context=context?memory.load(GuestAddress{context},bits):0;
                    status=context?rpc_invalid_binding:rpc_null_context;
                }else if(kind==0x31U){
                    // 自訂 binding：以 GENERIC_BINDING_ROUTINE_PAIR 的 bind 取得 handle，呼叫結束後 unbind。
                    const auto pair=stub(5*unit)+byte(at+4)*2*unit;auto object=parameter(offset);
                    if(flags&0x80U)object=memory.load(GuestAddress{object},(flags&0x0fU)==8?64:(flags&0x0fU)==2?16:(flags&0x0fU)==1?8:32);
                    binding=co_await service_callback::call_value(memory.load(GuestAddress{pair},bits),{object});
                    unbind=memory.load(GuestAddress{pair+unit},bits);unbind_object=object;
                }else status=rpc_invalid_binding;
            }else if(handle_type==0x32U)binding=implicit?memory.load(GuestAddress{implicit},bits):0;
            else if(handle_type==0x33U)binding=0; // 自動 handle：由名稱服務尋找伺服器
            else if(handle_type==0x31U){
                // 隱含自訂 binding：GENERIC_BINDING_INFO {pObj, Size, pfnBind, pfnUnbind}。
                const auto object=memory.load(GuestAddress{implicit},bits);
                binding=co_await service_callback::call_value(memory.load(GuestAddress{implicit+2*unit},bits),{object});
                unbind=memory.load(GuestAddress{implicit+3*unit},bits);unbind_object=object;
            }else status=rpc_invalid_binding;
            if(!status){
                if(handle_type==0x33U)status=ept_not_registered;
                else if(const auto found=runtime_.rpc_bindings.find(binding);found==runtime_.rpc_bindings.end())status=rpc_invalid_binding;
                else status=found->second.endpoint.empty()?ept_not_registered:rpc_server_unavailable;
            }
            if(unbind)(void)co_await service_callback::call(unbind,{unbind_object,binding});
            std::int16_t comm=-1;
            // MIDL_STUB_DESC::CommFaultOffsets（x64 位移 96、Win32 位移 52）：每個程序 {CommOffset, FaultOffset}，-1 無、-2 為回傳值。
            const auto comm_table=stub(x86_?52U:96U);
            if((oi_flags&0x20U) && comm_table)comm=static_cast<std::int16_t>(word(comm_table+procedure*4));
            if(comm==-2){call.return_to(state,status);co_return std::nullopt;}
            if(comm>=0){const auto target=parameter(static_cast<std::uint64_t>(comm));if(target)memory.store(GuestAddress{target},32,status);call.return_to(state,0);co_return std::nullopt;}
            GuestState caller=state;call.return_to(caller,0);begin_exception(caller,memory,status,1,{});state=caller;co_return std::nullopt;
        });
}
}
