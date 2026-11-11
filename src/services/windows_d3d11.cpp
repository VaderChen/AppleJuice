#include "parallel_mapping_lock.hpp"
#include "x64arm/entry_runtime_profile.hpp"
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>
#include <thread>
#include "x64arm/entry_runtime_profile.hpp"
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
#include <shared_mutex>
#include <unordered_map>
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
enum class Kind { Mesh10, MeshBuffer10, Sprite10, Font10, Effect10, EffectType10, EffectVariable10, EffectScalar10, EffectVector10, EffectMatrix10, EffectString10, EffectResource10, EffectRenderTarget10, EffectDepthView10, EffectBuffer10, EffectShader10, EffectBlend10, EffectDepth10, EffectRaster10, EffectSampler10, EffectTechnique10, EffectPass10, StateBlock10, ClassLinkage, ClassInstance, Reflection, ReflectionBuffer, ReflectionVariable, ReflectionType, Device, Context, VideoDevice, VideoContext, Multithread, Texture, Decoder, View, Factory, Adapter, BufferObject, Texture1D, Texture3D, ShaderResource, Unordered, RenderTarget, DepthView, VertexShader, PixelShader, ComputeShader, GeometryShader, HullShader, DomainShader, Layout, Blend, DepthState, Raster, Sampler, Query, Predicate, CommandList, DxgiDevice, SwapChain, Output, Blob, DxgiResource, Fence, Annotation };
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
// invoke_d3d11 內以值捕獲 GraphicsDraw（約 62 KB）／GraphicsStage（約 10 KB）的錄製命令：若在巨型分派函式內建立 lambda 暫存，
// 每次 D3D11 呼叫（含最常見的簡單方法）的堆疊框架都包含這些空間並要逐頁探測。改在不內聯的小函式以堆積快照建立，
// 快照時點與內容不變（呼叫當下的繪製狀態）。
[[gnu::noinline]] RecordedGraphicsCommand recorded_draw(const GraphicsDraw& draw){
    auto copy=std::make_shared<const GraphicsDraw>(draw);
    // 快照之後不再改變：重播時交給後端直接排隊，不再複製（同一命令清單可重複執行，共用同一份唯讀快照）。
    return {[copy](NativeOpenGlContext& native){native.graphics_draw_shared(copy);},1,0};
}
[[gnu::noinline]] RecordedGraphicsCommand recorded_draw_auto(const GraphicsDraw& draw){
    auto copy=std::make_shared<const GraphicsDraw>(draw);
    // SO 計數由命令執行緒更新；提交當下讀取會早於前面的 SO 繪製。
    // 每次重播另建快照，保留同一 command list 重複執行的狀態獨立性。
    return {[copy](NativeOpenGlContext& native){native.graphics_post([copy](NativeGraphics& ordered){
        auto current=std::make_shared<GraphicsDraw>(*copy);
        current->count=static_cast<std::uint32_t>(current->vertices[0].resource->stream_vertices);
        ordered.graphics_draw(*current);
    });},1,0};
}
[[gnu::noinline]] RecordedGraphicsCommand recorded_dispatch(const GraphicsStage& stage,std::array<std::uint32_t,3> groups){
    auto copy=std::make_shared<const GraphicsStage>(stage);
    return {[copy,groups](NativeOpenGlContext& native){native.graphics_dispatch_shared(copy,groups);},0,1};
}
[[gnu::noinline]] RecordedGraphicsCommand recorded_dispatch_indirect(const GraphicsStage& stage,std::shared_ptr<GraphicsResource> resource,std::uint32_t offset){
    auto copy=std::make_shared<const GraphicsStage>(stage);
    return {[copy,resource=std::move(resource),offset](NativeOpenGlContext& native){native.graphics_dispatch_indirect_shared(copy,resource,offset);},0,1};
}
// 繪製狀態快取輪替份數：重用某份前要等命令執行緒處理完上一次用它的繪製；份數越多越能吸收突發，但增量更新要與更舊的
// 狀態比對。X64ARM_D3D11_DRAW_CACHES=<1～32>，預設 32：份數太少時，大量繪製突發會讓前端很快就追上命令執行緒，
// 只能等待。快取用到才配置。
unsigned d3d11_draw_cache_count() noexcept {
    static const unsigned value=[] {const auto* text=std::getenv("X64ARM_D3D11_DRAW_CACHES");
        const auto parsed=text && *text?std::strtoul(text,nullptr,10):32UL;return static_cast<unsigned>(std::clamp<unsigned long>(parsed,1,32));}();
    return value;
}
// 立即內容的遮蔽查詢共用可見度池：Metal 每個 render pass 只能掛一個可見度緩衝，每個查詢各自持有緩衝時，查詢 draw
// 前後都要換 encoder（DQ3 遊戲內每幀約 128 次、占 encoder 的四分之三）。X64ARM_D3D11_VISIBILITY_POOL=0 回到每查詢緩衝。
bool d3d11_visibility_pool_enabled() noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_VISIBILITY_POOL");return !value || std::string_view(value)!="0";}();
    return enabled;
}
constexpr std::uint32_t d3d11_visibility_pool_bytes=65536; // 與原每查詢緩衝相同（8192 槽），在 Metal 可見度偏移範圍內。
struct DeferredGraphicsMap {
    GraphicsView view;
    std::vector<std::uint8_t> bytes;
    std::uint64_t pointer{};
    std::uint32_t row{},image{};
};
// 立即 context 的繪製狀態快取：保留上次解析的 GraphicsDraw 與各槽位的原始客體指標，
// 繪製時只重新解析指標有變動的槽位，不再每次重建約 65 KB 的狀態與 816 個 view。
// 非弱綁定 context 持有已綁定物件的引用，物件不會在綁定中被銷毀；任何 D3D11 物件銷毀後整份重新解析，
// 避免解除綁定後同一位址建立的新物件沿用舊解析結果。
struct D3D11DrawCache {
    GraphicsDraw draw;
    std::uint64_t destroyed{UINT64_MAX};
    std::array<std::array<std::vector<std::uint64_t>,4>,6> stage_raw; // ConstantBuffers、ShaderResources、Samplers、UnorderedAccessViews。
    std::array<std::uint64_t,6> shaders{};
    std::array<bool,6> present{};
    std::vector<std::uint64_t> stream_raw,vertex_raw,target_raw;
    std::uint64_t layout{},index{},depth{},blend{},depth_state{},raster{};
    std::uint64_t ticket{}; // 最後一次以參照交給後端的命令序號（同步後端恆為 0）。
};
// 固定名稱綁定的節點指標（8 個共用名稱＋6 stage 各 5 個名稱＋繪製狀態 5 個名稱，見 d3d11_setter_front_path.inc）。
using D3D11FixedBindings=std::array<std::vector<std::uint64_t>*,43>;
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
    std::uint64_t pipeline_state_serial{};
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
    // 最後一次 Present 的 QPC 時間（10 MHz）、IDXGISwapChain1 背景色（DXGI_RGBA）與旋轉。
    std::uint64_t present_ticks{};std::array<std::uint8_t,16> background_color{};std::uint32_t rotation{1};
    std::shared_ptr<GraphicsShader> shader;
    std::shared_ptr<GraphicsSampler> sampler;
    GraphicsView graphics_view;
    std::vector<GraphicsElement> layout;
    std::map<std::string,std::vector<std::uint64_t>> bindings;
    // 前置快路徑的 bindings 節點快取：std::map 節點位址穩定，依名稱存取與快取指向同一向量。只建立在非弱綁定的 D3D11 裝置內容上
    //（D3D10 sprite 會整份換掉的是弱綁定內容）；bindings 整體被移走時（clear_bindings）要一併清掉。
    std::unique_ptr<D3D11FixedBindings> fixed_bindings;
    // 多份輪流使用：Metal 命令執行緒可能仍以參照讀取先前繪製的那幾份（見 draw_state）。
    // 輪流使用的繪製狀態快取（實際份數見 d3d11_draw_cache_count，上限 32）。
    std::array<std::unique_ptr<D3D11DrawCache>,32> draw_caches;unsigned draw_slot{};
    std::map<Guid,std::vector<std::uint8_t>> private_data;
    std::map<Guid,std::uint64_t> private_interfaces;
    std::vector<std::array<float,6>> viewports;
    std::vector<std::array<std::int32_t,4>> scissors;
    std::array<float,4> blend_factor{1,1,1,1};
    std::array<std::uint32_t,32> strides{},offsets{};
    std::array<std::uint32_t,4> stream_offsets{};
    std::shared_ptr<GraphicsStreamStatistics> stream_statistics{std::make_shared<GraphicsStreamStatistics>()};
    GraphicsStreamStatistics stream_begin;
    bool warp_adapter{}; // 配接器：EnumWarpAdapter 建立的 WARP 身分（見 display_identity::warp_adapter_description）。
    // 管線統計：內容的累計計數、查詢 Begin／End 時的快照，以及內容上進行中的管線統計查詢（root 指標）。
    std::shared_ptr<GraphicsPipelineStatistics> pipeline_statistics{std::make_shared<GraphicsPipelineStatistics>()};
    GraphicsPipelineStatistics pipeline_begin,pipeline_end;
    std::vector<std::uint64_t> pipeline_queries;
    std::uint32_t topology{},index_format{42},index_offset{},sample_mask{UINT32_MAX},stencil_reference{},priority{},query_size{},context_type{};
    std::uint64_t window{},factory{},adapter{},frames{},draws{},dispatches{};
    std::vector<RecordedGraphicsCommand> commands;
    std::map<std::pair<std::uint64_t,std::uint32_t>,DeferredGraphicsMap> deferred_maps;
    bool offered{};
    std::vector<std::uint8_t> query_data;
    // 事件查詢（D3D11_QUERY_EVENT）以後端完成旗標判定就緒，End 不等待 GPU。
    std::shared_ptr<const std::atomic<bool>> query_fence;
    // staging 資源最後一次 GPU 存取（Copy*／UpdateSubresource／Unmap 上傳）的完成旗標，依序排在該命令之後。
    // Map 帶 D3D11_MAP_FLAG_DO_NOT_WAIT 時未完成即回 DXGI_ERROR_WAS_STILL_DRAWING；已完成的緩衝讀回不必等命令執行緒排空。
    std::shared_ptr<const std::atomic<bool>> gpu_fence;
    std::shared_ptr<const GraphicsTimestampFuture> query_timestamp;
    // 遮蔽查詢：End 只記錄 fence 與取樣序號，GetData 在 fence 完成後才讀回可見度緩衝區。
    std::uint64_t query_pending_sequence{};
    std::uint32_t visibility_planned{}; // 遮蔽查詢可見度緩衝已排入清零的位元組數（Begin 歸零）。
    // 立即內容的 OCCLUSION 查詢改用裝置內容共用的可見度池（d3d11_visibility_pool_enabled）：槽位只遞增不重用，
    // 池建立時已清零；查詢以 {緩衝, 起始位元組, 槽數} 記錄取樣範圍（池用完換新池時分段）。
    struct VisibilitySegment {std::shared_ptr<GraphicsResource> buffer;std::uint32_t base{},count{};};
    std::vector<VisibilitySegment> visibility_segments;
    std::shared_ptr<GraphicsResource> visibility_pool; // 裝置內容：目前的可見度池。
    std::uint32_t visibility_cursor{}; // 裝置內容：池內下一個可用位元組。
    bool visibility_pooled{}; // 查詢：本次 Begin 使用共用池。
    std::uint32_t query_polls{}; // GetData 連續輪詢仍未完成的次數；達門檻時的保底提交見 GetData，避免永不完成。
    std::uint64_t query_end_ticket{}; // 查詢：End 排入命令後的命令序號（0：同步後端，或延遲內容的 End 尚未重播）。
    std::uint64_t flush_ticket{}; // 立即內容：最後一次非等待提交排入後的命令序號；不大於此值的命令都已被提交涵蓋。
    bool query_active{},query_ready{},fullscreen{};
    std::uint64_t fullscreen_output{};
    std::array<std::int32_t,4> windowed_rect{};
    std::uint32_t windowed_style{},windowed_ex_style{};
    std::optional<std::array<std::uint32_t,4>> windowed_insets;
    std::vector<std::uint8_t> target_mode;
    std::chrono::steady_clock::time_point present_trace{};
    float min_lod{};
    // D3D11.1 *SetConstantBuffers1：各 stage 14 槽的起始常數與常數數（單位 16 位元組），數量 0 表示整個緩衝。
    std::array<std::array<std::uint32_t,14>,6> constant_first{},constant_count{};
    // ID3D11Query1 的 ContextType、ID3D11DeviceContext3 的硬體保護狀態、ID3D11Fence 已完成值與待觸發事件（值, 事件）。
    std::uint32_t query_context{};bool hardware_protection{};
    std::uint64_t fence_value{};std::vector<std::pair<std::uint64_t,std::uint64_t>> fence_events;

    std::uint64_t root{}, device{}, texture{}, current_view{}, mapped{}, lock_address{};
    // COM 引用計數所在的客體格位（見 reference_count）；建立根介面時配置。
    std::uint64_t counter{};
    // 管線／交換鏈保有的資源生命週期不計入客體 COM AddRef／Release 的回傳值。
    // 內部持有者只在 graphics_mutex 下修改；外部計數仍共用 resident AOT 的原子格位。
    std::uint32_t internal_references{};
    bool internal_texture{};
    std::uint32_t slice{}, creation_flags{}, feature_level{0xb000}, exception_mode{};
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
// COM 引用計數存放在客體記憶體：每個身分一格 32 位元（建立根介面時配置、初值 1），各介面物件 [this+指標寬] 指向它。
// x64 的 AddRef 與非最後一次 Release 由客體常式直接以原子指令完成（guest_thread_x64.s 的 ComBridgeAddRef／ComBridgeRelease），
// 宿主讀寫同一格也一律使用原子操作；格位不在可原生存取的單頁內時退回一般讀寫（此時沒有客體常式能同時存取）。
std::uint32_t* reference_cell(GuestMemory& memory,const Identity& id) noexcept {
    const auto window=memory.native_window(GuestAddress{id.counter},3);
    return window.data && window.after>=4 && !(reinterpret_cast<std::uintptr_t>(window.data)&3U)?reinterpret_cast<std::uint32_t*>(window.data):nullptr;
}
std::uint32_t reference_count(GuestMemory& memory,const Identity& id) {
    if(auto* cell=reference_cell(memory,id))return std::atomic_ref(*cell).load(std::memory_order_acquire);
    return static_cast<std::uint32_t>(memory.load(GuestAddress{id.counter},32));
}
// 原子地加一或減一並回傳新值。加一時已達上限、減一時不大於 floor 則不修改並回傳空值
//（floor=1 只做非最後一次 Release；floor=0 允許降到 0）。
std::optional<std::uint32_t> adjust_references(GuestMemory& memory,const Identity& id,bool increment,std::uint32_t floor=0) {
    const auto blocked=[&](std::uint32_t value){return increment?value==UINT32_MAX:value<=floor;};
    if(auto* cell=reference_cell(memory,id)){
        std::atomic_ref value(*cell);auto current=value.load(std::memory_order_relaxed);
        do{if(blocked(current))return std::nullopt;}
        while(!value.compare_exchange_weak(current,increment?current+1:current-1,std::memory_order_acq_rel,std::memory_order_relaxed));
        return increment?current+1:current-1;
    }
    const auto current=static_cast<std::uint32_t>(memory.load(GuestAddress{id.counter},32));
    if(blocked(current))return std::nullopt;
    const auto next=increment?current+1:current-1;memory.store(GuestAddress{id.counter},32,next);return next;
}
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
    switch(k){case Kind::ClassLinkage:case Kind::ClassInstance:case Kind::Context:case Kind::VideoContext:case Kind::Texture:case Kind::Texture1D:case Kind::Texture3D:case Kind::BufferObject:case Kind::Decoder:case Kind::View:case Kind::ShaderResource:case Kind::Unordered:case Kind::RenderTarget:case Kind::DepthView:case Kind::VertexShader:case Kind::PixelShader:case Kind::ComputeShader:case Kind::GeometryShader:case Kind::HullShader:case Kind::DomainShader:case Kind::Layout:case Kind::Blend:case Kind::DepthState:case Kind::Raster:case Kind::Sampler:case Kind::Query:case Kind::Predicate:case Kind::CommandList:case Kind::Fence:return true;default:return false;}
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

