#pragma once

#include "icu_collation_mappings.hpp"
#include <x64arm/memory.hpp>
#include <unicode/ucol.h>
#include <collationdata.h>
#include <collationdatareader.h>
#include <collationsettings.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace x64arm::icu_collation_binary {

using Bytes = std::span<const std::uint8_t>;
using Provider = std::shared_ptr<UCollator>;
using Index = icu::CollationDataReader;

inline std::uint32_t index_word(Bytes bytes, std::size_t at) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

struct Directory {
    std::array<std::uint32_t, Index::IX_TOTAL_SIZE + 1> words{};
    std::size_t header{}, total{};

    Bytes section(Bytes bytes, std::size_t index) const {
        return bytes.subspan(header + words[index], words[index + 1] - words[index]);
    }
};

inline std::size_t header_size(Bytes bytes) {
    if (bytes.size() < 24 || bytes[2] != 0xda || bytes[3] != 0x27 || bytes[8] != 0 ||
        bytes[9] != 0 || bytes[10] != 2 || std::memcmp(bytes.data() + 12, "UCol", 4) || bytes[16] != 5)
        return 0;
    const auto size = static_cast<std::size_t>(bytes[0]) + 256U * bytes[1];
    const auto info = static_cast<std::size_t>(bytes[4]) + 256U * bytes[5];
    return size >= 24 && size % 4 == 0 && info >= 20 && info <= size - 4 ? size : 0;
}

inline bool read_indexes(Bytes indexes, std::size_t header, std::size_t limit, Directory& result) {
    if (indexes.size() < 8 || header > limit) return false;
    const auto count = index_word(indexes, 0);
    const auto known = std::min<std::size_t>(count, result.words.size());
    if (count < 2 || count > (limit - header) / 4 || indexes.size() < known * 4) return false;
    std::uint32_t end = count * 4;
    for (std::size_t i = 0; i < known; ++i) {
        result.words[i] = index_word(indexes, i * 4);
        if (i < Index::IX_REORDER_CODES_OFFSET) continue;
        if (result.words[i] < end || result.words[i] > limit - header) return false;
        end = result.words[i];
    }
    if (count <= Index::IX_JAMO_CE32S_START) result.words[Index::IX_JAMO_CE32S_START] = UINT32_MAX;
    for (std::size_t i = std::max<std::size_t>(known, Index::IX_REORDER_CODES_OFFSET); i < result.words.size(); ++i)
        result.words[i] = end;
    result.header = header;
    result.total = header + end;
    return true;
}

// 只按目錄宣告的實際長度取客體快照。負長度時同樣先檢查目錄，再讀完整映像。
inline std::vector<std::uint8_t> snapshot(const GuestMemory& memory, std::uint64_t address,
                                         std::int32_t length, Directory& layout, UErrorCode& status) {
    if (!address || (length >= 0 && length < 24)) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return {};
    }
    const auto maximum = std::min<std::uint64_t>(static_cast<std::uint32_t>(length < 0 ? INT32_MAX : length),
                                                  UINT64_MAX - address);
    std::array<std::uint8_t, 24> header{};
    if (maximum < header.size()) { status = U_INVALID_FORMAT_ERROR; return {}; }
    memory.read(GuestAddress{address}, header);
    const auto size = header_size(header);
    if (!size || size > maximum) { status = U_INVALID_FORMAT_ERROR; return {}; }
    if (maximum - size < 8) { status = U_ILLEGAL_ARGUMENT_ERROR; return {}; }

    std::array<std::uint8_t, sizeof(layout.words)> indexes{};
    memory.read(GuestAddress{address + size}, std::span(indexes).first(4));
    const auto count = index_word(indexes, 0);
    const auto known = std::min<std::size_t>(count, layout.words.size()) * 4;
    if (count < 2 || count > (maximum - size) / 4) { status = U_INVALID_FORMAT_ERROR; return {}; }
    memory.read(GuestAddress{address + size}, std::span(indexes).first(known));
    if (!read_indexes(Bytes(indexes).first(known), size, maximum, layout)) { status = U_INVALID_FORMAT_ERROR; return {}; }

    memory.check(GuestAddress{address}, layout.total, Access::Read);
    std::vector<std::uint8_t> bytes(layout.total);
    memory.read(GuestAddress{address}, bytes);
    // 其他客體執行緒可能同時更新映像，所有交給 ICU 的資料都依快照重新切割。
    const auto copied_size = header_size(bytes);
    if (!copied_size || copied_size > bytes.size() ||
        !read_indexes(Bytes(bytes).subspan(copied_size), copied_size, bytes.size(), layout) ||
        layout.total != bytes.size()) {
        status = U_INVALID_FORMAT_ERROR;
        return {};
    }
    return bytes;
}

struct BinaryCloser {
    std::shared_ptr<const std::vector<std::uint8_t>> image;
    Provider base;
    void operator()(UCollator* object) const { ucol_close(object); }
};

inline Provider adopt_clone(UCollator* clone, const Provider& source) {
    if (const auto* backing = std::get_deleter<BinaryCloser>(source)) return Provider(clone, *backing);
    return Provider(clone, ucol_close);
}

