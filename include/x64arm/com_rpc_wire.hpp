#pragma once
#include "x64arm/windows_guid.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace x64arm::com_rpc {
using Bytes=std::vector<std::uint8_t>;
struct WireError{std::uint32_t code{1783};}; // RPC_X_BAD_STUB_DATA。
struct Endpoint {
    std::array<std::uint8_t,16> nonce{};std::uint64_t birth{};std::uint32_t native_pid{},guest_pid{};
    auto operator<=>(const Endpoint&)const=default;
};
struct Interface {
    Endpoint endpoint;std::uint64_t id{};WindowsGuid iid{};std::array<std::uint8_t,16> capability{};
    auto operator<=>(const Interface&)const=default;
};
enum class ValueType:std::uint8_t {Null,Unsigned,Signed,Bytes,Utf16,Interface,Record,Array};
struct Value {
    ValueType type{ValueType::Null};std::uint8_t width{};std::uint64_t number{};
    Bytes bytes;std::u16string text;Interface reference;std::vector<Value> children;
    bool operator==(const Value&)const=default;
};
enum class Opcode:std::uint16_t {Hello=1,Invoke,Acquire,Drop,PinPacket,ReleasePacket,Disconnect,Cancel,Reply};
struct Context {
    std::u16string sid,station;std::uint32_t session{},integrity{};Bytes token;
    bool operator==(const Context&)const=default;
};
struct Message {
    Opcode opcode{Opcode::Invoke};std::uint32_t flags{},method{};std::uint64_t request{},result{},deadline{};
    std::array<std::uint8_t,16> causality{};Endpoint sender;Interface target;Context context;std::vector<Value> values;
    bool operator==(const Message&)const=default;
};
struct Limits {
    std::size_t bytes{16U*1024U*1024U},nodes{65536},depth{64},token{256U*1024U};
};
namespace wire_detail {
constexpr std::array<std::uint8_t,8> magic{'W','6','4','R','P','C','0','1'};
constexpr std::size_t header_size=192,reference_size=72,node_header=16;
template<std::size_t N> bool zero(const std::array<std::uint8_t,N>& bytes){return std::all_of(bytes.begin(),bytes.end(),[](auto value){return value==0;});}
inline bool valid(const Endpoint& endpoint){return endpoint.native_pid && endpoint.guest_pid && !zero(endpoint.nonce);}
inline bool valid(const Interface& reference){return reference.id && valid(reference.endpoint) && !zero(reference.capability);}
inline void require(bool value){if(!value)throw WireError{};}
inline void limits(const Limits& value){require(value.bytes>=header_size+8 && value.bytes<=16U*1024U*1024U && value.nodes && value.nodes<=65536 && value.depth && value.depth<=64 && value.token<=256U*1024U);}
inline std::uint64_t checksum(std::span<const std::uint8_t> bytes){std::uint64_t value=14695981039346656037ULL;for(const auto byte:bytes)value=(value^byte)*1099511628211ULL;return value;}
inline void put(Bytes& bytes,std::size_t at,unsigned count,std::uint64_t value){require(count<=8 && at<=bytes.size() && count<=bytes.size()-at);for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned count){require(count<=8 && at<=bytes.size() && count<=bytes.size()-at);std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;}
inline void endpoint(Bytes& bytes,std::size_t at,const Endpoint& value){
    require(at<=bytes.size() && 32<=bytes.size()-at);std::copy(value.nonce.begin(),value.nonce.end(),bytes.data()+at);
    put(bytes,at+16,8,value.birth);put(bytes,at+24,4,value.native_pid);put(bytes,at+28,4,value.guest_pid);
}
inline Endpoint endpoint(std::span<const std::uint8_t> bytes,std::size_t at){
    require(at<=bytes.size() && 32<=bytes.size()-at);Endpoint value;std::copy_n(bytes.data()+at,16,value.nonce.begin());value.birth=get(bytes,at+16,8);
    value.native_pid=static_cast<std::uint32_t>(get(bytes,at+24,4));value.guest_pid=static_cast<std::uint32_t>(get(bytes,at+28,4));return value;
}
inline void reference(Bytes& bytes,std::size_t at,const Interface& value){
    require(at<=bytes.size() && reference_size<=bytes.size()-at);endpoint(bytes,at,value.endpoint);put(bytes,at+32,8,value.id);
    std::copy(value.iid.begin(),value.iid.end(),bytes.data()+at+40);std::copy(value.capability.begin(),value.capability.end(),bytes.data()+at+56);
}
inline Interface reference(std::span<const std::uint8_t> bytes,std::size_t at){
    require(at<=bytes.size() && reference_size<=bytes.size()-at);Interface value;value.endpoint=endpoint(bytes,at);value.id=get(bytes,at+32,8);
    std::copy_n(bytes.data()+at+40,16,value.iid.begin());std::copy_n(bytes.data()+at+56,16,value.capability.begin());return value;
}
inline void scalar(const Value& value){
    require(value.width==1 || value.width==2 || value.width==4 || value.width==8);
    if(value.width==8)return;const auto bits=8*value.width;const auto mask=(std::uint64_t{1}<<bits)-1;
    if(value.type==ValueType::Unsigned)require((value.number&~mask)==0);
    else{const auto low=value.number&mask;const auto canonical=(low&(std::uint64_t{1}<<(bits-1)))?low|~mask:low;require(canonical==value.number);}
}
inline void validate(const Message& message,const Limits& limit){
    const auto opcode=static_cast<unsigned>(message.opcode);require(opcode>=static_cast<unsigned>(Opcode::Hello) && opcode<=static_cast<unsigned>(Opcode::Reply));
    require(valid(message.sender) && message.request && !zero(message.causality) && message.method<=65535 && !(message.flags&~1U));
    require(!message.flags || message.opcode==Opcode::Invoke);
    const bool endpoint_only=message.opcode==Opcode::Hello || message.opcode==Opcode::Cancel || message.opcode==Opcode::Reply;
    require(valid(message.target) || (endpoint_only && message.target==Interface{}));
    require(message.context.sid.size()<=256 && message.context.station.size()<=260 && message.context.token.size()<=limit.token && message.values.size()<=limit.nodes);
}
class Encoder {
    Limits limit_;std::size_t nodes_{};
    void grow(std::size_t count){require(bytes.size()<=limit_.bytes-8 && count<=limit_.bytes-8-bytes.size());bytes.resize(bytes.size()+count);}
public:
    Bytes bytes;
    explicit Encoder(Limits limit):limit_(limit),bytes(header_size){}
    void raw(std::span<const std::uint8_t> data){const auto offset=bytes.size();grow(data.size());std::copy(data.begin(),data.end(),bytes.data()+offset);}
    void text(std::u16string_view data){require(data.size()<=std::numeric_limits<std::size_t>::max()/2);const auto offset=bytes.size();grow(data.size()*2);for(std::size_t i=0;i<data.size();++i)put(bytes,offset+i*2,2,data[i]);}
    void value(const Value& value,std::size_t depth=1){
        require(depth<=limit_.depth && nodes_<limit_.nodes);++nodes_;const auto type=static_cast<unsigned>(value.type);require(type<=static_cast<unsigned>(ValueType::Array));
        const bool is_scalar=value.type==ValueType::Unsigned || value.type==ValueType::Signed;
        const bool aggregate=value.type==ValueType::Record || value.type==ValueType::Array;
        require(is_scalar || (!value.width && !value.number));require(value.type==ValueType::Bytes || value.bytes.empty());require(value.type==ValueType::Utf16 || value.text.empty());
        require(value.type==ValueType::Interface || value.reference==Interface{});require(aggregate || value.children.empty());
        const auto begin=bytes.size();grow(node_header);bytes[begin]=static_cast<std::uint8_t>(value.type);bytes[begin+1]=value.width;
        if(is_scalar){scalar(value);put(bytes,begin+8,8,value.number);}
        else if(value.type==ValueType::Bytes)raw(value.bytes);
        else if(value.type==ValueType::Utf16)text(value.text);
        else if(value.type==ValueType::Interface){require(valid(value.reference));grow(reference_size);reference(bytes,begin+node_header,value.reference);}
        else if(aggregate){require(value.children.size()<=limit_.nodes-nodes_);const auto at=bytes.size();grow(4);put(bytes,at,4,value.children.size());for(const auto& child:value.children)this->value(child,depth+1);}
        put(bytes,begin+4,4,bytes.size()-begin);
    }
};
class Decoder {
    Limits limit_;std::size_t nodes_{};
public:
    explicit Decoder(Limits limit):limit_(limit){}
    static std::u16string text(std::span<const std::uint8_t> bytes){require(!(bytes.size()&1U));std::u16string result(bytes.size()/2,0);for(std::size_t i=0;i<result.size();++i)result[i]=static_cast<char16_t>(get(bytes,i*2,2));return result;}
    Value value(std::span<const std::uint8_t> bytes,std::size_t& consumed,std::size_t depth=1){
        require(depth<=limit_.depth && nodes_<limit_.nodes && bytes.size()>=node_header);++nodes_;
        const auto length=get(bytes,4,4);require(length>=node_header && length<=bytes.size() && !get(bytes,2,2));bytes=bytes.first(static_cast<std::size_t>(length));
        Value result;const auto type=bytes[0];require(type<=static_cast<unsigned>(ValueType::Array));result.type=static_cast<ValueType>(type);result.width=bytes[1];result.number=get(bytes,8,8);
        const auto body=bytes.subspan(node_header);
        if(result.type==ValueType::Unsigned || result.type==ValueType::Signed){require(body.empty());scalar(result);}
        else{
            require(!result.width && !result.number);
            if(result.type==ValueType::Null)require(body.empty());
            else if(result.type==ValueType::Bytes)result.bytes.assign(body.begin(),body.end());
            else if(result.type==ValueType::Utf16)result.text=text(body);
            else if(result.type==ValueType::Interface){require(body.size()==reference_size);result.reference=reference(body,0);require(valid(result.reference));}
            else{
                require(body.size()>=4);const auto count=get(body,0,4);require(count<=limit_.nodes-nodes_ && count<=(body.size()-4)/node_header);
                result.children.reserve(static_cast<std::size_t>(count));std::size_t offset=4;
                for(std::size_t i=0;i<count;++i){std::size_t size=0;result.children.push_back(value(body.subspan(offset),size,depth+1));offset+=size;}
                require(offset==body.size());
            }
        }consumed=static_cast<std::size_t>(length);return result;
    }
};
}

