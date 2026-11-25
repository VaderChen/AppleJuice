#pragma once
#include "icu_collation_context_trie.hpp"
#include <collation.h>
#include <collationdata.h>
#include <collationfastlatin.h>
#include <collationroot.h>
#include <utrie2.h>
#include <cstring>
#include <unordered_set>
#include <algorithm>

namespace x64arm::icu_collation_binary {

using MappingBytes = std::span<const std::uint8_t>;

template <class T> inline bool as_words(MappingBytes bytes, std::span<const T> &result) {
    if (bytes.size() % sizeof(T) || (!bytes.empty() && reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(T)))
        return false;
    result = {reinterpret_cast<const T *>(bytes.data()), bytes.size() / sizeof(T)};
    return true;
}

inline bool valid_backward_set(std::span<const std::uint16_t> words) {
    if (words.empty())
        return true;
    const bool supplementary = (words.front() & 0x8000U) != 0;
    const auto count = words.front() & 0x7fffU;
    const auto header = supplementary ? 2U : 1U;
    if (words.size() < header + count)
        return false;
    const auto bmp = supplementary ? words[1] : count;
    if (bmp > count || (count - bmp) % 2)
        return false;
    words = words.subspan(header, count);
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < words.size();) {
        const bool wide = i >= bmp;
        std::uint32_t edge = words[i++];
        if (wide) {
            edge = (edge << 16) | words[i++];
            if (edge < 0x10000 || edge > 0x110000)
                return false;
        }
        if (i > (wide ? 2U : 1U) && edge <= previous)
            return false;
        previous = edge;
    }
    return true;
}

inline bool valid_fast_latin(std::span<const std::uint16_t> table) {
    using Fast = icu::CollationFastLatin;
    if (table.empty())
        return true;
    const auto header = table.front() & 0xffU;
    if (table.front() >> 8 != Fast::VERSION || !header || table.size() < header + Fast::NUM_FAST_CHARS)
        return false;
    const auto variable_tops = table.subspan(1, header - 1);
    table = table.subspan(header);
    if (table[0] < Fast::CONTRACTION || table[0] >= Fast::EXPANSION)
        return false;
    // 快速比較必須走到字元零的縮合項目，才能辨識 NUL 結尾。
    for (auto top : variable_tops)
        if (top < table[0] || top >= Fast::MIN_SHORT)
            return false;
    for (const auto entry : table.first(Fast::NUM_FAST_CHARS)) {
        if (entry < Fast::CONTRACTION || entry >= Fast::MIN_LONG)
            continue;
        auto cursor = std::size_t{Fast::NUM_FAST_CHARS} + (entry & Fast::INDEX_MASK);
        if (entry >= Fast::EXPANSION) {
            if (cursor + 2 > table.size())
                return false;
            continue;
        }
        bool first = true;
        while (true) {
            if (cursor >= table.size())
                return false;
            const auto word = table[cursor];
            if (!first && (word & Fast::CONTR_CHAR_MASK) == Fast::CONTR_CHAR_MASK)
                break;
            const auto length = static_cast<unsigned>(word) >> Fast::CONTR_LENGTH_SHIFT;
            if (length < 1 || length > 3 || length > table.size() - cursor)
                return false;
            cursor += length;
            first = false;
        }
    }
    return true;
}

class CollationMappings {
    using C = icu::Collation;
    std::span<const std::uint32_t> ce32s_;
    std::span<const std::uint16_t> contexts_;
    std::span<const std::uint64_t> ces_;
    std::span<const std::uint32_t> jamos_;
    std::unordered_set<std::uint32_t> checked_contexts_;

    bool leaf(std::uint32_t ce) const {
        if (!C::isSpecialCE32(ce))
            return ce != C::NO_CE32;
        const auto offset = ce >> 13;
        const auto count = (ce >> 8) & 31U;
        switch (C::tagFromCE32(ce)) {
        case C::LONG_PRIMARY_TAG:
        case C::LONG_SECONDARY_TAG:
        case C::LATIN_EXPANSION_TAG:
            return true;
        case C::EXPANSION_TAG:
            return count && offset + count <= ces_.size();
        case C::EXPANSION32_TAG:
            if (!count || offset + count > ce32s_.size())
                return false;
            for (auto part : ce32s_.subspan(offset, count))
                if (part == C::NO_CE32 || (C::isSpecialCE32(part) && !C::isLongPrimaryCE32(part) &&
                                           !C::hasCE32Tag(part, C::LONG_SECONDARY_TAG)))
                    return false;
            return true;
        case C::IMPLICIT_TAG:
            return ce == C::UNASSIGNED_CE32;
        default:
            return false;
        }
    }

