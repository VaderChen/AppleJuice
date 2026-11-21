#pragma once

#define D3DX10_SDK_VERSIONS(apply) \
    apply(33) apply(34) apply(35) apply(36) apply(37) apply(38) \
    apply(39) apply(40) apply(41) apply(42) apply(43)

static inline unsigned d3dx10_check_version(unsigned d3d, unsigned d3dx, unsigned expected){
    return d3d==29 && d3dx==expected;
}
