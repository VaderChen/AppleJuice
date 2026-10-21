#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/native_text_layout.hpp"
#include "x64arm/resource_cache.hpp"
#include "x64arm/crypto.hpp"
#include "font_export_acceleration.hpp"
#include <dispatch/dispatch.h>
#include <charconv>
#import <CoreText/CoreText.h>
#import <AppKit/AppKit.h>
#include "x64arm/windows_gui.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <vector>
#include <mutex>
#include <map>
#include <stdexcept>
#include <chrono>
#include <deque>
#include <OpenGL/glu.h>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unicode/uchar.h>

namespace x64arm {
namespace {
std::mutex gui_font_mutex;
std::map<std::u16string,std::shared_ptr<const void>> shaped_fonts;
struct CfRelease {void operator()(const void* object)const{if(object)CFRelease(object);}};
template<class T>using Cf=std::unique_ptr<std::remove_pointer_t<T>,CfRelease>;
std::u16string text(CFStringRef value){if(!value)return {};const auto length=CFStringGetLength(value);std::u16string out(static_cast<std::size_t>(length),u'\0');CFStringGetCharacters(value,CFRangeMake(0,length),reinterpret_cast<UniChar*>(out.data()));return out;}
std::vector<std::uint8_t> table(CTFontRef font,CTFontTableTag tag){Cf<CFDataRef> data(CTFontCopyTable(font,tag,kCTFontTableOptionNoOptions));if(!data)return {};const auto* bytes=CFDataGetBytePtr(data.get());return {bytes,bytes+CFDataGetLength(data.get())};}
std::uint16_t word(const std::vector<std::uint8_t>& bytes,std::size_t offset){return offset+2<=bytes.size()?static_cast<std::uint16_t>((static_cast<unsigned>(bytes[offset])<<8)|bytes[offset+1]):0;}
std::uint32_t dword(const std::vector<std::uint8_t>& bytes,std::size_t offset){return (static_cast<std::uint32_t>(word(bytes,offset))<<16)|word(bytes,offset+2);}
std::int32_t metric(CGFloat value){if(!std::isfinite(value))return 0;return static_cast<std::int32_t>(std::clamp(std::round(value),static_cast<CGFloat>(INT32_MIN),static_cast<CGFloat>(INT32_MAX)));}
std::optional<NativeFontFace> describe(CTFontRef initial){
        const auto em=CTFontGetUnitsPerEm(initial);if(!em || em>65535)return {};
        Cf<CTFontRef> font(CTFontCreateCopyWithAttributes(initial,em,nullptr,nullptr));if(!font)return {};
        Cf<CFNumberRef> format(static_cast<CFNumberRef>(CTFontCopyAttribute(font.get(),kCTFontFormatAttribute)));int format_value=0;if(format)CFNumberGetValue(format.get(),kCFNumberIntType,&format_value);
        // 此路徑提供可縮放 SFNT 字型；舊 bitmap / Type 1 沒有相同的 metrics/signature 契約。
        if(format_value!=kCTFontFormatOpenTypePostScript && format_value!=kCTFontFormatOpenTypeTrueType && format_value!=kCTFontFormatTrueType)return {};
        NativeFontFace out;Cf<CFStringRef> family_name(CTFontCopyFamilyName(initial)),postscript(CTFontCopyPostScriptName(initial));out.family=text(family_name.get());out.postscript_name=text(postscript.get());
        Cf<CFStringRef> full(CTFontCopyFullName(font.get())),style(CTFontCopyName(font.get(),kCTFontSubFamilyNameKey));out.full_name=text(full.get());out.style=text(style.get());
        out.em=static_cast<std::int32_t>(em);out.ascent=std::max(0,metric(CTFontGetAscent(font.get())));out.descent=std::max(0,metric(CTFontGetDescent(font.get())));out.line_gap=std::max(0,metric(CTFontGetLeading(font.get())));
        const auto traits=CTFontGetSymbolicTraits(font.get());out.italic=(traits&kCTFontItalicTrait)!=0;out.fixed_pitch=(traits&kCTFontMonoSpaceTrait)!=0;
        const auto os2=table(font.get(),kCTFontTableOS2),hhea=table(font.get(),kCTFontTableHhea);
        if(os2.size()>=78){
            out.weight=std::clamp<std::int32_t>(word(os2,4),1,1000);out.average_width=std::max(0,static_cast<int>(static_cast<std::int16_t>(word(os2,2))));
            for(unsigned part=0;part<4;++part)out.signature[part]=dword(os2,42+part*4);
            out.first_character=word(os2,64);out.last_character=word(os2,66);
            if(word(os2,74) || word(os2,76)){out.ascent=word(os2,74);out.descent=word(os2,76);}
            const auto family_class=word(os2,30)>>8;out.family_class=family_class>=1 && family_class<=7?0x10:family_class==8?0x20:family_class==10?0x40:family_class==9 || family_class==12?0x50:0;
            if(os2.size()>=86 && word(os2,0)>=1){out.signature[4]=dword(os2,78);out.signature[5]=dword(os2,82);}
            if(os2.size()>=96 && word(os2,0)>=2){out.default_character=word(os2,90);out.break_character=word(os2,92);}
        }else{
            out.weight=(traits&kCTFontBoldTrait)?700:400;
            Cf<CFCharacterSetRef> chars(CTFontCopyCharacterSet(font.get()));
            if(chars){bool latin=true;for(UniChar ch=32;ch<127;++ch)latin&=CFCharacterSetIsCharacterMember(chars.get(),ch);for(UniChar ch=160;ch<256;++ch)latin&=CFCharacterSetIsCharacterMember(chars.get(),ch);
                constexpr UniChar cp1252[]{0x20ac,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,0x02c6,0x2030,0x0160,0x2039,0x0152,0x017d,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x02dc,0x2122,0x0161,0x203a,0x0153,0x017e,0x0178};for(const auto ch:cp1252)latin&=CFCharacterSetIsCharacterMember(chars.get(),ch);
                if(latin)out.signature[4]|=1;
                bool found=false;for(std::uint32_t ch=0;ch<=65535;++ch)if(CFCharacterSetIsCharacterMember(chars.get(),static_cast<UniChar>(ch))){if(!found){out.first_character=static_cast<std::uint16_t>(ch);found=true;}out.last_character=static_cast<std::uint16_t>(ch);}}
        }
        if(!out.average_width){const UniChar ch='x';CGGlyph glyph=0;CGSize advance{};if(CTFontGetGlyphsForCharacters(font.get(),&ch,&glyph,1)){CTFontGetAdvancesForGlyphs(font.get(),kCTFontOrientationHorizontal,&glyph,&advance,1);out.average_width=std::max(0,metric(advance.width));}}
        out.maximum_width=hhea.size()>=12?word(hhea,10):std::max(out.average_width,metric(CTFontGetBoundingBox(font.get()).size.width));
        if(out.fixed_pitch)out.family_class=0x30;
        out.flags=out.italic?1U:0U;if(out.weight>=700)out.flags|=0x20U;if(!(out.flags&0x21U))out.flags|=0x100U;
        out.flags|=format_value==kCTFontFormatOpenTypePostScript?0x20000U:format_value==kCTFontFormatOpenTypeTrueType?0x40000U:0U;
        out.design_metrics={out.em,out.ascent,out.descent,out.line_gap,
            metric(CTFontGetCapHeight(font.get())),metric(CTFontGetXHeight(font.get())),
            metric(CTFontGetUnderlinePosition(font.get())),metric(CTFontGetUnderlineThickness(font.get())),
            os2.size()>=30?static_cast<std::int16_t>(word(os2,28)):0,os2.size()>=28?static_cast<std::int16_t>(word(os2,26)):0};
        out.native=std::shared_ptr<const void>(CFRetain(font.get()),[](const void* value){CFRelease(value);});
        return out;
}
}
NSFont* native_gui_font(const x64arm::GuiFont& model) {
    std::lock_guard lock(gui_font_mutex);
    static NSMutableDictionary<NSString*, NSFont*>* cache = nil;
    if (!cache) cache = [NSMutableDictionary dictionary];
    const char* configured=std::getenv("X64ARM_WINDOWS_UI_FONT");
    NSString* chosen=configured?[NSString stringWithUTF8String:configured]:nil;
    NSString* key = [NSString stringWithFormat:@"%@/%d/%d/%d/%d/%p/%@", [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(model.face.data()) length:model.face.size()], model.height, model.weight, model.italic, model.pitch_family, model.native.get(),chosen?:@""];
    if (NSFont* cached = cache[key]) return cached;
    const CGFloat requested = static_cast<CGFloat>(std::clamp<std::int64_t>(model.height ? std::abs(static_cast<std::int64_t>(model.height)) : 13, 1, 512));
    NSString* face = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(model.face.data()) length:model.face.size()];
    const bool fixed = (model.pitch_family & 3) == 1 || (model.pitch_family & 0xf0) == 0x30 ||
        [@[@"consolas", @"lucida console", @"fixedsys", @"terminal", @"courier new"] containsObject:face.lowercaseString];
    NSFontManager* manager = [NSFontManager sharedFontManager];
    const auto traits = static_cast<NSFontTraitMask>((model.weight >= 600 ? NSBoldFontMask : 0) | (model.italic ? NSItalicFontMask : 0));
    NSFont* font=nil;
    if(model.native){Cf<CTFontRef> resized(CTFontCreateCopyWithAttributes(static_cast<CTFontRef>(model.native.get()),requested,nullptr,nullptr));font=(__bridge NSFont*)resized.get();}
    // CoreText 的系統替代字型名稱可能無法重新開啟；保留真實 font 身分供 LOGFONT 往返。
    if(const auto found=shaped_fonts.find(model.face);!font && found!=shaped_fonts.end()){
        Cf<CTFontRef> resized(CTFontCreateCopyWithAttributes(static_cast<CTFontRef>(found->second.get()),requested,nullptr,nullptr));
        font=(__bridge NSFont*)resized.get();
    }
    if(!font && !fixed && windows_ui_font_alias(model.face)){
        // 啟動器只傳入已安裝的向量 family；獨立 CLI 的無效設定回到原生系統向量字型。
        if(chosen.length && ![chosen isEqualToString:@"auto"]){
            NSFont* candidate=[manager fontWithFamily:chosen traits:traits weight:model.weight >= 600 ? 9 : 5 size:requested];
            if(candidate && describe((__bridge CTFontRef)candidate))font=candidate;
        }
        if(!font)font=[NSFont systemFontOfSize:requested weight:model.weight >= 600 ? NSFontWeightBold : NSFontWeightRegular];
    }
    if(!font && face.length)font=[manager fontWithFamily:face traits:traits weight:model.weight >= 600 ? 9 : 5 size:requested];
    if (!font && face.length) font = [NSFont fontWithName:face size:requested];
    if (!font) font = fixed ? [NSFont monospacedSystemFontOfSize:requested weight:model.weight >= 600 ? NSFontWeightBold : NSFontWeightRegular]
        : [NSFont systemFontOfSize:requested weight:model.weight >= 600 ? NSFontWeightBold : NSFontWeightRegular];
    if (traits) font = [manager convertFont:font toHaveTrait:traits];
    // 正 lfHeight 指定字格高度；負值指定 em 高度，對應 Cocoa 的 point size。
    if (model.height > 0) font = [manager convertFont:font toSize:requested * requested / std::max<CGFloat>(1, font.ascender - font.descender)];
    if (cache.count >= 256) [cache removeAllObjects];
    cache[key] = font;
    return font;
}
std::u16string native_ui_font_family(){
    const auto face=describe((__bridge CTFontRef)native_gui_font(GuiFont{}));
    if(!face)throw std::runtime_error("原生系統介面沒有可縮放字型");
    return face->family;
}
NativeScriptShape native_script_shape(const GuiFont& model,std::u16string_view input,bool rtl,bool logical,bool unshaped,bool no_glyphs){
    NativeScriptShape result;result.clusters.resize(input.size());if(input.empty())return result;
    NSFont* font=native_gui_font(model);const auto base=(__bridge CTFontRef)font;
    NSString* string=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(input.data()) length:input.size()];
    struct Glyph {CGGlyph index;std::size_t character;double advance;};std::vector<Glyph> glyphs;
    if(unshaped || no_glyphs){
        std::vector<CGGlyph> mapped(input.size());CTFontGetGlyphsForCharacters(base,reinterpret_cast<const UniChar*>(input.data()),mapped.data(),static_cast<CFIndex>(input.size()));
        for(std::size_t i=0;i<input.size();++i){glyphs.push_back({no_glyphs?static_cast<CGGlyph>(input[i]):mapped[i],i,1});
            if(!no_glyphs && input[i]>=0xd800 && input[i]<=0xdbff && i+1<input.size() && input[i+1]>=0xdc00 && input[i+1]<=0xdfff)++i;}
    }else{
        // 單一 item 已完成雙向分析；強制方向，避免 CoreText 再分派混合文字的方向。
        NSArray* direction=@[@(static_cast<NSInteger>(rtl?NSWritingDirectionRightToLeft:NSWritingDirectionLeftToRight)|static_cast<NSInteger>(NSWritingDirectionOverride))];
        NSAttributedString* attributed=[[NSAttributedString alloc]initWithString:string attributes:@{NSFontAttributeName:font,NSWritingDirectionAttributeName:direction}];
        Cf<CTLineRef> line(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed));if(!line)throw std::bad_alloc();
        Cf<CFStringRef> base_name(CTFontCopyPostScriptName(base));auto runs=CTLineGetGlyphRuns(line.get());
        for(CFIndex i=0;i<CFArrayGetCount(runs);++i){auto run=static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs,i)));const auto count=CTRunGetGlyphCount(run);
            const auto physical=static_cast<CTFontRef>(CFDictionaryGetValue(CTRunGetAttributes(run),kCTFontAttributeName));Cf<CFStringRef> name(physical?CTFontCopyPostScriptName(physical):nullptr);
            if(!name || !CFEqual(name.get(),base_name.get())){result.missing=true;return result;}
            const auto size=static_cast<std::size_t>(count);std::vector<CGGlyph> indices(size);std::vector<CFIndex> characters(size);std::vector<CGSize> advances(size);
            CTRunGetGlyphs(run,CFRangeMake(0,0),indices.data());CTRunGetStringIndices(run,CFRangeMake(0,0),characters.data());CTRunGetAdvances(run,CFRangeMake(0,0),advances.data());
            for(std::size_t j=0;j<size;++j){if(characters[j]<0 || static_cast<std::size_t>(characters[j])>=input.size())throw std::domain_error("CoreText 回傳無效群集");
                const auto cluster=[string rangeOfComposedCharacterSequenceAtIndex:static_cast<NSUInteger>(characters[j])].location;
                glyphs.push_back({indices[j],cluster,advances[j].width});if(!indices[j])result.missing=true;}
        }
    }
    // 群集的內部字形保持 CoreText 順序；僅轉換群集的邏輯／視覺排列。
    const bool reversed=rtl && !logical;
    std::stable_sort(glyphs.begin(),glyphs.end(),[&](const auto& a,const auto& b){return reversed?a.character>b.character:a.character<b.character;});
    if(glyphs.size()>65535)throw std::bad_alloc();
    std::map<std::size_t,std::uint16_t> clusters;
    for(std::size_t i=0;i<glyphs.size();++i){const auto& glyph=glyphs[i];result.glyphs.push_back(glyph.index);const bool start=rtl && !logical?i+1==glyphs.size() || glyphs[i+1].character!=glyph.character:!i || glyphs[i-1].character!=glyph.character;
        // cluster map 指向視覺陣列內該群集的第一個 glyph；fClusterStart 指向邏輯第一個。
        clusters.try_emplace(glyph.character,static_cast<std::uint16_t>(i));
        const auto ch=input[glyph.character];const bool space=[[NSCharacterSet whitespaceCharacterSet]characterIsMember:ch];
        const bool mark=[[NSCharacterSet nonBaseCharacterSet]characterIsMember:ch] || (!start && glyph.advance==0);
        result.attributes.push_back(static_cast<std::uint16_t>((start?16:0)|(mark?32:0)|(glyph.advance==0?64:0)|(space?2:1)));
    }
    if(clusters.empty()){result.missing=true;return result;}
    for(std::size_t i=0;i<input.size();++i){auto cluster=clusters.upper_bound(i);if(cluster!=clusters.begin())--cluster;result.clusters[i]=cluster->second;}
    return result;
}
NativeTextShape native_shape_text(const GuiFont& model,std::u16string_view input,bool rtl,bool fallback){
    NativeTextShape output;if(input.empty())return output;
    NSFont* font=native_gui_font(model);NSString* string=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(input.data()) length:input.size()];
    NSMutableParagraphStyle* paragraph=[[NSMutableParagraphStyle alloc]init];paragraph.baseWritingDirection=rtl?NSWritingDirectionRightToLeft:NSWritingDirectionLeftToRight;
    NSAttributedString* attributed=[[NSAttributedString alloc]initWithString:string attributes:@{NSFontAttributeName:font,NSParagraphStyleAttributeName:paragraph}];
    Cf<CTLineRef> line(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed));if(!line)throw std::bad_alloc();
    CGFloat ascent=0,descent=0,leading=0;output.width=metric(CTLineGetTypographicBounds(line.get(),&ascent,&descent,&leading));output.ascent=metric(ascent);output.height=metric(std::ceil(ascent+descent+leading));
    output.leading.resize(input.size());output.trailing.resize(input.size());output.attributes.resize(input.size());
    for(std::size_t i=0;i<input.size();++i){output.leading[i]=metric(CTLineGetOffsetForStringIndex(line.get(),static_cast<CFIndex>(i),nullptr));output.trailing[i]=metric(CTLineGetOffsetForStringIndex(line.get(),static_cast<CFIndex>(i+1),nullptr));const auto range=[string rangeOfComposedCharacterSequenceAtIndex:i];if(range.location==i)output.attributes[i]|=4;if([[NSCharacterSet whitespaceAndNewlineCharacterSet]characterIsMember:input[i]])output.attributes[i]|=3;if(!i || [[NSCharacterSet whitespaceAndNewlineCharacterSet]characterIsMember:input[i-1]])output.attributes[i]|=8;}
    auto runs=CTLineGetGlyphRuns(line.get());const auto base=(__bridge CTFontRef)font;Cf<CFStringRef> base_name(CTFontCopyPostScriptName(base));
    for(CFIndex i=0;i<CFArrayGetCount(runs);++i){auto run=static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs,i)));const auto range=CTRunGetStringRange(run);const auto count=CTRunGetGlyphCount(run);if(count<=0 || range.location<0 || range.length<0 || static_cast<std::size_t>(range.location+range.length)>input.size())continue;
        const auto attributes=CTRunGetAttributes(run);const auto physical=static_cast<CTFontRef>(CFDictionaryGetValue(attributes,kCTFontAttributeName));if(!physical)throw std::runtime_error("CoreText 字形缺少字型");Cf<CFStringRef> name(CTFontCopyPostScriptName(physical));
        if(!model.native){std::lock_guard lock(gui_font_mutex);shaped_fonts.try_emplace(text(name.get()),std::shared_ptr<const void>(CFRetain(physical),[](const void* value){CFRelease(value);}));}
        std::vector<CGGlyph> glyphs(static_cast<std::size_t>(count));CTRunGetGlyphs(run,CFRangeMake(0,0),glyphs.data());if(std::find(glyphs.begin(),glyphs.end(),0)!=glyphs.end() || (!fallback && !CFEqual(base_name.get(),name.get())))output.missing=true;
        std::vector<CGPoint> positions(static_cast<std::size_t>(count));CTRunGetPositions(run,CFRangeMake(0,0),positions.data());CGPoint position=positions.front();for(const auto& point:positions)position.x=std::min(position.x,point.x);NativeTextRun item;item.font=model;item.font.face=text(name.get());if(model.native)item.font.native=std::shared_ptr<const void>(CFRetain(physical),[](const void* value){CFRelease(value);});const auto traits=CTFontGetSymbolicTraits(physical);item.font.weight=(traits&kCTFontBoldTrait)?700:400;item.font.italic=(traits&kCTFontItalicTrait)!=0;item.font.height=-std::max(1,metric(CTFontGetSize(physical)));item.font.width=0;item.text=std::u16string(input.substr(static_cast<std::size_t>(range.location),static_cast<std::size_t>(range.length)));item.x=metric(position.x);item.top=metric(ascent-CTFontGetAscent(physical)-position.y);item.glyphs=static_cast<unsigned>(count);output.runs.push_back(std::move(item));
    }
    return output;
}
NativeGdiPlacement native_gdi_placement(const GuiFont& model,std::u16string_view input,bool rtl,std::uint32_t flags){
    @autoreleasepool {
    NativeGdiPlacement output;output.clusters.resize(input.size());if(input.empty())return output;
    NSFont* font=native_gui_font(model);const auto base=(__bridge CTFontRef)font;
    NSString* string=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(input.data()) length:input.size()];
    struct Glyph {CGGlyph index;std::size_t character;double advance;};std::vector<Glyph> glyphs;
    const auto nominal=[&](std::size_t begin,std::size_t length){
        std::vector<CGGlyph> mapped(length);std::vector<CGSize> advances(length);
        auto characters=std::u16string(input.substr(begin,length));if(rtl && !(flags&0x800000U))for(auto& ch:characters)ch=static_cast<char16_t>(u_charMirror(ch));
        CTFontGetGlyphsForCharacters(base,reinterpret_cast<const UniChar*>(characters.data()),mapped.data(),static_cast<CFIndex>(length));
        CTFontGetAdvancesForGlyphs(base,kCTFontOrientationHorizontal,mapped.data(),advances.data(),static_cast<CFIndex>(length));
        for(std::size_t i=0;i<length;++i){const auto at=begin+i;const auto cluster=[string rangeOfComposedCharacterSequenceAtIndex:at].location;
            glyphs.push_back({mapped[i],cluster,advances[i].width});
            if(input[at]>=0xd800 && input[at]<=0xdbff && i+1<length && input[at+1]>=0xdc00 && input[at+1]<=0xdfff)++i;
        }
    };
    if(!(flags&0x38U))nominal(0,input.size());
    else {
        NSArray* direction=@[@(static_cast<NSInteger>(rtl?NSWritingDirectionRightToLeft:NSWritingDirectionLeftToRight)|static_cast<NSInteger>(NSWritingDirectionOverride))];
        NSMutableDictionary* attributes=[@{NSFontAttributeName:font,NSWritingDirectionAttributeName:direction,NSLigatureAttributeName:@((flags&0x20U)?1:0)} mutableCopy];
        if(!(flags&8U))attributes[NSKernAttributeName]=@0;
        // CoreText 會對 RTL 符號鏡射；SYMSWAPOFF 時先反轉可鏡射字元以抵銷，避免重複鏡射。
        auto shaping=std::u16string(input);if(rtl && (flags&0x800000U))for(auto& ch:shaping)ch=static_cast<char16_t>(u_charMirror(ch));
        NSString* shaping_string=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(shaping.data()) length:shaping.size()];
        NSAttributedString* attributed=[[NSAttributedString alloc]initWithString:shaping_string attributes:attributes];
        Cf<CTLineRef> line(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed));if(!line)throw std::bad_alloc();
        Cf<CFStringRef> base_name(CTFontCopyPostScriptName(base));auto runs=CTLineGetGlyphRuns(line.get());
        for(CFIndex i=0;i<CFArrayGetCount(runs);++i){auto run=static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs,i)));const auto range=CTRunGetStringRange(run);
            if(range.location<0 || range.length<0 || static_cast<std::size_t>(range.location+range.length)>input.size())throw std::domain_error("CoreText 回傳無效字串範圍");
            const auto physical=static_cast<CTFontRef>(CFDictionaryGetValue(CTRunGetAttributes(run),kCTFontAttributeName));Cf<CFStringRef> name(physical?CTFontCopyPostScriptName(physical):nullptr);
            // GCP 的 glyph 隨選取的 HFONT 使用；缺字保留該字型的 .notdef，不能回傳另一個 face 的 glyph ID。
            if(!name || !CFEqual(name.get(),base_name.get())){nominal(static_cast<std::size_t>(range.location),static_cast<std::size_t>(range.length));continue;}
            const auto count=static_cast<std::size_t>(CTRunGetGlyphCount(run));std::vector<CGGlyph> indices(count);std::vector<CFIndex> characters(count);std::vector<CGSize> advances(count);
            CTRunGetGlyphs(run,CFRangeMake(0,0),indices.data());CTRunGetStringIndices(run,CFRangeMake(0,0),characters.data());CTRunGetAdvances(run,CFRangeMake(0,0),advances.data());
            for(std::size_t j=0;j<count;++j){if(characters[j]<0 || static_cast<std::size_t>(characters[j])>=input.size())throw std::domain_error("CoreText 回傳無效字形群集");
                const auto cluster=[string rangeOfComposedCharacterSequenceAtIndex:static_cast<NSUInteger>(characters[j])].location;glyphs.push_back({indices[j],cluster,advances[j].width});}
        }
    }
    std::stable_sort(glyphs.begin(),glyphs.end(),[&](const auto& a,const auto& b){return rtl?a.character>b.character:a.character<b.character;});
    double scale=1;if(model.width){const auto face=describe(base);if(face && face->average_width>0)scale=std::abs(static_cast<double>(model.width))*face->em/(face->average_width*CTFontGetSize(base));}
    std::map<std::size_t,std::uint32_t> clusters;
    for(const auto& glyph:glyphs){
        const auto range=[string rangeOfComposedCharacterSequenceAtIndex:glyph.character];bool only_format=true;
        for(NSUInteger i=range.location;i<NSMaxRange(range);++i)only_format&=u_charType(input[i])==U_FORMAT_CHAR;
        if(only_format && !(flags&0x400000U))continue;
        clusters.try_emplace(glyph.character,static_cast<std::uint32_t>(output.glyphs.size()));output.glyphs.push_back(glyph.index);
        output.advances.push_back(!(flags&0x100U) && u_charType(input[glyph.character])==U_NON_SPACING_MARK?0:glyph.advance*scale);
    }
    if(clusters.empty())return output;
    for(std::size_t i=0;i<input.size();++i){auto at=clusters.upper_bound(i);if(at!=clusters.begin())--at;output.clusters[i]=at->second;}
    return output;
    }
}
std::vector<std::int32_t> native_gdi_glyph_advances(const GuiFont& model,std::u16string_view input){
    @autoreleasepool {
    const auto font=(__bridge CTFontRef)native_gui_font(model);const auto face=describe(font);if(!face)throw std::domain_error("無法取得選取字型");
    const auto scale=model.width && face->average_width>0?std::abs(static_cast<double>(model.width))*face->em/(face->average_width*CTFontGetSize(font)):1;
    std::vector<CGGlyph> glyphs(input.begin(),input.end());const auto limit=CTFontGetGlyphCount(font);
    if(std::any_of(glyphs.begin(),glyphs.end(),[&](auto g){return g>=limit;}))throw std::invalid_argument("字形索引超出字型範圍");
    std::vector<CGSize> widths(glyphs.size());if(!glyphs.empty())CTFontGetAdvancesForGlyphs(font,kCTFontOrientationHorizontal,glyphs.data(),widths.data(),static_cast<CFIndex>(glyphs.size()));
    std::vector<std::int32_t> result;double total=0;std::int32_t previous=0;
    for(const auto& width:widths){total+=width.width*scale;if(!std::isfinite(total) || total<INT32_MIN || total>INT32_MAX)throw std::invalid_argument("字形字距超出範圍");const auto current=static_cast<std::int32_t>(std::lround(total));result.push_back(current-previous);previous=current;}return result;
    }
}
NativeGlyphRaster native_gdi_glyph_raster(const GuiDraw& draw){
    @autoreleasepool {
    const auto font=(__bridge CTFontRef)native_gui_font(draw.font);const auto face=describe(font);if(!face)throw std::domain_error("無法取得選取字型");
    const auto scale=draw.font.width && face->average_width>0?std::abs(static_cast<double>(draw.font.width))*face->em/(face->average_width*CTFontGetSize(font)):1;
    const auto advances=draw.advances.empty()?native_gdi_glyph_advances(draw.font,draw.text):draw.advances;
    if(advances.size()!=draw.text.size() || (!draw.advances_y.empty() && draw.advances_y.size()!=draw.text.size()))throw std::invalid_argument("字形字距數目不符");
    NativeGlyphRun run;run.em=static_cast<float>(CTFontGetSize(font));run.baseline_y=static_cast<float>(std::ceil(CTFontGetAscent(font)));run.transform[0]=static_cast<float>(scale);run.indices.assign(draw.text.begin(),draw.text.end());
    double y=0;for(std::size_t i=0;i<advances.size();++i){run.advances.push_back(static_cast<float>(advances[i]/scale));run.offsets.push_back({0,static_cast<float>(-y)});if(!draw.advances_y.empty())y+=draw.advances_y[i];}
    auto result=native_font_glyph_raster(*face,run);
    // 裝飾沿每個字形的邏輯位置繪製，包含 ETO_PDY 的垂直位移。
    if((draw.font.underline || draw.font.strikeout) && !advances.empty()){
        std::vector<std::array<std::int32_t,4>> lines;double x=0;y=0;
        const auto thickness=std::max(1.0,std::ceil(CTFontGetUnderlineThickness(font)));
        for(std::size_t i=0;i<advances.size();++i){for(const bool underline:{true,false}){if(underline?!draw.font.underline:!draw.font.strikeout)continue;
                const auto top=run.baseline_y+y-(underline?CTFontGetUnderlinePosition(font):CTFontGetXHeight(font)/2);
                const std::array<double,4> edge{std::floor(std::min(x,x+advances[i])),std::floor(top),std::ceil(std::max(x,x+advances[i])),std::ceil(top+thickness)};
                if(std::any_of(edge.begin(),edge.end(),[](double v){return !std::isfinite(v) || v<INT32_MIN || v>INT32_MAX;}))throw std::invalid_argument("字形裝飾範圍溢位");
                if(edge[0]<edge[2])lines.push_back({static_cast<int>(edge[0]),static_cast<int>(edge[1]),static_cast<int>(edge[2]),static_cast<int>(edge[3])});}
            x+=advances[i];if(!draw.advances_y.empty())y+=draw.advances_y[i];}
        auto bounds=result.bounds;bool empty=result.alpha.empty();for(const auto& line:lines){if(empty){bounds=line;empty=false;}else {bounds[0]=std::min(bounds[0],line[0]);bounds[1]=std::min(bounds[1],line[1]);bounds[2]=std::max(bounds[2],line[2]);bounds[3]=std::max(bounds[3],line[3]);}}
        if(!empty){const auto width=static_cast<std::int64_t>(bounds[2])-bounds[0],height=static_cast<std::int64_t>(bounds[3])-bounds[1];if(width>32768 || height>32768 || width*height*result.channels>64*1024*1024)throw std::bad_alloc();
            std::vector<std::uint8_t> alpha(static_cast<std::size_t>(width*height*result.channels));const auto old_width=static_cast<std::int64_t>(result.bounds[2])-result.bounds[0];
            for(int row=result.bounds[1];row<result.bounds[3];++row)std::copy_n(result.alpha.begin()+(row-result.bounds[1])*old_width*result.channels,old_width*result.channels,alpha.begin()+((row-bounds[1])*width+result.bounds[0]-bounds[0])*result.channels);
            for(const auto& line:lines)for(int row=line[1];row<line[3];++row)std::fill_n(alpha.begin()+((row-bounds[1])*width+line[0]-bounds[0])*result.channels,static_cast<std::size_t>(line[2]-line[0])*result.channels,255);
            result.bounds=bounds;result.alpha=std::move(alpha);}
    }
    return result;
    }
}
std::vector<NativeFontFace> native_load_font_faces(std::span<const std::uint8_t> bytes){
    if(bytes.empty() || bytes.size()>native_font_file_limit)return {};
    Cf<CFDataRef> data(CFDataCreate(nullptr,bytes.data(),static_cast<CFIndex>(bytes.size())));if(!data)return {};
    Cf<CFArrayRef> descriptors(CTFontManagerCreateFontDescriptorsFromData(data.get()));if(!descriptors || CFArrayGetCount(descriptors.get())>256)return {};
    std::vector<NativeFontFace> result;
    for(CFIndex i=0;i<CFArrayGetCount(descriptors.get());++i){
        const auto descriptor=static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors.get(),i));Cf<CTFontRef> font(CTFontCreateWithFontDescriptor(descriptor,16,nullptr));if(!font)return {};
        auto face=describe(font.get());if(!face)return {};result.push_back(std::move(*face));
    }
    return result;
}
std::optional<NativeFontFace> native_resolve_font(const GuiFont& model){
    NSFont* font=native_gui_font(model);if(!font)return {};return describe((__bridge CTFontRef)font);
}
NativeTextLayout native_text_layout(std::u16string_view input,std::span<const NativeLayoutSpan> spans,const NativeLayoutOptions& options){
    @autoreleasepool {
    NativeTextLayout output;
    if(spans.empty())throw std::invalid_argument("文字排版缺少字型");
    NSString* string=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(input.data()) length:input.size()];
    NSMutableAttributedString* attributed=[[NSMutableAttributedString alloc]initWithString:string];
    NSMutableParagraphStyle* paragraph=[[NSMutableParagraphStyle alloc]init];
    paragraph.baseWritingDirection=options.reading?NSWritingDirectionRightToLeft:NSWritingDirectionLeftToRight;
    paragraph.defaultTabInterval=options.tab;paragraph.tabStops=@[];
    [attributed addAttribute:NSParagraphStyleAttributeName value:paragraph range:NSMakeRange(0,input.size())];
    CGFloat default_ascent=0,default_descent=0,default_leading=0;
    for(const auto& span:spans){
        Cf<CTFontRef> font(span.face.native?CTFontCreateCopyWithAttributes(static_cast<CTFontRef>(span.face.native.get()),span.size,nullptr,nullptr):nullptr);
        if(!font){Cf<CFStringRef> name(CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(span.face.postscript_name.data()),static_cast<CFIndex>(span.face.postscript_name.size())));font.reset(CTFontCreateWithName(name.get(),span.size,nullptr));}
        if(!font)throw std::bad_alloc{};
        default_ascent=std::max(default_ascent,CTFontGetAscent(font.get()));default_descent=std::max(default_descent,CTFontGetDescent(font.get()));default_leading=std::max(default_leading,CTFontGetLeading(font.get()));
        if(span.length && !span.locale.empty()){NSString* locale=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(span.locale.data()) length:span.locale.size()];[attributed addAttribute:(__bridge NSString*)kCTLanguageAttributeName value:locale range:NSMakeRange(span.start,span.length)];}
        if(span.length){[attributed addAttribute:(__bridge NSString*)kCTFontAttributeName value:(__bridge id)font.get() range:NSMakeRange(span.start,span.length)];
            // 保留格式區段，即使相鄰字型相同，繪製效果與裝飾仍各自派送。
            [attributed addAttribute:@"X64ArmTextSpan" value:@(span.start) range:NSMakeRange(span.start,span.length)];}
    }
    Cf<CTTypesetterRef> typesetter(CTTypesetterCreateWithAttributedString((__bridge CFAttributedStringRef)attributed));if(!typesetter)throw std::bad_alloc{};
    std::uint32_t position=0;float top=0;bool last_empty=input.empty() || (!input.empty() && (input.back()==u'\n' || input.back()==u'\r'));
    bool has_ink=false;std::array<float,4> ink{};
    while(position<input.size() || last_empty){
        const auto start=position;std::uint32_t end=start;while(end<input.size() && input[end]!=u'\r' && input[end]!=u'\n')++end;
        std::uint32_t length=end-start,newline=0;
        if(options.wrapping!=1 && length){
            CFIndex suggested=options.wrapping==4?CTTypesetterSuggestClusterBreak(typesetter.get(),start,std::max(0.0f,options.width)):CTTypesetterSuggestLineBreak(typesetter.get(),start,std::max(0.0f,options.width));
            if(suggested<=0){const auto range=[string rangeOfComposedCharacterSequenceAtIndex:start];suggested=static_cast<CFIndex>(range.length);}
            length=std::min(length,static_cast<std::uint32_t>(suggested));
        }
        if(start+length==end && end<input.size())newline=(input[end]==u'\r' && end+1<input.size() && input[end+1]==u'\n')?2:1;
        Cf<CTLineRef> line(CTTypesetterCreateLine(typesetter.get(),CFRangeMake(start,length)));if(!line)throw std::bad_alloc{};
        CGFloat ascent=default_ascent,descent=default_descent,leading=default_leading;
        double width=0;if(length)width=CTLineGetTypographicBounds(line.get(),&ascent,&descent,&leading);
        NativeLayoutLine item;item.start=start;item.length=length+newline;item.newline=newline;
        for(auto at=start+length;at>start && [[NSCharacterSet whitespaceCharacterSet]characterIsMember:input[at-1]];--at)++item.trailing;
        item.trailing+=newline;item.width_trailing=static_cast<float>(width);item.width=static_cast<float>(width-CTLineGetTrailingWhitespaceWidth(line.get()));
        item.height=options.spacing==1?options.line_spacing:static_cast<float>(ascent+descent+leading);
        item.baseline=options.spacing==1?options.baseline:static_cast<float>(ascent);item.top=top;
        const auto align=options.alignment==2?2U:options.alignment==1?(options.reading?0U:1U):(options.reading?1U:0U);
        item.x=align==2?(options.width-item.width)*0.5f:align==1?options.width-item.width:0;
        const auto line_index=static_cast<unsigned>(output.lines.size());output.lines.push_back(item);top+=item.height;
        std::map<std::uint32_t,bool> boundaries;boundaries[start]=options.reading!=0;boundaries[start+length]=options.reading!=0;
        const auto runs=CTLineGetGlyphRuns(line.get());
        for(CFIndex i=0;i<CFArrayGetCount(runs);++i){
            auto run=static_cast<CTRunRef>(const_cast<void*>(CFArrayGetValueAtIndex(runs,i)));const auto count=CTRunGetGlyphCount(run);if(count<=0)continue;
            const auto range=CTRunGetStringRange(run);const auto attributes=CTRunGetAttributes(run);const auto font=static_cast<CTFontRef>(CFDictionaryGetValue(attributes,kCTFontAttributeName));
            auto face=describe(font);if(!face)throw std::runtime_error("CoreText 排版字型不支援 SFNT");
            NativeLayoutRun shaped;shaped.face=std::move(*face);shaped.start=static_cast<std::uint32_t>(range.location);shaped.length=static_cast<std::uint32_t>(range.length);shaped.line=line_index;
            auto& glyphs=shaped.glyphs;glyphs.em=static_cast<float>(CTFontGetSize(font));glyphs.rtl=(CTRunGetStatus(run)&kCTRunStatusRightToLeft)!=0;glyphs.pixels_per_dip=options.pixels_per_dip;glyphs.measuring=options.measuring;
            if(count>65535 || range.location<0 || range.length<0 || static_cast<std::size_t>(range.location+range.length)>input.size())throw std::bad_alloc{};
            const auto glyph_count=static_cast<std::size_t>(count);glyphs.indices.resize(glyph_count);glyphs.advances.resize(glyph_count);glyphs.offsets.resize(glyph_count);std::vector<CGPoint> points(glyph_count);std::vector<CGSize> advances(glyph_count);std::vector<CFIndex> indices(glyph_count);
            CTRunGetGlyphs(run,CFRangeMake(0,0),glyphs.indices.data());CTRunGetPositions(run,CFRangeMake(0,0),points.data());CTRunGetAdvances(run,CFRangeMake(0,0),advances.data());CTRunGetStringIndices(run,CFRangeMake(0,0),indices.data());
            glyphs.baseline_x=item.x+static_cast<float>(points[0].x+(glyphs.rtl?advances[0].width:0));glyphs.baseline_y=item.top+item.baseline;
            shaped.clusters.resize(shaped.length);std::map<std::uint32_t,std::uint16_t> mapping;float cursor=0;
            for(std::size_t j=0;j<glyphs.indices.size();++j){
                if(indices[j]<0 || static_cast<std::size_t>(indices[j])>=input.size())throw std::domain_error("CoreText 排版群集越界");
                const auto composed=[string rangeOfComposedCharacterSequenceAtIndex:static_cast<NSUInteger>(indices[j])];const auto cluster=static_cast<std::uint32_t>(composed.location);boundaries[cluster]=glyphs.rtl;
                mapping.try_emplace(static_cast<std::uint32_t>(indices[j]),static_cast<std::uint16_t>(j));
                auto advance=static_cast<float>(advances[j].width);if(options.measuring==1)advance=std::round(advance*options.pixels_per_dip)/options.pixels_per_dip;
                glyphs.advances[j]=advance;if(glyphs.rtl)cursor-=advance;
                const auto raw_x=static_cast<float>(points[j].x)+item.x;
                glyphs.offsets[j]={glyphs.rtl?glyphs.baseline_x+cursor-raw_x:raw_x-glyphs.baseline_x-cursor,static_cast<float>(points[j].y)};
                if(!glyphs.rtl)cursor+=advance;shaped.width+=advance;
            }
            for(std::uint32_t ch=0;ch<shaped.length;++ch){auto at=mapping.upper_bound(shaped.start+ch);shaped.clusters[ch]=at==mapping.begin()?0:std::prev(at)->second;}
            output.bidi_depth=std::max(output.bidi_depth,glyphs.rtl?2U:1U);output.runs.push_back(std::move(shaped));
        }
        for(auto at=boundaries.begin();at!=boundaries.end();++at){auto next=std::next(at);if(next==boundaries.end() || at->first>=start+length)break;
            const auto first=at->first,last=next->first;const auto a=CTLineGetOffsetForStringIndex(line.get(),first,nullptr),b=CTLineGetOffsetForStringIndex(line.get(),last,nullptr);
            NativeLayoutCluster c;c.start=first;c.length=last-first;c.line=line_index;c.x=item.x+static_cast<float>(std::min(a,b));c.width=static_cast<float>(std::abs(b-a));c.rtl=at->second;
            c.whitespace=true;for(auto ch=first;ch<last;++ch)c.whitespace&=[[NSCharacterSet whitespaceCharacterSet]characterIsMember:input[ch]];
            c.wrap=c.whitespace || input[last-1]==u'-' || (input[last-1]>=0x2e80 && input[last-1]<=0x9fff);output.clusters.push_back(c);
        }
        if(newline)output.clusters.push_back({start+length,newline,line_index,item.x+item.width_trailing,0,false,true,options.reading!=0,true});
        const auto bounds=CTLineGetImageBounds(line.get(),nullptr);
        if(!CGRectIsEmpty(bounds)){const std::array<float,4> rectangle{item.x+static_cast<float>(bounds.origin.x),item.top+item.baseline-static_cast<float>(CGRectGetMaxY(bounds)),item.x+static_cast<float>(CGRectGetMaxX(bounds)),item.top+item.baseline-static_cast<float>(bounds.origin.y)};
            if(!has_ink){ink=rectangle;has_ink=true;}else {ink[0]=std::min(ink[0],rectangle[0]);ink[1]=std::min(ink[1],rectangle[1]);ink[2]=std::max(ink[2],rectangle[2]);ink[3]=std::max(ink[3],rectangle[3]);}}
        position=start+length+newline;if(position==start){last_empty=false;break;}if(position==input.size() && !newline)last_empty=false;
    }
    output.height=top;output.top=options.paragraph==1?options.height-top:options.paragraph==2?(options.height-top)*0.5f:0;
    bool first=true;for(auto& line:output.lines){line.top+=output.top;if(first){output.left=line.x;first=false;}else output.left=std::min(output.left,line.x);output.width=std::max(output.width,line.width);output.width_trailing=std::max(output.width_trailing,line.width_trailing);}
    for(auto& run:output.runs)run.glyphs.baseline_y+=output.top;
    float word_width=0;for(const auto& cluster:output.clusters){if(!cluster.whitespace && !cluster.newline)word_width+=cluster.width;if(cluster.wrap || cluster.newline){output.minimum=std::max(output.minimum,word_width);word_width=0;}}output.minimum=std::max(output.minimum,word_width);
    if(has_ink)output.overhang={std::max(0.0f,-ink[0]),std::max(0.0f,-(ink[1]+output.top)),std::max(0.0f,ink[2]-options.width),std::max(0.0f,ink[3]+output.top-options.height)};
    return output;
    }
}
namespace {
Cf<CTFontRef> physical_font(const NativeFontFace& face){
    if(face.native)return Cf<CTFontRef>(static_cast<CTFontRef>(CFRetain(face.native.get())));
    Cf<CFStringRef> name(CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(face.postscript_name.data()),static_cast<CFIndex>(face.postscript_name.size())));
    Cf<CTFontRef> font(CTFontCreateWithName(name.get(),face.em,nullptr));
    Cf<CFStringRef> actual(font?CTFontCopyPostScriptName(font.get()):nullptr);
    if(!actual || text(actual.get())!=face.postscript_name)throw std::runtime_error("原生字型身分已失效");return font;
}
}
std::vector<std::uint8_t> native_font_table(const NativeFontFace& face,std::uint32_t tag){
    const auto font=physical_font(face);return table(font.get(),tag);
}
std::vector<NativeFontName> native_font_names(const NativeFontFace& face,std::uint16_t name_id){
    const auto bytes=native_font_table(face,0x6e616d65U);if(bytes.size()<6)return {};
    const auto version=word(bytes,0),count=word(bytes,2),storage=word(bytes,4);if(version>1 || count>16384 || 6U+count*12U>bytes.size() || storage>bytes.size())throw std::invalid_argument("字型 name table 不合法");
    const auto decode=[&](unsigned at,unsigned size,bool roman){
        if(at>bytes.size() || size>bytes.size()-at)throw std::invalid_argument("字型 name 字串超出範圍");
        if(!roman && (size&1U))throw std::invalid_argument("字型 name UTF-16 長度不合法");
        Cf<CFStringRef> value(CFStringCreateWithBytes(nullptr,bytes.data()+at,size,roman?kCFStringEncodingMacRoman:kCFStringEncodingUTF16BE,false));
        if(!value)throw std::invalid_argument("字型 name 編碼不合法");return text(value.get());
    };
    std::map<std::u16string,std::pair<unsigned,std::u16string>> names;
    for(unsigned i=0;i<count;++i){const unsigned at=6+i*12;const auto platform=word(bytes,at),encoding=word(bytes,at+2),language=word(bytes,at+4);
        if(word(bytes,at+6)!=name_id || !word(bytes,at+8))continue;
        const bool roman=platform==1 && encoding==0;
        if(!roman && platform!=0 && !(platform==3 && (encoding==0 || encoding==1 || encoding==10)))continue;
        std::u16string locale;
        if(version==1 && language>=0x8000){
            const unsigned tags_at=6+count*12;if(tags_at+2>bytes.size())throw std::invalid_argument("字型 language tag header 不完整");
            const auto tags=word(bytes,tags_at);const unsigned index=language-0x8000;if(index>=tags || tags_at+2U+tags*4U>bytes.size())throw std::invalid_argument("字型 language tag 不合法");
            const auto entry=tags_at+2+index*4;locale=decode(storage+word(bytes,entry+2),word(bytes,entry),false);
        }else if(platform==3){Cf<CFStringRef> value(CFLocaleCreateLocaleIdentifierFromWindowsLocaleCode(nullptr,language));locale=text(value.get());if(locale.empty())continue;}
        else if(roman){Cf<CFStringRef> value(CFLocaleCreateCanonicalLocaleIdentifierFromScriptManagerCodes(nullptr,static_cast<LangCode>(language),-1));locale=text(value.get());if(locale.empty())continue;}
        std::replace(locale.begin(),locale.end(),u'_',u'-');
        // Windows 的舊中文 LCID 是 zh-地區；name v1 的明示 BCP-47 tag 則保留 script。
        if(platform==3 && language<0x8000 && locale.starts_with(u"zh-")){if(locale.starts_with(u"zh-Hans-"))locale.erase(3,5);else if(locale.starts_with(u"zh-Hant-"))locale.erase(3,5);}
        auto value=decode(storage+word(bytes,at+10),word(bytes,at+8),roman);if(value.find(u'\0')!=value.npos)throw std::invalid_argument("字型 name 包含 NUL");
        const unsigned priority=platform==3?3U:platform==0?2U:1U;const auto old=names.find(locale);
        if(old==names.end() || priority>old->second.first)names[locale]={priority,std::move(value)};
    }
    std::vector<NativeFontName> result;for(auto& [locale,item]:names)result.push_back({locale,std::move(item.second)});return result;
}
namespace {
#include "font_outline_export.inc"
#include "font_resource_identity.inc"
}
std::vector<std::uint8_t> native_font_file(const NativeFontFace& face){
    const auto font=physical_font(face);
    // 不可變 CTFont 的查長度／讀取及 GDI／DWrite 共用匯出；不同 variation、
    // 自訂字型及模擬參數不得只因 family 相同就混用內容。
    struct Export {Cf<CTFontRef> font;unsigned simulations;std::int32_t em;std::vector<std::uint8_t> bytes;};
    static std::mutex export_mutex;
    static std::vector<Export> exports;
    static std::size_t exported_bytes=0;
    std::lock_guard lock(export_mutex);
    for(std::size_t i=0;i<exports.size();++i)if(exports[i].simulations==face.simulations && exports[i].em==face.em && CFEqual(exports[i].font.get(),font.get())){
        std::rotate(exports.begin()+static_cast<std::ptrdiff_t>(i),exports.begin()+static_cast<std::ptrdiff_t>(i+1),exports.end());
        return exports.back().bytes;
    }
    Cf<CFArrayRef> available(CTFontCopyAvailableTables(font.get(),kCTFontTableOptionNoOptions));
    std::vector<std::pair<std::uint32_t,std::vector<std::uint8_t>>> tables;
    for(CFIndex i=0;available && i<CFArrayGetCount(available.get());++i){const auto tag=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(CFArrayGetValueAtIndex(available.get(),i)));
        if(tag==0x44534947U)continue; // 重建 directory 後原 DSIG 簽章不再適用。
        auto data=table(font.get(),tag);if(data.empty())continue;if(tag==0x68656164U && data.size()>=12)std::fill(data.begin()+8,data.begin()+12,0);tables.emplace_back(tag,std::move(data));
    }
    const bool generated_outlines=std::none_of(tables.begin(),tables.end(),[](const auto& entry){return entry.first==0x676c7966 || entry.first==0x43464620 || entry.first==0x43464632 || entry.first==0x73626978 || entry.first==0x43424454 || entry.first==0x45424454;});
    // 完整 tables 的封裝比來源驗證及磁碟預載便宜；只持久保存需重建輪廓的產物。
    const auto* all=std::getenv("X64ARM_FONT_CACHE_ALL");
    const auto directory=(generated_outlines || (all && std::string_view(all)=="1"))?font_resource_directory():std::filesystem::path{};
    std::sort(tables.begin(),tables.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    const auto identity=directory.empty()?std::vector<std::uint8_t>{}:font_resource_identity(face,font.get(),tables);
    const auto started=std::chrono::steady_clock::now();
    auto resource=ResourceCache(directory).get_or_create(identity,[&]{
    if(generated_outlines)export_native_outlines(face,font.get(),tables);
    if(tables.empty() || tables.size()>4095)throw std::runtime_error("無法取得有效 OpenType tables");
    std::sort(tables.begin(),tables.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    const auto count=static_cast<unsigned>(tables.size());unsigned power=1,selector=0;while(power*2<=count){power*=2;++selector;}
    std::vector<std::uint8_t> bytes(12+count*16);const auto put=[&](std::size_t offset,unsigned size,std::uint32_t value){for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*(size-1-i)));};
    const bool cff=std::any_of(tables.begin(),tables.end(),[](const auto& item){return item.first==0x43464620U || item.first==0x43464632U;});
    put(0,4,cff?0x4f54544fU:0x10000U);put(4,2,count);put(6,2,power*16);put(8,2,selector);put(10,2,count*16-power*16);
    const bool accelerated=font_export_detail::acceleration_enabled();
    const auto sum=[accelerated](std::span<const std::uint8_t> data){return font_export_detail::checksum(data,accelerated);};
    std::size_t head=0,total_size=bytes.size();std::uint32_t table_checksums=0;
    for(const auto& entry:tables){
        const auto size=entry.second.size();
        if(size>native_font_file_limit-3U || total_size>native_font_file_limit-3U-size)throw std::bad_alloc{};
        total_size=(total_size+size+3)&~std::size_t{3};
    }
    bytes.reserve(total_size);
    // 共用容量上限小於 SFNT 的 32 位元 offset；連同 padding 檢查溢位。
    for(unsigned i=0;i<count;++i){const auto& [tag,data]=tables[i];if(data.size()>native_font_file_limit-3U || bytes.size()>native_font_file_limit-3U-data.size())throw std::bad_alloc{};
        const auto offset=static_cast<std::uint32_t>(bytes.size());put(12+i*16,4,tag);const auto table_checksum=sum(data);table_checksums+=table_checksum;put(16+i*16,4,table_checksum);put(20+i*16,4,offset);put(24+i*16,4,static_cast<std::uint32_t>(data.size()));
        bytes.insert(bytes.end(),data.begin(),data.end());bytes.resize((bytes.size()+3)&~std::size_t{3});if(tag==0x68656164U && data.size()>=12)head=offset;
    }
    // 每個 table 均 4-byte 對齊／補零；完整 SFNT sum 等於 directory 加各 table sum。
    // 重用 NEON 的結果，避免再次掃描整份大字型。
    if(head)put(head+8,4,0xb1b0afbaU-sum(std::span<const std::uint8_t>(bytes).first(12+count*16))-table_checksums);
    return bytes;
    });
    if(const auto* trace=std::getenv("X64ARM_FONT_CACHE_TRACE");trace && std::string_view(trace)=="1"){
        Cf<CFStringRef> name(CTFontCopyPostScriptName(font.get()));
        std::fprintf(stderr,"font_cache pid=%d hit=%u stored=%u pending=%u memory_hit=%u bytes=%zu stored_bytes=%llu build_ms=%.3f read_ms=%.3f decode_ms=%.3f encode_ms=%.3f write_ms=%.3f milliseconds=%.3f face=%s\n",::getpid(),resource.hit,resource.stored,resource.pending,resource.memory_hit,resource.bytes.size(),static_cast<unsigned long long>(resource.stored_bytes),
            resource.build_ms,resource.read_ms,resource.decode_ms,resource.encode_ms,resource.write_ms,
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),[(__bridge NSString*)name.get() UTF8String]);
    }
    auto bytes=std::move(resource.bytes);
    // 最多 32 個實例、256 MiB；持有 CTFont 防止指標重用，最近使用者留到最後。
    while(!exports.empty() && (exports.size()>=32 || exported_bytes>native_font_file_limit-bytes.size())){
        exported_bytes-=exports.front().bytes.size();exports.erase(exports.begin());
    }
    exports.push_back({Cf<CTFontRef>(static_cast<CTFontRef>(CFRetain(font.get()))),face.simulations,face.em,bytes});
    exported_bytes+=bytes.size();return bytes;
}
namespace {
Cf<CGPathRef> simulated_font_path(CTFontRef font,CGGlyph glyph,unsigned simulations,const CGAffineTransform* transform){
    Cf<CGPathRef> path(CTFontCreatePathForGlyph(font,glyph,nullptr));if(!path)return path;
    if(simulations&2){const auto shear=CGAffineTransformMake(1,0,.2,1,0,0);path=Cf<CGPathRef>(CGPathCreateCopyByTransformingPath(path.get(),&shear));}
    if((simulations&1) && !CGPathIsEmpty(path.get())){const auto strength=CTFontGetSize(font)/24;Cf<CGPathRef> stroke(CGPathCreateCopyByStrokingPath(path.get(),nullptr,strength,kCGLineCapRound,kCGLineJoinRound,4));Cf<CGMutablePathRef> combined(CGPathCreateMutable());if(!stroke || !combined)throw std::bad_alloc{};CGPathAddPath(combined.get(),nullptr,path.get());CGPathAddPath(combined.get(),nullptr,stroke.get());path=Cf<CGPathRef>(CGPathCreateCopy(combined.get()));}
    if(transform)path=Cf<CGPathRef>(CGPathCreateCopyByTransformingPath(path.get(),transform));return path;
}
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::vector<std::array<double,2>> native_polygon_triangles(const std::vector<std::vector<std::array<double,2>>>& contours){
    struct Tess {std::deque<std::array<double,3>> points;std::vector<std::array<double,2>> triangles;std::exception_ptr exception;unsigned error{};};
    Tess context;auto* tess=gluNewTess();if(!tess)throw std::bad_alloc{};
    const auto edge=+[](GLboolean,void*){};
    const auto vertex=+[](void* point,void* user){auto& c=*static_cast<Tess*>(user);if(c.exception)return;try{auto* p=static_cast<double*>(point);if(c.triangles.size()>=3000000)throw std::length_error("多邊形三角形數超限");c.triangles.push_back({p[0],p[1]});}catch(...){c.exception=std::current_exception();}};
    const auto error=+[](GLenum code,void* user){static_cast<Tess*>(user)->error=code;};
    const auto combine=+[](GLdouble coords[3],void*[4],GLfloat[4],void** result,void* user){auto& c=*static_cast<Tess*>(user);*result=nullptr;if(c.exception)return;try{c.points.push_back({coords[0],coords[1],coords[2]});*result=c.points.back().data();}catch(...){c.exception=std::current_exception();}};
    gluTessCallback(tess,GLU_TESS_EDGE_FLAG_DATA,reinterpret_cast<void(*)()>(edge));
    gluTessCallback(tess,GLU_TESS_VERTEX_DATA,reinterpret_cast<void(*)()>(vertex));
    gluTessCallback(tess,GLU_TESS_ERROR_DATA,reinterpret_cast<void(*)()>(error));
    gluTessCallback(tess,GLU_TESS_COMBINE_DATA,reinterpret_cast<void(*)()>(combine));
    gluTessProperty(tess,GLU_TESS_WINDING_RULE,GLU_TESS_WINDING_NONZERO);gluTessNormal(tess,0,0,1);
    try{gluTessBeginPolygon(tess,&context);for(const auto& contour:contours){gluTessBeginContour(tess);for(const auto& point:contour){context.points.push_back({point[0],point[1],0});auto* p=context.points.back().data();gluTessVertex(tess,p,p);}gluTessEndContour(tess);}gluTessEndPolygon(tess);}
    catch(...){gluDeleteTess(tess);throw;}gluDeleteTess(tess);
    if(context.exception)std::rethrow_exception(context.exception);if(context.error || context.triangles.size()%3)throw std::invalid_argument("字形輪廓無法細分成三角形");
    return context.triangles;
}
#pragma clang diagnostic pop
NativeGdiGlyph native_gdi_glyph(const NativeFontFace& face,std::uint16_t glyph,const std::array<double,4>& matrix,unsigned raster){
    const auto font=physical_font(face);if(glyph>=CTFontGetGlyphCount(font.get()))throw std::invalid_argument("字形索引不合法");
    for(auto value:matrix)if(!std::isfinite(value))throw std::invalid_argument("字形矩陣不合法");
    const auto transform=CGAffineTransformMake(matrix[0],matrix[1],matrix[2],matrix[3],0,0);NativeGdiGlyph result;CGSize advance{};
    CTFontGetAdvancesForGlyphs(font.get(),kCTFontOrientationHorizontal,&glyph,&advance,1);const auto vector=CGSizeApplyAffineTransform(advance,transform);result.advance={vector.width,vector.height};
    auto path=simulated_font_path(font.get(),glyph,face.simulations,&transform);if(!path || CGPathIsEmpty(path.get()))return result;
    const auto box=CGPathGetPathBoundingBox(path.get());const std::array<double,4> edges{std::floor(CGRectGetMinX(box)),std::floor(CGRectGetMinY(box)),std::ceil(CGRectGetMaxX(box)),std::ceil(CGRectGetMaxY(box))};
    for(unsigned i=0;i<4;++i){if(!std::isfinite(edges[i]) || std::abs(edges[i])>=32768)throw std::invalid_argument("字形範圍超出上限");result.bounds[i]=static_cast<std::int32_t>(edges[i]);}
    struct Collector{NativeGdiGlyph* out;std::exception_ptr error;};Collector collector{&result,{}};
    CGPathApply(path.get(),&collector,[](void* opaque,const CGPathElement* e){auto& c=*static_cast<Collector*>(opaque);if(c.error)return;try{
        if(e->type==kCGPathElementMoveToPoint){if(c.out->contours.size()>=65536)throw std::length_error("字形 contour 過多");c.out->contours.push_back({{e->points[0].x,e->points[0].y},{}});return;}
        if(e->type==kCGPathElementCloseSubpath)return;if(c.out->contours.empty())throw std::invalid_argument("字形 contour 沒有起點");
        NativeOutlineSegment segment;segment.kind=e->type==kCGPathElementAddLineToPoint?1:e->type==kCGPathElementAddQuadCurveToPoint?2:3;
        for(unsigned i=0;i<segment.kind;++i)segment.points[i]={e->points[i].x,e->points[i].y};auto& parts=c.out->contours.back().segments;if(parts.size()>=65536)throw std::length_error("字形曲線過多");parts.push_back(segment);
    }catch(...){c.error=std::current_exception();}});if(collector.error)std::rethrow_exception(collector.error);
    if(raster){const auto width=static_cast<std::size_t>(result.bounds[2]-result.bounds[0]),height=static_cast<std::size_t>(result.bounds[3]-result.bounds[1]);if(!width || !height)return result;
        if(width*height>64*1024*1024)throw std::bad_alloc();result.alpha.resize(width*height);Cf<CGColorSpaceRef> colors(CGColorSpaceCreateDeviceGray());
        Cf<CGContextRef> context(CGBitmapContextCreate(result.alpha.data(),width,height,8,width,colors.get(),kCGImageAlphaNone));if(!context)throw std::bad_alloc();
        CGContextSetGrayFillColor(context.get(),1,1);CGContextSetShouldAntialias(context.get(),raster!=1);CGContextTranslateCTM(context.get(),-result.bounds[0],-result.bounds[1]);CGContextAddPath(context.get(),path.get());CGContextFillPath(context.get());
    }return result;
}
std::vector<NativeGlyphMetrics> native_font_glyph_metrics(const NativeFontFace& face,std::span<const std::uint16_t> glyphs){
    const auto font=physical_font(face);const auto count=glyphs.size();
    const auto limit=CTFontGetGlyphCount(font.get());
    if(std::any_of(glyphs.begin(),glyphs.end(),[&](auto glyph){return glyph>=limit;}))throw std::invalid_argument("字形索引超出字型範圍");
    std::vector<NativeGlyphMetrics> result(count);if(!count)return result;
    std::vector<CGRect> boxes(count);std::vector<CGSize> horizontal(count),vertical(count),origins(count);
    CTFontGetBoundingRectsForGlyphs(font.get(),kCTFontOrientationHorizontal,glyphs.data(),boxes.data(),static_cast<CFIndex>(count));
    CTFontGetAdvancesForGlyphs(font.get(),kCTFontOrientationHorizontal,glyphs.data(),horizontal.data(),static_cast<CFIndex>(count));
    CTFontGetAdvancesForGlyphs(font.get(),kCTFontOrientationVertical,glyphs.data(),vertical.data(),static_cast<CFIndex>(count));
    CTFontGetVerticalTranslationsForGlyphs(font.get(),glyphs.data(),origins.data(),static_cast<CFIndex>(count));
    for(std::size_t i=0;i<count;++i){
        auto box=CGRectIsNull(boxes[i])?CGRectZero:boxes[i];
        if(face.simulations){const auto outline=simulated_font_path(font.get(),glyphs[i],face.simulations,nullptr);if(outline && !CGPathIsEmpty(outline.get()))box=CGPathGetPathBoundingBox(outline.get());}
        const auto left=metric(CGRectGetMinX(box)),right=metric(CGRectGetMaxX(box));
        const auto bottom=metric(CGRectGetMinY(box)),top=metric(CGRectGetMaxY(box));
        const auto width=std::max(0,metric(horizontal[i].width)),height=std::max(0,metric(std::hypot(vertical[i].width,vertical[i].height)));
        // CoreText 的 translation 是繪製 outline 所需的位移，與字形的 vertical origin 方向相反。
        const auto origin=metric(-origins[i].height);
        result[i]={left,width,width-right,origin-top,height,height-origin+bottom,origin};
    }
    return result;
}
NativeGlyphRaster native_font_glyph_raster(const NativeFontFace& face,const NativeGlyphRun& run){
    NativeGlyphRaster result;result.channels=run.aliased?1U:3U;
    const auto metrics=native_font_glyph_metrics(face,run.indices);if(run.indices.empty())return result;
    const auto font=physical_font(face);Cf<CGMutablePathRef> path(CGPathCreateMutable());if(!path)throw std::bad_alloc{};
    const double scale=static_cast<double>(run.em)/face.em,ppd=run.pixels_per_dip;double cursor=0;
    const auto& m=run.transform;const CGAffineTransform device=CGAffineTransformMake(m[0],m[1],m[2],m[3],m[4],m[5]);
    for(std::size_t i=0;i<run.indices.size();++i){
        double advance=run.advances.empty()?metrics[i][run.sideways?4:1]*scale:run.advances[i];
        if(run.advances.empty() && run.measuring==1)advance=std::floor(advance*ppd+0.5)/ppd;
        const auto offset=run.offsets.empty()?std::array<float,2>{}:run.offsets[i];
        if(run.rtl)cursor-=advance;
        const double x=run.baseline_x+cursor+(run.rtl?-offset[0]:offset[0]),y=run.baseline_y-offset[1];
        CGAffineTransform glyph;
        if(run.sideways){
            // 先以 vertical origin 為原點，再向左旋轉 90 度；DWrite 座標 Y 向下。
            const double origin_x=metrics[i][1]*0.5*scale,origin_y=metrics[i][6]*scale;
            glyph=CGAffineTransformMake(0,-scale*ppd,-scale*ppd,0,(x+origin_y)*ppd,(y+origin_x)*ppd);
        }else glyph=CGAffineTransformMake(scale*ppd,0,0,-scale*ppd,x*ppd,y*ppd);
        const auto transform=CGAffineTransformConcat(glyph,device);
        auto outline=simulated_font_path(font.get(),run.indices[i],face.simulations,&transform);
        if(outline)CGPathAddPath(path.get(),nullptr,outline.get());
        if(!run.rtl)cursor+=advance;
    }
    if(CGPathIsEmpty(path.get()))return result;
    const auto box=CGPathGetPathBoundingBox(path.get());
    const std::array<double,4> edges{std::floor(CGRectGetMinX(box))-1,std::floor(CGRectGetMinY(box))-1,std::ceil(CGRectGetMaxX(box))+1,std::ceil(CGRectGetMaxY(box))+1};
    for(unsigned i=0;i<4;++i){if(!std::isfinite(edges[i]) || edges[i]<INT32_MIN || edges[i]>INT32_MAX)throw std::invalid_argument("字形範圍溢位");result.bounds[i]=static_cast<std::int32_t>(edges[i]);}
    const auto width=static_cast<std::int64_t>(result.bounds[2])-result.bounds[0],height=static_cast<std::int64_t>(result.bounds[3])-result.bounds[1];
    if(width<=0 || height<=0)return {};
    if(width>32768 || height>32768 || width*height*result.channels>64*1024*1024)throw std::bad_alloc{};
    const auto stride=static_cast<std::size_t>(width)*result.channels;
    result.alpha.resize(stride*static_cast<std::size_t>(height));
    Cf<CGColorSpaceRef> colors(CGColorSpaceCreateDeviceGray());
    Cf<CGContextRef> context(CGBitmapContextCreate(result.alpha.data(),stride,static_cast<std::size_t>(height),8,stride,colors.get(),kCGImageAlphaNone));if(!context)throw std::bad_alloc{};
    CGContextSetGrayFillColor(context.get(),1,1);CGContextSetShouldAntialias(context.get(),!run.aliased);
    CGContextScaleCTM(context.get(),result.channels,1);CGContextTranslateCTM(context.get(),-static_cast<double>(result.bounds[0]),-static_cast<double>(result.bounds[1]));
    CGContextAddPath(context.get(),path.get());CGContextFillPath(context.get());
    for(std::size_t y=0;y<static_cast<std::size_t>(height)/2;++y)for(std::size_t x=0;x<stride;++x)
        std::swap(result.alpha[y*stride+x],result.alpha[(static_cast<std::size_t>(height)-1-y)*stride+x]);
    // 依真正有 coverage 的像素縮小範圍，空白字形回傳空矩形。
    std::int32_t left=static_cast<std::int32_t>(width),top=static_cast<std::int32_t>(height),right=0,bottom=0;
    for(std::int32_t y=0;y<height;++y)for(std::int32_t x=0;x<width;++x){bool ink=false;for(unsigned c=0;c<result.channels;++c)ink|=result.alpha[static_cast<std::size_t>(y)*stride+static_cast<std::size_t>(x)*result.channels+c]!=0;if(ink){left=std::min(left,x);right=std::max(right,x+1);top=std::min(top,y);bottom=std::max(bottom,y+1);}}
    if(right<=left || bottom<=top){result.bounds={};result.alpha.clear();return result;}
    const auto row=static_cast<std::size_t>(right-left)*result.channels;std::vector<std::uint8_t> cropped(row*static_cast<std::size_t>(bottom-top));
    for(auto y=top;y<bottom;++y)std::copy_n(result.alpha.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y)*stride+static_cast<std::size_t>(left)*result.channels),row,cropped.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y-top)*row));
    result.bounds={result.bounds[0]+left,result.bounds[1]+top,result.bounds[0]+right,result.bounds[1]+bottom};result.alpha=std::move(cropped);return result;
}
std::vector<std::uint16_t> native_font_glyph_indices(const NativeFontFace& face,std::span<const std::uint32_t> codepoints){
    const auto font=physical_font(face);std::vector<std::uint16_t> result;result.reserve(codepoints.size());
    for(const auto code:codepoints){
        UniChar chars[2]{};CGGlyph glyphs[2]{};CFIndex count=1;
        if(code>0x10ffff || (code>=0xd800 && code<=0xdfff)){result.push_back(0);continue;}
        if(code>0xffff){const auto value=code-0x10000;chars[0]=static_cast<UniChar>(0xd800+(value>>10));chars[1]=static_cast<UniChar>(0xdc00+(value&0x3ff));count=2;}else chars[0]=static_cast<UniChar>(code);
        (void)CTFontGetGlyphsForCharacters(font.get(),chars,glyphs,count);result.push_back(glyphs[0]);
    }
    return result;
}
namespace {
// 一次 CoreText 列舉的原始結果：descriptor 依其 family 屬性過濾，cascade face 依描述出的 family 過濾。
struct FontCandidate {std::shared_ptr<const __CFString> key;NativeFontFace face;};
struct FontInventory {std::vector<FontCandidate> descriptors,cascade;};
std::shared_ptr<const __CFString> retained_string(std::u16string_view value){
    return {CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(value.data()),static_cast<CFIndex>(value.size())),CfRelease{}};
}
// 已安裝字型的 descriptor 部分：建立 CoreText 集合（約佔列舉一半時間）並描述每個 face。
std::vector<FontCandidate> enumerate_descriptor_candidates(){
    const void* keys[]{kCTFontCollectionRemoveDuplicatesOption};const void* values[]{kCFBooleanTrue};
    Cf<CFDictionaryRef> options(CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
    Cf<CTFontCollectionRef> collection(CTFontCollectionCreateFromAvailableFonts(options.get()));
    Cf<CFArrayRef> descriptors(collection?CTFontCollectionCreateMatchingFontDescriptors(collection.get()):nullptr);
    if(!descriptors)throw std::runtime_error("CoreText 無法列舉已安裝字型");
    std::vector<FontCandidate> result;
    // 每個 descriptor 各自建立 CTFont 並讀取 OS/2、hhea 等表格（約 0.15 ms／face），是列舉的主要成本。
    // CoreText 的函式與字型物件可跨執行緒同時使用，因此以 GCD 並行描述；結果依索引寫回，
    // 再依原 descriptor 順序合併，與逐一列舉的內容和順序完全相同。例外在合併前依序重新拋出。
    const auto array=descriptors.get();const auto count=static_cast<std::size_t>(CFArrayGetCount(array));
    std::vector<std::optional<FontCandidate>> described(count);std::vector<std::exception_ptr> failures(count);
    auto* described_items=described.data();auto* failure_items=failures.data();
    dispatch_apply(count,DISPATCH_APPLY_AUTO,^(std::size_t i){
        @autoreleasepool {
            try {
                auto descriptor=static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(array,static_cast<CFIndex>(i)));
                std::shared_ptr<const __CFString> family_name(static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(descriptor,kCTFontFamilyNameAttribute)),CfRelease{});
                if(!family_name || CFStringGetLength(family_name.get())==0)return;
                Cf<CTFontRef> initial(CTFontCreateWithFontDescriptor(descriptor,16,nullptr));if(!initial)return;
                if(auto face=describe(initial.get()))described_items[i]=FontCandidate{std::move(family_name),std::move(*face)};
            }catch(...){failure_items[i]=std::current_exception();}
        }
    });
    for(std::size_t i=0;i<count;++i){
        if(failures[i])std::rethrow_exception(failures[i]);
        if(described[i])result.push_back(std::move(*described[i]));
    }
    return result;
}
FontInventory enumerate_font_inventory(){
    // descriptor 部分只用 CoreText，在背景執行緒進行；同時在呼叫端執行緒處理需要 AppKit 的 UI cascade，
    // 兩者互不相依，合併後的內容與順序與依序執行相同。
    auto pending=std::async(std::launch::async,[]{@autoreleasepool {return enumerate_descriptor_candidates();}});
    FontInventory result;
    // UI 字型及其跨文字系統 cascade 可使用公開 descriptors 未列出的 family。
    // 建立集合時一起納入，讓 GDI／DirectWrite fallback 往返同一個穩定的集合。
    const auto append=[&](CTFontRef font){
        if(!font)return;auto face=describe(font);if(!face)return;
        auto name=retained_string(face->family);if(name)result.cascade.push_back({std::move(name),std::move(*face)});
    };
    for(unsigned variant=0;variant<4;++variant){GuiFont model;model.pitch_family=variant==3?1:0;model.weight=variant==1?700:400;model.italic=variant==2;
        const auto font=(__bridge CTFontRef)native_gui_font(model);append(font);
        Cf<CFArrayRef> cascade(CTFontCopyDefaultCascadeListForLanguages(font,nullptr));
        for(CFIndex i=0;cascade && i<CFArrayGetCount(cascade.get());++i){
            const auto descriptor=static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(cascade.get(),i));
            Cf<CTFontRef> fallback(CTFontCreateWithFontDescriptor(descriptor,16,nullptr));if(!fallback)continue;
            // 邏輯 cascade 別名可能有虛擬字形編號；以實際 CGFont 建立可匯出的 SFNT face。
            Cf<CGFontRef> graphics(CTFontCopyGraphicsFont(fallback.get(),nullptr));if(!graphics)continue;
            Cf<CTFontRef> physical(CTFontCreateWithGraphicsFont(graphics.get(),16,nullptr,nullptr));append(physical.get());
        }
    }
    result.descriptors=pending.get();
    return result;
}
}
std::vector<NativeFontFace> native_font_faces(std::u16string_view family){
    // 完整列舉需描述每個已安裝字型（數十 ms）；短時間內重複列舉共用同一份快照。
    // 安裝新字型最多延後此期限可見；AddFontResource／記憶體字型由呼叫端另行合併，不經此快取。
    static std::mutex mutex;static std::shared_ptr<const FontInventory> cached;static std::chrono::steady_clock::time_point captured;
    constexpr auto lifetime=std::chrono::seconds(10);
    std::shared_ptr<const FontInventory> inventory;
    {
        std::lock_guard lock(mutex);const auto now=std::chrono::steady_clock::now();
        if(!cached || now-captured>=lifetime){cached=std::make_shared<FontInventory>(enumerate_font_inventory());captured=now;}
        inventory=cached;
    }
    const auto requested=retained_string(family);
    const auto matches=[&](const FontCandidate& item){return family.empty() || CFStringCompare(item.key.get(),requested.get(),kCFCompareCaseInsensitive)==kCFCompareEqualTo;};
    std::vector<NativeFontFace> result;
    for(const auto& item:inventory->descriptors)if(matches(item))result.push_back(item.face);
    for(const auto& item:inventory->cascade){
        if(!matches(item))continue;const auto& face=item.face;
        if(std::none_of(result.begin(),result.end(),[&](const auto& value){return value.postscript_name==face.postscript_name && value.weight==face.weight && value.italic==face.italic;}))result.push_back(face);
    }
    return result;
}
}
