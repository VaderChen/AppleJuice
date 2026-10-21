#pragma once
#include "socket_state_access.hpp"
#include <array>
#include <cstring>
#include <sys/socket.h>
#include <vector>

namespace x64arm::socket_detail {
// 使用共享檔案的保留區，不擴大既有 SharedSocketData ABI。所有操作須持有
// SocketStateAccess；封包內另存接受時的選項，之後切換不改寫已提交資料。
inline constexpr off_t receive_options_offset=256;
inline constexpr int receive_policy_control=0x58415250;
inline constexpr std::uint32_t receive_ecn4=0x40,receive_ecn6=0x80,receive_options_mask=0xff;
inline bool receive_options_read(int fd,std::uint32_t& flags){
    std::array<std::uint32_t,8> record{};ssize_t count;
    do{count=::pread(fd,record.data(),sizeof record,receive_options_offset);}while(count<0 && errno==EINTR);
    if(count<0)return false;
    if(count!=sizeof record){errno=EPROTO;return false;}
    if(record==std::array<std::uint32_t,8>{}){flags=0;return true;}
    if(record[0]!=0x52585031U || record[1]!=1 || (record[2]&~receive_options_mask) || record[3]!=~record[2] ||
       record[4] || record[5] || record[6] || record[7]){errno=EPROTO;return false;}
    flags=record[2];return true;
}
inline bool receive_options_write(int fd,std::uint32_t flags){
    if(flags&~receive_options_mask){errno=EINVAL;return false;}
    const std::array<std::uint32_t,8> record{0x52585031U,1,flags,~flags,0,0,0,0};ssize_t count;
    do{count=::pwrite(fd,record.data(),sizeof record,receive_options_offset);}while(count<0 && errno==EINTR);
    if(count!=sizeof record){if(count>=0)errno=EIO;return false;}return true;
}
inline void receive_policy_append(std::vector<std::uint8_t>& bytes,std::uint32_t flags){
    const auto at=bytes.size();bytes.resize(at+CMSG_SPACE(8),0);cmsghdr header{};
    header.cmsg_len=CMSG_LEN(8);header.cmsg_level=SOL_SOCKET;header.cmsg_type=receive_policy_control;
    std::memcpy(bytes.data()+at,&header,sizeof header);const std::array<std::uint32_t,2> policy{flags,~flags};
    std::memcpy(bytes.data()+at+CMSG_LEN(0),policy.data(),8);
}
inline bool receive_policy_read(const msghdr& message,std::uint32_t& flags){
    if(message.msg_controllen && !message.msg_control){errno=EFAULT;return false;}
    const auto* bytes=static_cast<const std::uint8_t*>(message.msg_control);const auto count=std::size_t(message.msg_controllen);bool found=false;
    for(std::size_t at=0;at<count;){
        if(count-at<sizeof(cmsghdr)){errno=EPROTO;return false;}cmsghdr header{};std::memcpy(&header,bytes+at,sizeof header);
        if(header.cmsg_len<CMSG_LEN(0) || header.cmsg_len>count-at){errno=EPROTO;return false;}
        const auto size=header.cmsg_len-CMSG_LEN(0);
        if(header.cmsg_level==SOL_SOCKET && header.cmsg_type==receive_policy_control){
            std::array<std::uint32_t,2> policy{};
            if(found || size!=8){errno=EPROTO;return false;}std::memcpy(policy.data(),bytes+at+CMSG_LEN(0),8);
            if((policy[0]&~receive_options_mask) || policy[1]!=~policy[0]){errno=EPROTO;return false;}flags=policy[0];found=true;
        }
        const auto next=std::size_t(CMSG_SPACE(size));if(next>count-at){if(header.cmsg_len!=count-at){errno=EPROTO;return false;}break;}at+=next;
    }
    return true;
}
} // namespace x64arm::socket_detail
