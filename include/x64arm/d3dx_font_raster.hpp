#pragma once
#include "x64arm/windows_gui.hpp"
#include "x64arm/windows_graphics.hpp"
#include "x64arm/memory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace x64arm {
// 快取的是完整排版結果及像素；每次仍建立原有 D3DX 貼圖／sprite 身分。
// 保留 memory DC 的最後繪製像素，避免 GetDC／GetPixel 觀察到舊內容。
class D3dxFontDrawCache {
public:
    struct Entry {
        std::string key;
        unsigned width{},height{};
        std::uint64_t result{},stamp{};
        std::vector<std::uint32_t> pixels;
        std::vector<std::uint8_t> rgba;
        std::size_t bytes()const{return key.size()+pixels.size()*4+rgba.size();}
    };
private:
    std::vector<Entry> entries_;
    std::size_t bytes_{};
    std::uint64_t stamp_{},hits_{},misses_{};
public:
    ~D3dxFontDrawCache(){if((hits_ || misses_) && std::getenv("X64ARM_D3DX_FONT_DRAW_CACHE"))std::fprintf(stderr,"[d3dx-font-draw-cache] hits=%llu misses=%llu entries=%zu bytes=%zu\n",static_cast<unsigned long long>(hits_),static_cast<unsigned long long>(misses_),entries_.size(),bytes_);}
    const Entry* find(const std::optional<std::string>& key){
        if(!key)return nullptr;
        for(auto& entry:entries_)if(entry.key==*key){entry.stamp=++stamp_;++hits_;return &entry;}
        ++misses_;return nullptr;
    }
    void remember(const std::optional<std::string>& key,unsigned width,unsigned height,std::uint64_t result,
                  const std::vector<std::uint32_t>& pixels,const std::vector<std::uint8_t>& rgba){
        constexpr std::size_t budget=16*1024*1024;
        if(!key || !result || key->size()+pixels.size()*4+rgba.size()>budget)return;
        try{
            Entry entry{*key,width,height,result,++stamp_,pixels,rgba};
            if(entries_.capacity()<32)entries_.reserve(32);
            while(!entries_.empty() && (entries_.size()>=32 || bytes_+entry.bytes()>budget)){
                const auto oldest=std::min_element(entries_.begin(),entries_.end(),[](const auto& a,const auto& b){return a.stamp<b.stamp;});
                bytes_-=oldest->bytes();entries_.erase(oldest);
            }
            const auto size=entry.bytes();entries_.push_back(std::move(entry));bytes_+=size;
        }catch(const std::bad_alloc&){/* 快取配置失敗不改變已完成的文字 API 結果。 */}
    }
    void clear(){entries_.clear();bytes_=0;}
};
inline std::optional<std::string> d3dx_font_draw_key(GuestMemory& memory,const GuiDc& dc,std::uint64_t font,
    std::span<const std::uint8_t> logical,std::uintptr_t native_font,std::uint32_t flags,
    const std::array<unsigned,2>& canvas,std::span<const std::uint8_t> rect,bool wide,
    std::uint64_t text,std::int32_t count,bool allowed){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3DX_FONT_DRAW_CACHE");return !value || std::string_view(value)=="1";}();
    // 路徑、metafile、修改輸入字串及有副作用的 DC 保留既有流程。
    if(!enabled || !allowed || dc.display || dc.font!=font || dc.path.state || dc.map_mode!=1 ||
       dc.graphics_mode!=1 || dc.rop2!=13 || dc.application_clip || dc.meta_clip ||
       dc.world!=gui_identity || dc.window_origin!=std::array<std::int32_t,2>{} ||
       dc.viewport_origin!=std::array<std::int32_t,2>{} || dc.window_extent!=std::array<std::int32_t,2>{1,1} ||
       dc.viewport_extent!=std::array<std::int32_t,2>{1,1} || (dc.alignment&~0x100U) ||
       (flags&~0x3fffffU) || (flags&(0x400U|0x1000U|0x10000U)) || !text || count<-1 || !count || count>4096)return {};
    try{
        std::string key;
        const auto append=[&](const auto& value){key.append(reinterpret_cast<const char*>(&value),sizeof(value));};
        append(flags);append(canvas);append(native_font);append(wide);append(dc.alignment);append(dc.clipped);append(dc.clip);
        const auto excluded=dc.excluded.size();append(excluded);for(const auto& region:dc.excluded)append(region);
        const auto logical_size=logical.size(),rect_size=rect.size();append(logical_size);append(rect_size);
        key.append(reinterpret_cast<const char*>(logical.data()),logical.size());key.append(reinterpret_cast<const char*>(rect.data()),rect.size());
        bool ended=count>=0;
        for(unsigned i=0;i<(count<0?4096U:static_cast<unsigned>(count));++i){
            const auto value=memory.load(GuestAddress{text+std::uint64_t{i}*(wide?2:1)},wide?16:8);
            if(count<0 && !value){ended=true;break;}
            key.push_back(static_cast<char>(value));if(wide)key.push_back(static_cast<char>(value>>8));
        }
        if(!ended)return {};
        return key;
    }catch(const std::bad_alloc&){return {};}
}
// 保留原有紋理大小、四邊形、UV 及透明 texel 的 RGB；僅在小畫布產生字形。
// 清除和擴展由 GPU 完成，不讀回或在 CPU 配置完整尺寸的 RGBA 中間結果。
inline void d3dx_font_upload_padded(NativeGraphics& native,const GraphicsView& target,unsigned width,unsigned height,
                                    std::span<const std::uint8_t> pixels){
    GraphicsResourceDesc desc;desc.dimension=3;desc.width=width;desc.height=height;desc.format=target.format;desc.bind=8;
    auto source=native.graphics_resource(desc);GraphicsView view{source,desc.format};
    native.graphics_write(view,pixels,width*4,width*height*4);
    native.graphics_clear(target,{1,1,1,0},0);
    native.graphics_copy(target,view,{0,0,0},{0,0,0,width,height,1});
}
// 共用 GDI 排版量測；左上對齊、未變換的 NOCLIP 文字不需要配置整個視窗大小的透明畫布。
// 其餘排版／映射契約仍使用原有畫布。MetalRaster 的字形遮罩只延伸量測寬高外 4 像素，
// 這裡保留 8 像素及字型 external leading，涵蓋進位、底線與每列字形遮罩。
template<class Measure>
std::array<unsigned,2> d3dx_font_raster_extent(const GuiDc& dc,std::uint64_t font,std::uint32_t flags,
                                             const GuiFontMetrics& metrics,std::array<unsigned,2> extent,Measure&& measure){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3DX_FONT_TIGHT_RASTER");return !value || std::string_view(value)!="0";}();
    if(!enabled || !(flags&0x100) || (flags&(0xf|0x80|0x1000|0x10000|0x20000)) || dc.font!=font || dc.path.state ||
       dc.map_mode!=1 || dc.world!=gui_identity || dc.window_origin!=std::array<std::int32_t,2>{} ||
       dc.viewport_origin!=std::array<std::int32_t,2>{} || dc.window_extent!=std::array<std::int32_t,2>{1,1} ||
       dc.viewport_extent!=std::array<std::int32_t,2>{1,1})return extent;
    const auto measured=measure();if(measured[0]<0 || measured[1]<=0)return extent;
    const std::array<std::int64_t,2> padding{8,8+std::max<std::int64_t>(0,metrics.external_leading)};
    for(unsigned i=0;i<2;++i)extent[i]=static_cast<unsigned>(std::min<std::int64_t>(extent[i],std::max<std::int64_t>(1,measured[i]+padding[i])));
    return extent;
}
} // namespace x64arm
