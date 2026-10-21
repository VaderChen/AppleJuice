#pragma once
#include "bluetooth_family.hpp"
#include "bluetooth_abi.hpp"
#include "x64arm/memory.hpp"

namespace x64arm::bluetooth::windows {
inline constexpr std::array<std::uint8_t,16> device_interface{0x18,0xee,0x1a,0x78,0x33,0x77,0xe4,0x4c,0xad,0xd0,0x91,0xf4,0x1c,0x67,0xb5,0x92};
inline constexpr std::array<std::uint8_t,16> service_interface{0x79,0xb6,0x3b,0x6e,0x72,0x43,0xc8,0x40,0x9e,0xaa,0x45,0x09,0xdf,0x26,0x0c,0xd8};
inline constexpr std::array<std::uint8_t,16> radio_interface{0x2a,0x30,0x50,0x08,0x44,0xb3,0xda,0x4f,0x9b,0xe9,0x90,0x57,0x6b,0x8d,0x46,0xf0};
inline constexpr std::array<std::uint8_t,16> device_class{0x6c,0xf0,0xcb,0xe0,0x8b,0xcd,0x47,0x46,0xbb,0x8a,0x26,0x3b,0x43,0xf0,0xf9,0x74};
inline constexpr std::u16string_view path_prefix=u"\\\\?\\x64arm#bluetooth#";
inline std::u16string fold(std::u16string value){for(auto& ch:value)if(ch>=u'A' && ch<=u'Z')ch+=32;return value;}
inline std::u16string encoded_id(std::string_view value){
    constexpr char16_t digits[]=u"0123456789abcdef";std::u16string result;
    for(const char ch:value){const auto byte=static_cast<unsigned char>(ch);result+=digits[byte>>4];result+=digits[byte&15U];}return result;
}
inline std::uint32_t win_error(std::uint32_t value){return !value?0:(value&0xffff0000U)==0x80070000U?value&0xffffU:value<=65535?value:31U;}
inline Bytes read(GuestMemory& memory,std::uint64_t pointer,std::size_t size){Bytes result(size);memory.read(GuestAddress{pointer},result);return result;}
inline Bytes characteristic_value(const Bytes& value){auto result=wire::characteristic_value(value);result.resize(std::max<std::size_t>(8,result.size()));return result;}
struct Reply {std::uint64_t value{};std::optional<std::uint32_t> last_error;};
inline Reply boolean(const Result<bool>& result){return {result.error?0U:result.value?1U:0U,result.error?std::optional{win_error(result.error)}:std::nullopt};}
enum class Kind {Radio,Device,Service};
struct Interface {
    Kind kind{};DeviceId device;std::optional<Service> service;
    std::u16string path,name;std::array<std::uint8_t,16> guid{};std::vector<std::array<std::uint8_t,16>> aliases;
};
struct Endpoint {Interface interface;std::uint32_t access{},share{3};};
struct Operation {
    std::uint64_t token{};std::string symbol;
    std::function<bool()> done;
    std::function<Reply(GuestMemory&)> finish;
    std::function<void()> cancel;
};
template<class T,class Finish> Operation operation(std::uint64_t token,std::string_view symbol,Pending<T> request,Finish finish,std::function<void()> discard={}){
    return {token,std::string(symbol),[request]{return request->poll().has_value();},
        [request,finish=std::move(finish)](GuestMemory& memory){const auto result=request->poll();if(!result)throw std::logic_error("Bluetooth 請求尚未完成");return finish(memory,*result);},
        [request,discard=std::move(discard)]{request->cancel();if(discard)discard();}};
}
}

namespace x64arm {
struct BluetoothState {
    std::shared_ptr<bluetooth::Backend> backend;
    bluetooth::Family family;
    std::map<std::uint64_t,bluetooth::windows::Endpoint> handles;
    std::map<std::uint64_t,bluetooth::windows::Operation> operations;
    std::vector<bluetooth::Radio> radios;
    std::uint64_t enumeration_deadline{};bool enumerated{};
    BluetoothState(std::shared_ptr<bluetooth::Backend> native,std::function<std::uint64_t()> allocate):backend(std::move(native)),family(backend,std::move(allocate)){}
    ~BluetoothState(){for(auto& [thread,pending]:operations){(void)thread;pending.cancel();}}
    std::vector<bluetooth::windows::Interface> interfaces()const{
        using namespace bluetooth::windows;std::vector<Interface> result;
        for(const auto& radio:radios){Interface item;item.kind=Kind::Radio;item.device=radio.id;item.name=radio.name;item.guid=radio_interface;item.path=std::u16string(path_prefix)+u"radio#"+encoded_id(radio.id);result.push_back(std::move(item));}
        for(const auto& device:family.device_snapshot()){
            const auto path=std::u16string(path_prefix)+u"le#"+encoded_id(device.id);
            Interface item;item.kind=Kind::Device;item.device=device.id;item.name=device.name;item.guid=device_interface;item.path=path;result.push_back(std::move(item));
            for(const auto& service:family.service_snapshot(device.id)){
                Interface entry;entry.kind=Kind::Service;entry.device=device.id;entry.service=service;entry.name=device.name;entry.guid=service.uuid.bytes;entry.aliases={service_interface};
                entry.path=path+u"#service#";const auto value=std::to_string(service.handle);entry.path.append(value.begin(),value.end());result.push_back(std::move(entry));
            }
        }return result;
    }
};
}
