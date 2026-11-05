#pragma once
#include "x64arm/memory.hpp"
#include <vector>
#include <string>
namespace x64arm {
// 對話框、spooler 與 GDI DC 共用同一份可實際輸出 PDF 的設定。
struct PrinterSettings {
    unsigned width{816},height{1056},copies{1};bool monochrome{},collate{};
    unsigned paper{1},paper_width{215900},paper_height{279400};
    std::u16string form{u"Letter"};bool landscape{};
    bool operator==(const PrinterSettings&)const=default;
};
std::vector<std::uint8_t> printer_devmode(bool,const PrinterSettings&);
}
