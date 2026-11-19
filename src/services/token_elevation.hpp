#pragma once
// 已提升的系統管理員權杖：完整性等級至少為高（0x3000），且 Administrators 群組已啟用（SE_GROUP_ENABLED，不是只供拒絕的群組）。
// 裝置組態的啟用／停用、Winsock 目錄設定等需要系統管理員權限的作業共用。
#include "x64arm/windows_tokens.hpp"
#include "x64arm/security_descriptor.hpp"
#include <algorithm>

namespace x64arm {
inline bool elevated_administrator(const VirtualToken& token){
    const auto* administrators=security::find_account("Administrators");if(!administrators)return false;
    const auto sid=security::account_sid(*administrators);
    return token.integrity>=0x3000U && std::any_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==sid && (group.attributes&0x14U)==4U;});
}
}