inline Provider open(const GuestMemory& memory, std::uint64_t address, std::int32_t length,
                     const Provider& base, UErrorCode& status) {
    if (U_FAILURE(status)) return {};
    if (!base) { status = U_ILLEGAL_ARGUMENT_ERROR; return {}; }
    try {
        Directory layout;
        auto bytes = snapshot(memory, address, length, layout, status);
        if (U_FAILURE(status)) return {};
        const auto options = layout.words[Index::IX_OPTIONS];
        using Settings = icu::CollationSettings;
        const auto strength = (options & Settings::STRENGTH_MASK) >> Settings::STRENGTH_SHIFT;
        if ((strength > UCOL_QUATERNARY && strength != UCOL_IDENTICAL) ||
            (options & Settings::ALTERNATE_MASK) > Settings::SHIFTED ||
            ((options & Settings::MAX_VARIABLE_MASK) >> Settings::MAX_VARIABLE_SHIFT) > Settings::MAX_VAR_CURRENCY ||
            (options & Settings::CASE_FIRST_AND_UPPER_MASK) == Settings::UPPER_FIRST) {
            status = U_INVALID_FORMAT_ERROR;
            return {};
        }
        const auto trie = layout.section(bytes, Index::IX_TRIE_OFFSET);
        if (trie.empty()) {
            for (std::size_t i = Index::IX_CES_OFFSET; i < Index::IX_TOTAL_SIZE; ++i) {
                if (!layout.section(bytes, i).empty()) { status = U_INVALID_FORMAT_ERROR; return {}; }
            }
            if (layout.words[Index::IX_JAMO_CE32S_START] != UINT32_MAX) { status = U_INVALID_FORMAT_ERROR; return {}; }
        } else {
            std::span<const std::uint32_t> ce32s;
            std::span<const std::uint64_t> ces;
            std::span<const std::uint16_t> contexts, unsafe_set, fast;
            if (!layout.section(bytes, Index::IX_ROOT_ELEMENTS_OFFSET).empty() ||
                !layout.section(bytes, Index::IX_SCRIPTS_OFFSET).empty() ||
                !layout.section(bytes, Index::IX_COMPRESSIBLE_BYTES_OFFSET).empty() ||
                !as_words(layout.section(bytes, Index::IX_CES_OFFSET), ces) ||
                !as_words(layout.section(bytes, Index::IX_CE32S_OFFSET), ce32s) ||
                !as_words(layout.section(bytes, Index::IX_CONTEXTS_OFFSET), contexts) ||
                !as_words(layout.section(bytes, Index::IX_UNSAFE_BWD_OFFSET), unsafe_set) ||
                !as_words(layout.section(bytes, Index::IX_FAST_LATIN_TABLE_OFFSET), fast) ||
                !valid_backward_set(unsafe_set) ||
                (((options >> 16) & 0xffU) == icu::CollationFastLatin::VERSION && !valid_fast_latin(fast))) {
                status = U_INVALID_FORMAT_ERROR;
                return {};
            }
            if ((address + layout.header + layout.words[Index::IX_TRIE_OFFSET]) % 4) {
                status = U_ILLEGAL_ARGUMENT_ERROR;
                return {};
            }
            CollationMappings mappings(ce32s, contexts, ces);
            if (!mappings.validate(trie, layout.words[Index::IX_JAMO_CE32S_START])) {
                status = U_INVALID_FORMAT_ERROR;
                return {};
            }
        }
        const auto reorder = layout.section(bytes, Index::IX_REORDER_CODES_OFFSET);
        const auto table = layout.section(bytes, Index::IX_REORDER_TABLE_OFFSET);
        if (reorder.size() % 4 || (reorder.empty() && table.size() >= 256)) {
            status = U_INVALID_FORMAT_ERROR;
            return {};
        }
        std::size_t codes = 0;
        while (codes < reorder.size() && index_word(reorder, codes) <= UINT16_MAX) codes += 4;
        // ICU 的配置式將代碼、最多 256 個區間、對齊及 256 位元組加速表加在有號長度上。
        constexpr auto reorder_overhead = 4 * (icu::CollationData::MAX_NUM_SCRIPT_RANGES + 3) + 256;
        if (codes > INT32_MAX - reorder_overhead) { status = U_MEMORY_ALLOCATION_ERROR; return {}; }
        for (auto at = codes; at < reorder.size(); at += 4) {
            if (!codes || index_word(reorder, at) <= UINT16_MAX) { status = U_INVALID_FORMAT_ERROR; return {}; }
        }
        // 在原加速表的尾端留下 script code，將表長設為零，讓 ICU 用根資料重建自己的表。
        // 後續資料段的位置不動，排序器仍能以這組代碼作為還原預設值的依據。
        if (layout.words[0] > Index::IX_REORDER_TABLE_OFFSET) {
            const auto end = layout.words[Index::IX_TRIE_OFFSET];
            const auto start = end - static_cast<std::uint32_t>(codes);
            if (codes && start % 4) { status = U_INVALID_FORMAT_ERROR; return {}; }
            auto* directory = bytes.data() + layout.header;
            if (codes) std::memmove(directory + start, reorder.data(), codes);
            std::memcpy(directory + 4 * Index::IX_REORDER_CODES_OFFSET, &start, sizeof(start));
            std::memcpy(directory + 4 * Index::IX_REORDER_TABLE_OFFSET, &end, sizeof(end));
        }
        auto image = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        auto* native = ucol_openBinary(image->data(), static_cast<std::int32_t>(image->size()), base.get(), &status);
        if (!native) return {};
        return Provider(native, BinaryCloser{std::move(image),base});
    } catch (const std::bad_alloc&) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return {};
    }
}

}
