#pragma once

#include "icu_trie_binary.hpp"
#include <rbbidata.h>
#include <algorithm>
#include <array>
#include <memory>

namespace x64arm::icu_break {
using icu::RBBIDataHeader;

template<class Value>
bool valid_rows(const icu::RBBIStateTable& table,std::uint32_t categories,std::span<const std::int32_t> status){
    if(table.fRowLen<(categories+3)*sizeof(Value) || table.fRowLen%sizeof(Value))return false;
    for(std::uint32_t row=0;row<table.fNumStates;++row){
        const auto* values=reinterpret_cast<const Value*>(table.fTableData+std::size_t{row}*table.fRowLen);
        const auto accepting=values[0],lookahead=values[1],tag=values[2];
        if((accepting>icu::ACCEPTING_UNCONDITIONAL && accepting>=table.fLookAheadResultsSize) ||
           (lookahead && lookahead>=table.fLookAheadResultsSize) || tag>=status.size())
            return false;
        if(status[tag]<1 || static_cast<std::size_t>(status[tag])>=status.size()-tag)return false;
        for(std::uint32_t col=0;col<categories;++col)
            if(values[3+col]>=table.fNumStates)return false;
    }
    return true;
}

inline bool valid_table(std::span<const std::uint8_t> bytes,std::uint32_t categories,
                        std::span<const std::int32_t> status){
    constexpr auto header=offsetof(icu::RBBIStateTable,fTableData);
    if(bytes.size()<header)return false;
    const auto& table=*reinterpret_cast<const icu::RBBIStateTable*>(bytes.data());
    if(table.fNumStates<2 || table.fNumStates>65536 || table.fDictCategoriesStart>categories ||
       table.fLookAheadResultsSize>65536 ||
       std::uint64_t{table.fNumStates}*table.fRowLen>bytes.size()-header)return false;
    if(table.fFlags&icu::RBBI_8BITS_ROWS)
        return table.fNumStates<=256 && valid_rows<std::uint8_t>(table,categories,status);
    return valid_rows<std::uint16_t>(table,categories,status);
}

// 宿主 ICU 直接讀取已編譯的狀態表。所有範圍與間接索引先在客體資料副本上核對。
inline bool valid_binary_rules(std::span<const std::uint8_t> bytes){
    if(bytes.size()<sizeof(RBBIDataHeader))return false;
    RBBIDataHeader header{};
    std::memcpy(&header,bytes.data(),sizeof(header));
    if(header.fMagic!=0xb1a0 || header.fFormatVersion[0]!=icu::RBBI_DATA_FORMAT_VERSION[0] ||
       header.fLength!=bytes.size() || header.fCatCount<3 || header.fCatCount>65536 ||
       !header.fRuleSourceLen || header.fStatusTableLen<8 || header.fStatusTableLen%sizeof(std::int32_t))
        return false;
    using Section=std::pair<std::uint32_t,std::uint32_t>;
    std::array sections{
        Section{header.fFTable,header.fFTableLen},Section{header.fRTable,header.fRTableLen},
        Section{header.fTrie,header.fTrieLen},Section{header.fRuleSource,header.fRuleSourceLen},
        Section{header.fStatusTable,header.fStatusTableLen}};
    std::ranges::sort(sections);
    std::uint32_t next=sizeof(RBBIDataHeader);
    for(const auto& [offset,length]:sections){
        if(offset<next || offset>bytes.size() || length>bytes.size()-offset)return false;
        next=offset+length;
    }
    if((header.fFTable | header.fRTable | header.fTrie | header.fStatusTable)&3)return false;
    const std::span status{reinterpret_cast<const std::int32_t*>(bytes.data()+header.fStatusTable),
                           header.fStatusTableLen/sizeof(std::int32_t)};
    if(!valid_table(bytes.subspan(header.fFTable,header.fFTableLen),header.fCatCount,status) ||
       !valid_table(bytes.subspan(header.fRTable,header.fRTableLen),header.fCatCount,status))
        return false;
    const auto trie_bytes=bytes.subspan(header.fTrie,header.fTrieLen);
    if(!icu_trie::valid_binary(trie_bytes,UCPTRIE_TYPE_FAST,UCPTRIE_VALUE_BITS_ANY))return false;
    UErrorCode error=U_ZERO_ERROR;
    const std::unique_ptr<UCPTrie,decltype(&ucptrie_close)> trie(
        ucptrie_openFromBinary(UCPTRIE_TYPE_FAST,UCPTRIE_VALUE_BITS_ANY,trie_bytes.data(),
                               static_cast<std::int32_t>(trie_bytes.size()),nullptr,&error),ucptrie_close);
    if(!trie || U_FAILURE(error) || ucptrie_getValueWidth(trie.get())==UCPTRIE_VALUE_BITS_32)return false;
    for(std::int32_t start=0;start<=0x10ffff;){
        std::uint32_t value{};
        const auto end=ucptrie_getRange(trie.get(),start,UCPMAP_RANGE_NORMAL,0,nullptr,nullptr,&value);
        if(end<start || end>0x10ffff || value>=header.fCatCount)return false;
        start=end+1;
    }
    return true;
}
}
