#pragma once
#include <cstdint>
namespace x64arm {
struct GuestState;
class GuestMemory;
struct WindowsServiceDescriptor;
struct ScalarMathOperation;
struct ResidentHostEnvironment;
struct ResidentScalarCall;
struct ResidentScalarErrors;
const ScalarMathOperation* scalar_math_operation(const WindowsServiceDescriptor&);
bool scalar_math_requested() noexcept;
// 回傳核心 ScalarRecipe 編號（0 表示沒有配方）；與 scalar_math_operation 使用同一登錄表。
unsigned scalar_math_recipe(const WindowsServiceDescriptor&) noexcept;
bool invoke_native_scalar_math(ResidentScalarCall&,GuestMemory&,const ScalarMathOperation&,std::uint64_t errno_cell) noexcept;
bool invoke_native_context_scalar_math(ResidentScalarCall&,GuestMemory&,const ScalarMathOperation&,
                                       const ResidentScalarErrors&) noexcept;
void invoke_scalar_math(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,
                        const ScalarMathOperation&,std::uint64_t errno_cell,bool x86,const ResidentHostEnvironment* = nullptr);
}
