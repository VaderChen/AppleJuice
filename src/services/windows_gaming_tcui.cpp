#include "x64arm/windows_services.hpp"

#include <vector>

namespace x64arm {
// Xbox 遊戲 UI（gamingtcui.dll；api-ms-win-gaming-tcui-l1-1-0 由 API Set 轉到這裡）。
// Show*UI 在 Windows 上非同步開啟 Xbox 的系統 UI，完成常式只在呼叫端執行 ProcessPendingGameUI 時於該執行緒上被呼叫。
// 沙箱沒有 Xbox 帳戶與系統 UI：Show*UI 檢查參數後排入一筆待完成的 UI，下一次 ProcessPendingGameUI 以「使用者取消」
// （HRESULT_FROM_WIN32(ERROR_CANCELLED)）依提出順序呼叫完成常式；TryCancelPendingGameUI 讓待完成的 UI 改回 E_ABORT。
std::span<const WindowsServiceDescriptor> gaming_tcui_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gamingtcui.dll","ProcessPendingGameUI",1},{"gamingtcui.dll","TryCancelPendingGameUI",0},
        {"gamingtcui.dll","ShowPlayerPickerUI",9},{"gamingtcui.dll","ShowProfileCardUI",3},
        {"gamingtcui.dll","ShowChangeFriendRelationshipUI",3},{"gamingtcui.dll","ShowTitleAchievementsUI",3},
        {"gamingtcui.dll","ShowGameInviteUI",6},
    };
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gaming_tcui(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const std::string_view symbol=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    constexpr std::uint32_t invalid_argument=0x80070057U,cancelled=0x800704C7U,aborted=0x80004004U;
    if(symbol=="ProcessPendingGameUI"){
        // waitForCompletion 不影響結果：沒有實際 UI，待完成的 UI 都在這次呼叫內完成。
        if(pending_game_ui_.empty())return finish(0);
        std::vector<GuestCallback> callbacks;
        for(const auto& ui:pending_game_ui_){
            if(ui.picker)callbacks.push_back({ui.routine,{ui.result,ui.context,0,0}}); // 沒有選取任何玩家。
            else callbacks.push_back({ui.routine,{ui.result,ui.context}});
        }
        pending_game_ui_.clear();
        start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
    }
    if(symbol=="TryCancelPendingGameUI"){
        for(auto& ui:pending_game_ui_)ui.result=aborted;
        return finish(pending_game_ui_.empty()?0U:1U);
    }
    // 各 Show*UI 的完成常式與 context 位置；HSTRING 的 NULL 等同空字串。
    unsigned routine=1;bool picker=false;
    if(symbol=="ShowPlayerPickerUI"){
        // (prompt, xuids, xuidsCount, preSelected, preSelectedCount, minSelection, maxSelection, routine, context)
        routine=7;picker=true;const auto count=arg(2),preselected=arg(4),minimum=arg(5),maximum=arg(6);
        if((count && !arg(1)) || (preselected && !arg(3)) || preselected>count || minimum>maximum || maximum>count)return finish(invalid_argument);
    }else if(symbol=="ShowProfileCardUI" || symbol=="ShowChangeFriendRelationshipUI"){
        if(!arg(0))return finish(invalid_argument); // 目標玩家的 XUID 不可為空。
    }else if(symbol=="ShowGameInviteUI"){
        // (serviceConfigurationId, sessionTemplateName, sessionId, invitationDisplayText, routine, context)
        routine=4;if(!arg(0) || !arg(1) || !arg(2))return finish(invalid_argument);
    }
    if(!arg(routine))return finish(invalid_argument);
    if(pending_game_ui_.size()>=64)return finish(0x8007000EU);
    pending_game_ui_.push_back({arg(routine),arg(routine+1),cancelled,picker});
    return finish(0);
}
}
