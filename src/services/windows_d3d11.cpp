#include <cstdio>
#include "x64arm/resource_usage.hpp"
#include <cstdlib>
#include "windows_display_identity.hpp"
#include "window_presentation_fault.hpp"
#include "graphics_frame_test_control.hpp"
#include "x64arm/windows_services.hpp"
#include <atomic>
#include "x64arm/d3dx_font_raster.hpp"
#include "x64arm/d3dx_sprite_sort.hpp"
#include "x64arm/media_surface.hpp"
#include "x64arm/video_codec.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/dxva_h264.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/windows_geometry.hpp"
#include <cmath>
#include <tuple>
#include <bit>
#include <cstring>
#include <chrono>
#include <set>
#include "d3d11_guids.hpp"
#include "d3d10_guids.hpp"
#include "d3d10_effect.hpp"
#include "d3d10_sprite.hpp"
#include "d3d10_mesh.hpp"
#include "x64arm/raster_surface.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "texture_image.hpp"
#include "texture_load.hpp"
#include "texture_transform.hpp"
#include "hlsl_include.hpp"
#include "dxbc_reflection.hpp"
#include "d3d_stream_output.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <sstream>

namespace x64arm {
namespace {
using namespace d3d11_guid;
constexpr std::uint32_t invalid=0x80070057U, no_interface=0x80004002U, not_supported=0x887a0004U,
    not_found=0x887a0002U, out_of_memory=0x8007000eU, failure=0x80004005U;
enum class Kind { Mesh10, MeshBuffer10, Sprite10, Font10, Effect10, EffectType10, EffectVariable10, EffectScalar10, EffectVector10, EffectMatrix10, EffectString10, EffectResource10, EffectRenderTarget10, EffectDepthView10, EffectBuffer10, EffectShader10, EffectBlend10, EffectDepth10, EffectRaster10, EffectSampler10, EffectTechnique10, EffectPass10, StateBlock10, ClassLinkage, ClassInstance, Reflection, ReflectionBuffer, ReflectionVariable, ReflectionType, Device, Context, VideoDevice, VideoContext, Multithread, Texture, Decoder, View, Factory, Adapter, BufferObject, Texture1D, Texture3D, ShaderResource, Unordered, RenderTarget, DepthView, VertexShader, PixelShader, ComputeShader, GeometryShader, HullShader, DomainShader, Layout, Blend, DepthState, Raster, Sampler, Query, Predicate, CommandList, DxgiDevice, SwapChain, Output, Blob, DxgiResource };
struct Buffer { std::uint64_t address{}; std::uint32_t capacity{}; bool acquired{}; std::vector<std::uint8_t> submitted; };
struct ReferenceSurface {
    std::array<std::int32_t,2> poc;
    std::uint64_t texture{}, generation{};
    std::uint16_t frame_num{};
};
struct RecordedGraphicsCommand {
    std::function<void(NativeOpenGlContext&)> execute;
    std::uint32_t draws{},dispatches{};
};
struct DeferredGraphicsMap {
    GraphicsView view;
    std::vector<std::uint8_t> bytes;
    std::uint64_t pointer{};
    std::uint32_t row{},image{};
};
struct Identity {
    std::map<std::pair<std::string,std::uint32_t>,std::pair<std::uint32_t,std::uint64_t>> class_instances;
    std::map<std::string,std::uint32_t> class_types;
    std::vector<std::shared_ptr<dxbc::Reflection>> linked_shaders;
    std::array<std::shared_ptr<GraphicsResource>,6> linkage_constants;
    std::array<std::vector<std::uint8_t>,6> linkage_keys;
    std::string class_name,class_type;
    std::array<std::uint32_t,8> class_desc{};
    std::shared_ptr<dxbc::Reflection> reflection;
    std::map<std::string,std::uint64_t> reflection_strings;
    std::uint64_t reflection_owner{};
    std::uint32_t reflection_buffer{UINT32_MAX},reflection_variable{UINT32_MAX},reflection_offset{};
    const dxbc::Type* reflection_type{};

    Kind kind{};
    bool api10{},predicate_value{},weak_bindings{};
    std::shared_ptr<fx10::Program> effect;
    std::shared_ptr<fx10::Type> effect_type;
    const fx10::Annotation* effect_annotation{};
    std::vector<std::uint8_t> effect_annotation_data;
    std::uint64_t effect_owner{};
    std::uint32_t effect_variable{UINT32_MAX},effect_buffer{UINT32_MAX},effect_technique{UINT32_MAX},effect_pass{UINT32_MAX},effect_element{},effect_offset{};
    bool effect_valid{true},effect_optimized{},effect_anonymous{};
    std::uint64_t effect_inline_shader{};
    std::map<std::string,std::uint64_t> effect_handles;

    std::shared_ptr<D3D10MeshData> mesh;
    D3D10SpriteData sprite;
    std::shared_ptr<Identity> sprite_saved;
    std::uint64_t font_dc{},font_handle{};
    std::map<unsigned,std::array<std::int32_t,4>> font_glyph_bounds;
    D3dxFontDrawCache font_draw_cache;
    std::uint64_t context10{};
    std::array<std::uint32_t,2> text_filter_size{1,1};
    std::shared_ptr<NativeOpenGlContext> native;
    std::shared_ptr<GraphicsResource> resource;
    // 交換鏈的完整前幀與下一幀；Present1 局部更新不能讀取未更新的 backbuffer 區域。
    std::shared_ptr<GraphicsResource> present_frame,present_spare;
    std::shared_ptr<GraphicsShader> shader;
    std::shared_ptr<GraphicsSampler> sampler;
    GraphicsView graphics_view;
    std::vector<GraphicsElement> layout;
    std::map<std::string,std::vector<std::uint64_t>> bindings;
    std::map<Guid,std::vector<std::uint8_t>> private_data;
    std::map<Guid,std::uint64_t> private_interfaces;
    std::vector<std::array<float,6>> viewports;
    std::vector<std::array<std::int32_t,4>> scissors;
    std::array<float,4> blend_factor{1,1,1,1};
    std::array<std::uint32_t,32> strides{},offsets{};
    std::array<std::uint32_t,4> stream_offsets{};
    std::shared_ptr<GraphicsStreamStatistics> stream_statistics{std::make_shared<GraphicsStreamStatistics>()};
    GraphicsStreamStatistics stream_begin;
    std::uint32_t topology{},index_format{42},index_offset{},sample_mask{UINT32_MAX},stencil_reference{},priority{},query_size{},context_type{};
    std::uint64_t window{},factory{},adapter{},frames{},draws{},dispatches{};
    std::vector<RecordedGraphicsCommand> commands;
    std::map<std::pair<std::uint64_t,std::uint32_t>,DeferredGraphicsMap> deferred_maps;
    bool offered{};
    std::vector<std::uint8_t> query_data;
    // 事件查詢（D3D11_QUERY_EVENT）以後端完成旗標判定就緒，End 不等待 GPU。
    std::shared_ptr<const std::atomic<bool>> query_fence;
    std::shared_ptr<const GraphicsTimestampFuture> query_timestamp;
    // 遮蔽查詢：End 只記錄 fence 與取樣序號，GetData 在 fence 完成後才讀回可見度緩衝區。
    std::uint64_t query_pending_sequence{};
    std::uint32_t query_polls{}; // GetData 以 DONOTFLUSH 連續輪詢的次數；超過門檻仍提交，避免永不完成。
    bool query_active{},query_ready{},fullscreen{};
    std::uint64_t fullscreen_output{};
    std::array<std::int32_t,4> windowed_rect{};
    std::uint32_t windowed_style{},windowed_ex_style{};
    std::optional<std::array<std::uint32_t,4>> windowed_insets;
    std::vector<std::uint8_t> target_mode;
    std::chrono::steady_clock::time_point present_trace{};
    float min_lod{};

    std::uint64_t root{}, device{}, texture{}, current_view{}, mapped{}, lock_address{};
    std::uint32_t references{1}, slice{}, creation_flags{}, feature_level{0xb000}, exception_mode{};
    struct SubresourceMap { std::uint64_t pointer{};std::size_t size{};std::uint32_t type{}; };
    // 每個 mip／array slice 的 CPU 映射獨立，不把整個資源當成單一鎖。
    std::map<std::uint32_t,SubresourceMap> subresource_maps;
    // DYNAMIC 資源的持久映射區塊（子資源 → 客體 heap 位址與大小）；保留在 allocations，資源釋放時一併歸還。
    struct PersistentMap { std::uint64_t pointer{};std::size_t size{}; };
    std::map<std::uint32_t,PersistentMap> persistent_maps;
    std::map<Kind,std::uint64_t> interfaces;
    std::vector<std::uint64_t> allocations;
    std::array<std::uint32_t,11> texture_desc{};
    std::vector<std::vector<std::uint8_t>> pixels;
    std::vector<video::DecodedFrame> video_frames;
    std::vector<std::uint64_t> generations;
    std::shared_ptr<GpuMemoryReservation> gpu_allocation;
    std::vector<std::uint8_t> desc, config, view_desc;
    std::map<std::uint32_t,Buffer> buffers;
    std::unique_ptr<video::HardwareDecoder> codec;
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    std::map<std::uint8_t,ReferenceSurface> reference_poc;
    bool frame_active{}, submitted{}, protected_threads{};
    std::uint64_t sequence{};
};
struct Object { Kind kind; std::shared_ptr<Identity> identity; };
Guid read_guid(const GuestMemory& memory,std::uint64_t address) { Guid value{};memory.read(GuestAddress{address},value);return value; }
std::vector<std::uint8_t> read_bytes(const GuestMemory& memory,std::uint64_t address,std::size_t size) {
    std::vector<std::uint8_t> value(size);memory.read(GuestAddress{address},value);return value;
}
std::uint32_t word(std::span<const std::uint8_t> data,std::size_t at) {
    if(at+4>data.size())throw std::invalid_argument("D3D11 結構長度不足");
    std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{data[at+i]}<<(8*i);return value;
}
void put_word(std::vector<std::uint8_t>& data,std::size_t at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)data.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));
}
std::vector<std::uint8_t> decoder_config() {
    std::vector<std::uint8_t> value(100);
    for(unsigned i=0;i<3;++i)std::copy(DXVA_NoEncrypt.begin(),DXVA_NoEncrypt.end(),value.begin()+i*16);
    put_word(value,48,2);value[96]=1;return value; // 未加密 H.264 short-slice／bitstream raw 2。
}
bool valid_decoder_desc(std::span<const std::uint8_t> value) {
    return value.size()==28 && std::equal(D3D11_DECODER_PROFILE_H264_VLD_NOFGT.begin(),D3D11_DECODER_PROFILE_H264_VLD_NOFGT.end(),value.begin()) &&
        word(value,16)>0 && word(value,16)<=4096 && word(value,20)>0 && word(value,20)<=4096 &&
        !(word(value,16)%2) && !(word(value,20)%2) && word(value,24)==103;
}
bool resource_kind(Kind k){return k==Kind::Texture || k==Kind::BufferObject || k==Kind::Texture1D || k==Kind::Texture3D;}
bool graphics_view_kind(Kind k){return k==Kind::ShaderResource || k==Kind::Unordered || k==Kind::RenderTarget || k==Kind::DepthView;}
bool child(Kind k) {
    switch(k){case Kind::ClassLinkage:case Kind::ClassInstance:case Kind::Context:case Kind::VideoContext:case Kind::Texture:case Kind::Texture1D:case Kind::Texture3D:case Kind::BufferObject:case Kind::Decoder:case Kind::View:case Kind::ShaderResource:case Kind::Unordered:case Kind::RenderTarget:case Kind::DepthView:case Kind::VertexShader:case Kind::PixelShader:case Kind::ComputeShader:case Kind::GeometryShader:case Kind::HullShader:case Kind::DomainShader:case Kind::Layout:case Kind::Blend:case Kind::DepthState:case Kind::Raster:case Kind::Sampler:case Kind::Query:case Kind::Predicate:case Kind::CommandList:return true;default:return false;}
}
}

