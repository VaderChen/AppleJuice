#pragma once
#include "x64arm/windows_gui.hpp"

namespace x64arm {
inline constexpr std::uint64_t gui_native_poll_deferred=UINT64_MAX;
// 自身啟用狀態與 WS_DISABLED 是同一份契約；父視窗的限制另由有效狀態查詢處理。
inline void gui_set_window_enabled(GuiWindow& window,bool enabled){
    window.enabled=enabled;
    window.style=(window.style&~0x08000000U)|(enabled?0U:0x08000000U);
}
// 祖先只限制輸入，不改寫子視窗本身的 WS_DISABLED 或 IsWindowEnabled 結果。
inline bool gui_window_input_enabled(const GuiState& gui,std::uint64_t handle){
    for(std::size_t depth=0;handle && depth<=gui.windows.size();++depth){
        if(handle==gui.desktop)return true;
        const auto window=gui.windows.find(handle);
        if(window==gui.windows.end() || !window->second.enabled)return false;
        handle=window->second.parent;
    }
    return handle==0;
}
inline void gui_cancel_window_menu(GuiState& gui,std::uint64_t owner){
    for(auto& [token,track]:gui.menu_tracks){(void)token;if(track.owner==owner && track.active)track.cancel();}
}
}
