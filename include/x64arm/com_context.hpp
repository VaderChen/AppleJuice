#pragma once
#include "x64arm/windows_guid.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace x64arm::com_context {
enum class Status { Ok, Missing, Invalid, Occupied, Frozen, Limit };
struct Property {
    WindowsGuid id{};
    std::uint32_t flags{};
    std::uint64_t object{}; // 客體 IUnknown 指標；引用呼叫由 COM driver 執行。
    bool operator==(const Property&) const = default;
};
struct EncodedProperty {std::array<std::uint8_t,32> bytes{};std::size_t size{};};
EncodedProperty encode_property(const Property&,bool win32);
std::optional<Property> decode_property(std::span<const std::uint8_t>,bool win32);
struct Context {
    std::uint64_t owner{}; // 共用 apartment/context key；不是宿主執行緒 ID。
    std::uint32_t references{1}; // active context 保有一個根引用。
    bool active{true}, frozen{};
    std::map<WindowsGuid,Property> properties{};
    bool operator==(const Context&) const = default;
};
struct Disposal {
    std::uint64_t token{};
    std::vector<Property> properties; // driver 必須 Release 這些持有的 IUnknown。
};
struct Reference {
    Status status{Status::Ok};
    std::uint32_t count{};
    std::optional<Disposal> disposal{};
};
struct Change {
    Status status{Status::Ok};
    std::optional<Property> previous{};
};
// Snapshot／Clone 不受後續 context 修改影響。driver 為快照中的物件保有引用。
class Enumeration {
public:
    explicit Enumeration(std::vector<Property> properties = {});
    std::span<const Property> next(std::size_t count);
    std::size_t skip(std::size_t count);
    void reset() noexcept;
    std::size_t position() const noexcept;
    const std::vector<Property>& properties() const noexcept;
    bool operator==(const Enumeration&) const = default;
private:
    std::vector<Property> properties_;
    std::size_t position_{};
};
// 純值狀態可隔離 services 比較分支；不能把 IObjContext 當成數值 token。
// token 由 driver 配置真正客體物件／vtable；core 保存引用與 active 身分。
class State {
public:
    static constexpr std::size_t max_contexts=4096,max_properties=4096;
    Status create(std::uint64_t owner,std::uint64_t token);
    std::optional<std::uint64_t> current(std::uint64_t owner) const;
    const Context* find(std::uint64_t token) const noexcept;
    Reference add_ref(std::uint64_t token);
    Reference release(std::uint64_t token);
    Reference retire(std::uint64_t owner);
    Change set(std::uint64_t token,Property property);
    Change remove(std::uint64_t token,const WindowsGuid& id);
    std::optional<Property> get(std::uint64_t token,const WindowsGuid& id) const;
    Status freeze(std::uint64_t token);
    std::optional<Enumeration> enumerate(std::uint64_t token) const;
    bool operator==(const State&) const = default;
private:
    std::map<std::uint64_t,Context> contexts_;
    std::map<std::uint64_t,std::uint64_t> active_;
    Reference drop(std::map<std::uint64_t,Context>::iterator found);
};
}