// 編譯器重播只讀取已開啟 include 的快照；每次客體 Open 僅執行一次。
// guest callback 仍在原呼叫執行緒執行，不在原生編譯器內巢狀跑 CPU。
struct D3DCompileInclude {
    std::string filename,parent,name;
    bool local{},success{};
    std::uint64_t data{};
    std::vector<std::uint8_t> bytes;
};
struct D3DCompileOperation {
    WindowsServiceDescriptor service{};
    std::uint64_t id{},thread{},stack{},handler{},root_data{},scratch{};
    std::size_t total_bytes{};
    std::vector<std::uint8_t> source;
    std::vector<D3DCompileInclude> includes;
    std::optional<D3DCompileInclude> pending;
    std::optional<GraphicsCompilation> result;
    bool closing{},closed{};
};

struct D3D11SharedResource {std::weak_ptr<GraphicsResource> resource;std::weak_ptr<NativeOpenGlContext> producer;std::vector<std::uint8_t> desc;};
struct D3D11State {
    std::map<std::uint32_t,D3D11SharedResource> shared_resources;
    std::uint32_t next_shared_resource{0xa6000000};
    std::map<std::uint64_t,D3DCompileOperation> compilations;
    std::uint64_t next_compilation{};
    std::map<std::uint64_t,Object> objects;
    std::map<std::pair<Kind,bool>,std::uint64_t> vtables;
    std::uint64_t hardware_sessions{}, decoded_frames{};
    std::string last_error;
    std::uint64_t graphics_draws{},graphics_dispatches{},graphics_frames{};
    std::uint64_t linkage_draws{},linkage_cache_hits{},linkage_uploads{},linkage_readback_bytes{};
    std::size_t peak_interfaces{};
};
void WindowsServices::discard_d3d_compile(GuestState& state,GuestMemory& memory,std::uint64_t key){
    if(!d3d11_)return;const auto at=d3d11_->compilations.find(key);if(at==d3d11_->compilations.end())return;
    if(at->second.scratch)(void)free_heap(memory,at->second.scratch,state);
    d3d11_->compilations.erase(at);
}
std::optional<std::uint32_t> WindowsServices::resume_d3d_compile(GuestState& state,GuestMemory& memory,std::uint64_t key,std::uint32_t value){
    if(!d3d11_ || !d3d11_->compilations.contains(key))throw HostCallFault("D3D include callback 缺少編譯狀態");
    auto& job=d3d11_->compilations.at(key);
    if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[hlsl-include] callback=%s file=%s result=%08x\n",job.closing?"Close":"Open",job.pending?job.pending->filename.c_str():"",value);
    if(job.thread!=threads_.current)throw HostCallFault("D3D include callback 執行緒不符");
    if(job.closing){job.closed=true;job.closing=false;}
    else {
        if(!job.pending || !job.scratch)throw HostCallFault("D3D include Open 狀態不完整");
        auto include=std::move(*job.pending);job.pending.reset();
        const unsigned pointer_size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(pointer_size*8);
        if(!(value&0x80000000U)){
            include.data=memory.load(GuestAddress{job.scratch},width);
            const auto size=memory.load(GuestAddress{job.scratch+pointer_size},32);
            if(size>16U*1024U*1024U || job.total_bytes+size>64U*1024U*1024U || (size && !include.data))
                throw HostCallFault("D3D include Open 返回不合法的資料範圍");
            include.bytes=read_bytes(memory,include.data,static_cast<std::size_t>(size));include.success=true;
            job.total_bytes+=size;
        }
        if(!free_heap(memory,job.scratch,state))throw HostCallFault("D3D include callback 暫存被破壞");
        job.scratch=0;job.includes.push_back(std::move(include));
    }
    const auto service=job.service;return invoke_d3d11(state,memory,service);
}
bool WindowsServices::media_graphics_device(std::uint64_t pointer) const {
    if(!d3d11_)return false;const auto found=d3d11_->objects.find(pointer);
    return found!=d3d11_->objects.end() && found->second.identity->kind==Kind::Device && bool(found->second.identity->native);
}
std::optional<MediaSurface> WindowsServices::media_graphics_surface(std::uint64_t pointer,std::uint32_t index) const {
    if(!d3d11_)return {};const auto found=d3d11_->objects.find(pointer);
    if(found==d3d11_->objects.end() || found->second.identity->kind!=Kind::Texture)return {};
    const auto id=found->second.identity;MediaSurface result;
    if(id->resource){
        const auto& desc=id->resource->desc;if(index>=desc.levels*desc.layers || desc.samples!=1)return {};
        const auto level=index%desc.levels,width=std::max(1U,desc.width>>level),height=std::max(1U,desc.height>>level);
        const auto row=static_cast<std::uint64_t>(width)*(desc.format==103?1U:graphics_format_bytes(desc.format));
        const auto size=row*height*(desc.format==103?3U:2U)/2U;if(row>INT32_MAX || size>256U*1024U*1024U)return {};
        result.row_pitch=static_cast<std::uint32_t>(row);result.height=height;result.size=static_cast<std::uint32_t>(size);result.format=desc.format;
        GraphicsView view{id->resource,desc.format,level,1,index/desc.levels,1};const auto pitch=result.row_pitch,total=result.size;
        result.read=[id,view]{return id->native->graphics_read(view);};
        result.write=[id,view,pitch,total](std::span<const std::uint8_t> bytes){if(bytes.size()!=total)throw std::invalid_argument("MF surface 資料長度不符");id->native->graphics_write(view,bytes,pitch,total);};
    }else {
        if(index>=id->pixels.size() || id->texture_desc[4]!=103)return {};
        const auto width=id->texture_desc[0],height=id->texture_desc[1];const auto size=static_cast<std::uint64_t>(width)*height*3/2;if(size>256U*1024U*1024U)return {};
        result.row_pitch=width;result.height=height;result.size=static_cast<std::uint32_t>(size);result.format=103;
        result.read=[id,index,width,height,size]{
            if(!id->pixels[index].empty())return id->pixels[index];const auto& frame=id->video_frames[index];
            if(!frame.native)return std::vector<std::uint8_t>(static_cast<std::size_t>(size));
            auto bytes=frame.native->copy_nv12();if(frame.width==width && frame.height==height)return bytes;
            if(frame.width>width || frame.height>height)throw std::invalid_argument("MF surface 的影格尺寸不符");std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
            for(unsigned y=0;y<frame.height;++y)std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(y*frame.width),frame.width,output.begin()+static_cast<std::ptrdiff_t>(y*width));
            for(unsigned y=0;y<frame.height/2;++y)std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>((frame.height+y)*frame.width),frame.width,output.begin()+static_cast<std::ptrdiff_t>((height+y)*width));return output;
        };
        result.write=[id,index,size](std::span<const std::uint8_t> bytes){if(bytes.size()!=size)throw std::invalid_argument("MF NV12 資料長度不符");id->pixels[index].assign(bytes.begin(),bytes.end());id->video_frames[index]={};++id->generations[index];};
    }
    return result;
}

std::string WindowsServices::video_diagnostics() const {
    if(!d3d11_)return {};
    std::ostringstream out;
    out<<"video_backend=VideoToolbox hardware_sessions="<<d3d11_->hardware_sessions<<" decoded_frames="<<d3d11_->decoded_frames<<" live_interfaces="<<d3d11_->objects.size()<<" peak_interfaces="<<d3d11_->peak_interfaces<<'\n';
    if(d3d11_->graphics_draws || d3d11_->graphics_dispatches || d3d11_->graphics_frames)out<<"d3d11_backend=Metal draws="<<d3d11_->graphics_draws<<" dispatches="<<d3d11_->graphics_dispatches<<" frames="<<d3d11_->graphics_frames<<'\n';
    if(d3d11_->linkage_draws)out<<"d3d11_linkage draws="<<d3d11_->linkage_draws<<" cache_hits="<<d3d11_->linkage_cache_hits<<" uploads="<<d3d11_->linkage_uploads<<" readback_bytes="<<d3d11_->linkage_readback_bytes<<'\n';
    if(std::getenv("X64ARM_TRACE_GRAPHICS_LIFETIME"))for(const auto& [pointer,entry]:d3d11_->objects){const auto& id=*entry.identity;out<<"d3d11_live pointer="<<pointer<<" root="<<id.root<<" kind="<<static_cast<unsigned>(entry.kind)<<" refs="<<id.references<<" device="<<id.device<<" texture="<<id.texture<<"\n";}
    if(!d3d11_->last_error.empty())out<<"video_error="<<d3d11_->last_error<<'\n';
    return out.str();
}

