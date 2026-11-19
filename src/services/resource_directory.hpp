#pragma once
#include <cstdint>
#include <span>

namespace x64arm {
class GuestMemory;
// 客體記憶體中映像的 .rsrc 目錄走訪結果：root 為資源目錄根位址；depth 為成功比對的層數（全部找到時等於 keys 數）；
// address 為最後找到的項目目標（目錄或 IMAGE_RESOURCE_DATA_ENTRY），directory 表示它是子目錄。present 為映像是否帶資源目錄。
struct ResourceLookup {std::uint64_t root{},address{};std::size_t depth{};bool directory{},present{};};
// keys 依序為 Type、Name、Language：ID（≤ 0xffff）或名稱字串指標（wide 決定寬字元或 ANSI），名稱不分大小寫；Language 只比對 ID。
// 目錄格式不合法（超出 .rsrc 範圍）時丟出 HostCallFault。
ResourceLookup find_resource_entry(const GuestMemory& memory,std::uint64_t base,std::uint64_t size,std::span<const std::uint64_t> keys,bool wide);
// 單一目錄項目名稱（IMAGE_RESOURCE_DIRECTORY_ENTRY.Name）是否符合要求的 ID 或字串；gui_resource 與 find_resource_entry 共用。
bool resource_name_matches(const GuestMemory& memory,std::uint64_t root,std::uint64_t extent,std::uint32_t entry,std::uint64_t requested,bool wide);
}
