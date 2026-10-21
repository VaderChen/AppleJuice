#pragma once
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <vector>

namespace x64arm::socket_detail {
// 只存在於捕捉的 host-side 請求。真正 sendmsg 前移除，不傳給核心／客體。
inline constexpr int send_ecn_control4=0x58415334,send_ecn_control6=0x58415336;
inline bool prepare_send_control(int descriptor,int type,const msghdr& original,
                                 std::vector<std::uint8_t>& converted,bool& changed)try{
    changed=false;if(original.msg_controllen && !original.msg_control){errno=EFAULT;return false;}
    const auto* bytes=static_cast<const std::uint8_t*>(original.msg_control);const auto count=std::size_t(original.msg_controllen);
    if(count>65536){errno=EINVAL;return false;}
    std::array<int,2> ecn{-1,-1};
    const auto visit=[&](const auto& callback){
        for(std::size_t at=0;at<count;){
            if(count-at<sizeof(cmsghdr)){errno=EINVAL;return false;}cmsghdr header{};std::memcpy(&header,bytes+at,sizeof header);
            if(header.cmsg_len<CMSG_LEN(0) || header.cmsg_len>count-at){errno=EINVAL;return false;}
            const auto size=std::size_t(header.cmsg_len-CMSG_LEN(0));if(!callback(at,header,size))return false;
            const auto next=std::size_t(CMSG_SPACE(size));if(next>count-at){if(header.cmsg_len!=count-at){errno=EINVAL;return false;}break;}at+=next;
        }return true;
    };
    if(!visit([&](std::size_t at,const cmsghdr& header,std::size_t size){
        if(header.cmsg_level!=SOL_SOCKET || (header.cmsg_type!=send_ecn_control4 && header.cmsg_type!=send_ecn_control6))return true;
        const unsigned slot=header.cmsg_type==send_ecn_control4?0U:1U;int value{};
        if(size!=4 || ecn[slot]>=0){errno=EINVAL;return false;}std::memcpy(&value,bytes+at+CMSG_LEN(0),4);
        if(value<0 || value>2){errno=EINVAL;return false;}ecn[slot]=value;return true;
    }))return false;
    if(ecn[0]<0 && ecn[1]<0)return true;
    if(type!=SOCK_DGRAM){errno=EOPNOTSUPP;return false;}
    sockaddr_storage endpoint{};socklen_t length=sizeof endpoint;
    if(original.msg_name){if(original.msg_namelen<sizeof(sockaddr) || original.msg_namelen>sizeof endpoint){errno=EINVAL;return false;}std::memcpy(&endpoint,original.msg_name,original.msg_namelen);length=original.msg_namelen;}
    else if(::getpeername(descriptor,reinterpret_cast<sockaddr*>(&endpoint),&length)<0)return false;
    bool ipv4;
    if(endpoint.ss_family==AF_INET && length>=sizeof(sockaddr_in))ipv4=true;
    else if(endpoint.ss_family==AF_INET6 && length>=sizeof(sockaddr_in6))ipv4=IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<const sockaddr_in6*>(&endpoint)->sin6_addr);
    else{errno=EAFNOSUPPORT;return false;}
    const unsigned slot=ipv4?0U:1U;if(ecn[slot]<0){errno=EINVAL;return false;}
    // 取得本次發送的真正預設值；不用 setsockopt 暫改共享 socket。
    int baseline{};socklen_t size=sizeof baseline;const int level=ipv4?IPPROTO_IP:IPPROTO_IPV6,option=ipv4?IP_TOS:IPV6_TCLASS;
    if(::getsockopt(descriptor,level,option,&baseline,&size)<0)return false;
    if(size!=sizeof baseline || baseline<0 || baseline>255){errno=EPROTO;return false;}
    converted.clear();converted.reserve(count+CMSG_SPACE(4));bool full_value=false;
    if(!visit([&](std::size_t at,const cmsghdr& header,std::size_t body){
        if(header.cmsg_level==SOL_SOCKET && (header.cmsg_type==send_ecn_control4 || header.cmsg_type==send_ecn_control6))return true;
        // 雙堆疊的完整 TOS／class 也依實際封包家族選擇；XNU 的
        // IPv4 路徑會接受兩種 level，不能讓另一家族的值搶先覆蓋。
        if((header.cmsg_level==IPPROTO_IP && header.cmsg_type==IP_TOS) ||
           (header.cmsg_level==IPPROTO_IPV6 && header.cmsg_type==IPV6_TCLASS)){
            if(header.cmsg_level!=level)return true;
        }
        const auto start=converted.size(),space=std::size_t(CMSG_SPACE(static_cast<unsigned>(body)));converted.resize(start+space,0);
        std::memcpy(converted.data()+start,bytes+at,header.cmsg_len);
        if(header.cmsg_level==level && header.cmsg_type==option){
            int value{};if(body!=4){errno=EINVAL;return false;}std::memcpy(&value,bytes+at+CMSG_LEN(0),4);
            if(value<-1 || value>255){errno=EINVAL;return false;}if(value==-1)value=baseline;
            value=(value&~3)|ecn[slot];std::memcpy(converted.data()+start+CMSG_LEN(0),&value,4);full_value=true;
        }return true;
    }))return false;
    if(!full_value){
        const auto at=converted.size();converted.resize(at+CMSG_SPACE(4),0);cmsghdr header{};header.cmsg_level=level;header.cmsg_type=option;header.cmsg_len=CMSG_LEN(4);
        std::memcpy(converted.data()+at,&header,sizeof header);const int value=(baseline&~3)|ecn[slot];std::memcpy(converted.data()+at+CMSG_LEN(0),&value,4);
    }
    changed=true;return true;
}catch(const std::bad_alloc&){errno=ENOMEM;return false;}
} // namespace x64arm::socket_detail