std::span<const WindowsServiceDescriptor> d3d11_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        #include "d3d10_descriptors.inc"
#include "d3d10_effect_descriptors.inc"
#include "d3d10_sprite_descriptors.inc"
#include "d3d10_mesh_descriptors.inc"
        {"d3d11.dll","D3D11CreateDeviceAndSwapChain",12},{"dxgi.dll","CreateDXGIFactory2",3},
        {"d3dx11_42.dll","D3DX11LoadTextureFromTexture",4},{"d3dx11_43.dll","D3DX11LoadTextureFromTexture",4},
        {"d3dx11_42.dll","D3DX11SaveTextureToFileA",4},{"d3dx11_42.dll","D3DX11SaveTextureToFileW",4},{"d3dx11_42.dll","D3DX11SaveTextureToMemory",5},
        {"d3dx11_43.dll","D3DX11SaveTextureToFileA",4},{"d3dx11_43.dll","D3DX11SaveTextureToFileW",4},{"d3dx11_43.dll","D3DX11SaveTextureToMemory",5},
        {"d3dx11_42.dll","D3DX11CompileFromFileA",11},
        {"d3dx11_42.dll","D3DX11CompileFromFileW",11},
        {"d3dx11_42.dll","D3DX11CompileFromMemory",13},
        {"d3dx11_42.dll","D3DX11CompileFromResourceA",13},
        {"d3dx11_42.dll","D3DX11CompileFromResourceW",13},
        {"d3dx11_43.dll","D3DX11CompileFromFileA",11},
        {"d3dx11_43.dll","D3DX11CompileFromFileW",11},
        {"d3dx11_43.dll","D3DX11CompileFromMemory",13},
        {"d3dx11_43.dll","D3DX11CompileFromResourceA",13},
        {"d3dx11_43.dll","D3DX11CompileFromResourceW",13},
        {"d3dx11_42.dll","D3DX11GetImageInfoFromFileA",4},
        {"d3dx11_42.dll","D3DX11GetImageInfoFromFileW",4},
        {"d3dx11_42.dll","D3DX11GetImageInfoFromMemory",5},
        {"d3dx11_42.dll","D3DX11GetImageInfoFromResourceA",5},
        {"d3dx11_42.dll","D3DX11GetImageInfoFromResourceW",5},
        {"d3dx11_42.dll","D3DX11CreateTextureFromFileA",6},
        {"d3dx11_42.dll","D3DX11CreateTextureFromFileW",6},
        {"d3dx11_42.dll","D3DX11CreateTextureFromMemory",7},
        {"d3dx11_42.dll","D3DX11CreateTextureFromResourceA",7},
        {"d3dx11_42.dll","D3DX11CreateTextureFromResourceW",7},
        {"d3dx11_42.dll","D3DX11CreateShaderResourceViewFromFileA",6},
        {"d3dx11_42.dll","D3DX11CreateShaderResourceViewFromFileW",6},
        {"d3dx11_42.dll","D3DX11CreateShaderResourceViewFromMemory",7},
        {"d3dx11_42.dll","D3DX11CreateShaderResourceViewFromResourceA",7},
        {"d3dx11_42.dll","D3DX11CreateShaderResourceViewFromResourceW",7},
        {"d3dx11_43.dll","D3DX11GetImageInfoFromFileA",4},
        {"d3dx11_43.dll","D3DX11GetImageInfoFromFileW",4},
        {"d3dx11_43.dll","D3DX11GetImageInfoFromMemory",5},
        {"d3dx11_43.dll","D3DX11GetImageInfoFromResourceA",5},
        {"d3dx11_43.dll","D3DX11GetImageInfoFromResourceW",5},
        {"d3dx11_43.dll","D3DX11CreateTextureFromFileA",6},
        {"d3dx11_43.dll","D3DX11CreateTextureFromFileW",6},
        {"d3dx11_43.dll","D3DX11CreateTextureFromMemory",7},
        {"d3dx11_43.dll","D3DX11CreateTextureFromResourceA",7},
        {"d3dx11_43.dll","D3DX11CreateTextureFromResourceW",7},
        {"d3dx11_43.dll","D3DX11CreateShaderResourceViewFromFileA",6},
        {"d3dx11_43.dll","D3DX11CreateShaderResourceViewFromFileW",6},
        {"d3dx11_43.dll","D3DX11CreateShaderResourceViewFromMemory",7},
        {"d3dx11_43.dll","D3DX11CreateShaderResourceViewFromResourceA",7},
        {"d3dx11_43.dll","D3DX11CreateShaderResourceViewFromResourceW",7},
        {"d3dcompiler_47.dll","D3DCompile",11},{"d3dcompiler_43.dll","D3DCompile",11},
        {"d3dcompiler_47.dll","D3DCompile2",14},{"d3dcompiler_47.dll","D3DCreateBlob",2},{"d3dcompiler_43.dll","D3DCreateBlob",2},
        {"d3dcompiler_47.dll","D3DDisassemble",5},{"d3dcompiler_43.dll","D3DDisassemble",5},
        {"internal-d3d11","BlobPointer",1},{"internal-d3d11","BlobSize",1},
