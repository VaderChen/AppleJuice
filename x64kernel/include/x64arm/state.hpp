#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace x64arm {
enum class ExecutionMode : std::uint8_t { X86, X64 };
struct GuestAddress {
    std::uint64_t value{};
    explicit constexpr GuestAddress(std::uint64_t address = 0) : value(address) {}
    auto operator<=>(const GuestAddress&) const = default;
};
enum class Gpr : std::uint8_t { Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15 };
inline constexpr std::array<std::string_view, 16> gpr_names = {
    "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
struct RegisterRef {
    Gpr index{Gpr::Rax};
    std::uint8_t width{64};
    bool high8{};
};
namespace flags {
inline constexpr std::uint64_t CF = 1ULL << 0, PF = 1ULL << 2, AF = 1ULL << 4;
inline constexpr std::uint64_t ZF = 1ULL << 6, SF = 1ULL << 7, OF = 1ULL << 11;
inline constexpr std::uint64_t arithmetic = CF | PF | AF | ZF | SF | OF;
inline constexpr std::uint64_t logical = arithmetic & ~AF;
inline constexpr std::uint64_t DF = 1ULL << 10;
}
bool valid_width(std::uint8_t width) noexcept;
std::uint64_t width_mask(std::uint8_t width);
struct ExtendedFloat {
    std::uint64_t significand{};
    std::uint16_t sign_exponent{};
    bool operator==(const ExtendedFloat&) const = default;
};
struct GuestState {
    std::array<std::uint64_t, 16> gpr{};
    GuestAddress rip{};
    std::uint64_t rflags{2};
    std::array<std::array<std::uint8_t, 16>, 16> xmm{};
    std::uint64_t fs_base{}, gs_base{};
    std::uint16_t x87_control{0x037f}, x87_status{}, x87_tag{0xffff};
    std::uint32_t mxcsr{0x1f80};
    std::array<ExtendedFloat, 8> x87{};
    std::uint64_t tsc{};
    std::uint32_t processor_id{};
    // ES、CS、SS、DS、FS、GS；selector 與線性位址基底分開保存。
    std::array<std::uint16_t,6> segments{};
    std::uint64_t x87_instruction_pointer{}, x87_data_pointer{};
    std::uint16_t x87_opcode{}, x87_code_selector{}, x87_data_selector{};
    // CPU trap 已完成指令，協調執行緒在下一條指令前派送；高 DWORD 為指令長度。
    std::uint64_t pending_cpu_trap{};
    // REP 比較的內部續跑資訊；隨客體執行緒／快照複製，不是額外架構暫存器。
    // 保存開始前旗標及上一個已完成元素的狀態，以區分切片續跑與新的指令執行。
    struct RepCompare {
        std::uint64_t initial_flags{},expected_flags{},rip{},remaining{},source{},destination{},operand{};
        std::uint32_t control{};
        bool operator==(const RepCompare&) const = default;
    } rep_compare;
    // DR0–DR3、DR6、DR7 隨架構 context 保存；啟用 comparator 前須有 CPU trap 支援。
    std::array<std::uint64_t,6> debug_registers{0,0,0,0,UINT64_C(0xffff0ff0),0x400};
    // YMM[127:0] 與既有 XMM 共用；高半部附加於尾端，保留既有 resident 欄位位移。
    // Legacy SSE 不改寫此區，VEX.128 的目的寫入必須將對應高半部清零。
    alignas(16) std::array<std::array<std::uint8_t,16>,16> ymm_upper{};
    std::uint64_t& reg(Gpr index) { return gpr.at(static_cast<std::size_t>(index)); }
    std::uint64_t reg(Gpr index) const { return gpr.at(static_cast<std::size_t>(index)); }
    std::uint64_t read(RegisterRef ref) const;
    void write(RegisterRef ref, std::uint64_t value);
    bool operator==(const GuestState&) const = default;
};
}
