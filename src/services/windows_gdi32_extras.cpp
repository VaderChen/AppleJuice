#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "x64arm/unicode.hpp"
#include <bit>
#include <cmath>

// gdi32 補充家族：PolyTextOut、PolyDraw、LineDDA、GetTransform、GetCharWidthFloat、CreateBitmapIndirect、GdiGetCodePage、
// GetGlyphIndicesA（ACP 轉 UTF-16 後交給 W 版本）、GdiGetCharDimensions（對話方塊基準單位使用的平均字寬）、
// 色彩空間物件（LOGCOLORSPACE 狀態，DC 預設 sRGB）、MirrorRgn、FixBrushOrgEx 與 EnumObjects。
// 全部以既有 GDI 操作（ExtTextOut、MoveToEx／LineTo／PolyBezierTo、座標轉換查詢、字元寬度、CreateBitmap）在宿主端組合；
// 只有 LineDDA 需要逐點回呼客體。
namespace x64arm {
std::span<const WindowsServiceDescriptor> gdi32_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","PolyTextOutA",3,0,0,0x4},{"gdi32.dll","PolyTextOutW",3,0,0,0x4},{"gdi32.dll","PolyDraw",4,0,0,0x8},
        {"gdi32.dll","LineDDA",6,0,0,0xf},{"gdi32.dll","GetTransform",3,0,0,0x2},
        {"gdi32.dll","GetCharWidthFloatA",4,0,0,0x6},{"gdi32.dll","GetCharWidthFloatW",4,0,0,0x6},
        {"gdi32.dll","CreateBitmapIndirect",1},{"gdi32.dll","GdiGetCodePage",1},
        {"gdi32.dll","GetGlyphIndicesA",5,0,0,0x14},{"gdi32.dll","GdiGetCharDimensions",3},
        {"gdi32.dll","CreateColorSpaceA",1},{"gdi32.dll","CreateColorSpaceW",1},{"gdi32.dll","DeleteColorSpace",1},
        {"gdi32.dll","GetColorSpace",1},{"gdi32.dll","SetColorSpace",2},{"gdi32.dll","GetLogColorSpaceA",3,0,0,0x4},{"gdi32.dll","GetLogColorSpaceW",3,0,0,0x4},
        {"gdi32.dll","MirrorRgn",2},{"gdi32.dll","FixBrushOrgEx",4,0,0,0x6},{"gdi32.dll","EnumObjects",4,0,0,0x2},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="EnumObjects"){
        // (hdc, OBJ_PEN 1／OBJ_BRUSH 2, proc(lpLogObject, lParam), lParam)：列舉 16 色 VGA 調色盤的實心筆，與 16 色實心筆刷加上 6 種黑色陰影筆刷；
        // 回呼傳回 0 即停止，函式回傳最後一次回呼的值。
        return nested_continuation(state,memory,"EnumObjects",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto type=static_cast<std::uint32_t>(call(1));
            if(!gui_.contexts.contains(call(0)) || (type!=1 && type!=2)){last_error_=87;call.return_to(s,0);co_return std::optional<std::uint32_t>{};}
            static constexpr std::array<std::uint32_t,16> colors{0x000000,0x000080,0x008000,0x008080,0x800000,0x800080,0x808000,0xc0c0c0,
                                                                0x808080,0x0000ff,0x00ff00,0x00ffff,0xff0000,0xff00ff,0xffff00,0xffffff};
            const unsigned unit=x86_?4U:8U;const auto record=allocate_heap(m,16);if(!record){last_error_=8;call.return_to(s,0);co_return std::optional<std::uint32_t>{};}
            std::uint64_t last=0;std::vector<std::array<std::uint64_t,3>> objects;
            if(type==1)for(const auto color:colors)objects.push_back({0,1,color});
            else {for(const auto color:colors)objects.push_back({0,color,0});for(unsigned hatch=0;hatch<6;++hatch)objects.push_back({2,0,hatch});}
            for(const auto& object:objects){
                m.write(GuestAddress{record},std::vector<std::uint8_t>(16));
                if(type==1){m.store(GuestAddress{record},32,object[0]);m.store(GuestAddress{record+4},32,object[1]);m.store(GuestAddress{record+12},32,object[2]);}
                else {m.store(GuestAddress{record},32,object[0]);m.store(GuestAddress{record+4},32,object[1]);m.store(GuestAddress{record+8},static_cast<std::uint8_t>(unit*8),object[2]);}
                last=static_cast<std::uint32_t>(co_await service_callback::call(call(2),{record,call(3)}));
                if(!last)break;
            }
            (void)free_heap(m,record,s);call.return_to(s,last);co_return std::optional<std::uint32_t>{};
        });
    }
    if(name=="LineDDA"){
        // (XStart, YStart, XEnd, YEnd, LineFunc(x, y, data), Data)：依 DDA 逐一回呼線段上的點，不含終點。
        return nested_continuation(state,memory,"LineDDA",[service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto value=[&](unsigned index){return static_cast<std::int32_t>(static_cast<std::uint32_t>(call(index)));};
            const auto x1=value(0),y1=value(1),x2=value(2),y2=value(3);const auto callback=call(4),data=call(5);
            const std::int64_t dx=std::abs(std::int64_t{x2}-x1),dy=std::abs(std::int64_t{y2}-y1),steps=std::max(dx,dy);
            const int sx=x2>=x1?1:-1,sy=y2>=y1?1:-1;std::int64_t error=steps/2;std::int64_t x=x1,y=y1;
            for(std::int64_t i=0;i<steps;++i){
                (void)co_await service_callback::call(callback,{static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)),static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)),data});
                if(dx>=dy){x+=sx;error-=dy;if(error<0){y+=sy;error+=dx;}}
                else {y+=sy;error-=dx;if(error<0){x+=sx;error+=dy;}}
            }
            call.return_to(s,1);co_return std::optional<std::uint32_t>{};
        });
    }
    const WindowsCall arg(state,memory,service);
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return done(0);};
    const auto gdi=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"gdi32.dll",symbol,args);};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto load_float=[&](std::uint64_t address){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{address},32)));};
    const auto store_float=[&](std::uint64_t address,float value){memory.store(GuestAddress{address},32,std::bit_cast<std::uint32_t>(value));};
    GuestScratch temporary(*this,memory,state);
    try{
        if(name.find("ColorSpace")!=name.npos){
            // LOGCOLORSPACE｛簽章 'PSOC', 版本 0x400, 大小, 類型, 意圖, CIEXYZTRIPLE, 三個 gamma, 檔名[260]｝；A 版檔名為 ACP 位元組。
            // 物件保存 W 版面；DC 預設選入程序共用的 sRGB 色彩空間（LCS_sRGB、LCS_GM_IMAGES）。
            constexpr unsigned header=68,wide_size=header+520,narrow_size=header+260;
            const auto default_space=[&]{
                if(!gui_.default_color_space){std::vector<std::uint8_t> bytes(wide_size);
                    const std::array<std::uint32_t,5> fields{0x50534f43U,0x400U,wide_size,0x73524742U,4U};
                    for(unsigned i=0;i<5;++i)for(unsigned b=0;b<4;++b)bytes[i*4+b]=static_cast<std::uint8_t>(fields[i]>>(8*b));
                    gui_.default_color_space=gui_.next_object++;gui_.color_spaces.emplace(gui_.default_color_space,std::move(bytes));}
                return gui_.default_color_space;
            };
            if(name.starts_with("CreateColorSpace")){
                const bool wide=name.ends_with('W');const auto source=arg(0);if(!source)return fail(87);
                if(memory.load(GuestAddress{source},32)!=0x50534f43U || memory.load(GuestAddress{source+4},32)!=0x400U || memory.load(GuestAddress{source+8},32)!=(wide?wide_size:narrow_size))return fail(87);
                std::vector<std::uint8_t> bytes(wide_size);memory.read(GuestAddress{source},std::span(bytes).first(header));
                const auto file=wide?read_utf16(memory,GuestAddress{source+header},259):utf8_to_utf16_lossy(read_ansi(memory,source+header,259));
                for(std::size_t i=0;i<file.size() && i<259;++i){bytes[header+i*2]=static_cast<std::uint8_t>(file[i]);bytes[header+i*2+1]=static_cast<std::uint8_t>(file[i]>>8);}
                for(unsigned b=0;b<4;++b)bytes[8+b]=static_cast<std::uint8_t>(wide_size>>(8*b));
                const auto handle=gui_.next_object++;gui_.color_spaces.emplace(handle,std::move(bytes));return done(handle);
            }
            if(name=="DeleteColorSpace"){
                const auto found=gui_.color_spaces.find(arg(0));if(found==gui_.color_spaces.end())return fail(6);
                if(arg(0)!=gui_.default_color_space){gui_.color_spaces.erase(found);for(auto& [handle,dc]:gui_.contexts){(void)handle;if(dc.color_space==arg(0))dc.color_space=0;}}
                return done(1);
            }
            if(name.starts_with("GetLogColorSpace")){
                const bool wide=name.ends_with('W');const auto found=gui_.color_spaces.find(arg(0));if(found==gui_.color_spaces.end())return fail(6);
                const auto size=wide?wide_size:narrow_size;if(!arg(1))return fail(87);if(u32(2)<size)return fail(122);
                auto bytes=found->second;
                if(!wide){std::u16string file;for(unsigned i=0;i<259;++i){const auto c=static_cast<char16_t>(bytes[header+i*2]|(bytes[header+i*2+1]<<8));if(!c)break;file+=c;}
                    const auto text=utf16_to_utf8(file);bytes.resize(narrow_size);std::fill(bytes.begin()+header,bytes.end(),0);std::copy_n(text.begin(),std::min<std::size_t>(text.size(),259),bytes.begin()+header);}
                for(unsigned b=0;b<4;++b)bytes[8+b]=static_cast<std::uint8_t>(size>>(8*b));
                memory.write(GuestAddress{arg(1)},bytes);return done(1);
            }
            const auto context=gui_.contexts.find(arg(0));if(context==gui_.contexts.end())return fail(6);
            const auto previous=context->second.color_space?context->second.color_space:default_space();
            if(name=="GetColorSpace")return done(previous);
            if(!gui_.color_spaces.contains(arg(1)))return fail(6);
            context->second.color_space=arg(1);return done(previous);
        }
        if(name=="MirrorRgn"){
            // 以視窗寬度為軸左右翻轉區域（x → 寬度 − x），結果重新正規化成水平帶。
            const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())return fail(1400);
            const auto region=gui_.regions.find(arg(1));if(region==gui_.regions.end())return fail(6);
            const auto width=static_cast<std::int32_t>(window->second.width);GuiRegion mirrored;
            for(const auto& r:region->second)mirrored.push_back({width-r[2],r[1],width-r[0],r[3]});
            if(!gui_region_coordinates(mirrored))return fail(87);
            region->second=gui_region_combine(mirrored,{},2);return done(1);
        }
        if(name=="FixBrushOrgEx")return done(0); // Win32 未實作，只為 Win32s 相容保留：不做任何事並回傳 FALSE。
        if(name=="PolyTextOutA" || name=="PolyTextOutW"){
            // POLYTEXT {int x, y; UINT n; LPCTSTR lpstr; UINT uiFlags; RECT rcl; int* pdx}：逐項交給 ExtTextOut，矩形只在 ETO_OPAQUE／ETO_CLIPPED 時使用。
            const auto stride=x86_?40U:56U,text_at=x86_?12U:16U,flags_at=x86_?16U:24U,rect_at=x86_?20U:28U,dx_at=x86_?36U:48U;
            const auto target=name.ends_with('W')?"ExtTextOutW":"ExtTextOutA";
            for(std::uint32_t i=0;i<u32(2);++i){
                const auto item=arg(1)+std::uint64_t{i}*stride;const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{item+flags_at},32));
                if(!gdi(target,{arg(0),memory.load(GuestAddress{item},32),memory.load(GuestAddress{item+4},32),flags,(flags&6U)?item+rect_at:0,
                                memory.load(GuestAddress{item+text_at},bits),memory.load(GuestAddress{item+8},32),memory.load(GuestAddress{item+dx_at},bits)}))return done(0);
            }
            return done(1);
        }
        if(name=="PolyDraw"){
            // (hdc, POINT*, BYTE* types, count)：PT_MOVETO 6、PT_LINETO 2、PT_BEZIERTO 4（三點一組），PT_CLOSEFIGURE（1）連回圖形起點。
            const auto points=arg(1),types=arg(2);const auto count=u32(3);
            std::vector<std::uint8_t> kinds(count);if(count)memory.read(GuestAddress{types},kinds);
            for(std::uint32_t i=0;i<count;){
                const auto kind=kinds[i]&~1U;
                if(kind==6 || kind==2){if(kind==6 && (kinds[i]&1U))return fail(87);++i;continue;}
                if(kind==4){if(i+3>count || (kinds[i+1]&~1U)!=4 || (kinds[i+2]&~1U)!=4)return fail(87);i+=3;continue;}
                return fail(87);
            }
            const auto current=temporary.allocate(16);if(!current)return fail(8);
            (void)gdi("GetCurrentPositionEx",{arg(0),current});
            std::int64_t start_x=static_cast<std::int32_t>(memory.load(GuestAddress{current},32)),start_y=static_cast<std::int32_t>(memory.load(GuestAddress{current+4},32));
            const auto point=[&](std::uint32_t index,unsigned axis){return memory.load(GuestAddress{points+index*8+axis*4},32);};
            const auto close=[&](){(void)gdi("LineTo",{arg(0),static_cast<std::uint32_t>(start_x),static_cast<std::uint32_t>(start_y)});};
            for(std::uint32_t i=0;i<count;){
                const auto kind=kinds[i]&~1U;
                if(kind==6){(void)gdi("MoveToEx",{arg(0),point(i,0),point(i,1),0});start_x=static_cast<std::int32_t>(point(i,0));start_y=static_cast<std::int32_t>(point(i,1));++i;}
                else if(kind==2){(void)gdi("LineTo",{arg(0),point(i,0),point(i,1)});if(kinds[i]&1U)close();++i;}
                else {(void)gdi("PolyBezierTo",{arg(0),points+i*8,3});if(kinds[i+2]&1U)close();i+=3;}
            }
            return done(1);
        }
        if(name=="GetGlyphIndicesA"){
            // ACP 字串轉成 UTF-16 後由 GetGlyphIndicesW 查字型；回傳的索引數為轉換後的字元數。
            const auto count=static_cast<std::int32_t>(u32(2));if(count<0 || (count && !arg(1)))return done(UINT32_MAX);
            const auto text=utf8_to_utf16_lossy(read_ansi(memory,arg(1),static_cast<std::size_t>(count)).substr(0,static_cast<std::size_t>(count)));
            const auto wide=temporary.text(text);if(!wide)return fail(8);
            return done(gdi("GetGlyphIndicesW",{arg(0),wide,text.size(),arg(3),u32(4)}));
        }
        if(name=="GdiGetCharDimensions"){
            // 平均字寬＝(52 個英文字母的寬度/26＋1)/2；可同時回傳 TEXTMETRICW 與字高。
            const auto metrics=arg(1)?arg(1):temporary.allocate(64);const auto size=temporary.allocate(8);
            const auto alphabet=temporary.text(u"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");if(!metrics || !size || !alphabet)return fail(8);
            if(!gdi("GetTextMetricsW",{arg(0),metrics}) || !gdi("GetTextExtentPointW",{arg(0),alphabet,52,size}))return done(0);
            if(arg(2))memory.store(GuestAddress{arg(2)},32,memory.load(GuestAddress{metrics},32));
            const auto width=static_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{size},32)));
            return done(static_cast<std::uint32_t>((width/26+1)/2));
        }
        if(name=="GetTransform"){
            // 0x203 世界→頁面（世界轉換）、0x304 頁面→裝置（映射模式）、0x204 世界→裝置（兩者合成）。XFORM 為 6 個 FLOAT。
            const auto kind=u32(1);if(!arg(2))return fail(87);
            if(kind!=0x203 && kind!=0x304 && kind!=0x204)return fail(87);
            const auto block=temporary.allocate(64);if(!block)return fail(8);
            std::array<float,6> world{1,0,0,1,0,0};
            if(kind!=0x304){if(!gdi("GetWorldTransform",{arg(0),block}))return done(0);for(unsigned i=0;i<6;++i)world[i]=load_float(block+i*4);}
            if(kind==0x203){for(unsigned i=0;i<6;++i)store_float(arg(2)+i*4,world[i]);return done(1);}
            if(!gdi("GetViewportExtEx",{arg(0),block}) || !gdi("GetWindowExtEx",{arg(0),block+8}) || !gdi("GetViewportOrgEx",{arg(0),block+16}) || !gdi("GetWindowOrgEx",{arg(0),block+24}))return done(0);
            const auto integer=[&](unsigned offset){return static_cast<double>(static_cast<std::int32_t>(memory.load(GuestAddress{block+offset},32)));};
            const double sx=integer(8)!=0.0?integer(0)/integer(8):1.0,sy=integer(12)!=0.0?integer(4)/integer(12):1.0;
            const double ox=integer(16)-integer(24)*sx,oy=integer(20)-integer(28)*sy;
            std::array<double,6> page{sx,0,0,sy,ox,oy};
            if(kind==0x204){
                // 先世界轉換再頁面轉換：[M11 M12; M21 M22] 依列向量相乘，位移一併套用頁面縮放與原點。
                page={world[0]*sx,world[1]*sy,world[2]*sx,world[3]*sy,world[4]*sx+ox,world[5]*sy+oy};
            }
            for(unsigned i=0;i<6;++i)store_float(arg(2)+i*4,static_cast<float>(page[i]));return done(1);
        }
        if(name=="GetCharWidthFloatA" || name=="GetCharWidthFloatW"){
            // 寬度＝ABC 三段之和；與 GetCharABCWidthsFloat 共用字型度量。
            const auto first=u32(1),last=u32(2);if(last<first || !arg(3))return fail(87);
            const auto count=static_cast<std::size_t>(last-first)+1;if(count>65536)return fail(87);
            const auto block=temporary.allocate(count*12);if(!block)return fail(8);
            if(!gdi(name.ends_with('W')?"GetCharABCWidthsFloatW":"GetCharABCWidthsFloatA",{arg(0),first,last,block}))return done(0);
            for(std::size_t i=0;i<count;++i)store_float(arg(3)+i*4,load_float(block+i*12)+load_float(block+i*12+4)+load_float(block+i*12+8));
            return done(1);
        }
        if(name=="CreateBitmapIndirect"){
            // BITMAP {LONG bmType, bmWidth, bmHeight, bmWidthBytes; WORD bmPlanes, bmBitsPixel; LPVOID bmBits}：與 CreateBitmap 相同的點陣圖物件。
            if(!arg(0))return fail(87);
            const auto bm=arg(0);
            return done(gdi("CreateBitmap",{memory.load(GuestAddress{bm+4},32),memory.load(GuestAddress{bm+8},32),memory.load(GuestAddress{bm+16},16),
                                            memory.load(GuestAddress{bm+18},16),memory.load(GuestAddress{bm+(x86_?20U:24U)},bits)}));
        }
        if(name=="GdiGetCodePage"){
            // 目前字型字元集對應的 ANSI 碼頁（TranslateCharsetInfo 的 ciACP）。
            const auto charset=static_cast<std::uint32_t>(gdi("GetTextCharsetInfo",{arg(0),0,0}));
            const auto info=temporary.allocate(32);if(!info)return done(1252);
            if(!gdi("TranslateCharsetInfo",{charset,info,1}))return done(1252);
            return done(memory.load(GuestAddress{info+4},32));
        }
    }catch(const MemoryFault&){return fail(998);}
    throw HostCallFault("未知的 gdi32 補充服務");
}
}
