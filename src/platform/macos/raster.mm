#include "metal_shared_frame.hpp"
#include "x64arm/resource_usage.hpp"
#include "metal_command_encoder.hpp"
#include "fixed_metal_library.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include "x64arm/gpu_memory.hpp"
#include "resource_usage.hpp"
#include "gui_timing.hpp"
#include <cmath>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include "x64arm/raster_surface.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/windows_transform.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <map>

namespace x64arm { NSFont* native_gui_font(const GuiFont&); }

// 由獨立的 layer-hosting view 承載 Metal，避免插在 AppKit 私有內容層下方。
// 不參與滑鼠命中測試；父視圖仍接收客體輸入，原生控制項維持在上方。
@interface X64ArmRasterView : NSView
@end
@implementation X64ArmRasterView
- (BOOL)isFlipped { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer { } // Metal command buffer 是唯一的呈現來源。
- (NSView*)hitTest:(NSPoint)point { (void)point;return nil; }
@end

@interface X64ArmGpuAllocation : NSObject
@property(nonatomic) std::shared_ptr<x64arm::GpuMemoryReservation> reservation;
@property(nonatomic,strong) id<MTLBuffer> buffer;
@end
@implementation X64ArmGpuAllocation
@end
namespace x64arm { namespace {
// Each command operates on a bounded rectangle. Command encoder order preserves
// GDI read-after-write ordering, including memory DC copies and readback.

struct alignas(16) Params {std::array<std::int32_t,4> rect{},source{},clip{};std::uint32_t kind{},color{},fill{UINT32_MAX},width{1};std::int32_t hatch{-1};std::uint32_t opaque{},background{},blend{UINT32_MAX},excludes{};alignas(8) std::array<std::int32_t,2> brush_origin{};std::uint32_t stretch{3},bitmap_pattern{},pattern_color{},dst_dib{};std::uint32_t operation{0xcc},transparent{UINT32_MAX};alignas(16) std::array<float,4> inverse_linear{1,0,0,1};std::array<float,2> inverse_offset{};std::uint32_t transformed{},rop2{13};};
static_assert(sizeof(Params)==160 && offsetof(Params,inverse_linear)==128 && offsetof(Params,inverse_offset)==144);
static_assert(offsetof(Params,excludes)==80 && offsetof(Params,brush_origin)==88);
static_assert(offsetof(Params,stretch)==96 && offsetof(Params,bitmap_pattern)==100 && offsetof(Params,pattern_color)==104 && offsetof(Params,dst_dib)==108);
struct Device {
    id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLComputePipelineState> pipeline;
    Device(){device=MTLCreateSystemDefaultDevice();if(!device)throw std::runtime_error("無可用 Metal GPU");queue=[device newCommandQueue];NSError* error=nil;
        auto lib=fixed_metal_library(device,&error);
        if(!lib)throw std::runtime_error(std::string("Metal shader 編譯失敗：")+error.localizedDescription.UTF8String);
        pipeline=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"raster"] error:&error];
        if(!pipeline || !queue)throw std::runtime_error("Metal pipeline 建立失敗");
    }
};
std::shared_ptr<Device> device(){static std::weak_ptr<Device> weak;auto p=weak.lock();if(!p){p=std::make_shared<Device>();weak=p;}return p;}
std::uint32_t bgra(std::uint32_t c){return (c&0xff00ff00U)|((c&255)<<16)|((c>>16)&255);}
class MetalRaster final:public RasterSurface,public std::enable_shared_from_this<MetalRaster> {
    MetalSharedFrame imported_frame_;
    std::shared_ptr<Device> device_;id<MTLTexture> texture_;id<MTLCommandBuffer> commands_,last_;
    CAMetalLayer* layer_;X64ArmRasterView* presentation_view_;bool presentation_dirty_{};std::uint32_t width_,height_;unsigned pending_{};bool dib_alpha_{};
    std::chrono::steady_clock::time_point presented_{};
    std::shared_ptr<macos_detail::GuiTiming> timing_;
    std::uint64_t timing_surface_{},timing_frame_{};
    std::map<std::string,id<MTLTexture>> icons_;
    struct GlyphMask {id<MTLTexture> texture;std::array<std::int32_t,2> size;std::shared_ptr<const void> font;std::array<std::int32_t,2> origin{};};
    std::map<std::u16string,GlyphMask> glyphs_;
    std::vector<std::uint32_t> upload_staging_;
    struct Reader {std::weak_ptr<MetalRaster> owner;id<MTLCommandBuffer> buffer;};
    std::vector<Reader> readers_;
    id<MTLTexture> texture(std::uint32_t w,std::uint32_t h){
        const auto alignment=[device_->device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
        const auto stride=(static_cast<NSUInteger>(w)*4+alignment-1)/alignment*alignment;
        auto allocation=[X64ArmGpuAllocation new];allocation.reservation=reserve_gpu_memory(stride*h);
        allocation.buffer=[device_->device newBufferWithLength:allocation.reservation->size() options:MTLResourceStorageModeShared];
        if(!allocation.buffer)throw std::bad_alloc();
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
        auto result=[allocation.buffer newTextureWithDescriptor:d offset:0 bytesPerRow:stride];if(!result)throw std::runtime_error("Metal shared-buffer texture 建立失敗");
        // GPU command buffers retain this texture and therefore its reservation.
        // The virtual address cannot be recycled before the last GPU use completes.
        static char key;objc_setAssociatedObject(result,&key,allocation,OBJC_ASSOCIATION_RETAIN_NONATOMIC);return result;
    }
    id<MTLTexture> cached_texture(std::uint32_t w,std::uint32_t h){
        try{return texture(w,h);}catch(const std::bad_alloc&){
            if(glyphs_.empty() && icons_.empty())throw;
            // 容量可能先於項目數上限耗盡；清除可重建快取並等候既有 GPU 讀取完成。
            glyphs_.clear();icons_.clear();flush(true);
            return texture(w,h);
        }
    }
    void finish_readers(bool wait){
        // 跨表面的讀取可能還在另一個未提交 buffer。先提交該次讀取，
        // GPU 寫入可沿同一 queue 排序；只有 CPU upload 必須等 GPU 完成。
        for(auto& reader:readers_){
            if(auto owner=reader.owner.lock();owner && owner->commands_==reader.buffer)owner->flush();
            if(wait){[reader.buffer waitUntilCompleted];if(reader.buffer.status==MTLCommandBufferStatusError)throw std::runtime_error(std::string("Metal 讀取同步失敗：")+reader.buffer.error.localizedDescription.UTF8String);}
        }
        readers_.clear();
    }
    void add_reader(MetalRaster& owner){
        if(&owner==this || !owner.commands_)return;
        std::erase_if(readers_,[](const Reader& reader){return reader.buffer.status==MTLCommandBufferStatusCompleted;});
        for(const auto& reader:readers_)if(reader.buffer==owner.commands_)return;
        readers_.push_back({owner.weak_from_this(),owner.commands_});
    }
    void begin(){finish_readers(false);if(!commands_)commands_=[device_->queue commandBuffer];}
    static void transform(Params& p,const GuiTransform& matrix){
        if(matrix==gui_identity)return;const auto inverse=gui_inverse(matrix);if(!inverse)throw std::invalid_argument("GDI 轉換不可逆");
        for(unsigned i=0;i<4;++i)p.inverse_linear[i]=static_cast<float>((*inverse)[i]);for(unsigned i=0;i<2;++i)p.inverse_offset[i]=static_cast<float>((*inverse)[i+4]);p.transformed=1;
    }
    void submit(const Params& p,const std::vector<std::array<std::int32_t,4>>& excluded,id<MTLTexture> source=nil,id<MTLTexture> brush=nil){
        auto lo=std::array<std::int32_t,2>{std::min(p.rect[0],p.rect[2]),std::min(p.rect[1],p.rect[3])},hi=std::array<std::int32_t,2>{std::max(p.rect[0],p.rect[2]),std::max(p.rect[1],p.rect[3])};
        if(p.kind==3){const auto pad=static_cast<std::int32_t>(p.width+1);for(unsigned i=0;i<2;++i){lo[i]-=pad;hi[i]+=pad;}}
        if(p.transformed){const GuiTransform inverse{p.inverse_linear[0],p.inverse_linear[1],p.inverse_linear[2],p.inverse_linear[3],p.inverse_offset[0],p.inverse_offset[1]};const auto forward=gui_inverse(inverse);if(!forward)throw std::invalid_argument("GDI 轉換精度不足");const auto bounds=gui_transform_bounds(*forward,{lo[0],lo[1],hi[0],hi[1]});lo={bounds[0],bounds[1]};hi={bounds[2],bounds[3]};}
        lo[0]=std::max(0,lo[0]);lo[1]=std::max(0,lo[1]);hi[0]=std::min(static_cast<std::int32_t>(width_),hi[0]);hi[1]=std::min(static_cast<std::int32_t>(height_),hi[1]);
        if(hi[0]<=lo[0] || hi[1]<=lo[1])return;
        begin();auto enc=[commands_ computeCommandEncoder];MetalEncoderScope enc_scope{enc};[enc setComputePipelineState:device_->pipeline];[enc setTexture:texture_ atIndex:0];[enc setTexture:source?source:texture_ atIndex:1];[enc setTexture:brush?brush:texture_ atIndex:2];[enc setBytes:&p length:sizeof(p) atIndex:0];
        const std::array<std::int32_t,4> empty{};[enc setBytes:excluded.empty()?static_cast<const void*>(empty.data()):static_cast<const void*>(excluded.data()) length:excluded.empty()?sizeof(empty):excluded.size()*sizeof(empty) atIndex:1];[enc setBytes:lo.data() length:sizeof(lo) atIndex:2];
        [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(hi[0]-lo[0]),static_cast<NSUInteger>(hi[1]-lo[1]),1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];enc_scope.end();
        presentation_dirty_=true;
        if(++pending_>=256 || (layer_ && std::chrono::steady_clock::now()-presented_>std::chrono::milliseconds(16)))flush();
    }
    Params params(const GuiDraw& d){Params p;p.rect={d.x,d.y,d.right,d.bottom};p.clip=d.clipped?d.clip:std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(width_),static_cast<std::int32_t>(height_)};p.color=d.color;p.rop2=d.rop2;p.width=static_cast<std::uint32_t>(std::max(0,d.pen_width));p.fill=d.filled?d.fill_color:UINT32_MAX;p.hatch=d.hatch;p.background=d.pattern_background;p.opaque=d.pattern_opaque;p.excludes=static_cast<std::uint32_t>(d.excluded.size());p.brush_origin=d.brush_origin;p.pattern_color=d.pattern_color;p.dst_dib=dib_alpha_;transform(p,d.transform);return p;}
    void icon(const GuiDraw& d){
        const auto w=static_cast<std::uint32_t>(std::max(0,d.right-d.x)),h=static_cast<std::uint32_t>(std::max(0,d.bottom-d.y));if(!w || !h)return;
        std::string key(reinterpret_cast<const char*>(d.image.data()),d.image.size());key.append(reinterpret_cast<const char*>(&d.system_icon),4);key.append(reinterpret_cast<const char*>(&w),4);key.append(reinterpret_cast<const char*>(&h),4);
        auto it=icons_.find(key);if(it==icons_.end()){
            NSImage* image=nil;if(!d.image.empty())image=[[NSImage alloc]initWithData:[NSData dataWithBytes:d.image.data() length:d.image.size()]];
            else if(d.system_icon==32513 || d.system_icon==32514 || d.system_icon==32516)image=[NSImage imageWithSystemSymbolName:d.system_icon==32513?@"xmark.octagon.fill":d.system_icon==32514?@"questionmark.circle":@"info.circle" accessibilityDescription:nil];else image=[NSImage imageNamed:d.system_icon==32515?NSImageNameCaution:NSImageNameApplicationIcon];
            if(!image)return;auto rep=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:nullptr pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaFirst|NSBitmapFormatThirtyTwoBitLittleEndian bytesPerRow:w*4 bitsPerPixel:32];std::memset(rep.bitmapData,0,static_cast<std::size_t>(w)*h*4);
            [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];[image drawInRect:NSMakeRect(0,0,w,h) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];[NSGraphicsContext restoreGraphicsState];auto source=cached_texture(w,h);[source replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:rep.bitmapData bytesPerRow:w*4];if(icons_.size()>=32)icons_.clear();it=icons_.emplace(std::move(key),source).first;
        }
        auto p=params(d);p.kind=4;p.source={0,0,static_cast<std::int32_t>(w),static_cast<std::int32_t>(h)};p.blend=0x01ff0000;submit(p,d.excluded,it->second);
    }
    void text(const GuiDraw& d){
        // CoreText/AppKit shapes a cached transparent glyph mask; Metal performs
        // all surface composition. Cache keys include the complete font layout.
        std::u16string key;
        const auto append=[&](std::uint32_t x){key.push_back(static_cast<char16_t>(x));key.push_back(static_cast<char16_t>(x>>16));};
        append(static_cast<std::uint32_t>(d.text.size()));key+=d.text;append(static_cast<std::uint32_t>(d.font.face.size()));key+=d.font.face;
        append(static_cast<std::uint32_t>(d.glyph_indices)|(static_cast<std::uint32_t>(d.rtl_reading)<<1));append(d.font.charset);append(static_cast<std::uint32_t>(d.advances.size()));append(static_cast<std::uint32_t>(d.advances_y.size()));for(auto a:d.advances_y)append(static_cast<std::uint32_t>(a));
        append(static_cast<std::uint32_t>(d.font.height));append(static_cast<std::uint32_t>(d.font.width));append(static_cast<std::uint32_t>(d.font.weight));append(d.cell_width);append(d.font.pitch_family);append(static_cast<std::uint32_t>(d.font.italic|(d.font.underline<<1)|(d.font.strikeout<<2)));for(auto a:d.advances)append(static_cast<std::uint32_t>(a));
        const auto identity=reinterpret_cast<std::uintptr_t>(d.font.native.get());append(static_cast<std::uint32_t>(identity));append(static_cast<std::uint32_t>(static_cast<std::uint64_t>(identity)>>32));
        auto it=glyphs_.find(key);if(it==glyphs_.end()){
            if(d.glyph_indices){
                const auto raster=native_gdi_glyph_raster(d);if(raster.alpha.empty())return;
                const auto tw=static_cast<unsigned>(raster.bounds[2]-raster.bounds[0]),th=static_cast<unsigned>(raster.bounds[3]-raster.bounds[1]);
                if(tw>16384 || th>16384 || static_cast<std::uint64_t>(tw)*th*4>64*1024*1024)throw std::bad_alloc();
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(tw)*th*4,255);
                for(std::size_t i=0;i<static_cast<std::size_t>(tw)*th;++i){unsigned alpha=0;for(unsigned c=0;c<raster.channels;++c)alpha+=raster.alpha[i*raster.channels+c];pixels[i*4+3]=static_cast<std::uint8_t>((alpha+raster.channels/2)/raster.channels);}
                auto mask=cached_texture(tw,th);[mask replaceRegion:MTLRegionMake2D(0,0,tw,th) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:static_cast<NSUInteger>(tw)*4];
                if(glyphs_.size()>=512)glyphs_.clear();it=glyphs_.emplace(std::move(key),GlyphMask{mask,{static_cast<int>(tw),static_cast<int>(th)},d.font.native,{raster.bounds[0],raster.bounds[1]}}).first;
            }else {
            NSString* string=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(d.text.data()) length:d.text.size()];
            // 與量測共用同一實體字型，保留私有 face、正 lfHeight 與等寬替代字型。
            NSFont* font=x64arm::native_gui_font(d.font);NSString* sample=font.fixedPitch?@"0":@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            const CGFloat average=std::max<CGFloat>(1,[sample sizeWithAttributes:@{NSFontAttributeName:font}].width/static_cast<CGFloat>(sample.length));
            const CGFloat width_scale=d.font.width?static_cast<CGFloat>(std::clamp<std::int64_t>(std::abs(static_cast<std::int64_t>(d.font.width)),1,512))/average:1;
            NSColor* color=[NSColor whiteColor];NSMutableDictionary* attrs=[@{NSFontAttributeName:font,NSForegroundColorAttributeName:color}mutableCopy];if(d.font.underline)attrs[NSUnderlineStyleAttributeName]=@(NSUnderlineStyleSingle);if(d.font.strikeout)attrs[NSStrikethroughStyleAttributeName]=@(NSUnderlineStyleSingle);
            if(d.rtl_reading)attrs[NSWritingDirectionAttributeName]=@[@(static_cast<NSInteger>(NSWritingDirectionRightToLeft)|static_cast<NSInteger>(NSWritingDirectionEmbedding))];
            const CGFloat line_height=std::ceil(font.ascender-font.descender+font.leading)+4;
            CGFloat left=0,top=0,right=[string sizeWithAttributes:attrs].width*width_scale+4,bottom=line_height;
            const auto include_ink=[&](NSString* part,CGFloat x,CGFloat y){
                NSAttributedString* attributed=[[NSAttributedString alloc]initWithString:part attributes:attrs];
                const auto line=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);if(!line)throw std::bad_alloc();
                const auto box=CTLineGetBoundsWithOptions(line,kCTLineBoundsUseGlyphPathBounds);CFRelease(line);
                if(!CGRectIsNull(box) && !CGRectIsEmpty(box)){
                    // 排版字寬不包含負側承及外伸筆畫，額外保留一個反鋸齒像素。
                    left=std::min(left,std::floor(x+CGRectGetMinX(box)*width_scale)-1);right=std::max(right,std::ceil(x+CGRectGetMaxX(box)*width_scale)+1);
                    top=std::min(top,std::floor(y+std::ceil(font.ascender)-CGRectGetMaxY(box))-1);bottom=std::max(bottom,std::ceil(y+std::ceil(font.ascender)-CGRectGetMinY(box))+1);
                }
            };
            if(!d.advances.empty() || d.cell_width){
                CGFloat x=0,y=0;right=0;
                for(NSUInteger i=0;i<string.length;){const auto range=[string rangeOfComposedCharacterSequenceAtIndex:i];NSString* part=[string substringWithRange:range];const auto size=[part sizeWithAttributes:attrs];left=std::min(left,x);top=std::min(top,y);right=std::max(right,x+size.width*width_scale+4);bottom=std::max(bottom,y+line_height);include_ink(part,x,y);
                    for(NSUInteger j=0;j<range.length;++j){x+=d.advances.empty()?static_cast<CGFloat>(d.cell_width):static_cast<CGFloat>(i+j<d.advances.size()?d.advances[i+j]:0);y+=i+j<d.advances_y.size()?d.advances_y[i+j]:0;}i+=range.length;}
            }else include_ink(string,0,0);
            left=std::floor(left);top=std::floor(top);CGFloat w=right-left,h=bottom-top;
            w=std::max(1.0,std::ceil(w));h=std::max(1.0,std::ceil(h));
            // ETO_PDY 可把可見字元放在很遠的前一個字元之後，不能裁短遮罩再回傳成功。
            if(!std::isfinite(w) || !std::isfinite(h) || w>16384 || h>16384 || w*h*4>64*1024*1024)throw std::bad_alloc();
            const auto tw=static_cast<std::uint32_t>(w),th=static_cast<std::uint32_t>(h);
            auto rep=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:nullptr pixelsWide:tw pixelsHigh:th bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaFirst|NSBitmapFormatThirtyTwoBitLittleEndian bytesPerRow:tw*4 bitsPerPixel:32];if(!rep || !rep.bitmapData)throw std::bad_alloc();std::memset(rep.bitmapData,0,static_cast<std::size_t>(tw)*th*4);
            // Flipped metadata must agree with the CTM, otherwise AppKit mirrors glyphs.
            auto bitmap_context=[NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
            [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:bitmap_context.CGContext flipped:YES]];NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:0 yBy:th];[transform scaleXBy:width_scale yBy:-1];[transform concat];
            if(d.advances.empty() && !d.cell_width)[string drawAtPoint:NSMakePoint(-left/width_scale,-top) withAttributes:attrs];else{CGFloat x=-left/width_scale,y=-top;for(NSUInteger i=0;i<string.length;){auto range=[string rangeOfComposedCharacterSequenceAtIndex:i];[[string substringWithRange:range]drawAtPoint:NSMakePoint(x,y) withAttributes:attrs];for(NSUInteger j=0;j<range.length;++j){x+=(d.advances.empty()?static_cast<CGFloat>(d.cell_width):static_cast<CGFloat>(i+j<d.advances.size()?d.advances[i+j]:0))/width_scale;y+=i+j<d.advances_y.size()?d.advances_y[i+j]:0;}i+=range.length;}}
            [NSGraphicsContext restoreGraphicsState];auto mask=cached_texture(tw,th);[mask replaceRegion:MTLRegionMake2D(0,0,tw,th) mipmapLevel:0 withBytes:rep.bitmapData bytesPerRow:tw*4];
            if(glyphs_.size()>=512)glyphs_.clear();it=glyphs_.emplace(std::move(key),GlyphMask{mask,{static_cast<std::int32_t>(tw),static_cast<std::int32_t>(th)},d.font.native,{static_cast<int>(left),static_cast<int>(top)}}).first;
            }
        }
        const auto coordinate=[](std::int64_t value){if(value<INT32_MIN || value>INT32_MAX)throw std::invalid_argument("文字遮罩座標溢位");return static_cast<std::int32_t>(value);};
        const auto x=static_cast<std::int64_t>(d.x)+it->second.origin[0],y=static_cast<std::int64_t>(d.y)+it->second.origin[1];
        auto p=params(d);p.kind=5;p.rect={coordinate(x),coordinate(y),coordinate(x+it->second.size[0]),coordinate(y+it->second.size[1])};p.source={0,0,it->second.size[0],it->second.size[1]};p.blend=0x01ff0000;submit(p,d.excluded,it->second.texture);
    }