// 這是 sandbox 內部格式。checksum 用於辨識損壞，不代表授權；transport 必須另核對
// endpoint lease、程序出生識別、虛擬權杖及每個 interface capability。
// id 只索引 exporter；不得把它當成來源程序或接收程序的客體位址。
inline Bytes encode(const Message& message,Limits limit={}){
    using namespace wire_detail;limits(limit);validate(message,limit);Encoder encoder(limit);auto& bytes=encoder.bytes;
    std::copy(magic.begin(),magic.end(),bytes.begin());put(bytes,8,2,1);put(bytes,10,2,static_cast<unsigned>(message.opcode));put(bytes,12,4,message.flags);put(bytes,16,8,message.request);
    std::copy(message.causality.begin(),message.causality.end(),bytes.begin()+24);endpoint(bytes,40,message.sender);reference(bytes,72,message.target);
    put(bytes,144,8,message.result);put(bytes,152,4,message.method);put(bytes,156,4,message.context.session);put(bytes,160,4,message.context.integrity);
    put(bytes,164,2,message.context.sid.size());put(bytes,166,2,message.context.station.size());put(bytes,168,4,message.context.token.size());put(bytes,172,4,message.values.size());put(bytes,184,8,message.deadline);
    encoder.text(message.context.sid);encoder.text(message.context.station);encoder.raw(message.context.token);for(const auto& value:message.values)encoder.value(value);
    put(bytes,176,4,bytes.size()-header_size);const auto sum=checksum(bytes);const auto end=bytes.size();bytes.resize(end+8);put(bytes,end,8,sum);return std::move(bytes);
}
inline Message decode(std::span<const std::uint8_t> bytes,Limits limit={}){
    using namespace wire_detail;limits(limit);require(bytes.size()>=header_size+8 && bytes.size()<=limit.bytes && std::equal(magic.begin(),magic.end(),bytes.begin()));
    require(get(bytes,8,2)==1 && !get(bytes,180,4) && get(bytes,176,4)==bytes.size()-header_size-8 && get(bytes,bytes.size()-8,8)==checksum(bytes.first(bytes.size()-8)));
    Message result;result.opcode=static_cast<Opcode>(get(bytes,10,2));result.flags=static_cast<std::uint32_t>(get(bytes,12,4));result.request=get(bytes,16,8);std::copy_n(bytes.begin()+24,16,result.causality.begin());
    result.sender=endpoint(bytes,40);result.target=reference(bytes,72);result.result=get(bytes,144,8);result.method=static_cast<std::uint32_t>(get(bytes,152,4));result.context.session=static_cast<std::uint32_t>(get(bytes,156,4));result.context.integrity=static_cast<std::uint32_t>(get(bytes,160,4));result.deadline=get(bytes,184,8);
    const auto sid=get(bytes,164,2),station=get(bytes,166,2),token=get(bytes,168,4),arguments=get(bytes,172,4);require(sid<=256 && station<=260 && token<=limit.token && arguments<=limit.nodes);
    auto payload=bytes.subspan(header_size,bytes.size()-header_size-8);require(sid*2<=payload.size());result.context.sid=Decoder::text(payload.first(static_cast<std::size_t>(sid*2)));payload=payload.subspan(static_cast<std::size_t>(sid*2));
    require(station*2<=payload.size());result.context.station=Decoder::text(payload.first(static_cast<std::size_t>(station*2)));payload=payload.subspan(static_cast<std::size_t>(station*2));
    require(token<=payload.size());const auto token_bytes=payload.first(static_cast<std::size_t>(token));result.context.token.assign(token_bytes.begin(),token_bytes.end());payload=payload.subspan(static_cast<std::size_t>(token));
    require(arguments<=payload.size()/node_header);result.values.reserve(static_cast<std::size_t>(arguments));Decoder decoder(limit);
    for(std::size_t i=0;i<arguments;++i){std::size_t used=0;result.values.push_back(decoder.value(payload,used));payload=payload.subspan(used);}
    require(payload.empty());validate(result,limit);return result;
}
}
