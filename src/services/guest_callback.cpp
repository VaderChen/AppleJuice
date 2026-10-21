#include "x64arm/guest_callback.hpp"
#include "x64arm/state.hpp"
#include "x64arm/host_services.hpp"
extern "C" {
#include "softfloat.h"
}
namespace x64arm {
std::uint64_t floating_callback_result(const GuestState& state,unsigned width,bool x86){
    if(width!=32 && width!=64)throw HostCallFault("浮點 callback 回傳寬度不合法");
    if(!x86){std::uint64_t result=0;for(unsigned byte=0;byte<width/8;++byte)result|=std::uint64_t{state.xmm[0][byte]}<<(8*byte);return result;}
    const auto top=(state.x87_status>>11)&7U;if(((state.x87_tag>>(top*2))&3U)==3)throw HostCallFault("浮點 callback 缺少 ST(0) 回傳值");
    const auto rounding=softfloat_roundingMode,exceptions=softfloat_exceptionFlags;
    constexpr uint_fast8_t modes[]{softfloat_round_near_even,softfloat_round_min,softfloat_round_max,softfloat_round_minMag};
    softfloat_roundingMode=modes[(state.x87_control>>10)&3U];const auto value=state.x87[top];
    const auto result=width==32?extF80_to_f32({value.significand,value.sign_exponent}).v:extF80_to_f64({value.significand,value.sign_exponent}).v;
    softfloat_roundingMode=rounding;softfloat_exceptionFlags=exceptions;return result;
}
}
