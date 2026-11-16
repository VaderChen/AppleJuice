#pragma once
#include "x64arm/security_access.hpp"
#include "x64arm/security_descriptor.hpp"

namespace x64arm::detail {
inline constexpr security::AccessMapping volume_access_mapping{0x120089U,0x120116U,0x1200a0U,0x1f01ffU};
inline security::SecurityDescriptor volume_device_security(std::uint32_t information=7){
    security::SecurityDescriptor descriptor;
    const auto system=security::account_sid(*security::find_account("SYSTEM"));
    const auto admin=security::account_sid(*security::find_account("Administrators"));
    if(information&1U)descriptor.data[0]=system;
    if(information&2U)descriptor.data[1]=admin;
    if(information&4U){descriptor.control|=4;
        descriptor.data[3]=security::encode_acl({security::encode_ace({0,0,volume_access_mapping[3],system}),security::encode_ace({0,0,volume_access_mapping[3],admin}),
            security::encode_ace({0,0,0x120080U,security::account_sid(*security::find_account("Everyone"))})});
    }
    return descriptor;
}
}
