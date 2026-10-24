#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <atomic>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm {
// D3D9 固定管線及 D3D11 可程式化管線共用同一 Metal context、提交與呈現。
struct GraphicsObject {virtual ~GraphicsObject()=default;};
struct GraphicsResourceDesc {
    std::uint32_t dimension{1},width{},height{1},depth{1},levels{1},layers{1},format{},samples{1},bind{},stride{},misc{};
    std::uint32_t sample_quality{};
};
struct GraphicsResource:GraphicsObject {
    GraphicsResourceDesc desc;
    std::uint64_t stream_cursor{},stream_vertices{};
};
struct GraphicsStreamElement {std::string semantic;std::uint32_t index{},first{},count{},slot{},stream{};};
struct GraphicsStreamStatistics {std::uint64_t written{},needed{};};
struct GraphicsBinding {std::uint32_t type{},slot{},native_slot{},counter_slot{UINT32_MAX};};
struct GraphicsInput {std::string semantic;std::uint32_t index{},attribute{};};
struct GraphicsLegacyShader {std::string source,entry;std::vector<GraphicsBinding> bindings;std::vector<GraphicsInput> inputs;};
GraphicsLegacyShader graphics_legacy_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes);
// SM5.1 DXBC 與 DXIL：經 vkd3d-shader（SPIR-V）與 SPIRV-Cross 轉為 MSL；入口為 xlatMtlMain。
struct GraphicsModernShader {std::string source,entry;std::vector<GraphicsBinding> bindings;std::vector<GraphicsInput> inputs;std::array<std::uint32_t,3> threads{1,1,1};};
bool graphics_modern_shader_required(std::span<const std::uint8_t> bytes);
GraphicsModernShader graphics_modern_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes);
struct GraphicsShader:GraphicsObject {
    std::vector<GraphicsStreamElement> stream_output;
    std::array<std::uint32_t,4> stream_strides{};
    std::uint32_t rasterized_stream{};
    std::uint32_t stage{};
    std::vector<GraphicsBinding> bindings;
    std::vector<GraphicsInput> inputs;
    std::array<std::uint32_t,3> threads{1,1,1};
};
struct GraphicsSampler:GraphicsObject {};
struct GraphicsTimestamp {std::uint64_t ticks{},frequency{1000000000};bool disjoint{};};
// 非同步時間戳：done 為 true 後 value 有效；由後端完成回呼寫入，讀者以 acquire 讀取。
struct GraphicsTimestampFuture {std::atomic<bool> done{};GraphicsTimestamp value{};};
struct GraphicsView {
    std::shared_ptr<GraphicsResource> resource;
    std::uint32_t format{},level{},levels{1},slice{},slices{1},first{},count{},flags{};
    // Append／Consume 計數器屬於 UAV view；同一資源的不同 view 不共用計數。
    std::shared_ptr<GraphicsResource> counter{};
    std::uint32_t dimension{}; // SRV 維度，保留 cube／cube array 的取樣形狀。
};
struct GraphicsStage {
    std::shared_ptr<GraphicsShader> shader;
    // b14 保留給引擎的動態 linkage 資料；D3D11 公開綁定仍限 b0–b13。
    std::array<std::shared_ptr<GraphicsResource>,15> constants;
    // D3D11.1 常數緩衝範圍的起點（位元組，256 對齊）；0 表示從緩衝起點綁定。
    std::array<std::uint32_t,15> constant_offsets{};
    std::array<GraphicsView,128> resources;
    std::array<GraphicsView,8> unordered;
    std::array<std::shared_ptr<GraphicsSampler>,16> samplers;
};
struct GraphicsElement {std::string semantic;std::uint32_t index{},format{},slot{},offset{},classification{},step{};};
struct GraphicsVertexBuffer {std::shared_ptr<GraphicsResource> resource;std::uint32_t stride{},offset{};};
struct GraphicsDraw {
    std::array<std::shared_ptr<GraphicsResource>,4> stream_targets;
    std::shared_ptr<GraphicsStreamStatistics> stream_statistics;
    std::array<GraphicsStage,6> stages; // VS、PS、GS、HS、DS、CS。
    std::vector<GraphicsElement> layout;
    std::array<GraphicsVertexBuffer,32> vertices;
    std::shared_ptr<GraphicsResource> indices;
    std::uint32_t index_format{42},index_offset{},topology{4};
    std::array<GraphicsView,8> targets;
    GraphicsView depth;
    std::vector<std::array<float,6>> viewports;
    std::vector<std::array<std::int32_t,4>> scissors;
    std::vector<std::uint8_t> blend,depth_state,raster;
    std::array<float,4> blend_factor{1,1,1,1};
    std::uint32_t sample_mask{UINT32_MAX},stencil_reference{},count{},start{},instances{1},start_instance{};
    std::int32_t base_vertex{};
    // D3D9 可程式化像素階段之後的 alpha test；0 表示停用。
    std::uint32_t alpha_function{},alpha_reference{};
    bool indexed{};
    std::shared_ptr<GraphicsResource> visibility;std::uint32_t visibility_offset{};
    // Draw*InstancedIndirect：count／instances／start／base_vertex／start_instance 改由此 buffer 的 offset 處取得
    // （D3D11 與 Metal 的間接參數佈局相同）；GPU 寫入的參數不經 CPU 讀回。
    std::shared_ptr<GraphicsResource> indirect;std::uint32_t indirect_offset{};
};
struct FixedGraphicsVertex {std::array<float,4> position,color,texcoord;std::array<float,4> secondary{};};
struct FixedGraphicsDraw {
    std::span<const FixedGraphicsVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::array<std::array<float,16>,3> transforms; // world、view、projection；D3D row-major。
    std::array<float,2> viewport_size;
    std::uint32_t topology{}; // D3D9 的 1–6。
};
class NativeGraphics {
public:
    virtual ~NativeGraphics()=default;
    virtual bool fixed_vertex_supported()const noexcept{return false;}
    virtual void graphics_fixed_targets(const GraphicsView&,const GraphicsView&){throw std::runtime_error("後端缺少固定管線繪製表面切換");}
    // enable、reference、read/write mask，接續正／背面的 compare、fail、zfail、pass（D3D enum）。
    virtual bool graphics_fixed_texture(const GraphicsView&){return false;}
    virtual void graphics_fixed_stencil(const std::array<std::uint32_t,12>&){throw std::runtime_error("後端缺少固定管線雙面 stencil");}
    virtual void graphics_fixed_draw(const FixedGraphicsDraw&){throw std::runtime_error("後端缺少固定管線頂點處理");}
    virtual std::shared_ptr<GraphicsResource> graphics_resource(const GraphicsResourceDesc&){throw std::runtime_error("後端缺少可程式化資源");}
    virtual bool graphics_samples_supported(std::uint32_t,std::uint32_t){return false;}
    virtual bool graphics_vertex_format_supported(std::uint32_t){return false;}
    virtual std::shared_ptr<GraphicsShader> graphics_shader(std::uint32_t,std::span<const std::uint8_t>){throw std::runtime_error("後端缺少 DXBC 著色器");}
    // 由相容層自行產生的 MSL compute 核心（例如 DirectML 運算子）；bindings 的 slot 對應 GraphicsStage 陣列。
    virtual std::shared_ptr<GraphicsShader> graphics_compute_source(const GraphicsModernShader&){throw std::runtime_error("後端缺少 MSL compute 核心");}
    virtual std::shared_ptr<GraphicsSampler> graphics_sampler(std::span<const std::uint8_t>){throw std::runtime_error("後端缺少取樣器");}
    virtual bool graphics_timestamp_supported()const{return false;}
    virtual GraphicsTimestamp graphics_timestamp(){throw std::runtime_error("後端沒有 GPU timestamp");}
    // 在目前提交點記錄 GPU 時間戳，不等待；供 D3D11 TIMESTAMP／TIMESTAMP_DISJOINT 查詢輪詢。預設後端同步取樣後直接完成。
    virtual std::shared_ptr<const GraphicsTimestampFuture> graphics_timestamp_future(){auto future=std::make_shared<GraphicsTimestampFuture>();future->value=graphics_timestamp();future->done.store(true,std::memory_order_release);return future;}
    virtual void graphics_write(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t){throw std::runtime_error("後端缺少資源寫入");}
    virtual void graphics_write_discard(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){graphics_write(view,data,row,image);}
    // WRITE_NO_OVERWRITE 的整個資源上傳：呼叫端保證 GPU 使用中的範圍與上次上傳的位元組相同，後端可直接寫入而不等待。
    virtual void graphics_write_no_overwrite(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){graphics_write(view,data,row,image);}
    virtual void graphics_write_region(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t,std::array<std::uint32_t,6>){throw std::runtime_error("後端缺少區域資源寫入");}
    virtual std::vector<std::uint8_t> graphics_read(const GraphicsView&){throw std::runtime_error("後端缺少資源讀回");}
    // 呼叫端已確認寫入此範圍的 GPU 工作都已完成（例如遮蔽查詢 fence）時讀回：不等待同一資源上較新的批次，
    // 非同步後端也不先排空命令佇列。只用於不會被 discard 改名的緩衝；預設沿用 graphics_read。
    virtual std::vector<std::uint8_t> graphics_read_completed(const GraphicsView& view){return graphics_read(view);}
    virtual void graphics_copy(const GraphicsView&,const GraphicsView&,std::array<std::uint32_t,3>,std::array<std::uint32_t,6>){throw std::runtime_error("後端缺少資源複製");}
    virtual void graphics_resolve(const GraphicsView&,const GraphicsView&,std::uint32_t){throw std::runtime_error("後端缺少多重取樣 resolve");}
    virtual void graphics_clear(const GraphicsView&,std::array<float,4>,std::uint32_t){throw std::runtime_error("後端缺少清除操作");}
    virtual bool graphics_clear_unordered(const GraphicsView&,std::array<std::uint32_t,4>,bool){throw std::runtime_error("後端缺少 UAV 清除操作");}
    virtual void graphics_draw(const GraphicsDraw&){throw std::runtime_error("後端缺少繪製操作");}
    virtual void graphics_dispatch(const GraphicsStage&,std::array<std::uint32_t,3>){throw std::runtime_error("後端缺少運算操作");}
    virtual void graphics_dispatch_indirect(const GraphicsStage&,const std::shared_ptr<GraphicsResource>&,std::uint32_t){throw std::runtime_error("後端缺少間接運算操作");}
    virtual void graphics_mips(const GraphicsView&){throw std::runtime_error("後端缺少 mipmap 操作");}
    // 呼叫期間由呼叫端持有跨程序鎖；返回前 GPU 已完成寫入。
    virtual void graphics_export_frame(const GraphicsView&,void*,std::size_t,std::uint32_t,std::shared_ptr<void> = {}){throw std::runtime_error("後端缺少共用影格匯出");}
    virtual void graphics_present(const GraphicsView&){throw std::runtime_error("後端缺少呈現操作");}
    // 讓消費佇列等待先前命令；不支援 GPU 事件的後端沿用 CPU 完成等待。
    virtual void graphics_handoff(NativeGraphics&,const GraphicsView&){graphics_flush(true);}
    // 提交目前已錄製的工作並回傳完成旗標：GPU 執行到此點（含先前所有提交）後旗標為 true。
    // 供 D3D11 事件查詢（End／GetData 輪詢）使用；不等待 GPU。預設後端同步等待後回傳已完成。
    virtual std::shared_ptr<const std::atomic<bool>> graphics_fence(){graphics_flush(true);return std::make_shared<std::atomic<bool>>(true);}
    // 不提交：回傳「目前尚未提交的命令批次」完成旗標；呼叫端（D3D11 GetData 無 DONOTFLUSH 時）再決定何時 flush。
    virtual std::shared_ptr<const std::atomic<bool>> graphics_fence_pending(){return graphics_fence();}
    virtual void graphics_flush(bool){throw std::runtime_error("後端缺少提交操作");}
    // 呼叫端保證 draw／stage 的內容在下一次 graphics_sync() 前不變時使用；非同步後端可只保留參照。預設同步執行。
    virtual void graphics_draw_retained(const GraphicsDraw& draw){graphics_draw(draw);}
    virtual void graphics_dispatch_retained(const GraphicsStage& stage,std::array<std::uint32_t,3> groups){graphics_dispatch(stage,groups);}
    // 等待先前排入的非同步命令完成，並拋出其中的錯誤；同步後端為空操作。
    virtual void graphics_sync(){}
    // 非同步後端：最後一個排入命令的序號，以及等到某序號（含之前）全部完成；同步後端恆為 0、不等待。
    virtual std::uint64_t graphics_ticket()const{return 0;}
    virtual void graphics_sync_until(std::uint64_t){graphics_sync();}
};
std::uint32_t graphics_format_bytes(std::uint32_t format);
std::uint32_t graphics_format_bc_family(std::uint32_t format) noexcept;
// 回傳實際資料列位元組數與列數；BC 的一列包含 4×4 壓縮區塊。
std::array<std::uint32_t,2> graphics_format_pitch(std::uint32_t format,std::uint32_t width,std::uint32_t height);
struct GraphicsCompilation {std::vector<std::uint8_t> bytecode;std::string messages;};
std::string graphics_disassemble_shader(std::span<const std::uint8_t> bytecode,std::uint32_t flags,std::string_view comments);
GraphicsCompilation graphics_compile_hlsl(std::span<const std::uint8_t> source,const std::string& name,
    const std::string& entry,const std::string& profile,const std::vector<std::pair<std::string,std::string>>& macros,std::uint32_t flags);
}
