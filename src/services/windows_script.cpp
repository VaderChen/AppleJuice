#include "x64arm/windows_services.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/windows_transform.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include "unicode_text_analysis.hpp"
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
        {"usp10.dll","ScriptStringGetLogicalWidths",2},{"usp10.dll","ScriptStringCPtoX",4},{"usp10.dll","ScriptStringXtoCP",4},{"usp10.dll","ScriptStringValidate",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_script(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    constexpr unsigned invalid=0x80070057U,unsupported=0x80004001U,missing=0x80040200U,out_of_memory=0x8007000eU;
    try{
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
        if(name=="ScriptItemize"){
            const auto count=integer(1),capacity=integer(2);
            if(!arg(0) || !arg(5) || !arg(6) || count<1 || count>65535 || capacity<2 || capacity>65536)return finish(invalid);
            const auto control=arg(3)?static_cast<unsigned>(memory.load(GuestAddress{arg(3)},32)):0;
            const auto initial=arg(4)?static_cast<unsigned>(memory.load(GuestAddress{arg(4)},16)):0;
            // 先提供標準 Unicode 分段；舊式數字替換與邊界覆寫不能默默忽略。
            if((control&~0x0100ffffU) || (initial&~31U))return finish(unsupported);
            std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+i*2},16));
            // 與 DirectWrite TextAnalyzer 共用 ICU 書寫系統、群集方向及中性符號分段。
            const auto analysis=text_analysis::analyze(text,initial,arg(3) || arg(4));const auto& items=analysis.items;
            if(std::any_of(analysis.levels.begin(),analysis.levels.end(),[](unsigned level){return level>31;}))return finish(unsupported);
            if(items.size()>=static_cast<unsigned>(capacity))return finish(out_of_memory);
            memory.check(GuestAddress{arg(5)},(items.size()+1)*8,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);
            for(std::size_t i=0;i<items.size();++i){const auto& item=items[i];memory.store(GuestAddress{arg(5)+i*8},32,item.start);memory.store(GuestAddress{arg(5)+i*8+4},16,item.script|((item.level&1)?0xc00:0));memory.store(GuestAddress{arg(5)+i*8+6},16,item.level);}
            memory.store(GuestAddress{arg(5)+items.size()*8},64,static_cast<unsigned>(count));memory.store(GuestAddress{arg(6)},32,items.size());return finish(0);
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
            const auto cache=memory.load(GuestAddress{arg(1)},bits);auto found=gui_.script_caches.find(cache);GuiFont font;
            if(cache){if(found==gui_.script_caches.end())return finish(invalid);font=found->second;}
            else{
                if(!arg(0))return finish(0x8000000aU);if(!gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)))return finish(invalid);
                font=gui_font(gui_,gui_.contexts[arg(0)].font);const auto physical=native_resolve_font(font);if(!physical)return finish(missing);font.native=physical->native;
            }
            std::u16string text(static_cast<std::size_t>(count),u'\0');for(unsigned i=0;i<static_cast<unsigned>(count);++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(2)+i*2},16));
            const auto shaped=native_script_shape(font,text,(analysis&0x400)!=0,(analysis&0x4000)!=0,(analysis&1023)==0,(analysis&0x8000)!=0);
            if(shaped.missing)return finish(missing);if(shaped.glyphs.size()>static_cast<unsigned>(capacity))return finish(out_of_memory);
            memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(6)},shaped.glyphs.size()*2,Access::Write);memory.check(GuestAddress{arg(7)},shaped.clusters.size()*2,Access::Write);memory.check(GuestAddress{arg(8)},shaped.attributes.size()*2,Access::Write);memory.check(GuestAddress{arg(9)},4,Access::Write);
            if(!cache){if(gui_.script_caches.size()>=4096)return finish(out_of_memory);const auto handle=allocate_heap(memory,8);if(!handle)return finish(out_of_memory);try{gui_.script_caches.emplace(handle,std::move(font));}catch(...){free_heap(memory,handle,state);throw;}memory.store(GuestAddress{arg(1)},bits,handle);}
            for(std::size_t i=0;i<shaped.glyphs.size();++i){memory.store(GuestAddress{arg(6)+i*2},16,shaped.glyphs[i]);memory.store(GuestAddress{arg(8)+i*2},16,shaped.attributes[i]);}
            for(std::size_t i=0;i<shaped.clusters.size();++i)memory.store(GuestAddress{arg(7)+i*2},16,shaped.clusters[i]);memory.store(GuestAddress{arg(9)},32,shaped.glyphs.size());return finish(0);
        }
        if(name=="ScriptPlace"){
            // 依 SCRIPT_CACHE 或 HDC 目前字型取得各 glyph 的實際前進寬度；GOFFSET 為 0，ABC 為總寬。
            const auto count=integer(3);
            if(!arg(1) || !arg(2) || !arg(4) || !arg(5) || !arg(6) || !arg(7) || count<1 || count>65535)return finish(invalid);
            const auto cache=memory.load(GuestAddress{arg(1)},bits);auto found=gui_.script_caches.find(cache);GuiFont font;
            if(cache){if(found==gui_.script_caches.end())return finish(invalid);font=found->second;}
            else{
                if(!arg(0))return finish(0x8000000aU);if(!gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)))return finish(invalid);
                font=gui_font(gui_,gui_.contexts[arg(0)].font);const auto physical=native_resolve_font(font);if(!physical)return finish(missing);font.native=physical->native;
            }
            std::u16string glyphs(static_cast<std::size_t>(count),u'\0');std::vector<std::uint16_t> attributes(static_cast<std::size_t>(count));
            for(unsigned i=0;i<static_cast<unsigned>(count);++i){glyphs[i]=static_cast<char16_t>(memory.load(GuestAddress{arg(2)+i*2},16));attributes[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(4)+i*2},16));}
            std::vector<std::int32_t> advances;
            try{advances=native_gdi_glyph_advances(font,glyphs);}catch(const std::invalid_argument&){return finish(invalid);}
            if(advances.size()!=glyphs.size())return finish(invalid);
            memory.check(GuestAddress{arg(6)},glyphs.size()*4,Access::Write);memory.check(GuestAddress{arg(7)},glyphs.size()*8,Access::Write);if(arg(8))memory.check(GuestAddress{arg(8)},12,Access::Write);
            memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);
            if(!cache){if(gui_.script_caches.size()>=4096)return finish(out_of_memory);const auto handle=allocate_heap(memory,8);if(!handle)return finish(out_of_memory);try{gui_.script_caches.emplace(handle,std::move(font));}catch(...){free_heap(memory,handle,state);throw;}memory.store(GuestAddress{arg(1)},bits,handle);}
            std::int64_t total=0;
            for(std::size_t i=0;i<glyphs.size();++i){
                const auto advance=(attributes[i]&0x40U)?0:advances[i]; // fZWSP 的 glyph 不佔寬度。
                memory.store(GuestAddress{arg(6)+i*4},32,static_cast<std::uint32_t>(advance));memory.store(GuestAddress{arg(7)+i*8},64,0);total+=advance;
            }
            if(arg(8)){memory.store(GuestAddress{arg(8)},32,0);memory.store(GuestAddress{arg(8)+4},32,static_cast<std::uint32_t>(std::clamp<std::int64_t>(total,0,UINT32_MAX)));memory.store(GuestAddress{arg(8)+8},32,0);}
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
        if(name=="ScriptString_pSize")return finish(arg(0));if(name=="ScriptString_pcOutChars")return finish(arg(0)+8);if(name=="ScriptString_pLogAttr")return finish(arg(0)+16);if(name=="ScriptStringValidate")return finish(0);
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