// COM 介面指標 → 物件。UE 每幀查表約 9 萬次（每次繪製十多次、每次狀態設定數次）；unordered_map 要取餘數再追指標，
// find_fast 先查直接對映快取，命中只需比對一次。快取存 iterator：插入造成 rehash 時全部清除，刪除時清掉對應項。
// find_fast 會寫快取，只可在持圖形鎖的路徑使用；未持鎖的路徑維持一般 find（不寫入）。
class D3D11ObjectTable : public std::unordered_map<std::uint64_t,Object> {
    using Base=std::unordered_map<std::uint64_t,Object>;
    static constexpr std::size_t cache_size=4096;
    struct Entry {std::uint64_t key{};Base::iterator value{};};
    mutable std::array<Entry,cache_size> cache_{};
    static std::size_t index(std::uint64_t key) noexcept {return static_cast<std::size_t>((key>>4)^(key>>16))&(cache_size-1);}
public:
    iterator find_fast(std::uint64_t key) {
        auto& entry=cache_[index(key)];
        if(key && entry.key==key)return entry.value;
        const auto found=Base::find(key);
        if(key && found!=Base::end())entry=Entry{key,found};
        return found;
    }
    const_iterator find_fast(std::uint64_t key) const {return const_cast<D3D11ObjectTable*>(this)->find_fast(key);}
    // 結構變動（插入、刪除）取獨占的 structure_；不持圖形鎖的查找（find_kind_unlocked）取共用鎖。worker 上的 CreateBuffer 會在只持
    // 映射共用鎖＋圖形鎖時插入，與同樣只持映射共用鎖的介面卡查詢（AdapterGetDesc、QueryVideoMemoryInfo）並行；持圖形鎖的查找不受影響。
    template<class... Arguments> auto emplace(Arguments&&... arguments){
        const std::unique_lock guard(structure_);
        const auto buckets=bucket_count();
        auto result=Base::emplace(std::forward<Arguments>(arguments)...);
        if(bucket_count()!=buckets)cache_.fill(Entry{});
        return result;
    }
    size_type erase(std::uint64_t key){
        const std::unique_lock guard(structure_);
        auto& entry=cache_[index(key)];if(entry.key==key)entry=Entry{};
        return Base::erase(key);
    }
    bool find_kind_unlocked(std::uint64_t key,Kind kind) const {
        const std::shared_lock guard(structure_);
        const auto found=Base::find(key);
        return found!=Base::end() && found->second.kind==kind;
    }
    // 硬體配接器（不含 WARP）：worker 上的配接器描述與顯存查詢只處理硬體配接器。
    bool find_hardware_adapter_unlocked(std::uint64_t key) const {
        const std::shared_lock guard(structure_);
        const auto found=Base::find(key);
        return found!=Base::end() && found->second.kind==Kind::Adapter && found->second.identity && !found->second.identity->warp_adapter;
    }
private:
    mutable std::shared_mutex structure_;
};
struct D3D11SharedResource {std::weak_ptr<GraphicsResource> resource;std::weak_ptr<NativeOpenGlContext> producer;std::vector<std::uint8_t> desc;};
struct D3D11State {
    std::map<std::uint32_t,D3D11SharedResource> shared_resources;
    std::uint32_t next_shared_resource{0xa6000000};
    std::map<std::uint64_t,D3DCompileOperation> compilations;
    std::uint64_t next_compilation{};
    // 每個 D3D11 呼叫都以介面指標查表數次；順序不影響語義（迭代點皆為全體掃描或唯一匹配）。
    D3D11ObjectTable objects;
    std::map<std::pair<Kind,bool>,std::uint64_t> vtables;
    std::uint64_t hardware_sessions{}, decoded_frames{};
    std::string last_error;
    std::uint64_t graphics_draws{},graphics_dispatches{},graphics_frames{};
    // IDXGIOutput::TakeOwnership 的持有裝置（單一公開輸出）；ReleaseOwnership 或裝置釋放前保留。
    std::uint64_t output_owner{};bool output_owner_exclusive{};
    std::uint64_t linkage_draws{},linkage_cache_hits{},linkage_uploads{},linkage_readback_bytes{};
    std::uint64_t destroyed_objects{},draw_cache_full{},draw_cache_incremental{};
    std::array<std::uint64_t,6> unmap_calls{},unmap_bytes{}; // 依 Map 類型累計上傳次數與位元組（診斷 X64ARM_TRACE_GRAPHICS_STATS）。
    // 診斷：狀態設定前置快路徑完成與交回完整路徑的次數（見 d3d11_setter_front_path.inc）。
    std::uint64_t setter_front_hits{},setter_front_fallbacks{};
    std::uint64_t draw_front_hits{},draw_front_fallbacks{};
    // 診斷：遊戲內各 D3D11 方法建立後端資源的次數（隨 X64ARM_TRACE_GRAPHICS_STATS 輸出）。
    std::map<std::string,std::uint64_t,std::less<>> resource_creations;
    // 本地協調服務中引用歸零的物件：延到下一次非本地 D3D11 呼叫再銷毀（銷毀會釋放 heap）。
    std::vector<std::pair<std::uint64_t,bool>> deferred_releases;
    std::size_t peak_interfaces{};
    // 介面物件與引用計數格（各 16 bytes 的客體 heap 區塊）：物件銷毀時保留在這裡，下一次建立介面直接取用並清零，
    // 不經 allocate_heap／free_heap 的空洞搜尋、頁計數與相鄰合併（雙人成行每幀約 40 次 CreateBuffer／Release，兩者約占協調者 2%）。
    // 區塊仍登錄為存活配置；最後一個 D3D11 物件釋放時全部歸還。上限 4096 個。X64ARM_D3D11_SLOT_REUSE=0 關閉。
    std::vector<std::uint64_t> free_slots;
    // 持久映射區塊（動態資源第一次 WRITE_DISCARD 對應配置的客體 heap 區塊）：資源銷毀或映射失效時依大小保留在這裡，下一次第一次對應
    // 同大小的子資源直接取用，不經 allocate_heap／free_heap（《崛起》繪製執行緒每幀數次建立 64 KiB 動態常數緩衝，協調者上每次配置約 68 µs）。
    // worker 只能從這裡取（不配置 heap），見 d3d11_local_eligible。區塊仍登錄為存活配置；最後一個 D3D11 物件釋放時全部歸還。
    // 每種大小最多 512 塊、總量 64 MiB、單塊 4 MiB 以下。X64ARM_D3D11_MAP_BLOCK_REUSE=0 關閉。
    std::unordered_map<std::size_t,std::vector<std::uint64_t>> map_blocks;
    std::size_t map_block_bytes{};
    // 程序結束時把每個 D3D11 裝置的 Metal 後端統計（draws、encoders、GPU 忙碌時間、CPU 等待）印到 stderr，
    // 供 CPU／GPU 瓶頸判斷；只讀統計字串，不改任何狀態。
    ~D3D11State(){
        try{
            for(const auto& [pointer,object]:objects)
                if(object.kind==Kind::Device && object.identity && object.identity->native && object.identity->root==pointer)
                    std::fprintf(stderr,"[d3d11_metal] device=0x%llx draws=%llu frames=%llu %s\n",static_cast<unsigned long long>(pointer),
                        static_cast<unsigned long long>(graphics_draws),static_cast<unsigned long long>(graphics_frames),object.identity->native->statistics().c_str());
        }catch(...){}
    }
};
namespace {
#include "d3d11_setter_front_path.inc"
#include "d3d11_draw_front_path.inc"
}
#include "d3d11_command_stream.inc"
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
    if(d3d11_->draw_cache_full || d3d11_->draw_cache_incremental)out<<"d3d11_draw_cache full="<<d3d11_->draw_cache_full<<" incremental="<<d3d11_->draw_cache_incremental<<" destroyed="<<d3d11_->destroyed_objects<<'\n';
    if(d3d11_->setter_front_hits || d3d11_->setter_front_fallbacks)out<<"d3d11_setter_front hits="<<d3d11_->setter_front_hits<<" fallbacks="<<d3d11_->setter_front_fallbacks<<'\n';
    if(d3d11_->draw_front_hits || d3d11_->draw_front_fallbacks)out<<"d3d11_draw_front hits="<<d3d11_->draw_front_hits<<" fallbacks="<<d3d11_->draw_front_fallbacks<<'\n';
    if(d3d11_->linkage_draws)out<<"d3d11_linkage draws="<<d3d11_->linkage_draws<<" cache_hits="<<d3d11_->linkage_cache_hits<<" uploads="<<d3d11_->linkage_uploads<<" readback_bytes="<<d3d11_->linkage_readback_bytes<<'\n';
    if(std::getenv("X64ARM_TRACE_GRAPHICS_LIFETIME"))for(const auto& [pointer,entry]:d3d11_->objects){const auto& id=*entry.identity;out<<"d3d11_live pointer="<<pointer<<" root="<<id.root<<" kind="<<static_cast<unsigned>(entry.kind)<<" counter=0x"<<std::hex<<id.counter<<std::dec<<" device="<<id.device<<" texture="<<id.texture<<"\n";}
    if(!d3d11_->last_error.empty())out<<"video_error="<<d3d11_->last_error<<'\n';
    return out.str();
}

