#pragma once
#include "x64arm/gui_icon.hpp"
#include "x64arm/windows_guid.hpp"
#include <map>
#include <optional>
#include <string>
#include <array>
namespace x64arm::desktop {
enum class Status { Ok, Missing, Exists, Invalid, Limit };
struct Icon {
    std::uint64_t token{},owner{},serial{};
    std::uint32_t id{},flags{},callback{},version{},state{},info_flags{},timeout{10000};
    std::optional<WindowsGuid> guid;
    GuiIcon image,balloon;
    std::u16string tip,info,title;
    bool operator==(const Icon&)const=default;
};
struct Change {Icon value;std::uint32_t state_mask{};};
struct Result {Status status{};std::uint64_t token{};};
struct Callback {std::uint64_t owner{};std::uint32_t id{},message{},version{};bool operator==(const Callback&)const=default;};
struct Event {
    std::uint64_t token{};std::uint32_t code{};std::array<std::int32_t,2> point{};
    std::optional<Callback> retired{}; // 刪除時的 balloon hide 仍須通知原接收視窗。
    bool operator==(const Event&)const=default;
};
struct Message {std::uint64_t owner{},wparam{},lparam{};std::uint32_t message{};};
class Icons {
public:
    Result apply(std::uint32_t action,const Change&);
    const std::map<std::uint64_t,Icon>& values()const{return values_;}
    std::uint64_t focused()const{return focused_;}
    std::optional<Message> event(const Event&)const;
    void erase_owner(std::uint64_t);
    bool operator==(const Icons&)const=default;
private:
    std::map<std::uint64_t,Icon> values_;
    std::uint64_t next_token_{1};
    std::uint64_t focused_{};
};
struct Flash {
    std::uint32_t flags{},remaining{};
    std::uint64_t interval{},deadline{};
    bool phase{};
    bool operator==(const Flash&)const=default;
};
class Attention {
public:
    Status set(std::uint64_t window,std::uint32_t flags,std::uint32_t count,std::uint32_t timeout,std::uint32_t caret,std::uint64_t now);
    Status toggle(std::uint64_t window,bool invert,std::uint32_t flags);
    bool caption(std::uint64_t window,bool active)const;
    bool tick(std::uint64_t now,std::uint64_t foreground);
    void erase(std::uint64_t window){values_.erase(window);}
    const std::map<std::uint64_t,Flash>& values()const{return values_;}
    std::uint64_t deadline()const;
    bool operator==(const Attention&)const=default;
private:std::map<std::uint64_t,Flash> values_;
};
}
