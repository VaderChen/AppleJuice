#pragma once
#include "x64arm/native_chain.hpp"
#include <functional>
#include <map>
#include <ostream>
#include <tuple>

namespace x64arm {
// 與 native_chain_arm64.S 的作用域描述對齊；只供既有 profiling 事件的事後歸因。
// 這是鏈的控制描述，不代表 FPCR 的完整值，也不將指令次數換算成時間。
constexpr unsigned native_chain_profile_scopes(unsigned incoming,unsigned flags) noexcept {
    const auto requested=flags&0xa3U;
    return flags&64U?(requested&~0x81U)|(incoming&0x81U):requested;
}
struct NativeChainProfile {
    using Site=std::tuple<std::uint64_t,std::uint64_t,unsigned,unsigned>; // serial、rip、flags、incoming
    struct SiteCount {
        std::uint64_t calls{},instructions{},starts{},start_instructions{},ends{},end_instructions{};
        SiteCount& operator+=(const SiteCount& other);
    };
    struct LinkCount {
        std::uint64_t calls{},source_instructions{},target_instructions{};
        LinkCount& operator+=(const LinkCount& other);
    };
    std::uint64_t batches{},blocks{},instructions{},incomplete_batches{},broken_links{};
    std::map<Site,SiteCount> sites;
    std::map<std::pair<Site,Site>,LinkCount> links;
    void record(const NativeChainContext& batch,const std::function<std::uint64_t(const NativeChainNode&)>& serial);
    NativeChainProfile& operator+=(const NativeChainProfile& other);
    void dump(std::ostream& out) const;
};
}
