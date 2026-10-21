#pragma once
#include "x64arm/windows_guid.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm::com_error {
inline constexpr std::uint32_t suppress_exceptions=1,force_exceptions=2,use_set_error_info=4,suppress_set_error_info=8;
// roerrorapi.h 的 MAX_ERROR_MESSAGE_CHARS 不包含結尾 NUL。
inline constexpr std::uint32_t maximum_message_units=512;
enum class Status { Ok, Missing, Invalid, Limit };
struct Reporting {
    bool exception{},channel{};
    bool operator==(const Reporting&) const = default;
};
Reporting reporting(std::uint32_t flags,bool debugger,bool com_initialized);
// 驅動層提供 FormatMessage 的通用文字；此處只決定 HRESULT／NUL／長度規則。
std::u16string reported_message(std::uint32_t hresult,std::optional<std::u16string_view> message,std::u16string_view fallback);
struct Object {
    WindowsGuid guid{};
    std::u16string source,description,help_file,restricted_description,capability_sid,reference;
    std::uint32_t hresult{},help_context{},references{1};
    bool restricted{},source_present{},description_present{},help_file_present{};
    bool operator==(const Object&) const = default;
};
struct Reference {
    Status status{Status::Ok};
    std::uint32_t count{};
    bool destroyed{};
    bool operator==(const Reference&) const = default;
};
// 全為客體 token／值；不持有宿主指標。外部 IErrorInfo 的 AddRef／Release 由 COM 驅動回呼。
class State {
public:
    Status create(std::uint64_t token,Object);
    Object* find(std::uint64_t token);
    const Object* find(std::uint64_t token) const;
    Reference add_ref(std::uint64_t token);
    Reference release(std::uint64_t token);
    std::size_t object_count() const noexcept {return objects_.size();}
    std::uint64_t current(std::uint64_t thread) const;
    // 在新物件 AddRef 完成後才 replace；回傳的舊引用須由驅動 Release。
    // 重入時使用當下的 slot，不能釋放 AddRef 前的過期快照。
    std::uint64_t replace(std::uint64_t thread,std::uint64_t pointer);
    std::uint64_t take(std::uint64_t thread); // 清除 slot，把唯一 channel 引用轉移給呼叫者。
    Status set_flags(std::uint32_t flags);
    std::uint32_t flags() const noexcept {return flags_;}
    bool operator==(const State&) const = default;
private:
    std::map<std::uint64_t,Object> objects_;
    std::map<std::uint64_t,std::uint64_t> channels_;
    std::uint32_t flags_{use_set_error_info};
};
}
