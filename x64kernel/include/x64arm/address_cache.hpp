#pragma once
#include "x64arm/crc32c.hpp"
#include <bit>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <span>
#include <cstring>
#include <type_traits>

namespace x64arm::address_cache {
// 所有位址位元參與 CRC32C；雜湊只決定槽位，不能取代完整鍵、權限或世代驗證。
inline constexpr std::uint32_t hash(std::uint64_t key) noexcept {return cpu_crc32c(0,key,8);}
inline constexpr std::size_t index(std::uint64_t key,std::size_t mask) noexcept {return hash(key)&mask;}
template<class Table>
constexpr decltype(auto) direct_slot(Table& table,std::uint64_t key) noexcept {
    assert(std::has_single_bit(std::size(table)));
    return table[index(key,std::size(table)-1)];
}
// Match 可檢查完整鍵或範圍；原子載入、生命週期與失效仍由呼叫端負責。
template<class Table,class Match>
auto direct_find(Table& table,std::uint64_t key,Match matches) noexcept {
    auto& slot=direct_slot(table,key);return matches(slot)?&slot:nullptr;
}
// 直接快取的 {鍵,值} 以單一 STP 寫入：槽位 16 位元組對齊時，LSE2 宿主的 LDP／STP 為單拷貝原子，
// 正在執行的 worker 不會讀到「新鍵配舊值」的錯配配對（並行模式在 worker 執行中發布新入口時必要）。
template<class Value>
void store_pair(Value& slot,const Value& value) noexcept {
#if defined(__aarch64__)
    if constexpr(sizeof(Value)==16 && alignof(Value)>=16){
        std::uint64_t words[2];std::memcpy(words,&value,sizeof words);
        __asm__ volatile("stp %0, %1, [%2]" : : "r"(words[0]),"r"(words[1]),"r"(&slot) : "memory");
        return;
    }
#endif
    slot=value;
}
template<class Table,class Value>
void direct_refill(Table& table,std::uint64_t key,const Value& value) noexcept {
    store_pair(direct_slot(table,key),value);
}
template<class Table,class Match>
void direct_invalidate(Table& table,std::uint64_t key,Match matches) noexcept {
    if(auto* slot=direct_find(table,key,matches))store_pair(*slot,std::remove_reference_t<decltype(*slot)>{});
}
// 線性探測表必須為二次方容量，且至少保留一個空槽。滿載政策／配置由擁有者管理。
// Occupied 獨立於鍵和值，因此 key=0、value=nullptr 都能是有效項目。
template<class Entry,std::size_t Extent,class Key,class Occupied>
std::size_t position(std::span<Entry,Extent> slots,std::uint64_t key,Key key_of,Occupied occupied) noexcept {
    assert(std::has_single_bit(slots.size()));
    const auto mask=slots.size()-1;auto at=index(key,mask);
    while(std::invoke(occupied,slots[at]) && std::invoke(key_of,slots[at])!=key)at=(at+1)&mask;
    return at;
}
template<class Entry,std::size_t Extent,class Key,class Occupied>
Entry* find(std::span<Entry,Extent> slots,std::uint64_t key,Key key_of,Occupied occupied) noexcept {
    if(slots.empty())return nullptr;
    auto& slot=slots[position(slots,key,key_of,occupied)];return std::invoke(occupied,slot)?&slot:nullptr;
}
template<class Source,class Destination,class Key,class Occupied>
void rehash(const Source& source,Destination& destination,Key key_of,Occupied occupied) noexcept {
    for(const auto& slot:source)if(std::invoke(occupied,slot))
        destination[position(std::span(destination),std::invoke(key_of,slot),key_of,occupied)]=slot;
}
template<class Entry,std::size_t Extent,class Key,class Occupied>
bool erase(std::span<Entry,Extent> slots,std::uint64_t key,Key key_of,Occupied occupied) noexcept {
    if(slots.empty())return false;
    auto hole=position(slots,key,key_of,occupied);
    if(!std::invoke(occupied,slots[hole]))return false;
    const auto mask=slots.size()-1;
    // 向後補洞，不累積墓碑；跨越表尾的碰撞鏈也保持可達。
    for(auto at=(hole+1)&mask;std::invoke(occupied,slots[at]);at=(at+1)&mask){
        const auto home=index(std::invoke(key_of,slots[at]),mask);
        if(((at-home)&mask)>=((at-hole)&mask)){slots[hole]=slots[at];hole=at;}
    }
    slots[hole]={};return true;
}
}
