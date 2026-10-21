#pragma once
#include "x64arm/windows_gui.hpp"
namespace x64arm {
struct RasterBlit {
    std::uint32_t operation{0xcc},transparent{UINT32_MAX};
    GuiDraw brush;
};
// 持續像素表面；所有繪圖依呼叫順序生效，讀回時同步 GPU。
class RasterSurface {
public:
    virtual ~RasterSurface() = default;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;
    virtual void draw(const GuiDraw&,RasterSurface* pattern=nullptr) = 0;
    // 同步既有表面的 CPU 像素；保留 allocation，等待先前 GPU 使用完成後才寫入。
    virtual void upload(std::span<const std::uint32_t>,bool preserve_alpha=false) = 0;
    // 共用映射由呼叫端保管；返回前完成 GPU 讀取，不保留映射指標。
    virtual void import_frame(void*,std::size_t,std::uint32_t,std::uint32_t,std::shared_ptr<void> = {}){throw std::runtime_error("後端缺少共用影格匯入");}
    virtual void set_dib_alpha(bool) = 0;
    virtual void set_transparent(bool) {}
    virtual void blit(RasterSurface&,const std::array<std::int32_t,4>& destination,
                      const std::array<std::int32_t,4>& source,const GuiDc&,std::uint32_t blend,
                      const RasterBlit& options={},RasterSurface* pattern=nullptr) = 0;
    virtual GuiBitmap snapshot() = 0;
    virtual std::uint32_t pixel(std::uint32_t,std::uint32_t) = 0;
    virtual void flush(bool wait = false) = 0;
    virtual std::string renderer() const = 0;
};
std::shared_ptr<RasterSurface> make_hardware_raster(std::uint32_t width,std::uint32_t height,
    std::span<const std::uint32_t> pixels = {},void* view = nullptr);
}
