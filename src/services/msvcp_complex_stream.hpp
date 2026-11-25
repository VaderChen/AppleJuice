#pragma once

#include "msvcp_number_scan.hpp"

namespace x64arm::msvcp_detail {

// 傳入同一個前瞻字元直到回傳 Consume；最後的括號須由呼叫端確認已取走才提交兩個分量。
class ComplexField {
public:
    enum class Step { Consume, Complete, Fail };

    ComplexField(NumberType type, NumberFormat format, NumericPunctuation punctuation, unsigned pointer_bytes)
        : type_(type), format_(format), punctuation_(std::move(punctuation)), pointer_bytes_(pointer_bytes) {}

    Step peek(char16_t c, bool eof, bool space) {
        for (;;) {
            switch (phase_) {
            case Phase::Start:
                if (skip(space)) return Step::Consume;
                bracket_ = !eof && c == u'(';
                phase_ = Phase::RealStart;
                if (bracket_) return Step::Consume;
                break;
            case Phase::RealStart:
            case Phase::ImagStart:
                if (skip(space)) return Step::Consume;
                field_ = NumberField{};
                field_.configure(type_, format_.flags, punctuation_, pointer_bytes_);
                phase_ = phase_ == Phase::RealStart ? Phase::Real : Phase::Imag;
                break;
            case Phase::Real:
            case Phase::Imag: {
                if (!eof && field_.accept(c)) return Step::Consume;
                const auto result = field_.conclude(eof);
                if (result.failed) return Step::Fail;
                if (phase_ == Phase::Real) {
                    real = result.bits;
                    if (!bracket_) return Step::Complete;
                    phase_ = Phase::AfterReal;
                } else {
                    imag = result.bits;
                    phase_ = Phase::Close;
                }
                break;
            }
            case Phase::AfterReal:
            case Phase::Close:
                if (skip(space)) return Step::Consume;
                if (!eof && c == u')') {
                    phase_ = Phase::Done;
                    return Step::Consume;
                }
                if (!eof && phase_ == Phase::AfterReal && c == u',') {
                    phase_ = Phase::ImagStart;
                    return Step::Consume;
                }
                return Step::Fail;
            case Phase::Done:
                return Step::Complete;
            }
        }
    }

    bool done() const { return phase_ == Phase::Done; }
    std::uint64_t real{}, imag{};

private:
    enum class Phase { Start, RealStart, Real, AfterReal, ImagStart, Imag, Close, Done };
    bool skip(bool space) const { return (format_.flags & 1U) && space; }
    Phase phase_{Phase::Start};
    bool bracket_{};
    NumberType type_;
    NumberFormat format_;
    NumericPunctuation punctuation_;
    unsigned pointer_bytes_;
    NumberField field_;
};

inline std::u16string format_complex(NumberInput real, NumberInput imag, NumberFormat format,
                                     const NumericPunctuation& punctuation) {
    const auto width = std::exchange(format.width, 0);
    auto result = u"(" + format_number(real, format, punctuation) + u"," +
                  format_number(imag, format, punctuation) + u")";
    if (width > 1048576) throw std::bad_alloc{};
    if (width > static_cast<std::int64_t>(result.size())) {
        const bool left = (format.flags & 0x1c0U) == 0x40U;
        result.insert(left ? result.size() : 0, static_cast<std::size_t>(width) - result.size(), format.fill);
    }
    return result;
}

}