std::span<const WindowsServiceDescriptor> d3d11_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        #include "d3d10_descriptors.inc"
#include "d3d10_effect_descriptors.inc"
#include "d3d10_sprite_descriptors.inc"
#include "d3d10_mesh_descriptors.inc"
        {"d3d11.dll","D3D11CreateDeviceAndSwapChain",12},{"dxgi.dll","CreateDXGIFactory2",3},{"dxgi.dll","DXGIGetDebugInterface1",3},
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

// 能否在其他 worker 執行期間完成（見 resident_coordinated_service）：Win32／Win64 共用 WindowsCall ABI；排除
// 建立介面、命令列表與開啟執行緒保護（裝置鎖可能要排程等待）的裝置；延後銷毀清單過長時
// 改走全體靜止的完整服務，順便清空清單。只讀取物件表，不改任何狀態。
// worker（共用映射鎖）上 Map／Unmap 只在重用既有持久映射時合格：立即內容上的 WRITE_DISCARD／NO_OVERWRITE 沿用同一
// 客體區塊，Unmap 上傳後保留區塊，都不配置或釋放 heap（每幀數千次，是平行模式全體靜止的最大來源）。協調者
//（coordinator=true）持獨占映射鎖，可配置與釋放 heap，Map／Unmap 與寫入持久映射資源的複製都可本地完成。
// worker 上完成 CreateBuffer 與動態緩衝第一次 WRITE_DISCARD 對應（槽與映射區塊取自預備池，見 take_slot／map_blocks）。
// 《崛起》地熱谷繪製執行緒每幀數次建立 64 KiB 動態常數緩衝後第一次對應，原本兩者都要往返協調者（負載下各約 100 µs）。
// X64ARM_D3D11_WORKER_CREATE=0 關閉。
bool d3d11_worker_create_enabled() noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_WORKER_CREATE");return !value || std::string_view(value)!="0";}();
    return enabled;
}
bool d3d11_map_block_reuse_enabled() noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_MAP_BLOCK_REUSE");return !value || std::string_view(value)!="0";}();
    return enabled;
}
bool WindowsServices::d3d11_adapter(std::uint64_t object) const{
    if(!d3d11_)return false;return d3d11_->objects.find_hardware_adapter_unlocked(object);
}
// 診斷：X64ARM_TRACE_D3D11_FULL=1 時記下最近一次本地資格的拒絕原因（只供完整路徑統計）。
thread_local const char* d3d11_local_refusal=nullptr;
bool WindowsServices::d3d11_local_eligible(const GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,bool coordinator) const try {
    d3d11_local_refusal="other";
    if(!d3d11_){d3d11_local_refusal="no-bridge";return false;}
    if(d3d11_->deferred_releases.size()>=256){d3d11_local_refusal="deferred-releases";return false;}
    const auto symbol=service.symbol;
    // 延遲命令清單的完成與重播（以及命令清單物件的引用計數）：協調者本地可配置／釋放 heap、立即銷毀物件，與本地 Create* 相同；
    // worker 上依下方條件完成（Fire Strike 每秒數千次，原本每次全停約 60 µs）。
    if(symbol=="QueryInterface")return false;
    // worker 上：FinishCommandList 只取預備槽建立命令清單物件（vtable 已快取、至少 6 個槽且頁面不是寫入時複製頁），
    // ExecuteCommandList 只在立即內容重播；釋放進延後清單。X64ARM_D3D11_COMMAND_LIST_WORKER=0 交回協調者本地路徑。
    if((symbol=="FinishCommandList" || symbol=="ExecuteCommandList") && !coordinator){
        static const bool worker_lists=[] {const auto* value=std::getenv("X64ARM_D3D11_COMMAND_LIST_WORKER");return !value || std::string_view(value)!="0";}();
        if(!worker_lists)return false;
        const WindowsCall call(state,memory,service);
        const auto self=d3d11_->objects.find_fast(call(0));if(self==d3d11_->objects.end() || self->second.kind!=Kind::Context || !self->second.identity)return false;
        if(symbol=="ExecuteCommandList"){if(self->second.identity->context_type)return false;}
        else{
            if(!self->second.identity->context_type || !d3d11_worker_create_enabled() || d3d11_->free_slots.size()<6 || !d3d11_->vtables.contains(std::pair{Kind::CommandList,false}))return false;
            for(std::size_t i=d3d11_->free_slots.size()-6;i<d3d11_->free_slots.size();++i)
                if(memory.is_copy_on_write(GuestAddress{d3d11_->free_slots[i]}) || memory.is_copy_on_write(GuestAddress{d3d11_->free_slots[i]+15}))return false;
        }
    }
    const WindowsCall arg(state,memory,service);
    const auto found=d3d11_->objects.find_fast(arg(0));if(found==d3d11_->objects.end())return false;
    // 著色器尚未編譯：交給背景編譯並回到完整服務路徑，由處理程式讓呼叫者等待後重試（見 d3d11_graphics_handlers.inc），
    // 不在持映射與圖形鎖的本地／worker 服務內轉譯（雙人成行走到新區域時每個約 106 ms，期間所有 D3D11 服務停住）。
    if(found->second.kind==Kind::Device && found->second.identity && found->second.identity->native && resident_context_key(state)!=present_thread_ &&
       (symbol=="CreateVertexShader" || symbol=="CreatePixelShader" || symbol=="CreateComputeShader" || symbol=="CreateGeometryShader" ||
        symbol=="CreateHullShader" || symbol=="CreateDomainShader") && arg(1) && arg(2)>0 && arg(2)<=16*1024*1024){
        const auto stage=symbol=="CreateVertexShader"?0U:symbol=="CreatePixelShader"?1U:symbol=="CreateComputeShader"?5U:symbol=="CreateGeometryShader"?2U:symbol=="CreateHullShader"?3U:4U;
        if(found->second.identity->native->graphics_shader_prefetch(stage,read_bytes(memory,arg(1),static_cast<std::size_t>(arg(2)))))return false;
    }
    // DXGI 介面卡的顯示記憶體預算查詢與保留：只讀寫自帶鎖的 GPU 額度池與參數指向的客體記憶體（電馭判客每秒約 500 次）。
    if(symbol=="QueryVideoMemoryInfo" || symbol=="SetVideoMemoryReservation")return found->second.kind==Kind::Adapter;
    if(!coordinator && (symbol=="CopyResource" || symbol=="CopySubresourceRegion" || symbol=="UpdateSubresource")){
        // 寫入目的資源會撤銷持久映射並釋放客體 heap；在任何副作用之前交回完整服務邊界。
        const auto destination=d3d11_->objects.find_fast(arg(1));
        if(destination!=d3d11_->objects.end() && !destination->second.identity->persistent_maps.empty())return false;
    }
    if(symbol=="Map" || symbol=="Unmap"){
        const auto resource=d3d11_->objects.find_fast(arg(1));
        if(found->second.kind==Kind::Context && found->second.identity->context_type){
            // 延遲內容：Map 的錄製區塊取自 map_blocks、Unmap 放回（見 d3d11_deferred_handlers.inc）。worker 上只在不必配置／釋放 heap 時完成：
            // Map 要有同大小且不是寫入時複製頁的保留區塊，Unmap 要放得回去；否則交回協調者本地路徑（配置 heap 並錄製命令）。
            // Fire Strike 錄製執行緒每秒數萬次，原本每次往返協調者。X64ARM_D3D11_DEFERRED_MAP_WORKER=0 恢復一律交回。
            if(resource==d3d11_->objects.end() || !resource->second.identity->resource)return false;
            if(!coordinator){
                static const bool deferred_worker=[] {const auto* value=std::getenv("X64ARM_D3D11_DEFERRED_MAP_WORKER");return !value || std::string_view(value)!="0";}();
                if(!deferred_worker || !d3d11_map_block_reuse_enabled())return false;
                const auto& context=*found->second.identity;const auto& id=*resource->second.identity;const auto& d=id.resource->desc;
                const auto index=static_cast<std::uint32_t>(arg(2));
                if(!d.levels || index>=d.levels*d.layers)return false;
                const auto found_map=context.deferred_maps.find(std::pair{id.root,index});
                if(symbol=="Map"){
                    const auto type=static_cast<std::uint32_t>(arg(3));if(type!=4 && type!=5)return false;
                    const auto level=index%d.levels;
                    const auto pitch=d.dimension==1?std::array{d.width,1U}:graphics_format_pitch(d.format,std::max(1U,d.width>>level),std::max(1U,d.height>>level));
                    const auto size=std::uint64_t{pitch[0]}*pitch[1]*std::max(1U,d.depth>>level);
                    if(!size || size>(std::uint64_t{4}<<20))return false;
                    const auto blocks=d3d11_->map_blocks.find(static_cast<std::size_t>(size));
                    if(blocks==d3d11_->map_blocks.end() || blocks->second.empty())return false;
                    const auto block=blocks->second.back();
                    if(memory.is_copy_on_write(GuestAddress{block}) || memory.is_copy_on_write(GuestAddress{block+size-1}))return false;
                }else{
                    if(found_map==context.deferred_maps.end() || !found_map->second.pointer)return false;
                    const auto size=found_map->second.bytes.size();
                    if(!size || size>(std::size_t{4}<<20) || d3d11_->map_block_bytes+size>(std::size_t{64}<<20))return false;
                    const auto live=allocations_.find(found_map->second.pointer);if(live==allocations_.end() || live->second!=size)return false;
                    if(const auto blocks=d3d11_->map_blocks.find(size);blocks!=d3d11_->map_blocks.end() && blocks->second.size()>=512)return false;
                }
            }
        }else if(coordinator){
            // 立即內容上的對應：一般完整服務同樣在協調執行緒執行（讀回會等命令執行緒與 GPU）。
            if(found->second.kind!=Kind::Context || found->second.identity->context_type || resource==d3d11_->objects.end() ||
               !resource->second.identity->resource)return false;
        }else{
            if(found->second.kind!=Kind::Context || found->second.identity->context_type || resource==d3d11_->objects.end() ||
               !resource->second.identity->resource)return false;
            const auto& id=*resource->second.identity;const auto subresource=static_cast<std::uint32_t>(arg(2));
            const auto kept=id.persistent_maps.find(subresource);
            if(kept==id.persistent_maps.end()){
                // 動態緩衝第一次 WRITE_DISCARD：同大小的保留區塊存在、頁面不是寫入時複製頁才在 worker 完成（不配置 heap）。
                // 規格檢查不符的呼叫交協調者，由完整檢查回報錯誤。
                if(symbol!="Map" || !d3d11_worker_create_enabled() || !d3d11_map_block_reuse_enabled())return false;
                static const bool persistent=[] {const auto* value=std::getenv("X64ARM_D3D11_PERSISTENT_MAP");return !value || std::string_view(value)!="0";}();
                static const bool discard=[] {const auto* value=std::getenv("X64ARM_GRAPHICS_DISCARD_RENAME");return !value || std::string_view(value)!="0";}();
                const auto type=static_cast<std::uint32_t>(arg(3)),flags=static_cast<std::uint32_t>(arg(4));
                const auto& d=id.resource->desc;
                if(!persistent || !discard || type!=4 || (flags&~0x100000U) || subresource || id.offered || id.subresource_maps.contains(0) ||
                   d.dimension!=1 || !d.width || id.desc.size()<16)return false;
                const auto word=[&](std::size_t at){std::uint32_t value=0;std::memcpy(&value,id.desc.data()+at,4);return value;};
                if(word(4)!=2 || !(word(12)&0x10000U))return false;
                const auto blocks=d3d11_->map_blocks.find(d.width);
                if(blocks==d3d11_->map_blocks.end() || blocks->second.empty())return false;
                const auto block=blocks->second.back();
                if(memory.is_copy_on_write(GuestAddress{block}) || memory.is_copy_on_write(GuestAddress{block+d.width-1}))return false;
            }else if(symbol=="Map"){const auto type=static_cast<std::uint32_t>(arg(3));if(type!=4 && type!=5)return false;}
            else{const auto mapped=id.subresource_maps.find(subresource);if(mapped==id.subresource_maps.end() || mapped->second.pointer!=kept->second.pointer)return false;}
        }
    }
    // worker 上的 CreateBuffer：介面物件與引用計數格只取預備槽（take_slot），至少留 4 個且頁面不是寫入時複製頁。
    if(!coordinator && symbol=="CreateBuffer"){
        if(!d3d11_worker_create_enabled() || found->second.kind!=Kind::Device || d3d11_->free_slots.size()<4)return false;
        for(std::size_t i=d3d11_->free_slots.size()-4;i<d3d11_->free_slots.size();++i)
            if(memory.is_copy_on_write(GuestAddress{d3d11_->free_slots[i]}) || memory.is_copy_on_write(GuestAddress{d3d11_->free_slots[i]+15}))return false;
    }
    // 交換鏈的全螢幕狀態查詢（UE 每幀一次）只讀狀態；要取回輸出時會持有物件參照，交完整路徑。
    if(symbol=="GetFullscreenState")return found->second.kind==Kind::SwapChain && !arg(2);
    // DXGI 工廠的 IsCurrent 只核對物件後回 TRUE（介面卡集合不會變動），不改狀態、不持參照；D3D12 遊戲主迴圈每幀一次，
    // 原本每次全停（《古墓奇兵：暗影》中畫質 4 vCPU 每次約 52～87 µs）。
    if(symbol=="FactoryIsCurrent")return found->second.kind==Kind::Factory;
    // 只限裝置、裝置內容、資源與 view、著色器、佈局、狀態物件、查詢、class linkage 與命令列表；交換鏈、工廠、輸出、
    // DXGI 裝置、多執行緒鎖、反射與 D3D10 物件仍走完整服務。
    switch(found->second.kind){
    case Kind::Device:case Kind::Context:case Kind::Texture:case Kind::BufferObject:case Kind::Texture1D:case Kind::Texture3D:
    case Kind::ShaderResource:case Kind::Unordered:case Kind::RenderTarget:case Kind::DepthView:
    case Kind::VertexShader:case Kind::PixelShader:case Kind::ComputeShader:case Kind::GeometryShader:case Kind::HullShader:case Kind::DomainShader:
    case Kind::Layout:case Kind::Blend:case Kind::DepthState:case Kind::Raster:case Kind::Sampler:case Kind::Query:case Kind::Predicate:
    case Kind::ClassLinkage:case Kind::ClassInstance:break;
    case Kind::CommandList:{
        static const bool command_lists=[] {const auto* value=std::getenv("X64ARM_LOCAL_COMMAND_LISTS");return !value || std::string_view(value)!="0";}();
        if(!command_lists)return false;break;}
    default:return false;
    }
    const auto& id=*found->second.identity;
    if(id.api10){d3d11_local_refusal="api10";return false;}
    const auto device=id.device?d3d11_->objects.find_fast(id.device):found;if(device==d3d11_->objects.end()){d3d11_local_refusal="no-device";return false;}
    if(device->second.identity->protected_threads && device->second.identity->lock_address){d3d11_local_refusal="multithread-protected";return false;}
    d3d11_local_refusal=nullptr;return true;
}catch(const MemoryFault&){d3d11_local_refusal="fault";return false;}catch(const HostCallFault&){d3d11_local_refusal="fault";return false;}
std::recursive_mutex& WindowsServices::graphics_mutex() noexcept {static std::recursive_mutex value;return value;}
// 前置快速路徑：完整分派函式內聯了上百個方法與數十個繪製狀態暫存物件，堆疊框架很大，每次呼叫都要逐頁探測
// 堆疊與初始化；遊戲內 D3D11 呼叫約八成是 COM AddRef／Release 與 GetDesc（DQ3 每秒十餘萬次），在進入完整函式前
// 以相同語義完成：AddRef 與非最後一次 Release 只改引用計數；GetDesc 只把建立時保存的描述寫回客體，並遵守
// 多執行緒保護裝置的鎖（被其他執行緒持有時交回完整路徑等待）。其餘情況（找不到物件、最後一次 Release 需要銷毀、
// 延後銷毀清單待處理、比對模式）一律交給完整路徑，例外與錯誤回報不變。X64ARM_D3D11_FRONT_PATH=0 關閉。
// IA／*Set* 狀態設定同樣先走前置路徑（d3d11_setter_front_path.inc）；X64ARM_D3D11_SETTER_FRONT_PATH=0 只關閉這一段。
std::optional<std::uint32_t> WindowsServices::invoke_d3d11(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    // 診斷：X64ARM_TRACE_D3D11_FULL=1 統計走完整路徑（非本地）的 D3D11 呼叫：服務、呼叫執行緒、是否串流擁有者、是否串流的立即內容、
    // 串流是否有擁有者、Map 類型；含排空的整體耗時，每 2 秒輸出一次。
    static const bool trace_full=std::getenv("X64ARM_TRACE_D3D11_FULL")!=nullptr;
    struct FullTrace {std::uint64_t start{};std::string key;
        ~FullTrace(){
            if(!start)return;
            static std::mutex lock;static std::map<std::string,std::pair<std::uint64_t,std::uint64_t>> totals;static std::uint64_t next=0;
            const auto now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);const std::lock_guard guard(lock);
            auto& entry=totals[key];++entry.first;entry.second+=now-start;
            if(now>=next){
                if(next){
                    std::vector<std::pair<std::uint64_t,std::string>> rows;for(const auto& [k,v]:totals)rows.emplace_back(v.second,k+" n="+std::to_string(v.first));
                    std::sort(rows.rbegin(),rows.rend());
                    for(std::size_t i=0;i<std::min<std::size_t>(rows.size(),12);++i)std::fprintf(stderr,"[d3d11_full] %.2f ms %s\n",static_cast<double>(rows[i].first)/1e6,rows[i].second.c_str());
                }
                totals.clear();next=now+UINT64_C(2000000000);
            }
        }} full_trace{};
    if(trace_full && d3d11_stream_ && !graphics_local() && service.family=="internal-d3d11"){
        const auto& stream=*d3d11_stream_;
        const auto owner=stream.owner.load(std::memory_order_relaxed),context=stream.context.load(std::memory_order_relaxed);
        std::string key=std::string(service.symbol)+" tid="+std::to_string(resident_context_key(state))+(owner?(state.gs_base==owner?" owner":" other"):" no-owner")+
            (context && state.reg(Gpr::Rcx)==context?" immediate":" ctx?");
        if(service.symbol=="Map")key+=" type="+std::to_string(static_cast<std::uint32_t>(state.reg(Gpr::R9)));
        // 協調者本地完成的呼叫另標記；CreateBuffer 附上用途、綁定、CPU 存取、大小級距與是否有初始資料，用來判斷能否改由 worker 完成。
        if(graphics_coordinator_local())key+=" local";
        if(service.symbol=="Map" && d3d11_){
            const auto resource=d3d11_->objects.find_fast(state.reg(Gpr::Rdx));
            if(resource!=d3d11_->objects.end() && resource->second.identity){
                const auto& id=*resource->second.identity;
                key+=id.kind==Kind::BufferObject?" buffer":" texture";
                key+=id.persistent_maps.contains(static_cast<std::uint32_t>(state.reg(Gpr::R8)))?" kept":" first";
            }
        }
        if(service.symbol=="CreateBuffer")try{
            const auto desc=state.reg(Gpr::Rdx);
            const auto width=static_cast<std::uint32_t>(memory.load(GuestAddress{desc},32));
            char flags[48];std::snprintf(flags,sizeof flags," bind=0x%x cpu=0x%x",static_cast<unsigned>(memory.load(GuestAddress{desc+8},32)),
                static_cast<unsigned>(memory.load(GuestAddress{desc+12},32)));
            key+=" usage="+std::to_string(static_cast<std::uint32_t>(memory.load(GuestAddress{desc+4},32)))+flags+" size<=2^"+
                 std::to_string(width>1?std::bit_width(width-1):0)+(state.reg(Gpr::R8)?" init":"");
        }catch(...){}
        key+=std::string(" refusal=")+(d3d11_local_refusal?d3d11_local_refusal:"none")+" deferred="+std::to_string(d3d11_?d3d11_->deferred_releases.size()/64*64:0);
        full_trace.key=std::move(key);full_trace.start=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    }
    // 全停中交給協調者的擁有者呼叫：能排入命令環的由協調者代為排入（見 d3d11_stream_full_enqueue），不排空佇列。
    if(d3d11_stream_ && !graphics_local() && !graphics_coordinator_local() && d3d11_stream_full_enqueue(state,memory,service))return std::nullopt;
    // 完整服務要看到佇列裡所有命令的結果（worker、協調者本地與串流執行緒自己都是本地路徑，各有處理）；擁有者自己的 Map／Unmap 只等同一資源。
    if(d3d11_stream_ && !graphics_local() && !d3d11_stream_owner_resource_sync(state,memory,service))d3d11_stream_full_service_sync(service);
    static const bool front_path=[] {const auto* value=std::getenv("X64ARM_D3D11_FRONT_PATH");return !value || std::string_view(value)!="0";}();
    static const bool setter_front_path=[] {const auto* value=std::getenv("X64ARM_D3D11_SETTER_FRONT_PATH");return front_path && (!value || std::string_view(value)!="0");}();
    static const bool draw_front_path=[] {const auto* value=std::getenv("X64ARM_D3D11_DRAW_FRONT_PATH");return front_path && (!value || std::string_view(value)!="0");}();
    const auto symbol=service.symbol;
    if(const auto setter=setter_front_path?d3d11_setter_method(service):D3D11SetterMethod{};setter.kind!=D3D11Setter::None && d3d11_ && service.family=="internal-d3d11"){
        std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
        auto& bridge=*d3d11_;
        if(runtime_.network.external_io && (graphics_local() || bridge.deferred_releases.empty())){
            const WindowsCall arg(state,memory,service);
            if(d3d11_setter_front_path(bridge,memory,arg,setter,x86_)){++bridge.setter_front_hits;arg.return_to(state,0);return std::nullopt;}
            ++bridge.setter_front_fallbacks;
        }
    }
    if(const auto method=draw_front_path?d3d11_draw_method(service):D3D11DrawMethod::None;method!=D3D11DrawMethod::None && d3d11_ && service.family=="internal-d3d11"){
        std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
        auto& bridge=*d3d11_;
        if(runtime_.network.external_io && (graphics_local() || bridge.deferred_releases.empty())){
            const WindowsCall arg(state,memory,service);
            std::array<std::uint64_t,6> values{};for(unsigned i=0;i<std::min<unsigned>(service.arguments,values.size());++i)values[i]=arg(i);
            // 繪製不碰客體記憶體，卻可能等命令執行緒：通過資格後放開 worker 持有的映射共用鎖，寫者不必等這段（否則所有 worker 服務都跟著等）。
            // 返回位址先讀好；x86 由 return_to 處理堆疊參數，維持持鎖。
            auto* const guard=x86_?nullptr:detail::d3d11_worker_mapping_guard();std::uint64_t return_target=0;
            const auto release_mapping=[&]{if(guard){return_target=memory.load(GuestAddress{state.reg(Gpr::Rsp)},64);guard->unlock();}};
            const bool done=!d3d11_front_exception && d3d11_draw_front_path(bridge,values,method,symbol,release_mapping);
            if(done){
                ++bridge.draw_front_hits;
                if(guard && !guard->owns_lock()){state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)+=8;}
                else arg.return_to(state,0);
                return std::nullopt;
            }
            ++bridge.draw_front_fallbacks;
            if(guard && !guard->owns_lock())throw detail::D3D11RelockMapping{};
        }
    }
    const bool com=symbol=="AddRef" || symbol=="Release";
    const bool desc=!com && (symbol=="BufferGetDesc" || symbol=="Texture1DGetDesc" || symbol=="Texture3DGetDesc" || symbol=="GraphicsGetDesc");
    if(front_path && (com || desc) && d3d11_ &&
       (service.family=="internal-d3d11" || (com && service.family=="internal-d3d10"))){
        std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
        auto& bridge=*d3d11_;
        if(runtime_.network.external_io && (graphics_local() || bridge.deferred_releases.empty())){
            const WindowsCall arg(state,memory,service);
            if(const auto found=bridge.objects.find_fast(arg(0));found!=bridge.objects.end() && found->second.identity){
                auto& id=*found->second.identity;
                if(symbol=="AddRef"){
                    if(const auto count=adjust_references(memory,id,true)){arg.return_to(state,*count);return std::nullopt;}
                }else if(symbol=="Release"){
                    if(const auto count=adjust_references(memory,id,false,1)){arg.return_to(state,*count);return std::nullopt;}
                }else{
                    const auto device=id.device?bridge.objects.find_fast(id.device):found;
                    if(device!=bridge.objects.end() && device->second.identity &&
                       !(device->second.identity->protected_threads && device->second.identity->lock_address)){
                        memory.write(GuestAddress{arg(1)},id.desc);
                        arg.return_to(state,0);return std::nullopt;
                    }
                }
            }
        }
    }
    return invoke_d3d11_full(state,memory,service);
}
std::optional<std::uint32_t> WindowsServices::invoke_d3d11_full(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    // worker 可直接執行裝置內容方法（見 graphics_worker_service）；所有 D3D11 服務在此串行化。
    // 以登記等待者的方式取鎖：D3D12 提交執行緒看到等待者就在命令之間讓出（DXGI EnumOutputs／GetDesc 原本在全停中等整段重播）。
    std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
    if(!runtime_.network.external_io)throw HostCallFault("原生硬體裝置不能在 compare 模式建立或使用");
    if(!d3d11_)d3d11_=std::make_shared<D3D11State>();
    auto& bridge=*d3d11_;
    const WindowsCall original_arg(state,memory,service);auto symbol=service.symbol;std::string adapted_symbol;
    std::array<std::uint64_t,20> adapted_arguments{};std::uint32_t adapted_mask=0;
    const auto arg=[&](unsigned i){return i<20 && (adapted_mask&(1U<<i))?adapted_arguments[i]:original_arg(i);};
    const auto adapt=[&](unsigned i,std::uint64_t value){adapted_arguments.at(i)=value;adapted_mask|=1U<<i;};
    std::vector<std::uint64_t> temporary_arguments;std::function<void()> finish_adapter;
    bool effect_compile=false;std::uint64_t effect_code_slot=0;
    struct TemporaryCleanup {
        WindowsServices& self;GuestMemory& memory;const GuestState& state;const std::vector<std::uint64_t>& pointers;
        ~TemporaryCleanup(){for(auto pointer:pointers)(void)self.free_heap(memory,pointer,state);}
    } temporary_cleanup{*this,memory,state,temporary_arguments};
    const unsigned pointer_size=x86_?4:8;const std::uint8_t pointer_bits=x86_?32:64;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto output=[&](unsigned i){memory.check(GuestAddress{arg(i)},pointer_size,Access::Write);};
    const auto object=[&](std::uint64_t address)->Object& {
        const auto found=bridge.objects.find_fast(address);if(found==bridge.objects.end())throw HostCallFault("D3D11 COM 物件已釋放或位址無效");return found->second;
    };
    const auto typed=[&](std::uint64_t address,Kind kind)->std::shared_ptr<Identity> {
        auto& value=object(address);if(value.kind!=kind)throw HostCallFault("D3D11 COM 介面類型不符");return value.identity;
    };
    const auto hold=[&](std::uint64_t address) {
        const auto count=adjust_references(memory,*object(address).identity,true);if(!count)throw HostCallFault("D3D11 reference count 溢位");return *count;
    };
    const auto retain=[&](std::uint64_t address) {
        auto& count=object(address).identity->internal_references;
        if(count==UINT32_MAX)throw HostCallFault("D3D11 內部引用計數溢位");
        ++count;
    };
    const auto internal_binding=[](const Identity& owner,std::string_view name) {
        return owner.kind==Kind::Context || (owner.kind==Kind::SwapChain && (name=="Backbuffers" || name=="PresentResolve"));
    };
#include "graphics_fullscreen_window.inc"
    const auto restore_fullscreen=[&](const std::shared_ptr<Identity>& id){
        if(!id->fullscreen)return true;
        if(!restore_graphics_fullscreen(*id))return false;
        put_word(id->desc,x86_?48:56,1);return true;
    };
    static const bool slot_reuse=[] {const auto* value=std::getenv("X64ARM_D3D11_SLOT_REUSE");return !value || std::string_view(value)!="0";}();
    // 持久映射區塊依大小保留（見 D3D11State::map_blocks）；不符合條件時由呼叫者照常 free_heap。
    const auto keep_block=[&](std::uint64_t pointer,std::size_t size)->bool {
        if(!d3d11_map_block_reuse_enabled() || !size || size>(std::size_t{4}<<20) || bridge.map_block_bytes+size>(std::size_t{64}<<20))return false;
        const auto live=allocations_.find(pointer);if(live==allocations_.end() || live->second!=size)return false;
        auto& list=bridge.map_blocks[size];if(list.size()>=512)return false;
        list.push_back(pointer);bridge.map_block_bytes+=size;return true;
    };
    // 遞迴釋放不經 std::function：其捕獲超過小物件緩衝，原本每個 D3D11 呼叫都多一次堆積配置。
    const auto release_object=[&](const auto& self,std::uint64_t address,bool internal=false)->void {
        auto id=object(address).identity;const auto references=reference_count(memory,*id);
        if(!(internal?id->internal_references:references))throw HostCallFault("D3D11 物件引用計數已為零");
        const bool last=references==(internal?0U:1U) && id->internal_references==(internal?1U:0U);
        if(graphics_local() && last){bridge.deferred_releases.emplace_back(address,internal);return;}
        if(last && id->fullscreen && !restore_fullscreen(id))throw WindowPresentationFault("無法恢復 DXGI 全螢幕視窗");
        if(last && id->lock_address && runtime_.critical_sections.at(id->lock_address).owner)throw HostCallFault("不可釋放仍鎖定的 D3D11 裝置");
        if(internal)--id->internal_references;
        else if(!adjust_references(memory,*id,false))throw HostCallFault("D3D11 COM 物件引用計數已為零");
        if(id->internal_references || reference_count(memory,*id))return;
        ++bridge.destroyed_objects;
        if(d3d11_stream_ && !id->persistent_maps.empty()){const std::lock_guard lock(d3d11_stream_->front_dropped_mutex);d3d11_stream_->front_dropped.push_back(address);d3d11_stream_->front_epoch.fetch_add(1,std::memory_order_release);}
        // 位址可能被新物件重用，命令串流不得再認得這個內容（協調者本地路徑的銷毀不經全停）。
        if(id->kind==Kind::Context && d3d11_stream_)d3d11_stream_->context.store(0,std::memory_order_relaxed);
        // 位址也可能變成別的緩衝：可排入更新的緩衝要重新核對。
        if(d3d11_stream_)d3d11_stream_->update_epoch.fetch_add(1,std::memory_order_release);
        // 裝置銷毀時輸出 Metal 後端統計（draws、encoders、GPU 忙碌時間、CPU 等待），供 CPU／GPU 瓶頸判斷。
        if(id->kind==Kind::Device && id->native && !id->api10)
            std::fprintf(stderr,"[d3d11_metal] device=0x%llx frames=%llu %s\n",static_cast<unsigned long long>(id->root),
                static_cast<unsigned long long>(bridge.graphics_frames),id->native->statistics().c_str());
        // 命令執行緒可能仍以參照讀取此 context 的繪製狀態快取。
        if(std::any_of(id->draw_caches.begin(),id->draw_caches.end(),[](const auto& cache){return bool(cache);}) && id->native)id->native->graphics_sync();
        // D3D10 的管線綁定不增加 COM 引用；物件銷毀時自動清除所有綁定位置。
        if(id->api10)for(auto& [pointer,entry]:bridge.objects){
            auto& context=*entry.identity;if(pointer!=context.root || !context.weak_bindings)continue;
            for(auto& [name,values]:context.bindings){(void)name;for(auto& bound:values)
                if(bound && std::any_of(id->interfaces.begin(),id->interfaces.end(),[&](const auto& item){return item.second==bound;}))bound=0;}
        }
        if(!id->weak_bindings)for(auto& [name,values]:id->bindings){for(auto pointer:values)if(pointer)self(self,pointer,internal_binding(*id,name));}
        for(const auto& item:id->sprite.items)if(item.retained)self(self,item.texture);
        for(auto& [guid,pointer]:id->private_interfaces){(void)guid;if(pointer)self(self,pointer);}
        if(id->font_dc){const auto found=gui_.contexts.find(id->font_dc);if(found!=gui_.contexts.end()){const auto bitmap=found->second.bitmap;rasters_.erase(bitmap);gui_.bitmaps.erase(bitmap);gui_.contexts.erase(found);}gui_.fonts.erase(id->font_handle);}
        if(id->sprite_saved)for(const auto& [key,values]:id->sprite_saved->bindings){(void)key;for(auto pointer:values)if(pointer)self(self,pointer);}
        if(id->factory)self(self,id->factory);
        if(id->adapter)self(self,id->adapter);
        if(id->fullscreen_output)self(self,id->fullscreen_output);
        if(id->lock_address)runtime_.critical_sections.erase(id->lock_address);
        for(const auto& [kind,pointer]:id->interfaces){(void)kind;bridge.objects.erase(pointer);}
        for(const auto pointer:id->allocations){
            // 16 bytes 的存活區塊（介面物件、引用計數格）留給下一個介面重用；其他配置與被客體破壞的區塊照常處理。
            if(slot_reuse && bridge.free_slots.size()<4096){
                const auto live=allocations_.find(pointer);
                if(live!=allocations_.end() && live->second==16){bridge.free_slots.push_back(pointer);continue;}
            }
            // 持久映射區塊留給下一次同大小的第一次對應。
            if(const auto kept=std::find_if(id->persistent_maps.begin(),id->persistent_maps.end(),[&](const auto& item){return item.second.pointer==pointer;});
               kept!=id->persistent_maps.end() && keep_block(pointer,kept->second.size))continue;
            if(!free_heap(memory,pointer,state))throw HostCallFault("D3D11 物件配置已被客體破壞");
        }
        if(bridge.objects.empty() && !bridge.free_slots.empty()){auto slots=std::move(bridge.free_slots);bridge.free_slots.clear();for(const auto slot:slots)(void)free_heap(memory,slot,state);}
        if(bridge.objects.empty() && !bridge.map_blocks.empty()){
            auto blocks=std::move(bridge.map_blocks);bridge.map_blocks.clear();bridge.map_block_bytes=0;
            for(const auto& [size,list]:blocks){(void)size;for(const auto block:list)(void)free_heap(memory,block,state);}
        }
        if(id->current_view)self(self,id->current_view,true);
        if(id->texture)self(self,id->texture,id->internal_texture);
        if(id->context10){auto context=object(id->context10).identity;context->device=0;self(self,id->context10);}
        if(id->device)self(self,id->device);
    };
    const auto release=[&](std::uint64_t address){release_object(release_object,address);};
    const auto release_internal=[&](std::uint64_t address){release_object(release_object,address,true);};
    const auto hold_binding=[&](const Identity& owner,std::string_view name,std::uint64_t address){
        if(internal_binding(owner,name))retain(address);else (void)hold(address);
    };
    const auto release_binding=[&](const Identity& owner,std::string_view name,std::uint64_t address){
        release_object(release_object,address,internal_binding(owner,name));
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
        case Kind::Texture:count=12;methods.insert({{7,"ResourceGetType"},{10,"TextureGetDesc"},{11,"Texture2DGetDesc1"}});break;
        case Kind::Fence:count=10;methods.insert({{7,"FenceCreateSharedHandle"},{8,"FenceGetCompletedValue"},{9,"FenceSetEventOnCompletion"}});break;
        case Kind::Annotation:count=7;methods.insert({{3,"AnnotationBeginEvent"},{4,"AnnotationEndEvent"},{5,"AnnotationSetMarker"},{6,"AnnotationGetStatus"}});break;
        case Kind::Decoder:count=9;methods[7]="GetCreationParameters";break;
        case Kind::View:count=9;methods.insert({{7,"ViewGetResource"},{8,"ViewGetDesc"}});break;
        case Kind::Multithread:count=7;methods.insert({{3,"Enter"},{4,"Leave"},{5,"SetMultithreadProtected"},{6,"GetMultithreadProtected"}});break;
        }
        #include "d3d10_vtables.inc"
        // vtable 前一格保存同家族的 Release 服務，供客體 Release 常式在最後一次（或計數異常）時尾跳。
        // x64 的 AddRef／Release 槽指向客體常式，不離開原生碼；X64ARM_D3D11_GUEST_REFCOUNT=0 維持服務。
        static const bool guest_refcount=[] {const auto* value=std::getenv("X64ARM_D3D11_GUEST_REFCOUNT");return !value || std::string_view(value)!="0";}();
        const auto family=d10_table?"internal-d3d10":"internal-d3d11";
        const auto base=runtime_allocate(memory,{},(count+1)*pointer_size),pointer=base+pointer_size;
        memory.store(GuestAddress{base},pointer_bits,resolve(family,"Release").value);
        const bool com=methods.contains(1) && methods.contains(2) && methods.at(1)=="AddRef" && methods.at(2)=="Release";
        const auto add_ref=guest_refcount && com?guest_thread_routine("ComBridgeAddRef"):GuestAddress{0};
        const auto release_ref=guest_refcount && com?guest_thread_routine("ComBridgeRelease"):GuestAddress{0};
        for(std::size_t i=0;i<count;++i){
            auto target=resolve(family,methods.contains(i)?methods.at(i):"Unsupported").value;
            if(i==1 && add_ref.value && release_ref.value)target=add_ref.value;
            if(i==2 && add_ref.value && release_ref.value)target=release_ref.value;
            memory.store(GuestAddress{pointer+i*pointer_size},pointer_bits,target);
        }
        bridge.vtables.emplace(key,pointer);return pointer;
    };
    // 16 bytes 的客體區塊：先取回收槽（清零後與新配置相同），沒有才配置；槽所在頁被客體改過保護時放棄該槽。
    // worker 與串流執行緒（graphics_local）不配置或釋放 heap：只取預備的槽（d3d11_local_eligible 已核對數量與頁面），不足即失敗。
    const auto take_slot=[&]()->std::uint64_t {
        while(!bridge.free_slots.empty()){
            const auto address=bridge.free_slots.back();bridge.free_slots.pop_back();
            static constexpr std::array<std::uint8_t,16> zero{};
            try{memory.write(GuestAddress{address},std::span<const std::uint8_t>(zero));return address;}
            catch(const MemoryFault&){if(graphics_local())throw;(void)free_heap(memory,address,state);}
        }
        if(graphics_local())throw std::bad_alloc{};
        return allocate_heap(memory,16);
    };
    const auto return_slot=[&](std::uint64_t address){if(graphics_local())bridge.free_slots.push_back(address);else (void)free_heap(memory,address,state);};
    const auto interface=[&](const std::shared_ptr<Identity>& id,Kind kind) {
        if(id->interfaces.contains(kind))return id->interfaces.at(kind);
        // COM 介面與紋理 array slice 的數量限制無關；配置由客體 heap、
        // 宿主配置及實際 GPU reservation 決定，不能用全域物件數誤報 OOM。
        id->allocations.reserve(id->allocations.size()+1);
        if(!id->counter){
            // 身分共用的引用計數格（見 reference_count），初值 1；列入 allocations 隨身分釋放。
            id->allocations.reserve(id->allocations.size()+2);
            const auto counter=take_slot();if(!counter)throw std::bad_alloc{};
            memory.store(GuestAddress{counter},32,1);id->counter=counter;id->allocations.push_back(counter);
        }
        // 介面物件：[vtable][引用計數格位址]。
        const auto table=vtable(kind,id->api10),pointer=take_slot();if(!pointer)throw std::bad_alloc{};
        try {
            id->interfaces.emplace(kind,pointer);bridge.objects.emplace(pointer,Object{kind,id});
            memory.store(GuestAddress{pointer},pointer_bits,table);memory.store(GuestAddress{pointer+pointer_size},pointer_bits,id->counter);
        } catch(...) {id->interfaces.erase(kind);bridge.objects.erase(pointer);return_slot(pointer);throw;}
        id->allocations.push_back(pointer);
        bridge.peak_interfaces=std::max(bridge.peak_interfaces,bridge.objects.size());
        return pointer;
    };
    const auto create=[&](Kind kind,std::uint64_t device=0,bool api10=false) {
        auto id=std::make_shared<Identity>();id->kind=kind;id->api10=api10 || (device && kind!=Kind::Context && object(device).identity->api10);id->root=interface(id,kind);
        if(!id->api10 && (kind==Kind::Layout || kind==Kind::Blend)){
            static std::atomic<std::uint64_t> serials{};
            id->pipeline_state_serial=serials.fetch_add(1,std::memory_order_relaxed)+1;
        }
        try {if(device)hold(device);}catch(...) {release(id->root);throw;}
        id->device=device;
        try{if(device)id->native=object(device).identity->native;
            else if(kind==Kind::Device || kind==Kind::Factory){
                id->native=make_metal_opengl(nullptr,1,1);
                // D3D11 裝置的 Metal 編碼移到專用宿主執行緒（見 metal_command_thread.inc）；X64ARM_METAL_COMMAND_THREAD=0 關閉。
                static const bool command_thread=[] {const auto* value=std::getenv("X64ARM_METAL_COMMAND_THREAD");return !value || std::string_view(value)!="0";}();
                if(command_thread && kind==Kind::Device && !id->api10)id->native=make_metal_command_thread(std::move(id->native));
            }
        }catch(...){release(id->root);throw;}
        return id;
    };
    std::uint64_t result=0;
    // 成功進入獨占全螢幕的視窗：Windows 的獨占全螢幕視窗必為前景，方法完成後尚未啟用時補上啟用通知。
    std::uint64_t fullscreen_activation=0;
