#pragma once
#include "x64arm/host_services.hpp"
#include <array>
#include <cerrno>
#include <cfenv>
#include <cstdint>

namespace x64arm {
struct ResidentHostEnvironment;
// CRT 各數學入口共用宿主環境；客體例外的提交仍由各 ABI 處理。
// Darwin ARM64 的 fenv 例外位等同 FPSR，RC 則需交換 x86 的向上／向下位。
class WindowsMathEnvironment {
    int* const errno_address_{&errno};
    const int saved_error_{*errno_address_};
#if defined(__aarch64__) && defined(__APPLE__)
    std::uint64_t saved_fpcr_,saved_fpsr_;
    bool restore_environment_{true};
#else
    std::fenv_t saved_{};
#endif
public:
    explicit WindowsMathEnvironment(std::uint32_t mxcsr,const ResidentHostEnvironment* boundary=nullptr) {
#if defined(__aarch64__) && defined(__APPLE__)
        static_assert(FE_ALL_EXCEPT==0x9f && FE_TONEAREST==0 &&
                      FE_DOWNWARD==0x800000 && FE_UPWARD==0x400000 && FE_TOWARDZERO==0xc00000);
        // 常駐 managed 邊界提供原宿主環境，省略先回宿主、再設定計算環境的往返。
        // 核心負責正常／例外返回後的客體恢復，這裡只保留 errno 與原來的計算語義。
#ifdef X64ARM_HOST_SERVICES_MANAGED_FP
        if(boundary){saved_fpcr_=boundary->control;saved_fpsr_=boundary->status;restore_environment_=false;}
        else
#else
        (void)boundary;
#endif
        {asm volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(saved_fpcr_),"=r"(saved_fpsr_) :: "memory");}
        const auto rounding=((std::uint64_t{mxcsr}&0x2000)<<10)|((std::uint64_t{mxcsr}&0x4000)<<8);
        const auto control=(saved_fpcr_&~UINT64_C(0xc09f00))|rounding;
        const auto status=saved_fpsr_&~UINT64_C(0x9f);
        // 合併 holdexcept 與 setround；保留 QC、FZ、DN 與其他非例外控制位。
        asm volatile("msr fpsr, %0\nmsr fpcr, %1" :: "r"(status),"r"(control) : "memory");
#else
        (void)boundary;
        if(std::feholdexcept(&saved_))throw HostCallFault("無法保存 CRT 浮點環境");
        constexpr std::array modes{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
        std::fesetround(modes[(mxcsr>>13)&3]);
#endif
        *errno_address_=0;
    }
    WindowsMathEnvironment(const WindowsMathEnvironment&)=delete;
    WindowsMathEnvironment& operator=(const WindowsMathEnvironment&)=delete;
    ~WindowsMathEnvironment() {
#if defined(__aarch64__) && defined(__APPLE__)
        if(restore_environment_)asm volatile("msr fpsr, %0\nmsr fpcr, %1" :: "r"(saved_fpsr_),"r"(saved_fpcr_) : "memory");
#else
        std::fesetenv(&saved_);
#endif
        *errno_address_=saved_error_;
    }
    int error() const noexcept {return *errno_address_;}
    static int raised() noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
        std::uint64_t status;
        asm volatile("mrs %0, fpsr" : "=r"(status) :: "memory");
        return static_cast<int>(status)&FE_ALL_EXCEPT;
#else
        return std::fetestexcept(FE_ALL_EXCEPT);
#endif
    }
};
}
