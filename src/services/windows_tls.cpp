#include "ansi_codepage.hpp"
#include "sandbox_certificate_roots.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/http.hpp"
#include "x64arm/tls_openssl.hpp"
#include <algorithm>
#include <chrono>
#include <array>
#include <optional>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace x64arm {
// 共用 TLS 政策的對應（WinHTTP 與 Schannel 都依賴這些結果）。
static_assert(tls_protocol_range(0)->minimum==0x0303 && tls_protocol_range(0)->maximum==0x0304);
static_assert(tls_protocol_range(0x880)->minimum==0x0301 && tls_protocol_range(0x880)->maximum==0x0303 && tls_protocol_range(0x880)->disabled==tls_protocol_tls11);
static_assert(!tls_protocol_range(0x20));
static_assert(tls_certificate_problem_ignored({10},winhttp_certificate_exceptions(0x2000)) && !tls_certificate_problem_ignored({10},winhttp_certificate_exceptions(0x100)));
static_assert(tls_certificate_problem_ignored({3},schannel_certificate_exceptions(0x1000)) && !tls_certificate_problem_ignored({3},schannel_certificate_exceptions(0x800)));
static_assert(tls_certificate_problem_ignored({3,false,false},schannel_certificate_exceptions(0x800)) && tls_certificate_problem_ignored({3,true},schannel_certificate_exceptions(0x400)));
static_assert(!tls_certificate_problem_ignored({23},winhttp_certificate_exceptions(0x3380)));  // 已撤銷永不放行
namespace {
constexpr std::uint32_t invalid=0x80090301,unsupported=0x80090302,internal=0x80090304,invalid_token=0x80090308;
struct SspiBadInput {};
constexpr std::uint32_t incomplete=0x80090318,small_buffer=0x80090321,continue_needed=0x90312,context_expired=0x90317;
constexpr std::uint64_t handle_cookie=0x534348414e4e454c;
struct Buffer {std::uint64_t descriptor{},pointer{};std::uint32_t size{},type{};};
std::vector<Buffer> buffers(GuestMemory& memory,std::uint64_t address) {
    if(!address)return {};
    if(memory.load(GuestAddress{address},32)!=0)throw HostCallFault("SecBufferDesc 版本不支援");
    const bool x86=memory.execution_mode()==ExecutionMode::X86;const std::uint8_t bits=x86?32:64;const unsigned size=x86?12U:16U;
    const auto count=memory.load(GuestAddress{address+4},32),array=memory.load(GuestAddress{address+8},bits);
    if(count>64)throw HostCallFault("SecBufferDesc 數量超過上限");
    memory.check(GuestAddress{array},static_cast<std::size_t>(count*size),Access::Write);std::vector<Buffer> result;
    for(std::uint64_t i=0;i<count;++i){const auto at=array+i*size;result.push_back({at,memory.load(GuestAddress{at+8},bits),static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32))});}
    return result;
}
void store_buffer(GuestMemory& memory,const Buffer& buffer,std::uint32_t type,std::uint64_t pointer,std::size_t size) {
    memory.store(GuestAddress{buffer.descriptor},32,size);memory.store(GuestAddress{buffer.descriptor+4},32,type);memory.store(GuestAddress{buffer.descriptor+8},memory.execution_mode()==ExecutionMode::X86?32:64,pointer);
}
std::vector<std::uint8_t> read_buffer(const GuestMemory& memory,const Buffer& buffer) {
    if(buffer.size>WindowsServices::output_limit)throw HostCallFault("TLS 單次資料超過 16 MiB 上限");
    std::vector<std::uint8_t> bytes(buffer.size);memory.read(GuestAddress{buffer.pointer},bytes);return bytes;
}
std::uint64_t read_handle(const GuestMemory& memory,std::uint64_t pointer) {
    const bool x86=memory.execution_mode()==ExecutionMode::X86;const std::uint8_t bits=x86?32:64;
    if(!pointer || memory.load(GuestAddress{pointer+(x86?4:8)},bits)!=(x86?static_cast<std::uint32_t>(handle_cookie):handle_cookie))return 0;return memory.load(GuestAddress{pointer},bits);
}
void write_handle(GuestMemory& memory,std::uint64_t pointer,std::uint64_t handle) {
    const bool x86=memory.execution_mode()==ExecutionMode::X86;const std::uint8_t bits=x86?32:64;
    memory.check(GuestAddress{pointer},x86?8:16,Access::Write);memory.store(GuestAddress{pointer},bits,handle);memory.store(GuestAddress{pointer+(x86?4:8)},bits,handle_cookie);
}
std::size_t record_size(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<5)return 5;return 5+(static_cast<std::size_t>(bytes[3])<<8)+bytes[4];
}
}
struct NativeTlsSession {
    SSL_CTX* context{};SSL* ssl{};TlsCredentials credentials;bool closing{};
    std::vector<std::uint8_t> application_data,keying_context;
    std::string keying_label;std::uint32_t keying_size{};
    std::map<std::string,std::shared_ptr<X509_CRL>> crls;
    std::chrono::steady_clock::time_point revocation_deadline{};
    explicit NativeTlsSession(const TlsCredentials& cred):credentials(cred) {}
    ~NativeTlsSession(){SSL_free(ssl);SSL_CTX_free(context);}
    std::vector<std::uint8_t> output() {
        auto* bio=SSL_get_wbio(ssl);const auto size=BIO_ctrl_pending(bio);
        if(size>WindowsServices::output_limit)throw HostCallFault("TLS 輸出超過上限");
        std::vector<std::uint8_t> bytes(size);if(size && BIO_read(bio,bytes.data(),static_cast<int>(size))!=static_cast<int>(size))throw HostCallFault("TLS BIO 輸出不完整");return bytes;
    }
};
TlsState::TlsState(const TlsState& other):digest(other.digest),credentials(other.credentials),buffers(other.buffers),next_handle(other.next_handle),function_tables(other.function_tables) {
    if(!other.sessions.empty())throw std::invalid_argument("作用中的 TLS 連線不能複製供兩後端差分執行");
}
TlsState& TlsState::operator=(const TlsState& other) {if(this!=&other){TlsState copy(other);*this=std::move(copy);}return *this;}
namespace {
STACK_OF(X509_CRL)* lookup_crls(const X509_STORE_CTX* store,const X509_NAME* issuer) {
    auto* ssl=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store,SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* session=ssl?static_cast<NativeTlsSession*>(SSL_get_app_data(ssl)):nullptr;
    auto* certificate=X509_STORE_CTX_get_current_cert(store);
    if(!session || !certificate)return nullptr;
    auto* result=X509_STORE_CTX_get1_crls(store,issuer);
    // get1_crls 查本機 store，不會再次呼叫此 lookup callback。
    if(result && sk_X509_CRL_num(result))return result;
    if(!result)result=sk_X509_CRL_new_null();
    if(!result || (session->credentials.flags&0xc000))return result;
    auto* points=static_cast<CRL_DIST_POINTS*>(X509_get_ext_d2i(certificate,NID_crl_distribution_points,nullptr,nullptr));
    if(!points)return result;
    if(session->revocation_deadline==std::chrono::steady_clock::time_point{})session->revocation_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(int p=0;p<sk_DIST_POINT_num(points);++p) {
        const auto* point=sk_DIST_POINT_value(points,p);if(!point->distpoint || point->distpoint->type!=0)continue;
        const auto* names=point->distpoint->name.fullname;
        for(int n=0;n<sk_GENERAL_NAME_num(names);++n) {
            const auto* name=sk_GENERAL_NAME_value(names,n);if(name->type!=GEN_URI)continue;
            const auto* uri=name->d.uniformResourceIdentifier;const auto length=ASN1_STRING_length(uri);if(length<8 || length>4096)continue;
            const std::string url(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),static_cast<std::size_t>(length));
            if(!url.starts_with("http://") || url.find('\0')!=url.npos)continue;
            auto found=session->crls.find(url);
            if(found==session->crls.end()) {
                if(session->crls.size()>=8 || std::chrono::steady_clock::now()>=session->revocation_deadline)continue;
                // CRL 的真實性由發行者簽章驗證；下載走與 WinHTTP 相同的宿主 HTTP 傳輸（流量計數、取消、逾時一致），
                // 限時、限大小，失敗不放寬撤銷檢查。
                // 每份 CRL 最多 2 秒，整次交握的撤銷查詢合計 10 秒。
                const auto deadline=std::min(session->revocation_deadline,std::chrono::steady_clock::now()+std::chrono::seconds(2));
                const auto remaining=static_cast<std::int32_t>(std::max<std::int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count()));
                HttpInput input;input.url=url;input.method="GET";input.max_redirects=2;input.timeouts={remaining,remaining,remaining,remaining};
                const auto response=fetch_native_http(std::move(input),deadline);
                std::shared_ptr<X509_CRL> crl;
                if(!response.error && response.status==200 && !response.body.empty() && response.body.size()<=4U*1024U*1024U) {
                    const auto* bytes=response.body.data();
                    crl.reset(d2i_X509_CRL(nullptr,&bytes,static_cast<long>(response.body.size())),X509_CRL_free);
                }
                found=session->crls.emplace(url,std::move(crl)).first;ERR_clear_error();
            }
            auto* crl=found->second.get();
            if(crl && X509_NAME_cmp(X509_CRL_get_issuer(crl),issuer)==0 && X509_CRL_up_ref(crl)) {
                if(!sk_X509_CRL_push(result,crl))X509_CRL_free(crl);
            }
        }
    }
    CRL_DIST_POINTS_free(points);return result;
}
// 驗證回呼與 WinHTTP／WinINet 的宿主傳輸共用 tls_verify_with_exceptions（同一份 OpenSSL 與 tls_policy）。
int verify_peer(int valid,X509_STORE_CTX* store) {
    if(valid)return 1;
    auto* ssl=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store,SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* session=static_cast<NativeTlsSession*>(SSL_get_app_data(ssl));
    if(!session)return 0;
    return tls_verify_with_exceptions(valid,store,schannel_certificate_exceptions(session->credentials.flags));
}
std::uint32_t tls_error(SSL* ssl,int code) {
    const auto error=SSL_get_error(ssl,code);
    if(error==SSL_ERROR_WANT_READ || error==SSL_ERROR_WANT_WRITE)return continue_needed;
    if(error==SSL_ERROR_ZERO_RETURN)return context_expired;
    switch(SSL_get_verify_result(ssl)) {
    case X509_V_OK:return 0x80090326;
    case X509_V_ERR_HOSTNAME_MISMATCH:case X509_V_ERR_IP_ADDRESS_MISMATCH:return 0x80090322;
    case X509_V_ERR_CERT_HAS_EXPIRED:case X509_V_ERR_CERT_NOT_YET_VALID:return 0x80090328;
    case X509_V_ERR_CERT_REVOKED:return 0x80090350;
    case X509_V_ERR_UNABLE_TO_GET_CRL:case X509_V_ERR_CRL_HAS_EXPIRED:case X509_V_ERR_CRL_NOT_YET_VALID:return 0x80090353;
    default:return 0x80090325;
    }
}
bool initialize_tls(NativeTlsSession& session,std::string_view hostname,ReadOnlyFiles& files) {
    session.context=SSL_CTX_new(TLS_client_method());if(!session.context)return false;
    // 版本範圍與 WinHTTP 共用 tls_protocol_range；OpenSSL 另可逐版關閉範圍內未啟用的版本。
    const auto range=tls_protocol_range(session.credentials.protocols);
    if(!range || !SSL_CTX_set_min_proto_version(session.context,range->minimum) || !SSL_CTX_set_max_proto_version(session.context,range->maximum))return false;
    std::uint64_t disabled=0;
    if(range->disabled&tls_protocol_tls11)disabled|=SSL_OP_NO_TLSv1_1;if(range->disabled&tls_protocol_tls12)disabled|=SSL_OP_NO_TLSv1_2;
    SSL_CTX_set_options(session.context,disabled|SSL_OP_NO_COMPRESSION|SSL_OP_NO_RENEGOTIATION);
    if(!SSL_CTX_set_cipher_list(session.context,"HIGH:!aNULL:!eNULL:!MD5:!3DES"))return false;
    const bool verify=!(session.credentials.flags&8);
    SSL_CTX_set_verify(session.context,verify?SSL_VERIFY_PEER:SSL_VERIFY_NONE,verify_peer);
    if(verify){const auto roots=sandbox_certificate_roots(files);if(roots.error || !add_certificate_root_pem(SSL_CTX_get_cert_store(session.context),roots.pem))return false;}
    if(verify && (session.credentials.flags&0x700))X509_STORE_set_lookup_crls(SSL_CTX_get_cert_store(session.context),lookup_crls);
    session.ssl=SSL_new(session.context);if(!session.ssl)return false;
    SSL_set_app_data(session.ssl,&session);SSL_set_connect_state(session.ssl);
    auto* input=BIO_new(BIO_s_mem());auto* output=BIO_new(BIO_s_mem());
    if(!input || !output){BIO_free(input);BIO_free(output);return false;}
    BIO_set_mem_eof_return(input,-1);BIO_set_mem_eof_return(output,-1);SSL_set_bio(session.ssl,input,output);
    if(!hostname.empty()) {
        const std::string host(hostname);auto* parameters=SSL_get0_param(session.ssl);
        if(!(session.credentials.flags&4)) {
            if(X509_VERIFY_PARAM_set1_ip_asc(parameters,host.c_str())!=1 && SSL_set1_host(session.ssl,host.c_str())!=1)return false;
        }
        if(SSL_set_tlsext_host_name(session.ssl,host.c_str())!=1)return false;
    }
    if(verify && (session.credentials.flags&0x700))X509_VERIFY_PARAM_set_flags(SSL_get0_param(session.ssl),X509_V_FLAG_CRL_CHECK|((session.credentials.flags&0x600)?X509_V_FLAG_CRL_CHECK_ALL:0));
    return true;
}
}
std::span<const WindowsServiceDescriptor> tls_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"secur32.dll","QuerySecurityPackageInfoA",2},{"secur32.dll","QuerySecurityPackageInfoW",2},
        {"secur32.dll","EnumerateSecurityPackagesA",2},{"secur32.dll","EnumerateSecurityPackagesW",2},
        {"secur32.dll","AcquireCredentialsHandleW",9},{"secur32.dll","AcquireCredentialsHandleA",9},
        {"secur32.dll","FreeCredentialsHandle",1},{"secur32.dll","InitializeSecurityContextA",12,0,0,0xb8},{"secur32.dll","InitializeSecurityContextW",12,0,0,0xb8},
        {"secur32.dll","AcceptSecurityContext",9,0,0,0x18},{"secur32.dll","CompleteAuthToken",2},{"secur32.dll","DeleteSecurityContext",1},{"secur32.dll","FreeContextBuffer",1},
        {"secur32.dll","QueryContextAttributesA",3,0,0,0x2},{"secur32.dll","QueryContextAttributesW",3,0,0,0x2},
        {"secur32.dll","QueryContextAttributesExA",4,0,0,0xa},{"secur32.dll","QueryContextAttributesExW",4,0,0,0xa},
        {"secur32.dll","SetContextAttributesA",4,0,0,0xa},{"secur32.dll","SetContextAttributesW",4,0,0,0xa},
        {"secur32.dll","EncryptMessage",4},{"secur32.dll","DecryptMessage",4},{"secur32.dll","ApplyControlToken",2},
        {"secur32.dll","InitSecurityInterfaceW",0},{"secur32.dll","InitSecurityInterfaceA",0},
        {"secur32.dll","QueryCredentialsAttributesA",3,0,0,0x2},{"secur32.dll","QueryCredentialsAttributesW",3,0,0,0x2},
        {"secur32.dll","SspiEncodeStringsAsAuthIdentity",4},{"secur32.dll","SspiEncodeAuthIdentityAsStrings",4},{"secur32.dll","SspiFreeAuthIdentity",1},
        {"secur32.dll","SspiZeroAuthIdentity",1},{"secur32.dll","SspiLocalFree",1},{"secur32.dll","SspiMarshalAuthIdentity",3},
        {"secur32.dll","SspiUnmarshalAuthIdentity",3,0,0,0x1},{"secur32.dll","SspiPrepareForCredWrite",7}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_tls(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint32_t result=0;auto& tls=runtime_.tls;
    const auto text=[&](unsigned i){return symbol.back()=='W'?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):ansi_to_utf8(read_ansi(memory,arg(i)));};
    const auto expiry=[&](std::uint64_t pointer){if(pointer)memory.store(GuestAddress{pointer},64,INT64_MAX);};
    const auto package_name=[](std::string value){for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return value=="schannel" || value=="microsoft unified security protocol provider";};
    if(symbol=="InitSecurityInterfaceW" || symbol=="InitSecurityInterfaceA"){
        // SecurityFunctionTable（SECURITY_SUPPORT_PROVIDER_INTERFACE_VERSION_4）：各槽指向本 DLL 既有的 SSPI 入口；
        // 尚未提供的函式指向未解析匯入陷阱（呼叫時可診斷），保留欄位依官方佈局為 0。程序內傳回同一份表。
        const bool wide=symbol.back()=='W';auto& table=tls.function_tables[wide?1:0];
        if(!table){
            const std::string suffix=wide?"W":"A";
            const std::vector<std::string> names{"EnumerateSecurityPackages"+suffix,"QueryCredentialsAttributes"+suffix,"AcquireCredentialsHandle"+suffix,
                "FreeCredentialsHandle","","InitializeSecurityContext"+suffix,"AcceptSecurityContext","CompleteAuthToken","DeleteSecurityContext",
                "ApplyControlToken","QueryContextAttributes"+suffix,"ImpersonateSecurityContext","RevertSecurityContext","MakeSignature",
                "VerifySignature","FreeContextBuffer","QuerySecurityPackageInfo"+suffix,"","","ExportSecurityContext","ImportSecurityContext"+suffix,
                "AddCredentials"+suffix,"","QuerySecurityContextToken","EncryptMessage","DecryptMessage","SetContextAttributes"+suffix,
                "SetCredentialsAttributes"+suffix,"ChangeAccountPassword"+suffix};
            const unsigned unit=x86_?4U:8U;std::vector<std::uint8_t> bytes(unit+names.size()*unit);bytes[0]=4;
            for(std::size_t i=0;i<names.size();++i){
                const auto target=names[i].empty()?0:bind_import("secur32.dll",names[i],true).value;
                for(unsigned b=0;b<unit;++b)bytes[unit+i*unit+b]=static_cast<std::uint8_t>(target>>(8*b));
            }
            table=runtime_allocate(memory,bytes);
        }
        arg.return_to(state,table);return std::nullopt;
    }
    if(symbol.starts_with("Sspi")){
        // SSPI 認證身分輔助函式：不透明身分為 SEC_WINNT_AUTH_IDENTITY_W（UNICODE，結構與字串在同一 LocalAlloc 區塊），可直接交給 AcquireCredentialsHandle；
        // 輸入另接受 SEC_WINNT_AUTH_IDENTITY_EXW（Version 0x200）與 ANSI 旗標。序列化為 SEC_WINNT_AUTH_IDENTITY_EX2（Version 0x201，長度以位元組計），
        // 密碼放在型別 SEC_WINNT_AUTH_DATA_TYPE_PASSWORD 的 SEC_WINNT_AUTH_PACKED_CREDENTIALS。所有輸出以 LocalAlloc 配置，SspiFreeAuthIdentity／SspiLocalFree 以 LocalFree 釋放。
        constexpr std::uint32_t ok=0,no_memory=0x80090300,bad_parameter=0x8009035d;
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        constexpr std::array<std::uint8_t,16> password_type{0x2f,0xc3,0xbf,0x28,0xf6,0x10,0x38,0x47,0x98,0xd1,0x1a,0xc0,0x61,0xdf,0x71,0x6a};
        struct Identity {std::optional<std::u16string> user,domain,password;};
        const auto local_alloc=[&](std::size_t size)->std::uint64_t{if(size>0x100000)return 0;return nested_service(state,memory,"kernel32.dll","LocalAlloc",{0x40,static_cast<std::uint32_t>(std::max<std::size_t>(size,1))});};
        const auto local_free=[&](std::uint64_t pointer){if(pointer)(void)nested_service(state,memory,"kernel32.dll","LocalFree",{pointer});};
        const auto read_identity=[&](std::uint64_t at)->std::optional<Identity>{
            if(!at)return std::nullopt;
            const bool extended=memory.load(GuestAddress{at},32)==0x200U && memory.load(GuestAddress{at+4},32)==(x86_?44U:72U);
            const unsigned base=extended?8U:0U;const unsigned step=x86_?8U:16U;const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{at+(extended?(x86_?32U:52U):(x86_?24U:44U))},32));
            if(!(flags&3U))return std::nullopt;
            Identity identity;
            const auto field=[&](unsigned index)->std::optional<std::u16string>{
                const std::uint64_t pointer=memory.load(GuestAddress{at+base+index*step},bits);const auto length=static_cast<std::uint32_t>(memory.load(GuestAddress{at+base+index*step+unit},32));
                if(!pointer)return std::nullopt;if(length>4096)throw SspiBadInput{};
                if(flags&2U){std::u16string text(length,u'\0');for(std::uint32_t i=0;i<length;++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{pointer+i*2},16));return text;}
                std::string text(length,'\0');for(std::uint32_t i=0;i<length;++i)text[i]=static_cast<char>(memory.load(GuestAddress{pointer+i},8));return utf8_to_utf16_lossy(text);
            };
            identity.user=field(0);identity.domain=field(1);identity.password=field(2);return identity;
        };
        const auto store_identity=[&](const Identity& identity)->std::uint64_t{
            const unsigned header=x86_?28U:48U;std::size_t size=header;for(const auto* part:{&identity.user,&identity.domain,&identity.password})if(*part)size+=((*part)->size()+1)*2;
            const auto block=local_alloc(size);if(!block)return 0;std::uint64_t cursor=block+header;
            const auto put=[&](unsigned index,const std::optional<std::u16string>& text){if(!text)return;memory.write(GuestAddress{cursor},utf16_bytes(*text));
                memory.store(GuestAddress{block+index*(x86_?8U:16U)},bits,cursor);memory.store(GuestAddress{block+index*(x86_?8U:16U)+unit},32,text->size());cursor+=(text->size()+1)*2;};
            put(0,identity.user);put(1,identity.domain);put(2,identity.password);memory.store(GuestAddress{block+(x86_?24U:44U)},32,2);return block;
        };
        const auto store_text=[&](const std::u16string& text)->std::uint64_t{const auto block=local_alloc((text.size()+1)*2);if(block)memory.write(GuestAddress{block},utf16_bytes(text));return block;};
        const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        try{
            if(symbol=="SspiEncodeStringsAsAuthIdentity"){
                // (pszUserName, pszDomainName, pszPackedCredentialsString, ppAuthIdentity)
                if(!arg(3))return finish(bad_parameter);if(!arg(0) && !arg(1) && !arg(2))return finish(invalid_token);
                Identity identity;const auto text_at=[&](unsigned i)->std::optional<std::u16string>{if(!arg(i))return std::nullopt;return read_utf16(memory,GuestAddress{arg(i)},4096);};
                identity.user=text_at(0);identity.domain=text_at(1);identity.password=text_at(2);
                memory.check(GuestAddress{arg(3)},unit,Access::Write);const auto block=store_identity(identity);if(!block)return finish(no_memory);
                memory.store(GuestAddress{arg(3)},bits,block);return finish(ok);
            }
            if(symbol=="SspiEncodeAuthIdentityAsStrings"){
                // (pAuthIdentity, ppszUserName, ppszDomainName, ppszPackedCredentialsString)：沒有的欄位輸出 NULL。
                const auto identity=read_identity(arg(0));if(!identity)return finish(invalid_token);
                std::vector<std::uint64_t> made;
                for(unsigned i=0;i<3;++i){if(!arg(i+1))continue;memory.check(GuestAddress{arg(i+1)},unit,Access::Write);
                    const auto& part=i==0?identity->user:i==1?identity->domain:identity->password;std::uint64_t pointer=0;
                    if(part){pointer=store_text(*part);if(!pointer){for(const auto item:made)local_free(item);return finish(no_memory);}made.push_back(pointer);}
                    memory.store(GuestAddress{arg(i+1)},bits,pointer);}
                return finish(ok);
            }
            if(symbol=="SspiFreeAuthIdentity" || symbol=="SspiLocalFree"){local_free(arg(0));return finish(0);}
            if(symbol=="SspiZeroAuthIdentity"){
                // 清除字串與結構（之後仍可交給 SspiFreeAuthIdentity）。
                if(!arg(0))return finish(0);const bool extended=memory.load(GuestAddress{arg(0)},32)==0x200U && memory.load(GuestAddress{arg(0)+4},32)==(x86_?44U:72U);
                const unsigned base=extended?8U:0U,step=x86_?8U:16U;const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+(extended?(x86_?32U:52U):(x86_?24U:44U))},32));
                for(unsigned i=0;i<3;++i){const std::uint64_t pointer=memory.load(GuestAddress{arg(0)+base+i*step},bits);const auto length=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+base+i*step+unit},32));
                    if(pointer && length<=4096)memory.write(GuestAddress{pointer},std::vector<std::uint8_t>(static_cast<std::size_t>(length)*((flags&2U)?2U:1U)));}
                memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(extended?(x86_?44U:72U):(x86_?28U:48U)));return finish(0);
            }
            if(symbol=="SspiMarshalAuthIdentity"){
                // (AuthIdentity, AuthIdentityLength, AuthIdentityByteArray)
                const auto identity=read_identity(arg(0));if(!identity)return finish(invalid_token);if(!arg(1) || !arg(2))return finish(bad_parameter);
                std::vector<std::uint8_t> bytes(48);
                const auto put16=[&](std::size_t at,std::uint32_t v){bytes[at]=static_cast<std::uint8_t>(v);bytes[at+1]=static_cast<std::uint8_t>(v>>8);};
                const auto put32=[&](std::size_t at,std::uint32_t v){put16(at,v&0xffffU);put16(at+2,v>>16);};
                const auto append=[&](const std::vector<std::uint8_t>& part)->std::uint32_t{const auto at=static_cast<std::uint32_t>(bytes.size());bytes.insert(bytes.end(),part.begin(),part.end());return at;};
                const auto text_bytes=[](const std::u16string& text){auto out=utf16_bytes(text);out.resize(text.size()*2);return out;};
                put32(0,0x201);put16(4,48);
                if(identity->user){const auto data=text_bytes(*identity->user);put32(12,append(data));put16(16,static_cast<std::uint32_t>(data.size()));}
                if(identity->domain){const auto data=text_bytes(*identity->domain);put32(20,append(data));put16(24,static_cast<std::uint32_t>(data.size()));}
                if(identity->password){
                    // SEC_WINNT_AUTH_PACKED_CREDENTIALS：cbHeaderLength、cbStructureLength、AuthData{CredType, CredData{ByteArrayOffset, ByteArrayLength}}，偏移相對於本結構。
                    const auto secret=text_bytes(*identity->password);std::vector<std::uint8_t> packed(28);
                    packed[0]=28;packed[2]=static_cast<std::uint8_t>((28+secret.size())&0xff);packed[3]=static_cast<std::uint8_t>((28+secret.size())>>8);
                    std::copy(password_type.begin(),password_type.end(),packed.begin()+4);packed[20]=28;packed[24]=static_cast<std::uint8_t>(secret.size());packed[25]=static_cast<std::uint8_t>(secret.size()>>8);
                    packed.insert(packed.end(),secret.begin(),secret.end());put32(28,append(packed));put16(32,static_cast<std::uint32_t>(packed.size()));
                }
                put32(36,2);put32(8,static_cast<std::uint32_t>(bytes.size()));
                memory.check(GuestAddress{arg(1)},4,Access::Write);memory.check(GuestAddress{arg(2)},unit,Access::Write);
                const auto block=local_alloc(bytes.size());if(!block)return finish(no_memory);memory.write(GuestAddress{block},bytes);
                memory.store(GuestAddress{arg(1)},32,bytes.size());memory.store(GuestAddress{arg(2)},bits,block);return finish(ok);
            }
            if(symbol=="SspiUnmarshalAuthIdentity"){
                // (AuthIdentityLength, AuthIdentityByteArray, ppAuthIdentity)
                const auto length=static_cast<std::uint32_t>(arg(0));if(!arg(1) || !arg(2) || length<48 || length>0x100000)return finish(bad_parameter);
                std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{arg(1)},bytes);
                const auto get16=[&](std::size_t at)->std::uint32_t{return bytes[at]|(std::uint32_t{bytes[at+1]}<<8);};const auto get32=[&](std::size_t at){return get16(at)|(get16(at+2)<<16);};
                if(get32(0)!=0x201 || get16(4)<48 || get32(8)>length)return finish(invalid_token);
                const auto slice=[&](std::uint32_t offset,std::uint32_t size)->std::optional<std::vector<std::uint8_t>>{if(!size)return std::nullopt;if(offset>length || size>length-offset)throw SspiBadInput{};return std::vector<std::uint8_t>(bytes.begin()+offset,bytes.begin()+offset+size);};
                const auto text_of=[](const std::vector<std::uint8_t>& data){std::u16string text;for(std::size_t i=0;i+1<data.size();i+=2)text.push_back(static_cast<char16_t>(data[i]|(data[i+1]<<8)));return text;};
                Identity identity;
                try{
                    if(const auto data=slice(get32(12),get16(16)))identity.user=text_of(*data);
                    if(const auto data=slice(get32(20),get16(24)))identity.domain=text_of(*data);
                    if(const auto packed=slice(get32(28),get16(32))){
                        if(packed->size()<28 || !std::equal(password_type.begin(),password_type.end(),packed->begin()+4))return finish(unsupported);
                        const std::uint32_t offset=std::uint32_t{(*packed)[20]}|(std::uint32_t{(*packed)[21]}<<8)|(std::uint32_t{(*packed)[22]}<<16)|(std::uint32_t{(*packed)[23]}<<24);
                        const std::uint32_t size=std::uint32_t{(*packed)[24]}|(std::uint32_t{(*packed)[25]}<<8);
                        if(offset>packed->size() || size>packed->size()-offset)return finish(invalid_token);
                        identity.password=text_of(std::vector<std::uint8_t>(packed->begin()+offset,packed->begin()+offset+size));
                    }
                }catch(const SspiBadInput&){return finish(invalid_token);}
                memory.check(GuestAddress{arg(2)},unit,Access::Write);const auto block=store_identity(identity);if(!block)return finish(no_memory);
                memory.store(GuestAddress{arg(2)},bits,block);return finish(ok);
            }
            if(symbol=="SspiPrepareForCredWrite"){
                // (AuthIdentity, pszTargetName, pCredmanCredentialType, ppszCredmanTargetName, ppszCredmanUserName, ppCredentialBlob, pCredentialBlobSize)：
                // CRED_TYPE_DOMAIN_PASSWORD；使用者為「網域\使用者」；沒有目標名稱時用使用者名稱；blob 為密碼的 UTF-16（不含 NUL）。
                const auto identity=read_identity(arg(0));if(!identity)return finish(invalid_token);
                for(unsigned i=2;i<7;++i)if(!arg(i))return finish(bad_parameter);
                const auto user=(identity->domain && !identity->domain->empty()?*identity->domain+u"\\":std::u16string{})+identity->user.value_or(u"");
                const auto target=arg(1)?read_utf16(memory,GuestAddress{arg(1)},4096):user;
                auto secret=utf16_bytes(identity->password.value_or(u""));secret.resize(identity->password.value_or(u"").size()*2);
                for(unsigned i=2;i<7;++i)memory.check(GuestAddress{arg(i)},i==2 || i==6?4U:unit,Access::Write);
                const auto target_block=store_text(target),user_block=store_text(user),blob=local_alloc(secret.size());
                if(!target_block || !user_block || !blob){local_free(target_block);local_free(user_block);local_free(blob);return finish(no_memory);}
                if(!secret.empty())memory.write(GuestAddress{blob},secret);
                memory.store(GuestAddress{arg(2)},32,2);memory.store(GuestAddress{arg(3)},bits,target_block);memory.store(GuestAddress{arg(4)},bits,user_block);
                memory.store(GuestAddress{arg(5)},bits,blob);memory.store(GuestAddress{arg(6)},32,secret.size());return finish(ok);
            }
        }catch(const MemoryFault&){return finish(bad_parameter);}catch(const SspiBadInput&){return finish(bad_parameter);}
    }
    if(symbol=="CompleteAuthToken" || symbol.starts_with("QuerySecurityPackageInfo") || symbol.starts_with("EnumerateSecurityPackages"))return invoke_digest(state,memory,service);
    if(symbol.starts_with("AcquireCredentialsHandle")){
        try{auto package=text(1);for(auto& c:package)if(c>='A' && c<='Z')c=static_cast<char>(c+32);if(package=="wdigest" || package=="digest")return invoke_digest(state,memory,service);}catch(const MemoryFault&){arg.return_to(state,0x8009035d);return {};}
    }else if(symbol!="FreeContextBuffer"){
        const unsigned index=symbol.starts_with("InitializeSecurityContext") || symbol=="AcceptSecurityContext"?1U:0U;
        const auto digest_handle=[&](std::uint64_t at){try{return at && memory.load(GuestAddress{at+(x86_?4U:8U)},x86_?32:64)==(x86_?static_cast<std::uint32_t>(sspi_digest::cookie):sspi_digest::cookie);}catch(const MemoryFault&){return false;}};
        if(digest_handle(arg(index)) || ((symbol.starts_with("InitializeSecurityContext") || symbol=="AcceptSecurityContext") && digest_handle(arg(0))))return invoke_digest(state,memory,service);
    }
    if(!runtime_.network.external_io && symbol!="FreeContextBuffer")throw HostCallFault("compare 不建立無法重播的 TLS 狀態");
    if(symbol.starts_with("AcquireCredentialsHandle")) {
        const auto package=text(1);TlsCredentials credentials;
        if(!package_name(package))result=0x80090305;
        else if(arg(0) || static_cast<std::uint32_t>(arg(2))!=2 || arg(3) || arg(5) || arg(6))result=unsupported;
        else {
            if(arg(4)) {
                memory.check(GuestAddress{arg(4)},x86_?56:80,Access::Read);
                if(memory.load(GuestAddress{arg(4)},32)!=4 || memory.load(GuestAddress{arg(4)+4},32) || memory.load(GuestAddress{arg(4)+(x86_?12:16)},x86_?32:64) || memory.load(GuestAddress{arg(4)+(x86_?16:24)},32) || memory.load(GuestAddress{arg(4)+(x86_?24:40)},32) || memory.load(GuestAddress{arg(4)+(x86_?52:76)},32))result=unsupported;
                credentials.protocols=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+(x86_?32:56)},32));credentials.minimum_bits=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+(x86_?36:60)},32));credentials.maximum_bits=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+(x86_?40:64)},32));credentials.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+(x86_?48:72)},32));
                if((credentials.protocols&~0x2a80U) || (credentials.flags&~0x405ffeU) || ((credentials.flags&0x28)==0x28))result=unsupported;
            }
            if(!result){if(tls.credentials.size()>=1024)result=0x80090300;else {const auto handle=tls.next_handle++;write_handle(memory,arg(7),handle);expiry(arg(8));tls.credentials.emplace(handle,credentials);}}
        }
    } else if(symbol=="FreeCredentialsHandle") {if(!tls.credentials.erase(read_handle(memory,arg(0))))result=invalid;}
    else if(symbol.starts_with("QueryCredentialsAttributes")) {
        // Schannel 認證：SECPKG_ATTR_SUPPORTED_ALGS（86，ALG_ID 陣列以 FreeContextBuffer 釋放）、SECPKG_ATTR_CIPHER_STRENGTHS（87）、SECPKG_ATTR_SUPPORTED_PROTOCOLS（88）；
        // 值來自 AcquireCredentialsHandle 的 SCHANNEL_CRED（未指定時為 OpenSSL 用戶端預設：TLS 1.2／1.3、128～256 位元）。Schannel 沒有認證名稱屬性。
        const auto found=tls.credentials.find(read_handle(memory,arg(0)));const auto attribute=static_cast<std::uint32_t>(arg(1));
        if(found==tls.credentials.end())result=invalid;
        else if(!arg(2))result=0x8009035d;
        else if(attribute==87){memory.check(GuestAddress{arg(2)},8,Access::Write);memory.store(GuestAddress{arg(2)},32,found->second.minimum_bits?found->second.minimum_bits:128U);memory.store(GuestAddress{arg(2)+4},32,found->second.maximum_bits?found->second.maximum_bits:256U);}
        else if(attribute==88){memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,found->second.protocols?found->second.protocols:0x2800U);}
        else if(attribute==86){
            // CALG_AES_128、CALG_AES_256、CALG_SHA_256、CALG_SHA_384、CALG_ECDHE、CALG_RSA_KEYX、CALG_ECDSA、CALG_RSA_SIGN（OpenSSL 用戶端實際協商的演算法）。
            constexpr std::array<std::uint32_t,8> algorithms{0x660e,0x6610,0x800c,0x800d,0xae06,0xa400,0x2203,0x2400};
            const unsigned unit=x86_?4U:8U;memory.check(GuestAddress{arg(2)},unit*2,Access::Write);
            const auto block=allocate_heap(memory,algorithms.size()*4);if(!block)result=0x80090300;
            else{for(std::size_t i=0;i<algorithms.size();++i)memory.store(GuestAddress{block+i*4},32,algorithms[i]);tls.buffers.insert(block);
                memory.store(GuestAddress{arg(2)},32,algorithms.size());memory.store(GuestAddress{arg(2)+unit},static_cast<std::uint8_t>(unit*8),block);}
        }else result=unsupported;
    }
    else if(symbol=="FreeContextBuffer") {if(arg(0) && (!tls.buffers.erase(arg(0)) || !free_heap(memory,arg(0),state)))result=invalid;}
    else if(symbol=="AcceptSecurityContext")result=unsupported;
    else if(symbol.starts_with("InitializeSecurityContext")) {
        const auto flags_value=static_cast<std::uint32_t>(arg(3));const auto input=buffers(memory,arg(6)),output=buffers(memory,arg(9));
        auto handle=read_handle(memory,arg(1));auto found=tls.sessions.find(handle);
        if(static_cast<std::uint32_t>(arg(4)) || static_cast<std::uint32_t>(arg(7)) || (flags_value&~0x1c19cU))result=unsupported;
        else if(output.empty())result=invalid_token;
        else {
            if(!arg(1)) {
                const auto cred=tls.credentials.find(read_handle(memory,arg(0)));
                if(cred==tls.credentials.end())result=invalid;
                else if(tls.sessions.size()>=1024)result=0x80090300;
                else {
                    auto session=std::make_shared<NativeTlsSession>(cred->second);
                    if(!initialize_tls(*session,arg(2)?text(2):"",filesystem_))result=internal;
                    else {handle=tls.next_handle++;found=tls.sessions.emplace(handle,std::move(session)).first;}
                }
            } else if(found==tls.sessions.end())result=invalid;
            if(!result) {
                auto& session=*found->second;auto* ssl=session.ssl;std::vector<std::uint8_t> token;std::size_t consumed=0;
                const auto incoming=std::find_if(input.begin(),input.end(),[](const auto& b){return b.type==2;});
                if(incoming!=input.end())token=read_buffer(memory,*incoming);
                if(session.closing){const auto status=SSL_shutdown(ssl);result=status<0?tls_error(ssl,status):0;}
                else {
                    result=continue_needed;
                    if(token.empty()){const auto status=SSL_do_handshake(ssl);result=status==1?0:tls_error(ssl,status);}
                    while(consumed<token.size() && result==continue_needed) {
                        const auto part=std::span<const std::uint8_t>(token).subspan(consumed);const auto size=record_size(part);
                        if(size>18437){result=invalid_token;break;}
                        if(part.size()<size){if(!consumed)result=incomplete;break;}
                        if(BIO_write(SSL_get_rbio(ssl),part.data(),static_cast<int>(size))!=static_cast<int>(size)){result=internal;break;}
                        consumed+=size;ERR_clear_error();const auto status=SSL_do_handshake(ssl);result=status==1?0:tls_error(ssl,status);
                    }
                }
                if(!result && !session.closing) {
                    const auto bits=static_cast<unsigned>(SSL_get_cipher_bits(ssl,nullptr));
                    if(bits<session.credentials.minimum_bits || (session.credentials.maximum_bits && bits>session.credentials.maximum_bits))result=0x80090331;
                }
                if(incoming!=input.end() && consumed && consumed<token.size()) {
                    const auto extra=std::find_if(input.begin(),input.end(),[](const auto& b){return b.type==0;});
                    if(extra!=input.end())store_buffer(memory,*extra,5,incoming->pointer+consumed,token.size()-consumed);
                }
                const auto bytes=session.output();
                auto outgoing=std::find_if(output.begin(),output.end(),[](const auto& b){return b.type==2 || b.type==0;});
                if(outgoing==output.end())result=invalid_token;
                else if(!bytes.empty()) {
                    auto pointer=outgoing->pointer;
                    if(flags_value&0x100){pointer=allocate_heap(memory,bytes.size());if(pointer)tls.buffers.insert(pointer);else result=0x80090300;}
                    else if(bytes.size()>outgoing->size)result=small_buffer;
                    if(pointer && result!=small_buffer && result!=0x80090300){memory.write(GuestAddress{pointer},bytes);store_buffer(memory,*outgoing,2,pointer,bytes.size());}
                } else if(outgoing!=output.end())store_buffer(memory,*outgoing,2,outgoing->pointer,0);
                if(arg(8))write_handle(memory,arg(8),handle);
                if(arg(10))memory.store(GuestAddress{arg(10)},32,flags_value);expiry(arg(11));
            }
        }
    } else {
        const auto found=tls.sessions.find(read_handle(memory,arg(0)));
        if(found==tls.sessions.end())result=invalid;
        else if(symbol=="DeleteSecurityContext")tls.sessions.erase(found);
        else if(symbol=="CompleteAuthToken"){
            // Microsoft 的契約僅支援 Digest SSP；此有效 context 屬於 Schannel。
            result=unsupported;
        }
        else {
            auto& session=*found->second;auto* ssl=session.ssl;
            if(symbol=="ApplyControlToken") {
                const auto input=buffers(memory,arg(1));if(input.size()!=1 || input[0].type!=2 || input[0].size!=4 || memory.load(GuestAddress{input[0].pointer},32)!=1)result=unsupported;else session.closing=true;
            } else if(symbol.starts_with("SetContextAttributes")) {
                const auto kind=arg(1),source=arg(2),size=arg(3);const unsigned unit=x86_?4:8;const auto width=static_cast<std::uint8_t>(unit*8);
                constexpr std::uint32_t bad_parameter=0x8009035d;
                try {
                    if(kind==94){
                        if(size!=8+unit || !source)result=bad_parameter;
                        else {memory.check(GuestAddress{source},8+unit,Access::Read);const auto count=memory.load(GuestAddress{source+4},32),data=memory.load(GuestAddress{source+8},width);
                            if(memory.load(GuestAddress{source},32) || count>output_limit || (count && !data))result=bad_parameter;
                            else {std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));if(count)memory.read(GuestAddress{data},bytes);session.application_data=std::move(bytes);}}
                    } else if(kind==106){
                        if(size!=5*unit || !source)result=bad_parameter;
                        else {memory.check(GuestAddress{source},5*unit,Access::Read);const auto label_size=memory.load(GuestAddress{source},16),label_at=memory.load(GuestAddress{source+unit},width),context_size=memory.load(GuestAddress{source+2*unit},16),context_at=memory.load(GuestAddress{source+3*unit},width),amount=memory.load(GuestAddress{source+4*unit},32);
                            if(label_size<2 || !label_at || !amount || amount>output_limit || (context_size?!context_at:context_at!=0))result=bad_parameter;
                            else {std::string label(static_cast<std::size_t>(label_size),'\0');memory.read(GuestAddress{label_at},std::span(reinterpret_cast<std::uint8_t*>(label.data()),label.size()));
                                if(label.back() || std::any_of(label.begin(),label.end()-1,[](char c){return !c || static_cast<unsigned char>(c)>127;}))result=bad_parameter;
                                else {label.pop_back();std::vector<std::uint8_t> context(static_cast<std::size_t>(context_size));if(context_size)memory.read(GuestAddress{context_at},context);session.keying_label=std::move(label);session.keying_context=std::move(context);session.keying_size=static_cast<std::uint32_t>(amount);}}
                        }
                    } else if(kind==34){
                        if(size!=4 || !source)result=bad_parameter;
                        else if(!SSL_is_dtls(ssl))result=unsupported;
                        else {const auto mtu=memory.load(GuestAddress{source},32);if(mtu<200 || mtu>65536)result=bad_parameter;else if(SSL_set_mtu(ssl,static_cast<long>(mtu))<=0)result=internal;}
                    } else result=unsupported;
                }catch(const MemoryFault&){result=bad_parameter;}
            } else if(symbol.starts_with("QueryContextAttributes")) {
                const auto kind=arg(1),destination=arg(2);
                const unsigned unit=x86_?4:8;
                unsigned required=0;
                switch(kind){
                case 4: required=20; break;
                case 94: required=8+unit; break;
                case 100: required=680; break;
                case 107: required=2*unit; break;
                }
                if(!SSL_is_init_finished(ssl))result=invalid;
                else if(!required)result=unsupported;
                else if(!destination)result=0x8009035d;
                else if(symbol.starts_with("QueryContextAttributesEx") && arg(3)<required)result=small_buffer;
                else if(kind==4) {const std::array<std::uint32_t,5> sizes{5,256,16384,4,1};for(std::size_t i=0;i<sizes.size();++i)memory.store(GuestAddress{destination+i*4},32,sizes[i]);}
                else if(kind==94 || kind==107){
                    const auto width=static_cast<std::uint8_t>(unit*8);const auto size=required;
                    memory.check(GuestAddress{destination},size,Access::Write);std::vector<std::uint8_t> payload;
                    if(kind==94)payload=session.application_data;
                    else if(!session.keying_size)result=0x8009035d;
                    else {payload.resize(session.keying_size);ERR_clear_error();if(SSL_export_keying_material(ssl,payload.data(),payload.size(),session.keying_label.data(),session.keying_label.size(),session.keying_context.data(),session.keying_context.size(),!session.keying_context.empty())!=1)result=internal;}
                    if(!result){const auto allocation=payload.empty()?0:allocate_heap(memory,payload.size());if(!payload.empty() && !allocation)result=0x80090300;
                        else {if(allocation){memory.write(GuestAddress{allocation},payload);tls.buffers.insert(allocation);}memory.write(GuestAddress{destination},std::vector<std::uint8_t>(size));memory.store(GuestAddress{destination+(kind==94?4:0)},32,payload.size());memory.store(GuestAddress{destination+(kind==94?8:unit)},width,allocation);}}
                    if(!payload.empty())OPENSSL_cleanse(payload.data(),payload.size());
                }
                else if(kind==100) {
                    memory.write(GuestAddress{destination},std::array<std::uint8_t,680>{});const auto* cipher=SSL_get_current_cipher(ssl);
                    memory.store(GuestAddress{destination},32,1);memory.store(GuestAddress{destination+4},32,static_cast<std::uint32_t>(SSL_version(ssl)));
                    memory.store(GuestAddress{destination+8},32,SSL_CIPHER_get_protocol_id(cipher));memory.store(GuestAddress{destination+12},32,SSL_CIPHER_get_protocol_id(cipher));
                    const auto put=[&](unsigned offset,const char* value){auto name=utf8_to_utf16(value?value:"");if(name.size()>63)name.resize(63);memory.write(GuestAddress{destination+offset},utf16_bytes(name));};
                    put(16,SSL_CIPHER_standard_name(cipher));put(144,OBJ_nid2sn(SSL_CIPHER_get_cipher_nid(cipher)));put(280,OBJ_nid2sn(SSL_CIPHER_get_digest_nid(cipher)));put(412,OBJ_nid2sn(SSL_CIPHER_get_kx_nid(cipher)));put(548,OBJ_nid2sn(SSL_CIPHER_get_auth_nid(cipher)));
                    memory.store(GuestAddress{destination+272},32,static_cast<unsigned>(SSL_CIPHER_get_bits(cipher,nullptr)));
                } else result=unsupported;
            } else if(symbol=="EncryptMessage") {
                auto output=buffers(memory,arg(2));
                const auto find=[&](unsigned type){return std::find_if(output.begin(),output.end(),[&](const auto& b){return b.type==type;});};
                auto header=find(7),data=find(1),trailer=find(6);
                if(arg(1) || !SSL_is_init_finished(ssl))result=unsupported;
                else if(header==output.end() || data==output.end() || trailer==output.end() || data->size>16384)result=invalid_token;
                else if(header->size<5 || trailer->size<256)result=small_buffer;
                else {
                    memory.check(GuestAddress{header->pointer},header->size,Access::Write);memory.check(GuestAddress{data->pointer},data->size,Access::Write);memory.check(GuestAddress{trailer->pointer},trailer->size,Access::Write);
                    const auto plain=read_buffer(memory,*data);std::size_t written=0;ERR_clear_error();const auto status=SSL_write_ex(ssl,plain.data(),plain.size(),&written);
                    if(!status)result=tls_error(ssl,status);
                    else {
                        const auto encrypted=session.output();
                        if(written!=plain.size() || encrypted.size()<5+written || encrypted.size()>5+written+trailer->size)result=internal;
                        else {memory.write(GuestAddress{header->pointer},std::span(encrypted).first(5));memory.write(GuestAddress{data->pointer},std::span(encrypted).subspan(5,written));memory.write(GuestAddress{trailer->pointer},std::span(encrypted).subspan(5+written));store_buffer(memory,*header,7,header->pointer,5);store_buffer(memory,*trailer,6,trailer->pointer,encrypted.size()-5-written);}
                    }
                }
            } else if(symbol=="DecryptMessage") {
                auto output=buffers(memory,arg(1));const auto input=std::find_if(output.begin(),output.end(),[](const auto& b){return b.type==1;});
                if(output.size()<4 || input==output.end() || !SSL_is_init_finished(ssl))result=invalid_token;
                else {
                    const auto bytes=read_buffer(memory,*input);const auto size=record_size(bytes);
                    if(size>18437)result=invalid_token;
                    else if(bytes.size()<size)result=incomplete;
                    else {
                        memory.check(GuestAddress{input->pointer},size,Access::Write);if(arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
                        if(BIO_write(SSL_get_rbio(ssl),bytes.data(),static_cast<int>(size))!=static_cast<int>(size))result=internal;
                        else {
                            std::array<std::uint8_t,16384> plain{};std::size_t read=0;ERR_clear_error();const auto status=SSL_read_ex(ssl,plain.data(),plain.size(),&read);
                            result=status==1?0:tls_error(ssl,status);if(result==continue_needed)result=0;
                            if(!result || result==context_expired) {
                                memory.write(GuestAddress{input->pointer+5},std::span(plain).first(read));
                                store_buffer(memory,output[0],7,input->pointer,5);store_buffer(memory,output[1],1,input->pointer+5,read);store_buffer(memory,output[2],6,input->pointer+5+read,size-5-read);
                                store_buffer(memory,output[3],bytes.size()>size?5:0,bytes.size()>size?input->pointer+size:0,bytes.size()-size);if(arg(3))memory.store(GuestAddress{arg(3)},32,0);
                            }
                        }
                    }
                }
            }
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
