#pragma once
#import <AppKit/AppKit.h>
#include "../../services/winhelp_document.hpp"
namespace x64arm::winhelp {
// 呼叫端提供 Windows 碼頁；連結保留資料，不交由系統開啟 URL。
NSAttributedString* attributed_page(const Document&,std::size_t page,std::uint32_t codepage);
}
