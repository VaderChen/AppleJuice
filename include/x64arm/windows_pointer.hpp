#pragma once
#include "x64arm/input.hpp"
#include "x64arm/windows_abi.hpp"
#include <map>

namespace x64arm {
std::span<const WindowsServiceDescriptor> pointer_service_descriptors();
struct PointerDeviceState {
    input::PointerDevice device;
    bool attached{},in_range{};
    std::uint32_t starting_id{};
    bool injected{}; // InjectTouchInput 的虛擬裝置；不隨原生裝置列舉移除。
};
struct PointerPacket {
    input::PointerSample sample;
    std::uint64_t device{},window{},thread{},performance{};
    std::uint32_t id{},frame{},flags{},time{},keys{},button_change{};
    std::array<std::int32_t,2> pixel{},himetric{};
};
struct PointerInputState {
    std::map<input::Guid,std::uint64_t> identities;
    std::map<std::uint64_t,PointerDeviceState> devices;
    std::map<std::uint64_t,bool> registrations;
    std::map<std::uint32_t,PointerPacket> current;
    std::map<std::uint64_t,PointerPacket> packets;
    std::map<std::uint64_t,std::uint64_t> delivered;
    std::uint32_t next_id{2},frame{};
    std::uint64_t next_packet{1};
};
}