#include "d3d11_graphics_descriptors.inc"
#include "d3d11_reflection_descriptors.inc"
        {"d3d11.dll","D3D11CreateDevice",10},{"dxgi.dll","CreateDXGIFactory",2},{"dxgi.dll","CreateDXGIFactory1",2},
        {"internal-d3d11","Unsupported",1},{"internal-d3d11","QueryInterface",3},{"internal-d3d11","AddRef",1},{"internal-d3d11","Release",1},
        {"internal-d3d11","GetDevice",2},{"internal-d3d11","CreateTexture2D",4},{"internal-d3d11","CheckFormatSupport",3},
        {"internal-d3d11","GetFeatureLevel",1},{"internal-d3d11","GetCreationFlags",1},{"internal-d3d11","GetDeviceRemovedReason",1},
        {"internal-d3d11","GetImmediateContext",2},{"internal-d3d11","Map",6},{"internal-d3d11","Unmap",3},
        {"internal-d3d11","CopySubresourceRegion",9},{"internal-d3d11","CopyResource",3},{"internal-d3d11","Flush",1},
        {"internal-d3d11","GetContextType",1},{"internal-d3d11","GetContextFlags",1},{"internal-d3d11","TextureGetDesc",2},
        {"internal-d3d11","ResourceGetType",2},{"internal-d3d11","CreateVideoDecoder",4},{"internal-d3d11","CreateVideoDecoderOutputView",4},
        {"internal-d3d11","GetVideoDecoderProfileCount",1},{"internal-d3d11","GetVideoDecoderProfile",3},
        {"internal-d3d11","CheckVideoDecoderFormat",4},{"internal-d3d11","GetVideoDecoderConfigCount",3},{"internal-d3d11","GetVideoDecoderConfig",4},
        {"internal-d3d11","GetDecoderBuffer",5},{"internal-d3d11","ReleaseDecoderBuffer",3},{"internal-d3d11","DecoderBeginFrame",5},
        {"internal-d3d11","DecoderEndFrame",2},{"internal-d3d11","SubmitDecoderBuffers",4},{"internal-d3d11","GetCreationParameters",3},
        {"internal-d3d11","ViewGetResource",2},{"internal-d3d11","ViewGetDesc",2},
        {"internal-d3d11","Enter",1},{"internal-d3d11","Leave",1},{"internal-d3d11","SetMultithreadProtected",2},{"internal-d3d11","GetMultithreadProtected",1},
        {"internal-d3d11","EnumOutputs",3},{"internal-d3d11","AdapterCheckInterfaceSupport",3},{"internal-d3d11","DxgiGetParent",3},{"internal-d3d11","EnumAdapters",3},{"internal-d3d11","FactoryGetCreationFlags",1},{"internal-d3d11","EnumAdapterByLuid",4,1U<<1},{"internal-d3d11","EnumWarpAdapter",3},{"internal-d3d11","FactoryCheckFeatureSupport",4},{"internal-d3d11","EnumAdapterByGpuPreference",5},{"internal-d3d11","RegisterAdaptersChangedEvent",3},{"internal-d3d11","UnregisterAdaptersChangedNotification",2},{"internal-d3d11","AdapterGetDesc",2},{"internal-d3d11","AdapterGetDesc1",2},{"internal-d3d11","AdapterGetDesc2",2},{"internal-d3d11","AdapterGetDesc3",2},
        {"internal-d3d11","QueryVideoMemoryInfo",4},{"internal-d3d11","SetVideoMemoryReservation",4,1U<<3},
        {"internal-d3d11","RegisterHardwareContentProtectionTeardownStatusEvent",3},{"internal-d3d11","UnregisterHardwareContentProtectionTeardownStatus",2},
        {"internal-d3d11","RegisterVideoMemoryBudgetChangeNotificationEvent",3},{"internal-d3d11","UnregisterVideoMemoryBudgetChangeNotification",2},{"internal-d3d11","FactoryIsCurrent",1}
    };
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_d3d11(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(!runtime_.network.external_io)throw HostCallFault("原生硬體裝置不能在 compare 模式建立或使用");
    if(!d3d11_)d3d11_=std::make_shared<D3D11State>();
    auto& bridge=*d3d11_;
    const WindowsCall original_arg(state,memory,service);auto symbol=service.symbol;std::string adapted_symbol;
    std::array<std::uint64_t,20> adapted_arguments{};std::uint32_t adapted_mask=0;
    const auto arg=[&](unsigned i){return i<20 && (adapted_mask&(1U<<i))?adapted_arguments[i]:original_arg(i);};
    const auto adapt=[&](unsigned i,std::uint64_t value){adapted_arguments.at(i)=value;adapted_mask|=1U<<i;};
    std::vector<std::uint64_t> temporary_arguments;std::function<void()> finish_adapter;
    bool effect_compile=false;std::uint64_t effect_code_slot=0;
    struct TemporaryCleanup { std::function<void()> cleanup;~TemporaryCleanup(){cleanup();} } temporary_cleanup{[&]{for(auto pointer:temporary_arguments)(void)free_heap(memory,pointer,state);}};
    const unsigned pointer_size=x86_?4:8;const std::uint8_t pointer_bits=x86_?32:64;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto output=[&](unsigned i){memory.check(GuestAddress{arg(i)},pointer_size,Access::Write);};
    const auto object=[&](std::uint64_t address)->Object& {
        const auto found=bridge.objects.find(address);if(found==bridge.objects.end())throw HostCallFault("D3D11 COM 物件已釋放或位址無效");return found->second;
    };
    const auto typed=[&](std::uint64_t address,Kind kind)->std::shared_ptr<Identity> {
        auto& value=object(address);if(value.kind!=kind)throw HostCallFault("D3D11 COM 介面類型不符");return value.identity;
    };
    const auto hold=[&](std::uint64_t address) {
        auto& id=*object(address).identity;if(id.references==UINT32_MAX)throw HostCallFault("D3D11 reference count 溢位");return ++id.references;
    };
    const auto notify_swap_window=[&](std::uint64_t hwnd,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height){
        auto& window=gui_.windows.at(hwnd);const bool moved=window.x!=x || window.y!=y,sized=window.width!=width || window.height!=height;
        window.x=x;window.y=y;window.width=width;window.height=height;present_gui(hwnd);
        if(moved)enqueue_gui_message({hwnd,0,gui_move_lparam(window),3});
        if(sized)enqueue_gui_message({hwnd,0,static_cast<std::uint64_t>(width)|(static_cast<std::uint64_t>(height)<<16),5});
    };
    const auto restore_fullscreen=[&](const std::shared_ptr<Identity>& id){
        if(!id->fullscreen)return true;
        if(native_gui_ && !native_gui_->display_fullscreen(id->window,false))return false;
        if(gui_.windows.contains(id->window)){auto& window=gui_.windows.at(id->window);window.style=id->windowed_style;window.ex_style=id->windowed_ex_style;window.frame_insets=id->windowed_insets;
            notify_swap_window(id->window,id->windowed_rect[0],id->windowed_rect[1],static_cast<std::uint32_t>(id->windowed_rect[2]),static_cast<std::uint32_t>(id->windowed_rect[3]));}
        id->fullscreen=false;put_word(id->desc,x86_?48:56,1);return true;
    };
    std::function<void(std::uint64_t)> release;
    release=[&](std::uint64_t address) {
        auto id=object(address).identity;
        if(id->references==1 && id->fullscreen && !restore_fullscreen(id))throw WindowPresentationFault("無法恢復 DXGI 全螢幕視窗");
        if(id->references==1 && id->lock_address && runtime_.critical_sections.at(id->lock_address).owner)throw HostCallFault("不可釋放仍鎖定的 D3D11 裝置");
        if(--id->references)return;
        // D3D10 的管線綁定不增加 COM 引用；物件銷毀時自動清除所有綁定位置。
        if(id->api10)for(auto& [pointer,entry]:bridge.objects){
            auto& context=*entry.identity;if(pointer!=context.root || !context.weak_bindings)continue;
            for(auto& [name,values]:context.bindings){(void)name;for(auto& bound:values)
                if(bound && std::any_of(id->interfaces.begin(),id->interfaces.end(),[&](const auto& item){return item.second==bound;}))bound=0;}
        }
        if(!id->weak_bindings)for(auto& [name,values]:id->bindings){(void)name;for(auto pointer:values)if(pointer)release(pointer);}
        for(const auto& item:id->sprite.items)if(item.retained)release(item.texture);
        for(auto& [guid,pointer]:id->private_interfaces){(void)guid;if(pointer)release(pointer);}
        if(id->font_dc){const auto found=gui_.contexts.find(id->font_dc);if(found!=gui_.contexts.end()){const auto bitmap=found->second.bitmap;rasters_.erase(bitmap);gui_.bitmaps.erase(bitmap);gui_.contexts.erase(found);}gui_.fonts.erase(id->font_handle);}
        if(id->sprite_saved)for(const auto& [key,values]:id->sprite_saved->bindings){(void)key;for(auto pointer:values)if(pointer)release(pointer);}
        if(id->factory)release(id->factory);
        if(id->adapter)release(id->adapter);
        if(id->fullscreen_output)release(id->fullscreen_output);
        if(id->lock_address)runtime_.critical_sections.erase(id->lock_address);
        for(const auto& [kind,pointer]:id->interfaces){(void)kind;bridge.objects.erase(pointer);}
        for(const auto pointer:id->allocations)if(!free_heap(memory,pointer,state))throw HostCallFault("D3D11 物件配置已被客體破壞");
        if(id->current_view)release(id->current_view);
        if(id->texture)release(id->texture);
        if(id->context10){auto context=object(id->context10).identity;context->device=0;release(id->context10);}
        if(id->device)release(id->device);
    };
    const auto vtable=[&](Kind kind,bool api10) {
        const auto key=std::pair{kind,api10};
        if(bridge.vtables.contains(key))return bridge.vtables.at(key);
        bool d10_table=false;
        std::size_t count=3;
        std::map<std::size_t,std::string_view> methods{{0,"QueryInterface"},{1,"AddRef"},{2,"Release"}};
        if(child(kind))methods.insert({{3,"GetDevice"},{4,"GetPrivateData"},{5,"SetPrivateData"},{6,"SetPrivateDataInterface"}});
        if(resource_kind(kind))methods.insert({{7,"ResourceGetType"},{8,"SetEvictionPriority"},{9,"GetEvictionPriority"}});
        switch(kind){
        case Kind::StateBlock10:count=7;methods={{0,"QueryInterface"},{1,"AddRef"},{2,"Release"},{3,"StateBlockCapture"},{4,"StateBlockApply"},{5,"StateBlockReleaseObjects"},{6,"GetDevice"}};break;
        case Kind::Blob:count=5;methods.insert({{3,"BlobPointer"},{4,"BlobSize"}});break;
#include "d3d11_graphics_vtables.inc"
#include "d3d10_effect_vtables.inc"
#include "d3d10_sprite_vtables.inc"
#include "d3d10_mesh_vtables.inc"
#include "d3d11_reflection_vtables.inc"
        case Kind::VideoDevice:count=20;methods.insert({{3,"CreateVideoDecoder"},{7,"CreateVideoDecoderOutputView"},{11,"GetVideoDecoderProfileCount"},{12,"GetVideoDecoderProfile"},{13,"CheckVideoDecoderFormat"},{14,"GetVideoDecoderConfigCount"},{15,"GetVideoDecoderConfig"}});break;
        case Kind::VideoContext:count=65;methods.insert({{7,"GetDecoderBuffer"},{8,"ReleaseDecoderBuffer"},{9,"DecoderBeginFrame"},{10,"DecoderEndFrame"},{11,"SubmitDecoderBuffers"}});break;
        case Kind::Texture:count=11;methods.insert({{7,"ResourceGetType"},{10,"TextureGetDesc"}});break;
        case Kind::Decoder:count=9;methods[7]="GetCreationParameters";break;
        case Kind::View:count=9;methods.insert({{7,"ViewGetResource"},{8,"ViewGetDesc"}});break;
        case Kind::Multithread:count=7;methods.insert({{3,"Enter"},{4,"Leave"},{5,"SetMultithreadProtected"},{6,"GetMultithreadProtected"}});break;
        }
        #include "d3d10_vtables.inc"
        const auto pointer=runtime_allocate(memory,{},count*pointer_size);
        for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{pointer+i*pointer_size},pointer_bits,resolve(d10_table?"internal-d3d10":"internal-d3d11",methods.contains(i)?methods.at(i):"Unsupported").value);
        bridge.vtables.emplace(key,pointer);return pointer;
    };
    const auto interface=[&](const std::shared_ptr<Identity>& id,Kind kind) {
        if(id->interfaces.contains(kind))return id->interfaces.at(kind);
        // COM 介面與紋理 array slice 的數量限制無關；配置由客體 heap、
        // 宿主配置及實際 GPU reservation 決定，不能用全域物件數誤報 OOM。
        id->allocations.reserve(id->allocations.size()+1);
        const auto table=vtable(kind,id->api10),pointer=allocate_heap(memory,pointer_size);if(!pointer)throw std::bad_alloc{};
        try {
            id->interfaces.emplace(kind,pointer);bridge.objects.emplace(pointer,Object{kind,id});
            memory.store(GuestAddress{pointer},pointer_bits,table);
        } catch(...) {id->interfaces.erase(kind);bridge.objects.erase(pointer);(void)free_heap(memory,pointer,state);throw;}
        id->allocations.push_back(pointer);
        bridge.peak_interfaces=std::max(bridge.peak_interfaces,bridge.objects.size());
        return pointer;
    };
    const auto create=[&](Kind kind,std::uint64_t device=0,bool api10=false) {
        auto id=std::make_shared<Identity>();id->kind=kind;id->api10=api10 || (device && kind!=Kind::Context && object(device).identity->api10);id->root=interface(id,kind);
        try {if(device)hold(device);}catch(...) {release(id->root);throw;}
        id->device=device;
        try{if(device)id->native=object(device).identity->native;
            else if(kind==Kind::Device || kind==Kind::Factory)id->native=make_metal_opengl(nullptr,1,1);
        }catch(...){release(id->root);throw;}
        return id;
    };
    std::uint64_t result=0;
