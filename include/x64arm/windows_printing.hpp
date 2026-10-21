#pragma once
#include "x64arm/memory.hpp"
#include <vector>
namespace x64arm {
// 對話框、spooler 與 GDI DC 共用同一份可實際輸出 PDF 的設定。
struct PrinterSettings {
    unsigned width{816},height{1056},copies{1};bool monochrome{},collate{};
    bool operator==(const PrinterSettings&)const=default;
};
unsigned read_printer_settings(GuestMemory&,std::uint64_t,bool,PrinterSettings&);
std::vector<std::uint8_t> printer_devmode(bool,const PrinterSettings&);
}
