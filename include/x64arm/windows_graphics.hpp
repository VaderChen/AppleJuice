#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>
#include <span>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm {
// 遊戲自己的升頻（XeSS、FSR）已初始化的情境數。大於 0 時停用內建的畫面內插與 2 倍放大，由遊戲的升頻輸出畫面；
// 遊戲銷毀升頻情境後恢復。服務層（內建 libxess／FidelityFX）增減，Metal 後端在呈現時讀取。
inline std::atomic<int>& game_upscaler_contexts(){static std::atomic<int> count{0};return count;}
// 情境存在不代表正在使用：把升頻設為關閉的遊戲常保留情境物件。以最近一次實際執行升頻的時間判斷（2 秒內），
// 之後視為停用、恢復內建的畫面內插與 2 倍放大；遊戲再次執行升頻時立即切回。
inline std::atomic<std::int64_t>& game_upscaler_last_dispatch(){static std::atomic<std::int64_t> ticks{0};return ticks;}
inline std::int64_t game_upscaler_clock(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void game_upscaler_dispatched(){game_upscaler_last_dispatch().store(game_upscaler_clock(),std::memory_order_release);}
inline bool game_upscaler_active(){
    if(game_upscaler_contexts().load(std::memory_order_acquire)<=0)return false;
    const auto last=game_upscaler_last_dispatch().load(std::memory_order_acquire);
    return last && game_upscaler_clock()-last<INT64_C(2'000'000'000);
}
class SharedWake;
// D3D9 固定管線及 D3D11 可程式化管線共用同一 Metal context、提交與呈現。
// pending_stamp：後端在途清單最近一次加入此物件的批次編號（Metal 在途追蹤以它略過同批重複加入，不查雜湊表）。
struct GraphicsObject {virtual ~GraphicsObject()=default;std::uint64_t pending_stamp{};};
struct GraphicsResourceDesc {
    std::uint32_t dimension{1},width{},height{1},depth{1},levels{1},layers{1},format{},samples{1},bind{},stride{},misc{};
    std::uint32_t sample_quality{};
    std::uint32_t initialized{}; // 呼叫端建立後會立即寫入的前段位元組數：緩衝的這一段不必先清零。
};
// D3D12 heap 的 GPU 記憶體：placed 資源從同一段記憶體依偏移建立，彼此可以別名（重疊）。
struct GraphicsHeap:GraphicsObject {std::uint64_t size{};bool zeroed{true};};
// 資源在 heap 內需要的大小與對齊（0 表示後端無法提供）。
struct GraphicsResourceAllocation {std::uint64_t size{},alignment{};};
struct GraphicsResource:GraphicsObject {
    GraphicsResourceDesc desc;
    std::uint64_t stream_cursor{},stream_vertices{};
};
struct GraphicsIndirectCountMask {std::shared_ptr<GraphicsResource> arguments;std::uint32_t offset{},stride{},words{},maximum{};
    std::shared_ptr<GraphicsResource> count;std::uint32_t count_offset{},target_offset{};bool dispatch{};};
struct GraphicsStreamElement {std::string semantic;std::uint32_t index{},first{},count{},slot{},stream{};};
struct GraphicsStreamStatistics {std::uint64_t written{},needed{};};
// D3D11_QUERY_DATA_PIPELINE_STATISTICS 的 11 個計數（順序與欄位相同）。由後端在命令執行緒依提交順序累加；
// 像素著色器呼叫數由查詢自己的可見度槽位在 GPU 完成後加總，不在這裡累計。
struct GraphicsPipelineStatistics {std::uint64_t ia_vertices{},ia_primitives{},vs_invocations{},gs_invocations{},gs_primitives{},c_invocations{},c_primitives{},ps_invocations{},hs_invocations{},ds_invocations{},cs_invocations{};};
// type：0 CBV、1 SRV 貼圖／typed buffer、2 取樣器、3 SRV 緩衝、4 UAV 緩衝、5 UAV 貼圖；
// 6～9 為 D3D12 描述元表（bindless 大陣列）：6 SRV 貼圖、7 UAV 貼圖、8 SRV 緩衝、9 UAV 緩衝，以 (space, slot) 對應根簽章範圍。
// count：描述元表宣告的描述元數（~0U 為無界；0 表示非描述元表）。
// source：D3D12 離散資源的非零 space／高編號暫存器改用未占用 slot，這裡保存原始暫存器（UINT32_MAX 表示未重新對應）。
// dimension：著色器宣告的貼圖維度（數值同 D3D_SRV_DIMENSION，0 表示未知）。
struct GraphicsBinding {std::uint32_t type{},slot{},native_slot{},counter_slot{UINT32_MAX},space{},count{},source{UINT32_MAX},dimension{};};
struct GraphicsInput {std::string semantic;std::uint32_t index{},attribute{},component{};}; // component：0 浮點、1 無號、2 有號。
struct GraphicsLegacyShader {std::string source,entry;std::vector<GraphicsBinding> bindings;std::vector<GraphicsInput> inputs;};
GraphicsLegacyShader graphics_legacy_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes,std::uint32_t shadow_samplers=0);
// SM5.1 DXBC 與 DXIL：經 vkd3d-shader（SPIR-V）與 SPIRV-Cross 轉為 MSL；入口為 xlatMtlMain。
// 原始 D3D 簽章的位置與分量遮罩；階段連結不能把轉譯器的 locnN 當成語意名稱。
struct GraphicsShaderSemantic {std::string semantic;std::uint32_t index{},location{},mask{};};
struct GraphicsModernShader {std::string source,entry;std::vector<GraphicsBinding> bindings;std::vector<GraphicsInput> inputs;std::vector<GraphicsShaderSemantic> signature_inputs,signature_outputs;std::array<std::uint32_t,3> threads{1,1,1};std::uint32_t buffer_sizes_slot{UINT32_MAX};std::uint32_t capture_output_slot{UINT32_MAX};
    // 細分階段的內部緩衝槽位（依 GraphicsTessellationSlot 索引；UINT32_MAX 表示未使用），及 HS 的細分設定（GraphicsTessellationInfo 索引）。
    std::array<std::uint32_t,7> tess_slots{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::array<std::uint32_t,6> tess_info{};};
enum GraphicsTessellationSlot:std::uint32_t {graphics_tess_params,graphics_tess_input,graphics_tess_output,graphics_tess_patch_output,graphics_tess_factor,graphics_tess_patch_input,graphics_tess_index};
// domain：0 三角形、1 四邊形、2 等值線；spacing：0 integer、1 fractional_odd、2 fractional_even；winding：0 順時針、1 逆時針。
enum GraphicsTessellationInfo:std::uint32_t {graphics_tess_domain,graphics_tess_spacing,graphics_tess_winding,graphics_tess_output_points,graphics_tess_point_mode,graphics_tess_input_points};
// 現代路徑的細分管線連結：VS 轉成擷取輸出的 compute kernel，HS 是 tessellation control kernel，DS 是 post-tessellation vertex 函式。
// 相鄰階段的介面依生產端的 location 與分量數對齊（{location, 分量數}），消費端缺少的欄位補位、分量較少者補齊，兩端結構版面一致；
// 位置內建變數另給一個 location。patch 常數的 location 正規化到 patch_base 之後。
struct GraphicsModernTessellationLink {
    std::vector<std::array<std::uint32_t,2>> vertex_locations,control_locations,patch_locations;
    std::uint32_t vertex_position{UINT32_MAX},control_position{UINT32_MAX},vertex_point_size{},patch_base{},hull_patch_offset{},domain_patch_offset{};
    // 帶非零 Component 的介面變數（同一暫存器有多個未合併的元素）：SPIRV-Cross 的細分階段不支援 Component，
    // 生產端與消費端都把這些 (暫存器, 分量) 改到「基底＋暫存器×4＋分量」的獨立 location；形狀取自生產端實際的 SPIR-V 變數。
    std::vector<std::array<std::uint32_t,2>> vertex_packed,control_packed,patch_packed;
    std::uint32_t vertex_packed_base{},control_packed_base{},patch_packed_base{};
    std::array<std::uint32_t,6> info{};
    std::uint32_t index_type{}; // VS：0 非索引、1 uint16、2 uint32
};
bool graphics_modern_shader_required(std::span<const std::uint8_t> bytes);
GraphicsModernShader graphics_modern_point_capture(std::span<const std::uint8_t> bytes);
GraphicsModernShader graphics_modern_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes);
// 由三個階段的位元組碼建立連結資訊（介面 location、patch 常數偏移、HS 的細分設定）；再以 graphics_modern_tessellation_stage 依連結轉譯 VS（階段 0）、HS（3）、DS（4）。
GraphicsModernTessellationLink graphics_modern_tessellation_link(std::span<const std::uint8_t> vertex,std::span<const std::uint8_t> hull,std::span<const std::uint8_t> domain);
GraphicsModernShader graphics_modern_tessellation_stage(std::uint32_t stage,std::span<const std::uint8_t> bytes,const GraphicsModernTessellationLink& link);
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
    // 結構化元素大小：D3D12 的 stride 屬於 view，不屬於資源；0 表示沿用資源或格式。
    // graphics_view_stride_unknown：根描述元沒有 stride，由著色器決定。
    std::uint32_t stride{};
    // 計數器在 counter 資源內的位元組位移（D3D12 的 CounterOffsetInBytes；D3D11 的隱藏計數器為 0）。
    std::uint32_t counter_offset{};
    // D3D12 SRV 的 Shader4ComponentMapping：0 表示原樣（R、G、B、A）；否則低 12 位元依序為 R／G／B／A 的來源
    //（各 3 位元：0～3 取記憶體分量、4 固定 0、5 固定 1），並設第 12 位元。只用於貼圖 SRV。
    std::uint32_t swizzle{};
};
inline constexpr std::uint32_t graphics_view_stride_unknown=UINT32_MAX;
// 時間性升頻（XeSS 等升頻 API）：抖動取樣的低解析度色彩、深度與動態向量合成高解析度輸出。
// 動態向量以輸入像素為單位、指向上一幀；motion_scale 先換算成這個單位。exposure 可為空（1×1 曝光紋理）。
struct GraphicsTemporalUpscale {
    GraphicsView color,depth,motion,exposure,output;
    std::uint32_t input_width{},input_height{},output_width{},output_height{};
    float jitter_x{},jitter_y{},motion_scale_x{1},motion_scale_y{1},pre_exposure{1};
    bool reset{},depth_reversed{},auto_exposure{};
    std::uint64_t history{}; // 呼叫端的歷史編號（每個升頻情境一個）：同一個編號沿用上一幀的歷史。
};
// D3D12 描述元表：heap 為描述元堆積的 GPU 鏡像（貼圖每項 8 位元組 MTLResourceID；緩衝每項位址＋長度共 16 位元組），
// offset 為此表第一個描述元在鏡像中的位元組位置。views：有界小表逐項解析的資源，後端據此宣告使用以維持危害追蹤。
// 遮擋查詢解析的一筆：該查詢的 visibility 計數緩衝與已使用的槽數（每槽 8 位元組）。
struct GraphicsOcclusionQuery {std::shared_ptr<GraphicsResource> buffer;std::uint32_t slots{};};
struct GraphicsDescriptorTable {std::uint32_t type{},space{},slot{};std::shared_ptr<GraphicsResource> heap;std::uint64_t offset{};std::vector<GraphicsView> views;};
struct GraphicsStage {
    std::shared_ptr<GraphicsShader> shader;
    // b14 保留給引擎的動態 linkage 資料；D3D11 公開綁定仍限 b0–b13。
    std::array<std::shared_ptr<GraphicsResource>,15> constants;
    // D3D11.1 常數緩衝範圍的起點（位元組，256 對齊）；0 表示從緩衝起點綁定。
    std::array<std::uint32_t,15> constant_offsets{};
    std::array<GraphicsView,128> resources;
    std::array<GraphicsView,8> unordered;
    std::array<std::shared_ptr<GraphicsSampler>,16> samplers;
    std::vector<GraphicsDescriptorTable> tables;
    // 運算階段的管線統計累計目標（D3D11 CSInvocations）；其他階段不用。
    std::shared_ptr<GraphicsPipelineStatistics> statistics;
};
struct GraphicsElement {std::string semantic;std::uint32_t index{},format{},slot{},offset{},classification{},step{};};
struct GraphicsVertexBuffer {std::shared_ptr<GraphicsResource> resource;std::uint32_t stride{},offset{};};
struct GraphicsDraw {
    std::array<std::shared_ptr<GraphicsResource>,4> stream_targets;
    std::shared_ptr<GraphicsStreamStatistics> stream_statistics;
    std::shared_ptr<GraphicsPipelineStatistics> pipeline_statistics;
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
    // 連續 indirect_count 筆間接參數（每筆相隔 indirect_stride 位元組）共用同一組狀態依序繪製，例如 ExecuteIndirect 遮罩後的參數陣列；
    // 只用於沒有幾何／曲面細分與遮蔽查詢的繪製。
    std::uint32_t indirect_count{1},indirect_stride{};
    // 非零時，值相同的繪製其著色器、輸入佈局、混色／深度／光柵狀態與 sample mask 皆相同（D3D12 PSO 建立後不變），
    // 後端可據此快取管線；0 表示不保證（D3D11 等逐次比對）。
    std::uint64_t pipeline_serial{};
    // D3D11 不可變的輸入佈局與混色物件序號。佈局序號為 0 時使用完整管線鍵。
    std::uint64_t pipeline_layout_serial{},pipeline_blend_serial{};
    // D3D12：draw 之間的 UAV 存取不保證順序，順序只由應用程式的 UAV barrier 指定（後端以 graphics_uav_barrier 收到）。
    // D3D11 為 false：draw 間 UAV 依序，有讀取或原子操作的像素 UAV 每個 draw 一個 render pass。
    bool explicit_uav_order{};
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
    // 固定管線與可程式化管線使用相同的硬體可見度計數；空資源解除目前繪製的查詢。
    virtual void graphics_fixed_visibility(const std::shared_ptr<GraphicsResource>& resource,std::uint32_t){if(resource)throw std::runtime_error("後端缺少固定管線可見度查詢");}
    virtual void graphics_fixed_targets(const GraphicsView&,const GraphicsView&){throw std::runtime_error("後端缺少固定管線繪製表面切換");}
    // enable、reference、read/write mask，接續正／背面的 compare、fail、zfail、pass（D3D enum）。
    virtual bool graphics_fixed_texture(const GraphicsView&){return false;}
    virtual bool graphics_fixed_sampler(const std::shared_ptr<GraphicsSampler>&){return false;}
    virtual void graphics_fixed_stencil(const std::array<std::uint32_t,12>&){throw std::runtime_error("後端缺少固定管線雙面 stencil");}
    virtual void graphics_fixed_draw(const FixedGraphicsDraw&){throw std::runtime_error("後端缺少固定管線頂點處理");}
    virtual std::shared_ptr<GraphicsResource> graphics_resource(const GraphicsResourceDesc&){throw std::runtime_error("後端缺少可程式化資源");}
    // 零複製緩衝：以呼叫端的宿主記憶體（宿主分頁對齊、長度為分頁倍數）作為緩衝內容，GPU 直接讀寫同一段記憶體；
    // owner 持有記憶體直到資源釋放。後端不支援或位址不符時回傳空值，呼叫端改用一般資源加上傳。
    virtual std::shared_ptr<GraphicsResource> graphics_buffer_alias(std::shared_ptr<void>,std::span<std::uint8_t>){return nullptr;}
    // D3D12 heap：graphics_heap 建立（實體記憶體可延後到第一個 placed 資源才配置），graphics_resource_placed 在指定偏移建立資源，
    // 與同一 heap 內其他資源共用記憶體；偏移未對齊或超出 heap 時回傳空值，呼叫端改用獨立配置。
    // graphics_resource_allocation 回報 placed 建立所需的大小與對齊，供 GetResourceAllocationInfo 讓應用程式的配置不重疊。
    // zeroed：D3D12 heap 建立時內容為零（D3D12_HEAP_FLAG_CREATE_NOT_ZEROED 除外），實體記憶體配置時一併清零。
    virtual std::shared_ptr<GraphicsHeap> graphics_heap(std::uint64_t,bool){return nullptr;}
    virtual std::shared_ptr<GraphicsResource> graphics_resource_placed(const GraphicsResourceDesc&,const std::shared_ptr<GraphicsHeap>&,std::uint64_t){return nullptr;}
    virtual GraphicsResourceAllocation graphics_resource_allocation(const GraphicsResourceDesc&){return {};}
    // 在途命令緩衝上限：每幀多次提交、由應用程式以 fence 控制幀數的 API（D3D12）可提高，CPU 不必每幾次提交就等 GPU。
    // 預設後端不限制；明確設定的環境變數優先。
    virtual void graphics_submission_depth(std::uint32_t){}
    virtual bool graphics_samples_supported(std::uint32_t,std::uint32_t){return false;}
    virtual bool graphics_vertex_format_supported(std::uint32_t){return false;}
    // 呼叫端之後會修改物件（例如加上串流輸出宣告）時使用：後端可依內容共用編譯結果，但回傳私有物件。
    virtual std::shared_ptr<GraphicsShader> graphics_shader_private(std::uint32_t stage,std::span<const std::uint8_t> bytes){return graphics_shader(stage,bytes);}
    virtual std::shared_ptr<GraphicsShader> graphics_shader(std::uint32_t,std::span<const std::uint8_t>){throw std::runtime_error("後端缺少 DXBC 著色器");}
    // D3D9 SM1–3：shadow_samplers 的位元指定以比較取樣處理的取樣器（由繪製端依綁定的陰影深度紋理決定）。
    virtual std::shared_ptr<GraphicsShader> graphics_legacy_shader_variant(std::uint32_t stage,std::span<const std::uint8_t> bytes,std::uint32_t shadow_samplers){
        if(shadow_samplers)throw std::runtime_error("後端缺少 D3D9 陰影取樣器變體");return graphics_shader(stage,bytes);}
    // 背景準備著色器：已在快取、曾經失敗或後端不支援背景編譯時回 0（呼叫端照常同步建立）；否則啟動或沿用進行中的背景
    // 轉譯與編譯並回傳非零票號，呼叫端可讓客體執行緒等票號完成後重試，不在持鎖的服務內編譯。
    virtual std::uint64_t graphics_shader_prefetch(std::uint32_t,std::span<const std::uint8_t>,
                                                   const std::shared_ptr<SharedWake>& = {}){return 0;}
    virtual bool graphics_shader_ticket_ready(std::uint64_t){return true;}
    // 由相容層自行產生的 MSL compute 核心（例如 DirectML 運算子）；bindings 的 slot 對應 GraphicsStage 陣列。
    virtual std::shared_ptr<GraphicsShader> graphics_compute_source(const GraphicsModernShader&){throw std::runtime_error("後端缺少 MSL compute 核心");}
    virtual std::shared_ptr<GraphicsSampler> graphics_sampler(std::span<const std::uint8_t>){throw std::runtime_error("後端缺少取樣器");}
    virtual bool graphics_timestamp_supported()const{return false;}
    virtual GraphicsTimestamp graphics_timestamp(){throw std::runtime_error("後端沒有 GPU timestamp");}
    // 目前的 GPU 時鐘值（與 graphics_timestamp／查詢時間戳同一時基），不提交命令、不等 GPU；不支援時為空。
    // 供 D3D12 GetClockCalibration 與 CPU 時鐘配對取樣。
    virtual std::optional<std::uint64_t> graphics_gpu_clock(){return std::nullopt;}
    // 在目前提交點記錄 GPU 時間戳，不等待；供 D3D11 TIMESTAMP／TIMESTAMP_DISJOINT 查詢輪詢。預設後端同步取樣後直接完成。
    virtual std::shared_ptr<const GraphicsTimestampFuture> graphics_timestamp_future(){auto future=std::make_shared<GraphicsTimestampFuture>();future->value=graphics_timestamp();future->done.store(true,std::memory_order_release);return future;}
    virtual void graphics_write(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t){throw std::runtime_error("後端缺少資源寫入");}
    virtual void graphics_write_discard(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){graphics_write(view,data,row,image);}
    // WRITE_NO_OVERWRITE 的整個資源上傳：呼叫端保證 GPU 使用中的範圍與上次上傳的位元組相同，後端可直接寫入而不等待。
    virtual void graphics_write_no_overwrite(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){graphics_write(view,data,row,image);}
    // 緩衝範圍寫入，呼叫端保證這個範圍尚未被任何已編碼的命令引用（暫存區每段只寫一次再綁定）：後端可直接寫入
    // 共享記憶體，不等待 GPU、不經 staging 複製（不切斷進行中的 render encoder）。
    virtual void graphics_write_unused(const GraphicsView& view,std::span<const std::uint8_t> data){graphics_write(view,data,static_cast<std::uint32_t>(data.size()),static_cast<std::uint32_t>(data.size()));}
    virtual void graphics_write_region(const GraphicsView&,std::span<const std::uint8_t>,std::uint32_t,std::uint32_t,std::array<std::uint32_t,6>){throw std::runtime_error("後端缺少區域資源寫入");}
    virtual std::vector<std::uint8_t> graphics_read(const GraphicsView&){throw std::runtime_error("後端缺少資源讀回");}
    // 描述元表項目：type 6／7 回傳 view 的 GPU 資源 ID（value[0]），8／9 回傳緩衝位址與長度；空 view 回傳後端的預設空資源。
    // keep 持有 view 物件直到描述元被覆寫；被引用的配置由後端維持駐留。
    virtual bool graphics_descriptor(const GraphicsView&,std::uint32_t,std::array<std::uint64_t,2>&,std::shared_ptr<void>&){return false;}
    // 取樣器堆積鏡像項目（8 位元組資源 ID）：SM6.6 SamplerDescriptorHeap 與取樣器描述元表使用；空取樣器回傳預設取樣器。
    virtual bool graphics_sampler_descriptor(const std::shared_ptr<GraphicsSampler>&,std::uint64_t&,std::shared_ptr<void>&){return false;}
    // 取樣器的縮減模擬旗標（位元 0～1：2＝MIN、3＝MAX；位元 2：線性 mip；位元 4～6／8～10／12～14：U／V／W 定址）；0 表示一般取樣。
    // 現代著色器以此在不支援取樣器縮減的 GPU 上模擬 D3D 的 MINIMUM_*／MAXIMUM_* 篩選（見 d3d12_shader.cpp）。
    virtual std::uint32_t graphics_sampler_flags(const std::shared_ptr<GraphicsSampler>&){return 0;}
    // 呼叫端已確認寫入此範圍的 GPU 工作都已完成（例如遮蔽查詢 fence）時讀回：不等待同一資源上較新的批次，
    // 非同步後端也不先排空命令佇列。只用於不會被 discard 改名的緩衝；預設沿用 graphics_read。
    virtual std::vector<std::uint8_t> graphics_read_completed(const GraphicsView& view){return graphics_read(view);}
    virtual void graphics_copy(const GraphicsView&,const GraphicsView&,std::array<std::uint32_t,3>,std::array<std::uint32_t,6>){throw std::runtime_error("後端缺少資源複製");}
    // 緩衝足跡與紋理子資源之間的 GPU 複製（依命令順序，不經 CPU）：offset／row／image 是緩衝端位移、列距與層距，
    // box 是紋理端範圍（texel）。to_texture 為 true 時緩衝→紋理。格式或對齊不支援時回傳 false 且不排入任何命令。
    virtual bool graphics_copy_buffer_texture(const GraphicsView&,const GraphicsView&,std::uint64_t,std::uint32_t,std::uint32_t,std::array<std::uint32_t,6>,bool){return false;}
    virtual void graphics_resolve(const GraphicsView&,const GraphicsView&,std::uint32_t){throw std::runtime_error("後端缺少多重取樣 resolve");}
    virtual bool graphics_temporal_upscale_supported(){return false;}
    virtual void graphics_temporal_upscale(const GraphicsTemporalUpscale&){throw std::runtime_error("後端缺少時間性升頻");}
    virtual void graphics_temporal_upscale_release(std::uint64_t){}
    virtual void graphics_clear(const GraphicsView&,std::array<float,4>,std::uint32_t){throw std::runtime_error("後端缺少清除操作");}
    virtual bool graphics_clear_unordered(const GraphicsView&,std::array<std::uint32_t,4>,bool){throw std::runtime_error("後端缺少 UAV 清除操作");}
    virtual void graphics_draw(const GraphicsDraw&){throw std::runtime_error("後端缺少繪製操作");}
    virtual void graphics_dispatch(const GraphicsStage&,std::array<std::uint32_t,3>){throw std::runtime_error("後端缺少運算操作");}
    virtual void graphics_dispatch_indirect(const GraphicsStage&,const std::shared_ptr<GraphicsResource>&,std::uint32_t){throw std::runtime_error("後端缺少間接運算操作");}
    // ExecuteIndirect 計數緩衝（依命令順序、不經 CPU）：每項把 arguments 自 offset 起每 stride 位元組一筆、每筆 words 個 32 位元字的參數，
    // 依 count 資源 count_offset 處的計數遮罩後緊密寫入 target 的 target_offset——第 i≥min(計數, maximum) 筆的實例數（繪製）或三個群組數（dispatch）歸零。
    // 一批共用一個 compute 段落；任一項形狀不支援就回傳 false 且不排入任何命令，呼叫端改回讀計數。
    virtual bool graphics_indirect_count_masks(std::span<const GraphicsIndirectCountMask>,const std::shared_ptr<GraphicsResource>&){return false;}
    // 遮擋查詢解析（D3D12 ResolveQueryData）：每個查詢的 visibility 緩衝前 slots 個 64 位元計數相加，依序寫到 destination 的 offset 起
    //（binary 時非零寫 1）。在命令流中於 GPU 完成，不讓 CPU 等待；buffer 為空的查詢寫 0。後端不支援時回傳 false。
    virtual bool graphics_resolve_occlusion(std::span<const GraphicsOcclusionQuery>,const std::shared_ptr<GraphicsResource>&,std::uint64_t,bool){return false;}
    virtual void graphics_mips(const GraphicsView&){throw std::runtime_error("後端缺少 mipmap 操作");}
    // 呼叫期間由呼叫端持有跨程序鎖；返回前 GPU 已完成寫入。
    virtual void graphics_export_frame(const GraphicsView&,void*,std::size_t,std::uint32_t,std::shared_ptr<void> = {}){throw std::runtime_error("後端缺少共用影格匯出");}
    virtual void graphics_present(const GraphicsView&){throw std::runtime_error("後端缺少呈現操作");}
    // 呈現是否不必等顯示緩衝：false 表示此刻呈現會阻塞到垂直同步釋出緩衝（CAMetalLayer 的 drawable 都在途）。
    // 呼叫端可先讓出 vCPU 再重試；只讀原子計數。沒有緩衝佇列的後端一律為 true。
    virtual bool graphics_present_ready() const noexcept {return true;}
    // 讓消費佇列等待先前命令；不支援 GPU 事件的後端沿用 CPU 完成等待。
    virtual void graphics_handoff(NativeGraphics&,const GraphicsView&){graphics_flush(true);}
    // 提交目前已錄製的工作並回傳完成旗標：GPU 執行到此點（含先前所有提交）後旗標為 true。
    // 供 D3D11 事件查詢（End／GetData 輪詢）使用；不等待 GPU。預設後端同步等待後回傳已完成。
    virtual std::shared_ptr<const std::atomic<bool>> graphics_fence(){graphics_flush(true);return std::make_shared<std::atomic<bool>>(true);}
    // 不提交：回傳「目前尚未提交的命令批次」完成旗標；呼叫端（D3D11 GetData 無 DONOTFLUSH 時）再決定何時 flush。
    virtual std::shared_ptr<const std::atomic<bool>> graphics_fence_pending(){return graphics_fence();}
    virtual void graphics_flush(bool){throw std::runtime_error("後端缺少提交操作");}
    // D3D12 UAV barrier：之後的 draw／dispatch 須看見之前的 UAV 寫入。預設不需動作（後端不合併 UAV draw 時已依序）。
    virtual void graphics_uav_barrier(){}
    // 呼叫端保證 draw／stage 的內容在下一次 graphics_sync() 前不變時使用；非同步後端可只保留參照。預設同步執行。
    virtual void graphics_draw_retained(const GraphicsDraw& draw){graphics_draw(draw);}
    virtual void graphics_dispatch_retained(const GraphicsStage& stage,std::array<std::uint32_t,3> groups){graphics_dispatch(stage,groups);}
    // 不可變快照（D3D11 延遲內容錄下的命令）：呼叫端保證內容不再改變，後端可直接持有指標排隊而不必再複製一份
    //（GraphicsDraw 約 70 KB，原本重播時每次繪製都在呼叫執行緒複製）。預設退回一般版本。
    virtual void graphics_draw_shared(const std::shared_ptr<const GraphicsDraw>& draw){graphics_draw(*draw);}
    virtual void graphics_dispatch_shared(const std::shared_ptr<const GraphicsStage>& stage,std::array<std::uint32_t,3> groups){graphics_dispatch(*stage,groups);}
    virtual void graphics_dispatch_indirect_shared(const std::shared_ptr<const GraphicsStage>& stage,const std::shared_ptr<GraphicsResource>& arguments,std::uint32_t offset){
        graphics_dispatch_indirect(*stage,arguments,offset);}
    // 等待先前排入的非同步命令完成，並拋出其中的錯誤；同步後端為空操作。
    virtual void graphics_sync(){}
    // 把一段後端工作依序排在先前的命令之後執行（非同步後端在編碼執行緒上以內層後端呼叫；同步後端立即執行）。
    // task 只能使用傳入的後端與自行擁有的資料。
    virtual void graphics_post(std::function<void(NativeGraphics&)> task){task(*this);}
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
