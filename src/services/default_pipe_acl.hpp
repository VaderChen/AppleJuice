#pragma once
#include "x64arm/security_access.hpp"
#include "x64arm/security_descriptor.hpp"

namespace x64arm {
// 具名管道的通用權限對應（FILE_GENERIC_READ／WRITE／EXECUTE、FILE_ALL_ACCESS）。
inline constexpr security::AccessMapping pipe_access_mapping{0x120089U,0x120116U,0x1200a0U,0x1f01ffU};
// 具名管道的預設 DACL：CreateNamedPipe 未帶安全描述元時與 RtlDefaultNpAcl 共用。LocalSystem、Administrators 與建立者
// （權杖擁有者 SID）完全控制，Everyone 與匿名登入讀取；權限已依管道的通用對應展開。
inline security::Bytes default_pipe_acl(const security::Bytes& owner){
    const auto known=[](std::string_view name){return security::account_sid(*security::find_account(name));};
    return security::encode_acl({
        security::encode_ace({0,0,pipe_access_mapping[3],known("SYSTEM")}),
        security::encode_ace({0,0,pipe_access_mapping[3],known("Administrators")}),
        security::encode_ace({0,0,pipe_access_mapping[3],owner}),
        security::encode_ace({0,0,pipe_access_mapping[0],known("Everyone")}),
        security::encode_ace({0,0,pipe_access_mapping[0],known("Anonymous Logon")})});
}
}
