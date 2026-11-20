#pragma once
#include "x64arm/shared_event.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm {
// 引擎既有全域／session 具名物件的目錄；獨立控制代碼，不能當作可等待事件。
// 索引 0～4 沿用既有紀錄（object-directory-<n>）；5 起為新增的內建目錄，不改變既有索引。
inline constexpr std::array<std::u16string_view,10> builtin_object_directories{
    u"\\",u"\\Sessions",u"\\Sessions\\1",u"\\Sessions\\1\\BaseNamedObjects",u"\\BaseNamedObjects",u"\\GLOBAL??",u"\\KnownDlls",u"\\KnownDlls32",u"\\Sessions\\1\\Windows",u"\\Sessions\\1\\Windows\\WindowStations"};
enum class ObjectNamespaceKind:std::uint32_t{Directory=1,SymbolicLink=2};
// 命名空間項目旗標：永久物件、私有命名空間（不經路徑查詢、沒有顯示名稱）、內建符號連結（隨需建立、永久）、無名項目（鍵為內部路徑）。
inline constexpr std::uint32_t object_namespace_permanent=1,object_namespace_private=2,object_namespace_builtin=4,object_namespace_anonymous=8;
// 動態目錄、符號連結與私有命名空間在 session 目錄中的共享紀錄。path 為保留大小寫的完整路徑（私有命名空間為內部路徑），
// target 為連結目標（私有命名空間保存邊界描述元的正規化鍵）；serial 是建立時的隨機身分，同名重建後不會沿用舊物件。
struct ObjectNamespaceEntry {
    std::u16string path,target;
    ObjectNamespaceKind kind{ObjectNamespaceKind::Directory};
    std::uint32_t flags{};
    std::uint64_t created{},serial{};
};
inline constexpr std::size_t object_namespace_path_limit=1024,object_namespace_target_limit=4096;
class ObjectDirectory {
    int file_{-1},directory_{-1};
    std::uint32_t index_{};
    ObjectNamespaceEntry entry_;
    std::string file_name_;
    bool counted_{};
public:
    static constexpr std::uint32_t dynamic_index=UINT32_MAX;
    ~ObjectDirectory();
    ObjectDirectory()=default;
    ObjectDirectory(const ObjectDirectory&)=delete;
    ObjectDirectory& operator=(const ObjectDirectory&)=delete;
    int descriptor() const noexcept{return file_;}
    int lock_descriptor() const noexcept{return directory_;}
    std::uint32_t index() const noexcept{return index_;}
    bool dynamic() const noexcept{return index_==dynamic_index;}
    ObjectNamespaceKind kind() const noexcept{return dynamic()?entry_.kind:ObjectNamespaceKind::Directory;}
    bool symbolic_link() const noexcept{return kind()==ObjectNamespaceKind::SymbolicLink;}
    bool private_namespace() const noexcept{return dynamic() && (entry_.flags&object_namespace_private);}
    // 內部完整路徑（具名物件以此為根）；name() 為 NtQueryObject 顯示的名稱，私有命名空間沒有名稱。
    std::u16string_view path() const noexcept{return dynamic()?std::u16string_view(entry_.path):builtin_object_directories[index_];}
    std::u16string_view name() const noexcept{return private_namespace()?std::u16string_view{}:path();}
    std::u16string_view target() const noexcept{return entry_.target;}
    const ObjectNamespaceEntry& entry() const noexcept{return entry_;}
    // 一般存取對應（GENERIC_READ／WRITE／EXECUTE／ALL）：目錄與符號連結不同。
    std::array<std::uint32_t,4> generic_mapping() const noexcept{
        return symbolic_link()?std::array<std::uint32_t,4>{0x20001U,0x20000U,0x20000U,0xf0001U}:std::array<std::uint32_t,4>{0x20001U,0x2000cU,0x20002U,0xf000fU};}
    std::uint32_t valid_access() const noexcept{return symbolic_link()?0xf0001U:0xf000fU;}
    static std::shared_ptr<ObjectDirectory> open(const std::string& session,std::uint32_t index,const ObjectSecurityFactory& security);
    // 接管傳入描述元；檢查失敗亦會關閉。
    static std::shared_ptr<ObjectDirectory> inherit(const std::string& session,int descriptor);
    // 以正規化鍵開啟動態項目；create 非空時不存在就建立。成功回傳 status 0；已存在時 existed=true（呼叫端依 OBJ_OPENIF 決定）。
    // 不存在回 STATUS_OBJECT_NAME_NOT_FOUND；鍵或種類不符回 STATUS_OBJECT_TYPE_MISMATCH。暫時項目在最後一個控制代碼關閉時刪除。
    struct Opened {std::shared_ptr<ObjectDirectory> object;std::uint32_t status{};bool existed{};};
    static Opened open_entry(const std::string& session,std::u16string_view key,const ObjectNamespaceEntry* create,const ObjectSecurityFactory& security);
    // 改變永久旗標；改為暫時且已無其他參考時於關閉後刪除。
    std::uint32_t set_permanent(bool permanent);
    // 內建連結的目標隨卷配置更新。
    std::uint32_t update_target(std::u16string_view target);
    // 讓私有命名空間不再能被開啟；既有控制代碼與其中物件維持有效。
    std::uint32_t remove_name();
    // 目前 session 的全部動態項目（含私有命名空間）；只讀紀錄，不改變參考數。
    static std::vector<ObjectNamespaceEntry> list_entries(const std::string& session);
    // 命名空間鍵：路徑大小寫折疊；私有命名空間以前置 U+0001 與邊界鍵區隔。
    static std::u16string path_key(std::u16string_view path);
    static std::u16string entry_key(const ObjectNamespaceEntry& entry){
        return (entry.flags&object_namespace_private) && !(entry.flags&object_namespace_anonymous)?u"\x01"+entry.target:path_key(entry.path);}
};
struct ObjectDirectoryHandle {std::shared_ptr<ObjectDirectory> object;std::uint32_t access{};bool operator==(const ObjectDirectoryHandle&)const=default;};
}
