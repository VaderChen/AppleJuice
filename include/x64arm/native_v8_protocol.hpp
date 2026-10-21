#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>
#include <unistd.h>

namespace x64arm::native_v8 {
// 模組協定只傳整數 token 與資料；V8 指標、tagged value、handle slot 不跨邊界。
constexpr std::uint32_t magic=0x38564158,version=1,limit=16U*1024U*1024U;
enum class Kind:std::uint32_t { Request=1, Response=2, Callback=3, CallbackReturn=4 };
enum class Op:std::uint32_t { Version=0, Create=1, Dispose=2, Eval=3, Retain=4, Release=5,
    String=6, Get=7, Set=8, Call=9, Function=10, Equal=11, Collect=12, Microtasks=13,
    ReadBuffer=14, WriteBuffer=15 };
enum class Status:std::uint32_t { Ok=0, Exception=1, Invalid=2, Thread=3, Busy=4, Capacity=5 };
struct Header {
    std::uint32_t magic_value{magic},protocol{version};Kind kind{Kind::Request};
    std::uint32_t sequence{};Op op{};std::uint32_t isolate{},handle{},aux{};
    Status status{Status::Ok};std::uint32_t length{};
};
static_assert(sizeof(Header)==40);
struct Packet {Header header;std::vector<std::uint8_t> data;};
inline void transfer(int fd,void* data,std::size_t length,bool writing){
    auto* bytes=static_cast<std::uint8_t*>(data);
    while(length){const auto n=writing ? ::write(fd,bytes,length) : ::read(fd,bytes,length);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)throw std::runtime_error("原生 V8 模組通道已關閉");
        bytes+=n;length-=static_cast<std::size_t>(n);
    }
}
inline void send(int fd,Packet packet){
    if(packet.data.size()>limit)throw std::runtime_error("V8 封包超過上限");
    packet.header.length=static_cast<std::uint32_t>(packet.data.size());
    transfer(fd,&packet.header,sizeof(Header),true);
    if(!packet.data.empty())transfer(fd,packet.data.data(),packet.data.size(),true);
}
inline Packet receive(int fd){
    Packet packet;transfer(fd,&packet.header,sizeof(Header),false);
    if(packet.header.magic_value!=magic || packet.header.protocol!=version || packet.header.length>limit)
        throw std::runtime_error("V8 模組協定錯誤");
    packet.data.resize(packet.header.length);
    if(!packet.data.empty())transfer(fd,packet.data.data(),packet.data.size(),false);
    return packet;
}
inline std::vector<std::uint8_t> bytes(const std::string& s){return {s.begin(),s.end()};}
inline std::string text(const Packet& p){return {p.data.begin(),p.data.end()};}
inline std::uint32_t word(const std::vector<std::uint8_t>& data,std::size_t at){
    if(at+4>data.size())throw std::runtime_error("V8 參數陣列長度無效");
    std::uint32_t value;std::memcpy(&value,data.data()+at,4);return value;
}
inline void append(std::vector<std::uint8_t>& data,std::uint32_t value){
    const auto size=data.size();data.resize(size+4);std::memcpy(data.data()+size,&value,4);
}
}
