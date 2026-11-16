#pragma once
#include "x64arm/shared_event.hpp"
#include <array>
#include <string_view>

namespace x64arm {
// 引擎既有全域／session 具名物件的目錄；獨立控制代碼，不能當作可等待事件。
inline constexpr std::array<std::u16string_view,5> builtin_object_directories{
    u"\\",u"\\Sessions",u"\\Sessions\\1",u"\\Sessions\\1\\BaseNamedObjects",u"\\BaseNamedObjects"};
class ObjectDirectory {
    int file_{-1},directory_{-1};
    std::uint32_t index_{};
public:
    ~ObjectDirectory();
    ObjectDirectory()=default;
    ObjectDirectory(const ObjectDirectory&)=delete;
    ObjectDirectory& operator=(const ObjectDirectory&)=delete;
    int descriptor() const noexcept{return file_;}
    int lock_descriptor() const noexcept{return directory_;}
    std::uint32_t index() const noexcept{return index_;}
    std::u16string_view name() const noexcept{return builtin_object_directories[index_];}
    static std::shared_ptr<ObjectDirectory> open(const std::string& session,std::uint32_t index,const ObjectSecurityFactory& security);
    // 接管傳入描述元；檢查失敗亦會關閉。
    static std::shared_ptr<ObjectDirectory> inherit(const std::string& session,int descriptor);
};
struct ObjectDirectoryHandle {std::shared_ptr<ObjectDirectory> object;std::uint32_t access{};bool operator==(const ObjectDirectoryHandle&)const=default;};
}
