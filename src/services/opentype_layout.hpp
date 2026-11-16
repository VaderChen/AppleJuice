#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>

// OpenType GSUB／GPOS 版面表格的共用解析：書寫系統、語言系統、功能與 lookup（含延伸子表）。
// 標籤一律以表格中的大端序數值表示（'latn' 為 0x6c61746e）；所有偏移先檢查再讀取，格式錯誤拋 std::invalid_argument。
namespace x64arm::opentype {
inline std::uint32_t read(std::span<const std::uint8_t> data,std::size_t at,unsigned width){
    if(at>data.size() || width>data.size()-at)throw std::invalid_argument("OpenType 表格偏移無效");
    std::uint32_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|data[at+i];return value;
}
inline std::int32_t read_signed(std::span<const std::uint8_t> data,std::size_t at){return static_cast<std::int16_t>(read(data,at,2));}
// Coverage 表：依序列出涵蓋的 glyph（索引即 coverage index）。
inline std::vector<std::uint16_t> coverage(std::span<const std::uint8_t> data,std::size_t at){
    std::vector<std::uint16_t> out;const auto format=read(data,at,2),count=read(data,at+2,2);
    if(format==1)for(unsigned i=0;i<count;++i)out.push_back(static_cast<std::uint16_t>(read(data,at+4+i*2,2)));
    else if(format==2)for(unsigned i=0;i<count;++i){const auto p=at+4+i*6;const auto a=read(data,p,2),b=read(data,p+2,2),index=read(data,p+4,2);
        if(a>b || index!=out.size())throw std::invalid_argument("OpenType coverage 無效");for(auto g=a;g<=b;++g)out.push_back(static_cast<std::uint16_t>(g));}
    else throw std::invalid_argument("OpenType coverage 格式無效");
    return out;
}
inline std::optional<std::size_t> coverage_index(std::span<const std::uint8_t> data,std::size_t at,std::uint16_t glyph){
    const auto glyphs=coverage(data,at);const auto found=std::find(glyphs.begin(),glyphs.end(),glyph);
    if(found==glyphs.end())return std::nullopt;return static_cast<std::size_t>(found-glyphs.begin());
}
inline bool layout_table(std::span<const std::uint8_t> table){return table.size()>=10 && read(table,0,2)==1;}
inline std::vector<std::uint32_t> script_tags(std::span<const std::uint8_t> table){
    std::vector<std::uint32_t> out;if(!layout_table(table))return out;const auto list=read(table,4,2);
    for(unsigned i=0;i<read(table,list,2);++i)out.push_back(read(table,list+2+i*6,4));return out;
}
inline std::optional<std::size_t> script_offset(std::span<const std::uint8_t> table,std::uint32_t tag){
    if(!layout_table(table))return std::nullopt;const auto list=read(table,4,2);
    for(unsigned i=0;i<read(table,list,2);++i)if(read(table,list+2+i*6,4)==tag)return list+read(table,list+2+i*6+4,2);return std::nullopt;
}
inline std::vector<std::uint32_t> language_tags(std::span<const std::uint8_t> table,std::uint32_t script){
    std::vector<std::uint32_t> out;const auto at=script_offset(table,script);if(!at)return out;
    for(unsigned i=0;i<read(table,*at+2,2);++i)out.push_back(read(table,*at+4+i*6,4));return out;
}
// 語言系統：指定的語言標籤不存在或為 0 時使用預設語言系統；回傳（必要功能, 功能索引）。
inline std::optional<std::size_t> language_offset(std::span<const std::uint8_t> table,std::uint32_t script,std::uint32_t language){
    const auto at=script_offset(table,script);if(!at)return std::nullopt;
    if(language)for(unsigned i=0;i<read(table,*at+2,2);++i)if(read(table,*at+4+i*6,4)==language)return *at+read(table,*at+4+i*6+4,2);
    const auto fallback=read(table,*at,2);if(!fallback)return std::nullopt;return *at+fallback;
}
inline std::vector<std::uint16_t> feature_indices(std::span<const std::uint8_t> table,std::uint32_t script,std::uint32_t language){
    std::vector<std::uint16_t> out;const auto at=language_offset(table,script,language);if(!at)return out;
    if(const auto required=read(table,*at+2,2);required!=0xffffU)out.push_back(static_cast<std::uint16_t>(required));
    for(unsigned i=0;i<read(table,*at+4,2);++i)out.push_back(static_cast<std::uint16_t>(read(table,*at+6+i*2,2)));return out;
}
inline std::uint32_t feature_tag(std::span<const std::uint8_t> table,std::uint16_t index){
    const auto list=read(table,6,2);if(index>=read(table,list,2))throw std::invalid_argument("OpenType 功能索引無效");return read(table,list+2+index*6,4);
}
// 功能的 lookup：script 為 0 時取表格中所有同名功能（例如直排字形），否則取該語言系統啟用的功能。
inline std::vector<std::uint16_t> feature_lookups(std::span<const std::uint8_t> table,std::uint32_t feature,std::uint32_t script,std::uint32_t language){
    std::set<std::uint16_t> lookups;if(!layout_table(table))return {};const auto list=read(table,6,2);
    std::vector<std::uint16_t> indices;
    if(script)indices=feature_indices(table,script,language);else for(unsigned i=0;i<read(table,list,2);++i)indices.push_back(static_cast<std::uint16_t>(i));
    for(const auto index:indices){if(feature_tag(table,index)!=feature)continue;const auto at=list+read(table,list+2+index*6+4,2);
        for(unsigned i=0;i<read(table,at+2,2);++i)lookups.insert(static_cast<std::uint16_t>(read(table,at+4+i*2,2)));}
    return {lookups.begin(),lookups.end()};
}
// 逐一處理 lookup 的子表；延伸子表（GSUB 7、GPOS 9）先解開成實際型別。visit(型別, 子表偏移) 回傳 true 時停止。
template<class Visit> bool visit_lookup(std::span<const std::uint8_t> table,std::uint16_t lookup,unsigned extension,Visit&& visit){
    const auto list=read(table,8,2);if(lookup>=read(table,list,2))throw std::invalid_argument("OpenType lookup 索引無效");
    const auto at=list+read(table,list+2+lookup*2,2);const auto type=read(table,at,2);
    for(unsigned i=0;i<read(table,at+4,2);++i){
        auto sub=at+read(table,at+6+i*2,2);auto actual=type;
        if(type==extension){actual=read(table,sub+2,2);sub+=read(table,sub+4,4);}
        if(visit(actual,sub))return true;
    }
    return false;
}
// GSUB 單一替換（型別 1）。
inline std::optional<std::uint16_t> single_substitution(std::span<const std::uint8_t> table,std::size_t sub,std::uint16_t glyph){
    const auto index=coverage_index(table,sub+read(table,sub+2,2),glyph);if(!index)return std::nullopt;const auto format=read(table,sub,2);
    if(format==1)return static_cast<std::uint16_t>(glyph+read(table,sub+4,2));
    if(format==2){if(*index>=read(table,sub+4,2))throw std::invalid_argument("GSUB 替換索引無效");return static_cast<std::uint16_t>(read(table,sub+6+*index*2,2));}
    throw std::invalid_argument("GSUB 單一替換格式無效");
}
// GSUB 替代字形（型別 3）。
inline std::vector<std::uint16_t> alternates(std::span<const std::uint8_t> table,std::size_t sub,std::uint16_t glyph){
    std::vector<std::uint16_t> out;if(read(table,sub,2)!=1)throw std::invalid_argument("GSUB 替代字形格式無效");
    const auto index=coverage_index(table,sub+read(table,sub+2,2),glyph);if(!index)return out;if(*index>=read(table,sub+4,2))throw std::invalid_argument("GSUB 替代集合無效");
    const auto set=sub+read(table,sub+6+*index*2,2);for(unsigned i=0;i<read(table,set,2);++i)out.push_back(static_cast<std::uint16_t>(read(table,set+2+i*2,2)));return out;
}
// GPOS 單一調整（型別 1）：回傳設計單位的（XPlacement, YPlacement, XAdvance, YAdvance）。
inline std::optional<std::array<std::int32_t,4>> single_adjustment(std::span<const std::uint8_t> table,std::size_t sub,std::uint16_t glyph){
    const auto index=coverage_index(table,sub+read(table,sub+2,2),glyph);if(!index)return std::nullopt;
    const auto format=read(table,sub,2),value_format=read(table,sub+4,2);unsigned size=0;for(unsigned bit=0;bit<8;++bit)if(value_format&(1U<<bit))size+=2;
    std::size_t record=0;if(format==1)record=sub+6;else if(format==2){if(*index>=read(table,sub+6,2))throw std::invalid_argument("GPOS 數值索引無效");record=sub+8+*index*size;}
    else throw std::invalid_argument("GPOS 單一調整格式無效");
    std::array<std::int32_t,4> values{};std::size_t at=record;for(unsigned bit=0;bit<8;++bit)if(value_format&(1U<<bit)){if(bit<4)values[bit]=read_signed(table,at);at+=2;}
    return values;
}
}
