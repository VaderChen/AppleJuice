#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unicode/utypes.h>

namespace x64arm::icu_resource_files {
using Bytes=std::vector<std::uint8_t>;
using Files=std::map<std::string,Bytes,std::less<>>;

// Windows ICU 要求相容的 little-endian 資料。檔名僅使用 ICU 的不變字元集合。
bool item_name(std::string_view name);
Files unpack(std::span<const std::uint8_t> bytes,std::string_view tree,UErrorCode& status);
bool validate(Files& files,std::string_view package,UErrorCode& status);

struct Package {
    std::filesystem::path directory;
    std::string native_path;
    std::string direct_locale;

    Package()=default;
    Package(const Package&)=delete;
    Package& operator=(const Package&)=delete;
    ~Package();
};

std::shared_ptr<const Package> stage(const Files& files,std::string direct_locale,UErrorCode& status);
}
