#pragma once

#include <unicode/ucol.h>
#include <algorithm>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

namespace x64arm::icu_collation_binary {

template<class Write>
std::int32_t clone(const UCollator* collator, std::uint64_t address, std::int32_t capacity,
                   UErrorCode& status, Write write) {
    if (U_FAILURE(status)) return 0;
    if (!collator || capacity < 0 || (capacity && !address)) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }
    if (!capacity) return ucol_cloneBinary(collator, nullptr, 0, &status);

    auto probe = U_ZERO_ERROR;
    const auto required = ucol_cloneBinary(collator, nullptr, 0, &probe);
    if (probe != U_BUFFER_OVERFLOW_ERROR || required <= 0) {
        status = probe;
        return required;
    }
    // 大容量呼叫只配置映像所需大小，並保留客體位址的對齊以重現 ICU 的 trie 輸出契約。
    const auto size = std::min(required, capacity);
    const auto alignment = static_cast<std::size_t>(address & 7);
    const auto extent = static_cast<std::size_t>(size);
    try {
        std::vector<std::uint8_t> first(extent + alignment);
        auto* buffer = first.data() + alignment;
        const auto initial = status;
        const auto result = ucol_cloneBinary(collator, buffer, size, &status);
        if (U_SUCCESS(status)) {
            if (result > 0) write(0, std::span<const std::uint8_t>(buffer, static_cast<std::size_t>(result)));
            return result;
        }

        // 短緩衝區可能只寫標頭與中間的資料段。用兩種底色辨認實際寫入範圍，
        // 保留段間原值，也讓只有寫入權限的客體頁面能接收結果。
        std::vector<std::uint8_t> second(extent + alignment, 0xff);
        auto* other = second.data() + alignment;
        auto repeat_status = initial;
        ucol_cloneBinary(collator, other, size, &repeat_status);
        std::size_t index = 0;
        while (index < extent) {
            while (index < extent && buffer[index] != other[index]) ++index;
            const auto start = index;
            while (index < extent && buffer[index] == other[index]) ++index;
            if (start < index) write(start, std::span<const std::uint8_t>(buffer + start, index - start));
        }
        return result;
    } catch (const std::bad_alloc&) {
        status = U_MEMORY_ALLOCATION_ERROR;
        return 0;
    }
}

}
