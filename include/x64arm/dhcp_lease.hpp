#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::dhcp {
struct Service {
    std::string id,name;
    bool dhcp{},configured{};
    double obtained{},expires{};
    std::vector<std::array<std::uint8_t,4>> addresses;
    std::shared_ptr<const void> settings;
    std::shared_ptr<const void> preferences;
};
struct Snapshot {
    std::uint32_t error{};
    bool interface_exists{};
    std::vector<Service> services;
    std::vector<std::array<std::uint8_t,4>> addresses;
};
// 設定僅作用於目前的 dynamic store；不提交 SCPreferences。
class Platform {
public:
    virtual ~Platform()=default;
    virtual Snapshot snapshot(std::string_view)=0;
    virtual std::uint32_t remove(const std::vector<Service>&)=0;
    virtual std::uint32_t restore(const Service&)=0;
    virtual std::uint32_t refresh(const Service&)=0;
    virtual void wait()=0;
};
struct Result {std::uint32_t error{};bool changed{};};
class Backend {
public:
    virtual ~Backend()=default;
    virtual Result change(std::string_view,bool renew)=0;
};
std::shared_ptr<Backend> backend(std::shared_ptr<Platform>,unsigned polls=600);
std::shared_ptr<Backend> native_backend();
std::shared_ptr<Platform> native_platform();
}
