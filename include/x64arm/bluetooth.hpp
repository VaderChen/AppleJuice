#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::bluetooth {
using Bytes=std::vector<std::uint8_t>;
using DeviceId=std::string;
constexpr std::uint32_t hresult(std::uint32_t error){return error?0x80070000U|(error&0xffffU):0;}
constexpr std::uint32_t att_error(std::uint32_t error){return 0x80650000U|error;}
struct Uuid {
    // Windows GUID 的記憶體順序；短 UUID 也正規化為 Bluetooth base UUID。
    std::array<std::uint8_t,16> bytes{};
    static Uuid short_uuid(std::uint16_t value){return {{{static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),0,0,0,0,0,0x10,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb}}};}
    std::optional<std::uint16_t> short_value()const{
        auto normalized=*this;normalized.bytes[0]=normalized.bytes[1]=0;
        if(normalized!=short_uuid(0))return {};
        return static_cast<std::uint16_t>(bytes[0]|(std::uint16_t{bytes[1]}<<8));
    }
    bool operator==(const Uuid&)const=default;
};
struct Radio {
    DeviceId id;std::uint64_t address{};std::u16string name;
    std::uint32_t device_class{};std::uint16_t subversion{},manufacturer{};
    bool powered{},connectable{},discoverable{},connectable_known{},discoverable_known{};
};
struct Device {
    DeviceId id,radio;std::uint64_t address{};std::u16string name;std::uint32_t device_class{};
    bool connected{},remembered{},authenticated{},low_energy{};
    std::array<std::uint16_t,8> last_seen{},last_used{};
};
struct Search {
    std::optional<DeviceId> radio;bool authenticated{},remembered{},unknown{},connected{},inquiry{};
    unsigned timeout_multiplier{};
    bool matches(const Device& value)const{
        return !value.low_energy && (!radio || *radio==value.radio) &&
            ((authenticated && value.authenticated) || (remembered && value.remembered) ||
             (connected && value.connected) || (unknown && !value.authenticated && !value.remembered));
    }
};
struct Service {Uuid uuid;std::uint16_t handle{};bool operator==(const Service&)const=default;};
struct Characteristic {
    std::uint16_t service{};Uuid uuid;std::uint16_t handle{},value_handle{};
    enum Property:unsigned {Broadcast=1,Read=2,Write=4,WriteWithoutResponse=8,SignedWrite=16,Notify=32,Indicate=64,Extended=128};
    std::uint8_t properties{};bool operator==(const Characteristic&)const=default;
};
struct Descriptor {std::uint16_t service{},characteristic{};std::uint32_t type{};Uuid uuid;std::uint16_t handle{};bool operator==(const Descriptor&)const=default;};
struct ValueEvent {
    DeviceId device;std::uint16_t handle{};Bytes value;
    enum class Kind {Value,ServicesChanged,Disconnected} kind{Kind::Value};
};
template<class T> struct Result {std::uint32_t error{};T value{};};

// 原生後端完成請求，客體執行緒可輪詢而不阻塞整個引擎；取消與完成只允許一次勝出。
template<class T> class Request {
    mutable std::mutex mutex_;
    std::optional<Result<T>> result_;
    std::function<void()> cancel_;
public:
    explicit Request(std::function<void()> cancel={}):cancel_(std::move(cancel)){}
    bool complete(Result<T> result){
        std::lock_guard lock(mutex_);if(result_)return false;result_=std::move(result);cancel_={};return true;
    }
    void cancel(){
        std::function<void()> callback;
        {std::lock_guard lock(mutex_);if(result_)return;result_=Result<T>{hresult(995),{}};callback=std::move(cancel_);}
        if(callback)callback();
    }
    std::optional<Result<T>> poll()const{std::lock_guard lock(mutex_);return result_;}
};
template<class T> using Pending=std::shared_ptr<Request<T>>;

class Backend {
public:
    virtual ~Backend()=default;
    virtual Result<std::vector<Radio>> radios()=0;
    virtual Pending<std::vector<Device>> devices(const Search&)=0;
    virtual Pending<std::vector<Device>> low_energy_devices(unsigned milliseconds)=0;
    virtual Pending<bool> discoverable(const DeviceId&,bool)=0;
    virtual Pending<bool> connectable(const DeviceId&,bool)=0;
    virtual Pending<std::vector<Service>> services(const DeviceId&)=0;
    virtual Pending<std::vector<Service>> included_services(const DeviceId&,const Service&)=0;
    virtual Pending<std::vector<Characteristic>> characteristics(const DeviceId&,const Service&)=0;
    virtual Pending<std::vector<Descriptor>> descriptors(const DeviceId&,const Characteristic&)=0;
    virtual Pending<Bytes> read(const DeviceId&,const Characteristic&,std::uint32_t flags)=0;
    virtual Pending<Bytes> read_descriptor(const DeviceId&,const Descriptor&,std::uint32_t flags)=0;
    virtual Pending<bool> write(const DeviceId&,const Characteristic&,Bytes,std::uint32_t flags)=0;
    virtual Pending<bool> write_descriptor(const DeviceId&,const Descriptor&,Bytes,std::uint32_t flags)=0;
    virtual Pending<bool> notifications(const DeviceId&,const Characteristic&,bool enabled)=0;
    virtual std::vector<ValueEvent> drain_events()=0;
};
struct NativeOptions {bool request_authorization{};};
std::shared_ptr<Backend> make_bluetooth_backend(NativeOptions options={});
}
