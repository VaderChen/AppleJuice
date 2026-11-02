#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x64arm {class SharedWake;}
namespace x64arm::wlan {
using Bytes=std::vector<std::uint8_t>;
struct Network {
    Bytes ssid,information_elements;
    std::array<std::uint8_t,6> bssid{};
    std::vector<std::uint32_t> phys;
    std::uint32_t bss_type{1},auth{1},cipher{},frequency_khz{},beacon_period{};
    std::int32_t rssi{-100};
    bool one_x{},in_reg_domain{};
};
struct Interface {
    std::string name,description;
    std::uint32_t state{4},channel{},phy{},tx_rate{},rx_rate{},connection_error{},networks_error{};
    bool power{},capabilities_known{},dot11d{};
    std::vector<std::uint32_t> phys;
    std::vector<std::array<std::uint32_t,2>> auth_cipher_pairs;
    std::map<std::uint32_t,std::uint32_t> options;
    std::optional<Network> connection;
    std::vector<Network> networks;
};
struct Connection {
    Bytes ssid;
    std::vector<std::array<std::uint8_t,6>> bssids;
    std::string password;
    std::uint32_t bss_type{1},auth{1},cipher{},flags{};
};
// 發布結果先於 done.store(release)。宿主工作只持有自己的物件，不接觸客體記憶體。
struct Operation {
    std::atomic<bool> done{};
    std::uint32_t error{},submission_error{};
    virtual ~Operation()=default;
};
enum class Change {Link,Radio,Signal,ScanCache,Unavailable};
struct Event {std::string interface;Change change;};
class Backend {
public:
    virtual ~Backend()=default;
    virtual std::uint32_t enumerate(std::vector<Interface>&)=0;
    virtual std::uint32_t query(const std::string&,Interface&)=0;
    virtual std::uint32_t set_radio(const std::string&,bool)=0;
    virtual std::uint32_t set_option(const std::string&,std::uint32_t,std::uint32_t){return 50;}
    virtual std::shared_ptr<Operation> scan(const std::string&,const std::optional<Bytes>&,std::span<const std::uint8_t>)=0;
    virtual std::shared_ptr<Operation> connect(const std::string&,const Connection&)=0;
    virtual std::shared_ptr<Operation> disconnect(const std::string&)=0;
    virtual std::uint32_t monitor(bool)=0;
    virtual std::vector<Event> poll()=0;
    virtual bool set_wake(std::shared_ptr<SharedWake>){return false;}
};
std::shared_ptr<Backend> native_backend();
inline std::uint32_t signal_quality(std::int32_t rssi){return rssi<=-100?0U:rssi>=-50?100U:static_cast<std::uint32_t>((rssi+100)*2);}
}
