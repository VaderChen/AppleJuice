#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x64arm {
// 客體指標與檔案路徑由 Windows 層驗證；原生層只接收擁有生命週期的資料。
struct NativeHelpRequest {
    std::uint64_t owner{};
    std::uint32_t command{},data{},codepage{1252};
    std::string file;
    std::u16string keyword;
    std::vector<std::uint8_t> bytes;
};
class NativeWinHelp {
public:
    virtual ~NativeWinHelp()=default;
    virtual std::uint32_t execute(const NativeHelpRequest&,void* anchor_view)=0;
};
std::shared_ptr<NativeWinHelp> make_native_winhelp();
}
