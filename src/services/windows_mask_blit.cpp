// MaskBlt／PlgBlt（gdi32）：沿用既有 BitBlt 的三元運算、筆刷與裁切，以及世界座標轉換。
// MaskBlt 在兩個與目的地相容的暫存點陣分別套用前景／背景 ROP，再以單色遮罩（1 取前景、0 取背景）合成後寫回；
// PlgBlt 以世界座標轉換把來源矩形映到平行四邊形，帶遮罩時只寫入遮罩為 1 的像素。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_transform.hpp"
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> mask_blit_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"gdi32.dll","MaskBlt",12},{"gdi32.dll","PlgBlt",10}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_mask_blit(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto gdi=[&](std::string_view name,std::initializer_list<std::uint64_t> arguments){return nested_service(state,memory,"gdi32.dll",name,arguments);};
    const auto signed_arg=[&](unsigned index){return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(arg(index))));};
    const auto valid_dc=[&](std::uint64_t h){return gui_.contexts.contains(h) || gui_.dc_objects.contains(h) || (h>=0x10000000 && gui_.windows.contains(h-0x10000000));};
    const auto monochrome=[&](std::uint64_t h){const auto found=gui_.bitmaps.find(h);return found!=gui_.bitmaps.end() && found->second.bits_per_pixel==1;};
    constexpr std::uint64_t srccopy=0x00cc0020,srcand=0x008800c6,srcpaint=0x00ee0086,dsna=0x00220326;
    // 暫存 DC／點陣與遮罩 DC 在任何結果下都釋放。
    struct Temporary {WindowsServices& self;GuestState& state;GuestMemory& memory;std::vector<std::pair<std::uint64_t,std::uint64_t>> dcs;std::vector<std::uint64_t> objects;
        ~Temporary(){try{for(const auto& [dc,previous]:dcs){if(previous)(void)self.nested_service(state,memory,"gdi32.dll","SelectObject",{dc,previous});(void)self.nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});}
            for(const auto object:objects)(void)self.nested_service(state,memory,"gdi32.dll","DeleteObject",{object});}catch(...){}}};
    Temporary temporary{*this,state,memory,{},{}};
    // 選入點陣的記憶體 DC；compatible 為 0 時建立與目的地相容的 width×height 點陣。
    const auto surface=[&](std::uint64_t reference,std::uint64_t bitmap,std::uint64_t width,std::uint64_t height)->std::uint64_t{
        const auto dc=gdi("CreateCompatibleDC",{reference});if(!dc)return 0;temporary.dcs.emplace_back(dc,0);
        if(!bitmap){bitmap=gdi("CreateCompatibleBitmap",{reference,width,height});if(!bitmap)return 0;temporary.objects.push_back(bitmap);}
        temporary.dcs.back().second=gdi("SelectObject",{dc,bitmap});return dc;
    };
    // 單色遮罩依 BitBlt 規則轉色：1 為背景色（白）、0 為文字色（黑）。
    const auto mask_colors=[&](std::uint64_t dc){(void)gdi("SetTextColor",{dc,0});(void)gdi("SetBkColor",{dc,0xffffff});};
    try{
        if(service.symbol=="MaskBlt"){
            // {hdcDest, x, y, w, h, hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, MAKEROP4(fore, back)}：沒有遮罩時等同以前景 ROP 的 BitBlt。
            const auto dest=arg(0),source=arg(5),mask=arg(8);const auto rop=static_cast<std::uint32_t>(arg(11));
            const std::uint64_t fore=rop&0x00ffffffU,back=static_cast<std::uint64_t>((rop>>24)&0xffU)<<16;
            if(!valid_dc(dest))return fail(6);
            if(!mask)return finish(gdi("BitBlt",{dest,signed_arg(1),signed_arg(2),signed_arg(3),signed_arg(4),source,signed_arg(6),signed_arg(7),fore}));
            if(!monochrome(mask))return fail(87);if(gui_.emf_recorders.contains(dest))return fail(50);
            const auto width=static_cast<std::int32_t>(arg(3)),height=static_cast<std::int32_t>(arg(4));if(width<=0 || height<=0)return finish(1);
            const auto mask_dc=surface(0,mask,0,0),fore_dc=surface(dest,0,static_cast<std::uint64_t>(width),static_cast<std::uint64_t>(height)),back_dc=surface(dest,0,static_cast<std::uint64_t>(width),static_cast<std::uint64_t>(height));
            if(!mask_dc || !fore_dc || !back_dc)return fail(8);
            // 暫存點陣沿用目的地的筆刷、筆刷原點（扣除位移）與文字／背景色，使 pattern 與單色來源的轉色與直接繪製相同。
            const auto brush=gdi("GetCurrentObject",{dest,2});GuestScratch scratch(*this,memory,state);const auto origin=scratch.allocate(8);if(!origin)return fail(8);
            (void)gdi("GetBrushOrgEx",{dest,origin});const auto ox=static_cast<std::int32_t>(memory.load(GuestAddress{origin},32)),oy=static_cast<std::int32_t>(memory.load(GuestAddress{origin+4},32));
            const auto text=gdi("GetTextColor",{dest}),background=gdi("GetBkColor",{dest});
            for(const auto dc:{fore_dc,back_dc}){if(brush)(void)gdi("SelectObject",{dc,brush});
                (void)gdi("SetBrushOrgEx",{dc,static_cast<std::uint64_t>(static_cast<std::int64_t>(ox)-static_cast<std::int32_t>(arg(1))),static_cast<std::uint64_t>(static_cast<std::int64_t>(oy)-static_cast<std::int32_t>(arg(2))),0});
                (void)gdi("SetTextColor",{dc,text});(void)gdi("SetBkColor",{dc,background});}
            const auto w=static_cast<std::uint64_t>(width),h=static_cast<std::uint64_t>(height);
            for(const auto& [dc,operation]:{std::pair{fore_dc,fore},std::pair{back_dc,back}}){
                if(!gdi("BitBlt",{dc,0,0,w,h,dest,signed_arg(1),signed_arg(2),srccopy}) || !gdi("BitBlt",{dc,0,0,w,h,source,signed_arg(6),signed_arg(7),operation}))return finish(0);
                mask_colors(dc);
            }
            // 前景 AND 遮罩、背景 AND NOT 遮罩，OR 合成後寫回目的地。
            if(!gdi("BitBlt",{fore_dc,0,0,w,h,mask_dc,signed_arg(9),signed_arg(10),srcand}) || !gdi("BitBlt",{back_dc,0,0,w,h,mask_dc,signed_arg(9),signed_arg(10),dsna}) ||
               !gdi("BitBlt",{fore_dc,0,0,w,h,back_dc,0,0,srcpaint}))return finish(0);
            return finish(gdi("BitBlt",{dest,signed_arg(1),signed_arg(2),w,h,fore_dc,0,0,srccopy}));
        }
        // PlgBlt {hdcDest, POINT[3], hdcSrc, xSrc, ySrc, width, height, hbmMask, xMask, yMask}：第四頂點為 p1+p2-p0。
        const auto dest=arg(0),source=arg(2),mask=arg(7);if(!valid_dc(dest) || !valid_dc(source))return fail(6);if(!arg(1))return fail(87);
        const auto width=static_cast<std::int32_t>(arg(5)),height=static_cast<std::int32_t>(arg(6));if(width<=0 || height<=0)return fail(87);
        if(mask && !monochrome(mask))return fail(87);if(gui_.emf_recorders.contains(dest))return fail(50);
        // 來源 DC 不可帶旋轉或斜切，與 Windows 相同。
        if(const auto found=gui_.contexts.find(source);found!=gui_.contexts.end()){const auto m=gui_device_transform(found->second);if(m[1]!=0 || m[2]!=0)return fail(87);}
        std::array<double,6> points{};for(unsigned i=0;i<6;++i)points[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
        GuestScratch scratch(*this,memory,state);const auto xform=scratch.allocate(24),saved=scratch.allocate(24);if(!xform || !saved)return fail(8);
        const std::array<float,6> matrix{static_cast<float>((points[2]-points[0])/width),static_cast<float>((points[3]-points[1])/width),static_cast<float>((points[4]-points[0])/height),
            static_cast<float>((points[5]-points[1])/height),static_cast<float>(points[0]),static_cast<float>(points[1])};
        for(unsigned i=0;i<6;++i)memory.store(GuestAddress{xform+i*4},32,std::bit_cast<std::uint32_t>(matrix[i]));
        const auto w=static_cast<std::uint64_t>(width),h=static_cast<std::uint64_t>(height);
        // 帶遮罩時先在來源空間準備 S AND 遮罩，目的地先清掉遮罩區再 OR 入，兩步都走同一個轉換。
        std::uint64_t mask_dc=0,masked=0;
        if(mask){mask_dc=surface(0,mask,0,0);masked=surface(dest,0,w,h);if(!mask_dc || !masked)return fail(8);
            if(!gdi("BitBlt",{masked,0,0,w,h,source,signed_arg(3),signed_arg(4),srccopy}))return finish(0);mask_colors(masked);
            if(!gdi("BitBlt",{masked,0,0,w,h,mask_dc,signed_arg(8),signed_arg(9),srcand}))return finish(0);}
        const auto mode=gdi("GetGraphicsMode",{dest});if(!gdi("GetWorldTransform",{dest,saved}))return finish(0);
        if(!gdi("SetGraphicsMode",{dest,2}) || !gdi("ModifyWorldTransform",{dest,xform,2})){(void)gdi("SetGraphicsMode",{dest,mode});return finish(0);}
        std::uint64_t result=0;
        if(!mask)result=gdi("BitBlt",{dest,0,0,w,h,source,signed_arg(3),signed_arg(4),srccopy});
        else {const auto text=gdi("GetTextColor",{dest}),background=gdi("GetBkColor",{dest});mask_colors(dest);
            result=gdi("BitBlt",{dest,0,0,w,h,mask_dc,signed_arg(8),signed_arg(9),dsna}) && gdi("BitBlt",{dest,0,0,w,h,masked,0,0,srcpaint});
            (void)gdi("SetTextColor",{dest,text});(void)gdi("SetBkColor",{dest,background});}
        // 先還原原本的世界轉換，再回到原本的繪圖模式（GM_COMPATIBLE 需要單位矩陣）。
        (void)gdi("SetWorldTransform",{dest,saved});(void)gdi("SetGraphicsMode",{dest,mode});
        return finish(result!=0);
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
}
}
