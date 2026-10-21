#pragma once
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace x64arm {
// 僅供明確啟用的原始程式驗證：完整 Present 後走正常 WM_CLOSE 訊息路徑。
// 不截斷繪製、不改客體回傳值；預設沒有幀數限制。
inline std::uint64_t graphics_close_frame_limit() {
    static const auto limit=[] {
        const auto* value=std::getenv("X64ARM_GRAPHICS_CLOSE_AFTER_FRAMES");
        if(!value || !*value)return std::uint64_t{};
        const std::string_view text(value);std::uint64_t count{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),count);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
            throw std::invalid_argument("繪圖測試關閉幀數必須為非負整數");
        return count;
    }();
    return limit;
}
template<class PostClose>
void graphics_close_after_present(std::uint64_t window,std::uint64_t frames,PostClose&& post) {
    const auto limit=graphics_close_frame_limit();
    if(!limit || frames!=limit || !post(window))return;
    std::fprintf(stderr,"[graphics-frame-limit] window=%llx frames=%llu message=WM_CLOSE\n",
                 static_cast<unsigned long long>(window),static_cast<unsigned long long>(frames));
}
}