#include "d3d11_graphics_void.inc"
    try {
        // 每個 host 呼叫為同步且不可重入；手動 Enter 的範圍仍須跨客體排程保護。
        if(service.family=="internal-d3d11" && symbol!="Unsupported" && symbol!="QueryInterface" && symbol!="AddRef" && symbol!="Release" && symbol!="Enter" && symbol!="Leave") {
            const auto identity=object(arg(0)).identity;
            const auto device=identity->device?object(identity->device).identity:identity;
            if(device->protected_threads && device->lock_address) {
                const auto& lock=runtime_.critical_sections.at(device->lock_address);
                if(lock.owner && lock.owner!=threads_.current) {
                    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::CriticalRetry;thread.wait.handles={device->lock_address};
                    schedule(state,memory);return std::nullopt;
                }
            }
        }
        #include "d3d10_adapter.inc"
        bool graphics_handled=false;
#include "d3d11_graphics_handlers.inc"
#include "d3d10_handlers.inc"
#include "d3d10_stateblock.inc"
#include "d3d10_effect_helpers.inc"
#include "d3d10_effect_create.inc"
#include "d3d10_effect_handlers.inc"
#include "d3d10_sprite_handlers.inc"
#include "d3d10_mesh_handlers.inc"
#include "d3dx11_save_handlers.inc"
#include "d3dx11_load_handlers.inc"
#include "d3dx11_transfer_handlers.inc"
        if(graphics_handled){}
        // 影片資源與圖形資源共用同一 COM 物件表及裝置生命週期。
        else if(symbol=="Unsupported")throw HostCallFault("尚未實作的 D3D11 COM 方法");
        else if(symbol=="CreateDXGIFactory" || symbol=="CreateDXGIFactory1") {
            output(1);memory.store(GuestAddress{arg(1)},pointer_bits,0);const auto guid=read_guid(memory,arg(0));
            if(!dxgi_factory_guid(guid) && guid!=IID_IUnknown)result=no_interface;
            else memory.store(GuestAddress{arg(1)},pointer_bits,create(Kind::Factory)->root);
        } else if(symbol=="D3D11CreateDevice" || symbol=="D3D11CreateDeviceAndSwapChain") {
            const bool with_swap=symbol=="D3D11CreateDeviceAndSwapChain";const unsigned od=with_swap?9:7,ol=with_swap?10:8,oc=with_swap?11:9;
            if(with_swap && arg(8)){output(8);write_pointer(arg(8),0);}
            if(arg(od))output(od);if(arg(oc))output(oc);if(arg(ol))memory.check(GuestAddress{arg(ol)},4,Access::Write);
            if(arg(od))memory.store(GuestAddress{arg(od)},pointer_bits,0);if(arg(oc))memory.store(GuestAddress{arg(oc)},pointer_bits,0);
            std::uint32_t level=0xb000;
            bool levels_valid=!arg(4) && !u32(5);
            if(arg(4) && u32(5)>0 && u32(5)<=16){levels_valid=false;for(unsigned i=0;i<u32(5);++i){const auto requested=memory.load(GuestAddress{arg(4)+4*i},32);if(requested==0xb000 || requested==0xa100 || requested==0xa000){level=static_cast<std::uint32_t>(requested);levels_valid=true;break;}}}
            if(arg(2) || u32(6)!=7 || (u32(3)&~0x8e3U) || !levels_valid || (arg(0)?u32(1)!=0:u32(1)!=1))result=invalid;
            else {
                if(arg(0))(void)typed(arg(0),Kind::Adapter);
                auto device=create(Kind::Device);device->creation_flags=u32(3);device->feature_level=level;if(arg(0)){hold(arg(0));device->adapter=arg(0);}
                std::uint64_t context=0;
                try {if(arg(oc))context=create(Kind::Context,device->root)->root;}
                catch(...) {release(device->root);throw;}
                if(arg(od))memory.store(GuestAddress{arg(od)},pointer_bits,device->root);
                if(arg(ol))memory.store(GuestAddress{arg(ol)},32,level);
                if(arg(oc))memory.store(GuestAddress{arg(oc)},pointer_bits,context);
                if(with_swap){try{if(arg(8)){require(arg(7)!=0);const auto swap=make_swap(device,0,arg(7),0,false);write_pointer(arg(8),swap->root);}}catch(...){if(context)release(context);release(device->root);throw;}}
                if(!arg(od))release(device->root);
            }
        } else if(symbol=="QueryInterface") {
            const auto value=object(arg(0));const auto id=value.identity;const auto guid=read_guid(memory,arg(1));output(2);memory.store(GuestAddress{arg(2)},pointer_bits,0);
            Kind target=value.kind;bool supported=false;
            if(guid==IID_IUnknown){target=id->kind;supported=true;}
            else if(id->api10){
                switch(id->kind){
                case Kind::Mesh10:supported=guid==d3d10_guid::IID_ID3DX10Mesh;break;
                case Kind::MeshBuffer10:supported=guid==d3d10_guid::IID_ID3DX10MeshBuffer;break;
                case Kind::Font10:supported=guid==d3d10_guid::IID_ID3DX10Font;break;
                case Kind::Sprite10:supported=guid==d3d10_guid::IID_ID3DX10Sprite;break;
                case Kind::Effect10:supported=guid==d3d10_guid::IID_ID3D10Effect;break;
                case Kind::SwapChain:supported=guid==IID_IDXGISwapChain || guid==IID_IDXGISwapChain1 || guid==IID_IDXGIDeviceSubObject || guid==IID_IDXGIObject;break;
                case Kind::StateBlock10:supported=guid==d3d10_guid::IID_ID3D10StateBlock;break;
                case Kind::Device:supported=guid==d3d10_guid::IID_ID3D10Device || guid==d3d10_guid::IID_ID3D10Device1;break;
                case Kind::Texture:supported=guid==d3d10_guid::IID_ID3D10Texture2D;break;
                case Kind::BufferObject:supported=guid==d3d10_guid::IID_ID3D10Buffer;break;
                case Kind::Texture1D:supported=guid==d3d10_guid::IID_ID3D10Texture1D;break;
                case Kind::Texture3D:supported=guid==d3d10_guid::IID_ID3D10Texture3D;break;
                case Kind::ShaderResource:supported=guid==d3d10_guid::IID_ID3D10ShaderResourceView || guid==d3d10_guid::IID_ID3D10ShaderResourceView1;break;
                case Kind::RenderTarget:supported=guid==d3d10_guid::IID_ID3D10RenderTargetView;break;
                case Kind::DepthView:supported=guid==d3d10_guid::IID_ID3D10DepthStencilView;break;
                case Kind::VertexShader:supported=guid==d3d10_guid::IID_ID3D10VertexShader;break;
                case Kind::PixelShader:supported=guid==d3d10_guid::IID_ID3D10PixelShader;break;
                case Kind::GeometryShader:supported=guid==d3d10_guid::IID_ID3D10GeometryShader;break;
                case Kind::Layout:supported=guid==d3d10_guid::IID_ID3D10InputLayout;break;
                case Kind::Blend:supported=guid==d3d10_guid::IID_ID3D10BlendState || guid==d3d10_guid::IID_ID3D10BlendState1;break;
                case Kind::DepthState:supported=guid==d3d10_guid::IID_ID3D10DepthStencilState;break;
                case Kind::Raster:supported=guid==d3d10_guid::IID_ID3D10RasterizerState;break;
                case Kind::Sampler:supported=guid==d3d10_guid::IID_ID3D10SamplerState;break;
                case Kind::Query:supported=guid==d3d10_guid::IID_ID3D10Query;break;
                case Kind::Predicate:supported=guid==d3d10_guid::IID_ID3D10Predicate;break;
                default:break;}
                if(child(id->kind) && guid==d3d10_guid::IID_ID3D10DeviceChild)supported=true;
                if(resource_kind(id->kind) && guid==d3d10_guid::IID_ID3D10Resource)supported=true;
                if(graphics_view_kind(id->kind) && guid==d3d10_guid::IID_ID3D10View)supported=true;
                if(id->kind==Kind::Device){if(guid==IID_IDXGIDevice || guid==IID_IDXGIDevice1 || guid==IID_IDXGIDevice2 || guid==IID_IDXGIObject){target=Kind::DxgiDevice;supported=true;}if(guid==IID_ID3D10Multithread){target=Kind::Multithread;supported=true;}}
            } else if(id->kind==Kind::Device){
                if(guid==IID_ID3D11Device){target=Kind::Device;supported=true;}
                if(guid==IID_IDXGIDevice || guid==IID_IDXGIDevice1 || guid==IID_IDXGIDevice2 || guid==IID_IDXGIObject){target=Kind::DxgiDevice;supported=true;}
                if(guid==IID_ID3D11VideoDevice){target=Kind::VideoDevice;supported=true;}
                if(guid==IID_ID3D10Multithread){target=Kind::Multithread;supported=true;}
            } else if(id->kind==Kind::Context){
                if(guid==IID_ID3D11DeviceContext || guid==IID_ID3D11DeviceChild){target=Kind::Context;supported=true;}
                if(guid==IID_ID3D11VideoContext){target=Kind::VideoContext;supported=true;}
            } else if(id->kind==Kind::Texture)supported=guid==IID_ID3D11Texture2D || guid==IID_ID3D11Resource || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::Decoder)supported=guid==IID_ID3D11VideoDecoder || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::View)supported=guid==IID_ID3D11VideoDecoderOutputView || guid==IID_ID3D11View || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::Reflection)supported=guid==IID_ID3D11ShaderReflection || guid==IID_ID3D11ShaderReflection43;
            else if(id->kind==Kind::Blob)supported=guid==IID_ID3D10Blob;
            else if(id->kind==Kind::Factory)supported=dxgi_factory_guid(guid) || guid==IID_IDXGIObject;
            else if(id->kind==Kind::Adapter)supported=dxgi_adapter_guid(guid) || guid==IID_IDXGIObject;
