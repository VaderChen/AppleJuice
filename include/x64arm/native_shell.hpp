#pragma once
#include "x64arm/readonly_files.hpp"
#include <array>
#include <span>

namespace x64arm::shell {
struct Application {
    std::string identifier;
    std::u16string name;
    bool operator==(const Application&) const = default;
};
struct Dock {
    std::array<std::int32_t,4> rectangle{}; // left, top, right, bottom
    std::uint32_t edge{},state{};
    bool operator==(const Dock&) const = default;
};
std::uint32_t reveal(std::span<const std::shared_ptr<HostFile>>,std::uint32_t flags);
std::vector<Application> applications(const std::shared_ptr<HostFile>&,std::string_view uri);
std::uint32_t open(const std::shared_ptr<HostFile>&,std::string_view uri,const Application&);
// 宿主為此 URI／檔案登錄的預設應用程式。
std::optional<Application> default_application(const std::shared_ptr<HostFile>&,std::string_view uri);
std::optional<Dock> dock();
}
