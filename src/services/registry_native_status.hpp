#pragma once
#include <cstdint>
namespace x64arm {
inline std::uint32_t registry_native_status(std::uint32_t code){
    switch(code){case 0:return 0;case 2:return 0xc0000034U;case 3:return 0xc000003aU;case 5:return 0xc0000022U;case 6:return 0xc0000008U;
    case 8:return 0xc0000017U;case 13:return 0xc000003eU;case 32:return 0xc0000043U;case 33:return 0xc0000054U;case 50:return 0xc00000bbU;
    case 80:case 183:return 0xc0000035U;case 87:return 0xc000000dU;case 123:return 0xc0000033U;case 170:return 0x80000011U;
    case 234:return 0x80000005U;case 998:return 0xc0000005U;case 1015:return 0xc000014cU;case 1017:return 0xc000015cU;
    case 1018:return 0xc000017cU;case 1021:return 0xc0000181U;case 1314:return 0xc0000061U;case 1336:return 0xc0000077U;
    case 1337:return 0xc0000078U;case 1338:return 0xc0000079U;default:return 0xc0000001U;}
}
}
