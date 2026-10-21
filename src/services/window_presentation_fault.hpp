#pragma once

#include <stdexcept>

namespace x64arm {
// 視窗關閉與 GPU 呈現可並行；過期 HWND 應交回 DXGI 錯誤，不終止客體程序。
class WindowPresentationFault final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
}
