#include "x64arm/http.hpp"
#include "x64arm/resource_usage.hpp"
#include "x64arm/tls_openssl.hpp"
#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include "x64arm/crypto.hpp"
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <charconv>
#include <deque>
#include <span>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <unistd.h>

namespace x64arm {
namespace {
struct HttpError {std::uint32_t code;};
// 憑證例外：libcurl 與 Schannel 使用同一份靜態 OpenSSL 與驗證回呼（tls_openssl.hpp）。
// 這個連線的 HttpInput 掛在 curl 建立的 SSL_CTX 上，驗證回呼依它的 SECURITY_FLAG_IGNORE_* 決定例外。
int http_input_index(){static const int index=SSL_CTX_get_ex_new_index(0,nullptr,nullptr,nullptr,nullptr);return index;}
int verify_certificate(int valid,X509_STORE_CTX* store){
    auto* ssl=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store,SSL_get_ex_data_X509_STORE_CTX_idx()));
    const auto* input=ssl?static_cast<const HttpInput*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl),http_input_index())):nullptr;
    return tls_verify_with_exceptions(valid,store,input?winhttp_certificate_exceptions(input->certificate_exceptions):0U);
}
// curl 以 SSL_VERIFY_NONE 建立 context、交握後再查驗證結果；保留它的模式，只掛上回呼。
CURLcode configure_ssl_context(CURL*,void* context,void* opaque){
    auto* ctx=static_cast<SSL_CTX*>(context);
    if(http_input_index()<0 || !SSL_CTX_set_ex_data(ctx,http_input_index(),opaque))return CURLE_SSL_CONNECT_ERROR;
    SSL_CTX_set_verify(ctx,SSL_CTX_get_verify_mode(ctx),verify_certificate);return CURLE_OK;
}
std::string lower(std::string value){for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return value;}
struct Url {std::string scheme,host,port;std::string origin()const{return scheme+"://"+host+":"+port;}};
Url parse_url(const std::string& text){
    std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url(curl_url(),curl_url_cleanup);if(!url)throw std::bad_alloc();
    if(curl_url_set(url.get(),CURLUPART_URL,text.c_str(),0)!=CURLUE_OK)throw HttpError{12005};
    const auto part=[&](CURLUPart field){char* raw=nullptr;if(curl_url_get(url.get(),field,&raw,CURLU_DEFAULT_PORT)!=CURLUE_OK)throw HttpError{12005};
        std::unique_ptr<char,decltype(&curl_free)> owned(raw,curl_free);return lower(std::string(raw));};
    Url value{part(CURLUPART_SCHEME),part(CURLUPART_HOST),part(CURLUPART_PORT)};if(value.scheme!="http" && value.scheme!="https")throw HttpError{12006};return value;
}
std::vector<std::string> entries(std::string_view list){
    std::vector<std::string> result;for(std::size_t i=0;i<list.size();){const auto begin=list.find_first_not_of(";, \t",i);if(begin==list.npos)break;
        const auto end=list.find_first_of(";, \t",begin);result.emplace_back(list.substr(begin,end==list.npos?list.size()-begin:end-begin));if(end==list.npos)break;i=end+1;}return result;
}
bool wildcard(std::string_view pattern,std::string_view value){
    std::size_t p=0,v=0,star=pattern.npos,retry=0;
    while(v<value.size()){
        if(p<pattern.size() && (pattern[p]=='?' || pattern[p]==value[v])){++p;++v;}
        else if(p<pattern.size() && pattern[p]=='*'){star=p++;retry=v;}
        else if(star!=pattern.npos){p=star+1;v=++retry;}
        else return false;
    }
    while(p<pattern.size() && pattern[p]=='*')++p;return p==pattern.size();
}
std::string proxy_for_url(const HttpInput& input,const Url& url){
    for(auto pattern:entries(input.bypass)){
        pattern=lower(std::move(pattern));if(pattern=="<local>" && url.host.find('.')==url.host.npos)return {};
        if(wildcard(pattern,url.host) || wildcard(pattern,url.host+":"+url.port))return {};
    }
    std::string fallback;
    for(const auto& entry:entries(input.proxy)){
        const auto equal=entry.find('=');if(equal==entry.npos){if(fallback.empty())fallback=entry;}
        else if(lower(entry.substr(0,equal))==url.scheme)return entry.substr(equal+1);
    }
    return fallback;
}
struct Transfer {
    std::atomic<bool> done{},cancel{};
    std::mutex wait_mutex;std::condition_variable finished;
    HttpResult result;
    mutable std::mutex io_mutex;process_io::Counters io;
    std::chrono::steady_clock::time_point progress{std::chrono::steady_clock::now()};
    curl_off_t download{},upload{};
    std::int32_t send_timeout{},receive_timeout{};
    bool receiving{},connected{},secure{};CURL* handle{};
};
bool transfer_expired(Transfer& transfer,std::chrono::steady_clock::time_point now,bool receiving){
    const auto timeout=receiving?transfer.receive_timeout:transfer.send_timeout;
    if(transfer.connected && timeout>0 && std::chrono::duration_cast<std::chrono::milliseconds>(now-transfer.progress).count()>timeout){transfer.result.error=12002;return true;}
    return false;
}
int io_callback(CURL*,curl_infotype type,char*,size_t size,void* userdata){
    auto& transfer=*static_cast<Transfer*>(userdata);if(!size)return 0;
    const bool read=transfer.secure?type==CURLINFO_SSL_DATA_IN:type==CURLINFO_HEADER_IN || type==CURLINFO_DATA_IN;
    const bool write=transfer.secure?type==CURLINFO_SSL_DATA_OUT:type==CURLINFO_HEADER_OUT || type==CURLINFO_DATA_OUT;
    if(read || write){std::lock_guard lock(transfer.io_mutex);const unsigned index=read?0U:1U;
        if(!transfer.io.values[index])++transfer.io.values[index];transfer.io.values[index+3]+=size;
    }return 0;
}
std::uint32_t curl_error(CURLcode value){
    switch(value){case CURLE_OK:return 0;case CURLE_OPERATION_TIMEDOUT:return 12002;case CURLE_COULDNT_RESOLVE_HOST:case CURLE_COULDNT_RESOLVE_PROXY:return 12007;
    case CURLE_COULDNT_CONNECT:return 12029;case CURLE_ABORTED_BY_CALLBACK:return 12017;case CURLE_PEER_FAILED_VERIFICATION:case CURLE_SSL_CONNECT_ERROR:return 12175;
    case CURLE_TOO_MANY_REDIRECTS:return 12156;case CURLE_URL_MALFORMAT:return 12005;case CURLE_OUT_OF_MEMORY:return 8;default:return 12030;}
}
size_t body_callback(char* bytes,size_t size,size_t count,void* userdata){
    auto& transfer=*static_cast<Transfer*>(userdata);if(size && count>SIZE_MAX/size)return 0;const auto length=size*count;
    const auto now=std::chrono::steady_clock::now();if(transfer_expired(transfer,now,true))return 0;transfer.progress=now;
    if(length>64*1024*1024-transfer.result.body.size()){transfer.result.error=8;return 0;}
    try{transfer.result.body.insert(transfer.result.body.end(),reinterpret_cast<std::uint8_t*>(bytes),reinterpret_cast<std::uint8_t*>(bytes)+length);}
    catch(...){transfer.result.error=8;return 0;}return length;
}
size_t header_callback(char* bytes,size_t size,size_t count,void* userdata){
    auto& transfer=*static_cast<Transfer*>(userdata);if(size && count>SIZE_MAX/size)return 0;const auto length=size*count;
    const auto now=std::chrono::steady_clock::now();if(transfer_expired(transfer,now,true))return 0;
    if(length>65536 || transfer.result.headers.size()>=1024){transfer.result.error=12182;return 0;}
    try{
        std::string line(bytes,length);while(!line.empty() && (line.back()=='\r' || line.back()=='\n'))line.pop_back();
        if(line.starts_with("HTTP/")){transfer.result.status_line=std::move(line);transfer.result.headers.clear();transfer.result.body.clear();}
        else if(const auto colon=line.find(':');colon!=std::string::npos){auto value=line.substr(colon+1);const auto first=value.find_first_not_of(" \t");value=first==std::string::npos?"":value.substr(first);transfer.result.headers.emplace_back(line.substr(0,colon),std::move(value));}
        transfer.receiving=true;transfer.progress=now;
    }catch(...){transfer.result.error=8;return 0;}return length;
}
int progress_callback(void* userdata,curl_off_t,curl_off_t download,curl_off_t upload_total,curl_off_t upload){
    auto& transfer=*static_cast<Transfer*>(userdata);if(transfer.cancel.load())return 1;
    const auto now=std::chrono::steady_clock::now();
    // HTTP 以 curl 的累計 payload 差值計數；重新導向歸零時建立新的基線。
    record_network_transfer(download>transfer.download?static_cast<std::uint64_t>(download-transfer.download):0,upload>transfer.upload?static_cast<std::uint64_t>(upload-transfer.upload):0);
    const bool progressed=download!=transfer.download || upload!=transfer.upload;
    transfer.download=download;transfer.upload=upload;
    if(!transfer.connected){double connected=0;curl_easy_getinfo(transfer.handle,transfer.secure?CURLINFO_APPCONNECT_TIME:CURLINFO_CONNECT_TIME,&connected);transfer.progress=now;if(connected<=0)return 0;transfer.connected=true;}
    if(transfer_expired(transfer,now,transfer.receiving || upload>=upload_total))return 1;
    if(progressed)transfer.progress=now;return 0;
}
// TLS 驗證、憑證例外、信任來源與協定版本：一般請求、WebSocket、PAC 與 CRL 下載共用同一組設定，
// 規則本身在 tls_policy.hpp 與 Schannel 共用。
void configure_security(CURL* curl,const HttpInput& input){
    const auto exceptions=winhttp_certificate_exceptions(input.certificate_exceptions);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,(exceptions&tls_ignore_name)?0L:2L);
    if(!input.certificate_authorities.empty())curl_easy_setopt(curl,CURLOPT_CAINFO,input.certificate_authorities.c_str());
    else if(!input.certificate_roots.empty()){
        // 只信任沙箱信任根：關閉 curl 內建的系統 CA 檔與目錄，再以記憶體 PEM 載入。
        curl_blob roots{const_cast<char*>(input.certificate_roots.data()),input.certificate_roots.size(),CURL_BLOB_NOCOPY};
        curl_easy_setopt(curl,CURLOPT_CAINFO,nullptr);curl_easy_setopt(curl,CURLOPT_CAPATH,nullptr);
        if(curl_easy_setopt(curl,CURLOPT_CAINFO_BLOB,&roots)!=CURLE_OK)throw HttpError{12175};
    }
    if(exceptions&~tls_ignore_name){
        if(curl_easy_setopt(curl,CURLOPT_SSL_CTX_FUNCTION,&configure_ssl_context)!=CURLE_OK ||
           curl_easy_setopt(curl,CURLOPT_SSL_CTX_DATA,const_cast<HttpInput*>(&input))!=CURLE_OK)throw HttpError{50};
    }
    if(input.secure_protocols){
        // curl 只能設定連續範圍；範圍內被關閉的版本（range->disabled）無法個別排除，依 WinHTTP 行為以首尾版本為界。
        const auto range=tls_protocol_range(input.secure_protocols);if(!range)throw HttpError{12175};
        constexpr long versions[]{CURL_SSLVERSION_TLSv1_0,CURL_SSLVERSION_TLSv1_1,CURL_SSLVERSION_TLSv1_2,CURL_SSLVERSION_TLSv1_3};
        constexpr long maximums[]{CURL_SSLVERSION_MAX_TLSv1_0,CURL_SSLVERSION_MAX_TLSv1_1,CURL_SSLVERSION_MAX_TLSv1_2,CURL_SSLVERSION_MAX_TLSv1_3};
        if(curl_easy_setopt(curl,CURLOPT_SSLVERSION,versions[range->minimum-0x0301]|maximums[range->maximum-0x0301])!=CURLE_OK)throw HttpError{12175};
    }
}
long curl_auth(std::uint32_t scheme){return static_cast<long>(scheme==1?CURLAUTH_BASIC:scheme==2?CURLAUTH_NTLM:scheme==8?CURLAUTH_DIGEST:scheme==16?CURLAUTH_NEGOTIATE:CURLAUTH_ANY);}
// 每個連線共用的設定：不讀宿主代理環境、不用訊號、TLS 政策與連線逾時。
void configure_connection(CURL* curl,const HttpInput& input){
    curl_easy_setopt(curl,CURLOPT_NOPROXY,"");curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    configure_security(curl,input);
    const auto connect=input.timeouts[1]>0?input.timeouts[1]:INT32_MAX;
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,static_cast<long>(input.timeouts[0]>0?std::min(input.timeouts[0],connect):connect));
}
// 依目標 URL 選代理並套用代理認證；回傳選到的代理（空字串為直連）。
std::string configure_proxy(CURL* curl,const HttpInput& input,const Url& url){
    const auto proxy=proxy_for_url(input,url);curl_easy_setopt(curl,CURLOPT_PROXY,proxy.c_str());
    const bool proxy_auth=!(input.disabled_features&4U) && (input.proxy_credentials_set || !input.proxy_username.empty());
    curl_easy_setopt(curl,CURLOPT_PROXYUSERNAME,proxy_auth?input.proxy_username.c_str():nullptr);
    curl_easy_setopt(curl,CURLOPT_PROXYPASSWORD,proxy_auth?input.proxy_password.c_str():nullptr);
    curl_easy_setopt(curl,CURLOPT_PROXYAUTH,proxy_auth?curl_auth(input.proxy_auth_scheme):static_cast<long>(CURLAUTH_NONE));
    return proxy;
}
class Request final:public NativeHttp {
public:
    explicit Request(std::shared_ptr<Transfer> state):state_(std::move(state)){}
    ~Request() override {cancel();}
    void cancel() noexcept override {state_->cancel.store(true);}
    process_io::Counters io_counters() const override {std::lock_guard lock(state_->io_mutex);return state_->io;}
    bool done() const override {return state_->done.load(std::memory_order_acquire);}
    bool wait_until(std::chrono::steady_clock::time_point deadline) override {
        std::unique_lock lock(state_->wait_mutex);return state_->finished.wait_until(lock,deadline,[&]{return done();});
    }
    HttpResult take() override {if(!done())throw std::logic_error("HTTP 尚未完成");return std::move(state_->result);}
private:std::shared_ptr<Transfer> state_;
};
// ---- RFC 6455 用戶端（WinHTTP WebSocket）----
// 內建 libcurl 停用 ws／wss（CURL_DISABLE_WEBSOCKETS，需要 WinHTTP 的緩衝區類型與關閉語義），以 CONNECT_ONLY 建立 TCP／TLS（含代理 CONNECT 通道）後，自行送出升級握手與處理訊框。
constexpr std::size_t websocket_inbound_limit=16*1024*1024,websocket_outbound_limit=64*1024*1024;
struct Utf8Validator {
    unsigned need{};std::uint8_t low{0x80},high{0xbf};
    bool feed(std::uint8_t c){
        if(need){if(c<low || c>high)return false;low=0x80;high=0xbf;--need;return true;}
        if(c<0x80)return true;
        if(c>=0xc2 && c<=0xdf){need=1;return true;}
        if(c>=0xe0 && c<=0xef){need=2;low=c==0xe0?0xa0:0x80;high=c==0xed?0x9f:0xbf;return true;}
        if(c>=0xf0 && c<=0xf4){need=3;low=c==0xf0?0x90:0x80;high=c==0xf4?0x8f:0xbf;return true;}
        return false;
    }
    bool complete() const {return !need;}
};
struct WebSocketPiece {std::uint32_t type{};std::vector<std::uint8_t> data;std::size_t offset{};bool last{};};
struct WebSocketState {
    std::atomic<bool> handshake_done{},cancel{};
    HttpResult result;
    mutable std::mutex mutex;
    std::deque<WebSocketPiece> inbound;std::size_t inbound_bytes{};
    std::vector<std::uint8_t> outbound;std::size_t outbound_offset{};std::uint64_t queued{},written{};
    std::uint32_t error{},send_fragment{UINT32_MAX};
    bool upgraded{},close_sent{},close_received{},discard{},finished{};
    std::uint16_t close_code{};std::vector<std::uint8_t> close_reason;
    process_io::Counters io;
    std::uint32_t keepalive{},chunk{32768};
    int wake[2]{-1,-1};
    WebSocketState(){if(pipe(wake)!=0){wake[0]=wake[1]=-1;return;}for(int fd:wake)fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);}
    ~WebSocketState(){for(int fd:wake)if(fd>=0)close(fd);}
    void notify() const {if(wake[1]>=0){const char c=0;(void)::write(wake[1],&c,1);}}
    void drain() const {char buffer[64];while(wake[0]>=0 && ::read(wake[0],buffer,sizeof(buffer))>0){}}
    // 呼叫端持有 mutex。
    std::uint64_t append_frame(std::uint8_t opcode,bool final,std::span<const std::uint8_t> payload){
        const auto before=outbound.size();const auto n=payload.size();
        outbound.push_back(static_cast<std::uint8_t>((final?0x80U:0U)|opcode));
        if(n<126)outbound.push_back(static_cast<std::uint8_t>(0x80U|n));
        else if(n<=0xffff){outbound.push_back(0x80U|126U);outbound.push_back(static_cast<std::uint8_t>(n>>8));outbound.push_back(static_cast<std::uint8_t>(n));}
        else {outbound.push_back(0x80U|127U);for(int i=7;i>=0;--i)outbound.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(n)>>(i*8)));}
        std::uint8_t mask[4];arc4random_buf(mask,sizeof(mask));outbound.insert(outbound.end(),mask,mask+4);
        const auto base=outbound.size();outbound.resize(base+n);for(std::size_t i=0;i<n;++i)outbound[base+i]=payload[i]^mask[i&3];
        queued+=outbound.size()-before;return queued;
    }
    // 協定錯誤：回覆 close（1002／1007）後關閉，接收端在交付完既有資料後回報錯誤。
    void fail(std::uint16_t status,std::uint32_t code){
        if(!error)error=code;
        if(!close_sent){const std::uint8_t body[2]{static_cast<std::uint8_t>(status>>8),static_cast<std::uint8_t>(status)};append_frame(8,true,body);close_sent=true;}
    }
};
// 伺服器端訊框解析：伺服器不可遮罩、保留位元須為 0、控制訊框不可分段且不超過 125 位元組。
struct WebSocketParser {
    std::vector<std::uint8_t> header,control;bool payload{},fin{};std::uint8_t opcode{};std::uint64_t remaining{};
    std::uint32_t message{UINT32_MAX};Utf8Validator utf8;bool stopped{};
    // 呼叫端持有 mutex。
    void feed(WebSocketState& state,std::span<const std::uint8_t> bytes){
        std::size_t i=0;
        while(i<bytes.size() && !stopped && !state.error){
            if(!payload){
                header.push_back(bytes[i++]);if(header.size()<2)continue;
                const auto length7=header[1]&0x7fU;const std::size_t need=2+(length7==126?2:length7==127?8:0);
                if(header.size()<need)continue;
                fin=(header[0]&0x80U)!=0;opcode=header[0]&0x0fU;
                std::uint64_t length=length7;if(length7==126)length=(static_cast<std::uint64_t>(header[2])<<8)|header[3];
                else if(length7==127){length=0;for(unsigned k=0;k<8;++k)length=(length<<8)|header[2+k];}
                const bool control_frame=opcode>=8;
                if((header[0]&0x70U) || (header[1]&0x80U) || (length7==127 && (length>>63)) || (opcode>2 && opcode<8) || opcode>10 ||
                   (control_frame && (!fin || length>125)) || (opcode==0 && message==UINT32_MAX) || ((opcode==1 || opcode==2) && message!=UINT32_MAX)){state.fail(1002,12152);return;}
                if(opcode==1 || opcode==2){message=opcode==1?2U:0U;utf8={};}
                header.clear();payload=true;remaining=length;control.clear();
                if(!remaining && !finish(state))return;
                continue;
            }
            const auto take=static_cast<std::size_t>(std::min<std::uint64_t>(remaining,bytes.size()-i));
            const auto chunk=bytes.subspan(i,take);i+=take;remaining-=take;
            if(opcode>=8)control.insert(control.end(),chunk.begin(),chunk.end());
            else {
                if(message==2U)for(auto c:chunk)if(!utf8.feed(c)){state.fail(1007,12152);return;}
                // 片段先以未結束加入；訊息最後一個訊框完成時才標記結束（零長度的結尾訊框也一樣）。
                if(!state.discard && take){
                    if(!state.inbound.empty() && state.inbound.back().type==message && !state.inbound.back().last && state.inbound.back().data.size()+take<=state.chunk){
                        auto& piece=state.inbound.back();piece.data.insert(piece.data.end(),chunk.begin(),chunk.end());
                    }else state.inbound.push_back({message,std::vector<std::uint8_t>(chunk.begin(),chunk.end()),0,false});
                    state.inbound_bytes+=take;
                }
            }
            if(!remaining && !finish(state))return;
        }
    }
    bool finish(WebSocketState& state){
        payload=false;
        if(opcode<8){if(fin){if(message==2U && !utf8.complete()){state.fail(1007,12152);return false;}
                if(!state.discard){if(!state.inbound.empty() && state.inbound.back().type==message && !state.inbound.back().last)state.inbound.back().last=true;else state.inbound.push_back({message,{},0,true});}
                message=UINT32_MAX;}return true;}
        if(opcode==9){if(!state.close_sent)state.append_frame(10,true,control);return true;}
        if(opcode==10)return true;
        // close：1 位元組本文為協定錯誤；狀態碼須是可傳送的值；原因須為 UTF-8。
        std::uint16_t status=1005;
        if(control.size()==1){state.fail(1002,12152);return false;}
        if(control.size()>=2){
            status=static_cast<std::uint16_t>((control[0]<<8)|control[1]);
            const bool valid=(status>=1000 && status<=1014 && status!=1004 && status!=1005 && status!=1006) || (status>=3000 && status<=4999);
            if(!valid){state.fail(1002,12152);return false;}
            Utf8Validator text;for(std::size_t k=2;k<control.size();++k)if(!text.feed(control[k])){state.fail(1007,12152);return false;}
            if(!text.complete()){state.fail(1007,12152);return false;}
        }
        state.close_received=true;state.close_code=status;state.close_reason.assign(control.size()>2?control.begin()+2:control.end(),control.end());
        state.inbound.push_back({4,{},0,true});stopped=true;return false;
    }
};
class WebSocket final:public NativeWebSocket {
public:
    explicit WebSocket(std::shared_ptr<WebSocketState> state):state_(std::move(state)){}
    ~WebSocket() override {cancel();}
    bool readable() const override {std::lock_guard lock(state_->mutex);return !state_->inbound.empty() || state_->error || state_->finished;}
    std::uint32_t receive(std::size_t capacity,std::vector<std::uint8_t>& data,std::uint32_t& type) override {
        auto& s=*state_;std::lock_guard lock(s.mutex);data.clear();
        if(s.inbound.empty())return s.error?s.error:4317U;
        if(s.inbound.front().type==4){type=4;s.inbound.pop_front();return 0;}
        const auto message=s.inbound.front().type;bool last=false;
        while(data.size()<capacity && !s.inbound.empty() && s.inbound.front().type==message){
            auto& piece=s.inbound.front();const auto take=std::min(capacity-data.size(),piece.data.size()-piece.offset);
            data.insert(data.end(),piece.data.begin()+static_cast<std::ptrdiff_t>(piece.offset),piece.data.begin()+static_cast<std::ptrdiff_t>(piece.offset+take));piece.offset+=take;s.inbound_bytes-=take;
            if(piece.offset<piece.data.size())break;
            last=piece.last;s.inbound.pop_front();if(last)break;
        }
        type=last?message:message+1;s.notify();return 0;
    }
    std::pair<std::uint32_t,std::uint64_t> send(std::uint32_t type,std::span<const std::uint8_t> payload) override {
        auto& s=*state_;std::lock_guard lock(s.mutex);
        if(s.error)return {s.error,0};if(s.close_sent || s.finished)return {4317,0};
        const auto kind=type&2U;if(s.send_fragment!=UINT32_MAX && s.send_fragment!=kind)return {87,0};
        if(payload.size()>websocket_outbound_limit-std::min(websocket_outbound_limit,s.outbound.size()-s.outbound_offset))return {8,0};
        const std::uint8_t opcode=s.send_fragment!=UINT32_MAX?0:kind?1:2;const bool final=!(type&1U);
        s.send_fragment=final?UINT32_MAX:kind;const auto ticket=s.append_frame(opcode,final,payload);s.notify();return {0,ticket};
    }
    std::pair<std::uint32_t,std::uint64_t> send_close(std::uint16_t status,std::span<const std::uint8_t> reason) override {
        auto& s=*state_;std::lock_guard lock(s.mutex);
        if(s.close_sent)return {4317,0};if(s.finished)return {s.error?s.error:4317U,0};
        std::vector<std::uint8_t> body;if(status!=1005){body={static_cast<std::uint8_t>(status>>8),static_cast<std::uint8_t>(status)};body.insert(body.end(),reason.begin(),reason.end());}
        s.close_sent=true;const auto ticket=s.append_frame(8,true,body);s.notify();return {0,ticket};
    }
    std::uint64_t written() const override {std::lock_guard lock(state_->mutex);return state_->written;}
    std::uint32_t error() const override {std::lock_guard lock(state_->mutex);return state_->error;}
    bool close_sent() const override {std::lock_guard lock(state_->mutex);return state_->close_sent;}
    bool close_status(std::uint16_t& status,std::vector<std::uint8_t>& reason) const override {
        std::lock_guard lock(state_->mutex);if(!state_->close_received)return false;status=state_->close_code;reason=state_->close_reason;return true;}
    void discard_data() override {
        auto& s=*state_;std::lock_guard lock(s.mutex);s.discard=true;
        std::erase_if(s.inbound,[](const WebSocketPiece& piece){return piece.type!=4;});s.inbound_bytes=0;s.notify();
    }
    void set_keepalive(std::uint32_t interval) override {std::lock_guard lock(state_->mutex);state_->keepalive=interval;state_->notify();}
    void cancel() noexcept override {state_->cancel.store(true);state_->notify();}
    process_io::Counters io_counters() const override {std::lock_guard lock(state_->mutex);return state_->io;}
private:std::shared_ptr<WebSocketState> state_;
};
class UpgradeRequest final:public NativeHttp {
public:
    explicit UpgradeRequest(std::shared_ptr<WebSocketState> state):state_(std::move(state)){}
    ~UpgradeRequest() override {if(!adopted_)cancel();}
    void cancel() noexcept override {state_->cancel.store(true);state_->notify();}
    process_io::Counters io_counters() const override {std::lock_guard lock(state_->mutex);return state_->io;}
    bool done() const override {return state_->handshake_done.load(std::memory_order_acquire);}
    HttpResult take() override {if(!done())throw std::logic_error("HTTP 尚未完成");return std::move(state_->result);}
    std::shared_ptr<NativeWebSocket> upgrade() override {
        if(!done() || adopted_)return {};{std::lock_guard lock(state_->mutex);if(!state_->upgraded)return {};}
        adopted_=true;return std::make_shared<WebSocket>(state_);
    }
private:std::shared_ptr<WebSocketState> state_;bool adopted_{};
};
// 在取消或逾時前等待 socket 可讀／可寫；timeout 小於等於 0 表示不限時。
bool wait_socket(WebSocketState& state,curl_socket_t socket,short events,std::int32_t timeout,std::chrono::steady_clock::time_point since){
    for(;;){
        if(state.cancel.load())throw HttpError{12017};
        int slice=1000;
        if(timeout>0){const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-since).count();if(elapsed>=timeout)throw HttpError{12002};slice=static_cast<int>(std::min<std::int64_t>(slice,timeout-elapsed));}
        pollfd fds[2]{{socket,events,0},{state.wake[0],POLLIN,0}};const auto ready=poll(fds,state.wake[0]>=0?2:1,slice);
        if(ready<0 && errno!=EINTR)throw HttpError{12030};
        if(state.wake[0]>=0 && (fds[1].revents&POLLIN))state.drain();
        if(ready>0 && fds[0].revents)return true;
    }
}
void count_io(WebSocketState& state,bool read,std::size_t bytes){
    std::lock_guard lock(state.mutex);const unsigned index=read?0U:1U;if(!state.io.values[index])++state.io.values[index];state.io.values[index+3]+=bytes;
    record_network_transfer(read?bytes:0,read?0:bytes);
}
void send_all(WebSocketState& state,CURL* curl,curl_socket_t socket,std::span<const std::uint8_t> bytes,std::int32_t timeout){
    auto since=std::chrono::steady_clock::now();
    while(!bytes.empty()){
        size_t sent=0;const auto code=curl_easy_send(curl,bytes.data(),bytes.size(),&sent);
        if(code==CURLE_AGAIN){wait_socket(state,socket,POLLOUT,timeout,since);continue;}
        if(code!=CURLE_OK)throw HttpError{12030};count_io(state,false,sent);bytes=bytes.subspan(sent);since=std::chrono::steady_clock::now();
    }
}
// 讀取一批資料；連線關閉回傳 false。
bool receive_some(WebSocketState& state,CURL* curl,curl_socket_t socket,std::vector<std::uint8_t>& buffer,std::int32_t timeout){
    const auto since=std::chrono::steady_clock::now();std::uint8_t chunk[16384];
    for(;;){
        size_t got=0;const auto code=curl_easy_recv(curl,chunk,sizeof(chunk),&got);
        if(code==CURLE_AGAIN){wait_socket(state,socket,POLLIN,timeout,since);continue;}
        if(code!=CURLE_OK)throw HttpError{12030};if(!got)return false;
        count_io(state,true,got);buffer.insert(buffer.end(),chunk,chunk+got);return true;
    }
}
std::string header_value(const HttpHeaders& headers,std::string_view name){for(const auto& [key,value]:headers)if(lower(key)==name)return value;return {};}
bool has_token(std::string_view list,std::string_view token){
    for(const auto& item:entries(list)){auto value=lower(item);if(value==token)return true;}return false;
}
void run_websocket(const std::shared_ptr<WebSocketState>& state,const HttpInput& input){
    CURL* curl=curl_easy_init();std::vector<std::uint8_t> buffer;curl_socket_t socket=CURL_SOCKET_BAD;
    const auto finish_handshake=[&]{state->handshake_done.store(true,std::memory_order_release);};
    try{
        if(!curl)throw std::bad_alloc();
        const auto parsed=parse_url(input.url);
        std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url(curl_url(),curl_url_cleanup);if(!url)throw std::bad_alloc();
        if(curl_url_set(url.get(),CURLUPART_URL,input.url.c_str(),0)!=CURLUE_OK)throw HttpError{12005};
        const auto part=[&](CURLUPart field,unsigned flags)->std::string{char* raw=nullptr;if(curl_url_get(url.get(),field,&raw,flags)!=CURLUE_OK)return {};
            std::unique_ptr<char,decltype(&curl_free)> owned(raw,curl_free);return std::string(raw);};
        auto target=part(CURLUPART_PATH,0);if(target.empty())target="/";if(const auto query=part(CURLUPART_QUERY,0);!query.empty())target+="?"+query;
        auto host=part(CURLUPART_HOST,0);if(host.find(':')!=std::string::npos && host.front()!='[')host="["+host+"]";
        if(const auto port=part(CURLUPART_PORT,0);!port.empty() && port!=(parsed.scheme=="https"?"443":"80"))host+=":"+port;
        configure_connection(curl,input);
        // WebSocket 走代理時一律以 CONNECT 建立通道，握手與訊框直接在通道內傳送。
        if(!configure_proxy(curl,input,parsed).empty())curl_easy_setopt(curl,CURLOPT_HTTPPROXYTUNNEL,1L);
        curl_easy_setopt(curl,CURLOPT_URL,input.url.c_str());curl_easy_setopt(curl,CURLOPT_CONNECT_ONLY,1L);
        const auto connected=curl_easy_perform(curl);if(connected!=CURLE_OK)throw HttpError{curl_error(connected)};
        if(curl_easy_getinfo(curl,CURLINFO_ACTIVESOCKET,&socket)!=CURLE_OK || socket==CURL_SOCKET_BAD)throw HttpError{12030};
        std::uint8_t nonce[16];arc4random_buf(nonce,sizeof(nonce));const auto key=crypto_base64_encode(nonce);
        std::string request="GET "+target+" HTTP/1.1\r\nHost: "+host+"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "+key+"\r\nSec-WebSocket-Version: 13\r\n";
        bool authorization=false;
        for(const auto& [name,value]:input.headers){
            const auto field=lower(name);
            if(field=="host" || field=="upgrade" || field=="connection" || field=="sec-websocket-key" || field=="sec-websocket-version" || field=="content-length" || field=="transfer-encoding")continue;
            authorization|=field=="authorization";request+=name+": "+value+"\r\n";
        }
        // 伺服器認證只能預先送出 Basic；其他方案需要多次往返，升級握手沿用標頭中的 Authorization。
        if(!authorization && !(input.disabled_features&4U) && (input.credentials_set || !input.username.empty()) && input.auth_scheme==1){
            const auto pair=input.username+":"+input.password;request+="Authorization: Basic "+crypto_base64_encode({reinterpret_cast<const std::uint8_t*>(pair.data()),pair.size()})+"\r\n";}
        request+="\r\n";
        send_all(*state,curl,socket,{reinterpret_cast<const std::uint8_t*>(request.data()),request.size()},input.timeouts[2]);
        std::size_t end=std::string::npos;
        while(end==std::string::npos){
            if(!receive_some(*state,curl,socket,buffer,input.timeouts[3]))throw HttpError{12152};
            const std::string_view view(reinterpret_cast<const char*>(buffer.data()),buffer.size());end=view.find("\r\n\r\n");
            if(end==std::string::npos && buffer.size()>65536)throw HttpError{12182};
        }
        const std::string head(reinterpret_cast<const char*>(buffer.data()),end);buffer.erase(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(end+4));
        HttpResult result;result.effective_url=input.url;
        for(std::size_t begin=0,line_end;begin<head.size();begin=line_end+2){
            line_end=head.find("\r\n",begin);if(line_end==std::string::npos)line_end=head.size();const auto line=head.substr(begin,line_end-begin);
            if(!begin){result.status_line=line;const auto space=line.find(' ');if(!line.starts_with("HTTP/") || space==std::string::npos)throw HttpError{12152};
                std::uint32_t status=0;const auto parsed_status=std::from_chars(line.data()+space+1,line.data()+line.size(),status);if(parsed_status.ec!=std::errc{})throw HttpError{12152};result.status=status;continue;}
            const auto colon=line.find(':');if(colon==std::string::npos)continue;auto value=line.substr(colon+1);const auto first=value.find_first_not_of(" \t");value=first==std::string::npos?"":value.substr(first);
            while(!value.empty() && (value.back()==' ' || value.back()=='\t'))value.pop_back();result.headers.emplace_back(line.substr(0,colon),std::move(value));
        }
        if(result.status==101){
            CryptoHash hash("SHA1",false,{});const auto source=key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";hash.update({reinterpret_cast<const std::uint8_t*>(source.data()),source.size()});
            const auto expected=crypto_base64_encode(hash.digest());const auto requested=header_value(input.headers,"sec-websocket-protocol"),chosen=header_value(result.headers,"sec-websocket-protocol");
            const bool valid=lower(header_value(result.headers,"upgrade"))=="websocket" && has_token(header_value(result.headers,"connection"),"upgrade") &&
                header_value(result.headers,"sec-websocket-accept")==expected && header_value(result.headers,"sec-websocket-extensions").empty() &&
                (chosen.empty() || has_token(requested,lower(chosen)));
            if(!valid)throw HttpError{12152};
            {std::lock_guard lock(state->mutex);state->result=std::move(result);state->upgraded=true;state->keepalive=input.websocket_keepalive;state->chunk=std::max<std::uint32_t>(input.websocket_receive_buffer,1);}
            finish_handshake();
        }else{
            // 伺服器拒絕升級：依 Content-Length、chunked 或連線關閉讀完本文，WinHttpReadData 照常讀取。
            const auto length=header_value(result.headers,"content-length");const bool chunked=has_token(header_value(result.headers,"transfer-encoding"),"chunked");
            const bool empty=(result.status>=100 && result.status<200) || result.status==204 || result.status==304;
            if(empty){}
            else if(chunked){
                std::vector<std::uint8_t> body;std::size_t cursor=0;
                for(;;){
                    std::string_view view(reinterpret_cast<const char*>(buffer.data()),buffer.size());auto line=view.find("\r\n",cursor);
                    while(line==std::string_view::npos){if(!receive_some(*state,curl,socket,buffer,input.timeouts[3]))throw HttpError{12030};view={reinterpret_cast<const char*>(buffer.data()),buffer.size()};line=view.find("\r\n",cursor);}
                    std::uint64_t size=0;const auto parsed_size=std::from_chars(view.data()+cursor,view.data()+line,size,16);if(parsed_size.ec!=std::errc{})throw HttpError{12152};
                    if(size>64*1024*1024-body.size())throw HttpError{8};cursor=line+2;
                    while(buffer.size()<cursor+size+2)if(!receive_some(*state,curl,socket,buffer,input.timeouts[3]))throw HttpError{12030};
                    body.insert(body.end(),buffer.begin()+static_cast<std::ptrdiff_t>(cursor),buffer.begin()+static_cast<std::ptrdiff_t>(cursor+size));cursor+=size+2;if(!size)break;
                }
                result.body=std::move(body);
            }else if(!length.empty()){
                std::uint64_t size=0;const auto parsed_length=std::from_chars(length.data(),length.data()+length.size(),size);if(parsed_length.ec!=std::errc{} || size>64*1024*1024)throw HttpError{12152};
                while(buffer.size()<size)if(!receive_some(*state,curl,socket,buffer,input.timeouts[3]))throw HttpError{12030};
                result.body.assign(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(size));
            }else{
                while(receive_some(*state,curl,socket,buffer,input.timeouts[3]))if(buffer.size()>64*1024*1024)throw HttpError{8};
                result.body=std::move(buffer);
            }
            {std::lock_guard lock(state->mutex);state->result=std::move(result);}
            finish_handshake();curl_easy_cleanup(curl);return;
        }
    }catch(const HttpError& error){std::lock_guard lock(state->mutex);state->result={};state->result.error=error.code;}
    catch(...){std::lock_guard lock(state->mutex);state->result={};state->result.error=8;}
    if(!state->handshake_done.load()){finish_handshake();if(curl)curl_easy_cleanup(curl);return;}
    // 升級後的訊框迴圈：持續讀入（受接收上限節流）、寫出佇列、回覆 ping、保活 pong，雙方 close 後結束。
    WebSocketParser parser;auto last_send=std::chrono::steady_clock::now();
    try{
        {std::lock_guard lock(state->mutex);parser.feed(*state,buffer);}buffer.clear();
        std::vector<std::uint8_t> chunk(65536);
        while(!state->cancel.load()){
            bool want_read=false,want_write=false;
            {
                std::lock_guard lock(state->mutex);auto& s=*state;
                if(s.outbound_offset && s.outbound_offset*2>=s.outbound.size()){s.outbound.erase(s.outbound.begin(),s.outbound.begin()+static_cast<std::ptrdiff_t>(s.outbound_offset));s.outbound_offset=0;}
                const auto now=std::chrono::steady_clock::now();
                if(s.keepalive && !s.close_sent && !s.error && s.outbound_offset==s.outbound.size() && std::chrono::duration_cast<std::chrono::milliseconds>(now-last_send).count()>=s.keepalive){s.append_frame(10,true,{});}
                want_write=s.outbound_offset<s.outbound.size();
                want_read=!s.error && !parser.stopped && s.inbound_bytes<websocket_inbound_limit;
                if(!want_write && ((s.close_sent && s.close_received) || s.error))break;
            }
            if(want_read){
                for(;;){
                    size_t got=0;const auto code=curl_easy_recv(curl,chunk.data(),chunk.size(),&got);
                    if(code==CURLE_AGAIN)break;
                    if(code!=CURLE_OK){std::lock_guard lock(state->mutex);if(!state->error)state->error=12030;break;}
                    if(!got){std::lock_guard lock(state->mutex);if(!state->close_received && !state->error)state->error=12030;parser.stopped=true;break;}
                    count_io(*state,true,got);std::lock_guard lock(state->mutex);parser.feed(*state,{chunk.data(),got});
                    if(parser.stopped || state->error || state->inbound_bytes>=websocket_inbound_limit)break;
                }
            }
            if(want_write){
                for(;;){
                    std::unique_lock lock(state->mutex);auto& s=*state;if(s.outbound_offset>=s.outbound.size())break;
                    const auto* data=s.outbound.data()+s.outbound_offset;const auto size=s.outbound.size()-s.outbound_offset;
                    size_t sent=0;const auto code=curl_easy_send(curl,data,size,&sent);
                    if(code==CURLE_AGAIN)break;if(code!=CURLE_OK){if(!s.error)s.error=12030;s.outbound_offset=s.outbound.size();s.written=s.queued;break;}
                    s.outbound_offset+=sent;s.written+=sent;lock.unlock();count_io(*state,false,sent);last_send=std::chrono::steady_clock::now();state->notify();
                }
            }
            // 每輪開頭都先直接讀到 CURLE_AGAIN，接收上限解除後也會讀到 TLS 層已緩衝的資料；這裡只決定 poll 的事件。
            {std::lock_guard lock(state->mutex);auto& s=*state;
             want_write=s.outbound_offset<s.outbound.size();want_read=!s.error && !parser.stopped && s.inbound_bytes<websocket_inbound_limit;
             if(!want_write && ((s.close_sent && s.close_received) || s.error))break;}
            int timeout=1000;
            {std::lock_guard lock(state->mutex);if(state->keepalive && !state->close_sent){const auto left=static_cast<std::int64_t>(state->keepalive)-std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-last_send).count();timeout=static_cast<int>(std::clamp<std::int64_t>(left,1,1000));}}
            pollfd fds[2]{{socket,static_cast<short>((want_read?POLLIN:0)|(want_write?POLLOUT:0)),0},{state->wake[0],POLLIN,0}};
            if(poll(fds,state->wake[0]>=0?2:1,timeout)<0 && errno!=EINTR){std::lock_guard lock(state->mutex);if(!state->error)state->error=12030;}
            if(state->wake[0]>=0 && (fds[1].revents&POLLIN))state->drain();
        }
    }catch(...){std::lock_guard lock(state->mutex);if(!state->error)state->error=12030;}
    {std::lock_guard lock(state->mutex);state->finished=true;if(state->cancel.load() && !state->error && !state->close_received)state->error=12017;}
    state->notify();curl_easy_cleanup(curl);
}
}
std::shared_ptr<NativeHttp> start_native_http(HttpInput input){
    static std::once_flag initialized;std::call_once(initialized,[]{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::bad_alloc();});
    if(input.websocket){
        auto state=std::make_shared<WebSocketState>();if(state->wake[0]<0)return {};auto result=std::make_shared<UpgradeRequest>(state);
        std::thread([state,input=std::move(input)]{run_websocket(state,input);}).detach();return result;
    }
    auto state=std::make_shared<Transfer>();state->send_timeout=input.timeouts[2];state->receive_timeout=input.timeouts[3];auto result=std::make_shared<Request>(state);
    std::thread([state,input=std::move(input)]{
        CURL* curl=curl_easy_init();curl_slist* headers=nullptr;
        try{
            if(!curl)throw std::bad_alloc();state->handle=curl;const auto original=parse_url(input.url);
            // 每次重新導向重新選擇代理與 bypass（configure_proxy），並關閉宿主環境的代理設定。
            configure_connection(curl,input);curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
            const char* encoding=input.decompression==3?"gzip, deflate":input.decompression==1?"gzip":input.decompression==2?"deflate":nullptr;
            curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,encoding);
            curl_easy_setopt(curl,CURLOPT_FORBID_REUSE,(input.disabled_features&8U)?1L:0L);
            curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
            curl_easy_setopt(curl,CURLOPT_VERBOSE,1L);
            curl_easy_setopt(curl,CURLOPT_DEBUGFUNCTION,io_callback);curl_easy_setopt(curl,CURLOPT_DEBUGDATA,state.get());
            curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,body_callback);curl_easy_setopt(curl,CURLOPT_WRITEDATA,state.get());
            curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,header_callback);curl_easy_setopt(curl,CURLOPT_HEADERDATA,state.get());
            curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress_callback);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,state.get());curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
            std::string url=input.url,method=input.method,previous_scheme=original.scheme;bool send_body=true;
            for(unsigned redirects=0;;++redirects){
                const auto parsed=parse_url(url);if(input.redirect_policy==1 && previous_scheme=="https" && parsed.scheme!="https")throw HttpError{12156};
                const bool same_origin=parsed.origin()==original.origin();state->secure=parsed.scheme=="https";
                state->result={};state->receiving=state->connected=false;state->download=state->upload=0;state->progress=std::chrono::steady_clock::now();
                curl_easy_setopt(curl,CURLOPT_HTTPHEADER,nullptr);curl_slist_free_all(headers);headers=nullptr;
                for(const auto& [name,value]:input.headers){
                    const auto key=lower(name);if(!same_origin && (key=="authorization" || key=="cookie" || key=="host" || key=="proxy-authorization"))continue;
                    if((input.decompression && key=="accept-encoding") || ((input.disabled_features&8U) && key=="connection"))continue;
                    if(!send_body && (key=="content-length" || key=="content-type" || key=="transfer-encoding"))continue;
                    auto* next=curl_slist_append(headers,(name+": "+value).c_str());if(!next)throw std::bad_alloc();headers=next;
                }
                if(input.disabled_features&8U){auto* next=curl_slist_append(headers,"Connection: close");if(!next)throw std::bad_alloc();headers=next;}
                configure_proxy(curl,input,parsed);
                curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
                const bool server_auth=same_origin && !(input.disabled_features&4U) && (input.credentials_set || !input.username.empty());
                curl_easy_setopt(curl,CURLOPT_USERNAME,server_auth?input.username.c_str():nullptr);
                curl_easy_setopt(curl,CURLOPT_PASSWORD,server_auth?input.password.c_str():nullptr);
                curl_easy_setopt(curl,CURLOPT_HTTPAUTH,server_auth?curl_auth(input.auth_scheme):static_cast<long>(CURLAUTH_NONE));
                curl_easy_setopt(curl,CURLOPT_POSTFIELDS,nullptr);curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);curl_easy_setopt(curl,CURLOPT_NOBODY,method=="HEAD"?1L:0L);
                if(method!="HEAD" && send_body && (!input.body.empty() || method=="POST" || method=="PUT")){
                    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,input.body.empty()?"":reinterpret_cast<const char*>(input.body.data()));curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(input.body.size()));}
                curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,method.c_str());
                const auto code=curl_easy_perform(curl);if(!state->result.error)state->result.error=curl_error(code);
                long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);state->result.status=static_cast<std::uint32_t>(status);
                char* effective=nullptr;curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);if(effective)state->result.effective_url=effective;
                if(std::getenv("X64ARM_TRACE_HTTP"))std::fprintf(stderr,"[http] pid=%d host=%s status=%ld error=%u response_bytes=%zu certificate_exceptions=%x\n",getpid(),parsed.host.c_str(),status,state->result.error,state->result.body.size(),input.certificate_exceptions);
                if(state->result.error || !input.follow_redirects || !input.redirect_policy || (input.disabled_features&2U))break;char* next=nullptr;curl_easy_getinfo(curl,CURLINFO_REDIRECT_URL,&next);if(!next)break;
                if(redirects>=input.max_redirects)throw HttpError{12156};previous_scheme=parsed.scheme;url=next;
                if(((status==301 || status==302) && method=="POST") || (status==303 && method!="HEAD")){method="GET";send_body=false;}
            }
        }catch(const HttpError& error){state->result.error=error.code;}catch(...){state->result.error=8;}
        curl_slist_free_all(headers);if(curl)curl_easy_cleanup(curl);
        {std::lock_guard lock(state->wait_mutex);state->done.store(true,std::memory_order_release);}state->finished.notify_all();
    }).detach();return result;
}
}
