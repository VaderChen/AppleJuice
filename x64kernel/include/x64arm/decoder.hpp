#pragma once
#include <unordered_map>
#include <deque>
#include "x64arm/memory.hpp"
#include <optional>
#include <span>
#include <string>

namespace x64arm {
enum class Opcode {
    Mov, Lea, Add, Sub, Cmp, Test, And, Or, Xor, Inc, Dec,
    Jmp, Je, Jne, Jl, Jge, Jle, Jg, Jb, Jae, Jbe, Ja,
    Push, Pop, Call, Ret, Nop,
    Movzx, Movsx, Adc, Sbb, Neg, Not, Shl, Shr, Sar, Imul, Cmov, Setcc, Jcc, Xchg, Bswap,
    VectorMove, VectorXor, VectorScalarMove, Movs, Stos, Cmps, Scas, Lods, Cld, Std,
    Cmpxchg, Xadd, SignExtend, SignExtendPair, Fninit, ScalarMove, UnpackLow, MulWide, ImulWide, Div, Idiv,
    UnpackHigh, ShufflePairs, ShuffleDwords, ShuffleLowWords, ShuffleHighWords, VectorAnd, VectorOr, VectorAndNot, Rol, Ror, Rcl, Rcr, Shld, Shrd, MoveHigh, MoveLow, MoveLH, MoveHL,
    FpAdd, FpSub, FpMul, FpDiv, FpSqrt, FpMin, FpMax, FpCompare, FpCompareQuiet, FpMaskScalar, FpMaskPacked,
    FpFromInt, FpToInt, FpTruncToInt, FpWiden, FpNarrow,
    FpPackedFromInt, FpPackedToInt, FpPackedTruncToInt, FpPackedAdd, FpPackedSub, FpPackedMul, FpPackedDiv, FpPackedAddSub, FpPackedSqrt, FpPackedHorizontalAdd, FpPackedHorizontalSub,
    FpPackedWiden, FpPackedNarrow, Bt, Bts, Btr, Btc, Bsf, Bsr, Cpuid,
    VectorEqual, VectorGreater, VectorMask, VectorAdd, VectorSub, VectorAddUnsignedSaturate, VectorSubUnsignedSaturate, VectorAddSignedSaturate, VectorSubSignedSaturate, VectorMulLow, VectorMulHighUnsigned, VectorMulHighSigned, VectorShl, VectorShr, VectorSar, VectorByteShl, VectorByteShr, VectorPackSigned, VectorPackUnsigned, VectorMulEvenUnsigned,
    LoadX87Control, StoreX87Control, StoreX87Status, LoadMxcsr, StoreMxcsr, WaitX87, PushFlags, PopFlags, X87, Fence, Rdtsc, VectorDuplicateLow, VectorDuplicateHigh,
    Jcxz, Loop, Loope, Loopne, VectorMultiplyAddWords, VectorAverage, VectorSumAbsoluteDifferences,
    VectorMinUnsigned, VectorMaxUnsigned, VectorMinSigned, VectorMaxSigned, Emms,
    VectorExtractWord, VectorInsertWord, Leave, FpPackedMin, FpPackedMax, FpReciprocal, FpReciprocalSqrt, FpPackedReciprocal, FpPackedReciprocalSqrt, Cmpxchg8b, LoadX87Environment, StoreX87Environment, Lahf, Sahf, Breakpoint, PushAll, PopAll, Pclmulqdq, AesEnc, AesEncLast, AesDec, AesDecLast, AesImc, AesKeygenAssist, VectorShuffleBytes, VectorAlignRight, VectorExtractScalar, VectorInsertScalar, Cmpxchg16b, Unsupported, Syscall,
    Sha1Nexte, Sha1Msg1, Sha1Msg2, Sha1Rnds4, Sha256Rnds2, Sha256Msg1, Sha256Msg2,
    VectorHorizontalAdd, VectorHorizontalSub, VectorHorizontalAddSaturate, VectorHorizontalSubSaturate,
    VectorMultiplyAddUnsignedBytes, VectorMultiplyRoundedHigh, VectorSign, VectorAbsolute,
    VectorBlendImmediate, VectorBlendVariable,
    // SSE4.1／SSE4.2、BMI1／BMI2、ADX、MOVBE、POPCNT／LZCNT、RDRAND／RDSEED、FXSR。
    // 只附加在尾端：模板與 SMAP 以數值保存 opcode，既有編號不得移動。
    VectorExtendSigned, VectorExtendUnsigned, VectorMulEvenSigned, VectorTest, VectorInsertFloat,
    VectorMultipleSad, VectorMinPosition, VectorCompareStringExplicitIndex, VectorCompareStringExplicitMask,
    VectorCompareStringImplicitIndex, VectorCompareStringImplicitMask, FpRound, FpPackedRound, FpDotProduct,
    Crc32, Popcnt, Tzcnt, Lzcnt, Movbe, AddCarryX, AddOverflowX,
    AndNot, BitExtract, LowestBitIsolate, LowestBitMask, LowestBitReset,
    ZeroHighBits, MulNoFlags, ParallelDeposit, ParallelExtract, RotateRightNoFlags,
    ShiftArithmeticNoFlags, ShiftLeftNoFlags, ShiftRightNoFlags, Rdrand, Rdseed, Fxsave, Fxrstor,
    Clc, Stc, Cmc,
    // 基礎 x86：ENTER、BCD、段暫存器、系統表查詢、陷阱；MMX／SSE2 遮罩儲存與 MMX 轉換；x87 BCD／FXTRACT／FNSAVE。
    Enter, AsciiAdjustAdd, AsciiAdjustSubtract, DecimalAdjustAdd, DecimalAdjustSubtract,
    AsciiAdjustMultiply, AsciiAdjustDivide, SetAlFromCarry, AdjustRpl, Bound, OverflowTrap, DebugTrap,
    MoveToSegment, LoadFarPointer, StoreGdt, StoreIdt, StoreLdt, StoreTaskRegister, StoreMachineStatus,
    LoadAccessRights, LoadSegmentLimit, VerifyRead, VerifyWrite, InterruptReturn, CacheFlush,
    VectorMaskedStore, FpFromMmx, FpToMmx, FpTruncToMmx, X87LoadBcd, X87StoreBcd, X87Extract, X87Save, X87Restore,
    // 使用者模式特權指令：依架構產生 #GP，Windows 以 STATUS_PRIVILEGED_INSTRUCTION 派送。
    PrivilegedInstruction,
    // RDTSCP／RDPID：讀取 IA32_TSC_AUX（Windows 為處理器編號）。
    Rdtscp, Rdpid,
    // GFNI（legacy SSE）、MOVDIR64B、FSGSBASE、WAITPKG。
    GaloisMultiply, GaloisAffine, GaloisAffineInverse, MoveDirect64,
    ReadFsBase, ReadGsBase, WriteFsBase, WriteGsBase, UserMonitor, UserWait, TimedPause,
    FarCall, FarJump, FarReturn, SoftwareInterrupt, Sysenter,
    AvxInteger,
    // AMD 3DNow!／3DNow!+：avx_control 保存 0F 0F 後綴 byte。
    Amd3dNow,
    // SSE4a EXTRQ／INSERTQ：立即值形式以 Immediate operand 保存 length|index<<8。
    ExtractField, InsertField,
    // TBM 單來源位元操作：avx_control 為 TrailingBitOp；BEXTR 立即值形式沿用 BitExtract。
    TrailingBitManipulation,
    // RAO-INT AADD／AAND／AOR／AXOR：avx_control 為 0 add、1 and、2 or、3 xor。
    RemoteAtomic,
    // CLZERO、XSAVE 家族（avx_control：0 XSAVE、1 XSAVEOPT、2 XSAVEC）、XRSTOR、XGETBV。
    CacheLineZero, ExtendedSave, ExtendedRestore, ExtendedGetControl,
    // RTM：不建立交易，XBEGIN 一律立即中止並跳至 fallback。
    TransactionBegin, TransactionEnd, TransactionAbort, TransactionTest,
    // VEX 浮點家族；沿用 avx_operation／avx_form／vector_bytes 描述，不移動既有 opcode。
    AvxFloating, AvxFma, AvxMaskedMove, AvxGather, AvxPromoted, AvxHalf
};
enum class TrailingBitOp : std::uint8_t { Blcfill, Blci, Blcic, Blcmsk, Blcs, Blsfill, Blsic, T1mskc, Tzmsk };
enum class X87Op : std::uint8_t {
    Load, Store, StoreTruncate, Add, Sub, SubReverse, Mul, Div, DivReverse,
    Compare, CompareQuiet, CompareFlags, CompareQuietFlags, Exchange, Sqrt, Round,
    Negate, Absolute, Constant, Test, Examine, Clear, Free, IncTop, DecTop, Nop,
    Remainder, RemainderNearest, Log2Multiply, Exp2MinusOne, Scale, Sin, Cos, SinCos, Tan, Atan, Log2PlusOneMultiply,
    ConditionalMove
};
enum class Condition : std::uint8_t { O, No, B, Ae, E, Ne, Be, A, S, Ns, P, Np, L, Ge, Le, G };
bool condition_holds(Condition condition, std::uint64_t rflags);
enum class OperandKind { Register, Memory, Immediate, VectorRegister, X87Register, MmxRegister, SegmentRegister };
struct MemoryOperand {
    enum class Segment { Flat, Fs, Gs } segment{Segment::Flat};
    std::optional<RegisterRef> base;
    std::optional<RegisterRef> index;
    std::uint8_t scale{1};
    std::uint64_t displacement{};
    bool ip_relative{};
};
struct Operand {
    OperandKind kind{OperandKind::Immediate};
    std::uint8_t width{};
    RegisterRef reg{};
    std::uint8_t vector_index{};
    MemoryOperand memory{};
    std::uint64_t immediate{};
    bool relative{};
};
enum class AvxForm : std::uint8_t {
    None, Move, Unary, Binary, Extend, Broadcast, ZeroUpper, ZeroAll, VariableShift,
    UniformShift, UnaryImmediate, BinaryImmediate, BinaryMask, PermuteVariable,
    PermuteImmediate, PermuteLanes, InsertLane, ExtractLane, FusedScalar, FusedPacked,
    Merge, Test, PermuteWithin
};
struct Instruction {
    Opcode opcode{Opcode::Unsupported};
    std::uint8_t length{};
    std::uint8_t width{};
    std::uint8_t address_width{64};
    std::uint8_t stack_width{64};
    std::uint8_t operand_count{};
    std::array<Operand, 3> operands{};
    Condition condition{Condition::E};
    bool locked{};
    bool aligned_vector{};
    bool repeat{};
    bool repeat_equal{true};
    X87Op x87_operation{X87Op::Load};
    std::uint8_t x87_pop{}, x87_constant{};
    bool x87_integer{};
    MemoryOperand::Segment string_segment{MemoryOperand::Segment::Flat};
    std::array<std::uint8_t, 15> bytes{};
    std::string text;
    // VEX 整數向量沿用 128-bit lane 配方；容器寬度獨立於 scalar／element width。
    // Operand.width 維持既有 ABI 的 lane／實際窄來源寬度，不用 8-bit 欄位表示 256。
    Opcode avx_operation{Opcode::Unsupported};
    AvxForm avx_form{AvxForm::None};
    std::uint8_t vector_bytes{};
    // VEX 第四個控制運算元獨立保存，維持既有三個資料運算元的偏移。
    std::uint8_t avx_control{},avx_mask{};
};
enum class DecodeStatus { Ok, Incomplete, Invalid, Unsupported, HostCall };
struct DecodeResult { DecodeStatus status; Instruction instruction; std::string detail; };
// 公開契約不包含第三方型別；Zydis 僅負責解碼與反組譯。
class Decoder {
    ExecutionMode mode_{ExecutionMode::X64};
    bool format_{true};
public:
    explicit Decoder(ExecutionMode mode = ExecutionMode::X64,bool format = true) : mode_(mode),format_(format) {}
    DecodeResult decode(std::span<const std::uint8_t> bytes, GuestAddress rip) const;
    DecodeResult decode_at(const GuestMemory& memory, GuestAddress rip) const;
};
// 三種執行器共用無文字解碼。由協調執行緒持有，編譯 worker 只接收 IR 快照。
// 預設按需保存一百萬個入口；不同後端可借用同一個 cache，毋須重解碼。
class DecodeCache {
public:
    static constexpr std::size_t default_capacity=1024*1024;
    struct Stats {std::uint64_t decodes{},hits{},revalidations{},evictions{},context_resets{},index_tombstones{},index_compactions{},admission_deferred{},admissions{};};
    struct Profile {std::uint64_t requests{},decoder_calls{},decoder_nanoseconds{},validation_nanoseconds{},store_nanoseconds{};};
    explicit DecodeCache(bool enabled=true,std::size_t capacity=default_capacity);
    const DecodeResult& get(const GuestMemory&,GuestAddress,DecodeResult& fresh) const;
    // 額外分段計時是明確選用的路徑；普通 get 不讀取時鐘。
    const DecodeResult& get_profiled(const GuestMemory&,GuestAddress,DecodeResult& fresh,Profile&) const;
    const Stats& stats() const noexcept {return decode_stats_;}
    std::size_t size() const noexcept {return decoded_values_.size();}
private:
    template<bool Profiled> const DecodeResult& get_impl(const GuestMemory&,GuestAddress,DecodeResult&,Profile*) const;
    std::size_t index_slot(std::uint64_t) const noexcept;
    void reserve_index() const;
    void erase_index(std::uint64_t) const;
    bool cache_decodes_{},tombstone_index_{},reuse_admission_{};
    std::size_t decode_cache_capacity{};
    mutable std::uint64_t identity_{};
    mutable ExecutionMode mode_{ExecutionMode::X64};
    struct CachedDecode {
        DecodeResult value;std::uint64_t generation{},rip{};
        CachedDecode(DecodeResult&& decoded,std::uint64_t gen,std::uint64_t address)
            :value(std::move(decoded)),generation(gen),rip(address){}
    };
    struct IndexSlot {std::uint64_t rip{};std::size_t value_plus_one{};};
    // 稀疏連續索引配合分塊值儲存：成長索引不搬動已借出的解碼結果。
    mutable std::vector<IndexSlot> decoded_index_;
    // 首次只記完整位址；再次遇到才保存較大的解碼結果。候選只影響准入，不替代取指核對。
    mutable std::vector<IndexSlot> decode_candidates_;
    mutable std::deque<CachedDecode> decoded_values_;
    // 已刪除槽位繼續串接碰撞鏈；累積至索引四分之一時一次重建。
    static constexpr std::size_t deleted_index=SIZE_MAX;
    mutable std::size_t decode_cursor_{},deleted_indices_{};
    mutable Stats decode_stats_;
};
// 編譯用 IR 可省去文字；只有 dump／profile 才從保存的 bytes 還原。
std::string instruction_text(const Instruction&,GuestAddress);
GuestAddress effective_address(const Operand& operand, const Instruction& instruction,
                               const GuestState& state, GuestAddress next_rip);
// 32-bit 模式的 16-bit 運算元近程轉移（含 rel8／rel16 Jcc、JMP、CALL、LOOP／JCXZ）依 SDM 將 EIP 截為 16 位元；
// 其他形式依堆疊寬度回繞。64-bit 模式的 66 前綴不改變近程轉移的運算元大小。
inline std::uint64_t branch_target_mask(const Instruction& instruction) {
    return instruction.stack_width == 32 && instruction.width == 16 ? UINT64_C(0xffff) : width_mask(instruction.stack_width);
}
}
