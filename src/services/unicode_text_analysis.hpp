#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/utf16.h>

namespace x64arm::text_analysis {
struct Item {unsigned start{},script{},level{};};
struct Analysis {std::vector<unsigned> scripts,levels;std::vector<Item> items;unsigned paragraph{};};
inline Analysis analyze(std::u16string_view text,unsigned initial,bool bidi_enabled){
    if(text.size()>65535 || initial>125)throw std::invalid_argument("Unicode 文字分析範圍無效");
    Analysis result;result.scripts.resize(text.size());result.levels.resize(text.size());if(text.empty())return result;
    UErrorCode error=U_ZERO_ERROR;std::unique_ptr<UBiDi,decltype(&ubidi_close)> bidi(ubidi_openSized(static_cast<int32_t>(text.size()),0,&error),ubidi_close);
    if(U_FAILURE(error) || !bidi)throw std::bad_alloc{};
    if(bidi_enabled){ubidi_setPara(bidi.get(),text.data(),static_cast<int32_t>(text.size()),static_cast<UBiDiLevel>(initial),nullptr,&error);if(U_FAILURE(error))throw std::invalid_argument("Unicode 雙向分析失敗");result.paragraph=ubidi_getParaLevel(bidi.get());}
    unsigned previous=USCRIPT_COMMON;
    for(unsigned i=0;i<text.size();){const auto begin=i;UChar32 cp;U16_NEXT(text.data(),i,static_cast<unsigned>(text.size()),cp);const auto script=uscript_getScript(cp,&error);if(U_FAILURE(error))throw std::invalid_argument("Unicode 書寫系統無效");
        const auto selected=script==USCRIPT_INHERITED?previous:static_cast<unsigned>(script);if(script!=USCRIPT_COMMON && script!=USCRIPT_INHERITED)previous=selected;
        const unsigned level=bidi_enabled?ubidi_getLevelAt(bidi.get(),static_cast<int32_t>(begin)):uscript_isRightToLeft(static_cast<UScriptCode>(selected))?1U:0U;
        for(unsigned j=begin;j<i;++j){result.scripts[j]=selected;result.levels[j]=level;}}
    for(unsigned i=0;i<text.size();++i)if(result.scripts[i]==USCRIPT_COMMON || result.scripts[i]==USCRIPT_INHERITED){
        if(i && result.levels[i]==result.levels[i-1] && result.scripts[i-1]>USCRIPT_INHERITED)result.scripts[i]=result.scripts[i-1];
        else {unsigned end=i+1;while(end<text.size() && result.levels[end]==result.levels[i] && result.scripts[end]<=USCRIPT_INHERITED)++end;if(end<text.size() && result.levels[end]==result.levels[i])result.scripts[i]=result.scripts[end];}}
    for(unsigned i=0;i<text.size();++i)if(!i || result.scripts[i]!=result.scripts[i-1] || result.levels[i]!=result.levels[i-1])result.items.push_back({i,result.scripts[i]+1,result.levels[i]});
    return result;
}
inline std::vector<std::uint8_t> line_breaks(std::u16string_view text,const char* locale){
    UErrorCode error=U_ZERO_ERROR;std::unique_ptr<UBreakIterator,decltype(&ubrk_close)> iterator(ubrk_open(UBRK_LINE,locale,text.data(),static_cast<int32_t>(text.size()),&error),ubrk_close);
    if(U_FAILURE(error) || !iterator)throw std::invalid_argument("Unicode 換行分析失敗");
    std::vector<unsigned> boundaries(text.size()+1,2);boundaries.front()=0;boundaries.back()=0;
    for(auto at=ubrk_first(iterator.get());at!=UBRK_DONE;at=ubrk_next(iterator.get())){
        if(at>0 && static_cast<std::size_t>(at)<=text.size())boundaries[static_cast<std::size_t>(at)]=ubrk_getRuleStatus(iterator.get())>=UBRK_LINE_HARD?3U:1U;
    }
    std::vector<std::uint8_t> result(text.size());
    for(std::size_t i=0;i<text.size();++i)result[i]=static_cast<std::uint8_t>(boundaries[i]|(boundaries[i+1]<<2)|(u_isUWhiteSpace(text[i])?16U:0U)|(text[i]==0xad?32U:0U));
    return result;
}
}