#include "d3d11_graphics_query.inc"
            if(resource_kind(id->kind) && (guid==IID_IDXGIResource || guid==IID_IDXGIDeviceSubObject || guid==IID_IDXGIObject)){target=Kind::DxgiResource;supported=true;}
            if(!supported)result=no_interface;
            else {const auto pointer=interface(id,target);hold(pointer);memory.store(GuestAddress{arg(2)},pointer_bits,pointer);}
        } else if(symbol=="AddRef")result=hold(arg(0));
        else if(symbol=="Release"){result=object(arg(0)).identity->references-1;release(arg(0));}
        else if(symbol=="GetDevice") {auto id=object(arg(0)).identity;output(1);if(!id->device)throw HostCallFault("D3D11 child 沒有所屬裝置");hold(id->device);memory.store(GuestAddress{arg(1)},pointer_bits,id->device);}
        else if(symbol=="GetImmediateContext") {
            auto id=typed(arg(0),Kind::Device);output(1);std::uint64_t context=0;
            for(const auto& [pointer,item]:bridge.objects)if(item.kind==Kind::Context && !item.identity->context_type && item.identity->device==id->root){context=pointer;break;}
            if(context)hold(context);else context=create(Kind::Context,id->root)->root;memory.store(GuestAddress{arg(1)},pointer_bits,context);
        } else if(symbol=="GetFeatureLevel" || symbol=="GetCreationFlags" || symbol=="GetDeviceRemovedReason") {
            auto id=typed(arg(0),Kind::Device);result=symbol=="GetFeatureLevel"?id->feature_level:symbol=="GetCreationFlags"?id->creation_flags:0;
        } else if(symbol=="SetMultithreadProtected" || symbol=="GetMultithreadProtected" || symbol=="Enter" || symbol=="Leave") {
            auto id=typed(arg(0),Kind::Multithread);result=id->protected_threads;
            if(symbol=="SetMultithreadProtected")id->protected_threads=arg(1)!=0;
            else if(symbol=="Enter" || symbol=="Leave") {
                if(!id->lock_address) {
                    if(symbol=="Leave")throw HostCallFault("D3D11 Leave 沒有對應的 Enter");
                    id->allocations.reserve(id->allocations.size()+1);
                    const auto address=allocate_heap(memory,x86_?24:40);if(!address)throw std::bad_alloc{};
                    try {runtime_.critical_sections.emplace(address,CriticalSection{});}
                    catch(...) {(void)free_heap(memory,address,state);throw;}
                    id->allocations.push_back(address);id->lock_address=address;
                }
                auto& section=runtime_.critical_sections.at(id->lock_address);
                if(symbol=="Leave") {
                    if(section.owner!=threads_.current || !section.recursion)throw HostCallFault("執行緒未持有 D3D11 裝置鎖");
                    if(!--section.recursion)section.owner=0;
                } else if(!section.owner || section.owner==threads_.current) {
                    if(section.recursion==UINT32_MAX)throw HostCallFault("D3D11 Enter 遞迴過深");
                    section.owner=threads_.current;++section.recursion;
                } else {
                    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Critical;thread.wait.handles={id->lock_address};
                    schedule(state,memory);return std::nullopt;
                }
                memory.store(GuestAddress{id->lock_address+(x86_?4:8)},32,section.recursion-1U);memory.store(GuestAddress{id->lock_address+(x86_?8:12)},32,section.recursion);memory.store(GuestAddress{id->lock_address+(x86_?12:16)},pointer_bits,section.owner);
            }
        } else if(symbol=="EnumAdapters") {
            (void)typed(arg(0),Kind::Factory);output(2);memory.store(GuestAddress{arg(2)},pointer_bits,0);
            if(u32(1))result=not_found;else memory.store(GuestAddress{arg(2)},pointer_bits,create(Kind::Adapter,arg(0))->root);
        } else if(symbol=="EnumAdapterByLuid" || symbol=="EnumAdapterByGpuPreference") {
            // LUID 是跨 DXGI／DXCore／D3D12 的配接器身分，不能忽略呼叫端指定的值。
            const bool by_luid=symbol=="EnumAdapterByLuid";const auto riid=arg(by_luid?2:3);const unsigned out=by_luid?3U:4U;
            (void)typed(arg(0),Kind::Factory);output(out);memory.store(GuestAddress{arg(out)},pointer_bits,0);const auto guid=read_guid(memory,riid);
            if(!by_luid && u32(2)>2)result=invalid;
            else if((by_luid && arg(1)!=display_identity::adapter_luid) || (!by_luid && u32(1)))result=not_found;
            else if(guid!=IID_IUnknown && !dxgi_adapter_guid(guid) && guid!=IID_IDXGIObject)result=no_interface;
            else memory.store(GuestAddress{arg(out)},pointer_bits,create(Kind::Adapter,arg(0))->root);
        } else if(symbol=="EnumWarpAdapter") {
            // 沒有軟體光柵器；不以硬體配接器冒充 WARP。
            (void)typed(arg(0),Kind::Factory);output(2);memory.store(GuestAddress{arg(2)},pointer_bits,0);result=0x887a0004U;
        } else if(symbol=="FactoryGetCreationFlags"){(void)typed(arg(0),Kind::Factory);result=0;}
        else if(symbol=="FactoryCheckFeatureSupport") {
            // DXGI_FEATURE_PRESENT_ALLOW_TEARING：可變更新率撕裂呈現不支援。
            (void)typed(arg(0),Kind::Factory);if(u32(1)!=0 || u32(3)!=4 || !arg(2))result=invalid;else memory.store(GuestAddress{arg(2)},32,0);
        } else if(symbol=="RegisterAdaptersChangedEvent") {
            (void)typed(arg(0),Kind::Factory);if(!arg(1) || !arg(2))result=invalid;else memory.store(GuestAddress{arg(2)},32,1);
        } else if(symbol=="UnregisterAdaptersChangedNotification"){(void)typed(arg(0),Kind::Factory);}
        else if(symbol=="AdapterCheckInterfaceSupport") {
            (void)typed(arg(0),Kind::Adapter);const auto guid=read_guid(memory,arg(1));
            if(guid!=IID_IDXGIDevice && guid!=IID_ID3D11Device)result=not_supported;
            else if(arg(2))memory.store(GuestAddress{arg(2)},64,display_identity::version_number);
        } else if(symbol=="DxgiGetParent") {
            const auto value=object(arg(0));const auto guid=read_guid(memory,arg(1));output(2);memory.store(GuestAddress{arg(2)},pointer_bits,0);
            std::uint64_t parent=0;Kind kind=Kind::Factory;
            if(value.kind==Kind::DxgiDevice){parent=device_adapter(value.identity);kind=Kind::Adapter;}
            else if(value.kind==Kind::Adapter || value.kind==Kind::Output){parent=value.identity->device;kind=value.kind==Kind::Output?Kind::Adapter:Kind::Factory;}
            else if(value.kind==Kind::SwapChain)parent=value.identity->factory;
            else if(value.kind==Kind::DxgiResource){parent=interface(object(value.identity->device).identity,Kind::DxgiDevice);kind=Kind::DxgiDevice;}
            const bool supported=guid==IID_IUnknown || guid==IID_IDXGIObject ||
                (kind==Kind::Adapter && dxgi_adapter_guid(guid)) ||
                (kind==Kind::Factory && dxgi_factory_guid(guid)) ||
                (kind==Kind::DxgiDevice && (guid==IID_IDXGIDevice || guid==IID_IDXGIDevice1 || guid==IID_IDXGIDevice2));
            if(!parent || !supported)result=no_interface;
            else {hold(parent);memory.store(GuestAddress{arg(2)},pointer_bits,parent);}
        } else if(symbol=="AdapterGetDesc" || symbol=="AdapterGetDesc1" || symbol=="AdapterGetDesc2" || symbol=="AdapterGetDesc3") {
            (void)typed(arg(0),Kind::Adapter);
            if(!arg(1))result=invalid;
            else {
                const bool extended=symbol=="AdapterGetDesc2" || symbol=="AdapterGetDesc3";
                const std::size_t size=extended?(x86_?304U:320U):symbol=="AdapterGetDesc1"?(x86_?296U:312U):(x86_?292U:304U);
                std::vector<std::uint8_t> desc(size);const auto name=utf16_bytes(display_identity::adapter_name);std::copy(name.begin(),name.end(),desc.begin());
                // SIZE_T 依客體架構排列；UMA 沒有獨立顯存。搶占粒度為提交的 DMA buffer 邊界。
                const auto shared=gpu_memory_stats().limit;
                put_word(desc,x86_?280:288,static_cast<std::uint32_t>(x86_?std::min<std::uint64_t>(shared,UINT32_MAX):shared));
                if(!x86_)put_word(desc,292,static_cast<std::uint32_t>(shared>>32));
                put_word(desc,256,display_identity::vendor_id);
                put_word(desc,x86_?284:296,static_cast<std::uint32_t>(display_identity::adapter_luid));
                put_word(desc,x86_?288:300,static_cast<std::uint32_t>(display_identity::adapter_luid>>32));
                memory.write(GuestAddress{arg(1)},desc);
            }
        } else if(symbol=="QueryVideoMemoryInfo" || symbol=="SetVideoMemoryReservation") {
            (void)typed(arg(0),Kind::Adapter);
            if(u32(1) || u32(2)>1)result=invalid;
            else if(symbol=="SetVideoMemoryReservation")result=set_gpu_memory_reservation(u32(2)==0,arg(3))?0:invalid;
            else if(!arg(3))result=invalid;
            else {
                const auto budget=gpu_memory_budget(u32(2)==0);
                const std::array<std::uint64_t,4> values{budget.budget,budget.usage,budget.available,budget.reservation};
                memory.check(GuestAddress{arg(3)},32,Access::Write);
                for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(3)+8*i},64,values[i]);
            }
        } else if(symbol=="RegisterHardwareContentProtectionTeardownStatusEvent" || symbol=="RegisterVideoMemoryBudgetChangeNotificationEvent") {
            (void)typed(arg(0),Kind::Adapter);
            // 固定的程序 GPU 配額沒有動態預算通知；目前也未提供硬體保護工作階段。
            // 不回傳虛構 cookie 讓應用程式誤以為已訂閱事件。
            if(!arg(2))result=invalid;else {memory.store(GuestAddress{arg(2)},32,0);result=not_supported;}
        } else if(symbol=="UnregisterHardwareContentProtectionTeardownStatus" || symbol=="UnregisterVideoMemoryBudgetChangeNotification") {
            (void)typed(arg(0),Kind::Adapter);
        } else if(symbol=="FactoryIsCurrent"){(void)typed(arg(0),Kind::Factory);result=1;}
        else if(symbol=="CheckFormatSupport") {
            (void)typed(arg(0),Kind::Device);memory.check(GuestAddress{arg(2)},4,Access::Write);
            memory.store(GuestAddress{arg(2)},32,u32(1)==103?0x08000020U:0);if(u32(1)!=103)result=not_supported;
        } else if(symbol=="CreateTexture2D") {
            auto device=typed(arg(0),Kind::Device);if(arg(3)){output(3);memory.store(GuestAddress{arg(3)},pointer_bits,0);}
            const auto bytes=read_bytes(memory,arg(1),44);std::array<std::uint32_t,11> desc{};for(unsigned i=0;i<11;++i)desc[i]=word(bytes,4*i);
            const auto size=std::uint64_t{desc[0]}*desc[1]*3/2;
            if(!desc[0] || !desc[1] || desc[0]>4096 || desc[1]>4096 || (desc[0]%2) || (desc[1]%2) || desc[2]!=1 || !desc[3] || desc[3]>128 || desc[4]!=103 || desc[5]!=1 || desc[6] || (desc[7]!=0 && desc[7]!=3) || (desc[8]&~0x200U) || (desc[9]&~0x30000U) || desc[10])result=invalid;
            else if((desc[7]==0 && desc[9]) || (desc[7]==3 && desc[8]))result=invalid;
            else {
                std::vector<std::vector<std::uint8_t>> pixels(desc[3]);bool valid=true;
                if(arg(2))for(unsigned index=0;index<desc[3];++index){
                    const auto initial=arg(2)+index*(x86_?12U:24U),pointer=memory.load(GuestAddress{initial},pointer_bits);
                    const auto pitch=memory.load(GuestAddress{initial+pointer_bits/8U},32);const auto rows=desc[1]+desc[1]/2;
                    if(!pointer || pitch<desc[0] || pitch*rows>256U*1024U*1024U){valid=false;break;}
                    pixels[index].resize(static_cast<std::size_t>(size));
                    for(unsigned y=0;y<rows;++y)memory.read(GuestAddress{pointer+y*pitch},std::span(pixels[index]).subspan(static_cast<std::size_t>(y)*desc[0],desc[0]));
                }
                if(!valid)result=invalid;
                else if(!arg(3))result=1; // S_FALSE：合法的僅驗證呼叫。
                else {auto allocation=reserve_gpu_memory(static_cast<std::size_t>(size*desc[3]));std::vector<std::uint64_t> generations(desc[3]);auto texture=create(Kind::Texture,device->root);texture->gpu_allocation=std::move(allocation);texture->texture_desc=desc;texture->pixels=std::move(pixels);texture->video_frames.resize(desc[3]);texture->generations=std::move(generations);memory.store(GuestAddress{arg(3)},pointer_bits,texture->root);}
            }
        } else if(symbol=="TextureGetDesc") {
            auto id=typed(arg(0),Kind::Texture);std::vector<std::uint8_t> bytes(44);for(unsigned i=0;i<11;++i)put_word(bytes,i*4,id->texture_desc[i]);memory.write(GuestAddress{arg(1)},bytes);
        } else if(symbol=="ResourceGetType") {(void)typed(arg(0),Kind::Texture);memory.store(GuestAddress{arg(1)},32,3);}
        else if(symbol=="GetVideoDecoderProfileCount") {(void)typed(arg(0),Kind::VideoDevice);result=video::hardware_decode_supported(video::Codec::H264)?1:0;}
        else if(symbol=="GetVideoDecoderProfile") {
            (void)typed(arg(0),Kind::VideoDevice);if(u32(1))result=invalid;else memory.write(GuestAddress{arg(2)},D3D11_DECODER_PROFILE_H264_VLD_NOFGT);
        } else if(symbol=="CheckVideoDecoderFormat") {
            (void)typed(arg(0),Kind::VideoDevice);const auto matches=read_guid(memory,arg(1))==D3D11_DECODER_PROFILE_H264_VLD_NOFGT && u32(2)==103;
            memory.store(GuestAddress{arg(3)},32,matches?1:0);
        } else if(symbol=="GetVideoDecoderConfigCount" || symbol=="GetVideoDecoderConfig") {
            (void)typed(arg(0),Kind::VideoDevice);const auto desc=read_bytes(memory,arg(1),28);
            if(symbol=="GetVideoDecoderConfigCount")memory.store(GuestAddress{arg(2)},32,valid_decoder_desc(desc)?1:0);
            else if(!valid_decoder_desc(desc) || u32(2))result=invalid;else memory.write(GuestAddress{arg(3)},decoder_config());
        } else if(symbol=="CreateVideoDecoder") {
            auto device=typed(arg(0),Kind::VideoDevice);output(3);memory.store(GuestAddress{arg(3)},pointer_bits,0);
            auto desc=read_bytes(memory,arg(1),28),config=read_bytes(memory,arg(2),100);
            if(!valid_decoder_desc(desc) || config!=decoder_config())result=invalid;
            else {auto decoder=create(Kind::Decoder,device->root);decoder->desc=std::move(desc);decoder->config=std::move(config);memory.store(GuestAddress{arg(3)},pointer_bits,decoder->root);}
        } else if(symbol=="CreateVideoDecoderOutputView") {
            auto device=typed(arg(0),Kind::VideoDevice),texture=typed(arg(1),Kind::Texture);output(3);memory.store(GuestAddress{arg(3)},pointer_bits,0);auto desc=read_bytes(memory,arg(2),24);
            if(texture->device!=device->root || !std::equal(D3D11_DECODER_PROFILE_H264_VLD_NOFGT.begin(),D3D11_DECODER_PROFILE_H264_VLD_NOFGT.end(),desc.begin()) || word(desc,16)!=1 || word(desc,20)>=texture->pixels.size() || !(texture->texture_desc[8]&0x200))result=invalid;
            else {auto view=create(Kind::View,device->root);view->texture=arg(1);hold(arg(1));view->slice=word(desc,20);view->view_desc=std::move(desc);memory.store(GuestAddress{arg(3)},pointer_bits,view->root);}
        } else if(symbol=="ViewGetDesc"){auto id=typed(arg(0),Kind::View);memory.write(GuestAddress{arg(1)},id->view_desc);}
        else if(symbol=="ViewGetResource"){auto id=typed(arg(0),Kind::View);output(1);hold(id->texture);memory.store(GuestAddress{arg(1)},pointer_bits,id->texture);}
        else if(symbol=="GetCreationParameters") {auto id=typed(arg(0),Kind::Decoder);memory.check(GuestAddress{arg(1)},28,Access::Write);memory.check(GuestAddress{arg(2)},100,Access::Write);memory.write(GuestAddress{arg(1)},id->desc);memory.write(GuestAddress{arg(2)},id->config);}
        else if(symbol=="DecoderBeginFrame") {
            auto context=typed(arg(0),Kind::VideoContext),decoder=typed(arg(1),Kind::Decoder),view=typed(arg(2),Kind::View);
            if(context->device!=decoder->device || view->device!=decoder->device || decoder->frame_active || u32(3) || arg(4))result=invalid;
            else {decoder->frame_active=true;decoder->submitted=false;decoder->current_view=arg(2);hold(arg(2));for(auto& [type,buffer]:decoder->buffers){(void)type;buffer.submitted.clear();}}
        } else if(symbol=="GetDecoderBuffer") {
            auto context=typed(arg(0),Kind::VideoContext),decoder=typed(arg(1),Kind::Decoder);
            memory.check(GuestAddress{arg(3)},4,Access::Write);output(4);memory.store(GuestAddress{arg(3)},32,0);memory.store(GuestAddress{arg(4)},pointer_bits,0);
            const auto type=u32(2);
            if(context->device!=decoder->device || !decoder->frame_active || (type!=0 && type!=4 && type!=5 && type!=6))result=invalid;
            else {
                auto& buffer=decoder->buffers[type];
                if(buffer.acquired)result=invalid;
                else {
                    if(!buffer.address){decoder->allocations.reserve(decoder->allocations.size()+1);buffer.capacity=type==0?4096U:type==4?4096U:type==5?65536U:16U*1024U*1024U;buffer.address=allocate_heap(memory,buffer.capacity);if(!buffer.address)throw std::bad_alloc{};decoder->allocations.push_back(buffer.address);}
                    buffer.acquired=true;memory.store(GuestAddress{arg(3)},32,buffer.capacity);memory.store(GuestAddress{arg(4)},pointer_bits,buffer.address);
                }
            }
        } else if(symbol=="ReleaseDecoderBuffer") {
            auto context=typed(arg(0),Kind::VideoContext),decoder=typed(arg(1),Kind::Decoder);const auto buffer=decoder->buffers.find(u32(2));
            if(context->device!=decoder->device || !decoder->frame_active || buffer==decoder->buffers.end() || !buffer->second.acquired)result=invalid;else buffer->second.acquired=false;
        } else if(symbol=="SubmitDecoderBuffers") {
            auto context=typed(arg(0),Kind::VideoContext),decoder=typed(arg(1),Kind::Decoder);
            if(context->device!=decoder->device || !decoder->frame_active || !u32(2) || u32(2)>4)result=invalid;
            else {
                std::map<std::uint32_t,std::vector<std::uint8_t>> copies;
                for(unsigned i=0;i<u32(2);++i){
                    const unsigned descriptor_size=x86_?64:72;const auto desc=read_bytes(memory,arg(3)+descriptor_size*i,descriptor_size);const auto type=word(desc,0),offset=word(desc,8),size=word(desc,12);const auto buffer=decoder->buffers.find(type);
                    if(buffer==decoder->buffers.end() || buffer->second.acquired || !size || offset>buffer->second.capacity || size>buffer->second.capacity-offset || word(desc,4) || word(desc,36) || std::any_of(desc.begin()+40,desc.begin()+(x86_?64:68),[](auto byte){return byte!=0;}) || copies.contains(type) || !buffer->second.submitted.empty())throw std::invalid_argument("D3D11 decoder buffer 描述不合法或含加密資料");
                    copies.emplace(type,read_bytes(memory,buffer->second.address+offset,size));
                }
                for(auto& [type,bytes]:copies)decoder->buffers.at(type).submitted=std::move(bytes);
                constexpr std::array<unsigned,4> required_types{0,4,5,6};
                decoder->submitted=std::all_of(required_types.begin(),required_types.end(),[&](auto type){return decoder->buffers.contains(type) && !decoder->buffers.at(type).submitted.empty();});
            }
        } else if(symbol=="DecoderEndFrame") {
            auto context=typed(arg(0),Kind::VideoContext),decoder=typed(arg(1),Kind::Decoder);
            if(context->device!=decoder->device || !decoder->frame_active)result=invalid;
            else {
                const auto current_view=decoder->current_view;decoder->frame_active=false;decoder->current_view=0;
                try {
                    if(!decoder->submitted)throw std::invalid_argument("D3D11 frame 尚未提交完整 buffer");
                    auto view=typed(current_view,Kind::View),texture=typed(view->texture,Kind::Texture);
                    const auto packet=video::convert_dxva_h264(decoder->buffers.at(0).submitted,decoder->buffers.at(4).submitted,decoder->buffers.at(5).submitted,decoder->buffers.at(6).submitted);
                    if(packet.target_surface!=view->slice || packet.coded_width!=word(decoder->desc,16) || packet.coded_height!=word(decoder->desc,20) || packet.coded_width>texture->texture_desc[0] || packet.coded_height>texture->texture_desc[1])throw std::invalid_argument("DXVA picture 與 decoder/output surface 尺寸或索引不符");
                    if(packet.idr)decoder->reference_poc.clear();
                    for(const auto& reference:packet.references){
                        const auto found=decoder->reference_poc.find(reference.surface);
                        if(found==decoder->reference_poc.end() || reference.surface>=texture->generations.size() || found->second.poc!=reference.field_order_count ||
                           found->second.texture!=texture->root || found->second.generation!=texture->generations[reference.surface] ||
                           (!reference.long_term && found->second.frame_num!=reference.frame_num_or_long_term_index))
                            throw std::invalid_argument("DXVA reference surface 已失效、被改寫或尚未作為 reference 解碼");
                    }
                    if(!decoder->codec || decoder->parameter_sets!=packet.parameter_sets){
                        if(!packet.idr)throw std::invalid_argument("硬體解碼必須從 IDR 開始或在 IDR 變更參數集");
                        video::DecoderConfig config;config.parameter_sets=packet.parameter_sets;config.retain_native_frames=true;
                        auto codec=std::make_unique<video::HardwareDecoder>(config);
                        if(!codec->hardware_accelerated())throw std::invalid_argument("解碼 session 沒有使用硬體");
                        decoder->codec=std::move(codec);decoder->parameter_sets=packet.parameter_sets;++bridge.hardware_sessions;
                    }
                    auto frames=decoder->codec->decode(packet.sample,{static_cast<std::int64_t>(decoder->sequence++),30},{1,30},current_view);
                    if(frames.size()!=1 || frames.front().tag!=current_view)throw std::invalid_argument("D3D11 frame 的硬體輸出尚未完成或 tag 不符");
                    auto& frame=frames.front();if(frame.width!=packet.coded_width || frame.height!=packet.coded_height)throw std::invalid_argument("VideoToolbox surface 尺寸不符");
                    // 保留唯讀 VT surface；CopyResource 只共享所有權，Map 才建立 CPU 像素。
                    texture->video_frames.at(view->slice)=std::move(frame);texture->pixels.at(view->slice).clear();++texture->generations.at(view->slice);
                    decoder->reference_poc.erase(packet.target_surface);
                    if(packet.reference_picture)decoder->reference_poc.emplace(packet.target_surface,ReferenceSurface{packet.field_order_count,texture->root,texture->generations.at(view->slice),packet.frame_num});
                    ++bridge.decoded_frames;
                } catch(...) {decoder->codec.reset();decoder->parameter_sets.clear();decoder->reference_poc.clear();release(current_view);throw;}
                release(current_view);
            }
        } else if(symbol=="CopySubresourceRegion" || symbol=="CopyResource") {
            auto context=typed(arg(0),Kind::Context),destination=typed(arg(1),Kind::Texture);
            const bool entire=symbol=="CopyResource";auto source=typed(arg(entire?2:6),Kind::Texture);
            const auto dst_index=entire?0:u32(2),src_index=entire?0:u32(7);
            if(context->device!=source->device || context->device!=destination->device || (entire?(!destination->subresource_maps.empty() || !source->subresource_maps.empty()):(destination->subresource_maps.contains(dst_index) || source->subresource_maps.contains(src_index))) || (!entire && (u32(3) || u32(4) || u32(5) || arg(8))) || destination->texture_desc[0]!=source->texture_desc[0] || destination->texture_desc[1]!=source->texture_desc[1] || dst_index>=destination->pixels.size() || src_index>=source->pixels.size())throw HostCallFault("D3D11 texture copy 僅支援相同尺寸的完整 NV12 subresource");
            if(entire){if(source->pixels.size()!=destination->pixels.size())throw HostCallFault("D3D11 texture array 大小不符");auto pixels=source->pixels;auto frames=source->video_frames;destination->pixels=std::move(pixels);destination->video_frames=std::move(frames);for(auto& generation:destination->generations)++generation;}
            else {auto pixels=source->pixels[src_index];auto frame=source->video_frames[src_index];destination->pixels[dst_index]=std::move(pixels);destination->video_frames[dst_index]=std::move(frame);++destination->generations[dst_index];}
        } else if(symbol=="Map") {
            auto context=typed(arg(0),Kind::Context),texture=typed(arg(1),Kind::Texture);memory.check(GuestAddress{arg(5)},x86_?12:16,Access::Write);
            const auto type=u32(3),index=u32(2);const auto access=texture->texture_desc[9];
            if(context->device!=texture->device || texture->texture_desc[7]!=3 || texture->subresource_maps.contains(index) || index>=texture->pixels.size() || (type!=1 && type!=2 && type!=3) || (u32(4)&~0x100000U) || ((type==1 || type==3) && !(access&0x20000)) || ((type==2 || type==3) && !(access&0x10000)))result=invalid;
            else {
                const auto size=static_cast<std::size_t>(texture->texture_desc[0])*texture->texture_desc[1]*3/2;
                auto& pixels=texture->pixels[index];
                if(pixels.empty()){
                    std::vector<std::uint8_t> materialized;const auto& frame=texture->video_frames[index];
                    if(frame.native){auto source=frame.native->copy_nv12();const auto width=texture->texture_desc[0],height=texture->texture_desc[1];
                        if(frame.width==width && frame.height==height)materialized=std::move(source);
                        else {materialized.resize(size);
                            for(unsigned y=0;y<frame.height;++y)std::copy_n(source.begin()+static_cast<std::ptrdiff_t>(y*frame.width),frame.width,materialized.begin()+static_cast<std::ptrdiff_t>(y*width));
                            for(unsigned y=0;y<frame.height/2;++y)std::copy_n(source.begin()+static_cast<std::ptrdiff_t>((frame.height+y)*frame.width),frame.width,materialized.begin()+static_cast<std::ptrdiff_t>((height+y)*width));
                        }
                    }else materialized.resize(size);
                    pixels=std::move(materialized);
                }
                texture->allocations.reserve(texture->allocations.size()+1);
                const auto pointer=allocate_heap(memory,size);if(!pointer)throw std::bad_alloc{};
                try {memory.write(GuestAddress{pointer},pixels);}catch(...) {(void)free_heap(memory,pointer,state);throw;}
                texture->allocations.push_back(pointer);texture->subresource_maps.emplace(index,Identity::SubresourceMap{pointer,size,type});
                memory.store(GuestAddress{arg(5)},pointer_bits,pointer);memory.store(GuestAddress{arg(5)+pointer_size},32,texture->texture_desc[0]);memory.store(GuestAddress{arg(5)+pointer_size+4},32,size);
            }
        } else if(symbol=="Unmap") {
            auto context=typed(arg(0),Kind::Context),texture=typed(arg(1),Kind::Texture);
            const auto index=u32(2);const auto found=texture->subresource_maps.find(index);
            if(context->device!=texture->device || found==texture->subresource_maps.end())throw HostCallFault("D3D11 Unmap 沒有對應的 Map");
            const auto mapping=found->second;
            if(mapping.type!=1){texture->pixels[index]=read_bytes(memory,mapping.pointer,mapping.size);texture->video_frames[index]={};++texture->generations[index];}
            if(!free_heap(memory,mapping.pointer,state))throw HostCallFault("D3D11 Map 記憶體已被客體破壞");
            std::erase(texture->allocations,mapping.pointer);texture->subresource_maps.erase(found);
        } else if(symbol=="Flush" || symbol=="GetContextType" || symbol=="GetContextFlags") {(void)typed(arg(0),Kind::Context);}
        else throw HostCallFault("尚未實作的 D3D11 服務："+std::string(symbol));
    } catch(const std::bad_alloc&){
        const auto gpu=gpu_memory_stats();
        bridge.last_error=std::string(symbol)+"：宿主、客體 heap 或 GPU 配置失敗；live_interfaces="+std::to_string(bridge.objects.size())+
            " gpu_bytes="+std::to_string(gpu.used)+"/"+std::to_string(gpu.limit);
        if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d11] %s\n",bridge.last_error.c_str());
        if(symbol=="Enter" || symbol=="CopyResource" || symbol=="CopySubresourceRegion" || symbol=="GetImmediateContext")throw HostCallFault(bridge.last_error);
        result=out_of_memory;
    }
      catch(const texture_image::Error& error){result=error.hresult;bridge.last_error=error.what();}
      catch(const video::VideoCodecError& error){result=failure;bridge.last_error=error.what();}
      catch(const video::DxvaH264Error& error){result=not_supported;bridge.last_error=error.what();}
      catch(const WindowPresentationFault& error){result=0x887a0001U;bridge.last_error=error.what();record_diagnostic("d3d11_present_fault");set_diagnostic("d3d11_present_fault_last",error.what());if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d11] %.*s: %s\n",static_cast<int>(symbol.size()),symbol.data(),error.what());}
      catch(const std::invalid_argument& error){bridge.last_error=error.what();if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d11] %.*s: %s\n",static_cast<int>(symbol.size()),symbol.data(),error.what());if(void_methods.contains(symbol))throw HostCallFault(bridge.last_error);result=invalid;}
    if(effect_compile){const auto hresult=original_arg(service.symbol.find("FromFile")!=std::string_view::npos?11:13);if(hresult)memory.store(GuestAddress{hresult},32,result);}
    if(symbol.starts_with("D3DX11GetImageInfoFrom") || symbol.starts_with("D3DX11CreateTextureFrom") || symbol.starts_with("D3DX11CreateShaderResourceViewFrom")){
        const bool info=symbol.starts_with("D3DX11GetImageInfo"),file=symbol.find("FromFile")!=std::string_view::npos;
        const unsigned pump=(info?0U:2U)+(file?1U:2U);
        if(!arg(pump) && arg(pump+2))memory.store(GuestAddress{arg(pump+2)},32,result);
    }
    if(symbol.starts_with("D3DX11") && (result&0x80000000U) && std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))
        std::fprintf(stderr,"[d3dx11] %.*s HRESULT=%08x %s\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned>(result),bridge.last_error.c_str());
    if((result&0x80000000U) && std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS") && symbol=="CreateTexture2D" && arg(1)){
        std::fprintf(stderr,"[d3d11] CreateTexture2D HRESULT=%08x desc=",static_cast<unsigned>(result));
        for(unsigned i=0;i<11;++i)std::fprintf(stderr,"%s%x",i?",":"",static_cast<unsigned>(memory.load(GuestAddress{arg(1)+i*4},32)));
        std::fprintf(stderr," error=%s\n",bridge.last_error.c_str());
    }
    if(std::getenv("X64ARM_TRACE_DXGI_STATE") && service.family=="internal-d3d11"){
        const bool present=symbol=="Present" || symbol=="Present1";
        const bool change=symbol=="SetFullscreenState" || symbol=="ResizeTarget" || symbol=="ResizeBuffers" || symbol=="EnumOutputs" || symbol=="GetContainingOutput";
        if(present || change){const auto id=object(arg(0)).identity;const auto now=std::chrono::steady_clock::now();
            if((result&0x80000000U) || change || !id->present_trace.time_since_epoch().count() || now-id->present_trace>=std::chrono::seconds(5)){
                if(present)id->present_trace=now;
                std::fprintf(stderr,"[dxgi] %.*s this=%llx result=%llx a1=%llx a2=%llx frames=%llu fullscreen=%u\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(arg(0)),static_cast<unsigned long long>(result),static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(arg(2)),static_cast<unsigned long long>(id->frames),id->fullscreen);
                if((result&0x80000000U) && !bridge.last_error.empty())std::fprintf(stderr,"[dxgi] error=%s\n",bridge.last_error.c_str());
            }
        }
    }
    if(finish_adapter && !(result&0x80000000U))finish_adapter();
    original_arg.return_to(state,result);return std::nullopt;
}
}
