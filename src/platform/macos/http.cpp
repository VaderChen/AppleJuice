#include "x64arm/http.hpp"
#include "x64arm/resource_usage.hpp"
#include <curl/curl.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace x64arm {
namespace {
struct HttpError {std::uint32_t code;};
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
class Request final:public NativeHttp {
public:
    explicit Request(std::shared_ptr<Transfer> state):state_(std::move(state)){}
    ~Request() override {cancel();}
    void cancel() noexcept override {state_->cancel.store(true);}
    process_io::Counters io_counters() const override {std::lock_guard lock(state_->io_mutex);return state_->io;}
    bool done() const override {return state_->done.load(std::memory_order_acquire);}
    HttpResult take() override {if(!done())throw std::logic_error("HTTP 尚未完成");return std::move(state_->result);}
private:std::shared_ptr<Transfer> state_;
};
}
std::shared_ptr<NativeHttp> start_native_http(HttpInput input){
    static std::once_flag initialized;std::call_once(initialized,[]{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::bad_alloc();});
    auto state=std::make_shared<Transfer>();state->send_timeout=input.timeouts[2];state->receive_timeout=input.timeouts[3];auto result=std::make_shared<Request>(state);
    std::thread([state,input=std::move(input)]{
        CURL* curl=curl_easy_init();curl_slist* headers=nullptr;
        try{
            if(!curl)throw std::bad_alloc();state->handle=curl;const auto original=parse_url(input.url);
            // 每次重新導向重新選擇代理與 bypass，並關閉宿主環境的代理設定。
            curl_easy_setopt(curl,CURLOPT_NOPROXY,"");
            curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
            if(input.secure_protocols){
                constexpr unsigned masks[]{0x80,0x200,0x800,0x2000};
                constexpr long versions[]{CURL_SSLVERSION_TLSv1_0,CURL_SSLVERSION_TLSv1_1,CURL_SSLVERSION_TLSv1_2,CURL_SSLVERSION_TLSv1_3};
                long first=0,last=0;for(unsigned i=0;i<4;++i)if(input.secure_protocols&masks[i]){if(!first)first=versions[i];last=versions[i];}
                if(curl_easy_setopt(curl,CURLOPT_SSLVERSION,first|(last<<16))!=CURLE_OK)throw HttpError{12175};
            }
            const char* encoding=input.decompression==3?"gzip, deflate":input.decompression==1?"gzip":input.decompression==2?"deflate":nullptr;
            curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,encoding);
            curl_easy_setopt(curl,CURLOPT_FORBID_REUSE,(input.disabled_features&8U)?1L:0L);
            curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl,CURLOPT_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
            const auto connect=input.timeouts[1]>0?input.timeouts[1]:INT32_MAX;
            curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,static_cast<long>(input.timeouts[0]>0?std::min(input.timeouts[0],connect):connect));
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
                const auto proxy=proxy_for_url(input,parsed);curl_easy_setopt(curl,CURLOPT_PROXY,proxy.c_str());
                curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
                curl_easy_setopt(curl,CURLOPT_USERNAME,same_origin && !(input.disabled_features&4U) && !input.username.empty()?input.username.c_str():nullptr);
                curl_easy_setopt(curl,CURLOPT_PASSWORD,same_origin && !(input.disabled_features&4U) && !input.password.empty()?input.password.c_str():nullptr);curl_easy_setopt(curl,CURLOPT_HTTPAUTH,(input.disabled_features&4U)?CURLAUTH_NONE:CURLAUTH_ANY);
                curl_easy_setopt(curl,CURLOPT_POSTFIELDS,nullptr);curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);curl_easy_setopt(curl,CURLOPT_NOBODY,method=="HEAD"?1L:0L);
                if(method!="HEAD" && send_body && (!input.body.empty() || method=="POST" || method=="PUT")){
                    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,input.body.empty()?"":reinterpret_cast<const char*>(input.body.data()));curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(input.body.size()));}
                curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,method.c_str());
                const auto code=curl_easy_perform(curl);if(!state->result.error)state->result.error=curl_error(code);
                long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);state->result.status=static_cast<std::uint32_t>(status);
                char* effective=nullptr;curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);if(effective)state->result.effective_url=effective;
                if(state->result.error || !input.follow_redirects || !input.redirect_policy || (input.disabled_features&2U))break;char* next=nullptr;curl_easy_getinfo(curl,CURLINFO_REDIRECT_URL,&next);if(!next)break;
                if(redirects>=input.max_redirects)throw HttpError{12156};previous_scheme=parsed.scheme;url=next;
                if(((status==301 || status==302) && method=="POST") || (status==303 && method!="HEAD")){method="GET";send_body=false;}
            }
        }catch(const HttpError& error){state->result.error=error.code;}catch(...){state->result.error=8;}
        curl_slist_free_all(headers);if(curl)curl_easy_cleanup(curl);state->done.store(true,std::memory_order_release);
    }).detach();return result;
}
}
