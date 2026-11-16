#include "x64arm/windows_services.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/windows_transform.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include "unicode_text_analysis.hpp"
#include "opentype_layout.hpp"
#include "x64arm/native_font_data.hpp"
#include "windows_locale_data.hpp"
#include <unicode/uscript.h>
#include <unicode/utf16.h>
#include <stdexcept>
namespace x64arm {
std::span<const WindowsServiceDescriptor> script_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"usp10.dll","ScriptApplyDigitSubstitution",3},{"usp10.dll","ScriptRecordDigitSubstitution",2},
        {"gdi32.dll","ScriptApplyDigitSubstitution",3},{"gdi32.dll","ScriptRecordDigitSubstitution",2},
        {"usp10.dll","ScriptItemize",7},{"usp10.dll","ScriptShape",10},{"usp10.dll","ScriptFreeCache",1},
        {"usp10.dll","ScriptPlace",9},{"gdi32.dll","ScriptPlace",9},
        {"gdi32.dll","ScriptItemize",7},{"gdi32.dll","ScriptShape",10},{"gdi32.dll","ScriptFreeCache",1},
        {"usp10.dll","ScriptStringAnalyse",13},{"usp10.dll","ScriptStringOut",8},{"usp10.dll","ScriptStringFree",1},
        {"usp10.dll","ScriptString_pSize",1},{"usp10.dll","ScriptString_pcOutChars",1},{"usp10.dll","ScriptString_pLogAttr",1},
        {"usp10.dll","ScriptStringGetLogicalWidths",2},{"usp10.dll","ScriptStringCPtoX",4},{"usp10.dll","ScriptStringXtoCP",4},{"usp10.dll","ScriptStringValidate",1},
        {"usp10.dll","ScriptStringGetOrder",2},{"usp10.dll","ScriptGetLogicalWidths",7},{"usp10.dll","ScriptCPtoX",9},{"usp10.dll","ScriptXtoCP",9},
        {"usp10.dll","ScriptApplyLogicalWidth",9},{"usp10.dll","ScriptJustify",6},{"usp10.dll","ScriptLayout",4},{"usp10.dll","ScriptBreak",4},
        {"usp10.dll","ScriptIsComplex",3},{"usp10.dll","ScriptGetProperties",2},{"usp10.dll","ScriptCacheGetHeight",3},{"usp10.dll","ScriptGetCMap",6},
        {"usp10.dll","ScriptGetGlyphABCWidth",4},{"usp10.dll","ScriptGetFontProperties",3},{"usp10.dll","ScriptTextOut",14},
        {"usp10.dll","ScriptItemizeOpenType",8},{"usp10.dll","ScriptShapeOpenType",16},{"usp10.dll","ScriptPlaceOpenType",18},
        {"usp10.dll","ScriptGetFontScriptTags",6},{"usp10.dll","ScriptGetFontLanguageTags",7},{"usp10.dll","ScriptGetFontFeatureTags",8},
        {"usp10.dll","ScriptGetFontAlternateGlyphs",10},{"usp10.dll","ScriptSubstituteSingleGlyph",9},{"usp10.dll","ScriptPositionSingleGlyph",12,1U<<9},
        // gdi32 的 Script* 與 usp10 同一實作。
        {"gdi32.dll","ScriptStringGetOrder",2},{"gdi32.dll","ScriptGetLogicalWidths",7},{"gdi32.dll","ScriptCPtoX",9},{"gdi32.dll","ScriptXtoCP",9},
        {"gdi32.dll","ScriptApplyLogicalWidth",9},{"gdi32.dll","ScriptJustify",6},{"gdi32.dll","ScriptLayout",4},{"gdi32.dll","ScriptBreak",4},
        {"gdi32.dll","ScriptIsComplex",3},{"gdi32.dll","ScriptGetProperties",2},{"gdi32.dll","ScriptCacheGetHeight",3},{"gdi32.dll","ScriptGetCMap",6},
        {"gdi32.dll","ScriptGetGlyphABCWidth",4},{"gdi32.dll","ScriptGetFontProperties",3},{"gdi32.dll","ScriptTextOut",14},
        {"gdi32.dll","ScriptItemizeOpenType",8},{"gdi32.dll","ScriptShapeOpenType",16},{"gdi32.dll","ScriptPlaceOpenType",18},
        {"gdi32.dll","ScriptGetFontScriptTags",6},{"gdi32.dll","ScriptGetFontLanguageTags",7},{"gdi32.dll","ScriptGetFontFeatureTags",8},
        {"gdi32.dll","ScriptGetFontAlternateGlyphs",10},{"gdi32.dll","ScriptSubstituteSingleGlyph",9},{"gdi32.dll","ScriptPositionSingleGlyph",12,1U<<9}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_script(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    constexpr unsigned invalid=0x80070057U,unsupported=0x80004001U,missing=0x80040200U,out_of_memory=0x8007000eU;
    try{
        // SCRIPT_CACHE：快取保存首次解析的字型；沒有快取時由 HDC 目前字型解析，成功後才建立快取（ScriptShape、ScriptPlace 與其他字型查詢共用）。
        const auto script_font=[&](std::uint64_t hdc,std::uint64_t cache_pointer,GuiFont& font,bool& created)->std::optional<std::uint32_t>{
            if(!cache_pointer)return invalid;const auto cache=memory.load(GuestAddress{cache_pointer},bits);created=false;
            if(cache){const auto found=gui_.script_caches.find(cache);if(found==gui_.script_caches.end())return invalid;font=found->second;return std::nullopt;}
            if(!hdc)return 0x8000000aU;if(!gui_.contexts.contains(hdc) && !gui_.dc_objects.contains(hdc))return invalid;
            font=gui_font(gui_,gui_.contexts[hdc].font);const auto physical=native_resolve_font(font);if(!physical)return missing;font.native=physical->native;created=true;return std::nullopt;};
        const auto remember_cache=[&](std::uint64_t cache_pointer,const GuiFont& font)->bool{
            if(gui_.script_caches.size()>=4096)return false;const auto handle=allocate_heap(memory,8);if(!handle)return false;
            try{gui_.script_caches.emplace(handle,font);}catch(...){free_heap(memory,handle,state);throw;}memory.store(GuestAddress{cache_pointer},bits,handle);return true;};
        // 字元對應的 glyph：未經造字的 cmap 查詢；字型沒有的字元為 0。
        const auto character_glyphs=[&](const GuiFont& font,std::u16string_view text){
            auto shaped=native_script_shape(font,text,false,true,true,false);
            if(shaped.glyphs.size()!=text.size()){shaped.glyphs.assign(text.size(),0);for(std::size_t i=0;i<text.size();++i){const auto single=native_script_shape(font,text.substr(i,1),false,true,true,false);if(!single.glyphs.empty())shaped.glyphs[i]=single.glyphs[0];}}
            return shaped.glyphs;};
        const auto read_words=[&](std::uint64_t address,std::size_t count){std::vector<std::uint16_t> words(count);for(std::size_t i=0;i<count;++i)words[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{address+i*2},16));return words;};
        const auto read_ints=[&](std::uint64_t address,std::size_t count){std::vector<std::int32_t> values(count);for(std::size_t i=0;i<count;++i)values[i]=static_cast<std::int32_t>(memory.load(GuestAddress{address+i*4},32));return values;};
        // 每個字元的邏輯寬度：同一 cluster 的字元平分 cluster 內所有 glyph 的寬度，餘數給前面的字元；與字形陣列的排列方向無關。
        const auto logical_widths=[&](const std::vector<std::uint16_t>& clusters,const std::vector<std::int32_t>& advances){
            std::vector<std::int32_t> widths(clusters.size());std::vector<std::uint16_t> starts(clusters.begin(),clusters.end());std::sort(starts.begin(),starts.end());starts.erase(std::unique(starts.begin(),starts.end()),starts.end());
            for(std::size_t i=0;i<clusters.size();){
                auto j=i;while(j<clusters.size() && clusters[j]==clusters[i])++j;
                const auto begin=clusters[i];const auto next=std::upper_bound(starts.begin(),starts.end(),begin);const std::size_t end=next==starts.end()?advances.size():*next;
                std::int64_t total=0;for(auto g=static_cast<std::size_t>(begin);g<end && g<advances.size();++g)total+=advances[g];
                const auto chars=static_cast<std::int64_t>(j-i);for(auto k=i;k<j;++k)widths[k]=static_cast<std::int32_t>(total/chars+((static_cast<std::int64_t>(k-i)<total%chars)?1:0));
                i=j;
            }
            return widths;};
        if(name=="ScriptRecordDigitSubstitution"){
            if(!arg(1))return finish(0x80004003U);const auto policy=windows_locale_digit_substitution(static_cast<std::uint32_t>(arg(0)));
            if(!policy)return finish(invalid);memory.check(GuestAddress{arg(1)},12,Access::Write);
            memory.store(GuestAddress{arg(1)},32,(*policy)[0]|((*policy)[1]<<16));memory.store(GuestAddress{arg(1)+4},32,(*policy)[2]);memory.store(GuestAddress{arg(1)+8},32,0);return finish(0);
        }
        if(name=="ScriptApplyDigitSubstitution"){
            if(!arg(1) || !arg(2))return finish(0x80004003U);std::array<std::uint32_t,3> policy{};
            if(arg(0)){const auto languages=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));policy={languages&0xffffU,languages>>16,static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},8))};}
            else {const auto defaults=windows_locale_digit_substitution(0x400);if(!defaults)return finish(invalid);policy=*defaults;}
            if(policy[2]>3)return finish(invalid);memory.check(GuestAddress{arg(1)},4,Access::Write);memory.check(GuestAddress{arg(2)},2,Access::Write);
            auto control=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32))&~0x1ffffU;
            auto initial=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},16))&~0x100U;
            if(policy[2]==1)control|=9U;
            else {control|=policy[2]==2?policy[0]:policy[1];if(!policy[2])control|=0x10000U;initial|=0x100U;}
            memory.store(GuestAddress{arg(1)},32,control);memory.store(GuestAddress{arg(2)},16,initial);return finish(0);
        }
        if(name=="ScriptItemize" || name=="ScriptItemizeOpenType"){
            const bool open_type=name=="ScriptItemizeOpenType";const unsigned items_count=open_type?7U:6U;
            const auto count=integer(1),capacity=integer(2);
            if(!arg(0) || !arg(5) || !arg(items_count) || (open_type && !arg(6)) || count<1 || count>65535 || capacity<2 || capacity>65536)return finish(invalid);
            const auto control=arg(3)?static_cast<unsigned>(memory.load(GuestAddress{arg(3)},32)):0;
            const auto initial=arg(4)?static_cast<unsigned>(memory.load(GuestAddress{arg(4)},16)):0;
            // 先提供標準 Unicode 分段；舊式數字替換與邊界覆寫不能默默忽略。
            if((control&~0x0100ffffU) || (initial&~31U))return finish(unsupported);
            std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+i*2},16));
            // 與 DirectWrite TextAnalyzer 共用 ICU 書寫系統、群集方向及中性符號分段。
            const auto analysis=text_analysis::analyze(text,initial,arg(3) || arg(4));const auto& items=analysis.items;
            if(std::any_of(analysis.levels.begin(),analysis.levels.end(),[](unsigned level){return level>31;}))return finish(unsupported);
            if(items.size()>=static_cast<unsigned>(capacity))return finish(out_of_memory);
            memory.check(GuestAddress{arg(5)},(items.size()+1)*8,Access::Write);memory.check(GuestAddress{arg(items_count)},4,Access::Write);
            if(open_type){
                // eScript 為 ICU 書寫系統代碼加一（0 為 SCRIPT_UNDEFINED）。OpenType 書寫系統標籤取 ISO 15924 短名的小寫（Hira／Kana 為 kana）；Common／Inherited 為 SCRIPT_TAG_UNKNOWN。
                memory.check(GuestAddress{arg(6)},items.size()*4,Access::Write);
                for(std::size_t i=0;i<items.size();++i){std::uint32_t tag=0;const auto code=items[i].script?static_cast<UScriptCode>(items[i].script-1):USCRIPT_UNKNOWN;
                    if(code!=USCRIPT_COMMON && code!=USCRIPT_INHERITED && code!=USCRIPT_UNKNOWN)if(const char* short_name=uscript_getShortName(code);short_name && std::char_traits<char>::length(short_name)==4){
                        std::string tag_text=code==USCRIPT_HIRAGANA || code==USCRIPT_KATAKANA?"kana":short_name;for(unsigned k=0;k<4;++k)tag|=std::uint32_t{static_cast<unsigned char>(tag_text[k]|0x20)}<<(8*k);}
                    memory.store(GuestAddress{arg(6)+i*4},32,tag);}
            }
            for(std::size_t i=0;i<items.size();++i){const auto& item=items[i];memory.store(GuestAddress{arg(5)+i*8},32,item.start);memory.store(GuestAddress{arg(5)+i*8+4},16,item.script|((item.level&1)?0xc00:0));memory.store(GuestAddress{arg(5)+i*8+6},16,item.level);}
            memory.store(GuestAddress{arg(5)+items.size()*8},64,static_cast<unsigned>(count));memory.store(GuestAddress{arg(items_count)},32,items.size());return finish(0);
        }
        if(name=="ScriptFreeCache"){
            if(!arg(0))return finish(invalid);const auto handle=memory.load(GuestAddress{arg(0)},bits);memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);
            if(handle && !gui_.script_caches.contains(handle))return finish(invalid);
            if(handle){gui_.script_caches.erase(handle);free_heap(memory,handle,state);}memory.store(GuestAddress{arg(0)},bits,0);return finish(0);
        }
        if(name=="ScriptShape"){
            const auto count=integer(3),capacity=integer(4);
            if(!arg(1) || !arg(2) || !arg(5) || !arg(6) || !arg(7) || !arg(8) || !arg(9) || count<1 || count>65535 || capacity<0 || capacity>65535)return finish(invalid);
            const auto analysis=static_cast<unsigned>(memory.load(GuestAddress{arg(5)},16)),script_state=static_cast<unsigned>(memory.load(GuestAddress{arg(5)+2},16));
            if((analysis&0x3000U) || (script_state&~31U) || (analysis&1023U)>USCRIPT_CODE_LIMIT)return finish(unsupported);
            GuiFont font;bool created=false;if(const auto error=script_font(arg(0),arg(1),font,created))return finish(*error);
            std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(2)+i*2},16));
            const auto shaped=native_script_shape(font,text,(analysis&0x400)!=0,(analysis&0x4000)!=0,(analysis&1023)==0,(analysis&0x8000)!=0);
            if(shaped.missing)return finish(missing);if(shaped.glyphs.size()>static_cast<unsigned>(capacity))return finish(out_of_memory);
            memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(6)},shaped.glyphs.size()*2,Access::Write);memory.check(GuestAddress{arg(7)},shaped.clusters.size()*2,Access::Write);memory.check(GuestAddress{arg(8)},shaped.attributes.size()*2,Access::Write);memory.check(GuestAddress{arg(9)},4,Access::Write);
            if(created && !remember_cache(arg(1),font))return finish(out_of_memory);
            for(std::size_t i=0;i<shaped.glyphs.size();++i){memory.store(GuestAddress{arg(6)+i*2},16,shaped.glyphs[i]);memory.store(GuestAddress{arg(8)+i*2},16,shaped.attributes[i]);}
            for(std::size_t i=0;i<shaped.clusters.size();++i)memory.store(GuestAddress{arg(7)+i*2},16,shaped.clusters[i]);memory.store(GuestAddress{arg(9)},32,shaped.glyphs.size());return finish(0);
        }
        if(name=="ScriptPlace"){
            // 依 SCRIPT_CACHE 或 HDC 目前字型取得各 glyph 的實際前進寬度；GOFFSET 為 0，ABC 為總寬。
            const auto count=integer(3);
            if(!arg(1) || !arg(2) || !arg(4) || !arg(5) || !arg(6) || !arg(7) || count<1 || count>65535)return finish(invalid);
            GuiFont font;bool created=false;if(const auto error=script_font(arg(0),arg(1),font,created))return finish(*error);
            std::u16string glyphs(static_cast<std::size_t>(count),u'\0');std::vector<std::uint16_t> attributes(static_cast<std::size_t>(count));
            for(unsigned i=0;i<static_cast<unsigned>(count);++i){glyphs[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(2)+i*2},16));attributes[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(4)+i*2},16));}
            std::vector<std::int32_t> advances;
            try{advances=native_gdi_glyph_advances(font,glyphs);}catch(const std::invalid_argument&){return finish(invalid);}
            if(advances.size()!=glyphs.size())return finish(invalid);
            memory.check(GuestAddress{arg(6)},glyphs.size()*4,Access::Write);memory.check(GuestAddress{arg(7)},glyphs.size()*8,Access::Write);if(arg(8))memory.check(GuestAddress{arg(8)},12,Access::Write);
            memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);
            if(created && !remember_cache(arg(1),font))return finish(out_of_memory);
            std::int64_t total=0;
            for(std::size_t i=0;i<glyphs.size();++i){
                const auto advance=(attributes[i]&0x40U)?0:advances[i]; // fZWSP 的 glyph 不佔寬度。
                memory.store(GuestAddress{arg(6)+i*4},32,static_cast<std::uint32_t>(advance));memory.store(GuestAddress{arg(7)+i*8},64,0);total+=advance;
            }
            if(arg(8)){memory.store(GuestAddress{arg(8)},32,0);memory.store(GuestAddress{arg(8)+4},32,static_cast<std::uint32_t>(std::clamp<std::int64_t>(total,0,UINT32_MAX)));memory.store(GuestAddress{arg(8)+8},32,0);}
            return finish(0);
        }
        if(name=="ScriptGetLogicalWidths"){
            // (psa, cChars, cGlyphs, piGlyphWidth, pwLogClust, psva, piDx)
            const auto chars=integer(1),glyphs=integer(2);if(!arg(0) || !arg(3) || !arg(4) || !arg(6) || chars<1 || glyphs<1)return finish(invalid);
            const auto widths=logical_widths(read_words(arg(4),static_cast<std::size_t>(chars)),read_ints(arg(3),static_cast<std::size_t>(glyphs)));
            memory.check(GuestAddress{arg(6)},widths.size()*4,Access::Write);for(std::size_t i=0;i<widths.size();++i)memory.store(GuestAddress{arg(6)+i*4},32,static_cast<std::uint32_t>(widths[i]));return finish(0);
        }
        if(name=="ScriptCPtoX" || name=="ScriptXtoCP"){
            // CPtoX：(iCP, fTrailing, cChars, cGlyphs, pwLogClust, psva, piAdvance, psa, piX)；XtoCP：(iX, cChars, cGlyphs, pwLogClust, psva, piAdvance, psa, piCP, piTrailing)。
            // X 由執行區段左緣起算；由右至左的區段中第一個字元在最右側。
            const bool to_x=name=="ScriptCPtoX";const unsigned base=to_x?2U:1U;const auto chars=integer(base),glyphs=integer(base+1);
            if(!arg(base+2) || !arg(base+4) || !arg(base+5) || !arg(base+6) || (!to_x && !arg(base+7)) || chars<1 || glyphs<1)return finish(invalid);
            const bool rtl=(memory.load(GuestAddress{arg(base+5)},16)&0x400U)!=0;
            const auto widths=logical_widths(read_words(arg(base+2),static_cast<std::size_t>(chars)),read_ints(arg(base+4),static_cast<std::size_t>(glyphs)));
            std::vector<std::int64_t> edges(widths.size()+1);for(std::size_t i=0;i<widths.size();++i)edges[i+1]=edges[i]+widths[i];const auto total=edges.back();
            const auto visual=[&](std::int64_t logical){return rtl?total-logical:logical;};
            if(to_x){
                const auto cp=integer(0);std::int64_t logical=cp<0?0:cp>=chars?total:edges[static_cast<std::size_t>(cp)]+(arg(1)?widths[static_cast<std::size_t>(cp)]:0);
                memory.store(GuestAddress{arg(8)},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(visual(logical))));return finish(0);
            }
            const std::int64_t x=integer(0);const auto logical=rtl?total-x:x;std::int32_t cp=0,trailing=0;
            if(logical<0){cp=-1;trailing=1;}else if(logical>=total){cp=chars;trailing=0;}
            else {for(std::size_t i=0;i<widths.size();++i)if(logical<edges[i+1]){cp=static_cast<std::int32_t>(i);trailing=logical>=edges[i]+widths[i]/2?1:0;break;}}
            memory.store(GuestAddress{arg(7)},32,static_cast<std::uint32_t>(cp));memory.store(GuestAddress{arg(8)},32,static_cast<std::uint32_t>(trailing));return finish(0);
        }
        if(name=="ScriptApplyLogicalWidth"){
            // (piDx, cChars, cGlyphs, pwLogClust, psva, piAdvance, psa, pABC, piJustify)：各 cluster 的差額加到 cluster 最後一個 glyph，並更新總寬。
            const auto chars=integer(1),glyphs=integer(2);if(!arg(0) || !arg(3) || !arg(5) || !arg(6) || !arg(8) || chars<1 || glyphs<1)return finish(invalid);
            const auto desired=read_ints(arg(0),static_cast<std::size_t>(chars));const auto clusters=read_words(arg(3),static_cast<std::size_t>(chars));auto justified=read_ints(arg(5),static_cast<std::size_t>(glyphs));
            const auto current=logical_widths(clusters,justified);std::vector<std::uint16_t> starts(clusters.begin(),clusters.end());std::sort(starts.begin(),starts.end());starts.erase(std::unique(starts.begin(),starts.end()),starts.end());
            for(std::size_t i=0;i<clusters.size();){auto j=i;std::int64_t difference=0;while(j<clusters.size() && clusters[j]==clusters[i]){difference+=desired[j]-current[j];++j;}
                const auto next=std::upper_bound(starts.begin(),starts.end(),clusters[i]);const std::size_t end=next==starts.end()?justified.size():*next;
                if(end>clusters[i] && end<=justified.size())justified[end-1]=static_cast<std::int32_t>(justified[end-1]+difference);i=j;}
            memory.check(GuestAddress{arg(8)},justified.size()*4,Access::Write);std::int64_t total=0;
            for(std::size_t i=0;i<justified.size();++i){memory.store(GuestAddress{arg(8)+i*4},32,static_cast<std::uint32_t>(justified[i]));total+=justified[i];}
            if(arg(7)){const auto a=static_cast<std::int32_t>(memory.load(GuestAddress{arg(7)},32)),c=static_cast<std::int32_t>(memory.load(GuestAddress{arg(7)+8},32));memory.store(GuestAddress{arg(7)+4},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(total-a-c)));}
            return finish(0);
        }
        if(name=="ScriptJustify"){
            // (psva, piAdvance, cGlyphs, iDx, iMinKashida, piJustify)：先分配到空白，沒有空白時分配到可調整字元；餘數給前面的 glyph。
            const auto glyphs=integer(2);if(!arg(0) || !arg(1) || !arg(5) || glyphs<1)return finish(invalid);
            auto advances=read_ints(arg(1),static_cast<std::size_t>(glyphs));const auto attributes=read_words(arg(0),static_cast<std::size_t>(glyphs));
            std::vector<std::size_t> targets;for(std::size_t i=0;i<attributes.size();++i){const auto kind=attributes[i]&15U;if(kind==1 || kind==4)targets.push_back(i);}
            if(targets.empty())for(std::size_t i=0;i<attributes.size();++i)if((attributes[i]&15U)>=2)targets.push_back(i);
            const auto extra=static_cast<std::int64_t>(integer(3));
            if(!targets.empty()){const auto count=static_cast<std::int64_t>(targets.size());for(std::size_t k=0;k<targets.size();++k)advances[targets[k]]=static_cast<std::int32_t>(advances[targets[k]]+extra/count+(static_cast<std::int64_t>(k)<std::abs(extra%count)?(extra<0?-1:1):0));}
            memory.check(GuestAddress{arg(5)},advances.size()*4,Access::Write);for(std::size_t i=0;i<advances.size();++i)memory.store(GuestAddress{arg(5)+i*4},32,static_cast<std::uint32_t>(advances[i]));return finish(0);
        }
        if(name=="ScriptLayout"){
            // (cRuns, pbLevel, piVisualToLogical, piLogicalToVisual)：依 Unicode 雙向演算法 L2，從最高層級到最低奇數層級反轉連續片段。
            const auto runs=integer(0);if(!arg(1) || runs<0)return finish(invalid);if(!runs)return finish(0);
            std::vector<std::uint8_t> levels(static_cast<std::size_t>(runs));memory.read(GuestAddress{arg(1)},levels);
            std::vector<std::int32_t> order(levels.size());for(std::size_t i=0;i<order.size();++i)order[i]=static_cast<std::int32_t>(i);
            const auto highest=*std::max_element(levels.begin(),levels.end());unsigned lowest_odd=255;for(const auto level:levels)if(level&1U)lowest_odd=std::min<unsigned>(lowest_odd,level);
            for(int level=highest;level>=static_cast<int>(lowest_odd) && lowest_odd!=255;--level)
                for(std::size_t i=0;i<order.size();){if(levels[static_cast<std::size_t>(order[i])]<level){++i;continue;}auto j=i;while(j<order.size() && levels[static_cast<std::size_t>(order[j])]>=level)++j;std::reverse(order.begin()+static_cast<std::ptrdiff_t>(i),order.begin()+static_cast<std::ptrdiff_t>(j));i=j;}
            if(arg(2)){memory.check(GuestAddress{arg(2)},order.size()*4,Access::Write);for(std::size_t i=0;i<order.size();++i)memory.store(GuestAddress{arg(2)+i*4},32,static_cast<std::uint32_t>(order[i]));}
            if(arg(3)){memory.check(GuestAddress{arg(3)},order.size()*4,Access::Write);for(std::size_t i=0;i<order.size();++i)memory.store(GuestAddress{arg(3)+static_cast<std::uint64_t>(order[i])*4},32,static_cast<std::uint32_t>(i));}
            return finish(0);
        }
        if(name=="ScriptBreak"){
            // SCRIPT_LOGATTR：fSoftBreak（可在此字元前換行）、fWhiteSpace、fCharStop（字素起點）、fWordStop（單字起點）；與換行分析共用 ICU。
            const auto count=integer(1);if(!arg(0) || !arg(2) || !arg(3) || count<1)return finish(invalid);
            std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+i*2},16));
            std::vector<std::uint8_t> attributes(text.size());const auto lines=text_analysis::line_breaks(text,"");
            for(std::size_t i=0;i<text.size();++i){if(i && (lines[i]&3U)==1U)attributes[i]|=1U;if(lines[i]&16U)attributes[i]|=2U;}
            const auto boundaries=[&](UBreakIteratorType type,std::uint8_t bit,bool words){
                UErrorCode error=U_ZERO_ERROR;std::unique_ptr<UBreakIterator,decltype(&ubrk_close)> iterator(ubrk_open(type,"",text.data(),static_cast<int32_t>(text.size()),&error),ubrk_close);
                if(U_FAILURE(error) || !iterator)throw std::domain_error("Unicode 分段失敗");
                for(auto at=ubrk_first(iterator.get());at!=UBRK_DONE;){const auto next=ubrk_next(iterator.get());if(at>=0 && static_cast<std::size_t>(at)<text.size() && (!words || ubrk_getRuleStatus(iterator.get())!=UBRK_WORD_NONE))attributes[static_cast<std::size_t>(at)]|=bit;at=next;}};
            boundaries(UBRK_CHARACTER,4U,false);boundaries(UBRK_WORD,8U,true);
            memory.write(GuestAddress{arg(3)},attributes);return finish(0);
        }
        if(name=="ScriptIsComplex"){
            // (pwcInChars, cInChars, dwFlags)：SIC_COMPLEX 檢查需要造字的書寫系統，SIC_ASCIIDIGIT 把 ASCII 數字視為複雜，SIC_NEUTRAL 把中性字元視為複雜。
            const auto count=integer(1);const auto flags=static_cast<std::uint32_t>(arg(2));if(!arg(0) || count<0 || (flags&~7U))return finish(invalid);
            for(std::size_t at=0;at<static_cast<std::size_t>(count);){
                UChar32 code=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+at*2},16));++at;
                if(U16_IS_LEAD(code) && at<static_cast<std::size_t>(count)){const auto trail=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+at*2},16));if(U16_IS_TRAIL(trail)){code=U16_GET_SUPPLEMENTARY(code,trail);++at;}}
                if((flags&2U) && code>=u'0' && code<=u'9')return finish(0);
                UErrorCode error=U_ZERO_ERROR;const auto script=uscript_getScript(code,&error);
                if((flags&4U) && (script==USCRIPT_COMMON) && !(code>=u'0' && code<=u'9'))return finish(0);
                if((flags&1U) && text_analysis::complex_script(script,code))return finish(0);
            }
            return finish(1);
        }
        if(name=="ScriptGetProperties"){
            // 以 eScript（ICU 書寫系統代碼加一，0 為 SCRIPT_UNDEFINED）為索引的 SCRIPT_PROPERTIES 表，配置一次後由整個程序共用。
            if(!arg(0) && !arg(1))return finish(invalid);
            if(!gui_.script_properties){const auto count=static_cast<std::size_t>(USCRIPT_CODE_LIMIT)+1;const unsigned pointer=x86_?4U:8U;
                const auto table=allocate_heap(memory,count*(pointer+8));if(!table)return finish(out_of_memory);
                for(std::size_t i=0;i<count;++i){const auto entry=table+count*pointer+i*8;const auto values=i?text_analysis::script_properties(static_cast<UScriptCode>(i-1)):std::array<std::uint32_t,2>{1U<<20,0};
                    memory.store(GuestAddress{entry},32,values[0]);memory.store(GuestAddress{entry+4},32,values[1]);memory.store(GuestAddress{table+i*pointer},bits,entry);}
                gui_.script_properties=table;}
            if(arg(0))memory.store(GuestAddress{arg(0)},bits,gui_.script_properties);if(arg(1))memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(USCRIPT_CODE_LIMIT)+1);return finish(0);
        }
        if(name=="ScriptCacheGetHeight" || name=="ScriptGetCMap" || name=="ScriptGetGlyphABCWidth" || name=="ScriptGetFontProperties"){
            GuiFont font;bool created=false;if(const auto error=script_font(arg(0),arg(1),font,created))return finish(*error);
            std::optional<std::uint32_t> result;
            if(name=="ScriptCacheGetHeight"){
                if(!arg(2))return finish(invalid);const auto metrics=native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
                memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(metrics.height));result=0;
            }else if(name=="ScriptGetCMap"){
                // (hdc, psc, pwcInChars, cChars, dwFlags, pwOutGlyphs)：SGCM_RTL 先套用鏡像字元；有字元不在字型中時回 S_FALSE。
                const auto count=integer(3);if(!arg(2) || !arg(5) || count<1 || (arg(4)&~1U))return finish(invalid);
                std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i){auto c=static_cast<char16_t>(memory.load(GuestAddress{arg(2)+i*2},16));if(arg(4)&1U)c=static_cast<char16_t>(u_charMirror(c));text[i]=c;}
                const auto glyphs=character_glyphs(font,text);memory.check(GuestAddress{arg(5)},glyphs.size()*2,Access::Write);bool all=true;
                for(std::size_t i=0;i<glyphs.size();++i){memory.store(GuestAddress{arg(5)+i*2},16,glyphs[i]);if(!glyphs[i] && !U16_IS_TRAIL(text[i]))all=false;}
                result=all?0U:1U;
            }else if(name=="ScriptGetGlyphABCWidth"){
                if(!arg(3))return finish(invalid);std::vector<std::int32_t> advances;
                try{advances=native_gdi_glyph_advances(font,std::u16string(1,static_cast<char16_t>(arg(2))));}catch(const std::invalid_argument&){return finish(invalid);}
                if(advances.size()!=1)return finish(invalid);memory.check(GuestAddress{arg(3)},12,Access::Write);
                memory.store(GuestAddress{arg(3)},32,0);memory.store(GuestAddress{arg(3)+4},32,static_cast<std::uint32_t>(advances[0]));memory.store(GuestAddress{arg(3)+8},32,0);result=0;
            }else {
                // SCRIPT_FONTPROPERTIES（16 位元組）：空白、預設字元、無效（.notdef）與 Arabic kashida 的 glyph，以及 kashida 寬度。
                if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=16)return finish(invalid);
                const auto face=native_resolve_font(font);const char16_t default_character=face?face->default_character:u'?';
                const auto glyphs=character_glyphs(font,std::u16string{u' ',default_character,u'ـ'});std::int32_t kashida_width=0;
                if(glyphs[2]){try{const auto advances=native_gdi_glyph_advances(font,std::u16string(1,static_cast<char16_t>(glyphs[2])));if(!advances.empty())kashida_width=advances[0];}catch(const std::invalid_argument&){}}
                memory.store(GuestAddress{arg(2)+4},16,glyphs[0]);memory.store(GuestAddress{arg(2)+6},16,glyphs[1]);memory.store(GuestAddress{arg(2)+8},16,0);
                memory.store(GuestAddress{arg(2)+10},16,glyphs[2]?glyphs[2]:0xffffU);memory.store(GuestAddress{arg(2)+12},32,static_cast<std::uint32_t>(kashida_width));result=0;
            }
            if(created){memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);if(!remember_cache(arg(1),font))return finish(out_of_memory);}
            return finish(*result);
        }
        if(name=="ScriptTextOut"){
            // (hdc, psc, x, y, fuOptions, lprc, psa, pwcReserved, iReserved, pwGlyphs, cGlyphs, piAdvance, piJustify, pGoffset)：
            // 以 ETO_GLYPH_INDEX 交給 ExtTextOutW；glyph 陣列預設為視覺順序，fLogicalOrder 的由右至左區段先反轉；有垂直位移時逐一繪製。
            const auto count=integer(10);const auto options=static_cast<std::uint32_t>(arg(4));
            if(!arg(0) || !arg(6) || !arg(9) || !arg(11) || count<1 || (options&~0x2006U))return finish(invalid);
            GuiFont font;bool created=false;if(const auto error=script_font(arg(0),arg(1),font,created))return finish(*error);
            if(created){memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);if(!remember_cache(arg(1),font))return finish(out_of_memory);}
            auto glyphs=read_words(arg(9),static_cast<std::size_t>(count));auto advances=read_ints(arg(12)?arg(12):arg(11),static_cast<std::size_t>(count));
            std::vector<std::array<std::int32_t,2>> offsets(glyphs.size());if(arg(13))for(std::size_t i=0;i<glyphs.size();++i)offsets[i]={static_cast<std::int32_t>(memory.load(GuestAddress{arg(13)+i*8},32)),static_cast<std::int32_t>(memory.load(GuestAddress{arg(13)+i*8+4},32))};
            const auto analysis=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(6)},16));
            if((analysis&0x400U) && (analysis&0x4000U)){std::reverse(glyphs.begin(),glyphs.end());std::reverse(advances.begin(),advances.end());std::reverse(offsets.begin(),offsets.end());}
            GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(glyphs.size()*6+8);if(!buffer)return finish(out_of_memory);
            const auto draw=[&](std::int64_t x,std::int64_t y,std::size_t first,std::size_t length,std::uint32_t flags)->bool{
                for(std::size_t i=0;i<length;++i){memory.store(GuestAddress{buffer+i*2},16,glyphs[first+i]);memory.store(GuestAddress{buffer+glyphs.size()*2+i*4},32,static_cast<std::uint32_t>(advances[first+i]));}
                return nested_service(state,memory,"gdi32.dll","ExtTextOutW",{arg(0),static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)),static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)),flags|16U,arg(5),buffer,length,buffer+glyphs.size()*2})!=0;};
            const bool vertical=std::any_of(offsets.begin(),offsets.end(),[](const auto& offset){return offset[1]!=0;});
            bool ok=true;
            if(!vertical){
                // 水平位移併入前一個 glyph 的前進量與起點。
                const std::int64_t first_offset=offsets.empty()?0:offsets[0][0];
                for(std::size_t i=0;i+1<advances.size();++i)advances[i]+=offsets[i+1][0]-offsets[i][0];
                ok=draw(integer(2)+first_offset,integer(3),0,glyphs.size(),options);
            }else {
                std::int64_t x=integer(2);
                for(std::size_t i=0;i<glyphs.size() && ok;++i){ok=draw(x+offsets[i][0],integer(3)-offsets[i][1],i,1,i?(options&~2U):options);x+=advances[i];}
            }
            return finish(ok?0U:0x80004005U);
        }
        if(name=="ScriptGetFontScriptTags" || name=="ScriptGetFontLanguageTags" || name=="ScriptGetFontFeatureTags" ||
           name=="ScriptGetFontAlternateGlyphs" || name=="ScriptSubstituteSingleGlyph" || name=="ScriptPositionSingleGlyph"){
            // 讀取字型的 GSUB／GPOS（與 DirectWrite 共用 opentype_layout.hpp）；OPENTYPE_TAG 的第一個字元在最低位元組，表格標籤為大端序。
            GuiFont font;bool created=false;if(const auto error=script_font(arg(0),arg(1),font,created))return finish(*error);
            const auto face=native_resolve_font(font);if(!face)return finish(missing);
            const auto gsub=native_font_table(*face,0x47535542U),gpos=native_font_table(*face,0x47504f53U);
            const auto swap=[](std::uint32_t value){return ((value&0xffU)<<24)|((value&0xff00U)<<8)|((value>>8)&0xff00U)|(value>>24);};
            const auto merge=[](std::vector<std::uint32_t> first,const std::vector<std::uint32_t>& second){for(const auto tag:second)if(std::find(first.begin(),first.end(),tag)==first.end())first.push_back(tag);return first;};
            const auto store_list=[&](const std::vector<std::uint32_t>& values,unsigned maximum,unsigned output,unsigned count,bool tags)->std::uint32_t{
                if(integer(maximum)<=0 || !arg(output) || !arg(count))return invalid;memory.check(GuestAddress{arg(count)},4,Access::Write);memory.store(GuestAddress{arg(count)},32,values.size());
                if(values.size()>static_cast<std::size_t>(integer(maximum)))return out_of_memory;
                for(std::size_t i=0;i<values.size();++i){if(tags)memory.store(GuestAddress{arg(output)+i*4},32,swap(values[i]));else memory.store(GuestAddress{arg(output)+i*2},16,values[i]);}return 0;};
            std::uint32_t result=0;
            if(name=="ScriptGetFontScriptTags")result=store_list(merge(opentype::script_tags(gsub),opentype::script_tags(gpos)),3,4,5,true);
            else if(name=="ScriptGetFontLanguageTags"){const auto script=swap(static_cast<std::uint32_t>(arg(3)));result=store_list(merge(opentype::language_tags(gsub,script),opentype::language_tags(gpos,script)),4,5,6,true);}
            else if(name=="ScriptGetFontFeatureTags"){
                const auto script=swap(static_cast<std::uint32_t>(arg(3))),language=swap(static_cast<std::uint32_t>(arg(4)));std::vector<std::uint32_t> tags;
                for(const auto* table:{&gsub,&gpos})for(const auto index:opentype::feature_indices(*table,script,language)){const auto tag=opentype::feature_tag(*table,index);if(std::find(tags.begin(),tags.end(),tag)==tags.end())tags.push_back(tag);}
                result=store_list(tags,5,6,7,true);
            }else {
                const auto script=swap(static_cast<std::uint32_t>(arg(3))),language=swap(static_cast<std::uint32_t>(arg(4))),feature=swap(static_cast<std::uint32_t>(arg(5)));
                const auto glyph=static_cast<std::uint16_t>(arg(name=="ScriptGetFontAlternateGlyphs"?6:7));
                if(name=="ScriptPositionSingleGlyph"){
                    // GPOS 單一調整依字型的 em 換算為裝置單位；lParameter 為 0 時停用功能。
                    if(!arg(10) || !arg(11))return finish(invalid);std::int64_t advance=integer(8);const auto packed=arg(9);
                    std::int64_t du=static_cast<std::int32_t>(packed&0xffffffffU),dv=static_cast<std::int32_t>(packed>>32);
                    if(arg(6) && face->em>0){
                        const double em=font.height<0?-font.height:font.height>0 && face->ascent+face->descent>0?static_cast<double>(font.height)*face->em/(face->ascent+face->descent):13.0;const double scale=em/face->em;
                        for(const auto lookup:opentype::feature_lookups(gpos,feature,script,language))opentype::visit_lookup(gpos,lookup,9,[&](unsigned type,std::size_t sub){
                            if(type!=1)return false;const auto values=opentype::single_adjustment(gpos,sub,glyph);if(!values)return false;
                            du+=static_cast<std::int64_t>(std::lround((*values)[0]*scale));dv+=static_cast<std::int64_t>(std::lround((*values)[1]*scale));advance+=static_cast<std::int64_t>(std::lround((*values)[2]*scale));return true;});
                    }
                    memory.store(GuestAddress{arg(10)},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(advance)));memory.store(GuestAddress{arg(11)},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(du)));memory.store(GuestAddress{arg(11)+4},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(dv)));
                }else {
                    // 替代字形清單第一項為原字形；lParameter n 取第 n 個替代字形，0 表示停用功能。
                    std::vector<std::uint32_t> list{glyph};
                    for(const auto lookup:opentype::feature_lookups(gsub,feature,script,language))opentype::visit_lookup(gsub,lookup,7,[&](unsigned type,std::size_t sub){
                        if(type==1){if(const auto mapped=opentype::single_substitution(gsub,sub,glyph)){list.push_back(*mapped);return true;}}
                        else if(type==3){const auto values=opentype::alternates(gsub,sub,glyph);if(!values.empty()){list.insert(list.end(),values.begin(),values.end());return true;}}
                        return false;});
                    if(name=="ScriptGetFontAlternateGlyphs")result=store_list(list,7,8,9,false);
                    else {if(!arg(8))return finish(invalid);const auto parameter=static_cast<std::size_t>(arg(6));memory.store(GuestAddress{arg(8)},16,parameter && parameter<list.size()?list[parameter]:glyph);}
                }
            }
            if(created){memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);if(!remember_cache(arg(1),font))return finish(out_of_memory);}
            return finish(result);
        }
        if(name=="ScriptShapeOpenType" || name=="ScriptPlaceOpenType"){
            // 預設 OpenType 功能造字與定位；與 ScriptShape／ScriptPlace 共用字型解析與原生造字，自訂功能範圍目前沿用預設功能。
            const bool shape=name=="ScriptShapeOpenType";
            if(!arg(2))return finish(invalid);
            const auto analysis=static_cast<unsigned>(memory.load(GuestAddress{arg(2)},16)),script_state=static_cast<unsigned>(memory.load(GuestAddress{arg(2)+2},16));
            if((analysis&0x3000U) || (script_state&~31U) || (analysis&1023U)>USCRIPT_CODE_LIMIT)return finish(unsupported);
            GuiFont font;bool created=false;if(const auto error=script_font(arg(0),arg(1),font,created))return finish(*error);
            if(shape){
                // (hdc, psc, psa, tagScript, tagLangSys, rcRangeChars, rpRangeProperties, cRanges, pwcChars, cChars, cMaxGlyphs, pwLogClust, pCharProps, pwOutGlyphs, pOutGlyphProps, pcGlyphs)
                const auto count=integer(9),capacity=integer(10);
                if(!arg(8) || !arg(11) || !arg(12) || !arg(13) || !arg(14) || !arg(15) || count<1 || count>65535 || capacity<0 || capacity>65535)return finish(invalid);
                std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(8)+i*2},16));
                const auto shaped=native_script_shape(font,text,(analysis&0x400)!=0,(analysis&0x4000)!=0,(analysis&1023)==0,(analysis&0x8000)!=0);
                if(shaped.missing)return finish(missing);if(shaped.glyphs.size()>static_cast<unsigned>(capacity))return finish(out_of_memory);
                memory.check(GuestAddress{arg(11)},shaped.clusters.size()*2,Access::Write);memory.check(GuestAddress{arg(12)},shaped.clusters.size()*2,Access::Write);
                memory.check(GuestAddress{arg(13)},shaped.glyphs.size()*2,Access::Write);memory.check(GuestAddress{arg(14)},shaped.glyphs.size()*4,Access::Write);
                for(std::size_t i=0;i<shaped.clusters.size();++i){
                    // fCanGlyphAlone：cluster 只有這個字元且只對應一個 glyph。
                    const auto start=shaped.clusters[i];const auto members=std::count(shaped.clusters.begin(),shaped.clusters.end(),start);
                    std::size_t glyph_count=0;for(std::size_t g=start;g<shaped.glyphs.size();++g){if(g>start && std::find(shaped.clusters.begin(),shaped.clusters.end(),static_cast<std::uint16_t>(g))!=shaped.clusters.end())break;++glyph_count;}
                    memory.store(GuestAddress{arg(11)+i*2},16,start);memory.store(GuestAddress{arg(12)+i*2},16,members==1 && glyph_count==1?1U:0U);
                }
                for(std::size_t i=0;i<shaped.glyphs.size();++i){memory.store(GuestAddress{arg(13)+i*2},16,shaped.glyphs[i]);memory.store(GuestAddress{arg(14)+i*4},16,shaped.attributes[i]);memory.store(GuestAddress{arg(14)+i*4+2},16,0);}
                memory.store(GuestAddress{arg(15)},32,shaped.glyphs.size());
            }else {
                // (hdc, psc, psa, tagScript, tagLangSys, rcRangeChars, rpRangeProperties, cRanges, pwcChars, pwLogClust, pCharProps, cChars, pwGlyphs, pGlyphProps, cGlyphs, piAdvance, pGoffset, pABC)
                const auto count=integer(14);if(!arg(12) || !arg(13) || !arg(15) || !arg(16) || count<1 || count>65535)return finish(invalid);
                std::u16string glyphs(static_cast<std::size_t>(count),u'\0');std::vector<std::uint16_t> attributes(static_cast<std::size_t>(count));
                for(unsigned i=0;i<static_cast<unsigned>(count);++i){glyphs[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(12)+i*2},16));attributes[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(13)+i*4},16));}
                std::vector<std::int32_t> advances;try{advances=native_gdi_glyph_advances(font,glyphs);}catch(const std::invalid_argument&){return finish(invalid);}
                if(advances.size()!=glyphs.size())return finish(invalid);
                memory.check(GuestAddress{arg(15)},glyphs.size()*4,Access::Write);memory.check(GuestAddress{arg(16)},glyphs.size()*8,Access::Write);if(arg(17))memory.check(GuestAddress{arg(17)},12,Access::Write);
                std::int64_t total=0;
                for(std::size_t i=0;i<glyphs.size();++i){const auto advance=(attributes[i]&0x40U)?0:advances[i];memory.store(GuestAddress{arg(15)+i*4},32,static_cast<std::uint32_t>(advance));memory.store(GuestAddress{arg(16)+i*8},64,0);total+=advance;}
                if(arg(17)){memory.store(GuestAddress{arg(17)},32,0);memory.store(GuestAddress{arg(17)+4},32,static_cast<std::uint32_t>(std::clamp<std::int64_t>(total,0,UINT32_MAX)));memory.store(GuestAddress{arg(17)+8},32,0);}
            }
            if(created){memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);if(!remember_cache(arg(1),font))return finish(out_of_memory);}
            return finish(0);
        }
        if(name=="ScriptStringAnalyse"){
            if(!arg(12))return finish(invalid);memory.check(GuestAddress{arg(12)},x86_?4:8,Access::Write);memory.store(GuestAddress{arg(12)},bits,0);
            const auto count=integer(2),capacity=integer(3);const auto flags=static_cast<std::uint32_t>(arg(5));
            if(!arg(1) || count<1 || count>65536 || capacity<0 || capacity>1048576)return finish(invalid);
            if((flags&~0x040019e0U) || integer(4)!=-1 || arg(9) || arg(10) || arg(11))return finish(unsupported);
            if((arg(7) && (memory.load(GuestAddress{arg(7)},32)&~0xffffU)) || (arg(8) && memory.load(GuestAddress{arg(8)},16)))return finish(unsupported);
            if((arg(0) || (flags&0x80)) && !gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)))return finish(invalid);
            std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(1)+i*2},16));
            const auto font=arg(0)?gui_font(gui_,gui_.contexts[arg(0)].font):GuiFont{};GuiScriptAnalysis analysis;analysis.dc=arg(0);analysis.flags=flags;analysis.shape=native_shape_text(font,text,(flags&0x100)!=0,(flags&0x1020)!=0);
            if(analysis.shape.missing)return finish(missing);unsigned glyphs=0;for(const auto& run:analysis.shape.runs)glyphs+=run.glyphs;if(capacity && glyphs>static_cast<unsigned>(capacity))return finish(out_of_memory);
            const auto handle=allocate_heap(memory,16+analysis.shape.attributes.size());if(!handle)return finish(out_of_memory);
            try{memory.store(GuestAddress{handle},32,static_cast<std::uint32_t>(analysis.shape.width));memory.store(GuestAddress{handle+4},32,static_cast<std::uint32_t>(analysis.shape.height));memory.store(GuestAddress{handle+8},32,static_cast<unsigned>(count));memory.write(GuestAddress{handle+16},analysis.shape.attributes);gui_.script_analyses.emplace(handle,std::move(analysis));memory.store(GuestAddress{arg(12)},bits,handle);return finish(0);}catch(...){gui_.script_analyses.erase(handle);free_heap(memory,handle,state);throw;}
        }
        if(name=="ScriptStringFree"){
            if(!arg(0))return finish(invalid);const auto handle=memory.load(GuestAddress{arg(0)},bits);if(!gui_.script_analyses.contains(handle))return finish(invalid);memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);gui_.script_analyses.erase(handle);free_heap(memory,handle,state);memory.store(GuestAddress{arg(0)},bits,0);return finish(0);
        }
        const auto found=gui_.script_analyses.find(arg(0));if(found==gui_.script_analyses.end())return finish(name.starts_with("ScriptString_p")?0:invalid);const auto& analysis=found->second;const auto& shape=analysis.shape;
        if(name=="ScriptString_pSize")return finish(arg(0));
        if(name=="ScriptStringGetOrder"){
            // 每個輸出字元在視覺順序中的 glyph 位置：依字元左緣排序，同一 cluster（相同左右緣）的字元共用位置。
            if(!arg(1))return finish(invalid);const auto count=shape.leading.size();std::vector<std::size_t> order(count);for(std::size_t i=0;i<count;++i)order[i]=i;
            const auto left=[&](std::size_t i){return std::min(shape.leading[i],shape.trailing[i]);};
            std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return left(a)<left(b);});
            memory.check(GuestAddress{arg(1)},count*4,Access::Write);std::uint32_t position=0;
            for(std::size_t k=0;k<count;++k){if(k && (left(order[k])!=left(order[k-1]) || std::max(shape.leading[order[k]],shape.trailing[order[k]])!=std::max(shape.leading[order[k-1]],shape.trailing[order[k-1]])))++position;memory.store(GuestAddress{arg(1)+order[k]*4},32,position);}
            return finish(0);
        }if(name=="ScriptString_pcOutChars")return finish(arg(0)+8);if(name=="ScriptString_pLogAttr")return finish(arg(0)+16);if(name=="ScriptStringValidate")return finish(0);
        if(name=="ScriptStringGetLogicalWidths"){
            if(!arg(1))return finish(invalid);memory.check(GuestAddress{arg(1)},shape.leading.size()*4,Access::Write);for(std::size_t i=0;i<shape.leading.size();++i)memory.store(GuestAddress{arg(1)+i*4},32,static_cast<unsigned>(std::abs(shape.trailing[i]-shape.leading[i])));return finish(0);
        }
        if(name=="ScriptStringCPtoX"){
            if(!arg(3) || integer(1)<0 || arg(1)>shape.leading.size())return finish(invalid);const auto index=static_cast<std::size_t>(arg(1));const auto x=index==shape.leading.size()?shape.trailing.back():arg(2)?shape.trailing[index]:shape.leading[index];memory.store(GuestAddress{arg(3)},32,static_cast<unsigned>(x));return finish(0);
        }
        if(name=="ScriptStringXtoCP"){
            if(!arg(2) || !arg(3))return finish(invalid);memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);std::int64_t best=INT64_MAX;unsigned index=0,trailing=0;
            for(unsigned i=0;i<shape.leading.size();++i)if(shape.attributes[i]&4)for(unsigned side=0;side<2;++side){const auto distance=std::abs(static_cast<std::int64_t>(integer(1))-(side?shape.trailing[i]:shape.leading[i]));if(distance<best){best=distance;index=i;trailing=side;}}
            memory.store(GuestAddress{arg(2)},32,index);memory.store(GuestAddress{arg(3)},32,trailing);return finish(0);
        }
        if(name=="ScriptStringOut"){
            if(!(analysis.flags&0x80) || !gui_.contexts.contains(analysis.dc) || (arg(3)&~6U) || ((arg(3)&4) && !arg(4)))return finish(invalid);if(integer(5)<integer(6))return finish(unsupported);
            auto& dc=gui_.contexts.at(analysis.dc);std::optional<GuiRegion> clip;if(dc.clipped)clip=gui_region_combine(GuiRegion{dc.clip},dc.excluded,4);GuiRect rectangle{};if(arg(4))for(unsigned i=0;i<4;++i)rectangle[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(4)+i*4},32));
            if(arg(3)&4){const auto region=gui_transform_region_rect(gui_device_transform(dc),rectangle);clip=clip?gui_region_combine(*clip,region,1):region;}
            const auto window=gui_.windows.find(analysis.dc-0x10000000);const auto paint=[&](GuiDraw draw){if(clip){draw.clipped=true;draw.clip=gui_region_bounds(*clip);draw.excluded=gui_region_combine(GuiRegion{draw.clip},*clip,4);}if(raster_draw(memory,analysis.dc,draw))return true;if(window==gui_.windows.end())return false;window->second.drawing.push_back(std::move(draw));return true;};
            if((arg(3)&2) && arg(4)){GuiDraw fill;fill.kind=GuiDraw::Kind::Fill;fill.color=dc.background;fill.x=rectangle[0];fill.y=rectangle[1];fill.right=rectangle[2];fill.bottom=rectangle[3];if(!paint(fill))return finish(unsupported);}
            for(const auto& run:shape.runs){GuiDraw draw;draw.kind=GuiDraw::Kind::Text;draw.font=run.font;draw.text=run.text;draw.color=dc.color;const auto x=static_cast<std::int64_t>(integer(1))+run.x,y=static_cast<std::int64_t>(integer(2))+run.top;if(x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX)return finish(invalid);draw.x=static_cast<std::int32_t>(x);draw.y=static_cast<std::int32_t>(y);if(!paint(draw))return finish(unsupported);}
            if(window!=gui_.windows.end())present_gui(window->first);return finish(0);
        }
        return finish(unsupported);
    }catch(const std::bad_alloc&){return finish(out_of_memory);}catch(const std::domain_error&){return finish(unsupported);}
}
}