#include "d3d11_graphics_void.inc"
    try {
        if(d3d11_front_exception){const auto error=std::exchange(d3d11_front_exception,nullptr);std::rethrow_exception(error);}
        if(!graphics_local() && !bridge.deferred_releases.empty()){
            auto pending=std::move(bridge.deferred_releases);bridge.deferred_releases.clear();
            for(const auto& [address,internal]:pending)release_object(release_object,address,internal);
        }
        // worker 上的建立方法只取預備槽（take_slot）：協調者本地與完整路徑可配置 heap，低於 256 個時補到 1024 個。
        if(!graphics_local() && slot_reuse && d3d11_worker_create_enabled() && bridge.free_slots.size()<256 && !bridge.objects.empty()){
            bridge.free_slots.reserve(1024);
            while(bridge.free_slots.size()<1024){const auto slot=allocate_heap(memory,16);if(!slot)break;bridge.free_slots.push_back(slot);}
        }
        // COM 的 AddRef／Release 佔呼叫大宗，原本要先比對數百個方法名稱才命中；兩者語義與介面種類無關，先行處理。
        // 仍在同一 try 內，例外與收尾（return_to）與一般路徑相同。
        if((symbol=="AddRef" || symbol=="Release") && (service.family=="internal-d3d11" || service.family=="internal-d3d10")){
            if(symbol=="AddRef")result=hold(arg(0));
            else {result=reference_count(memory,*object(arg(0)).identity)-1;release(arg(0));}
        }else {
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
        // stage 順序與 GraphicsDraw::stages 相同；轉接器與共用處理器一起使用。
        const auto stage_index=[](std::string_view name)->unsigned {if(name.starts_with("VS"))return 0;if(name.starts_with("PS"))return 1;if(name.starts_with("GS"))return 2;if(name.starts_with("HS"))return 3;if(name.starts_with("DS"))return 4;return 5;};
        static const std::array<std::string,6> stage_names{"VS","PS","GS","HS","DS","CS"};
        // D3D11_FORMAT_SUPPORT／FORMAT_SUPPORT2 依共用後端（Metal）的實際能力；CheckFormatSupport 與 CheckFeatureSupport 共用。
        // 診斷對照：X64ARM_D3D11_FORMAT_CAPS=legacy 時回到 2026-10-24 前的格式能力回報（不含 BLENDABLE、typed UAV、MIP_AUTOGEN、
        // CPU_LOCKABLE、DISPLAY、GATHER 位元，FORMAT_SUPPORT2 為 0、D3D11_OPTIONS 全 FALSE），用於判斷能力回報改變對遊戲路徑的影響。
        static const bool legacy_format_caps=[]{const auto* value=std::getenv("X64ARM_D3D11_FORMAT_CAPS");return value && std::string_view(value)=="legacy";}();
        const auto format_support=[&](const std::shared_ptr<Identity>& device,std::uint32_t f)->std::optional<std::uint32_t> {
            std::uint32_t support=0;
            if(device->native->graphics_samples_supported(f,1)){
                support=0x1U|0x10U|0x20U|0x40U|0x80U|0x100U|0x200U|0x1000U|0x4000U;
                if(f==67)support&=~0x4000U;
                if(f>=5 && f<=8){support&=~0x4000U;if(f!=6)support&=~0x200U;if(f==5)support&=~0x101U;}
                const bool combined=(f>=19 && f<=22) || (f>=44 && f<=47),depth=combined || f==40 || f==55;
                if(depth){
                    support=0x20U|0x1000U|0x20000U;
                    if(f==20 || f==40 || f==45 || f==55)support|=0x10000U;
                    else if(f==21 || f==46)support|=0x100U|0x200U|0x400U;
                    else if(f==22 || f==47)support|=0x100U;
                    if(f==19 || f==44)support|=0x100000U;
                }
                const bool bc=graphics_format_bc_family(f)!=0;
                if(const auto family=graphics_format_bc_family(f)){support=0x20U|0x40U|0x80U|0x1000U;if(f!=family)support|=0x100U|0x200U;}
                if(device->native->graphics_samples_supported(f,2)){
                    if(!depth || f==20 || f==40 || f==45 || f==55)support|=0x200000U;
                    if(!depth)support|=0x40000U;
                    if(f==21 || f==22 || f==46 || f==47)support|=0x400000U;
                }
                if(!depth && !bc && !legacy_format_caps){
                    const bool integer=f==3 || f==4 || f==12 || f==14 || f==17 || f==18 || f==25 || f==30 || f==32 || f==36 || f==38 ||
                        f==42 || f==43 || f==50 || f==52 || f==57 || f==59 || f==62 || f==64;
                    const bool srgb=f==29 || f==72 || f==74 || f==76 || f==91 || f==93 || f==99;
                    support|=0x20000U; // CPU_LOCKABLE：staging 資源可映射。
                    if(support&0x200U)support|=0x800000U; // SHADER_GATHER
                    if((support&0x4000U) && !integer){
                        support|=0x8000U; // BLENDABLE
                        if(f!=2 && f!=16 && f!=41)support|=0x2000U; // MIP_AUTOGEN（32 位元浮點不保證可濾波）
                    }
                    // TYPED_UNORDERED_ACCESS_VIEW：Metal 可由著色器寫入的色彩格式；sRGB、共用指數、打包 16 位元、A8 與 RGB32 不可。
                    if(!srgb && f!=65 && f!=67 && f!=68 && f!=69 && f!=85 && f!=86 && f!=115 && !(f>=5 && f<=8))support|=0x2000000U;
                    if(f==10 || f==24 || f==28 || f==29 || f==87 || f==91)support|=0x80000U; // DISPLAY（scanout 格式）
                }
                if((support&0x400U) && !legacy_format_caps)support|=0x4000000U; // SHADER_GATHER_COMPARISON
            }else if(device->native->graphics_vertex_format_supported(f))support=1;else return std::nullopt;
            if(device->native->graphics_vertex_format_supported(f))support|=2;
            if(f==42 || f==57)support|=4; // IA_INDEX_BUFFER：R32_UINT／R16_UINT
            return support;
        };
        const auto format_support2=[&](const std::shared_ptr<Identity>& device,std::uint32_t f)->std::uint32_t {
            if(legacy_format_caps)return 0;
            const auto support=format_support(device,f);if(!support)return 0;std::uint32_t value=0;
            if(*support&0x2000000U)value|=0x80U; // UAV_TYPED_STORE
            if(f==41 || f==42 || f==43)value|=0x40U; // UAV_TYPED_LOAD：FL11 保證的 R32 格式
            if(f==42 || f==43)value|=0x3fU; // R32_UINT／SINT 的原子加、位元、比較交換、交換與極值
            return value;
        };
        #include "d3d10_adapter.inc"
        #include "d3d11_1_adapter.inc"
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
                // ID3D11Device1～5 的 vtable 是同一份延伸表。
                if(guid==IID_ID3D11Device || guid==IID_ID3D11Device1 || guid==IID_ID3D11Device2 || guid==IID_ID3D11Device3 ||
                   guid==IID_ID3D11Device4 || guid==IID_ID3D11Device5){target=Kind::Device;supported=true;}
                if(guid==IID_IDXGIDevice || guid==IID_IDXGIDevice1 || guid==IID_IDXGIDevice2 || guid==IID_IDXGIObject){target=Kind::DxgiDevice;supported=true;}
                if(guid==IID_ID3D11VideoDevice){target=Kind::VideoDevice;supported=true;}
                if(guid==IID_ID3D10Multithread){target=Kind::Multithread;supported=true;}
            } else if(id->kind==Kind::Context){
                if(guid==IID_ID3D11DeviceContext || guid==IID_ID3D11DeviceChild || guid==IID_ID3D11DeviceContext1 || guid==IID_ID3D11DeviceContext2 ||
                   guid==IID_ID3D11DeviceContext3 || guid==IID_ID3D11DeviceContext4){target=Kind::Context;supported=true;}
                if(guid==IID_ID3DUserDefinedAnnotation){target=Kind::Annotation;supported=true;}
                if(guid==IID_ID3D11VideoContext){target=Kind::VideoContext;supported=true;}
            } else if(id->kind==Kind::Texture)supported=guid==IID_ID3D11Texture2D || guid==IID_ID3D11Texture2D1 || guid==IID_ID3D11Resource || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::Fence)supported=guid==IID_ID3D11Fence || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::Decoder)supported=guid==IID_ID3D11VideoDecoder || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::View)supported=guid==IID_ID3D11VideoDecoderOutputView || guid==IID_ID3D11View || guid==IID_ID3D11DeviceChild;
            else if(id->kind==Kind::Reflection)supported=guid==IID_ID3D11ShaderReflection || guid==IID_ID3D11ShaderReflection47 || guid==IID_ID3D11ShaderReflection43 || guid==IID_ID3D12ShaderReflection;
            else if(id->kind==Kind::Blob)supported=guid==IID_ID3D10Blob;
            else if(id->kind==Kind::Factory)supported=dxgi_factory_guid(guid) || guid==IID_IDXGIObject;
            else if(id->kind==Kind::Adapter)supported=dxgi_adapter_guid(guid) || guid==IID_IDXGIObject;
