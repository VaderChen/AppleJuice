#pragma once
#include <cstdint>
#include <span>
#include <algorithm>
#include <map>

namespace x64arm::icu_collation_binary {

// UCharsTrie 的節點、分支與線性字串均向前編碼。逐一檢查每條邊及其 CE32，
// 使用顯式待訪表以免客體提供的長字串決定宿主呼叫堆疊深度。
template <class CheckValue> bool valid_context_trie(std::span<const std::uint16_t> words, CheckValue check) {
    if (words.empty() || words.front() >= 0x40)
        return false;
    struct Paths {
        std::uint32_t longest{}, count{};
    };
    // 按資料偏移遞增走訪，所有前置邊都已計入，合法共用尾端只解析一次。
    std::map<std::pair<std::size_t, std::size_t>, Paths> pending{{{0, 0}, {0, 1}}};
    std::uint32_t key_count{};
    while (!pending.empty()) {
        auto node_record = pending.extract(pending.begin());
        auto [pos, branches] = node_record.key();
        const auto paths = node_record.mapped();
        if (pos >= words.size())
            return false;
        const auto enqueue = [&](std::size_t target, std::size_t width, unsigned extra) {
            if (target >= words.size() || extra > UINT16_MAX - paths.longest)
                return false;
            auto &next = pending[{target, width}];
            if (next.count > INT32_MAX - paths.count)
                return false;
            next.count += paths.count;
            next.longest = std::max(next.longest, paths.longest + extra);
            return true;
        };
        const auto emit = [&](std::uint32_t value) {
            if (key_count > INT32_MAX - paths.count)
                return false;
            key_count += paths.count;
            return check(value);
        };

        // 三種整數編碼（末端值、中間值、分支距離）各有自己的前導字。
        const auto integer = [&](std::uint32_t lead, std::uint32_t two, std::uint32_t three, std::uint32_t &value) {
            if (lead < two) {
                value = lead;
                return true;
            }
            if (pos == words.size())
                return false;
            if (lead < three) {
                value = ((lead - two) << 16) | words[pos++];
                return true;
            }
            if (words.size() - pos < 2)
                return false;
            value = (std::uint32_t{words[pos]} << 16) | words[pos + 1];
            pos += 2;
            return true;
        };
        if (!branches) {
            const auto node = words[pos++];
            std::uint32_t value{};
            if (node & 0x8000U) {
                if (!integer(node & 0x7fffU, 0x4000, 0x7fff, value) || !emit(value))
                    return false;
                continue;
            }
            if (node >= 0x40) {
                if (node < 0x4040)
                    value = (static_cast<std::uint32_t>(node) >> 6) - 1;
                else if (node < 0x7fc0) {
                    if (pos == words.size())
                        return false;
                    value = (((node & 0x7fc0U) - 0x4040) << 10) | words[pos++];
                } else if (!integer(0x7fff, 0x4000, 0x7fff, value))
                    return false;
                if (!emit(value))
                    return false;
            }
            const auto type = node & 0x3fU;
            if (type >= 0x30) {
                const auto length = type - 0x30 + 1;
                if (!enqueue(pos + length, 0, length))
                    return false;
                continue;
            }
            if (!type && pos == words.size())
                return false;
            branches = 1U + (type ? type : words[pos++]);
            if (branches < 2)
                return false;
        }
        if (branches > 5) {
            if (words.size() - pos < 2)
                return false;
            ++pos; // 左右子樹的分界字元
            const auto lead = words[pos++];
            std::uint32_t delta{};
            if (!integer(lead, 0xfc00, 0xffff, delta) || delta >= words.size() - pos)
                return false;
            if (!enqueue(pos + delta, branches / 2, 0) || !enqueue(pos, branches - branches / 2, 0))
                return false;
            continue;
        }
        while (branches > 1) {
            if (words.size() - pos < 2)
                return false;
            ++pos; // 字元後接末端值或下一節點的距離
            const auto lead = words[pos++];
            std::uint32_t value{};
            if (!integer(lead & 0x7fffU, 0x4000, 0x7fff, value))
                return false;
            if (lead & 0x8000U) {
                if (paths.longest == UINT16_MAX || !emit(value))
                    return false;
            } else {
                if (value >= words.size() - pos)
                    return false;
                if (!enqueue(pos + value, 0, 1))
                    return false;
            }
            --branches;
        }
        if (words.size() - pos < 2)
            return false;
        if (!enqueue(pos + 1, 0, 1))
            return false;
    }
    return true;
}

} // namespace x64arm::icu_collation_binary
