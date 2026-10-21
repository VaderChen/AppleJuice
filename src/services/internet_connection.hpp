#pragma once
#include "x64arm/http.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <unicode/uidna.h>

namespace x64arm::internet_connection {
inline std::optional<std::string> url_host(std::string_view url){
    const auto scheme=url.find("://");if(!scheme || scheme==url.npos)return {};
    for(std::size_t i=0;i<scheme;++i){const auto c=static_cast<unsigned char>(url[i]);
        const bool letter=(c>='a' && c<='z') || (c>='A' && c<='Z');if(!letter && (!i || !((c>='0' && c<='9') || c=='+' || c=='-' || c=='.')))return {};
    }
    auto authority=url.substr(scheme+3,url.find_first_of("/?#",scheme+3)-(scheme+3));
    if(const auto user=authority.rfind('@');user!=authority.npos)authority.remove_prefix(user+1);
    if(authority.empty())return {};std::string host;std::string_view port;
    if(authority.front()=='['){const auto close=authority.find(']');if(close==authority.npos || close==1)return {};host=authority.substr(1,close-1);
        if(close+1<authority.size()){if(authority[close+1]!=':' || close+2==authority.size())return {};port=authority.substr(close+2);}const auto zone=host.find("%25");if(zone!=host.npos)host.replace(zone,3,"%");
        const auto address=host.substr(0,host.find('%'));in6_addr parsed{};if(::inet_pton(AF_INET6,address.c_str(),&parsed)!=1)return {};
    }else {const auto colon=authority.find(':');if(colon!=authority.npos){if(authority.find(':',colon+1)!=authority.npos || colon+1==authority.size())return {};port=authority.substr(colon+1);authority=authority.substr(0,colon);}host=authority;}
    unsigned number=0;for(const auto c:port){if(c<'0' || c>'9' || number>6553)return {};number=number*10+static_cast<unsigned>(c-'0');}if(number>65535 || (!port.empty() && !number))return {};
    if(host.empty() || host.size()>1024 || host.find_first_of(" \t\r\n\\")!=host.npos || host.find('\0')!=host.npos)return {};
    if(std::any_of(host.begin(),host.end(),[](unsigned char c){return c<=32 || c==127;}))return {};
    if(host.find(':')==host.npos){
        UErrorCode status=U_ZERO_ERROR;std::unique_ptr<UIDNA,decltype(&uidna_close)> idna(uidna_openUTS46(UIDNA_CHECK_BIDI|UIDNA_CHECK_CONTEXTJ|UIDNA_NONTRANSITIONAL_TO_ASCII,&status),uidna_close);
        if(U_FAILURE(status) || !idna)return {};UIDNAInfo info=UIDNA_INFO_INITIALIZER;std::array<char,1024> ascii{};
        const auto size=uidna_nameToASCII_UTF8(idna.get(),host.data(),static_cast<std::int32_t>(host.size()),ascii.data(),static_cast<std::int32_t>(ascii.size()),&info,&status);
        if(U_FAILURE(status) || info.errors || size<=0 || size>255)return {};host.assign(ascii.data(),static_cast<std::size_t>(size));
    }return host;
}
inline std::uint16_t checksum(std::span<const std::uint8_t> bytes){
    std::uint32_t sum=0;for(std::size_t i=0;i<bytes.size();i+=2)sum+=(std::uint32_t{bytes[i]}<<8)|(i+1<bytes.size()?bytes[i+1]:0);
    while(sum>>16)sum=(sum&65535U)+(sum>>16);return static_cast<std::uint16_t>(~sum);
}
inline bool echo_reply(std::span<const std::uint8_t> reply,std::span<const std::uint8_t> sent,bool ipv6){
    if(!ipv6 && !reply.empty() && (reply[0]>>4)==4){const auto header=(reply[0]&15U)*4U;if(header<20 || header>reply.size())return false;reply=reply.subspan(header);}
    if(reply.size()<sent.size() || reply[0]!=(ipv6?129U:0U) || reply[1] || reply[6]!=sent[6] || reply[7]!=sent[7])return false;
    // Datagam ICMP 的 identifier 可由核心改寫；以整個隨機 payload 配對回覆。
    return std::equal(sent.begin()+8,sent.end(),reply.begin()+8);
}
class Probe final:public NativeHttp {
public:
    explicit Probe(std::string host):worker_([this,host=std::move(host)]{run(host);}){}
    ~Probe() override{cancel();if(worker_.joinable())worker_.join();}
    bool done() const override{return done_.load(std::memory_order_acquire);}
    HttpResult take() override{if(!done()){HttpResult result;result.error=995;return result;}return result_;}
    void cancel() noexcept override{canceled_.store(true,std::memory_order_relaxed);}
    process_io::Counters io_counters() const override{process_io::Counters result;for(unsigned i=0;i<6;++i)result.values[i]=io_[i].load(std::memory_order_relaxed);return result;}
private:
    // 共用 NativeHttp 的完成／取消／I/O 計數介面；不產生 HTTP 要求或偽造 HTTP 狀態。
    std::atomic<bool> done_{},canceled_{};std::array<std::atomic<std::uint64_t>,6> io_{};
    HttpResult result_=[]{HttpResult result;result.error=2250;return result;}();std::thread worker_;
    void run(const std::string& host) noexcept{
        try{
            addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_DGRAM;addrinfo* addresses=nullptr;
            const auto error=::getaddrinfo(host.c_str(),nullptr,&hints,&addresses);
            std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> resolved(addresses,freeaddrinfo);
            if(!error && !canceled_.load()){
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);unsigned count=0;
                for(auto item=addresses;item && count++<32 && !canceled_.load() && std::chrono::steady_clock::now()<deadline;item=item->ai_next){
                    if(item->ai_family!=AF_INET && item->ai_family!=AF_INET6)continue;
                    if(ping(*item,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(500)))){result_.error=0;break;}
                }
            }
            if(canceled_.load())result_.error=995;
        }catch(...){result_.error=2250;}
        done_.store(true,std::memory_order_release);
    }
    bool ping(const addrinfo& target,std::chrono::steady_clock::time_point deadline){
        const bool ipv6=target.ai_family==AF_INET6;
        struct Socket{int fd;~Socket(){if(fd>=0)::close(fd);}} socket{::socket(target.ai_family,SOCK_DGRAM,ipv6?IPPROTO_ICMPV6:IPPROTO_ICMP)};
        if(socket.fd<0)return false;(void)::fcntl(socket.fd,F_SETFD,FD_CLOEXEC);const auto flags=::fcntl(socket.fd,F_GETFL,0);
        if(flags<0 || ::fcntl(socket.fd,F_SETFL,flags|O_NONBLOCK)<0 || ::connect(socket.fd,target.ai_addr,target.ai_addrlen)<0)return false;
        std::array<std::uint8_t,40> packet{};packet[0]=ipv6?128U:8U;packet[6]=0x12;packet[7]=0x34;
        if(RAND_bytes(packet.data()+8,static_cast<int>(packet.size()-8))!=1)return false;packet[4]=packet[8];packet[5]=packet[9];
        if(!ipv6){const auto sum=checksum(packet);packet[2]=static_cast<std::uint8_t>(sum>>8);packet[3]=static_cast<std::uint8_t>(sum);}
        const auto sent=::send(socket.fd,packet.data(),packet.size(),0);if(sent!=static_cast<ssize_t>(packet.size()))return false;
        io_[1].fetch_add(1);io_[4].fetch_add(packet.size());
        while(!canceled_.load() && std::chrono::steady_clock::now()<deadline){
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            pollfd input{socket.fd,POLLIN,0};const auto ready=::poll(&input,1,static_cast<int>(std::clamp<std::int64_t>(left,1,50)));
            if(ready<0){if(errno==EINTR)continue;return false;}if(!ready)continue;
            std::array<std::uint8_t,2048> response{};const auto size=::recv(socket.fd,response.data(),response.size(),0);
            if(size<0){if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)continue;return false;}
            if(!size)return false;io_[0].fetch_add(1);io_[3].fetch_add(static_cast<std::uint64_t>(size));
            if(echo_reply(std::span(response).first(static_cast<std::size_t>(size)),packet,ipv6))return true;
        }return false;
    }
};
}
