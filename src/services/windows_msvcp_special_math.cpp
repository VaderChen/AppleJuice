#include "x64arm/windows_services.hpp"
#include "special_math.hpp"
#include <bit>

namespace x64arm {
void windows_floating_return(GuestState& state,bool x86,double value,bool single);
// msvcp140_2.dll：C++17 特殊數學函式。匯出為 __stdcall，x86 帶 @位元組數裝飾；f 字尾是 float 版本，共用 double 實作後窄化。
#define X64ARM_SMF2(name,args,mask,bytes,bytesf) \
    {"msvcp140_2.dll","__std_smf_" name,args,mask},{"msvcp140_2.dll","__std_smf_" name "f",args}, \
    {"msvcp140_2.dll","__std_smf_" name "@" bytes,args,mask},{"msvcp140_2.dll","__std_smf_" name "f@" bytesf,args}
std::span<const WindowsServiceDescriptor> msvcp_special_math_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        X64ARM_SMF2("assoc_laguerre",3,4,"16","12"),X64ARM_SMF2("assoc_legendre",3,4,"16","12"),
        X64ARM_SMF2("beta",2,3,"16","8"),
        X64ARM_SMF2("comp_ellint_1",1,1,"8","4"),X64ARM_SMF2("comp_ellint_2",1,1,"8","4"),X64ARM_SMF2("comp_ellint_3",2,3,"16","8"),
        X64ARM_SMF2("cyl_bessel_i",2,3,"16","8"),X64ARM_SMF2("cyl_bessel_j",2,3,"16","8"),X64ARM_SMF2("cyl_bessel_k",2,3,"16","8"),X64ARM_SMF2("cyl_neumann",2,3,"16","8"),
        X64ARM_SMF2("ellint_1",2,3,"16","8"),X64ARM_SMF2("ellint_2",2,3,"16","8"),X64ARM_SMF2("ellint_3",3,7,"24","12"),
        X64ARM_SMF2("expint",1,1,"8","4"),
        X64ARM_SMF2("hermite",2,2,"12","8"),X64ARM_SMF2("laguerre",2,2,"12","8"),X64ARM_SMF2("legendre",2,2,"12","8"),
        X64ARM_SMF2("riemann_zeta",1,1,"8","4"),
        X64ARM_SMF2("sph_bessel",2,2,"12","8"),X64ARM_SMF2("sph_legendre",3,4,"16","12"),X64ARM_SMF2("sph_neumann",2,2,"12","8"),
        X64ARM_SMF2("hypot3",3,7,"24","12"),
    };
    return table;
}
#undef X64ARM_SMF2

std::optional<std::uint32_t> WindowsServices::invoke_msvcp_special_math(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    auto name=service.symbol.substr(0,service.symbol.find('@'));
    name.remove_prefix(std::string_view("__std_smf_").size());
    const bool single=name.ends_with('f');
    if(single)name.remove_suffix(1);
    const auto real=[&](unsigned index){
        if(x86_)return single?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(arg(index)))):std::bit_cast<double>(arg(index));
        std::uint64_t raw=0;for(unsigned i=0;i<(single?4U:8U);++i)raw|=std::uint64_t{state.xmm[index][i]}<<(i*8);
        return single?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))):std::bit_cast<double>(raw);
    };
    const auto integer=[&](unsigned index){return static_cast<unsigned>(arg(index));};
    namespace math=special_math;
    double value=0;
    if(name=="assoc_laguerre")value=math::assoc_laguerre(integer(0),integer(1),real(2));
    else if(name=="assoc_legendre")value=math::assoc_legendre(integer(0),integer(1),real(2));
    else if(name=="beta")value=math::beta(real(0),real(1));
    else if(name=="comp_ellint_1")value=math::comp_ellint_1(real(0));
    else if(name=="comp_ellint_2")value=math::comp_ellint_2(real(0));
    else if(name=="comp_ellint_3")value=math::comp_ellint_3(real(0),real(1));
    else if(name=="cyl_bessel_i")value=math::cyl_bessel_i(real(0),real(1));
    else if(name=="cyl_bessel_j")value=math::cyl_bessel_j(real(0),real(1));
    else if(name=="cyl_bessel_k")value=math::cyl_bessel_k(real(0),real(1));
    else if(name=="cyl_neumann")value=math::cyl_neumann(real(0),real(1));
    else if(name=="ellint_1")value=math::ellint_1(real(0),real(1));
    else if(name=="ellint_2")value=math::ellint_2(real(0),real(1));
    else if(name=="ellint_3")value=math::ellint_3(real(0),real(1),real(2));
    else if(name=="expint")value=math::expint(real(0));
    else if(name=="hermite")value=math::hermite(integer(0),real(1));
    else if(name=="laguerre")value=math::laguerre(integer(0),real(1));
    else if(name=="legendre")value=math::legendre(integer(0),real(1));
    else if(name=="riemann_zeta")value=math::riemann_zeta(real(0));
    else if(name=="sph_bessel")value=math::sph_bessel(integer(0),real(1));
    else if(name=="sph_legendre")value=math::sph_legendre(integer(0),integer(1),real(2));
    else if(name=="sph_neumann")value=math::sph_neumann(integer(0),real(1));
    else if(name=="hypot3")value=math::hypot3(real(0),real(1),real(2));
    else throw HostCallFault("未知的 msvcp140_2 服務");
    windows_floating_return(state,x86_,value,single);
    arg.return_to(state,0);
    return std::nullopt;
}
}
