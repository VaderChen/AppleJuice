#include "sandbox_certificate_roots.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <chrono>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/http.h>
#include <openssl/x509v3.h>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80090301,unsupported=0x80090302,internal=0x80090304,invalid_token=0x80090308;
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
                // CRL 的真實性由發行者簽章驗證；下載本身限時、限大小，失敗不放寬撤銷檢查。
                auto* response=OSSL_HTTP_get(url.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,4096,nullptr,nullptr,1,4U*1024U*1024U,2);
                std::shared_ptr<X509_CRL> crl(response?d2i_X509_CRL_bio(response,nullptr):nullptr,X509_CRL_free);BIO_free(response);
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
int verify_peer(int valid,X509_STORE_CTX* store) {
    if(valid)return 1;
    auto* ssl=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store,SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* session=static_cast<NativeTlsSession*>(SSL_get_app_data(ssl));
    if(!session)return 0;
    const auto error=X509_STORE_CTX_get_error(store);const auto flags=session->credentials.flags;
    if(error==X509_V_ERR_UNABLE_TO_GET_CRL) {
        auto* certificate=X509_STORE_CTX_get_current_cert(store);
        const bool no_location=X509_get_ext_by_NID(certificate,NID_crl_distribution_points,-1)<0;
        const auto* chain=X509_STORE_CTX_get0_chain(store);
        const bool root=(flags&0x400) && X509_STORE_CTX_get_error_depth(store)==sk_X509_num(chain)-1 && X509_check_issued(certificate,certificate)==X509_V_OK;
        if(root || ((flags&0x800) && no_location) || (flags&0x1000)){X509_STORE_CTX_set_error(store,X509_V_OK);return 1;}
    }
    return 0;
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
    const auto protocols=session.credentials.protocols?session.credentials.protocols:0x2800U;
    int minimum=0,maximum=0;
    for(const auto [bit,version]:std::array<std::pair<unsigned,int>,4>{{{0x80,TLS1_VERSION},{0x200,TLS1_1_VERSION},{0x800,TLS1_2_VERSION},{0x2000,TLS1_3_VERSION}}})if(protocols&bit){if(!minimum)minimum=version;maximum=version;}
    if(!minimum || !SSL_CTX_set_min_proto_version(session.context,minimum) || !SSL_CTX_set_max_proto_version(session.context,maximum))return false;
    std::uint64_t disabled=0;
    if(!(protocols&0x80))disabled|=SSL_OP_NO_TLSv1;if(!(protocols&0x200))disabled|=SSL_OP_NO_TLSv1_1;if(!(protocols&0x800))disabled|=SSL_OP_NO_TLSv1_2;if(!(protocols&0x2000))disabled|=SSL_OP_NO_TLSv1_3;
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
        {"secur32.dll","FreeCredentialsHandle",1},{"secur32.dll","InitializeSecurityContextA",12},{"secur32.dll","InitializeSecurityContextW",12},
        {"secur32.dll","AcceptSecurityContext",9},{"secur32.dll","CompleteAuthToken",2},{"secur32.dll","DeleteSecurityContext",1},{"secur32.dll","FreeContextBuffer",1},
        {"secur32.dll","QueryContextAttributesA",3},{"secur32.dll","QueryContextAttributesW",3},
        {"secur32.dll","SetContextAttributesA",4},{"secur32.dll","SetContextAttributesW",4},
        {"secur32.dll","EncryptMessage",4},{"secur32.dll","DecryptMessage",4},{"secur32.dll","ApplyControlToken",2},
        {"secur32.dll","InitSecurityInterfaceW",0},{"secur32.dll","InitSecurityInterfaceA",0}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_tls(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint32_t result=0;auto& tls=runtime_.tls;
    const auto text=[&](unsigned i){return symbol.back()=='W'?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)})):read_ansi(memory,arg(i));};
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
                if(!SSL_is_init_finished(ssl))result=invalid;
                else if(kind==4) {const std::array<std::uint32_t,5> sizes{5,256,16384,4,1};for(std::size_t i=0;i<sizes.size();++i)memory.store(GuestAddress{destination+i*4},32,sizes[i]);}
                else if(kind==94 || kind==107){
                    const unsigned unit=x86_?4:8;const auto width=static_cast<std::uint8_t>(unit*8);const auto size=kind==94?8+unit:2*unit;
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
