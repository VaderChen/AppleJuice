#include "metal_shared_frame.hpp"
#include "metal_command_encoder.hpp"
#include "metal_gpu_counter.hpp"
#include "metal_stencil.hpp"
#include "metal_gl_shared_names.hpp"
#include "packed_float.hpp"
#include "x64arm/glsl.hpp"
#include "fixed_metal_library.hpp"
#include "opengl_lighting.hpp"
#include "resource_usage.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/gpu_memory.hpp"
#include <IOKit/IOKitLib.h>
#include "x64arm/graphics_index_range.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <map>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <deque>
#include <chrono>
#include <cstdlib>
#include <regex>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sstream>
#include <hlslcc.h>
#include <stdexcept>

namespace x64arm { namespace {
using namespace metal_gl;
Matrix identity(){Matrix m{};m[0]=m[5]=m[10]=m[15]=1;return m;}
Matrix multiply(const Matrix& a,const Matrix& b){Matrix m{};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)m[c*4+r]+=a[k*4+r]*b[c*4+k];return m;}
struct Vertex {Vec position{},color{},texcoord{},normal{},ambient{},diffuse{},specular{},emission{},shininess{};};
static_assert(sizeof(Vertex)==9*16);
struct Fog {Vec color{};float density{1},start{},end{1},index{};unsigned mode{0x800};};

struct Texture {bool created{};id<MTLTexture> native;std::shared_ptr<GpuMemoryReservation> lease;unsigned min{0x2702},mag{0x2601},s{0x2901},t{0x2901},anisotropy{1},max_level{1000};std::map<unsigned,unsigned> formats;};
struct TextureEnvironment {unsigned mode{};Vec color{};};
struct GlStencil {unsigned function{0x207},reference{},read_mask{UINT32_MAX},write_mask{UINT32_MAX},fail{0x1e00},depth_fail{0x1e00},pass{0x1e00},clear{};};
struct State {
    Vec color{1,1,1,1},texcoord{0,0,0,1},normal{0,0,1,0},clear{};
    std::array<Light,8> lights=default_lights();std::array<Material,2> materials;
    Fog fog;unsigned fog_hint{0x1100},perspective_hint{0x1100};
    GlStencil stencil;
    std::array<double,2> depth_range{0,1};double depth_clear{1};bool depth_write{true};
    // glColorMask 位元依 R、G、B、A；線寬依 ALIASED_LINE_WIDTH_RANGE=[1,1] 柵格化，但保留查詢值。
    unsigned color_mask{15};float line_width{1};std::array<float,2> pixel_zoom{1,1};unsigned render_mode{0x1c00};
    unsigned texture_color_op{4},texture_alpha_op{4},texture_arguments{0x80000000U|2U|(1U<<6)|(2U<<12)|(1U<<18)};
    bool texture_combine_enabled{};
    std::array<TextureEnvironment,32> environments{};unsigned alpha_func{0x207};float alpha_ref{};
    unsigned cull_face{0x405},front_face{0x901};
    unsigned depth_func{0x201},blend_src{1},blend_dst{},binding{},matrix_mode{0x1700},draw_buffer{0x405},read_buffer{0x405};
    std::array<int,4> viewport{},scissor{};
    std::set<unsigned> enabled;
};
struct Command {GlOperation op;std::vector<std::uint64_t> args;std::vector<std::uint8_t> data;};
struct Array {int size{};unsigned type{};std::vector<std::uint8_t> bytes;bool enabled{};};
struct ViewUse {unsigned count{};CALayer* original;bool wanted{};};
std::map<void*,ViewUse> metal_view_users;
#include "metal_programmable_types.inc"
id<MTLTexture> metal_shader_texture(const GraphicsView&);

#include "metal_gl_program_types.inc"
#include "metal_immediate_present.inc"
#include "metal_command_thread.inc"
class MetalGl final:public NativeOpenGlContext {
    MetalSharedFrame exported_frame_;
    id<MTLDevice> device_;id<MTLCommandQueue> queue_;id<MTLLibrary> library_;
    id<MTLTexture> color_,front_,depth_;id<MTLCommandBuffer> commands_;
    bool commands_gpu_work_{};
    id<MTLEvent> handoff_event_;std::uint64_t handoff_value_{};
    NSView* view_{};CAMetalLayer* layer_{};
    std::unique_ptr<MetalImmediatePresenter> immediate_presenter_;
    bool immediate_unavailable_{};
    static bool immediate_present_enabled(){static const bool enabled=[] {const auto* value=std::getenv("X64ARM_METAL_IMMEDIATE_PRESENT");return !value || std::string_view(value)=="1";}();return enabled;}
    void finish_immediate_present(){if(immediate_presenter_){immediate_presenter_->finish();immediate_presenter_->report();immediate_presenter_.reset();}}
    std::shared_ptr<GpuMemoryReservation> surface_;
    std::vector<std::shared_ptr<GpuMemoryReservation>> pending_leases_;
    std::map<std::tuple<std::uint64_t,unsigned,unsigned,unsigned>,id<MTLRenderPipelineState>> pipelines_;
    bool fixed_targets_set_{};GraphicsView fixed_color_,fixed_depth_,fixed_texture_;
    std::array<std::uint32_t,12> fixed_stencil_{};
    GlSharedNames<Texture,true> textures_;
    struct TextureLevelInfo {unsigned width{},height{},format{};std::array<unsigned,4> bits{};unsigned depth{};};
    std::map<unsigned,TextureLevelInfo> proxy_levels_;
    void texture_level(GlOperation,std::span<const std::uint64_t>,std::span<std::uint8_t>);
    MetalPendingObjects graphics_pending_;
    struct Flight {id<MTLCommandBuffer> command;std::vector<std::shared_ptr<GpuMemoryReservation>> leases;MetalPendingObjects objects;std::shared_ptr<std::atomic<bool>> done;std::shared_ptr<MetalFenceRelays> relays;};
    std::deque<Flight> flights_;
    std::size_t flight_limit_{3},flight_peak_{};
    std::uint64_t wait_count_{},wait_ns_{},resource_wait_count_{};
    std::uint64_t constant_upload_skips_{},constant_upload_saved_bytes_{};
    // 鍵為數百位元組的二進位欄位串接；有序 map 每次查找要做十幾次長字串比較，改為雜湊。
    std::unordered_map<std::string,id<MTLRenderPipelineState>> graphics_pipelines_;
    std::shared_ptr<MetalBufferPool> discard_pool_{std::make_shared<MetalBufferPool>()};std::uint64_t discard_in_place_{};
    std::shared_ptr<std::atomic<bool>> commands_done_;
    std::shared_ptr<MetalFenceRelays> commands_relays_;
    #include "metal_state_cache.inc"
    std::map<std::array<unsigned,4>,std::shared_ptr<GraphicsResource>> discard_depth_;
    std::map<std::string,std::shared_ptr<MetalGeometryPipeline>> geometry_pipelines_;
    std::map<std::string,id<MTLComputePipelineState>> stream_copy_pipelines_;
    std::uint64_t stream_gpu_draws_{},stream_gpu_bytes_{};
    std::map<std::string,std::shared_ptr<MetalTessellationPipeline>> tessellation_pipelines_;
    std::uint64_t shader_serial_{};
    std::array<Array,4> arrays_;
    State state_;
    GlProgrammable gl_;
    std::optional<std::uint64_t> programmable_execute(GlOperation,std::span<const std::uint64_t>,std::span<const std::uint8_t>,std::span<std::uint8_t>);
    std::shared_ptr<GlBuffer> gl_buffer(unsigned);
    std::shared_ptr<GraphicsResource> gl_buffer_resource(const std::shared_ptr<GlBuffer>&);
    void gl_buffer_update(const std::shared_ptr<GlBuffer>&,std::span<const std::uint8_t>,std::uint32_t);
    std::optional<std::uint64_t> vertex_buffers_execute(GlOperation,std::span<const std::uint64_t>,std::span<const std::uint8_t>,std::span<std::uint8_t>);
    void gl_link(GlProgram&);
    void gl_stencil(MTLDepthStencilDescriptor*,bool clearing=false,unsigned clear_mask=0);
    void texture_environment(GlOperation,std::span<const std::uint64_t>,std::span<const std::uint8_t>,std::span<std::uint8_t>);
    void gl_draw(GlOperation,std::span<const std::uint64_t>,std::span<const std::uint8_t>);
    void gl_draw_once(GlOperation,std::span<const std::uint64_t>,std::span<const std::uint8_t>,bool&);
    void gl_texture(std::span<const std::uint64_t>,std::span<const std::uint8_t>,bool subimage=false);
    MTLRenderPassDescriptor* gl_pass(unsigned clear_mask=0);
    unsigned gl_framebuffer_status(bool read=false);
    void gl_read_pixels(std::span<const std::uint64_t>,std::span<std::uint8_t>);
    std::array<std::vector<Matrix>,3> matrices_{{{identity()},{identity()},{identity()}}};
    std::map<unsigned,int> pixel_{{0xcf5,4},{0xd05,4}};
    std::vector<std::map<unsigned,int>> pixel_stack_;
    struct AttributeSnapshot {
        unsigned mask{};State state;unsigned active{};std::array<unsigned,32> units{};
        std::array<unsigned,5> programmable{};std::array<std::array<float,4>,16> current_attributes{};
        std::map<unsigned,std::array<unsigned,6>> texture_parameters;
    };
    std::vector<AttributeSnapshot> attributes_;
    GlSharedNames<std::vector<Command>> lists_;
    GlSharedCounter next_list_{1},next_texture_{1};
    unsigned compiling_{},compile_mode_{},recursion_{},error_{},primitive_{};
    bool begun_{},replaying_{};std::vector<Vertex> vertices_;
    std::uint32_t width_{},height_{};int interval_{1};
    std::uint64_t draws_{},submissions_{},list_bytes_{};unsigned pending_{};
    GlOperation active_operation_{};
    void error(unsigned e){if(std::getenv("X64ARM_GL_TRACE"))std::fprintf(stderr,"[gl-error] operation=%u error=0x%x\n",static_cast<unsigned>(active_operation_),e);if(!error_)error_=e;}
    // 連續繪製沿用的 render encoder 與其附件鍵；任何其他編碼、提交前都先結束。
    id<MTLRenderCommandEncoder> open_render_;std::vector<std::uintptr_t> open_render_key_;std::uint64_t coalesced_draws_{},render_encoders_{};
    std::shared_ptr<std::atomic<std::uint64_t>> gpu_busy_ns_=std::make_shared<std::atomic<std::uint64_t>>(0);
    // 診斷：記錄每次結束沿用中 render encoder 的原因（統計字串輸出），找出切斷合併的來源。
    std::map<std::string,std::uint64_t> encoder_breaks_;
    void end_open_render(const char* why=nullptr){if(open_render_){++encoder_breaks_[why?why:"other"];[open_render_ endEncoding];open_render_=nil;open_render_key_.clear();}}
    void ensure_commands(bool gpu_work){if(!commands_){commands_=[queue_ commandBuffer];commands_gpu_work_=false;}if(!commands_)throw std::runtime_error("Metal command buffer 建立失敗");commands_gpu_work_|=gpu_work;}
    void begin_commands(bool gpu_work=true,const char* why=nullptr){end_open_render(why);ensure_commands(gpu_work);}
    void retire_front(bool wait){
        auto command=flights_.front().command;
        if(wait && command.status<MTLCommandBufferStatusCompleted){
            const auto begin=std::chrono::steady_clock::now();[command waitUntilCompleted];++wait_count_;
            wait_ns_+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
        }
        if(command.status==MTLCommandBufferStatusError)throw std::runtime_error("Metal GPU command failed："+std::string(command.error.localizedDescription.UTF8String));
        flights_.pop_front();
    }
    void reap(){while(!flights_.empty() && flights_.front().command.status>=MTLCommandBufferStatusCompleted)retire_front(false);}
    void flush(bool wait,const char* why="flush"){
        end_open_render(why);
        reap();
        if(commands_){
            if(flights_.size()>=flight_limit_)retire_front(true);
            // 在 commit 之前配置追蹤節點；資源／額度保留至 GPU 完成，避免非同步重用。
            flights_.emplace_back();auto& flight=flights_.back();flight.command=commands_;
            flight.leases=std::move(pending_leases_);flight.objects=std::move(graphics_pending_);
            // 完成旗標供 D3D11 事件查詢輪詢；回呼只寫旗標，不觸碰後端狀態。
            flight.done=commands_done_?std::move(commands_done_):std::make_shared<std::atomic<bool>>(false);commands_done_=nullptr;auto done=flight.done;
            flight.relays=commands_relays_?std::move(commands_relays_):std::make_shared<MetalFenceRelays>();commands_relays_=nullptr;auto relays=flight.relays;
            // GPU 忙碌時間累計供診斷（CPU／GPU 瓶頸判斷）；只讀命令緩衝時間戳，不觸碰後端狀態。
            auto gpu_ns=gpu_busy_ns_;
            [commands_ addCompletedHandler:^(id<MTLCommandBuffer> buffer){
                const auto start=buffer.GPUStartTime,end=buffer.GPUEndTime;
                if(end>start)gpu_ns->fetch_add(static_cast<std::uint64_t>((end-start)*1e9),std::memory_order_relaxed);
                done->store(true,std::memory_order_release);relays->fire();}];
            if(commands_gpu_work_)track_gpu_commands(commands_);[commands_ commit];commands_=nil;pending_leases_.clear();graphics_pending_.clear();pending_=0;++submissions_;
            flight_peak_=std::max(flight_peak_,flights_.size());
        }
        if(wait)while(!flights_.empty())retire_front(true);
    }
    void wait_resource(const std::shared_ptr<GraphicsResource>& resource){
        const auto identity=metal_resource_use_identity(resource);
        const auto uses=[&](const auto& objects){return objects.contains(identity);};
        if(uses(graphics_pending_))flush(false,"wait_resource");else reap();
        std::size_t through=0;for(std::size_t n=0;n<flights_.size();++n)if(uses(flights_[n].objects))through=n+1;
        if(through)++resource_wait_count_;
        // 同一 queue 保證先後；只等待最後使用此資源的批次，保留後續無關工作。
        while(through--)retire_front(true);
    }
    bool graphics_upload_buffer(const std::shared_ptr<GraphicsResource>&,std::span<const std::uint8_t>,std::uint32_t);
    bool graphics_replace_buffer(const std::shared_ptr<GraphicsResource>&,std::span<const std::uint8_t>);
    void graphics_write_buffer(const std::shared_ptr<GraphicsResource>&,std::span<const std::uint8_t>,std::uint32_t);
    void detach(){if(!view_)return;auto it=metal_view_users.find((__bridge void*)view_);if(it!=metal_view_users.end()){
        if(view_.layer==layer_)view_.layer=it->second.original;
        if(--it->second.count==0){view_.layer=it->second.original;view_.wantsLayer=it->second.wanted;if([view_ respondsToSelector:NSSelectorFromString(@"setOpenGLManaged:")])[view_ setValue:@NO forKey:@"openGLManaged"];metal_view_users.erase(it);[view_ setNeedsDisplay:YES];}}
        view_=nil;layer_=nil;
    }
    Matrix& matrix(){return matrices_[state_.matrix_mode-0x1700].back();}
    static MTLSamplerAddressMode address_mode(unsigned value){
        switch(value){case 0x2901:return MTLSamplerAddressModeRepeat;case 0x8370:return MTLSamplerAddressModeMirrorRepeat;case 0x812d:return MTLSamplerAddressModeClampToBorderColor;default:return MTLSamplerAddressModeClampToEdge;}
    }
    static bool factor(unsigned x){return x<=1 || (x>=0x300 && x<=0x308);}
    static MTLBlendFactor blend(unsigned x){switch(x){case 0:return MTLBlendFactorZero;case 1:return MTLBlendFactorOne;case 0x300:return MTLBlendFactorSourceColor;case 0x301:return MTLBlendFactorOneMinusSourceColor;case 0x302:return MTLBlendFactorSourceAlpha;case 0x303:return MTLBlendFactorOneMinusSourceAlpha;case 0x304:return MTLBlendFactorDestinationAlpha;case 0x305:return MTLBlendFactorOneMinusDestinationAlpha;case 0x306:return MTLBlendFactorDestinationColor;case 0x307:return MTLBlendFactorOneMinusDestinationColor;default:return MTLBlendFactorSourceAlphaSaturated;}}
    id<MTLRenderPipelineState> pipeline(bool clear,bool color_write=true,bool fixed=false,MTLPixelFormat color_format=MTLPixelFormatBGRA8Unorm,MTLPixelFormat depth_format=MTLPixelFormatDepth32Float_Stencil8,unsigned samples=1){
        const bool blending=!clear && state_.enabled.contains(0xbe2);const std::uint64_t flags=(blending?1ULL:0ULL)|(std::uint64_t{state_.blend_src}<<1)|(std::uint64_t{state_.blend_dst}<<16)|(color_write?1ULL<<40:0)|(fixed?1ULL<<41:0)|(std::uint64_t{state_.color_mask&15U}<<42);
        const auto key=std::tuple{flags,static_cast<unsigned>(color_format),static_cast<unsigned>(depth_format),samples};
        if(pipelines_.contains(key))return pipelines_.at(key);
        auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=[library_ newFunctionWithName:fixed?@"fixed_vertex_main":@"vertex_main"];desc.fragmentFunction=[library_ newFunctionWithName:@"fragment_main"];desc.colorAttachments[0].pixelFormat=color_format;desc.depthAttachmentPixelFormat=depth_format;desc.stencilAttachmentPixelFormat=depth_format==MTLPixelFormatDepth32Float_Stencil8?depth_format:MTLPixelFormatInvalid;desc.sampleCount=samples;
        auto attachment=desc.colorAttachments[0];attachment.blendingEnabled=blending;attachment.sourceRGBBlendFactor=blend(state_.blend_src);attachment.sourceAlphaBlendFactor=blend(state_.blend_src);attachment.destinationRGBBlendFactor=blend(state_.blend_dst);attachment.destinationAlphaBlendFactor=blend(state_.blend_dst);attachment.writeMask=color_write?((state_.color_mask&1U?MTLColorWriteMaskRed:MTLColorWriteMaskNone)|(state_.color_mask&2U?MTLColorWriteMaskGreen:MTLColorWriteMaskNone)|(state_.color_mask&4U?MTLColorWriteMaskBlue:MTLColorWriteMaskNone)|(state_.color_mask&8U?MTLColorWriteMaskAlpha:MTLColorWriteMaskNone)):MTLColorWriteMaskNone;
        NSError* e=nil;auto p=[device_ newRenderPipelineStateWithDescriptor:desc error:&e];if(!p)throw std::runtime_error(std::string("Metal GL pipeline: ")+e.localizedDescription.UTF8String);pipelines_[key]=p;return p;
    }
    MTLScissorRect scissor(bool enabled,unsigned width=0,unsigned height=0){
        const auto clip_width=width?width:width_,clip_height=height?height:height_;
        if(!enabled)return {0,0,clip_width,clip_height};
        const auto& r=state_.scissor;const auto x=std::clamp<std::int64_t>(r[0],0,clip_width),y=std::clamp<std::int64_t>(r[1],0,clip_height);
        const auto right=std::clamp<std::int64_t>(static_cast<std::int64_t>(r[0])+r[2],x,clip_width),top=std::clamp<std::int64_t>(static_cast<std::int64_t>(r[1])+r[3],y,clip_height);
        return {static_cast<NSUInteger>(x),static_cast<NSUInteger>(clip_height-top),static_cast<NSUInteger>(right-x),static_cast<NSUInteger>(top-y)};
    }
    void draw(std::span<const Vertex> vertices,MTLPrimitiveType primitive,bool clearing=false,unsigned clear_mask=0,const FixedGraphicsDraw* fixed=nullptr){
        id<MTLTexture> render_color=fixed_targets_set_?metal_resource(fixed_color_.resource).texture:(state_.draw_buffer==0x404?front_:color_);
        id<MTLTexture> render_depth=fixed_targets_set_?(fixed_depth_.resource?metal_resource(fixed_depth_.resource).texture:nil):depth_;
        const unsigned target_width=fixed_targets_set_?std::max(1U,fixed_color_.resource->desc.width>>fixed_color_.level):width_;
        const unsigned target_height=fixed_targets_set_?std::max(1U,fixed_color_.resource->desc.height>>fixed_color_.level):height_;
        if((fixed?fixed->vertices.empty():vertices.empty()) || (!clearing && primitive==MTLPrimitiveTypeTriangle && state_.enabled.contains(0xb44) && state_.cull_face==0x408))return;const auto clip=scissor(state_.enabled.contains(0xc11),target_width,target_height);if(!clip.width || !clip.height)return;
        if(!clearing && (!state_.viewport[2] || !state_.viewport[3]))return;
        auto p=pipeline(clearing,!clearing || (clear_mask&0x4000),fixed!=nullptr,render_color.pixelFormat,render_depth?render_depth.pixelFormat:MTLPixelFormatInvalid,static_cast<unsigned>(render_color.sampleCount));
        const auto vertex_bytes=fixed?fixed->vertices.size_bytes():vertices.size_bytes();const void* vertex_data=fixed?static_cast<const void*>(fixed->vertices.data()):vertices.data();
        auto lease=reserve_gpu_memory(vertex_bytes+(fixed?fixed->indices.size_bytes():0));auto buffer=[device_ newBufferWithBytes:vertex_data length:vertex_bytes options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};
        id<MTLBuffer> indices=nil;if(fixed && !fixed->indices.empty()){indices=[device_ newBufferWithBytes:fixed->indices.data() length:fixed->indices.size_bytes() options:MTLResourceStorageModeShared];if(!indices)throw std::bad_alloc{};}
        auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=clearing?MTLCompareFunctionAlways:state_.enabled.contains(0xb71)?static_cast<MTLCompareFunction>(state_.depth_func-0x200):MTLCompareFunctionAlways;depth.depthWriteEnabled=clearing?((clear_mask&0x100) && state_.depth_write):(state_.enabled.contains(0xb71) && state_.depth_write);
        gl_stencil(depth,clearing,clear_mask);auto ds=cached_depth_state(depth);if(!ds)throw std::bad_alloc{};
        begin_commands();pending_leases_.push_back(std::move(lease));
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=render_color;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.depthAttachment.texture=render_depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.stencilAttachment.texture=render_depth.pixelFormat==MTLPixelFormatDepth32Float_Stencil8?render_depth:nil;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;
        if(fixed_targets_set_){pass.colorAttachments[0].level=fixed_color_.level;pass.colorAttachments[0].slice=fixed_color_.slice;graphics_pending_.push_back(fixed_color_.resource);if(fixed_depth_.resource){pass.depthAttachment.level=pass.stencilAttachment.level=fixed_depth_.level;pass.depthAttachment.slice=pass.stencilAttachment.slice=fixed_depth_.slice;graphics_pending_.push_back(fixed_depth_.resource);}}
        auto enc=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope enc_scope{enc};[enc setRenderPipelineState:p];[enc setDepthStencilState:ds];[enc setStencilReferenceValue:clearing?state_.stencil.clear:fixed_targets_set_?(fixed_stencil_[1]&255):state_.stencil.reference];[enc setScissorRect:clip];
        const auto& v=state_.viewport;[enc setViewport:clearing?MTLViewport{0,0,static_cast<double>(target_width),static_cast<double>(target_height),0,1}:MTLViewport{static_cast<double>(v[0]),static_cast<double>(target_height)-v[1]-v[3],static_cast<double>(v[2]),static_cast<double>(v[3]),state_.depth_range[0],state_.depth_range[1]}];
        [enc setCullMode:state_.enabled.contains(0xb44)&&!clearing?(state_.cull_face==0x404?MTLCullModeFront:MTLCullModeBack):MTLCullModeNone];[enc setFrontFacingWinding:state_.front_face==0x901?MTLWindingCounterClockwise:MTLWindingClockwise];
        if(fixed){[enc setVertexBuffer:buffer offset:0 atIndex:0];[enc setVertexBytes:fixed->transforms.data() length:sizeof(fixed->transforms) atIndex:1];[enc setVertexBytes:fixed->viewport_size.data() length:sizeof(fixed->viewport_size) atIndex:2];}
        else {
        const auto m=clearing?identity():multiply(matrices_[1].back(),matrices_[0].back());[enc setVertexBuffer:buffer offset:0 atIndex:0];[enc setVertexBytes:m.data() length:sizeof(m) atIndex:1];
        [enc setVertexBytes:matrices_[0].back().data() length:sizeof(Matrix) atIndex:2];
        const auto normal=normal_matrix(matrices_[0].back());[enc setVertexBytes:normal.data() length:sizeof(normal) atIndex:3];
        unsigned light_mask=0;for(unsigned i=0;i<8;++i)if(state_.enabled.contains(0x4000+i))light_mask|=1U<<i;
        const Vec lighting_flags{!clearing && state_.enabled.contains(0xb50)?1.F:0.F,state_.enabled.contains(0xba1)?1.F:0.F,static_cast<float>(light_mask),0};
        [enc setVertexBytes:state_.lights.data() length:sizeof(state_.lights) atIndex:4];[enc setVertexBytes:lighting_flags.data() length:sizeof(lighting_flags) atIndex:5];
        }
        const auto& fog=state_.fog;const std::array<Vec,2> fog_uniform{{fog.color,{fog.density,fog.start,fog.end,!clearing && state_.enabled.contains(0xb60)?static_cast<float>(fog.mode):0}}};
        [enc setFragmentBytes:fog_uniform.data() length:sizeof(fog_uniform) atIndex:1];
        unsigned enabled=0;auto& t=textures_[state_.binding];auto default_sampler=[MTLSamplerDescriptor new];[enc setFragmentTexture:textures_.at(UINT32_MAX).native atIndex:0];[enc setFragmentSamplerState:cached_sampler_state(default_sampler) atIndex:0];
        if(!clearing && state_.enabled.contains(0xde1)){
            // 依 MAX_LEVEL 檢查實際 mip 完整度；單層 D3D 紋理也能使用 mip filter。
            enabled=1;auto sampler=[MTLSamplerDescriptor new];sampler.maxAnisotropy=t.anisotropy;sampler.minFilter=t.min==0x2600 || t.min==0x2700 || t.min==0x2702?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;sampler.mipFilter=t.min<=0x2601?MTLSamplerMipFilterNotMipmapped:t.min<=0x2701?MTLSamplerMipFilterNearest:MTLSamplerMipFilterLinear;auto texture=fixed_targets_set_ && fixed_texture_.resource?metal_shader_texture(fixed_texture_):t.native;unsigned mip_levels=0;if(fixed_targets_set_ && fixed_texture_.resource){mip_levels=fixed_texture_.levels;graphics_pending_.push_back(fixed_texture_.resource);}else while(t.formats.contains(mip_levels))++mip_levels;sampler.lodMaxClamp=mip_levels?static_cast<float>(std::min(t.max_level,mip_levels-1)):0;sampler.magFilter=t.mag==0x2600?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;sampler.sAddressMode=address_mode(t.s);sampler.tAddressMode=address_mode(t.t);
            const unsigned required=texture?std::min(t.max_level,static_cast<unsigned>(texture.mipmapLevelCount-1))+1:0;id<MTLTexture> sampled=(texture && (t.min<=0x2601 || mip_levels>=required))?texture:textures_.at(UINT32_MAX).native;
            // MAX_LEVEL 限制資源可見的 mip 範圍，而不只是 sampler LOD；各向異性 footprint 不得讀到未定義層。
            if(sampled==texture && required<texture.mipmapLevelCount){sampled=[texture newTextureViewWithPixelFormat:texture.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0,required) slices:NSMakeRange(0,1)];if(!sampled)throw std::runtime_error("Metal mip view 建立失敗");}
            [enc setFragmentTexture:sampled atIndex:0];[enc setFragmentSamplerState:cached_sampler_state(sampler) atIndex:0];
        }
        auto color_op=state_.texture_color_op,alpha_op=state_.texture_alpha_op;const auto& environment=state_.environments[gl_.active_texture];
        if(environment.mode){const auto format=t.formats.contains(0)?t.formats.at(0):0;const bool alpha=format==4 || format==0x1908 || format==0x8058 || format==0x881a || format==0x8814;
            color_op=environment.mode==0x1e01?2:environment.mode==0x2101?5:environment.mode==0xbe2?6:environment.mode==0x104?7:4;
            alpha_op=environment.mode==0x2101?3:environment.mode==0x1e01?(alpha?2:3):4;}
        const std::array<Vec,2> fragment_state{{environment.color,{!clearing && state_.enabled.contains(0xbc0)?static_cast<float>(state_.alpha_func):0.F,state_.alpha_ref,0,0}}};
        [enc setFragmentBytes:fragment_state.data() length:sizeof(fragment_state) atIndex:2];
        if(!clearing && !environment.mode && state_.texture_combine_enabled)enabled|=2U;
        const std::array<unsigned,4> texture_mode{enabled,color_op,alpha_op,environment.mode?0U:state_.texture_arguments};[enc setFragmentBytes:texture_mode.data() length:sizeof(texture_mode) atIndex:0];if(indices)[enc drawIndexedPrimitives:primitive indexCount:fixed->indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:0];
        else [enc drawPrimitives:primitive vertexStart:0 vertexCount:fixed?fixed->vertices.size():vertices.size()];enc_scope.end();++draws_;if(++pending_>=128)flush(false);
    }
    void end_primitive(){
        std::vector<Vertex> expanded;MTLPrimitiveType type=MTLPrimitiveTypeTriangle;
        const auto add=[&](std::size_t i){expanded.push_back(vertices_[i]);};
        switch(primitive_){
        case 0:type=MTLPrimitiveTypePoint;expanded=vertices_;break;
        case 1:type=MTLPrimitiveTypeLine;expanded.assign(vertices_.begin(),vertices_.begin()+static_cast<std::ptrdiff_t>(vertices_.size()/2*2));break;
        case 2:case 3:type=MTLPrimitiveTypeLine;for(std::size_t i=1;i<vertices_.size();++i){add(i-1);add(i);}if(primitive_==2 && vertices_.size()>1){add(vertices_.size()-1);add(0);}break;
        case 4:expanded.assign(vertices_.begin(),vertices_.begin()+static_cast<std::ptrdiff_t>(vertices_.size()/3*3));break;
        case 5:for(std::size_t i=2;i<vertices_.size();++i){add(i%2?i-1:i-2);add(i%2?i-2:i-1);add(i);}break;
        case 6:case 9:for(std::size_t i=2;i<vertices_.size();++i){add(0);add(i-1);add(i);}break;
        case 7:for(std::size_t i=0;i+3<vertices_.size();i+=4){add(i);add(i+1);add(i+2);add(i);add(i+2);add(i+3);}break;
        case 8:for(std::size_t i=0;i+3<vertices_.size();i+=2){add(i);add(i+1);add(i+3);add(i);add(i+3);add(i+2);}break;
        default:error(0x500);break;
        }
        draw(expanded,type);vertices_.clear();
    }
    static unsigned unit(unsigned type){switch(type){case 0x1400:case 0x1401:return 1;case 0x1402:case 0x1403:return 2;case 0x1404:case 0x1405:case 0x1406:return 4;case 0x140a:return 8;default:throw std::invalid_argument("GL scalar type");}}
    static double number(std::span<const std::uint8_t> b,std::size_t at,unsigned type){const auto size=unit(type);if(at>b.size() || size>b.size()-at)throw std::invalid_argument("GL scalar bounds");std::uint64_t n=0;std::memcpy(&n,b.data()+at,size);switch(type){case 0x1400:return static_cast<std::int8_t>(n);case 0x1401:return static_cast<std::uint8_t>(n);case 0x1402:return static_cast<std::int16_t>(n);case 0x1403:return static_cast<std::uint16_t>(n);case 0x1404:return static_cast<std::int32_t>(n);case 0x1405:return static_cast<std::uint32_t>(n);case 0x1406:return std::bit_cast<float>(static_cast<std::uint32_t>(n));default:return std::bit_cast<double>(n);}}
    void vertex(Vec position){if(!begun_){error(0x502);return;}if(vertices_.size()>=1048576)throw std::bad_alloc{};Vec tex{};for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)tex[r]+=matrices_[2].back()[k*4+r]*state_.texcoord[k];const auto& material=state_.materials[0];vertices_.push_back({position,state_.color,tex,state_.normal,material.ambient,material.diffuse,material.specular,material.emission,material.shininess});}
    void track_color(){if(state_.enabled.contains(0xb57)){auto color=state_.color;for(auto& c:color)c=std::clamp(c,0.F,1.F);for(auto& material:state_.materials)material.ambient=material.diffuse=color;}}
    void array_vertex(unsigned i){
        for(unsigned index:{1U,2U,3U,0U}){const auto& a=arrays_[index];if(!a.enabled)continue;Vec value{0,0,0,1};for(int c=0;c<a.size;++c){auto n=number(a.bytes,(static_cast<std::size_t>(i)*static_cast<unsigned>(a.size)+static_cast<unsigned>(c))*unit(a.type),a.type);if((index==1 || index==2) && a.type!=0x1406 && a.type!=0x140a){const auto bits=unit(a.type)*8;const bool sign=(a.type&1)==0;n=std::max(-1.,n/(sign?std::pow(2.,bits-1)-1:std::pow(2.,bits)-1));}value[static_cast<unsigned>(c)]=static_cast<float>(n);}if(index==0)vertex(value);else if(index==1)state_.normal=value;else if(index==2){state_.color=value;track_color();}else state_.texcoord=value;}
    }
    std::pair<std::size_t,std::size_t> pixel_layout(unsigned width,unsigned components,bool pack){const auto base=pack?0xd00U:0xcf0U;const auto align=static_cast<unsigned>(pixel_[base+5]);const auto row=pixel_[base+2]?static_cast<unsigned>(pixel_[base+2]):width;const auto stride=(static_cast<std::size_t>(row)*components+align-1)/align*align;return {stride,static_cast<std::size_t>(pixel_[base+3])*stride+static_cast<std::size_t>(pixel_[base+4])*components};}
    void query(GlOperation op,unsigned pname,std::span<std::uint8_t> out){
        std::vector<double> values;const auto vector=[&](const auto& v){for(auto x:v)values.push_back(x);};
        if(pname==0x84ff)values={16};
        else if(pname==0x8b4d || pname==0x8872 || pname==0x8b4c)values={32};
        else if(pname==0x8869)values={16};else if(pname==0x8cdf || pname==0x8824)values={8};
        else if(pname==0x8b8d)values={static_cast<double>(gl_.program)};
        else if(pname==0x8894 || pname==0x8895 || pname==0x8f36 || pname==0x8f37 || pname==0x8a28){const auto buffer=gl_buffer(pname==0x8894?0x8892:pname==0x8895?0x8893:pname==0x8a28?0x8a11:pname);values={static_cast<double>(buffer?buffer->name:0)};}
        else if(pname==0x85b5)values={static_cast<double>(gl_.vertex_array)};
        else if(pname==0x84e0)values={static_cast<double>(0x84c0+gl_.active_texture)};
        else if(pname==0x821b)values={1};else if(pname==0x821c)values={1};else if(pname==0x821d)values={0};
        else if(pname>=0xba6 && pname<=0xba8)vector(matrices_[pname-0xba6].back());
        else if(pname==0xba2)vector(state_.viewport);else if(pname==0xc10)vector(state_.scissor);else if(pname==0xb00)vector(state_.color);else if(pname==0xb03)vector(state_.texcoord);else if(pname==0xc22)vector(state_.clear);
        else if(pname==0xc01)values={static_cast<double>(state_.draw_buffer)};else if(pname==0xc02)values={static_cast<double>(state_.read_buffer)};else if(pname==0xc32)values={1};else if(pname==0xb54)values={0x1d01};else if(pname==0xbe0)values={static_cast<double>(state_.blend_dst)};else if(pname==0xbe1)values={static_cast<double>(state_.blend_src)};
        else if(pname==0xba0)values={static_cast<double>(state_.matrix_mode)};else if(pname==0x8069)values={static_cast<double>(state_.binding)};
        else if(pname==0xb45)values={static_cast<double>(state_.cull_face)};else if(pname==0xb46)values={static_cast<double>(state_.front_face)};else if(pname==0xb70)vector(state_.depth_range);
        else if(pname==0xc23)values={state_.color_mask&1U?1.:0.,state_.color_mask&2U?1.:0.,state_.color_mask&4U?1.:0.,state_.color_mask&8U?1.:0.};
        else if(pname==0xb21)values={state_.line_width};else if(pname==0x846e || pname==0xb22)values={1,1};else if(pname==0xb23)values={0};
        else if(pname==0xd16)values={state_.pixel_zoom[0]};else if(pname==0xd17)values={state_.pixel_zoom[1]};else if(pname==0xc40)values={static_cast<double>(state_.render_mode)};
        else if(pname==0xb72)values={state_.depth_write?1.:0.};else if(pname==0xb73)values={state_.depth_clear};else if(pname==0xb74)values={static_cast<double>(state_.depth_func)};
        else if(pname>=0xb91 && pname<=0xb98){const auto& st=state_.stencil;const std::array values_by_name{st.clear,st.function,st.read_mask,st.fail,st.depth_fail,st.pass,st.reference,st.write_mask};values={static_cast<double>(std::bit_cast<std::int32_t>(values_by_name[pname-0xb91]))};}
        else if(pname==0xbc1)values={static_cast<double>(state_.alpha_func)};else if(pname==0xbc2)values={state_.alpha_ref};
        else if(pname==0xd33)values={16384};else if(pname==0xd50 || pname==0xd51 || pname==0xd52 || pname==0xd53)values={8};else if(pname==0xd56)values={32};else if(pname==0xd57)values={gl_.draw_framebuffer?0.:8.};
        else if(pname>=0xba3 && pname<=0xba5)values={static_cast<double>(matrices_[pname-0xba3].size())};
        else if(pname==0xd36)values={32};else if(pname==0xd38 || pname==0xd39)values={2};else if(pname==0xd3a)values={16384,16384};
        else if(pixel_.contains(pname))values={static_cast<double>(pixel_[pname])};
        else if(lighting_cap(pname) || pname==0xb71 || pname==0xb90 || pname==0xbc0 || pname==0xbe2 || pname==0xc11 || pname==0xde1 || pname==0xb44 || pname==0xb60)values={state_.enabled.contains(pname)?1.:0.};
        else if(pname==0xb02){values={state_.normal[0],state_.normal[1],state_.normal[2]};}
        else if(pname==0xd31)values={8};else if(pname==0xb53)values={.2,.2,.2,1};
        else if(pname==0xb51 || pname==0xb52)values={0};else if(pname==0xb55)values={0x408};else if(pname==0xb56)values={0x1602};
        else if(pname==0xb66)vector(state_.fog.color);
        else if(pname==0xb61)values={state_.fog.index};else if(pname==0xb62)values={state_.fog.density};
        else if(pname==0xb63)values={state_.fog.start};else if(pname==0xb64)values={state_.fog.end};
        else if(pname==0xb65)values={static_cast<double>(state_.fog.mode)};else if(pname==0xc54)values={static_cast<double>(state_.fog_hint)};else if(pname==0xc50)values={static_cast<double>(state_.perspective_hint)};
        else {error(0x500);return;}
        const unsigned size=op==GlOperation::GetDoublev?8:op==GlOperation::GetBooleanv?1:4;if(values.size()*size>out.size())throw std::invalid_argument("GL query bounds");
        for(std::size_t i=0;i<values.size();++i){const auto n=values[i];std::uint64_t value=0;if(op==GlOperation::GetDoublev)value=std::bit_cast<std::uint64_t>(n);else if(op==GlOperation::GetFloatv)value=std::bit_cast<std::uint32_t>(static_cast<float>(n));else if(op==GlOperation::GetBooleanv)value=n!=0;else {const auto scaled=(pname==0xb00 || pname==0xc22 || pname==0xb66 || pname==0xb53 || pname==0xb02 || pname==0xbc2)?n*2147483647.:n;const auto safe=std::isfinite(scaled)?std::clamp(std::round(scaled),-2147483648.,2147483647.):0.;value=static_cast<std::uint32_t>(static_cast<std::int32_t>(safe));}std::memcpy(out.data()+i*size,&value,size);}
    }
