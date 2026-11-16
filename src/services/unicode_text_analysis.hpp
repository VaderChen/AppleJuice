#pragma once
#include <array>
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
// 需要造字、重排或定位的書寫系統：由右至左、南亞與東南亞書寫系統，以及組合用附加符號（Inherited）。
inline bool complex_script(UScriptCode script,UChar32){
    if(script==USCRIPT_INHERITED || uscript_isRightToLeft(script))return true;
    switch(script){
    case USCRIPT_COMMON:case USCRIPT_UNKNOWN:case USCRIPT_LATIN:case USCRIPT_GREEK:case USCRIPT_CYRILLIC:case USCRIPT_ARMENIAN:case USCRIPT_GEORGIAN:
    case USCRIPT_HAN:case USCRIPT_HIRAGANA:case USCRIPT_KATAKANA:case USCRIPT_HANGUL:case USCRIPT_BOPOMOFO:case USCRIPT_YI:case USCRIPT_ETHIOPIC:
    case USCRIPT_CHEROKEE:case USCRIPT_CANADIAN_ABORIGINAL:case USCRIPT_OGHAM:case USCRIPT_RUNIC:case USCRIPT_BRAILLE:return false;
    default:return true;}
}
// Uniscribe SCRIPT_PROPERTIES 的兩個 DWORD 位元欄（langid、fComplex、bCharSet…／fInvalidLogAttr、fCDM…），以 ICU 書寫系統代碼為索引。
inline std::array<std::uint32_t,2> script_properties(UScriptCode script){
    struct Row{UScriptCode script;std::uint16_t language;std::uint8_t charset;};
    static constexpr Row rows[]{
        {USCRIPT_LATIN,0x09,0},{USCRIPT_GREEK,0x08,161},{USCRIPT_CYRILLIC,0x19,204},{USCRIPT_ARMENIAN,0x2b,1},{USCRIPT_HEBREW,0x0d,177},{USCRIPT_ARABIC,0x01,178},
        {USCRIPT_SYRIAC,0x5a,1},{USCRIPT_THAANA,0x65,1},{USCRIPT_DEVANAGARI,0x39,1},{USCRIPT_BENGALI,0x45,1},{USCRIPT_GURMUKHI,0x46,1},{USCRIPT_GUJARATI,0x47,1},
        {USCRIPT_ORIYA,0x48,1},{USCRIPT_TAMIL,0x49,1},{USCRIPT_TELUGU,0x4a,1},{USCRIPT_KANNADA,0x4b,1},{USCRIPT_MALAYALAM,0x4c,1},{USCRIPT_SINHALA,0x5b,1},
        {USCRIPT_THAI,0x1e,222},{USCRIPT_LAO,0x54,1},{USCRIPT_TIBETAN,0x51,1},{USCRIPT_MYANMAR,0x55,1},{USCRIPT_GEORGIAN,0x37,1},{USCRIPT_HANGUL,0x12,129},
        {USCRIPT_ETHIOPIC,0x5e,1},{USCRIPT_CHEROKEE,0x5c,1},{USCRIPT_KHMER,0x53,1},{USCRIPT_MONGOLIAN,0x50,1},{USCRIPT_HAN,0x04,134},{USCRIPT_HIRAGANA,0x11,128},
        {USCRIPT_KATAKANA,0x11,128},{USCRIPT_BOPOMOFO,0x04,136},{USCRIPT_YI,0x78,1}};
    std::uint16_t language=0;std::uint8_t charset=1;for(const auto& row:rows)if(row.script==script){language=row.language;charset=row.charset;}
    const bool complex=script!=USCRIPT_COMMON && script!=USCRIPT_UNKNOWN && complex_script(script,0);
    const bool word_breaking=script==USCRIPT_THAI || script==USCRIPT_LAO || script==USCRIPT_KHMER || script==USCRIPT_MYANMAR;
    const bool indic=script==USCRIPT_DEVANAGARI || script==USCRIPT_BENGALI || script==USCRIPT_GURMUKHI || script==USCRIPT_GUJARATI || script==USCRIPT_ORIYA ||
        script==USCRIPT_TAMIL || script==USCRIPT_TELUGU || script==USCRIPT_KANNADA || script==USCRIPT_MALAYALAM || script==USCRIPT_SINHALA || script==USCRIPT_TIBETAN;
    const bool caret=word_breaking || indic;
    std::uint32_t first=language|(complex?1U<<17:0U)|(word_breaking?1U<<18:0U)|(caret?1U<<19:0U)|(std::uint32_t{charset}<<20)|(word_breaking?1U<<30:0U)|(indic || word_breaking?1U<<31:0U);
    const bool diacritics=script==USCRIPT_LATIN || script==USCRIPT_GREEK || script==USCRIPT_CYRILLIC;
    std::uint32_t second=(indic || word_breaking?1U:0U)|(diacritics?2U:0U)|(script==USCRIPT_HAN?4U:0U)|(complex?8U:0U);
    return {first,second};
}
}
