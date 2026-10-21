#pragma once
#include "x64arm/input.hpp"
#include "x64arm/hid.hpp"
#include "x64arm/windows_abi.hpp"
#include <deque>
#include <map>
#include <set>
#include <string>

namespace x64arm {
inline constexpr std::array<std::uint8_t,16> hid_interface_guid{0xb2,0x55,0x1e,0x4d,0x6f,0xf1,0xcf,0x11,0x88,0xcb,0,0x11,0x11,0,0,0x30};
inline constexpr std::array<std::uint8_t,16> hid_class_guid{0xa0,0x17,0x5a,0x74,0xd3,0x74,0xd0,0x11,0xb6,0xfe,0,0xa0,0xc9,0x0f,0x57,0xda};
std::span<const WindowsServiceDescriptor> raw_input_service_descriptors();
std::span<const WindowsServiceDescriptor> hid_service_descriptors();
bool hid_path(std::u16string_view);
struct HidFile {
    input::Device device;
    std::shared_ptr<input::Backend> backend;
    hid::Layout layout;
    std::deque<std::vector<std::uint8_t>> reports;
    bool disconnected{};
};
struct RawInputDevice {
    input::Device device;
    std::uint32_t type{};
    std::u16string name;
    std::vector<std::uint8_t> preparsed;
};
struct RawInputRegistration {
    std::uint16_t page{},usage{};
    std::uint32_t flags{};
    std::uint64_t window{};
};
struct RawInputPacket {
    std::uint64_t device{},window{},thread{};
    std::uint32_t type{},code{};
    std::vector<std::uint8_t> payload;
};
struct RawInputState {
    std::map<std::uint64_t,RawInputDevice> devices;
    std::map<input::Guid,std::uint64_t> identities;
    std::map<std::pair<std::uint16_t,std::uint16_t>,RawInputRegistration> registrations;
    std::map<std::uint64_t,RawInputPacket> packets;
    std::set<std::uint64_t> owned_preparsed;
    std::array<std::uint8_t,256> keyboard{};
    std::uint8_t buttons{};
};
}