#include "d3d11_graphics_query.inc"
            if(resource_kind(id->kind) && (guid==IID_IDXGIResource || guid==IID_IDXGIDeviceSubObject || guid==IID_IDXGIObject)){target=Kind::DxgiResource;supported=true;}
            if(!supported)result=no_interface;
            else {const auto pointer=interface(id,target);hold(pointer);memory.store(GuestAddress{arg(2)},pointer_bits,pointer);}
        } else if(symbol=="AddRef")result=hold(arg(0));
        else if(symbol=="Release"){result=reference_count(memory,*object(arg(0)).identity)-1;release(arg(0));}
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
            else if((by_luid && arg(1)!=display_identity::adapter_luid && arg(1)!=display_identity::warp_adapter_luid) || (!by_luid && u32(1)))result=not_found;
            else if(guid!=IID_IUnknown && !dxgi_adapter_guid(guid) && guid!=IID_IDXGIObject)result=no_interface;
            else {auto adapter=create(Kind::Adapter,arg(0));adapter->warp_adapter=by_luid && arg(1)==display_identity::warp_adapter_luid;memory.store(GuestAddress{arg(out)},pointer_bits,adapter->root);}
        } else if(symbol=="EnumWarpAdapter") {
            // WARP 身分的配接器（見 display_identity::warp_adapter_description）：如實標示為軟體配接器，裝置仍由 Metal 執行。
            (void)typed(arg(0),Kind::Factory);output(2);memory.store(GuestAddress{arg(2)},pointer_bits,0);const auto guid=read_guid(memory,arg(1));
            if(guid!=IID_IUnknown && !dxgi_adapter_guid(guid) && guid!=IID_IDXGIObject)result=no_interface;
            else {auto adapter=create(Kind::Adapter,arg(0));adapter->warp_adapter=true;memory.store(GuestAddress{arg(2)},pointer_bits,adapter->root);}
        } else if(symbol=="FactoryGetCreationFlags"){(void)typed(arg(0),Kind::Factory);result=0;}
        else if(symbol=="FactoryCheckFeatureSupport") {
            // DXGI_FEATURE_PRESENT_ALLOW_TEARING：可變更新率撕裂呈現不支援。
            (void)typed(arg(0),Kind::Factory);if(u32(1)!=0 || u32(3)!=4 || !arg(2))result=invalid;else memory.store(GuestAddress{arg(2)},32,0);
        } else if(symbol=="RegisterAdaptersChangedEvent") {
            (void)typed(arg(0),Kind::Factory);if(!arg(1) || !arg(2))result=invalid;else memory.store(GuestAddress{arg(2)},32,1);
        } else if(symbol=="UnregisterAdaptersChangedNotification"){(void)typed(arg(0),Kind::Factory);}
        else if(symbol=="IsWindowedStereoEnabled"){(void)typed(arg(0),Kind::Factory);result=0;} // macOS 沒有視窗化立體顯示。
        else if(symbol=="RegisterStereoStatusWindow" || symbol=="RegisterOcclusionStatusWindow" ||
                symbol=="RegisterStereoStatusEvent" || symbol=="RegisterOcclusionStatusEvent") {
            // 立體模式不存在、Present 不回報 DXGI_STATUS_OCCLUDED，狀態不會改變，因此登記後不會送出通知；
            // 仍驗證視窗／事件並配發可註銷的 cookie，與 Windows 在狀態未變時的行為一致。
            (void)typed(arg(0),Kind::Factory);const bool window=symbol.ends_with("Window");const unsigned out=window?3U:2U;
            static std::uint32_t next_status_cookie=1;
            if(!arg(out))result=invalid;
            else if(window && !gui_.windows.contains(arg(1)))result=invalid;
            else if(!window){const auto event=runtime_.wait_objects.find(arg(1));if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)result=invalid;}
            if(arg(out)){memory.check(GuestAddress{arg(out)},4,Access::Write);memory.store(GuestAddress{arg(out)},32,result?0U:next_status_cookie++);}
        } else if(symbol=="UnregisterStereoStatus" || symbol=="UnregisterOcclusionStatus"){(void)typed(arg(0),Kind::Factory);}
        else if(symbol=="GetSharedResourceAdapterLuid") {
            (void)typed(arg(0),Kind::Factory);
            const auto found=arg(1)<=UINT32_MAX?bridge.shared_resources.find(u32(1)):bridge.shared_resources.end();
            if(!arg(2) || found==bridge.shared_resources.end() || found->second.resource.expired())result=invalid;
            else {memory.check(GuestAddress{arg(2)},8,Access::Write);memory.store(GuestAddress{arg(2)},64,display_identity::adapter_luid);}
        } else if(symbol=="CreateSwapChainForCoreWindow") {
            // 桌面程序沒有 CoreWindow；與非 UWP 環境相同回 DXGI_ERROR_INVALID_CALL。
            (void)typed(arg(0),Kind::Factory);output(5);memory.store(GuestAddress{arg(5)},pointer_bits,0);result=0x887a0001U;
        }
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
            const auto adapter=typed(arg(0),Kind::Adapter);
            if(!arg(1))result=invalid;
            else {
                const auto desc=adapter->warp_adapter?display_identity::warp_adapter_description(x86_,gpu_memory_stats().limit):display_identity::adapter_description(x86_,gpu_memory_stats().limit);
                memory.write(GuestAddress{arg(1)},std::span(desc).first(display_identity::adapter_description_size(symbol,x86_)));
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
        } else if(symbol=="TextureGetDesc" || symbol=="Texture2DGetDesc1") {
            // D3D11_TEXTURE2D_DESC1 只多 TextureLayout；建立時只接受 UNDEFINED。
            auto id=typed(arg(0),Kind::Texture);std::vector<std::uint8_t> bytes(symbol=="TextureGetDesc"?44:48);for(unsigned i=0;i<11;++i)put_word(bytes,i*4,id->texture_desc[i]);memory.write(GuestAddress{arg(1)},bytes);
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
            else {auto view=create(Kind::View,device->root);view->texture=arg(1);view->internal_texture=true;retain(arg(1));view->slice=word(desc,20);view->view_desc=std::move(desc);memory.store(GuestAddress{arg(3)},pointer_bits,view->root);}
        } else if(symbol=="ViewGetDesc"){auto id=typed(arg(0),Kind::View);memory.write(GuestAddress{arg(1)},id->view_desc);}
        else if(symbol=="ViewGetResource"){auto id=typed(arg(0),Kind::View);output(1);hold(id->texture);memory.store(GuestAddress{arg(1)},pointer_bits,id->texture);}
        else if(symbol=="GetCreationParameters") {auto id=typed(arg(0),Kind::Decoder);memory.check(GuestAddress{arg(1)},28,Access::Write);memory.check(GuestAddress{arg(2)},100,Access::Write);memory.write(GuestAddress{arg(1)},id->desc);memory.write(GuestAddress{arg(2)},id->config);}
        else if(symbol=="DecoderBeginFrame") {
            auto context=typed(arg(0),Kind::VideoContext),decoder=typed(arg(1),Kind::Decoder),view=typed(arg(2),Kind::View);
            if(context->device!=decoder->device || view->device!=decoder->device || decoder->frame_active || u32(3) || arg(4))result=invalid;
            else {decoder->frame_active=true;decoder->submitted=false;decoder->current_view=arg(2);retain(arg(2));for(auto& [type,buffer]:decoder->buffers){(void)type;buffer.submitted.clear();}}
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
                } catch(...) {decoder->codec.reset();decoder->parameter_sets.clear();decoder->reference_poc.clear();release_internal(current_view);throw;}
                release_internal(current_view);
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
        }
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
      catch(const std::invalid_argument& error){bridge.last_error=error.what();if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3d11] %.*s: %s\n",static_cast<int>(symbol.size()),symbol.data(),error.what());if(void_methods.contains(symbol)){
            // 診斷：X64ARM_TRACE_SHADER_FAILURE_DIR=<目錄> 時把繪製／分派的著色器編譯失敗寫成檔案並略過這次呼叫，一輪收集所有失敗的著色器；正常執行仍視為內部錯誤。
            static const char* const failure_dir=std::getenv("X64ARM_TRACE_SHADER_FAILURE_DIR");
            if(!failure_dir || bridge.last_error.find("Metal 著色器編譯失敗")==std::string::npos)throw HostCallFault(bridge.last_error);
            const auto digest=std::hash<std::string>{}(bridge.last_error);
            char name[32];std::snprintf(name,sizeof name,"/%016zx.txt",digest);
            if(auto* out=std::fopen((std::string(failure_dir)+name).c_str(),"wb")){std::fwrite(bridge.last_error.data(),1,bridge.last_error.size(),out);std::fclose(out);}
        }
        result=invalid;}
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
    // 診斷：X64ARM_TRACE_GRAPHICS_ERRORS 時列出所有回傳失敗 HRESULT 的 D3D11／DXGI 方法（D3DX11 已於上方列出）。
    static const bool trace_graphics_errors=std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS")!=nullptr;
    if(trace_graphics_errors && (result&0x80000000U) && !symbol.starts_with("D3DX11") && symbol!="CreateTexture2D")
        std::fprintf(stderr,"[d3d11] %.*s HRESULT=%08x %s\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned>(result),bridge.last_error.c_str());
    static const bool trace_dxgi_state=std::getenv("X64ARM_TRACE_DXGI_STATE")!=nullptr; // 每次 D3D11 呼叫都經過，不重複查環境變數。
    if(trace_dxgi_state && service.family=="internal-d3d11"){
        const bool present=symbol=="Present" || symbol=="Present1";
        const bool change=symbol=="SetFullscreenState" || symbol=="ResizeTarget" || symbol=="ResizeBuffers" || symbol=="EnumOutputs" || symbol=="GetContainingOutput";
        if(present || change){const auto id=object(arg(0)).identity;const auto now=std::chrono::steady_clock::now();
            // 長幀統計：相鄰 Present 間隔超過 50／100 ms 的累計次數與本段（約 5 秒）最大間隔，供操作中量測判讀卡頓
            //（Present 只在協調者執行，以函式內靜態值累計第一個交換鏈以外也無妨，只作診斷）。
            static std::chrono::steady_clock::time_point last_present{};static double window_gap_ms{};static unsigned long long over50{},over100{};
            if(present){
                if(last_present.time_since_epoch().count()){
                    const auto gap=std::chrono::duration<double,std::milli>(now-last_present).count();
                    window_gap_ms=std::max(window_gap_ms,gap);if(gap>50)++over50;if(gap>100)++over100;
                }
                last_present=now;
            }
            if((result&0x80000000U) || change || !id->present_trace.time_since_epoch().count() || now-id->present_trace>=std::chrono::seconds(5)){
                if(present)id->present_trace=now;
                if(present)std::fprintf(stderr,"[dxgi] gaps max_ms=%.1f over50=%llu over100=%llu\n",window_gap_ms,over50,over100);
                if(present)window_gap_ms=0;
                std::fprintf(stderr,"[dxgi] %.*s this=%llx result=%llx a1=%llx a2=%llx frames=%llu fullscreen=%u\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(arg(0)),static_cast<unsigned long long>(result),static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(arg(2)),static_cast<unsigned long long>(id->frames),id->fullscreen);
                if((result&0x80000000U) && !bridge.last_error.empty())std::fprintf(stderr,"[dxgi] error=%s\n",bridge.last_error.c_str());
            }
        }
    }
    if(finish_adapter && !(result&0x80000000U))finish_adapter();
    if(fullscreen_activation && !(result&0x80000000U) && gui_.active!=fullscreen_activation){
        // 與 SetForegroundWindow 相同的啟用流程（WM_NCACTIVATE／WM_ACTIVATE 回呼）；只限位於輸入桌面的頂層視窗。
        // 呼叫者通常是渲染執行緒而非視窗執行緒，不套用 SetActiveWindow 的同輸入佇列限制。
        const auto window=gui_.windows.find(fullscreen_activation);
        if(window!=gui_.windows.end() && !window->second.parent && window->second.desktop_object==gui_.input_desktop){
            auto callbacks=activate_gui_window(fullscreen_activation);
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;}
        }
    }
    original_arg.return_to(state,result);return std::nullopt;
}
}