    // 前綴可以選到縮合；縮合結果和展開元素不能再跳回同一層。
    bool contextual(std::uint32_t ce, bool prefix_allowed) {
        if (!C::isPrefixCE32(ce) && !C::isContractionCE32(ce))
            return leaf(ce);
        const bool prefix = C::isPrefixCE32(ce);
        if (prefix && !prefix_allowed)
            return false;
        if (checked_contexts_.contains(ce))
            return true;
        const auto pos = ce >> 13;
        if (pos + 2 >= contexts_.size())
            return false;
        auto check = [&](std::uint32_t value) { return prefix ? contextual(value, false) : leaf(value); };
        if (!check((std::uint32_t{contexts_[pos]} << 16) | contexts_[pos + 1]) ||
            !valid_context_trie(contexts_.subspan(pos + 2), check))
            return false;
        checked_contexts_.insert(ce);
        return true;
    }

  public:
    CollationMappings(std::span<const std::uint32_t> ce32s, std::span<const std::uint16_t> contexts,
                      std::span<const std::uint64_t> ces)
        : ce32s_(ce32s), contexts_(contexts), ces_(ces) {}

    bool validate(MappingBytes bytes, std::uint32_t jamo_start) { return jamos(jamo_start) && trie(bytes); }

  private:
    bool jamos(std::uint32_t start) {
        constexpr auto count = icu::CollationData::JAMO_CE32S_LENGTH;
        if (start == UINT32_MAX) {
            auto status = U_ZERO_ERROR;
            const auto *root = icu::CollationRoot::getData(status);
            if (!root || U_FAILURE(status))
                return false;
            jamos_ = {root->jamoCE32s, count};
        } else {
            if (std::size_t{start} + count > ce32s_.size())
                return false;
            jamos_ = ce32s_.subspan(start, count);
        }
        for (auto ce : jamos_)
            if (!contextual(ce, true))
                return false;
        return true;
    }

    bool mapping(std::uint32_t ce, std::uint32_t first, std::uint32_t last, bool lead_unit) {
        if (ce == C::FALLBACK_CE32)
            return true;
        if (first == 0 && ce != (C::SPECIAL_CE32_LOW_BYTE | C::U0000_TAG))
            return false;
        if (!C::isSpecialCE32(ce))
            return ce != C::NO_CE32;
        switch (C::tagFromCE32(ce)) {
        case C::LEAD_SURROGATE_TAG:
            return lead_unit && (ce & C::LEAD_TYPE_MASK) <= C::LEAD_MIXED;
        case C::HANGUL_TAG:
            if (first < 0xac00 || last > 0xd7a3)
                return false;
            if (ce & C::HANGUL_NO_SPECIAL_JAMO) {
                for (auto group = (first - 0xac00) / 588; group <= (last - 0xac00) / 588; ++group)
                    if (C::isSpecialCE32(jamos_[group]))
                        return false;
                for (auto value : jamos_.subspan(19))
                    if (C::isSpecialCE32(value))
                        return false;
            }
            return true;
        case C::OFFSET_TAG: {
            const auto index = ce >> 13;
            if (index >= ces_.size())
                return false;
            const auto origin = static_cast<std::uint32_t>(ces_[index]) >> 8;
            return origin <= first && (ces_[index] & 0x7fU) != 0;
        }
        case C::U0000_TAG:
            return first == 0 && last == 0 && !ce32s_.empty() && contextual(ce32s_[0], true);
        case C::DIGIT_TAG:
            return ((ce >> 8) & 31U) <= 9 && (ce >> 13) < ce32s_.size() && contextual(ce32s_[ce >> 13], true);
        default:
            return contextual(ce, true);
        }
    }

