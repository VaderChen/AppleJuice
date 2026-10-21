#include "../services/iphelper_legacy.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <arpa/inet.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif
namespace x64arm::iphelper {
namespace {
struct Socket {int fd;~Socket(){if(fd>=0)::close(fd);}};
std::uint32_t error(){switch(errno){case EACCES:case EPERM:return 5;case EADDRINUSE:return 32;case ENOMEM:case ENOBUFS:return 8;case ENETUNREACH:case EHOSTUNREACH:return 1231;default:return 31;}}
}
std::uint32_t dhcp_inform(const NetworkInterface& interface,std::span<const std::uint8_t> requested,const std::map<std::uint8_t,std::vector<std::uint8_t>>& send,std::span<const std::uint8_t> class_id,std::map<std::uint8_t,std::vector<std::uint8_t>>& result){
    if(!interface.dhcp_enabled || interface.dhcp_server==std::array<std::uint8_t,4>{})return 232;
    const auto address=std::find_if(interface.addresses.begin(),interface.addresses.end(),[](const auto& value){return value.bytes.size()==16 && value.bytes[0]==2;});if(address==interface.addresses.end())return 232;
    if(interface.physical.size()!=6)return 50;
    Socket socket{::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)};if(socket.fd<0)return error();int enabled=1;
    if(::setsockopt(socket.fd,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled)))return error();
#ifdef SO_REUSEPORT
    if(::setsockopt(socket.fd,SOL_SOCKET,SO_REUSEPORT,&enabled,sizeof(enabled)))return error();
#endif
#ifdef IP_BOUND_IF
    const auto index=interface.index;if(::setsockopt(socket.fd,IPPROTO_IP,IP_BOUND_IF,&index,sizeof(index)))return error();
#endif
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(68);std::memcpy(&local.sin_addr,address->bytes.data()+4,4);if(::bind(socket.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local)))return error();
    sockaddr_in server{};server.sin_family=AF_INET;server.sin_port=htons(67);std::memcpy(&server.sin_addr,interface.dhcp_server.data(),4);
    std::vector<std::uint8_t> packet(240);packet[0]=1;packet[1]=1;packet[2]=6;
    if(::getentropy(packet.data()+4,4))return error();std::copy_n(address->bytes.begin()+4,4,packet.begin()+12);std::copy(interface.physical.begin(),interface.physical.end(),packet.begin()+28);packet[236]=99;packet[237]=130;packet[238]=83;packet[239]=99;
    const auto option=[&](std::uint8_t code,std::span<const std::uint8_t> bytes){if(bytes.empty() || bytes.size()>255 || packet.size()+2+bytes.size()>1471)return false;packet.push_back(code);packet.push_back(static_cast<std::uint8_t>(bytes.size()));packet.insert(packet.end(),bytes.begin(),bytes.end());return true;};
    const std::array<std::uint8_t,1> type{8};if(!option(53,type))return 87;if(!requested.empty() && !option(55,requested))return 87;
    std::vector<std::uint8_t> client{1};client.insert(client.end(),interface.physical.begin(),interface.physical.end());if(!option(61,client))return 87;
    if(!class_id.empty() && !option(60,class_id))return 87;for(const auto& [code,data]:send)if(!option(code,data))return 87;packet.push_back(255);if(packet.size()<300)packet.resize(300);
    for(unsigned attempt=0;attempt<3;++attempt){if(::sendto(socket.fd,packet.data(),packet.size(),0,reinterpret_cast<sockaddr*>(&server),sizeof(server))!=static_cast<ssize_t>(packet.size()))return error();
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        while(std::chrono::steady_clock::now()<end){const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count();pollfd event{socket.fd,POLLIN,0};const int polled=::poll(&event,1,static_cast<int>(std::max<std::int64_t>(1,remaining)));if(polled<0){if(errno==EINTR)continue;return error();}if(!polled)break;
            std::array<std::uint8_t,65536> bytes{};sockaddr_in peer{};socklen_t size=sizeof(peer);const auto count=::recvfrom(socket.fd,bytes.data(),bytes.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&peer),&size);if(count<0){if(errno==EAGAIN || errno==EINTR)continue;return error();}
            if(count<240 || peer.sin_port!=server.sin_port || peer.sin_addr.s_addr!=server.sin_addr.s_addr || bytes[0]!=2 || bytes[1]!=1 || bytes[2]!=6 || !std::equal(packet.begin()+4,packet.begin()+8,bytes.begin()+4) || !std::equal(interface.physical.begin(),interface.physical.end(),bytes.begin()+28) || !std::equal(packet.begin()+236,packet.begin()+240,bytes.begin()+236))continue;
            std::map<std::uint8_t,std::vector<std::uint8_t>> options;bool valid=true;for(std::size_t at=240;at<static_cast<std::size_t>(count);){const auto code=bytes[at++];if(!code)continue;if(code==255)break;if(at>=static_cast<std::size_t>(count)){valid=false;break;}const auto length=bytes[at++];if(length>static_cast<std::size_t>(count)-at){valid=false;break;}auto& value=options[code];value.insert(value.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+length));at+=length;}
            if(valid && options[53]==std::vector<std::uint8_t>{5}){result=std::move(options);return 0;}
        }
    }return 1460;
}
}
