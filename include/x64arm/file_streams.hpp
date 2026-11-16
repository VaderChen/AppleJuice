#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::file_streams {
// 剖析最後一個檔名的 NTFS $DATA 名稱；不把 DOS 磁碟分隔符當成串流。
struct Path {std::string base,name;std::uint32_t error{};bool specified{};};
Path parse(std::string_view path);
std::string folded(std::string_view name);
std::string storage_key(std::string_view name);
std::pair<std::string,std::uint32_t> descriptor_name(int descriptor);
inline constexpr std::string_view store_name=".x64arm-streams-v1";

// 描述元能力由呼叫端授權；儲存層不授予 Windows ACL 權限。
// 資料只寫入 sandbox 根目錄的私有側錄區，不修改基底檔案或其 xattr。
struct Directory {
    int descriptor{-1};std::uint32_t error{};
    Directory()=default;
    Directory(int fd,std::uint32_t code):descriptor(fd),error(code){}
    Directory(const Directory&)=delete;Directory& operator=(const Directory&)=delete;
    Directory(Directory&&) noexcept;Directory& operator=(Directory&&) noexcept;
    ~Directory();
};
struct Open {int descriptor{-1};std::uint32_t error{};bool existed{};};
struct Entry {std::string name;std::uint64_t size{},allocation{};};
struct Listing {std::vector<Entry> entries;std::uint32_t error{};};
Directory directory(int sandbox_root,int base_descriptor,bool create);
Open open(const Directory&,std::string_view name,int native_flags,bool create,bool exclusive);
Listing list(const Directory&);
std::uint32_t remove(const Directory&,std::string_view name);
std::uint32_t rename(const Directory&,std::string_view source,std::string_view target,bool replace);
}
