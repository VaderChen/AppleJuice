#pragma once
#include <cstdint>
namespace x64arm::desktop {
// 由 WinSDK shellapi.h 的欄位順序及 MSVC x86／x64 對齊產生。
struct Layout {
    std::uint32_t owner,id,flags,callback,icon,tip,state,info,version,title,info_flags,guid,balloon,v1,v2,v3,size;
    constexpr Layout(bool x86,bool wide):owner(x86?4:8),id(owner+(x86?4:8)),flags(id+4),callback(flags+4),icon(x86?20:32),tip(icon+(x86?4:8)),
        state(tip+128*(wide?2:1)),info(state+8),version(info+256*(wide?2:1)),title(version+4),info_flags(title+64*(wide?2:1)),
        guid(info_flags+4),balloon(guid+16),v1(tip+64*(wide?2:1)),v2(guid),v3(balloon),size(balloon+(x86?4:8)){}
    constexpr bool accepts(std::uint32_t cb)const{return cb==v1 || cb==v2 || cb==v3 || cb==size;}
};
}
