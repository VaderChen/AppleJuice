#include "x64arm/com_marshal.hpp"
#include <stdexcept>
namespace x64arm::com_marshal {
namespace {constexpr std::array<std::uint8_t,8> remote_magic{'W','6','4','X','M','R','0','1'};}
bool remote_prefix(std::span<const std::uint8_t> bytes){return bytes.size()>=remote_magic.size() && std::equal(remote_magic.begin(),remote_magic.end(),bytes.begin());}
std::vector<std::uint8_t> encode(const RemoteWire& value){
    if(!com_rpc::wire_detail::valid(value.interface) || !value.packet || !valid_flags(value.flags) || value.context>1)throw std::invalid_argument("COM 遠端封送資料不合法");
    std::vector<std::uint8_t> bytes(remote_wire_size);std::copy(remote_magic.begin(),remote_magic.end(),bytes.begin());
    com_rpc::wire_detail::put(bytes,8,4,1);com_rpc::wire_detail::put(bytes,12,4,value.flags);com_rpc::wire_detail::reference(bytes,16,value.interface);
    com_rpc::wire_detail::put(bytes,88,8,value.packet);com_rpc::wire_detail::put(bytes,96,4,value.context);
    com_rpc::wire_detail::put(bytes,104,8,com_rpc::wire_detail::checksum(std::span(bytes).first(104)));return bytes;
}
std::optional<RemoteWire> decode_remote(std::span<const std::uint8_t> bytes){
    if(bytes.size()!=remote_wire_size || !remote_prefix(bytes))return std::nullopt;
    if(com_rpc::wire_detail::get(bytes,8,4)!=1 || com_rpc::wire_detail::get(bytes,100,4) || com_rpc::wire_detail::get(bytes,104,8)!=com_rpc::wire_detail::checksum(bytes.first(104)))return std::nullopt;
    RemoteWire value;value.flags=static_cast<std::uint32_t>(com_rpc::wire_detail::get(bytes,12,4));value.interface=com_rpc::wire_detail::reference(bytes,16);value.packet=com_rpc::wire_detail::get(bytes,88,8);value.context=static_cast<std::uint32_t>(com_rpc::wire_detail::get(bytes,96,4));
    if(!com_rpc::wire_detail::valid(value.interface) || !value.packet || !valid_flags(value.flags) || value.context>1)return std::nullopt;return value;
}
std::vector<std::uint8_t> encode(Wire value){
    if(!value.token || !value.object || !value.pid || !valid_flags(value.flags))throw std::invalid_argument("COM marshal 記錄不合法");
    std::vector<std::uint8_t> bytes(32);const auto put=[&](unsigned at,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)bytes[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
    put(0,0x314d574d,4);put(4,value.flags,4);put(8,value.token,8);put(16,value.object,8);put(24,value.pid,4);return bytes;
}
std::optional<Wire> decode(std::span<const std::uint8_t> bytes){
    if(bytes.size()!=32)return {};const auto read=[&](unsigned at,unsigned n){std::uint64_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint64_t{bytes[at+i]}<<(8*i);return value;};
    if(read(0,4)!=0x314d574d || read(28,4) || !valid_flags(static_cast<std::uint32_t>(read(4,4))) || !read(8,8) || !read(16,8) || !read(24,4))return {};
    return Wire{read(8,8),read(16,8),static_cast<std::uint32_t>(read(24,4)),static_cast<std::uint32_t>(read(4,4))};
}
std::uint64_t Registry::publish(Packet packet){
    if(!packet.object || !packet.owner || !valid_flags(packet.flags))throw std::invalid_argument("COM marshal 引用不合法");if(packets_.size()>=4096)throw std::bad_alloc();
    while(!next_ || packets_.contains(next_))++next_;packet.token=next_;packets_.emplace(next_,packet);return next_++;
}
const Packet* Registry::find(std::uint64_t token)const{const auto found=packets_.find(token);return found==packets_.end()?nullptr:&found->second;}
std::optional<Packet> Registry::consume(std::uint64_t token){const auto found=packets_.find(token);if(found==packets_.end())return {};auto packet=found->second;if(mode_flags(packet.flags)==0)packets_.erase(found);return packet;}
std::optional<Packet> Registry::release(std::uint64_t token){const auto found=packets_.find(token);if(found==packets_.end())return {};auto packet=found->second;packets_.erase(found);return packet;}
std::vector<Packet> Registry::retire_standard(std::uint64_t owner,const std::function<bool(std::uint64_t)>& retained){
    std::vector<Packet> result;
    for(const auto& [token,packet]:packets_)if(packet.standard && packet.owner==owner && (!retained || !retained(token)))result.push_back(packet);
    for(const auto& packet:result)packets_.erase(packet.token);return result;
}
void Registry::move_standard_owner(std::uint64_t from,std::uint64_t to){
    for(auto& [token,packet]:packets_){(void)token;if(packet.standard && packet.owner==from)packet.owner=to;}
}
}