public:
    MetalRaster(std::uint32_t w,std::uint32_t h,std::span<const std::uint32_t> pixels,void* view):device_(device()),width_(w),height_(h){
        texture_=texture(w,h);if(!texture_)throw std::runtime_error("Metal surface 配置失敗");std::vector<std::uint32_t> initial(static_cast<std::size_t>(w)*h,0xffffffffU);if(!pixels.empty()){if(pixels.size()!=initial.size())throw std::invalid_argument("Metal surface 像素數量不符");std::transform(pixels.begin(),pixels.end(),initial.begin(),[](auto c){return bgra(c)|0xff000000U;});}
        [texture_ replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:initial.data() bytesPerRow:static_cast<NSUInteger>(w)*4];
        if(view){NSView* host=(__bridge NSView*)view;host.wantsLayer=YES;layer_=[CAMetalLayer layer];layer_.device=device_->device;layer_.pixelFormat=MTLPixelFormatBGRA8Unorm;layer_.framebufferOnly=NO;
            auto color_space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);layer_.colorspace=color_space;CGColorSpaceRelease(color_space);
            layer_.drawableSize=CGSizeMake(w,h);presentation_view_=[[X64ArmRasterView alloc] initWithFrame:host.bounds];
            presentation_view_.identifier=@"x64arm-raster-presentation";
            presentation_view_.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
            presentation_view_.layer=layer_;presentation_view_.wantsLayer=YES;
            presentation_view_.canDrawSubviewsIntoLayer=NO;
            [host addSubview:presentation_view_ positioned:NSWindowBelow relativeTo:nil];
            // 自訂客體迴圈可能長時間不返回 AppKit run loop，必須提交新圖層樹。
            [CATransaction flush];presentation_dirty_=true;
            timing_=macos_detail::gui_timing();if(timing_)timing_surface_=timing_->surface();}
    }
    ~MetalRaster(){if(commands_){track_gpu_commands(commands_);[commands_ commit];[commands_ waitUntilCompleted];}if(last_)[last_ waitUntilCompleted];[presentation_view_ removeFromSuperview];}
    std::uint32_t width()const override{return width_;}std::uint32_t height()const override{return height_;}
    std::string renderer()const override{return std::string("Metal / ")+device_->device.name.UTF8String;}
    void set_dib_alpha(bool value)override{dib_alpha_=value;}
    void set_transparent(bool value)override{if(layer_){layer_.opaque=!value;layer_.backgroundColor=value?CGColorGetConstantColor(kCGColorClear):nullptr;}}
    void upload(std::span<const std::uint32_t> pixels,bool preserve_alpha=false)override{
        if(pixels.size()!=static_cast<std::size_t>(width_)*height_)throw std::invalid_argument("Metal upload 像素數量不符");
        upload_staging_.resize(pixels.size());
        std::transform(pixels.begin(),pixels.end(),upload_staging_.begin(),[&](auto c){return bgra(c)|(preserve_alpha?0U:0xff000000U);});
        finish_readers(true);flush(true);[texture_ replaceRegion:MTLRegionMake2D(0,0,width_,height_) mipmapLevel:0 withBytes:upload_staging_.data() bytesPerRow:static_cast<NSUInteger>(width_)*4];presentation_dirty_=true;
    }
    void import_frame(void* bytes,std::size_t length,std::uint32_t stride,std::uint32_t format,std::shared_ptr<void> owner)override{@autoreleasepool{
        // 保留原始 UNORM bytes，避免在跨程序傳輸階段重複套用 sRGB 解碼。
        const auto pixel_format=(format==87 || format==91)?MTLPixelFormatBGRA8Unorm:MTLPixelFormatRGBA8Unorm;
        auto source=imported_frame_.texture(device_->device,bytes,length,stride,pixel_format,width_,height_,owner);
        Params p;p.kind=4;p.rect=p.source=p.clip={0,0,static_cast<std::int32_t>(width_),static_cast<std::int32_t>(height_)};p.dst_dib=1;
        submit(p,{},source);flush(true);
    }}
    void draw(const GuiDraw& d,RasterSurface* pattern=nullptr)override{@autoreleasepool{if(d.kind==GuiDraw::Kind::Text){text(d);return;}if(d.kind==GuiDraw::Kind::Icon){icon(d);return;}auto p=params(d);if(d.kind==GuiDraw::Kind::Fill)p.kind=0;else if(d.kind==GuiDraw::Kind::Rectangle)p.kind=1;else if(d.kind==GuiDraw::Kind::Ellipse)p.kind=2;else if(d.kind==GuiDraw::Kind::Line)p.kind=3;else throw std::invalid_argument("Metal raster 不支援此圖元");id<MTLTexture> input=nil;if(pattern){auto* source=dynamic_cast<MetalRaster*>(pattern);if(!source || source==this)throw std::invalid_argument("不合法的 raster pattern");source->flush();begin();source->add_reader(*this);input=source->texture_;p.bitmap_pattern=d.pattern_monochrome?2:1;}submit(p,d.excluded,input);}}
    void blit(RasterSurface& other,const std::array<std::int32_t,4>& destination,const std::array<std::int32_t,4>& source,const GuiDc& dc,std::uint32_t blend,const RasterBlit& options={},RasterSurface* pattern=nullptr)override{@autoreleasepool{
        auto* src=dynamic_cast<MetalRaster*>(&other);if(!src)throw std::invalid_argument("不同 raster backend 的 blit");src->flush();id<MTLTexture> input=src->texture_;begin();
        src->add_reader(*this);
        if(src==this){input=texture(width_,height_);auto copy=[commands_ blitCommandEncoder];MetalEncoderScope copy_scope{copy};[copy copyFromTexture:texture_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1) toTexture:input destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];copy_scope.end();}
        Params p;p.kind=4;p.rect=destination;p.source=source;p.clip=dc.clipped?dc.clip:std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(width_),static_cast<std::int32_t>(height_)};p.blend=blend;p.stretch=blend==UINT32_MAX?dc.stretch_mode:3;p.dst_dib=dib_alpha_;p.excludes=static_cast<std::uint32_t>(dc.excluded.size());
        transform(p,gui_device_transform(dc));
        p.operation=options.operation;p.transparent=options.transparent;p.color=options.brush.color;p.hatch=options.brush.hatch;p.opaque=options.brush.pattern_opaque;p.background=options.brush.pattern_background;p.brush_origin=options.brush.brush_origin;p.pattern_color=options.brush.pattern_color;
        id<MTLTexture> brush=nil;if(pattern){auto* paint=dynamic_cast<MetalRaster*>(pattern);if(!paint || paint==this)throw std::invalid_argument("不合法的 raster brush");paint->flush();paint->add_reader(*this);brush=paint->texture_;p.bitmap_pattern=options.brush.pattern_monochrome?2:1;}
        submit(p,dc.excluded,input,brush);
    }}
    GuiBitmap snapshot()override{flush(true);GuiBitmap result{width_,height_,std::vector<std::uint32_t>(static_cast<std::size_t>(width_)*height_)};[texture_ getBytes:result.pixels.data() bytesPerRow:static_cast<NSUInteger>(width_)*4 fromRegion:MTLRegionMake2D(0,0,width_,height_) mipmapLevel:0];for(auto& p:result.pixels)p=bgra(p);return result;}
    std::uint32_t pixel(std::uint32_t x,std::uint32_t y)override{if(x>=width_ || y>=height_)return UINT32_MAX;flush(true);std::uint32_t p=0;[texture_ getBytes:&p bytesPerRow:4 fromRegion:MTLRegionMake2D(x,y,1,1) mipmapLevel:0];return bgra(p)&0xffffffU;}
    void flush(bool wait=false)override{@autoreleasepool{
        // CPU upload／初始內容也需要 present；drawable 暫時不可用時保留 dirty。
        if(layer_ && presentation_dirty_ && !commands_)begin();
        if(commands_){
            if(layer_ && presentation_dirty_){
                const auto request=timing_?CACurrentMediaTime():0;
                auto drawable=[layer_ nextDrawable];
                record_diagnostic(drawable?"raster_drawable":"raster_no_drawable");
                if(static NSUInteger reported=0;drawable && layer_ && ++reported%120==1){
                    // 呈現視窗所在螢幕與狀態：名稱、更新率、縮放、drawable 尺寸、遮蔽狀態。
                    NSWindow* window=presentation_view_?presentation_view_.window:nil;
                    NSScreen* screen=window.screen;
                    // 呈現 view 被放在宿主 view 最底層；記錄兄弟 view 數、自身框與是否隱藏，判斷是否被其他圖層蓋住。
                    NSView* host=presentation_view_.superview;long fps=0;if(@available(macOS 12.0,*))fps=screen?static_cast<long>(screen.maximumFramesPerSecond):0;
                    char text[240];std::snprintf(text,sizeof text,"screen=%s fps=%ld scale=%.1f drawable=%.0fx%.0f texture=%ux%u visible=%d key=%d siblings=%lu frame=%.0fx%.0f host=%.0fx%.0f hidden=%d",
                        screen?screen.localizedName.UTF8String:"?",fps,screen?screen.backingScaleFactor:0.0,
                        layer_.drawableSize.width,layer_.drawableSize.height,width_,height_,window?((window.occlusionState&NSWindowOcclusionStateVisible)!=0):-1,window?window.isKeyWindow:-1,
                        static_cast<unsigned long>(host?host.subviews.count:0),presentation_view_.frame.size.width,presentation_view_.frame.size.height,host?host.bounds.size.width:0.0,host?host.bounds.size.height:0.0,presentation_view_.hidden?1:0);
                    set_diagnostic("raster_present_state",text);
                }
                if(drawable){
                    [drawable addPresentedHandler:^(id<MTLDrawable> shown){if(shown.presentedTime>0)record_diagnostic("raster_presented");else record_diagnostic("raster_present_dropped");}];
                    auto copy=[commands_ blitCommandEncoder];MetalEncoderScope copy_scope{copy};
                    [copy copyFromTexture:texture_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1) toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                    copy_scope.end();
                    if(timing_){
                        const auto timing=timing_;const auto surface=timing_surface_,frame=++timing_frame_;
                        const auto width=width_,height=height_;const auto submit=CACurrentMediaTime();
                        timing->frame("submitted",surface,frame,width,height,request,submit);
                        [drawable addPresentedHandler:^(id<MTLDrawable> shown){
                            timing->frame("presented",surface,frame,width,height,request,submit,shown.presentedTime);
                        }];
                        [commands_ addCompletedHandler:^(id<MTLCommandBuffer> buffer){
                            timing->frame("completed",surface,frame,width,height,request,submit,0,buffer.GPUStartTime,buffer.GPUEndTime);
                        }];
                    }
                    [commands_ presentDrawable:drawable];
                    presentation_dirty_=false;presented_=std::chrono::steady_clock::now();
                }
            }
            track_gpu_commands(commands_);[commands_ commit];last_=commands_;commands_=nil;pending_=0;
        }
        if(wait && last_){[last_ waitUntilCompleted];if(last_.status==MTLCommandBufferStatusError)throw std::runtime_error(std::string("Metal 執行失敗：")+last_.error.localizedDescription.UTF8String);}
    }}
};
}
std::shared_ptr<RasterSurface> make_hardware_raster(std::uint32_t w,std::uint32_t h,std::span<const std::uint32_t> pixels,void* view){if(!w || !h || w>16384 || h>16384 || std::uint64_t{w}*h>64*1024*1024)throw std::invalid_argument("Metal surface 大小超限");@autoreleasepool{return std::make_shared<MetalRaster>(w,h,pixels,view);}}
}
