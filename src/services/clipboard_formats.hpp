#pragma once
#include <cstdint>
#include <string>

namespace x64arm {
struct GuiStationNamespace;
// name 為空時依 id 查詢；成功後可由 station 的名稱表取得原始拼寫。
std::uint32_t clipboard_format_id(GuiStationNamespace& station, const std::string& directory,
                                  std::u16string_view station_name, std::u16string_view name,
                                  std::uint32_t id);
}
