#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 宿主 USB 裝置目錄（macOS IOKit IOUSBHostDevice）。只描述實際存在的裝置與介面，
// 供裝置管理建立 Windows 的 USB devnode；資料傳輸另由 WinUSB 後端處理。
namespace x64arm::usb {
struct Interface {
    unsigned number{},cls{},subclass{},protocol{};
    bool claimed{}; // 已有 macOS 驅動接管（例如 HID、大量儲存）
    std::string path;  // IOService plane 路徑；其下的 HID 等服務路徑以此為前綴
};
struct Device {
    std::uint64_t registry_id{},parent_id{}; // parent_id：上層 USB 裝置（集線器）；0 表示直接接在主控制器
    std::uint32_t location{},controller{};    // controller：主控制器的 locationID
    unsigned vendor{},product{},revision{},cls{},subclass{},protocol{},port{},speed{},address{};
    std::u16string product_name,vendor_name,serial;
    std::string path;
    std::vector<Interface> interfaces;
};
std::vector<Device> devices();
// 一次非同步傳輸；完成回呼在宿主佇列更新，服務層輪詢 done 後取回結果。
struct Transfer {
    std::atomic<bool> done{};
    std::uint32_t error{};       // Win32 錯誤碼
    std::uint32_t transferred{};
    std::vector<std::uint8_t> data;
};
// WinUSB 工作階段：只開啟沒有 macOS 驅動接管的介面（WinUSB 為該介面的功能驅動）。
class Session {
public:
    virtual ~Session()=default;
    virtual std::vector<std::uint8_t> device_descriptor()const=0;
    virtual std::vector<std::uint8_t> configuration_descriptor()const=0;
    virtual std::vector<unsigned> interfaces()const=0;
    virtual std::optional<unsigned> alternate(unsigned interface)const=0;
    virtual std::uint32_t select_alternate(unsigned interface,unsigned alternate)=0;
    // 控制傳輸（預設端點 0）；data 對 OUT 為送出內容、對 IN 為長度與接收區。
    virtual std::shared_ptr<Transfer> control(unsigned interface,std::uint8_t type,std::uint8_t request,std::uint16_t value,std::uint16_t index,std::vector<std::uint8_t> data,std::uint32_t timeout_ms)=0;
    virtual std::shared_ptr<Transfer> pipe(unsigned interface,std::uint8_t endpoint,std::vector<std::uint8_t> data,std::uint32_t timeout_ms)=0;
    virtual std::uint32_t abort(unsigned interface,std::uint8_t endpoint)=0;
    virtual std::uint32_t reset(unsigned interface,std::uint8_t endpoint)=0;
};
std::unique_ptr<Session> open(std::uint64_t registry_id,std::span<const unsigned> interfaces,std::uint32_t& error);
// 裝置介面路徑對應的開啟目標：WinUSB 函式（未被 macOS 接管的介面）或一般 USB 裝置。
struct Target {std::uint64_t registry_id{};std::vector<unsigned> interfaces;bool winusb{};unsigned speed{};};
// \\?\usb#… 形式的 USB 裝置介面路徑。
bool interface_path(std::u16string_view);
}
