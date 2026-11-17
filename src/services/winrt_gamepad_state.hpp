#pragma once
#include "x64arm/input.hpp"
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace x64arm::winrt_gamepad {
using Guid=input::Guid;
enum View : unsigned { Statics, Statics2, Activation, Gamepad, Gamepad2, Controller, Vector, Iterable, Iterator };
struct Method {std::string_view name;unsigned arguments;std::uint32_t wide{},narrow{};};
inline constexpr Method methods[]{
    {"Winrt.QueryInterface",3},{"Winrt.AddRef",1},{"Winrt.Release",1},
    {"Winrt.GetIids",3},{"Winrt.GetRuntimeClassName",2},{"Winrt.GetTrustLevel",2},
    {"Winrt.ActivateInstance",2},{"Winrt.Added",3},{"Winrt.RemoveAdded",2,2},
    {"Winrt.Removed",3},{"Winrt.RemoveRemoved",2,2},{"Winrt.Gamepads",2},
    {"Winrt.FromController",3},{"Winrt.GetVibration",2},{"Winrt.PutVibration",2},
    // Win32 的 32-byte 結構在堆疊內傳值；Win64 以間接指標傳入。兩個入口各保留完整 ABI。
    {"Winrt.PutVibration32",9},{"Winrt.Reading",2},{"Winrt.ButtonLabel",3,0,2},
    {"Winrt.HeadsetConnected",3},{"Winrt.RemoveHeadsetConnected",2,2},
    {"Winrt.HeadsetDisconnected",3},{"Winrt.RemoveHeadsetDisconnected",2,2},
    {"Winrt.UserChanged",3},{"Winrt.RemoveUserChanged",2,2},
    {"Winrt.Headset",2},{"Winrt.IsWireless",2},{"Winrt.User",2},
    {"Winrt.GetAt",3,0,2},{"Winrt.Size",2},{"Winrt.IndexOf",4},
    {"Winrt.GetMany",5,0,6},{"Winrt.First",2},{"Winrt.Current",2},
    {"Winrt.HasCurrent",2},{"Winrt.MoveNext",2},{"Winrt.IteratorGetMany",4,0,2},
    {"Winrt.Dispatch",1,1}
};
inline constexpr std::string_view base[]{"Winrt.QueryInterface","Winrt.AddRef","Winrt.Release","Winrt.GetIids","Winrt.GetRuntimeClassName","Winrt.GetTrustLevel"};
inline std::vector<std::string_view> slots(unsigned view,bool x86){
    std::vector<std::string_view> out(std::begin(base),std::end(base));
    const auto append=[&](std::initializer_list<std::string_view> values){out.insert(out.end(),values);};
    switch(view){
    case Statics:append({"Winrt.Added","Winrt.RemoveAdded","Winrt.Removed","Winrt.RemoveRemoved","Winrt.Gamepads"});break;
    case Statics2:append({"Winrt.FromController"});break;
    case Activation:append({"Winrt.ActivateInstance"});break;
    case Gamepad:append({"Winrt.GetVibration",x86?"Winrt.PutVibration32":"Winrt.PutVibration","Winrt.Reading"});break;
    case Gamepad2:append({"Winrt.ButtonLabel"});break;
    case Controller:append({"Winrt.HeadsetConnected","Winrt.RemoveHeadsetConnected","Winrt.HeadsetDisconnected","Winrt.RemoveHeadsetDisconnected","Winrt.UserChanged","Winrt.RemoveUserChanged","Winrt.Headset","Winrt.IsWireless","Winrt.User"});break;
    case Vector:append({"Winrt.GetAt","Winrt.Size","Winrt.IndexOf","Winrt.GetMany"});break;
    case Iterable:append({"Winrt.First"});break;
    case Iterator:append({"Winrt.Current","Winrt.HasCurrent","Winrt.MoveNext","Winrt.IteratorGetMany"});break;
    default:throw std::invalid_argument("WinRT Gamepad view");
    }
    return out;
}
inline constexpr Guid iid_inspectable{0xe0,0xe2,0x86,0xaf,0x2d,0xb1,0x6a,0x4c,0x9c,0x5a,0xd7,0xaa,0x65,0x10,0x1e,0x90};
inline constexpr Guid iid_agile{0x94,0x2b,0xea,0x94,0xcc,0xe9,0xe0,0x49,0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90};
inline constexpr Guid iid_activation{0x35,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
inline constexpr Guid iid_statics{0x29,0xe5,0xbc,0x8b,0x9c,0xd4,0xe9,0x39,0x95,0x60,0xe4,0x7d,0xde,0x96,0xb7,0xc8};
inline constexpr Guid iid_statics2{0xc5,0x6d,0x67,0x42,0x56,0x8,0xc4,0x47,0x92,0x13,0xb3,0x95,0x50,0x4c,0x3a,0x3c};
inline constexpr Guid iid_gamepad{0x3c,0xb4,0x7b,0xbc,0x69,0xa,0x3,0x39,0x9e,0x9d,0xa5,0xf,0x86,0xa4,0x5d,0xe5};
inline constexpr Guid iid_gamepad2{0xbd,0x89,0x16,0x3c,0x15,0x59,0x45,0x42,0xb0,0xc0,0xc8,0x9f,0xae,0x3,0x8,0xff};
inline constexpr Guid iid_controller{0x22,0x65,0xaf,0x1b,0x64,0x5f,0xc5,0x42,0x82,0x67,0xb9,0xfe,0x22,0x15,0xbf,0xbd};
inline constexpr Guid iid_vector{0x69,0xbb,0x97,0xeb,0xc9,0x9,0x99,0x5a,0x86,0xb2,0x3e,0x36,0x8,0x52,0x84,0xd4};
inline constexpr Guid iid_iterable{0xa0,0x2b,0x13,0x47,0x17,0x6b,0xd2,0x5c,0xa8,0xbd,0xb5,0xd3,0x44,0x3c,0xcb,0x13};
inline constexpr Guid iid_iterator{0xe8,0x37,0x67,0x24,0xbc,0x12,0x64,0x5c,0xaf,0x52,0x6,0xdb,0x4b,0x13,0xfa,0x2f};

}

namespace x64arm {
struct WinrtGamepadObject {
    enum class Kind {Factory,Pad,Vector,Iterator} kind{Kind::Factory};
    std::uint32_t references{1};
    std::uint64_t canonical{};
    std::map<unsigned,std::uint64_t> interfaces;
    input::Guid device{};
    std::vector<std::shared_ptr<WinrtGamepadObject>> items;
    std::size_t cursor{};
};
struct WinrtGamepadSubscription {
    std::uint64_t handler{},token{};
    unsigned kind{},pending{};
    bool removed{};
    std::shared_ptr<WinrtGamepadObject> owner;
};
struct WinrtGamepadBridge {
    struct Interface {std::shared_ptr<WinrtGamepadObject> object;unsigned kind;};
    struct Event {std::shared_ptr<WinrtGamepadSubscription> subscription;std::shared_ptr<WinrtGamepadObject> pad;bool dispatched{};};
    std::map<std::uint64_t,Interface> views;
    std::map<unsigned,std::uint64_t> tables;
    std::shared_ptr<WinrtGamepadObject> factory;
    std::map<input::Guid,std::shared_ptr<WinrtGamepadObject>> pads;
    std::map<std::uint64_t,std::shared_ptr<WinrtGamepadSubscription>> subscriptions;
    std::map<std::uint64_t,Event> pending;
    std::uint64_t next_token{1},next_event{1},polled{};
    bool initialized{};
};
}