    // UTrie2 的 BMP、單一高代理、UTF-8 雙位元組及補充平面各有自己的索引布局。
    bool trie(MappingBytes bytes) {
        std::span<const std::uint16_t> words;
        if (bytes.size() < 16 || !as_words(bytes, words) || words[0] != 0x6932 || words[1] != 0x5472 ||
            words[2] != UTRIE2_32_VALUE_BITS)
            return false;
        const auto index_count = words[3];
        const auto data_count = std::uint32_t{words[4]} << UTRIE2_INDEX_SHIFT;
        const auto high = std::uint32_t{words[7]} << UTRIE2_SHIFT_1;
        if (index_count < UTRIE2_INDEX_1_OFFSET || data_count < UTRIE2_DATA_START_OFFSET ||
            words[6] + std::size_t{UTRIE2_DATA_BLOCK_LENGTH} > data_count || high > 0x110000 ||
            bytes.size() < 16U + 2U * index_count + 4U * data_count)
            return false;
        auto index = words.subspan(8, index_count);
        const auto null_index = words[5];
        if (null_index != UINT16_MAX) {
            if (null_index + std::size_t{UTRIE2_INDEX_2_BLOCK_LENGTH} > index.size())
                return false;
            for (auto entry : index.subspan(null_index, UTRIE2_INDEX_2_BLOCK_LENGTH))
                if ((std::uint32_t{entry} << UTRIE2_INDEX_SHIFT) != words[6])
                    return false;
        }
        std::span<const std::uint32_t> values;
        if (!as_words(bytes.subspan(16U + 2U * index_count, 4U * data_count), values))
            return false;
        // 空區塊與高位預設值也會由 ICU 的範圍列舉器直接使用。
        if (values[words[6]] != C::FALLBACK_CE32 ||
            !std::all_of(values.begin() + words[6], values.begin() + words[6] + UTRIE2_DATA_BLOCK_LENGTH,
                         [](auto v) { return v == C::FALLBACK_CE32; }))
            return false;
        const auto high_value = values[data_count - UTRIE2_DATA_GRANULARITY];
        if (high < 0x110000 && !mapping(high_value, high, 0x10ffff, false))
            return false;

        const auto block = [&](std::uint32_t slot, std::uint32_t cp, bool lead) {
            if (slot >= index.size())
                return false;
            auto base = std::uint32_t{index[slot]} << UTRIE2_INDEX_SHIFT;
            if (base + UTRIE2_DATA_BLOCK_LENGTH > values.size())
                return false;
            if (base == words[6])
                return true;
            for (unsigned j = 0; j < UTRIE2_DATA_BLOCK_LENGTH; ++j)
                if (!mapping(values[base + j], cp + j, cp + j, lead))
                    return false;
            return true;
        };
        for (std::uint32_t cp = 0; cp < 0x10000; cp += UTRIE2_DATA_BLOCK_LENGTH) {
            const auto slot = cp >> UTRIE2_SHIFT_2;
            const bool lead = cp >= 0xd800 && cp < 0xdc00;
            if (!block(slot, cp, lead) ||
                (lead && !block(UTRIE2_LSCP_INDEX_2_OFFSET + ((cp - 0xd800) >> UTRIE2_SHIFT_2), cp, false)))
                return false;
        }
        for (std::uint32_t cp = 0x10000; cp < high; cp += UTRIE2_DATA_BLOCK_LENGTH) {
            const auto slot = UTRIE2_INDEX_1_OFFSET - UTRIE2_OMITTED_BMP_INDEX_1_LENGTH + (cp >> UTRIE2_SHIFT_1);
            if (slot >= index.size() || !block(index[slot] + ((cp >> UTRIE2_SHIFT_2) & UTRIE2_INDEX_2_MASK), cp, false))
                return false;
        }
        for (unsigned i = 0; i < UTRIE2_UTF8_2B_INDEX_2_LENGTH; ++i) {
            const auto offset = index[UTRIE2_UTF8_2B_INDEX_2_OFFSET + i];
            if (offset + 64U > values.size())
                return false;
            for (unsigned j = 0; j < 64; ++j) {
                const auto cp = 64 * i + j;
                if (i < 2) {
                    if (!mapping(values[offset + j], 0xfffd, 0xfffd, false))
                        return false;
                } else if (values[offset + j] !=
                           values[(std::uint32_t{index[cp >> UTRIE2_SHIFT_2]} << UTRIE2_INDEX_SHIFT) + (cp & 31)])
                    return false;
            }
        }
        for (unsigned cp = 0; cp < 128; ++cp)
            if (values[cp] != values[(std::uint32_t{index[cp >> UTRIE2_SHIFT_2]} << UTRIE2_INDEX_SHIFT) + (cp & 31)])
                return false;
        return true;
    }
};

} // namespace x64arm::icu_collation_binary
