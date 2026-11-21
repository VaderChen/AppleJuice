#include "builtin_pe_images.hpp"
#include "x64arm/pe_loader.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace x64arm::detail {
namespace {
using Bytes = std::span<const std::uint8_t>;
struct BuiltinImage {
    std::string_view module;
    Bytes x86,x64;
};
// 目前沒有隨附的客體 PE 映像；查詢一律回「沒有」，由一般模組搜尋與內建服務承接。
const std::array<BuiltinImage,0> images{};

auto find_image(std::string_view module) {
    return std::find_if(images.begin(),images.end(),[&](const auto& image){return image.module==module;});
}
}

std::span<const std::uint8_t> builtin_pe_bytes(std::string_view module, bool x86) {
    const auto entry=find_image(module);
    if(entry==images.end())return {};
    return x86?entry->x86:entry->x64;
}

bool builtin_pe_has_export(std::string_view module, std::string_view symbol, bool x86) {
    const auto entry=find_image(module);
    if(entry==images.end())return false;
    // 盤點直接查實際映像的 EAT，名稱與序號沿用一般 PE 的解析結果。
    static const auto tables=[] {
        std::array<std::array<PeInventory,2>,images.size()> result;
        for(std::size_t i=0;i<images.size();++i)
            result[i]={inspect_pe_image(images[i].x86),inspect_pe_image(images[i].x64)};
        return result;
    }();
    const auto& exports=tables[static_cast<std::size_t>(entry-images.begin())][x86?0:1];
    if(!symbol.starts_with('#'))return exports.export_names.contains(std::string(symbol));
    symbol.remove_prefix(1);
    std::uint16_t ordinal{};
    const auto [end,error]=std::from_chars(symbol.data(),symbol.data()+symbol.size(),ordinal);
    return error==std::errc{} && end==symbol.data()+symbol.size() && exports.export_ordinals.contains(ordinal);
}
}
