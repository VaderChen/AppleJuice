#pragma once
#include <cstddef>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#include <sys/sysctl.h>
#endif

namespace x64arm {
namespace host_features_detail {
inline bool arm64_feature(const char* name) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    int value=0;std::size_t size=sizeof(value);
    return sysctlbyname(name,&value,&size,nullptr,0)==0 && size==sizeof(value) && value!=0;
#else
    (void)name;return false;
#endif
}
}
// 直譯器、JIT／AOT 與離線識別共用一次性查詢；core 不反向依賴 JIT 平台函式庫。
inline bool host_arm64_afp() {
    static const bool supported=host_features_detail::arm64_feature("hw.optional.arm.FEAT_AFP");
    return supported;
}
inline bool host_arm64_frintts() {
    static const bool supported=host_features_detail::arm64_feature("hw.optional.arm.FEAT_FRINTTS");
    return supported;
}
inline bool host_arm64_flagm() {
    static const bool supported=host_features_detail::arm64_feature("hw.optional.arm.FEAT_FlagM");
    return supported;
}
inline bool host_arm64_flagm2() {
    static const bool supported=host_features_detail::arm64_feature("hw.optional.arm.FEAT_FlagM2");
    return supported;
}
inline bool host_arm64_rpres() {
    static const bool supported=host_arm64_afp() && host_features_detail::arm64_feature("hw.optional.arm.FEAT_RPRES");
    return supported;
}
}
