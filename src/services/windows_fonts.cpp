#include "x64arm/windows_services.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <set>
#include <tuple>
#include <cmath>
#include <numeric>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>

namespace x64arm {
bool windows_ui_font_alias(std::u16string_view family){
    constexpr std::u16string_view aliases[]{u"",u"System",u"Segoe UI",u"MS Shell Dlg",u"MS Shell Dlg 2",u"MS Sans Serif",u"Microsoft Sans Serif",u"Tahoma"};
    const auto lower=[](char16_t value){return value>=u'A' && value<=u'Z'?static_cast<char16_t>(value+32):value;};
    return std::any_of(std::begin(aliases),std::end(aliases),[&](auto alias){return alias.size()==family.size() && std::equal(alias.begin(),alias.end(),family.begin(),[&](auto a,auto b){return lower(a)==lower(b);});});
}
namespace {
struct Charset {std::uint8_t value;unsigned bit;std::u16string_view name;};
constexpr Charset charsets[]{{0,0,u"Western"},{238,1,u"Central European"},{204,2,u"Cyrillic"},{161,3,u"Greek"},{162,4,u"Turkish"},{177,5,u"Hebrew"},{178,6,u"Arabic"},{186,7,u"Baltic"},{163,8,u"Vietnamese"},{222,16,u"Thai"},{128,17,u"Japanese"},{134,18,u"Chinese GB2312"},{129,19,u"Korean"},{136,20,u"Chinese Big5"},{130,21,u"Korean Johab"},{77,29,u"Macintosh"},{255,30,u"OEM"},{2,31,u"Symbol"}};
bool valid_charset(std::uint8_t value){return value==1 || std::any_of(std::begin(charsets),std::end(charsets),[&](const auto& entry){return entry.value==value;});}
template<class Integer> void number(std::vector<std::uint8_t>& bytes,std::size_t offset,Integer value,unsigned size=4){const auto raw=static_cast<std::uint64_t>(value);for(unsigned i=0;i<size;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(raw>>(i*8));}
void string(std::vector<std::uint8_t>& bytes,std::size_t offset,std::size_t capacity,std::u16string_view text,bool wide){
    if(wide){auto count=std::min(text.size(),capacity-1);if(count && text[count-1]>=0xd800 && text[count-1]<=0xdbff)--count;for(std::size_t i=0;i<count;++i)number(bytes,offset+i*2,text[i],2);}
    else {const auto value=utf16_to_utf8(text);auto count=std::min(value.size(),capacity-1);while(count<value.size() && count && (static_cast<unsigned char>(value[count])&0xc0)==0x80)--count;std::copy_n(value.begin(),count,bytes.begin()+static_cast<std::ptrdiff_t>(offset));}
}
FontEnumerationRecord record(const NativeFontFace& face,bool wide,std::uint8_t charset,std::u16string_view script){
    FontEnumerationRecord out;out.logical.resize(wide?348:188);out.metrics.resize(wide?100:96);out.type=face.type;
    const auto scaled=[&](std::int32_t value){return static_cast<std::int32_t>((static_cast<std::int64_t>(value)*16+face.em/2)/face.em);};
    const auto ascent=scaled(face.ascent),descent=scaled(face.descent),height=ascent+descent,average=scaled(face.average_width);
    number(out.logical,0,static_cast<std::uint32_t>(height));number(out.logical,4,static_cast<std::uint32_t>(average));number(out.logical,16,static_cast<std::uint32_t>(face.weight));
    out.logical[20]=face.italic;out.logical[23]=charset;out.logical[24]=3;out.logical[25]=2;out.logical[27]=static_cast<std::uint8_t>(face.family_class|(face.fixed_pitch?1:2));
    const auto log_size=wide?92U:60U,unit=wide?2U:1U;string(out.logical,28,32,face.family,wide);string(out.logical,log_size,64,face.full_name,wide);string(out.logical,log_size+64*unit,32,face.style,wide);string(out.logical,log_size+96*unit,32,script,wide);
    const std::array<std::int32_t,11> metrics{height,ascent,descent,std::max(0,height-16),scaled(face.line_gap),average,scaled(face.maximum_width),face.weight,0,96,96};
    for(unsigned i=0;i<metrics.size();++i)number(out.metrics,i*4,static_cast<std::uint32_t>(metrics[i]));
    const std::array characters{face.first_character,face.last_character,face.default_character,face.break_character};
    for(unsigned i=0;i<4;++i)number(out.metrics,44+i*unit,wide?characters[i]:std::min<std::uint16_t>(characters[i],255),unit);
    const auto bytes=44+4*unit;out.metrics[bytes]=face.italic;out.metrics[bytes+3]=static_cast<std::uint8_t>(face.family_class|6U|(face.fixed_pitch?0U:1U));out.metrics[bytes+4]=charset;
    const auto extended=wide?60U:56U;number(out.metrics,extended,face.flags);number(out.metrics,extended+4,static_cast<std::uint32_t>(face.em));number(out.metrics,extended+8,static_cast<std::uint32_t>(face.ascent+face.descent));number(out.metrics,extended+12,static_cast<std::uint32_t>(face.average_width));
    for(unsigned i=0;i<6;++i)number(out.metrics,extended+16+i*4,face.signature[i]);
    return out;
}
}
std::vector<FontEnumerationRecord> font_enumeration_records(std::vector<NativeFontFace> faces,bool wide,std::uint8_t charset,bool all_styles){
    std::sort(faces.begin(),faces.end(),[](const auto& a,const auto& b){return std::tuple{a.family,std::abs(a.weight-400)+(a.italic?1000:0),a.style,a.full_name}<std::tuple{b.family,std::abs(b.weight-400)+(b.italic?1000:0),b.style,b.full_name};});
    std::vector<FontEnumerationRecord> result;std::set<std::u16string> families;
    for(const auto& face:faces){
        if(face.em<=0 || face.family.empty() || face.family.size()>=32 || (!all_styles && families.contains(face.family)))continue;
        bool added=false;
        for(const auto& entry:charsets){if((charset==1 || charset==entry.value) && (face.signature[4]&(UINT32_C(1)<<entry.bit))){result.push_back(record(face,wide,entry.value,entry.name));added=true;if(!all_styles)break;}}
        if(!added && charset==1 && !face.signature[4]){result.push_back(record(face,wide,1,u""));added=true;}
        if(added)families.insert(face.family);
    }
    return result;
}
std::span<const WindowsServiceDescriptor> font_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","GetCharacterPlacementA",6,0,0,0x2c},{"gdi32.dll","GetCharacterPlacementW",6,0,0,0x2c},{"gdi32.dll","GetFontLanguageInfo",1},
        {"gdi32.dll","AddFontResourceA",1},{"gdi32.dll","AddFontResourceW",1},{"gdi32.dll","RemoveFontResourceA",1},{"gdi32.dll","RemoveFontResourceW",1},
        {"gdi32.dll","GetOutlineTextMetricsA",3,0,0,0x2},{"gdi32.dll","GetOutlineTextMetricsW",3,0,0,0x2},
        {"gdi32.dll","AddFontResourceExA",3,0,0,0x2},{"gdi32.dll","AddFontResourceExW",3,0,0,0x2},{"gdi32.dll","RemoveFontResourceExA",3,0,0,0x2},{"gdi32.dll","RemoveFontResourceExW",3,0,0,0x2},{"gdi32.dll","GetGlyphOutlineA",7,0,0,0x16},{"gdi32.dll","GetGlyphOutlineW",7,0,0,0x16},
        {"gdi32.dll","GetTextExtentPointI",4,0,0,0x4},{"gdi32.dll","GetTextExtentExPointI",7,0,0,0xc},
        {"gdi32.dll","GetCharABCWidthsA",4,0,0,0x6},{"gdi32.dll","GetCharABCWidthsW",4,0,0,0x6},{"gdi32.dll","GetCharABCWidthsI",5,0,0,0x6},
        {"gdi32.dll","GetCharWidthA",4,0,0,0x6},{"gdi32.dll","GetCharWidthW",4,0,0,0x6},{"gdi32.dll","GetCharWidthI",5,0,0,0x6},{"gdi32.dll","AddFontMemResourceEx",4,0,0,0x2},{"gdi32.dll","RemoveFontMemResourceEx",1},{"gdi32.dll","EnumFontFamiliesExA",5,0,0,0x10},{"gdi32.dll","EnumFontFamiliesExW",5,0,0,0x10},{"gdi32.dll","GetFontData",5,0,0,0x16},{"gdi32.dll","GetGlyphIndicesW",5,0,0,0x14},{"gdi32.dll","GetFontUnicodeRanges",2},{"gdi32.dll","GetTextFaceA",3,0,0,0x2},{"gdi32.dll","GetTextFaceW",3,0,0,0x2},{"gdi32.dll","EnumFontFamiliesA",4},{"gdi32.dll","EnumFontFamiliesW",4}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_fonts(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(service.symbol.starts_with("AddFontResource") || service.symbol.starts_with("RemoveFontResource")){
        const bool extended=service.symbol.find("ResourceEx")!=std::string_view::npos;
        const auto flags=extended?static_cast<std::uint32_t>(arg(1)):0U;
        if(!arg(0) || (flags&~0x30U) || (extended && arg(2))){last_error_=87;return finish(0);}
        const auto path=service.symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
        const auto file=filesystem_.open(path);if(file.error){last_error_=file.error;return finish(0);}
        const auto key=std::make_pair(file.key,static_cast<std::uint32_t>(flags));
        if(service.symbol.starts_with("Remove")){
            const auto found=gui_.file_fonts.find(key);if(found==gui_.file_fonts.end() || found->second.empty()){last_error_=2;return finish(0);}
            gui_.memory_fonts.erase(found->second.back());found->second.pop_back();if(found->second.empty())gui_.file_fonts.erase(found);return finish(1);
        }
        if(gui_.memory_fonts.size()>=256){last_error_=8;return finish(0);}auto faces=native_load_font_faces(*file.bytes);if(faces.empty()){last_error_=13;return finish(0);}
        const auto count=static_cast<std::uint32_t>(faces.size()),handle=static_cast<std::uint32_t>(gui_.next_object++);gui_.memory_fonts.emplace(handle,std::move(faces));gui_.file_fonts[key].push_back(handle);return finish(count);
    }
    if(service.symbol=="RemoveFontMemResourceEx")return finish(gui_.memory_fonts.erase(arg(0))?1U:0U);
    if(service.symbol=="AddFontMemResourceEx"){
        const auto size=static_cast<std::uint32_t>(arg(1));if(!arg(0) || !size || size>output_limit || arg(2) || !arg(3) || gui_.memory_fonts.size()>=256)return finish(0);
        memory.check(GuestAddress{arg(3)},4,Access::Write);std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(0)},bytes);auto faces=native_load_font_faces(bytes);if(faces.empty())return finish(0);
        const auto handle=gui_.next_object++;memory.store(GuestAddress{arg(3)},32,faces.size());gui_.memory_fonts.emplace(handle,std::move(faces));return finish(static_cast<std::uint32_t>(handle));
    }
    const bool extended=service.symbol.starts_with("EnumFontFamiliesEx"),wide=service.symbol.ends_with('W');
    const bool query=!service.symbol.starts_with("EnumFont");
    const auto failure=service.symbol=="GetFontLanguageInfo"?0x8000U:service.symbol=="GetFontData" || service.symbol=="GetGlyphIndicesW" || service.symbol.starts_with("GetGlyphOutline")?UINT32_MAX:0U;
    if(!gui_.dc_objects.contains(arg(0)) && !gui_.contexts.contains(arg(0))){last_error_=6;return finish(failure);}
    if(query){
        const auto fail=[&](unsigned error){last_error_=error;return finish(failure);};
        const auto dc=gui_.contexts.find(arg(0));const auto model=gui_font(gui_,dc==gui_.contexts.end()?0xb011:dc->second.font);
        const auto face=native_resolve_font(model);if(!face)return fail(50);
        const auto name=service.symbol;
#include "gdi_character_placement.inc"
        if(name.starts_with("GetOutlineTextMetrics")){
            if(dc==gui_.contexts.end() || dc->second.map_mode!=1 || dc->second.world!=gui_identity)return fail(50);
            const auto metrics=native_gui_?native_gui_->font_metrics(model).value_or(fallback_font_metrics(model)):fallback_font_metrics(model);
            const unsigned tm_size=wide?60:56,selection=(4+tm_size+11+3)&~3U,pointer_size=x86_?4:8;
            const auto pointers=(selection+124+pointer_size-1)&~(pointer_size-1),header=pointers+4*pointer_size;
            std::vector<std::uint8_t> bytes(header);
            auto basic=record(*face,wide,model.charset,u"").metrics;
            std::copy_n(basic.begin(),tm_size,bytes.begin()+4);
            for(const auto [offset,value]:std::array<std::pair<unsigned,std::int32_t>,9>{{{0,metrics.height},{4,metrics.ascent},{8,metrics.descent},{12,std::max(0,metrics.height-static_cast<int>(std::abs(model.height)))},{16,metrics.external_leading},{20,metrics.average_width},{24,metrics.maximum_width},{28,model.weight},{32,0}}})number(bytes,4+offset,static_cast<std::uint32_t>(value));
            bytes[4+44+(wide?8:4)]=model.italic;bytes[4+45+(wide?8:4)]=model.underline;bytes[4+46+(wide?8:4)]=model.strikeout;
            const auto os2=native_font_table(*face,0x4f532f32),head=native_font_table(*face,0x68656164),hhea=native_font_table(*face,0x68686561),post=native_font_table(*face,0x706f7374);
            const auto word=[&](const auto& table,unsigned at,bool signed_value=false)->int{if(at+2>table.size())return 0;const auto value=static_cast<std::uint16_t>((unsigned(table[at])<<8)|table[at+1]);return signed_value?static_cast<std::int16_t>(value):value;};
            const double em=model.height<0?-static_cast<double>(model.height):model.height?static_cast<double>(model.height)*face->em/std::max(1,face->ascent+face->descent):13;
            const auto scaled=[&](int value){return static_cast<std::int32_t>(std::lround(static_cast<double>(value)*em/face->em));};
            if(os2.size()>=42)std::copy_n(os2.begin()+32,10,bytes.begin()+5+tm_size);
            number(bytes,selection,word(os2,62));number(bytes,selection+4,word(os2,8));
            number(bytes,selection+8,word(hhea,18,true));number(bytes,selection+12,word(hhea,20,true));
            if(post.size()>=8){const auto angle=static_cast<std::int32_t>((std::uint32_t(post[4])<<24)|(std::uint32_t(post[5])<<16)|(std::uint32_t(post[6])<<8)|post[7]);number(bytes,selection+16,static_cast<std::uint32_t>(std::lround(angle*10.0/65536)));}
            number(bytes,selection+20,face->em);number(bytes,selection+24,scaled(face->ascent));number(bytes,selection+28,static_cast<std::uint32_t>(-scaled(face->descent)));number(bytes,selection+32,scaled(face->line_gap));
            number(bytes,selection+36,scaled(face->design_metrics[4]));number(bytes,selection+40,scaled(face->design_metrics[5]));
            for(unsigned i=0;i<4;++i)number(bytes,selection+44+i*4,scaled(word(head,36+i*2,true)));
            // RECT 採 left/top/right/bottom；SFNT 順序為 xMin/yMin/xMax/yMax。
            number(bytes,selection+48,scaled(word(head,42,true)));number(bytes,selection+56,scaled(word(head,38,true)));
            number(bytes,selection+60,scaled(word(hhea,4,true)));number(bytes,selection+64,scaled(word(hhea,6,true)));number(bytes,selection+68,scaled(word(hhea,8,true)));number(bytes,selection+72,word(head,46));
            for(unsigned i=0;i<8;++i)number(bytes,selection+76+i*4,scaled(word(os2,10+i*2,true)));
            number(bytes,selection+108,scaled(face->design_metrics[9]));number(bytes,selection+112,scaled(face->design_metrics[8]));number(bytes,selection+116,scaled(face->design_metrics[7]));number(bytes,selection+120,scaled(face->design_metrics[6]));
            const std::array<std::u16string,4> names{face->family,model.face.empty()?face->family:model.face,face->style,face->full_name};
            for(unsigned i=0;i<names.size();++i){number(bytes,pointers+i*pointer_size,static_cast<std::uint32_t>(bytes.size()),pointer_size);const auto value=wide?utf16_bytes(names[i]):[&](){auto text=utf16_to_utf8(names[i]);std::vector<std::uint8_t> data(text.begin(),text.end());data.push_back(0);return data;}();bytes.insert(bytes.end(),value.begin(),value.end());}
            number(bytes,0,static_cast<std::uint32_t>(bytes.size()));
            if(!arg(2))return finish(static_cast<std::uint32_t>(bytes.size()));
            const auto count=std::min<std::size_t>(static_cast<std::uint32_t>(arg(1)),bytes.size());if(!count)return fail(87);
            memory.write(GuestAddress{arg(2)},std::span(bytes).first(count));return finish(static_cast<std::uint32_t>(count));
        }
        if(name.starts_with("GetGlyphOutline") || name=="GetTextExtentPointI" || name=="GetTextExtentExPointI" || name.starts_with("GetCharABCWidths") || name=="GetCharWidthA" || name=="GetCharWidthW" || name=="GetCharWidthI"){
            try {
                if(dc==gui_.contexts.end() || dc->second.map_mode!=1 || dc->second.world!=gui_identity)return fail(50);
                if(const auto selected=gui_.fonts.find(dc->second.font);selected!=gui_.fonts.end()){
                    const auto& logfont=selected->second;if(logfont.size()>=16 && std::any_of(logfont.begin()+8,logfont.begin()+16,[](auto byte){return byte!=0;}))return fail(50);
                }
                const double em=model.height<0?-static_cast<double>(model.height):model.height?static_cast<double>(model.height)*face->em/std::max(1,face->ascent+face->descent):13;
                const double sy=em/face->em,sx=model.width?std::abs(static_cast<double>(model.width))/std::max(1,face->average_width):sy;
                if(em>32768 || std::abs(static_cast<double>(model.width))>32768)return fail(87);
                if(name.starts_with("GetGlyphOutline")){
                    const auto format=static_cast<unsigned>(arg(2)),mode=format&0x7fU;const bool index=(format&0x80)!=0;
                    if(format&~0x1ffU || mode>6 || ((format&0x100) && mode!=2 && mode!=3) || arg(1)>65535 || !arg(3) || !arg(6))return fail(87);
                    if(!wide && !index && arg(1)>127)return fail(50);
                    const auto glyph=index?static_cast<std::uint16_t>(arg(1)):native_font_glyph_indices(*face,std::array<std::uint32_t,1>{static_cast<unsigned>(arg(1))})[0];
                    std::array<double,4> matrix{};for(unsigned i=0;i<4;++i)matrix[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(6)+i*4},32))/65536.0*(i<2?sx:sy);
                    const auto native=native_gdi_glyph(*face,glyph,matrix,mode==1?1:mode>=4?2:0);const auto& box=native.bounds;
                    const auto width=static_cast<unsigned>(box[2]-box[0]),height=static_cast<unsigned>(box[3]-box[1]);
                    const auto dx=std::round(native.advance[0]),dy=-std::round(native.advance[1]);if(dx<INT16_MIN || dx>INT16_MAX || dy<INT16_MIN || dy>INT16_MAX)return fail(534);
                    std::vector<std::uint8_t> metrics(20);number(metrics,0,width);number(metrics,4,height);number(metrics,8,static_cast<unsigned>(box[0]));number(metrics,12,static_cast<unsigned>(box[3]));number(metrics,16,static_cast<std::uint16_t>(dx),2);number(metrics,18,static_cast<std::uint16_t>(dy),2);
                    std::vector<std::uint8_t> bytes;
                    if(mode==1 || mode>=4){const auto stride=mode==1?((width+31)/32)*4:(width+3)&~3U;bytes.resize(static_cast<std::size_t>(stride)*height);const unsigned levels=mode==4?4:mode==5?16:64;
                        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto alpha=native.alpha[static_cast<std::size_t>(y)*width+x];const auto at=static_cast<std::size_t>(y)*stride;
                            if(mode==1){if(alpha>=128)bytes[at+x/8]|=static_cast<std::uint8_t>(0x80U>>(x&7));}else bytes[at+x]=static_cast<std::uint8_t>((static_cast<unsigned>(alpha)*levels+127)/255);}
                    }else if(mode==2 || mode==3){
                        const auto pointfx=[&](std::size_t at,const std::array<double,2>& p){for(unsigned i=0;i<2;++i){const double value=std::round(p[i]*65536);if(value<INT32_MIN || value>INT32_MAX)throw std::invalid_argument("POINTFX 溢位");number(bytes,at+i*4,static_cast<unsigned>(static_cast<std::int32_t>(value)));}};
                        for(const auto& contour:native.contours){const auto begin=bytes.size();bytes.resize(begin+16);number(bytes,begin+4,24);pointfx(begin+8,contour.start);auto previous=contour.start;
                            for(auto part:contour.segments){if(mode==3 && part.kind==2){const auto control=part.points[0],end=part.points[1];for(unsigned i=0;i<2;++i){part.points[0][i]=previous[i]+(control[i]-previous[i])*2/3;part.points[1][i]=end[i]+(control[i]-end[i])*2/3;part.points[2][i]=end[i];}part.kind=3;}
                                const auto at=bytes.size();if(at+4+part.kind*8>64*1024*1024)throw std::length_error("字形輪廓超出上限");bytes.resize(at+4+part.kind*8);number(bytes,at,part.kind,2);number(bytes,at+2,part.kind,2);for(unsigned i=0;i<part.kind;++i)pointfx(at+4+i*8,part.points[i]);previous=part.points[part.kind-1];}
                            if(previous!=contour.start){const auto at=bytes.size();bytes.resize(at+12);number(bytes,at,1,2);number(bytes,at+2,1,2);pointfx(at+4,contour.start);}number(bytes,begin,static_cast<unsigned>(bytes.size()-begin));}
                    }
                    memory.check(GuestAddress{arg(3)},20,Access::Write);if(mode && arg(5) && arg(4)){if(arg(4)<bytes.size())return fail(122);memory.check(GuestAddress{arg(5)},bytes.size(),Access::Write);}
                    memory.write(GuestAddress{arg(3)},metrics);if(mode && arg(5) && arg(4))memory.write(GuestAddress{arg(5)},bytes);return finish(static_cast<unsigned>(bytes.size()));
                }
                const bool extent=name.starts_with("GetTextExtent"),extent_ex=name=="GetTextExtentExPointI",indexed=extent || name.ends_with('I'),abc=name.starts_with("GetCharABC");
                const auto first=static_cast<unsigned>(arg(1));const auto count=extent || indexed?static_cast<unsigned>(arg(2)):arg(2)>=arg(1) && arg(2)<=65535?static_cast<unsigned>(arg(2)-arg(1)+1):UINT32_MAX;
                if(count>65536 || (!extent && !indexed && !wide && arg(2)>127))return fail(87);
                std::vector<std::uint16_t> glyphs(count);
                if(indexed){const auto pointer=arg(extent?1:3);if(extent && count && !pointer)return fail(87);if(!pointer && (first>65535 || count>65536-first))return fail(87);for(unsigned i=0;i<count;++i)glyphs[i]=pointer?static_cast<std::uint16_t>(memory.load(GuestAddress{pointer+i*2},16)):static_cast<std::uint16_t>(first+i);}
                else {std::vector<std::uint32_t> characters(count);for(unsigned i=0;i<count;++i)characters[i]=first+i;glyphs=native_font_glyph_indices(*face,characters);}
                const auto metrics=native_font_glyph_metrics(*face,glyphs);const auto output=arg(extent?(extent_ex?6:3):(indexed?4:3));if(!output)return fail(87);
                std::vector<std::int32_t> advances(count);for(unsigned i=0;i<count;++i){const auto value=std::round(metrics[i][1]*sx);if(value>INT32_MAX)return fail(534);advances[i]=static_cast<std::int32_t>(value);}
                if(extent){if(extent_ex && static_cast<std::int32_t>(arg(3))<0)return fail(87);std::int64_t total=0;unsigned fit=0;std::vector<std::uint8_t> distances(count*4);
                    for(unsigned i=0;i<count;++i){total+=advances[i];if(total>INT32_MAX)return fail(534);number(distances,i*4,static_cast<unsigned>(total));if(total<=static_cast<std::int32_t>(arg(3)))fit=i+1;}
                    memory.check(GuestAddress{output},8,Access::Write);if(extent_ex && arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);if(extent_ex && arg(5))memory.check(GuestAddress{arg(5)},distances.size(),Access::Write);
                    memory.store(GuestAddress{output},32,static_cast<unsigned>(total));memory.store(GuestAddress{output+4},32,count?static_cast<unsigned>(std::round((face->ascent+face->descent)*sy)):0);
                    if(extent_ex && arg(4))memory.store(GuestAddress{arg(4)},32,fit);if(extent_ex && arg(5))memory.write(GuestAddress{arg(5)},distances);return finish(1);
                }
                std::vector<std::uint8_t> bytes(count*(abc?12:4));for(unsigned i=0;i<count;++i){if(!abc)number(bytes,i*4,static_cast<unsigned>(advances[i]));else{const auto a=static_cast<std::int32_t>(std::floor(metrics[i][0]*sx)),right=static_cast<std::int32_t>(std::ceil((metrics[i][1]-metrics[i][2])*sx));const auto b=std::max(0,right-a);number(bytes,i*12,static_cast<unsigned>(a));number(bytes,i*12+4,static_cast<unsigned>(b));number(bytes,i*12+8,static_cast<unsigned>(advances[i]-a-b));}}
                memory.write(GuestAddress{output},bytes);return finish(1);
            }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::domain_error&){return fail(50);}catch(const std::invalid_argument&){return fail(87);}
        }
        if(name=="GetFontData"){
            const auto tag=static_cast<std::uint32_t>(arg(1)),offset=static_cast<std::uint32_t>(arg(2)),capacity=static_cast<std::uint32_t>(arg(4));
            // Windows tag 為 little-endian DWORD，CoreText 使用 OpenType big-endian tag。
            const auto swapped=((tag&255U)<<24)|((tag&0xff00U)<<8)|((tag>>8)&0xff00U)|(tag>>24);
            const auto bytes=tag?native_font_table(*face,swapped):native_font_file(*face);
            if(bytes.empty() || offset>=bytes.size())return fail(87);
            const auto available=static_cast<std::uint32_t>(bytes.size()-offset);
            if(!arg(3) || !capacity)return finish(available);
            const auto count=std::min(capacity,available);memory.write(GuestAddress{arg(3)},std::span<const std::uint8_t>{bytes}.subspan(offset,count));return finish(count);
        }
        if(name.starts_with("GetTextFace")){
            const auto capacity=static_cast<std::int32_t>(arg(1));if(capacity<0)return fail(87);
            const auto text=face->family;
            if(wide){if(!arg(2))return finish(static_cast<std::uint32_t>(text.size()+1));if(!capacity)return fail(87);
                auto count=std::min(text.size(),static_cast<std::size_t>(capacity-1));if(count && text[count-1]>=0xd800 && text[count-1]<=0xdbff)--count;
                memory.write(GuestAddress{arg(2)},utf16_bytes(text.substr(0,count)));return finish(static_cast<std::uint32_t>(count+1));}
            const auto utf8=utf16_to_utf8(text);if(!arg(2))return finish(static_cast<std::uint32_t>(utf8.size()+1));if(!capacity)return fail(87);
            auto count=std::min(utf8.size(),static_cast<std::size_t>(capacity-1));while(count<utf8.size() && count && (static_cast<unsigned char>(utf8[count])&0xc0)==0x80)--count;
            std::vector<std::uint8_t> bytes(utf8.begin(),utf8.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);memory.write(GuestAddress{arg(2)},bytes);return finish(static_cast<std::uint32_t>(bytes.size()));
        }
        if(name=="GetGlyphIndicesW"){
            const auto count=static_cast<std::int32_t>(arg(2));if(count<0 || count>1048576 || (arg(4)&~UINT64_C(1)) || (count && (!arg(1) || !arg(3))))return fail(87);
            if(!count)return finish(0);memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(count)*2,Access::Write);
            std::vector<std::uint32_t> chars(static_cast<std::size_t>(count));for(std::size_t i=0;i<chars.size();++i)chars[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+i*2},16));
            auto glyphs=native_font_glyph_indices(*face,chars);const auto replacement=arg(4)?std::uint16_t{0xffff}:native_font_glyph_indices(*face,std::array<std::uint32_t,1>{face->default_character})[0];
            for(std::size_t i=0;i<glyphs.size();++i)memory.store(GuestAddress{arg(3)+i*2},16,glyphs[i]?glyphs[i]:replacement);return finish(static_cast<std::uint32_t>(count));
        }
        if(name=="GetFontUnicodeRanges"){
            std::vector<std::uint32_t> chars(65536);for(unsigned i=0;i<chars.size();++i)chars[i]=i;
            const auto glyphs=native_font_glyph_indices(*face,chars);std::vector<std::pair<std::uint16_t,std::uint16_t>> ranges;unsigned supported=0;
            for(unsigned at=0;at<glyphs.size();){if(!glyphs[at]){++at;continue;}const auto first=at;while(at<glyphs.size() && glyphs[at] && at-first<65535){++at;++supported;}ranges.emplace_back(static_cast<std::uint16_t>(first),static_cast<std::uint16_t>(at-first));}
            std::vector<std::uint8_t> bytes(16+ranges.size()*4);number(bytes,0,static_cast<std::uint32_t>(bytes.size()));number(bytes,8,supported);number(bytes,12,static_cast<std::uint32_t>(ranges.size()));
            for(std::size_t i=0;i<ranges.size();++i){number(bytes,16+i*4,ranges[i].first,2);number(bytes,18+i*4,ranges[i].second,2);}
            if(arg(1))memory.write(GuestAddress{arg(1)},bytes);return finish(static_cast<std::uint32_t>(bytes.size()));
        }
        return fail(50);
    }
    if(!arg(2) || (extended && (!arg(1) || static_cast<std::uint32_t>(arg(4))))){last_error_=87;return finish(0);}
    std::uint8_t charset=1;std::u16string family;
    if(extended){memory.check(GuestAddress{arg(1)},wide?92:60,Access::Read);charset=static_cast<std::uint8_t>(memory.load(GuestAddress{arg(1)+23},8));if(memory.load(GuestAddress{arg(1)+27},8) || !valid_charset(charset)){last_error_=87;return finish(0);}}
    if(arg(1)){const auto pointer=arg(1)+(extended?28U:0U);std::string narrow;bool terminated=false;
        for(unsigned i=0;i<32;++i){const auto value=memory.load(GuestAddress{pointer+i*(wide?2U:1U)},wide?16:8);if(!value){terminated=true;break;}if(wide)family.push_back(static_cast<char16_t>(value));else narrow.push_back(static_cast<char>(value));}
        if(!terminated){last_error_=87;return finish(0);}if(!wide)family=utf8_to_utf16_lossy(narrow);}
    if(!runtime_.network.external_io)throw HostCallFault("原生字型列舉不支援 deterministic compare");
    if(!handles(GuestAddress{arg(2)}))memory.check(GuestAddress{arg(2)},1,Access::Execute);
    FontEnumeration operation;operation.callback=arg(2);operation.parameter=arg(3);operation.thread=threads_.current;
    try{auto faces=native_font_faces(family);for(const auto& [key,handles]:gui_.file_fonts)if(!(key.second&0x20U) && !handles.empty())for(const auto& face:gui_.memory_fonts.at(handles.back()))if(family.empty() || family==face.family)faces.push_back(face);
        operation.records=font_enumeration_records(std::move(faces),wide,charset,!family.empty() || charset!=1);
        if(operation.records.empty())return finish(0);
        operation.logical=allocate_gui_scratch(memory,operation.scratch,{},348);operation.metrics=allocate_gui_scratch(memory,operation.scratch,{},100);
    }catch(const std::bad_alloc&){release_gui_scratch(memory,state,operation.scratch);last_error_=8;return finish(0);}catch(...){release_gui_scratch(memory,state,operation.scratch);throw;}
    const auto token=operation.logical;
    try{font_enumerations_.emplace(token,std::move(operation));}
    catch(const std::bad_alloc&){release_gui_scratch(memory,state,operation.scratch);last_error_=8;return finish(0);}
    try{return advance_font_enumeration(state,memory,token);}
    catch(const std::bad_alloc&){discard_font_enumeration(token,memory,state);last_error_=8;return finish(0);}
    catch(...){discard_font_enumeration(token,memory,state);throw;}
}
std::optional<std::uint32_t> WindowsServices::advance_font_enumeration(GuestState& state,GuestMemory& memory,std::uint64_t token){
    const auto& operation=font_enumerations_.at(token);const auto& item=operation.records.at(operation.index);
    memory.write(GuestAddress{operation.logical},item.logical);memory.write(GuestAddress{operation.metrics},item.metrics);
    start_callbacks(state,memory,{{operation.callback,{operation.logical,operation.metrics,item.type,operation.parameter}}},Continuation::FontEnumeration,token);return std::nullopt;
}
void WindowsServices::discard_font_enumeration(std::uint64_t token,GuestMemory& memory,const GuestState& state){
    const auto found=font_enumerations_.find(token);if(found==font_enumerations_.end())return;
    release_gui_scratch(memory,state,found->second.scratch);font_enumerations_.erase(found);
}
std::optional<std::uint32_t> WindowsServices::resume_font_enumeration(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result){
    auto& operation=font_enumerations_.at(frame.value);const auto value=static_cast<std::uint32_t>(result);
    if(!value || ++operation.index==operation.records.size()){discard_font_enumeration(frame.value,memory,state);complete_call(state,memory,value);return std::nullopt;}
    try{return advance_font_enumeration(state,memory,frame.value);}
    catch(const std::bad_alloc&){discard_font_enumeration(frame.value,memory,state);last_error_=8;complete_call(state,memory,0);return std::nullopt;}
    catch(...){discard_font_enumeration(frame.value,memory,state);throw;}
}
}