public:
    MetalGl(void* view,unsigned w,unsigned h){@autoreleasepool{
        if(const char* value=std::getenv("X64ARM_METAL_IN_FLIGHT")){char* end=nullptr;const auto count=std::strtoul(value,&end,10);if(!*value || *end || count<1 || count>8)throw std::invalid_argument("X64ARM_METAL_IN_FLIGHT 必須為 1–8");flight_limit_=count;}
        device_=MTLCreateSystemDefaultDevice();if(!device_)throw std::runtime_error("無可用 Metal GPU");queue_=[device_ newCommandQueue];NSError* e=nil;library_=fixed_metal_library(device_,&e);if(!library_ || !queue_)throw std::runtime_error(std::string("Metal GL shader: ")+(e?e.localizedDescription.UTF8String:"queue unavailable"));
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;Texture black;black.lease=reserve_gpu_memory(4);black.native=[device_ newTextureWithDescriptor:d];if(!black.native)throw std::bad_alloc{};const std::array<std::uint8_t,4> pixel{0,0,0,255};[black.native replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:pixel.data() bytesPerRow:4];textures_[UINT32_MAX]=std::move(black);
        pipeline(false);pipeline(true,false);
        drawable(view,w,h);
    }}
    ~MetalGl()override{end_open_render();if(commands_){if(commands_gpu_work_)track_gpu_commands(commands_);[commands_ commit];[commands_ waitUntilCompleted];}for(const auto& flight:flights_)[flight.command waitUntilCompleted];finish_immediate_present();if(std::getenv("X64ARM_METAL_IMMUTABLE_STATE_CACHE"))std::fprintf(stderr,"[metal-state-cache] enabled=%u depth_hits=%llu depth_builds=%llu sampler_hits=%llu sampler_builds=%llu waits=%llu resource_waits=%llu wait_ns=%llu\n",immutable_state_cache_enabled()?1U:0U,static_cast<unsigned long long>(depth_state_hits_),static_cast<unsigned long long>(depth_state_builds_),static_cast<unsigned long long>(sampler_state_hits_),static_cast<unsigned long long>(sampler_state_builds_),static_cast<unsigned long long>(wait_count_),static_cast<unsigned long long>(resource_wait_count_),static_cast<unsigned long long>(wait_ns_));if(std::getenv("X64ARM_METAL_CONSTANT_UPLOAD_SNAPSHOT"))std::fprintf(stderr,"[metal-constant-upload] enabled=%u skips=%llu saved_bytes=%llu\n",MetalResource::constant_snapshot_enabled()?1U:0U,static_cast<unsigned long long>(constant_upload_skips_),static_cast<unsigned long long>(constant_upload_saved_bytes_));if(std::getenv("X64ARM_METAL_STREAM_OUTPUT_GPU"))std::fprintf(stderr,"[metal-stream-output] draws=%llu bytes=%llu\n",static_cast<unsigned long long>(stream_gpu_draws_),static_cast<unsigned long long>(stream_gpu_bytes_));detach();}
    void bind()override{if(view_ && view_.layer!=layer_)view_.layer=layer_;}
    void drawable(void* raw,unsigned w,unsigned h)override{@autoreleasepool{
        if(!w || !h || w>16384 || h>16384 || std::uint64_t{w}*h>64*1024*1024)throw std::bad_alloc{};
        if(w!=width_ || h!=height_){flush(true);finish_immediate_present();auto lease=reserve_gpu_memory(static_cast<std::size_t>(w)*h*16);auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;auto color=[device_ newTextureWithDescriptor:d];auto front=[device_ newTextureWithDescriptor:d];d.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;auto depth=[device_ newTextureWithDescriptor:d];if(!color || !front || !depth)throw std::bad_alloc{};
            if(!width_){state_.viewport={0,0,static_cast<int>(w),static_cast<int>(h)};state_.scissor=state_.viewport;}width_=w;height_=h;surface_=std::move(lease);color_=color;front_=front;depth_=depth;
            begin_commands();auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=color_;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.depthAttachment.texture=depth_;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.clearDepth=1;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.stencilAttachment.texture=depth_;pass.stencilAttachment.loadAction=MTLLoadActionClear;pass.stencilAttachment.clearStencil=0;pass.stencilAttachment.storeAction=MTLStoreActionStore;auto enc=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope enc_scope{enc};enc_scope.end();
        }
        auto view=(__bridge NSView*)raw;if(view!=view_){finish_immediate_present();immediate_unavailable_=false;detach();if(view){view_=view;auto& use=metal_view_users[raw];if(!use.count){use.original=view.layer;use.wanted=view.wantsLayer;}++use.count;view.wantsLayer=YES;layer_=[CAMetalLayer layer];layer_.device=device_;layer_.pixelFormat=MTLPixelFormatBGRA8Unorm;layer_.framebufferOnly=NO;view.layer=layer_;if([view respondsToSelector:NSSelectorFromString(@"setOpenGLManaged:")])[view setValue:@YES forKey:@"openGLManaged"];}}
        if(layer_){layer_.drawableSize=CGSizeMake(w,h);layer_.displaySyncEnabled=interval_!=0;}bind();
    }}
    bool swap_interval(std::int32_t n)override{if(n<0 || n>1)return false;if(n)finish_immediate_present();interval_=n;if(layer_)layer_.displaySyncEnabled=n!=0;return true;}
    bool swap()override{@autoreleasepool{bind();begin_commands();auto front_copy=[commands_ blitCommandEncoder];MetalEncoderScope front_copy_scope{front_copy};[front_copy copyFromTexture:color_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1) toTexture:front_ destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];front_copy_scope.end();if(layer_ && !interval_ && immediate_present_enabled() && !immediate_unavailable_){
            if(!immediate_presenter_)try{immediate_presenter_=std::make_unique<MetalImmediatePresenter>(device_,layer_,width_,height_);}catch(const std::bad_alloc&){immediate_unavailable_=true;}
            if(immediate_presenter_){
                const auto index=immediate_presenter_->prepare();auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope scope{encoder};
                [encoder copyFromTexture:color_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                          sourceSize:MTLSizeMake(width_,height_,1) toTexture:immediate_presenter_->texture(index)
                    destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                scope.end();auto ready=commands_;flush(false);immediate_presenter_->publish(index,ready);return true;
            }
        }
        if(layer_){auto drawable=[layer_ nextDrawable];if(!drawable)return false;begin_commands();auto enc=[commands_ blitCommandEncoder];MetalEncoderScope enc_scope{enc};[enc copyFromTexture:color_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1) toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];enc_scope.end();[commands_ presentDrawable:drawable];}flush(false);return true;}}
    OpenGlRendererInfo info()const override{return {"MacWin",std::string(device_.name.UTF8String)+" (Metal)","1.1 MacWin Metal subset",true};}
    std::string string(unsigned name)override{const auto i=info();switch(name){case 0x1f00:return i.vendor;case 0x1f01:return i.renderer;case 0x1f02:return i.version;case 0x1f03:return {};case 0x8b8c:return "4.60 MacWin GLSL compiler";default:error(0x500);return {};}}
    std::string statistics()const override{return "opengl_backend=metal opengl_metal_draws="+std::to_string(draws_)+" opengl_metal_submissions="+std::to_string(submissions_)+" opengl_metal_inflight="+std::to_string(flights_.size())+" opengl_metal_inflight_limit="+std::to_string(flight_limit_)+" opengl_metal_inflight_peak="+std::to_string(flight_peak_)+" opengl_metal_render_encoders="+std::to_string(render_encoders_)+" opengl_metal_encoder_breaks="+[&]{std::string text;for(const auto& [why,count]:encoder_breaks_)text+=why+":"+std::to_string(count)+",";return text.empty()?std::string("none"):text;}()+" opengl_metal_coalesced_draws="+std::to_string(coalesced_draws_)+" opengl_metal_gpu_busy_ns="+std::to_string(gpu_busy_ns_->load(std::memory_order_relaxed))+" opengl_metal_waits="+std::to_string(wait_count_)+" opengl_metal_wait_ns="+std::to_string(wait_ns_)+" glsl_compiles="+std::to_string(gl_.compiles)+" glsl_links="+std::to_string(gl_.links)+" glsl_compile_ns="+std::to_string(gl_.compile_ns)+" glsl_link_ns="+std::to_string(gl_.link_ns)+" opengl_metal_resource_waits="+std::to_string(resource_wait_count_)+" gl_buffer_uploads="+std::to_string(gl_.buffer_uploads)+" gl_buffer_upload_bytes="+std::to_string(gl_.buffer_upload_bytes)+" gl_direct_vertex_bindings="+std::to_string(gl_.direct_vertex_bindings)+" gl_direct_index_draws="+std::to_string(gl_.direct_index_draws)+" gl_converted_vertices="+std::to_string(gl_.converted_vertices)+"\n";}
    void array(unsigned index,int size,unsigned type,std::span<const std::uint8_t> bytes,bool enabled)override{if(index>=4 || size<1 || size>4)throw std::invalid_argument("GL array");auto& a=arrays_[index];a.size=size;a.type=type;a.enabled=enabled;a.bytes.assign(bytes.begin(),bytes.end());}
    std::string capture(const std::filesystem::path& path)override{@autoreleasepool{flush(true);std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width_)*height_*4);[color_ getBytes:pixels.data() bytesPerRow:width_*4 fromRegion:MTLRegionMake2D(0,0,width_,height_) mipmapLevel:0];auto bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:width_ pixelsHigh:height_ bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width_*4 bitsPerPixel:32];if(!bitmap)throw std::bad_alloc{};for(std::size_t i=0;i<pixels.size();i+=4){std::swap(pixels[i],pixels[i+2]);}std::memcpy(bitmap.bitmapData,pixels.data(),pixels.size());if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());if(![[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES])throw std::runtime_error("Metal PNG capture failed");return "opengl_capture="+path.string()+" backend=metal\n";}}
    bool programmable()const noexcept override{return true;}
    OpenGlShareResult share_objects(NativeOpenGlContext& other)override{
        auto* source=dynamic_cast<MetalGl*>(&other);if(!source || source->device_!=device_)return OpenGlShareResult::Unsupported;
        if(textures_.same_group(source->textures_))return OpenGlShareResult::Shared;
        if(!textures_.unused() || !lists_.unused() || !gl_.shaders.unused() || !gl_.programs.unused() || !gl_.buffers.unused() || compiling_)return OpenGlShareResult::Busy;
        // 共用物件改由同一個 command queue 提交，跨 context 的上傳與取樣維持提交順序。
        flush(true);source->flush(false);queue_=source->queue_;
        textures_.join(source->textures_);lists_.join(source->lists_);gl_.shaders.join(source->gl_.shaders);gl_.programs.join(source->gl_.programs);gl_.buffers.join(source->gl_.buffers);
        next_list_.join(source->next_list_);next_texture_.join(source->next_texture_);gl_.next_object.join(source->gl_.next_object);gl_.next_buffer.join(source->gl_.next_buffer);
        return OpenGlShareResult::Shared;
    }
    std::shared_ptr<OpenGlBuffer> buffer_object(std::uint32_t name)override{const auto it=gl_.buffers.find(name);return it==gl_.buffers.end()?nullptr:it->second;}
    std::shared_ptr<OpenGlBuffer> bound_buffer(std::uint32_t target)override{return gl_buffer(target);}
    bool fixed_vertex_supported()const noexcept override{return true;}
    void graphics_fixed_targets(const GraphicsView& color,const GraphicsView& depth)override{if(!color.resource || !metal_resource(color.resource).texture)throw std::invalid_argument("固定管線需要色彩繪製表面");fixed_color_=color;fixed_depth_=depth;fixed_targets_set_=true;}
    bool graphics_fixed_texture(const GraphicsView& view)override{if(view.resource && (view.resource->desc.dimension!=3 || view.resource->desc.layers!=1 || view.resource->desc.samples!=1))return false;fixed_texture_=view;return true;}
    void graphics_fixed_stencil(const std::array<std::uint32_t,12>& state)override{fixed_stencil_=state;}
    void graphics_fixed_draw(const FixedGraphicsDraw& input)override{@autoreleasepool{
        if(input.topology<1 || input.topology>6 || input.viewport_size[0]<=0 || input.viewport_size[1]<=0)throw std::invalid_argument("固定管線圖元／viewport 不合法");
        for(auto index:input.indices)if(index>=input.vertices.size())throw std::invalid_argument("固定管線索引越界");
        auto fixed=input;std::vector<std::uint32_t> fan;
        const auto count=input.indices.empty()?input.vertices.size():input.indices.size();
        if(input.topology==6){if(count<3)return;fan.reserve((count-2)*3);const auto index=[&](std::size_t n){return input.indices.empty()?static_cast<std::uint32_t>(n):input.indices[n];};for(std::size_t n=2;n<count;++n){fan.push_back(index(0));fan.push_back(index(n-1));fan.push_back(index(n));}fixed.indices=fan;}
        const std::array types{MTLPrimitiveTypePoint,MTLPrimitiveTypeLine,MTLPrimitiveTypeLineStrip,MTLPrimitiveTypeTriangle,MTLPrimitiveTypeTriangleStrip,MTLPrimitiveTypeTriangle};
        draw({},types[input.topology-1],false,0,&fixed);
    }}
    std::shared_ptr<GraphicsResource> graphics_resource(const GraphicsResourceDesc&)override;
    GraphicsView graphics_drawable(bool depth)override{
        auto resource=std::make_shared<MetalResource>();resource->texture=depth?depth_:color_;resource->lease=surface_;
        auto& desc=resource->desc;desc.dimension=3;desc.width=width_;desc.height=height_;desc.format=depth?20:87;desc.bind=depth?0x40:0x28;
        return GraphicsView{resource,desc.format};
    }
    bool graphics_samples_supported(std::uint32_t,std::uint32_t)override;
    bool graphics_vertex_format_supported(std::uint32_t)override;
    std::shared_ptr<GraphicsShader> graphics_shader(std::uint32_t,std::span<const std::uint8_t>)override;
    std::shared_ptr<GraphicsShader> graphics_compute_source(const GraphicsModernShader&)override;
    std::shared_ptr<GraphicsSampler> graphics_sampler(std::span<const std::uint8_t>)override;
    bool graphics_timestamp_supported()const override;
    GraphicsTimestamp graphics_timestamp()override;
    std::shared_ptr<const GraphicsTimestampFuture> graphics_timestamp_future()override;
    void graphics_write(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t)override;
    void graphics_write_discard(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t)override;
    void graphics_write_no_overwrite(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t)override;
    void graphics_write_region(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t,std::array<std::uint32_t,6>)override;
    std::vector<std::uint8_t> graphics_read(const GraphicsView&)override;
    void graphics_copy(const GraphicsView&,const GraphicsView&,std::array<std::uint32_t,3>,std::array<std::uint32_t,6>)override;
    void graphics_resolve(const GraphicsView&,const GraphicsView&,std::uint32_t)override;
    void graphics_clear(const GraphicsView&,std::array<float,4>,std::uint32_t)override;
    bool graphics_clear_unordered(const GraphicsView&,std::array<std::uint32_t,4>,bool)override;
    void graphics_draw(const GraphicsDraw&)override;
    void graphics_dispatch(const GraphicsStage&,std::array<std::uint32_t,3>)override;
    void graphics_dispatch_indirect(const GraphicsStage&,const std::shared_ptr<GraphicsResource>&,std::uint32_t)override;
    void graphics_handoff(NativeGraphics&,const GraphicsView&)override;
    void graphics_mips(const GraphicsView&)override;
    void graphics_present(const GraphicsView&)override;
    void graphics_capture(const GraphicsDraw&);
    void graphics_capture_present(const GraphicsView&);
    void graphics_export_frame(const GraphicsView&,void*,std::size_t,std::uint32_t,std::shared_ptr<void>)override;
    void graphics_flush(bool wait)override{flush(wait);}
    // 有待提交工作就提交；回傳最新 flight 的完成旗標（同一 queue 依序完成，涵蓋先前所有提交）。
    // 不為 fence 建立空命令緩衝：那會占用 flight 名額，上限一到 flush 就同步等待，與原本等 GPU 閒置無異。
    // 查詢 End 不再逐次 commit：回傳目前 command buffer 的完成旗標，等 Present／GetData 或 flight 額度才提交。
    // 命令執行緒的中繼 fence：語義同 graphics_fence／graphics_fence_pending，只是旗標由呼叫端先建立。
    void fence_relay(std::shared_ptr<std::atomic<bool>> relay,bool flush_first){
        if(flush_first && commands_)flush(false);else reap();
        if(commands_){if(!commands_relays_)commands_relays_=std::make_shared<MetalFenceRelays>();commands_relays_->attach(std::move(relay));return;}
        if(flights_.empty() || !flights_.back().relays){relay->store(true,std::memory_order_release);return;}
        flights_.back().relays->attach(std::move(relay));
    }
    std::shared_ptr<const std::atomic<bool>> graphics_fence_pending()override{
        reap();
        if(commands_){if(!commands_done_)commands_done_=std::make_shared<std::atomic<bool>>(false);return commands_done_;}
        if(flights_.empty())return std::make_shared<std::atomic<bool>>(true);
        return flights_.back().done;
    }
    std::shared_ptr<const std::atomic<bool>> graphics_fence()override{@autoreleasepool{
        static const bool trace=std::getenv("X64ARM_TRACE_GRAPHICS_STATE")!=nullptr;
        const auto begin=trace?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const bool pending=bool(commands_);
        if(commands_)flush(false);else reap();
        if(trace){static std::uint64_t calls=0,pending_calls=0,ns=0;++calls;pending_calls+=pending;ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
            if(calls%500==1)std::fprintf(stderr,"[metal_fence] calls=%llu pending=%llu avg_us=%llu inflight=%zu\n",static_cast<unsigned long long>(calls),static_cast<unsigned long long>(pending_calls),static_cast<unsigned long long>(ns/calls/1000),flights_.size());}
        if(flights_.empty())return std::make_shared<std::atomic<bool>>(true);
        return flights_.back().done;
    }}
    void graphics_bind(id<MTLRenderCommandEncoder>,id<MTLComputeCommandEncoder>,const GraphicsStage&,bool);
    MetalGeometryDraw graphics_geometry(const GraphicsDraw&,MTLVertexDescriptor*,const std::array<int,32>&,const std::vector<std::array<std::uint32_t,3>>&,const std::string&);
    bool graphics_stream_output(const GraphicsDraw&,const MetalShader&,id<MTLBuffer>,unsigned,unsigned,unsigned);
    std::shared_ptr<GraphicsShader> graphics_translate_shader(std::uint32_t,std::span<const std::uint8_t>,GLSLCrossDependencyData*);
    MetalTessellationDraw graphics_tessellation(const GraphicsDraw&,const std::string&);
    std::uint64_t execute(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output)override;
};
#include "metal_gl_vertex_buffers.inc"
#include "metal_gl_program.inc"
#include "metal_gl_texture_environment.inc"
#include "metal_gl_stencil.inc"
#include "metal_geometry.inc"
#include "metal_loop_compaction.inc"
#include "metal_programmable.inc"
#include "metal_timestamp.inc"
#include "metal_graphics_capture.inc"
std::uint64_t MetalGl::execute(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){@autoreleasepool{
    active_operation_=op;
    const bool current_attribute=op==GlOperation::VertexAttrib1f || op==GlOperation::VertexAttrib2f || op==GlOperation::VertexAttrib3f || op==GlOperation::VertexAttrib4f;
    if(begun_ && op>=GlOperation::CreateShader && !current_attribute){error(0x502);return 0;}
    if(const auto result=programmable_execute(op,a,data,output))return *result;
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};const auto n=[&](unsigned i){return static_cast<std::int32_t>(u(i));};const auto f=[&](unsigned i){return std::bit_cast<float>(u(i));};const auto d=[&](unsigned i){return std::bit_cast<double>(a[i]);};
    const auto vector=[&](unsigned count,bool wide=false){Vec value{0,0,0,1};for(unsigned i=0;i<count;++i)value[i]=static_cast<float>(number(data,i*(wide?8:4),wide?0x140a:0x1406));return value;};
    if(op==GlOperation::GetError){if(begun_){error(0x502);return 0;}const auto e=error_;error_=0;return e;}
    if(compiling_ && !replaying_ && op!=GlOperation::IsTexture && op!=GlOperation::GetTexLevelParameteriv && op!=GlOperation::GetTexLevelParameterfv && !(op==GlOperation::TexImage2D && u(0)==0x8064) && op!=GlOperation::EndList && op!=GlOperation::NewList && op!=GlOperation::GenLists && op!=GlOperation::DeleteLists && op!=GlOperation::GenTextures && op!=GlOperation::DeleteTextures && op!=GlOperation::GetIntegerv && op!=GlOperation::GetFloatv && op!=GlOperation::GetDoublev && op!=GlOperation::GetBooleanv && op!=GlOperation::IsEnabled && op!=GlOperation::GetTexEnviv && op!=GlOperation::GetTexEnvfv && op!=GlOperation::ReadPixels && op!=GlOperation::PixelStorei && op!=GlOperation::PushClientAttrib && op!=GlOperation::PopClientAttrib){
        if(op==GlOperation::TexImage2D || op==GlOperation::TexSubImage2D || op==GlOperation::DrawArrays || op==GlOperation::DrawElements){error(0x502);return 0;}
        const auto bytes=sizeof(Command)+a.size_bytes()+data.size();if(list_bytes_+bytes>32*1024*1024)throw std::bad_alloc{};
        lists_[compiling_].push_back({op,{a.begin(),a.end()},{data.begin(),data.end()}});list_bytes_+=bytes;if(compile_mode_==0x1300)return 0;
    }
    const bool attribute=op==GlOperation::Materialf || op==GlOperation::Materialfv || op==GlOperation::Color3f || op==GlOperation::Color4f || op==GlOperation::Color4fv || op==GlOperation::Color4dv || op==GlOperation::TexCoord2f || op==GlOperation::TexCoord2fv || op==GlOperation::TexCoord4fv || op==GlOperation::TexCoord4dv || op==GlOperation::Normal3f || op==GlOperation::Normal3fv || op==GlOperation::Normal3dv;
    const bool position=op==GlOperation::Vertex2f || op==GlOperation::Vertex2i || op==GlOperation::Vertex3f || op==GlOperation::Vertex3fv || op==GlOperation::Vertex4fv || op==GlOperation::Vertex4dv;
    if(begun_ && !attribute && !position && op!=GlOperation::End && op!=GlOperation::CallList){error(0x502);return 0;}
    try {switch(op){
    case GlOperation::Begin:if(u(0)>9)error(0x500);else {begun_=true;primitive_=u(0);vertices_.clear();}break;
    case GlOperation::End:if(!begun_)error(0x502);else {begun_=false;end_primitive();}break;
    case GlOperation::Vertex2f:vertex({f(0),f(1),0,1});break;case GlOperation::Vertex2i:vertex({static_cast<float>(n(0)),static_cast<float>(n(1)),0,1});break;
    case GlOperation::Vertex3f:vertex({f(0),f(1),f(2),1});break;case GlOperation::Vertex3fv:vertex(vector(3));break;case GlOperation::Vertex4fv:vertex(vector(4));break;case GlOperation::Vertex4dv:vertex(vector(4,true));break;
    case GlOperation::Color3f:state_.color={f(0),f(1),f(2),1};break;case GlOperation::Color4f:state_.color={f(0),f(1),f(2),f(3)};break;case GlOperation::Color4fv:state_.color=vector(4);break;case GlOperation::Color4dv:state_.color=vector(4,true);break;
    case GlOperation::TexCoord2f:state_.texcoord={f(0),f(1),0,1};break;case GlOperation::TexCoord2fv:state_.texcoord=vector(2);break;case GlOperation::TexCoord4fv:state_.texcoord=vector(4);break;case GlOperation::TexCoord4dv:state_.texcoord=vector(4,true);break;
    case GlOperation::Normal3f:state_.normal={f(0),f(1),f(2),0};break;case GlOperation::Normal3fv:state_.normal=vector(3);break;case GlOperation::Normal3dv:state_.normal=vector(3,true);break;
    case GlOperation::ClearColor:for(unsigned i=0;i<4;++i)state_.clear[i]=std::clamp(f(i),0.0F,1.0F);break;
    case GlOperation::DepthRange:state_.depth_range={std::clamp(d(0),0.,1.),std::clamp(d(1),0.,1.)};break;
    case GlOperation::ClearDepth:state_.depth_clear=std::clamp(d(0),0.,1.);break;
    case GlOperation::Clear:{if(u(0)&~0x4500U){error(0x501);break;}if(!u(0))break;const auto z=static_cast<float>(state_.depth_clear*2-1);const std::array<Vertex,3> v{{{{-1,-1,z,1},state_.clear,{0,0,0,1}},{{3,-1,z,1},state_.clear,{0,0,0,1}},{{-1,3,z,1},state_.clear,{0,0,0,1}}}};draw(v,MTLPrimitiveTypeTriangle,true,u(0));break;}
    case GlOperation::CullFace:if(u(0)!=0x404 && u(0)!=0x405 && u(0)!=0x408)error(0x500);else state_.cull_face=u(0);break;
    case GlOperation::FrontFace:if(u(0)!=0x900 && u(0)!=0x901)error(0x500);else state_.front_face=u(0);break;
    case GlOperation::Enable:case GlOperation::Disable:{const auto cap=u(0);if(cap!=0xb71 && cap!=0xb90 && cap!=0xbc0 && cap!=0xbe2 && cap!=0xc11 && cap!=0xde1 && cap!=0xb44 && cap!=0xb60 && !lighting_cap(cap)){error(0x500);break;}if(op==GlOperation::Enable){state_.enabled.insert(cap);if(cap==0xb57)track_color();}else state_.enabled.erase(cap);break;}
    case GlOperation::IsEnabled:if(u(0)!=0xb71 && u(0)!=0xb90 && u(0)!=0xbc0 && u(0)!=0xbe2 && u(0)!=0xc11 && u(0)!=0xde1 && u(0)!=0xb44 && u(0)!=0xb60 && !lighting_cap(u(0))){error(0x500);return 0;}return state_.enabled.contains(u(0));
    case GlOperation::BlendFunc:if(!factor(u(0)) || !factor(u(1)) || u(1)==0x308)error(0x500);else {state_.blend_src=u(0);state_.blend_dst=u(1);}break;
    case GlOperation::StencilFunc:if(u(0)<0x200 || u(0)>0x207)error(0x500);else {state_.stencil.function=u(0);state_.stencil.reference=static_cast<unsigned>(std::clamp(n(1),0,255));state_.stencil.read_mask=u(2);}break;
    case GlOperation::StencilMask:state_.stencil.write_mask=u(0);break;
    case GlOperation::StencilOp:if(!gl_stencil_operation(u(0)) || !gl_stencil_operation(u(1)) || !gl_stencil_operation(u(2)))error(0x500);else {state_.stencil.fail=u(0);state_.stencil.depth_fail=u(1);state_.stencil.pass=u(2);}break;
    case GlOperation::ClearStencil:state_.stencil.clear=u(0)&255U;break;
    case GlOperation::AlphaFunc:if(u(0)<0x200 || u(0)>0x207)error(0x500);else {state_.alpha_func=u(0);state_.alpha_ref=std::clamp(f(1),0.F,1.F);}break;
    case GlOperation::DepthFunc:if(u(0)<0x200 || u(0)>0x207)error(0x500);else state_.depth_func=u(0);break;
    case GlOperation::DepthMask:state_.depth_write=u(0)!=0;break;
    case GlOperation::ColorMask:state_.color_mask=(u(0)?1U:0U)|(u(1)?2U:0U)|(u(2)?4U:0U)|(u(3)?8U:0U);break;
    case GlOperation::LineWidth:if(!(f(0)>0))error(0x501);else state_.line_width=f(0);break;
    case GlOperation::PixelZoom:state_.pixel_zoom={f(0),f(1)};break;
    case GlOperation::RenderMode:{
        // 只有 GL_RENDER 可用：沒有 glSelectBuffer／glFeedbackBuffer，進入 SELECT／FEEDBACK 依規格回報 INVALID_OPERATION。
        if(u(0)<0x1c00 || u(0)>0x1c02){error(0x500);return 0;}if(u(0)!=0x1c00){error(0x502);return 0;}state_.render_mode=u(0);return 0;}
    case GlOperation::ShadeModel:if(u(0)!=0x1d01)error(u(0)==0x1d00?0x502:0x500);break;
    case GlOperation::Hint:if(u(1)<0x1100 || u(1)>0x1102)error(0x500);else if(u(0)==0xc54)state_.fog_hint=u(1);else if(u(0)==0xc50)state_.perspective_hint=u(1);else error(0x500);break;
    case GlOperation::Lightfv:{
        if(u(0)<0x4000 || u(0)>0x4007){error(0x500);break;}auto& light=state_.lights[u(0)-0x4000];
        switch(u(1)){
        case 0x1200:light.ambient=vector(4);break;case 0x1201:light.diffuse=vector(4);break;case 0x1202:light.specular=vector(4);break;
        case 0x1203:light.position=transform(matrices_[0].back(),vector(4));break;
        case 0x1204:{auto direction=vector(3);direction[3]=0;light.direction=transform(normal_matrix(matrices_[0].back()),direction);break;}
        case 0x1205:{const auto value=vector(1)[0];if(value<0 || value>128)error(0x501);else light.spot[0]=value;break;}
        case 0x1206:{const auto value=vector(1)[0];if(value!=180 && (value<0 || value>90))error(0x501);else {light.spot[2]=value;light.spot[1]=value==180?-1:std::cos(value*0.017453292519943295F);}break;}
        case 0x1207:case 0x1208:case 0x1209:{const auto value=vector(1)[0];if(value<0)error(0x501);else light.attenuation[u(1)-0x1207]=value;break;}
        default:error(0x500);break;}break;
    }
    case GlOperation::Materialf:case GlOperation::Materialfv:{
        if(u(0)!=0x404 && u(0)!=0x405 && u(0)!=0x408){error(0x500);break;}
        if(op==GlOperation::Materialf && u(1)!=0x1601){error(0x500);break;}
        const unsigned name=u(1);if(name!=0x1200 && name!=0x1201 && name!=0x1202 && name!=0x1600 && name!=0x1601 && name!=0x1602 && name!=0x1603){error(0x500);break;}
        const auto value=op==GlOperation::Materialf?Vec{f(2),0,0,0}:vector(name==0x1601?1:name==0x1603?3:4);
        if(name==0x1601 && (value[0]<0 || value[0]>128)){error(0x501);break;}
        for(unsigned face=0;face<2;++face){if(u(0)!=0x408 && u(0)!=0x404+face)continue;auto& material=state_.materials[face];
            if(state_.enabled.contains(0xb57) && (name==0x1200 || name==0x1201 || name==0x1602))continue;
            switch(name){case 0x1200:material.ambient=value;break;case 0x1201:material.diffuse=value;break;case 0x1202:material.specular=value;break;
            case 0x1600:material.emission=value;break;case 0x1601:material.shininess=value;break;case 0x1602:material.ambient=material.diffuse=value;break;case 0x1603:material.indexes=value;break;}
        }break;
    }
    case GlOperation::Fogf:case GlOperation::Fogi:case GlOperation::Fogfv:{
        if(u(0)==0xb66){if(op!=GlOperation::Fogfv){error(0x500);break;}auto color=vector(4);for(auto& c:color)c=std::clamp(c,0.F,1.F);state_.fog.color=color;break;}
        const float value=op==GlOperation::Fogfv?vector(1)[0]:op==GlOperation::Fogi?static_cast<float>(n(1)):f(1);
        switch(u(0)){
        case 0xb61:state_.fog.index=value;break;
        case 0xb62:if(value<0)error(0x501);else state_.fog.density=value;break;
        case 0xb63:state_.fog.start=value;break;case 0xb64:state_.fog.end=value;break;
        case 0xb65:if(value!=0x800 && value!=0x801 && value!=0x2601)error(0x500);else state_.fog.mode=static_cast<unsigned>(value);break;
        default:error(0x500);break;}break;
    }
    case GlOperation::Viewport:case GlOperation::Scissor:if(n(2)<0 || n(3)<0)error(0x501);else {auto& rect=op==GlOperation::Viewport?state_.viewport:state_.scissor;rect={n(0),n(1),op==GlOperation::Viewport?std::min(n(2),16384):n(2),op==GlOperation::Viewport?std::min(n(3),16384):n(3)};}break;
    case GlOperation::MatrixMode:if(u(0)<0x1700 || u(0)>0x1702)error(0x500);else state_.matrix_mode=u(0);break;
    case GlOperation::LoadIdentity:matrix()=identity();break;
    case GlOperation::LoadMatrixf:case GlOperation::MultMatrixf:{if(data.size()!=64)throw std::invalid_argument("matrix bounds");Matrix m;std::memcpy(m.data(),data.data(),64);matrix()=op==GlOperation::LoadMatrixf?m:multiply(matrix(),m);break;}
    case GlOperation::PushMatrix:{auto& stack=matrices_[state_.matrix_mode-0x1700];if(stack.size()>=(state_.matrix_mode==0x1700?32U:2U))error(0x503);else stack.push_back(stack.back());break;}
    case GlOperation::PopMatrix:{auto& stack=matrices_[state_.matrix_mode-0x1700];if(stack.size()==1)error(0x504);else stack.pop_back();break;}
    case GlOperation::Translatef:{auto m=identity();m[12]=f(0);m[13]=f(1);m[14]=f(2);matrix()=multiply(matrix(),m);break;}
    case GlOperation::Scalef:{auto m=identity();m[0]=f(0);m[5]=f(1);m[10]=f(2);matrix()=multiply(matrix(),m);break;}
    case GlOperation::Rotatef:{const auto length=std::sqrt(f(1)*f(1)+f(2)*f(2)+f(3)*f(3));if(length==0)break;const float x=f(1)/length,y=f(2)/length,z=f(3)/length,t=f(0)*0.017453292519943295F,c=std::cos(t),s=std::sin(t),q=1-c;Matrix m{x*x*q+c,y*x*q+z*s,x*z*q-y*s,0,x*y*q-z*s,y*y*q+c,y*z*q+x*s,0,x*z*q+y*s,y*z*q-x*s,z*z*q+c,0,0,0,0,1};matrix()=multiply(matrix(),m);break;}
    case GlOperation::Ortho:case GlOperation::Frustum:case GlOperation::Perspective:{double l,r,b,t,near,far;if(op==GlOperation::Perspective){near=d(2);far=d(3);t=near*std::tan(d(0)*3.141592653589793/360);b=-t;r=t*d(1);l=-r;}else {l=d(0);r=d(1);b=d(2);t=d(3);near=d(4);far=d(5);}if(l==r || b==t || near==far || (op!=GlOperation::Ortho && (near<=0 || far<=0))){error(0x501);break;}Matrix m{};if(op==GlOperation::Ortho){m[0]=static_cast<float>(2/(r-l));m[5]=static_cast<float>(2/(t-b));m[10]=static_cast<float>(-2/(far-near));m[12]=static_cast<float>(-(r+l)/(r-l));m[13]=static_cast<float>(-(t+b)/(t-b));m[14]=static_cast<float>(-(far+near)/(far-near));m[15]=1;}else {m[0]=static_cast<float>(2*near/(r-l));m[5]=static_cast<float>(2*near/(t-b));m[8]=static_cast<float>((r+l)/(r-l));m[9]=static_cast<float>((t+b)/(t-b));m[10]=static_cast<float>(-(far+near)/(far-near));m[11]=-1;m[14]=static_cast<float>(-2*far*near/(far-near));}matrix()=multiply(matrix(),m);break;}
    case GlOperation::BindTexture:if(u(0)!=0xde1 || u(1)==UINT32_MAX)error(0x500);else {if(!textures_.contains(u(1)) && textures_.size()>=4096)throw std::bad_alloc{};textures_.try_emplace(u(1));textures_.at(u(1)).created=true;state_.binding=u(1);}break;
    case GlOperation::TexEnvi:case GlOperation::TexEnvf:case GlOperation::TexEnviv:case GlOperation::TexEnvfv:case GlOperation::GetTexEnviv:case GlOperation::GetTexEnvfv:texture_environment(op,a,data,output);break;
    case GlOperation::TextureCombine:if(u(0)<2 || u(0)>4 || u(1)<2 || u(1)>4)error(0x500);else{state_.texture_color_op=u(0);state_.texture_alpha_op=u(1);state_.texture_arguments=a.size()>2?u(2):(0x80000000U|2U|(1U<<6)|(2U<<12)|(1U<<18));state_.texture_combine_enabled=a.size()>3 && u(3);state_.environments[gl_.active_texture].mode=0;}break;
    case GlOperation::IsTexture:{const auto found=textures_.find(u(0));return u(0)!=0 && u(0)!=UINT32_MAX && found!=textures_.end() && found->second.created;}
    case GlOperation::GenTextures:if(n(0)<0 || static_cast<std::size_t>(u(0))*4>output.size())error(0x501);else if(u(0)>4096-textures_.size())throw std::bad_alloc{};else for(unsigned i=0;i<u(0);++i){while(textures_.contains(next_texture_) && next_texture_!=UINT32_MAX)++next_texture_;if(next_texture_==UINT32_MAX)throw std::bad_alloc{};const auto name=next_texture_++;textures_.try_emplace(name);std::memcpy(output.data()+i*4,&name,4);}break;
    case GlOperation::DeleteTextures:if(n(0)<0 || static_cast<std::size_t>(u(0))*4>data.size())error(0x501);else {flush(true);for(unsigned i=0;i<u(0);++i){unsigned id;std::memcpy(&id,data.data()+i*4,4);if(id && id!=UINT32_MAX){textures_.erase(id);for(auto& unit:gl_.texture_units)if(unit==id)unit=0;if(state_.binding==id)state_.binding=0;}}}break;
    case GlOperation::TexParameteri:case GlOperation::TexParameterf:{if(u(1)==0x84fe){const auto value=op==GlOperation::TexParameterf?f(2):static_cast<float>(n(2));if(u(0)!=0xde1)error(0x500);else if(!std::isfinite(value) || value<1)error(0x501);else textures_[state_.binding].anisotropy=static_cast<unsigned>(std::clamp(value,1.F,16.F));break;}if(op==GlOperation::TexParameterf && (!std::isfinite(f(2)) || f(2)<0 || f(2)>65535)){error(0x500);break;}const auto value=op==GlOperation::TexParameteri?u(2):static_cast<unsigned>(f(2));if(u(0)!=0xde1){error(0x500);break;}auto& t=textures_[state_.binding];if(u(1)==0x813d){if(value>INT32_MAX)error(0x501);else t.max_level=value;}else if(u(1)==0x2801){if(value!=0x2600 && value!=0x2601 && (value<0x2700 || value>0x2703))error(0x500);else t.min=value;}else if(u(1)==0x2800){if(value!=0x2600 && value!=0x2601)error(0x500);else t.mag=value;}else if(u(1)==0x2802 || u(1)==0x2803){if(value!=0x2901 && value!=0x812f && value!=0x812d && value!=0x8370)error(0x500);else (u(1)==0x2802?t.s:t.t)=value;}else error(0x500);break;}
    case GlOperation::GetTexLevelParameteriv:case GlOperation::GetTexLevelParameterfv:texture_level(op,a,output);break;
    case GlOperation::TexImage2D:case GlOperation::TexSubImage2D:gl_texture(a,data,op==GlOperation::TexSubImage2D);break;
    case GlOperation::PixelStorei:if((u(0)<0xcf0 || u(0)>0xcf5) && (u(0)<0xd00 || u(0)>0xd05))error(0x500);else if(n(1)<0 || ((u(0)&15)==5 && u(1)!=1 && u(1)!=2 && u(1)!=4 && u(1)!=8))error(0x501);else pixel_[u(0)]=(u(0)&15)<=1?(u(1)?1:0):n(1);break;
    case GlOperation::PushClientAttrib:if(pixel_stack_.size()>=16)error(0x503);else pixel_stack_.push_back((u(0)&1)?pixel_:std::map<unsigned,int>{});break;
    case GlOperation::PopClientAttrib:if(pixel_stack_.empty())error(0x504);else {if(!pixel_stack_.back().empty())pixel_=pixel_stack_.back();pixel_stack_.pop_back();}break;
    case GlOperation::PushAttrib:if(u(0)!=0xfffffU && (u(0)&~0xcfdedU))error(0x501);else if(attributes_.size()>=16)error(0x503);else {
        AttributeSnapshot snapshot;snapshot.mask=u(0);snapshot.state=state_;
        snapshot.programmable={gl_.src_alpha,gl_.dst_alpha,gl_.equation_rgb,gl_.equation_alpha,gl_.polygon_mode};snapshot.current_attributes=gl_.attribute_values;
        if(u(0)&0x40000){
            snapshot.active=gl_.active_texture;snapshot.units=gl_.texture_units;snapshot.units[snapshot.active]=state_.binding;
            for(const auto id:snapshot.units)if(const auto t=textures_.find(id);t!=textures_.end())snapshot.texture_parameters[id]={t->second.min,t->second.mag,t->second.s,t->second.t,t->second.anisotropy,t->second.max_level};
        }
        attributes_.push_back(std::move(snapshot));
    }break;
    case GlOperation::PopAttrib:if(attributes_.empty())error(0x504);else {const auto snapshot=std::move(attributes_.back());attributes_.pop_back();const auto mask=snapshot.mask;const auto& s=snapshot.state;
        if(mask==0xfffffU)state_=s;
        if(mask&1)gl_.attribute_values=snapshot.current_attributes;
        if(mask&0x4000){gl_.src_alpha=snapshot.programmable[0];gl_.dst_alpha=snapshot.programmable[1];gl_.equation_rgb=snapshot.programmable[2];gl_.equation_alpha=snapshot.programmable[3];}
        if(mask&8)state_.read_buffer=s.read_buffer;
        if(mask&0x20){gl_.polygon_mode=snapshot.programmable[4];state_.cull_face=s.cull_face;state_.front_face=s.front_face;state_.enabled.erase(0xb44);if(s.enabled.contains(0xb44))state_.enabled.insert(0xb44);}

        if(mask&0x40000){
            gl_.active_texture=snapshot.active;gl_.texture_units=snapshot.units;state_.binding=s.binding;state_.environments=s.environments;
            state_.enabled.erase(0xde1);if(s.enabled.contains(0xde1))state_.enabled.insert(0xde1);
            // 還原參數與綁定，不回退 Push 之後更新的貼圖影像與資源。
            for(const auto& [id,p]:snapshot.texture_parameters){auto& t=textures_[id];t.min=p[0];t.mag=p[1];t.s=p[2];t.t=p[3];t.anisotropy=p[4];t.max_level=p[5];}
        }
        if(mask&1){state_.color=s.color;state_.texcoord=s.texcoord;state_.normal=s.normal;}if(mask&0x40){state_.lights=s.lights;state_.materials=s.materials;for(unsigned cap:{0xb50U,0xb57U,0x4000U,0x4001U,0x4002U,0x4003U,0x4004U,0x4005U,0x4006U,0x4007U}){state_.enabled.erase(cap);if(s.enabled.contains(cap))state_.enabled.insert(cap);}}if(mask&0x8000){state_.fog_hint=s.fog_hint;state_.perspective_hint=s.perspective_hint;}if(mask&0x80){state_.fog=s.fog;state_.enabled.erase(0xb60);if(s.enabled.contains(0xb60))state_.enabled.insert(0xb60);}if(mask&0x100){state_.depth_clear=s.depth_clear;state_.depth_func=s.depth_func;state_.depth_write=s.depth_write;state_.enabled.erase(0xb71);if(s.enabled.contains(0xb71))state_.enabled.insert(0xb71);}if(mask&0x400){state_.stencil=s.stencil;state_.enabled.erase(0xb90);if(s.enabled.contains(0xb90))state_.enabled.insert(0xb90);}if(mask&0x800){state_.viewport=s.viewport;state_.depth_range=s.depth_range;}if(mask&0x1000){state_.matrix_mode=s.matrix_mode;state_.enabled.erase(0xba1);if(s.enabled.contains(0xba1))state_.enabled.insert(0xba1);}if(mask&0x2000)state_.enabled=s.enabled;if(mask&0x4000){state_.alpha_func=s.alpha_func;state_.alpha_ref=s.alpha_ref;state_.enabled.erase(0xbc0);if(s.enabled.contains(0xbc0))state_.enabled.insert(0xbc0);state_.clear=s.clear;state_.draw_buffer=s.draw_buffer;state_.blend_src=s.blend_src;state_.blend_dst=s.blend_dst;state_.enabled.erase(0xbe2);if(s.enabled.contains(0xbe2))state_.enabled.insert(0xbe2);}if(mask&0x4000)state_.color_mask=s.color_mask;if(mask&4)state_.line_width=s.line_width;if(mask&0x20)state_.pixel_zoom=s.pixel_zoom;if(mask&0x80000){state_.scissor=s.scissor;state_.enabled.erase(0xc11);if(s.enabled.contains(0xc11))state_.enabled.insert(0xc11);}}break;
    case GlOperation::NewList:if(compiling_ || !u(0) || (u(1)!=0x1300 && u(1)!=0x1301))error(0x502);else {if(!lists_.contains(u(0)) && lists_.size()>=65536)throw std::bad_alloc{};for(const auto& c:lists_[u(0)])list_bytes_-=sizeof(Command)+c.args.size()*8+c.data.size();lists_[u(0)].clear();compiling_=u(0);compile_mode_=u(1);}break;
    case GlOperation::EndList:if(!compiling_)error(0x502);else compiling_=0;break;
    case GlOperation::GenLists:if(n(0)<0)error(0x501);else if(u(0)){
        if(u(0)>65536 || lists_.size()+u(0)>65536)throw std::bad_alloc{};
        std::uint64_t candidate=next_list_;for(auto it=lists_.lower_bound(next_list_);it!=lists_.end() && it->first<candidate+u(0);++it)candidate=std::uint64_t{it->first}+1;
        if(candidate+u(0)>UINT32_MAX)throw std::bad_alloc{};const auto first=static_cast<unsigned>(candidate);next_list_=first;for(unsigned i=0;i<u(0);++i)lists_.try_emplace(next_list_++);return first;}break;
    case GlOperation::DeleteLists:if(n(1)<0)error(0x501);else for(auto it=lists_.lower_bound(u(0));it!=lists_.end() && std::uint64_t{it->first}<std::uint64_t{u(0)}+u(1);){for(const auto& c:it->second)list_bytes_-=sizeof(Command)+c.args.size()*8+c.data.size();it=lists_.erase(it);}break;
    case GlOperation::CallList:if(recursion_>=64)error(0x503);else if(const auto it=lists_.find(u(0));it!=lists_.end()){const bool saved=replaying_;replaying_=true;++recursion_;try{for(const auto& c:it->second)execute(c.op,c.args,c.data,{});}catch(...){--recursion_;replaying_=saved;throw;}--recursion_;replaying_=saved;}break;
    case GlOperation::DrawArrays:case GlOperation::DrawElements:{if(!arrays_[0].enabled)break;const unsigned count=u(op==GlOperation::DrawArrays?2:1);if(count>1048576 || u(0)>9){error(0x501);break;}primitive_=u(0);vertices_.clear();begun_=true;try{for(unsigned i=0;i<count;++i)array_vertex(op==GlOperation::DrawArrays?u(1)+i:static_cast<unsigned>(number(data,static_cast<std::size_t>(i)*unit(u(2)),u(2))));}catch(...){begun_=false;vertices_.clear();throw;}begun_=false;end_primitive();break;}
    case GlOperation::GetIntegerv:case GlOperation::GetFloatv:case GlOperation::GetDoublev:case GlOperation::GetBooleanv:query(op,u(0),output);break;
    case GlOperation::ReadPixels:{if((u(4)!=0x1908 && u(4)!=0x1907) || u(5)!=0x1401){error(0x500);break;}if(n(2)<0 || n(3)<0){error(0x501);break;}flush(true);const unsigned components=u(4)==0x1908?4:3;const auto [stride,offset]=pixel_layout(u(2),components,true);if(u(2) && u(3) && offset+(u(3)-1)*stride+std::size_t{u(2)}*components>output.size())throw std::invalid_argument("read pixels bounds");
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width_)*4);for(unsigned y=0;y<u(3);++y){const auto src_y=static_cast<std::int64_t>(n(1))+y;if(src_y<0 || src_y>=height_)continue;[(state_.read_buffer==0x404?front_:color_) getBytes:row.data() bytesPerRow:width_*4 fromRegion:MTLRegionMake2D(0,static_cast<NSUInteger>(height_-1-src_y),width_,1) mipmapLevel:0];for(unsigned x=0;x<u(2);++x){const auto src_x=static_cast<std::int64_t>(n(0))+x;if(src_x<0 || src_x>=width_)continue;const auto at=offset+y*stride+x*components,src=static_cast<std::size_t>(src_x)*4;output[at]=row[src+2];output[at+1]=row[src+1];output[at+2]=row[src];if(components==4)output[at+3]=row[src+3];}}break;}
    case GlOperation::DrawBuffer:case GlOperation::ReadBuffer:if(u(0)!=0x404 && u(0)!=0x405)error(0x500);else (op==GlOperation::DrawBuffer?state_.draw_buffer:state_.read_buffer)=u(0);break;
    case GlOperation::Flush:flush(false);break;case GlOperation::Finish:flush(true);break;
    default:error(0x502);break;
    }}catch(const std::invalid_argument&){error(0x501);}
    if(op==GlOperation::Color3f || op==GlOperation::Color4f || op==GlOperation::Color4fv || op==GlOperation::Color4dv)track_color();
    return 0;
}}
void metal_fence_relay(NativeOpenGlContext& native,std::shared_ptr<std::atomic<bool>> relay,bool flush){
    if(auto* gl=dynamic_cast<MetalGl*>(&native)){gl->fence_relay(std::move(relay),flush);return;}
    native.graphics_flush(true);relay->store(true,std::memory_order_release);
}
}
// MTLCreateSystemDefaultDevice 每個程序第一次呼叫約 50 ms；只為裝置列舉取名稱時改讀 IOKit。
// 單一 IOAccelerator 的 model 與 Metal 預設裝置的 name 相同（Apple Silicon 實測一致）；
// 多個加速器（預設裝置可能是其中任一個）或讀不到字串時回空，由呼叫端建立 Metal 裝置取得原值。
std::optional<std::pair<std::string,std::string>> metal_opengl_identity(){
    io_iterator_t iterator=IO_OBJECT_NULL;
    if(IOServiceGetMatchingServices(MACH_PORT_NULL,IOServiceMatching("IOAccelerator"),&iterator)!=KERN_SUCCESS)return std::nullopt;
    unsigned count=0;std::optional<std::string> model;
    while(const auto service=IOIteratorNext(iterator)){
        ++count;
        if(const auto value=IORegistryEntryCreateCFProperty(service,CFSTR("model"),kCFAllocatorDefault,0)){
            std::array<char,256> text{};
            if(CFGetTypeID(value)==CFStringGetTypeID() && CFStringGetCString(static_cast<CFStringRef>(value),text.data(),text.size(),kCFStringEncodingUTF8) && text[0])model=text.data();
            CFRelease(value);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if(count!=1 || !model)return std::nullopt;
    return std::pair<std::string,std::string>{*model+" (Metal)","MacWin"};
}
std::shared_ptr<NativeOpenGlContext> make_metal_opengl(void* view,std::uint32_t w,std::uint32_t h){return std::make_shared<MetalGl>(view,w,h);}
std::shared_ptr<NativeOpenGlContext> make_metal_command_thread(std::shared_ptr<NativeOpenGlContext> inner){return std::make_shared<MetalCommandThread>(std::move(inner));}
}
