#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/d3d12_abi.hpp"
#include "x64arm/unicode.hpp"
#include "windows_display_identity.hpp"
#include "x64arm/shared_wake.hpp"
#include <vkd3d_shader.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>

namespace x64arm {
// Direct3D 12：與 D3D9／10／11 共用同一 Metal 圖形後端（NativeGraphics）。
// 命令清單於錄製時保存參數，ExecuteCommandLists 依序重播為 NativeGraphics 呼叫後同步完成，佇列 Signal 隨後生效。
// UPLOAD／READBACK 堆積的資源以客體記憶體為後備，Map 直接回傳該位址；DEFAULT 堆積資源位於 GPU。
// 根簽章以 vkd3d-shader 序列化／解析；SM5.0 著色器沿用 HLSLcc，SM5.1／DXIL 經 SPIR-V 轉 MSL。
// 運算、繪圖、交換鏈、間接命令、Bundle、條件執行與 UAV 清除均使用共用 NativeGraphics／Metal 後端。
// 參考：https://learn.microsoft.com/windows/win32/direct3d12/directx-12-programming-guide
// D3D12 重播的 Metal 編碼執行緒（預設開啟，X64ARM_D3D12_COMMAND_THREAD=0 關閉）：裝置後端包上 MetalCommandThread，提交執行緒只做 D3D12
// 前端翻譯，繪製、運算、複製與描述元鏡像更新依序交給編碼執行緒，兩段並行（原本同一條執行緒約各占一半）。
// 繪製狀態以環狀池傳遞：重播的 draw 狀態整份約 64 KB，每次繪製整份複製太貴；池格只複製著色器綁定實際讀取的槽位
// 並記下設定過的槽位，下次重用前清除。池格以排入序號確認編碼執行緒已用完才重用。
struct D3D12DrawPool {
    struct Slot {GraphicsDraw draw;std::uint64_t ticket{};std::array<std::vector<std::pair<std::uint8_t,std::uint8_t>>,6> set;};
    std::vector<std::unique_ptr<Slot>> slots;std::size_t next{};
    // 池格數＝前端可領先編碼執行緒的繪製數；X64ARM_D3D12_DRAW_POOL=<格數>（16～1024）調整，對照用。
    static unsigned capacity(){static const unsigned value=[] {const auto* text=std::getenv("X64ARM_D3D12_DRAW_POOL");
        return text?std::clamp<unsigned>(static_cast<unsigned>(std::strtoul(text,nullptr,10)),16,1024):64U;}();return value;}
    D3D12DrawPool(){slots.reserve(capacity());for(unsigned i=0;i<capacity();++i)slots.push_back(std::make_unique<Slot>());}
    // wait_ns：等池格空出（編碼執行緒尚未用完）的累計時間，供提交統計分開列出等待與複製。
    std::uint64_t wait_ns{};
    // 指派前先比較：池格上一次（數十次繪製前）的內容常與這次相同（同一批目標、取樣器、頂點緩衝、描述元堆積），
    // 相同就不重新指派，省去 shared_ptr 參照計數的原子操作（編碼執行緒同時讀寫同一批資源的計數，會互搶快取列）。
    // X64ARM_D3D12_POOL_SHARE=0 恢復每次重設再複製（對照用）。
    template<class T> static void share(std::shared_ptr<T>& to,const std::shared_ptr<T>& from){if(to!=from)to=from;}
    static void share(GraphicsView& to,const GraphicsView& from){
        share(to.resource,from.resource);share(to.counter,from.counter);
        to.format=from.format;to.level=from.level;to.levels=from.levels;to.slice=from.slice;to.slices=from.slices;
        to.first=from.first;to.count=from.count;to.flags=from.flags;to.dimension=from.dimension;
    }
    static bool share_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_D3D12_POOL_SHARE");return !text || std::string_view(text)!="0";}();return value;}
    Slot& fill(NativeGraphics& native,const GraphicsDraw& source){
        auto& slot=*slots[next];next=(next+1)%slots.size();
        if(slot.ticket){
            static const bool timing=std::getenv("X64ARM_TRACE_D3D12_SUBMIT")!=nullptr;
            const auto start=timing?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};native.graphics_sync_until(slot.ticket);
            if(timing)wait_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
        }
        auto& target=slot.draw;const bool keep=share_enabled();
        for(unsigned s=0;s<target.stages.size();++s){
            auto& stage=target.stages[s];const auto& from=source.stages[s];
            // 這次設定的槽位（種類 0 常數、1 資源、2 取樣器、3 UAV）；上次設定、這次沒設定的才清除。
            std::array<std::uint64_t,2> resources{};std::uint32_t constants=0,samplers=0,unordered=0;
            const auto mark=[&](std::uint8_t kind,std::uint8_t index){
                switch(kind){case 0:constants|=1U<<index;break;case 1:resources[index>>6]|=std::uint64_t{1}<<(index&63U);break;case 2:samplers|=1U<<index;break;default:unordered|=1U<<index;break;}};
            const auto marked=[&](std::uint8_t kind,std::uint8_t index){
                switch(kind){case 0:return ((constants>>index)&1U)!=0;case 1:return ((resources[index>>6]>>(index&63U))&1U)!=0;case 2:return ((samplers>>index)&1U)!=0;default:return ((unordered>>index)&1U)!=0;}};
            // 與暫存清單交換（不配置）：previous 取得上次的設定清單，池格清單沿用暫存的容量重新填寫。
            thread_local std::vector<std::pair<std::uint8_t,std::uint8_t>> previous;previous.clear();previous.swap(slot.set[s]);
            if(!keep){
                for(const auto [kind,index]:previous)switch(kind){
                    case 0:stage.constants[index].reset();stage.constant_offsets[index]=0;break;
                    case 1:stage.resources[index]={};break;
                    case 2:stage.samplers[index].reset();break;
                    default:stage.unordered[index]={};break;
                }
                previous.clear();stage.tables.clear();stage.shader.reset();
            }
            bool tables=false;
            if(from.shader){
                share(stage.shader,from.shader);
                for(const auto& binding:from.shader->bindings){
                    const auto index=binding.slot;
                    switch(binding.type){
                        case 0:if(index<stage.constants.size()){share(stage.constants[index],from.constants[index]);stage.constant_offsets[index]=from.constant_offsets[index];slot.set[s].emplace_back(0,index);mark(0,static_cast<std::uint8_t>(index));}break;
                        case 1:case 3:if(index<stage.resources.size()){share(stage.resources[index],from.resources[index]);slot.set[s].emplace_back(1,index);mark(1,static_cast<std::uint8_t>(index));}break;
                        case 2:if(index<stage.samplers.size()){share(stage.samplers[index],from.samplers[index]);slot.set[s].emplace_back(2,index);mark(2,static_cast<std::uint8_t>(index));}break;
                        case 4:case 5:if(index<stage.unordered.size()){share(stage.unordered[index],from.unordered[index]);slot.set[s].emplace_back(3,index);mark(3,static_cast<std::uint8_t>(index));}break;
                        default:
                            if(!tables){
                                tables=true;stage.tables.resize(from.tables.size());
                                for(std::size_t t=0;t<from.tables.size();++t){
                                    auto& to=stage.tables[t];const auto& table=from.tables[t];
                                    to.type=table.type;to.space=table.space;to.slot=table.slot;to.offset=table.offset;share(to.heap,table.heap);
                                    to.views.resize(table.views.size());for(std::size_t v=0;v<table.views.size();++v)share(to.views[v],table.views[v]);
                                }
                            }
                            break;
                    }
                }
            }else stage.shader.reset();
            if(!tables)stage.tables.clear();
            for(const auto [kind,index]:previous)if(!marked(kind,index))switch(kind){
                case 0:stage.constants[index].reset();stage.constant_offsets[index]=0;break;
                case 1:stage.resources[index]={};break;
                case 2:stage.samplers[index].reset();break;
                default:stage.unordered[index]={};break;
            }
        }
        for(std::size_t i=0;i<target.stream_targets.size();++i)share(target.stream_targets[i],source.stream_targets[i]);
        share(target.stream_statistics,source.stream_statistics);
        // 輸入佈局與混色／深度／光柵狀態由 PSO 決定：序號相同（非零）就沿用池格內容。
        if(!keep || !source.pipeline_serial || target.pipeline_serial!=source.pipeline_serial){
            target.layout=source.layout;target.blend=source.blend;target.depth_state=source.depth_state;target.raster=source.raster;target.sample_mask=source.sample_mask;
        }
        for(std::size_t i=0;i<target.vertices.size();++i){share(target.vertices[i].resource,source.vertices[i].resource);target.vertices[i].stride=source.vertices[i].stride;target.vertices[i].offset=source.vertices[i].offset;}
        share(target.indices,source.indices);target.index_format=source.index_format;target.index_offset=source.index_offset;
        target.topology=source.topology;for(std::size_t i=0;i<target.targets.size();++i)share(target.targets[i],source.targets[i]);share(target.depth,source.depth);
        target.viewports=source.viewports;target.scissors=source.scissors;
        target.blend_factor=source.blend_factor;
        target.stencil_reference=source.stencil_reference;target.count=source.count;target.start=source.start;
        target.instances=source.instances;target.start_instance=source.start_instance;target.base_vertex=source.base_vertex;
        target.alpha_function=source.alpha_function;target.alpha_reference=source.alpha_reference;target.indexed=source.indexed;
        share(target.visibility,source.visibility);target.visibility_offset=source.visibility_offset;share(target.indirect,source.indirect);target.indirect_offset=source.indirect_offset;
        target.pipeline_serial=source.pipeline_serial;
        return slot;
    }
};
struct D3D12State {
    template<class T> static std::unordered_map<std::uint64_t,T> reserved(std::size_t count){std::unordered_map<std::uint64_t,T> table;table.reserve(count);return table;}
    enum class Kind {Device,Queue,Allocator,List,Fence,Resource,DescriptorHeap,RootSignature,Pipeline,QueryHeap,Heap,Blob,CommandSignature,SwapChain,PipelineLibrary};
    struct Object {Kind kind{};unsigned references{1};std::uint64_t device{};std::map<std::array<std::uint8_t,16>,std::vector<std::uint8_t>> data;};
    struct Resource {
        std::uint32_t dimension{},format{},height{1},depth{1},levels{1},samples{1},flags{},heap_type{1};std::uint64_t width{};
        std::uint64_t address{},cpu{},cpu_size{};std::shared_ptr<GraphicsResource> gpu;std::vector<std::uint8_t> desc;std::uint32_t structure{};
        bool owned_cpu{true}; // placed 資源的 CPU 後備屬於堆積，不由資源釋放。
        std::uint32_t cpu_page{}; // CUSTOM 堆積的 D3D12_CPU_PAGE_PROPERTY（1 不可存取、2 write-combine、3 write-back）。
        // UMA 上 CPU 可見的 CUSTOM 堆積同時具備 UPLOAD（GPU 使用前更新）與 READBACK（GPU 寫入後回寫）語意。
        bool uploads() const {return heap_type==2 || (heap_type==4 && cpu_page>1);}
        bool reads_back() const {return heap_type==3 || (heap_type==4 && cpu_page>1);}
        // 零複製：CPU 後備與 GPU 緩衝是同一段宿主記憶體，GPU 直接讀取客體寫入的資料、GPU 寫入直接對客體可見，
        // 不做重播上傳與讀回。host_mapping 非零表示客體映射屬於此資源（committed），釋放時撤除。
        bool host_shared{};std::uint64_t host_mapping{};
        // 零複製讀回資源最近一次 GPU 寫入所屬提交的完成旗標：Map 時尚未完成就先等 GPU（與原本 Map 時讀回的語義相同）。
        std::shared_ptr<const std::atomic<bool>> gpu_writes;
        // 非同步提交時，最近一次寫入此讀回資源的 ExecuteCommandLists 排入序號（D3D12Submitter::queued）：客體呼叫在提交執行緒
        // 重播前就返回，gpu_writes 尚未設定；Map 先等提交執行緒完成到此序號（只等這一批，不清空整個佇列）。
        std::uint64_t submit_ticket{};
    };
    // 描述元鏡像值快取：SRV／UAV 建立時配置，複製描述元時一併共用。編碼執行緒第一次算出的鏡像值（貼圖 ID、緩衝位址＋長度）
    // 連同當時的後端資源記下，之後同一份描述元被複製到著色器可見堆積時直接沿用，不再逐項呼叫 graphics_descriptor。
    // 只由編碼執行緒讀寫（描述元複製只複製 shared_ptr）。
    struct MirrorCache {const GraphicsResource* resource{};bool valid{},uav{};std::uint64_t texture{};std::array<std::uint64_t,2> buffer{};std::shared_ptr<void> keep;};
    struct Descriptor {
        enum class Type {Empty,Constant,Shader,Unordered,Sampler,Target,Depth} type{Type::Empty};
        GraphicsView view;std::uint64_t resource{},address{};std::uint32_t size{};std::shared_ptr<GraphicsSampler> sampler;std::shared_ptr<MirrorCache> mirror;
    };
    // shader-visible CBV_SRV_UAV 堆積另有 GPU 鏡像供描述元表（bindless）使用：mirrors[0] 每項 8 位元組資源 ID，
    // mirrors[1] 每項 16 位元組緩衝位址＋長度；dirty 標記經 CPU handle 寫入過的項目，下次綁定前更新。
    struct Heap {
        std::uint32_t type{},count{};bool visible{};std::uint64_t cpu{},gpu{};std::vector<Descriptor> descriptors;
        std::array<std::shared_ptr<GraphicsResource>,2> mirrors;std::array<std::vector<std::uint8_t>,2> shadow;
        // dirty_chunks：每 64 項一個位元的待更新區塊點陣，寫入方以原子 OR 設定（worker 上的描述元建立／複製不取鎖）；
        // 鏡像更新（重播，全停下）依區塊順序取出待更新項目，等同排序後的清單。
        // dirty_summary：每 64 個區塊一個位元的摘要（寫入方在區塊位元之後設定）。重播每次綁定描述元表都可能更新鏡像，
        // 原本只要 any_dirty 就掃描全部區塊（百萬項的堆積約 1.6 萬個字組；worker 錄製下一幀時幾乎一直有新描述元）。
        std::vector<std::shared_ptr<void>> keep;std::vector<std::uint8_t> dirty;std::vector<std::uint64_t> dirty_chunks,dirty_summary;bool any_dirty{};
        // 編碼執行緒模式（D3D12DrawPool）：鏡像項目由編碼執行緒計算並直接寫入，view 保留物件放在共享向量（排入的工作一併持有）。
        std::shared_ptr<std::vector<std::shared_ptr<void>>> posted_keep;
    };
    struct Range {std::uint32_t type{},count{},base{},space{},offset{};};
    // visibility：D3D12_SHADER_VISIBILITY（0＝全部階段、1＝VS、5＝PS…）。同一暫存器可由不同可見度的根參數各自提供（例如 VS 與 PS 各有 b6）。
    struct Parameter {std::uint32_t type{},reg{},space{},values{},visibility{};std::vector<Range> ranges;};
    struct StaticSampler {std::uint32_t reg{},visibility{};std::shared_ptr<GraphicsSampler> sampler;};
    struct RootSignature {std::vector<Parameter> parameters;std::vector<StaticSampler> samplers;};
    struct Pipeline {
        bool compute{};std::uint64_t root{};std::shared_ptr<GraphicsShader> shader,pixel;
        std::vector<GraphicsElement> layout;std::vector<std::uint8_t> blend,depth,raster;std::uint32_t sample_mask{UINT32_MAX};
        // 建立時配發、不重用的序號（客體物件位址會被重用）：繪製以 GraphicsDraw::pipeline_serial 傳給後端快取管線。
        std::uint64_t serial{};
    };
    // views：錄製時即解析的 RTV／DSV（D3D12 於呼叫當下讀取 CPU 描述元）。
    // 命令參數：多數命令不超過 8 個，內建儲存免堆積配置（錄製在客體執行緒上，每幀數萬個命令）；超過時改用堆積。
    class Values {
        std::array<std::uint64_t,8> inline_{};std::vector<std::uint64_t> heap_;std::uint32_t size_{};
    public:
        Values()=default;
        Values(std::initializer_list<std::uint64_t> list){for(const auto value:list)push_back(value);}
        Values& operator=(std::initializer_list<std::uint64_t> list){clear();for(const auto value:list)push_back(value);return *this;}
        void reserve(std::size_t count){if(count>inline_.size())heap_.reserve(count);}
        void push_back(std::uint64_t value){
            if(heap_.empty() && size_<inline_.size()){inline_[size_++]=value;return;}
            if(heap_.empty())heap_.assign(inline_.begin(),inline_.begin()+size_);
            heap_.push_back(value);++size_;
        }
        std::size_t size()const noexcept{return size_;}
        bool empty()const noexcept{return !size_;}
        const std::uint64_t* data()const noexcept{return heap_.empty()?inline_.data():heap_.data();}
        std::uint64_t operator[](std::size_t index)const noexcept{return data()[index];}
        const std::uint64_t* begin()const noexcept{return data();}
        const std::uint64_t* end()const noexcept{return data()+size_;}
        void clear()noexcept{size_=0;heap_.clear();}
    };
    // op 指向靜態字串（服務描述表的方法名稱或字面常數），錄製時不配置。
    struct Command {std::string_view op;Values values;std::vector<std::uint8_t> data;std::vector<GraphicsView> views;};
    // 提交快照：第一次 ExecuteCommandLists 把錄製內容移入共用唯讀快照（再次提交同一清單共用它），Reset 時捨棄。
    // 提交執行緒只讀快照，客體在 ExecuteCommandLists 返回後即可重設並重新錄製同一清單。
    // readback_targets：錄製時記下的複製目的中屬於 CPU 讀回資源者（READBACK 等），提交排入時標上序號。
    struct Program {std::uint32_t type{};std::vector<Command> commands;std::vector<D3D12Replay> callbacks;std::vector<std::uint64_t> readback_targets;};
    struct List {std::uint32_t type{};std::uint64_t allocator{};bool open{true},invalid_recording{};std::vector<Command> commands;std::vector<D3D12Replay> callbacks;
        std::vector<std::uint64_t> readback_targets;std::shared_ptr<const Program> submitted;};
    struct IndirectArgument {std::uint32_t type{},index{},offset{},count{},size{};};
    struct CommandSignature {std::uint32_t stride{},operation{};std::uint64_t root{};std::vector<IndirectArgument> arguments;};
    std::map<std::uint64_t,CommandSignature> signatures;
    struct Fence {std::uint64_t value{};std::vector<std::pair<std::uint64_t,std::uint64_t>> events;};
    struct Device {std::shared_ptr<NativeOpenGlContext> native;std::uint32_t level{0xc000};std::shared_ptr<D3D12DrawPool> pool;};
    // external：OpenExistingHeapFromAddress 直接使用應用程式的 VirtualAlloc 記憶體，釋放堆積時不歸還。
    // host／host_bytes：零複製 CPU 可見堆積的宿主記憶體與客體映射範圍；placed 緩衝以其中的對應範圍建立 GPU 別名。
    struct MemoryHeap {std::uint64_t size{},cpu{};std::uint32_t type{},cpu_page{};std::vector<std::uint8_t> desc;bool external{};std::shared_ptr<void> host;std::span<std::uint8_t> host_bytes;};
    // 查詢堆積：TIMESTAMP 存 GPU 時戳；OCCLUSION 每個索引一個 visibility 緩衝，期間每次繪製各佔一格。
    struct QueryHeap {
        std::uint32_t type{},count{};std::vector<std::uint64_t> timestamps;
        // TIMESTAMP 以非同步取樣（命令緩衝完成時填值），不在重播中等 GPU；值就緒後同步寫回 timestamps。
        std::vector<std::shared_ptr<const GraphicsTimestampFuture>> futures;
        struct Occlusion {std::shared_ptr<GraphicsResource> buffer;std::uint32_t slots{};};std::vector<Occlusion> occlusion;
    };
    // 翻轉模型交換鏈：背景緩衝為裝置上的 D3D12 資源，呈現由獨立 Metal context 接手。
    struct SwapChain {
        std::uint64_t queue{},device{},window{},factory{};std::vector<std::uint64_t> buffers;std::shared_ptr<NativeOpenGlContext> native;
        std::uint32_t width{},height{},format{},flags{},index{},interval{};std::uint64_t frames{};std::vector<std::uint8_t> desc1;
    };
    // 物件、資源、命令清單與管線表每次服務呼叫或每次繪製都會查詢（數萬筆時 std::map 樹狀查找以快取未命中為主）：改雜湊表。
    // 沒有依序走訪；元素參照在重新雜湊後仍有效（重播的資源指標快取依此），只在建立／釋放時增刪。
    std::unordered_map<std::uint64_t,Object> objects=reserved<Object>(1U<<16);
    std::map<std::uint64_t,Device> devices;std::unordered_map<std::uint64_t,Resource> resources=reserved<Resource>(1U<<15);std::map<std::uint64_t,Heap> heaps;
    std::map<std::uint64_t,RootSignature> roots;std::unordered_map<std::uint64_t,Pipeline> pipelines=reserved<Pipeline>(1U<<13);std::unordered_map<std::uint64_t,List> lists=reserved<List>(1U<<10);
    std::map<std::uint64_t,Fence> fences;std::map<std::uint64_t,std::vector<std::uint8_t>> blobs;std::map<std::uint64_t,std::uint32_t> queues;std::map<std::uint64_t,SwapChain> swaps;
    std::map<std::uint64_t,MemoryHeap> memory_heaps;std::map<std::uint64_t,QueryHeap> query_heaps;
    // 非同步提交：ExecuteCommandLists 不等 GPU，記錄各佇列最近一次提交的完成旗標與待讀回資源（READBACK 堆積）。
    // Queue.Signal 遇到未完成的提交時登記待完成訊號，由 poll_d3d12（客體等待輪詢、GetCompletedValue、
    // SetEventOnCompletion）在 GPU 完成後依序讀回、設定 fence 值並觸發事件。
    // 寫入 CPU 端目的地（READBACK 堆積）的時間戳：重播時先寫暫定值，佇列下一次 Signal 生效前補上 GPU 實際值。
    struct TimestampWrite {std::uint64_t address{};std::vector<std::shared_ptr<const GraphicsTimestampFuture>> futures;};
    struct QueueState {std::shared_ptr<const std::atomic<bool>> done;std::vector<std::uint64_t> readbacks;std::vector<TimestampWrite> timestamp_writes;};
    std::map<std::uint64_t,QueueState> queue_states;
    // 管線庫：名稱對應已儲存的 PSO（持有參照）；由序列化資料還原的名稱沒有現存 PSO（值為 0），載入時依呼叫端描述重新建立。
    std::map<std::uint64_t,std::map<std::u16string,std::uint64_t>> libraries;
    std::map<Kind,std::uint64_t> tables;
    std::uint64_t next_address{UINT64_C(0x0000100000000000)},next_descriptor{UINT64_C(0x0000700000000000)};
    // 緩衝 GPU 位址 → 資源（位址單調遞增、不重用）；資源釋放後的失效項目於查詢時清除。
    std::map<std::uint64_t,std::uint64_t> addresses;
    // 位址查找快取：每個緩衝從 64 KiB 對齊的唯一位址開始、互不重疊也不重用，同一個 64 KiB 區塊只屬於一個資源。
    // 以區塊編號直接對應最近解析出的資源（含節點指標，命中時不再查雜湊表），結果與 addresses 的樹狀查找相同
    // （電馭判客 Benchmark 重播約 15% 時間花在 addresses 與 resources 的查找）。資源從 resources 刪除前以
    // forget_address_hits 清除指向它的項目，unordered_map 的節點指標在刪除前不失效。只在持圖形鎖時讀寫；
    // granule 存區塊編號＋1，0 為空。
    struct AddressHit {std::uint64_t granule{},object{};const Resource* resource{};};
    std::array<AddressHit,4096> address_hits{};
    void forget_address_hits(const Resource& resource){
        if(!resource.address)return;
        const auto first=resource.address>>16,last=(resource.address+std::max<std::uint64_t>(resource.width,1)-1)>>16;
        for(auto granule=first;granule<=last && granule-first<address_hits.size();++granule)
            if(auto& hit=address_hits[granule&(address_hits.size()-1)];hit.resource==&resource)hit={};
    }
    // 描述元位址 → 堆積：CPU 與 GPU handle 範圍取自同一個遞增、不重用的位址空間，兩者的基底各登錄一筆；
    // 堆積釋放後的失效項目於查詢時清除。
    std::map<std::uint64_t,std::uint64_t> descriptor_ranges;
    std::set<std::string> reported;
    // worker 上的 D3D12 服務產生的程序事件，下一次協調者端 D3D12 呼叫時併入 module_events。
    // notes_mutex 保護 reported 與 worker_notes：命令清單錄製在 worker 上不持圖形鎖，可能同時回報未支援方法。
    std::vector<std::string> worker_notes;std::mutex notes_mutex;
    // 描述元項目的分段鎖：worker 上的建立／複製與錄製讀取不持圖形鎖，應用程式可能把相同內容並行寫入同一槽，
    // 描述元含共享指標，寫入在鎖內交換、讀取在鎖內複製（重播在全停下直接存取）。
    // 臨界區段只有一次描述元複製／交換：用自旋鎖（std::mutex 競爭時進核心，實測錄製讀 RTV 平均 13 µs）。
    struct alignas(64) DescriptorSpin {
        std::atomic<bool> held{false};
        void lock() noexcept {while(held.exchange(true,std::memory_order_acquire))while(held.load(std::memory_order_relaxed)){
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }}
        void unlock() noexcept {held.store(false,std::memory_order_release);}
    };
    std::array<DescriptorSpin,256> descriptor_stripes;
    // 寫入完成後（已離開分段鎖）標記鏡像待更新：項目位元組、區塊位元、any_dirty 依序以原子操作設定；已標記者直接返回。
    // 重播在全停下讀取（ExecuteCommandLists 與 worker 並行的兩次嘗試都讓遊戲以 exit_code=3 中止，已撤回）。
    static void mark_dirty(Heap& heap,std::uint64_t index){
        if(heap.dirty.empty() || std::atomic_ref<std::uint8_t>(heap.dirty[index]).load(std::memory_order_seq_cst))return;
        std::atomic_ref<std::uint8_t>(heap.dirty[index]).store(1,std::memory_order_seq_cst);
        std::atomic_ref<std::uint64_t>(heap.dirty_chunks[index>>6]).fetch_or(std::uint64_t{1}<<(index&63U),std::memory_order_seq_cst);
        std::atomic_ref<std::uint64_t>(heap.dirty_summary[index>>12]).fetch_or(std::uint64_t{1}<<((index>>6)&63U),std::memory_order_seq_cst);
        std::atomic_ref<bool>(heap.any_dirty).store(true,std::memory_order_seq_cst);
    }
    DescriptorSpin& descriptor_lock(const Heap* heap,std::uint64_t index){return descriptor_stripes[((reinterpret_cast<std::uintptr_t>(heap)>>4)^index)&255U];}
    // 資源的 gpu／gpu_writes 共享指標：提交執行緒（持圖形鎖）與 worker 上不取圖形鎖的 Map、RTV／DSV 建立會同時存取，一律在此分段鎖內讀寫。
    DescriptorSpin& resource_lock(std::uint64_t object){return descriptor_stripes[((object>>4)^(object>>12))&255U];}
};
// D3D12 提交執行緒（X64ARM_D3D12_ASYNC_SUBMIT=0 關閉）：與實體驅動的提交執行緒相同，ExecuteCommandLists 只把命令清單快照排入
// 佇列就返回，重播（轉成 Metal 命令）在這條宿主執行緒持圖形鎖依序進行。客體執行緒不等重播，平行模式也不必為重播全停所有 worker
// （電馭判客 Benchmark 原本約三分之一時間全體停住等協調者重播）。Queue.Signal 同樣排入，在前面的重播之後掛上完成旗標；
// Present 等需要先前重播結果的方法先等佇列清空。工作依提交順序單執行緒處理，跨佇列順序與原本的同步重播相同。
struct D3D12Submitter {
    struct Job {std::array<std::uint64_t,4> arguments{};const WindowsServiceDescriptor* method{};std::vector<std::shared_ptr<const D3D12State::Program>> programs;};
    std::mutex mutex;std::condition_variable wake,idle;std::deque<Job> jobs;std::uint64_t queued{},done{};bool stopping{};
    // 已排入、尚未完成的 Present（在 mutex 內增減）：超過 DXGI 預設的最大幀延遲 3 時，客體 Present 先等。
    unsigned presents{};
    std::shared_ptr<SharedWake> notify;pthread_t thread{};bool started{};
    // 診斷（X64ARM_TRACE_D3D12_SUBMIT）：提交執行緒處理工作與等圖形鎖的時間、各類工作數，以及客體端等在途 Present、
    // 等佇列上限與等佇列清空的時間（奈秒）；提交執行緒每 5 秒於 stderr 列出區間差。
    struct Stats {std::atomic<std::uint64_t> busy{},lock{},executes{},lists{},signals{},presents{},present_wait{},enqueue_wait{},drain_wait{},
        draws{},table_entries{},upload_steps{},uav_clears{},uav_clear_bytes{},texture_copies{};
        // 前端每次繪製的分段時間（奈秒累計）：VS 綁定、PS 綁定、其餘設定、狀態池複製（含等池格）、排入編碼執行緒；
        // replay 為整批命令重播（不含等提交工作）。
        std::atomic<std::uint64_t> draw_vs{},draw_ps{},draw_setup{},draw_fill{},draw_post{},draw_fill_wait{},replay{};} stats;
    static bool tracing(){static const bool on=std::getenv("X64ARM_TRACE_D3D12_SUBMIT")!=nullptr;return on;}
    static std::uint64_t now_ns(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    ~D3D12Submitter(){{const std::lock_guard lock(mutex);stopping=true;}wake.notify_all();if(started)pthread_join(thread,nullptr);}
};
namespace {
using Kind=D3D12State::Kind;
constexpr std::uint32_t ok=0,false_ok=1,not_implemented=0x80004001U,no_interface=0x80004002U,pointer_error=0x80004003U,fail=0x80004005U,
    invalid=0x80070057U,no_memory=0x8007000eU,unsupported=0x887a0004U,not_found=0x887a0002U;
constexpr std::uint64_t descriptor_size=32;
// 管線庫序列化格式：識別字串＋格式版本＋名稱數，之後每筆為 UTF-16 長度與內容。識別不符即視為其他驅動版本產生。
constexpr std::array<std::uint8_t,16> library_magic{'X','6','4','A','R','M','-','P','S','O','L','I','B',0,0,1};
constexpr std::uint32_t driver_version_mismatch=0x887e0002U;
struct KindInfo {Kind kind;std::string_view prefix;std::string_view top;std::vector<std::string_view> chain;};
const std::vector<KindInfo>& kinds(){
    static const std::vector<KindInfo> list{
        {Kind::Device,"Device.","ID3D12Device14",{"ID3D12Object","ID3D12Device","ID3D12Device1","ID3D12Device2","ID3D12Device3","ID3D12Device4","ID3D12Device5","ID3D12Device6","ID3D12Device7","ID3D12Device8","ID3D12Device9","ID3D12Device10","ID3D12Device11","ID3D12Device12","ID3D12Device13","ID3D12Device14"}},
        {Kind::Queue,"Queue.","ID3D12CommandQueue",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12CommandQueue"}},
        {Kind::Allocator,"Allocator.","ID3D12CommandAllocator",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12CommandAllocator"}},
        {Kind::List,"List.","ID3D12GraphicsCommandList10",{"ID3D12Object","ID3D12DeviceChild","ID3D12CommandList","ID3D12GraphicsCommandList","ID3D12GraphicsCommandList1","ID3D12GraphicsCommandList2","ID3D12GraphicsCommandList3","ID3D12GraphicsCommandList4","ID3D12GraphicsCommandList5","ID3D12GraphicsCommandList6","ID3D12GraphicsCommandList7","ID3D12GraphicsCommandList8","ID3D12GraphicsCommandList9","ID3D12GraphicsCommandList10"}},
        {Kind::Fence,"Fence.","ID3D12Fence1",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12Fence","ID3D12Fence1"}},
        {Kind::Resource,"Resource.","ID3D12Resource2",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12Resource","ID3D12Resource1","ID3D12Resource2"}},
        {Kind::DescriptorHeap,"DescriptorHeap.","ID3D12DescriptorHeap",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12DescriptorHeap"}},
        {Kind::RootSignature,"RootSignature.","ID3D12RootSignature",{"ID3D12Object","ID3D12DeviceChild","ID3D12RootSignature"}},
        {Kind::Pipeline,"Pipeline.","ID3D12PipelineState",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12PipelineState"}},
        {Kind::CommandSignature,"CommandSignature.","ID3D12CommandSignature",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12CommandSignature"}},
        {Kind::QueryHeap,"QueryHeap.","ID3D12QueryHeap",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12QueryHeap"}},
        {Kind::Heap,"Heap.","ID3D12Heap1",{"ID3D12Object","ID3D12DeviceChild","ID3D12Pageable","ID3D12Heap","ID3D12Heap1"}},
        {Kind::PipelineLibrary,"PipelineLibrary.","ID3D12PipelineLibrary1",{"ID3D12Object","ID3D12DeviceChild","ID3D12PipelineLibrary","ID3D12PipelineLibrary1"}},
    };
    return list;
}
const KindInfo* info_of(Kind kind){for(const auto& item:kinds())if(item.kind==kind)return &item;return nullptr;}
// IDXGISwapChain4 完整 vtable 順序與 Win32 參數數量（不含 This）。
constexpr std::pair<std::string_view,unsigned> swap_methods[]{
    {"QueryInterface",2},{"AddRef",0},{"Release",0},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"GetPrivateData",3},{"GetParent",2},{"GetDevice",2},
    {"Present",2},{"GetBuffer",3},{"SetFullscreenState",2},{"GetFullscreenState",2},{"GetDesc",1},{"ResizeBuffers",5},{"ResizeTarget",1},{"GetContainingOutput",1},
    {"GetFrameStatistics",1},{"GetLastPresentCount",1},{"GetDesc1",1},{"GetFullscreenDesc",1},{"GetHwnd",1},{"GetCoreWindow",2},{"Present1",3},{"IsTemporaryMonoSupported",0},
    {"GetRestrictToOutput",1},{"SetBackgroundColor",1},{"GetBackgroundColor",1},{"SetRotation",1},{"GetRotation",1},{"SetSourceSize",2},{"GetSourceSize",2},
    {"SetMaximumFrameLatency",1},{"GetMaximumFrameLatency",1},{"GetFrameLatencyWaitableObject",0},{"SetMatrixTransform",1},{"GetMatrixTransform",1},
    {"GetCurrentBackBufferIndex",0},{"CheckColorSpaceSupport",2},{"SetColorSpace1",1},{"ResizeBuffers1",7},{"SetHDRMetaData",3},
};
// IDXGIObject、IDXGIDeviceSubObject、IDXGISwapChain–IDXGISwapChain4。
constexpr std::array<std::array<std::uint8_t,16>,7> swap_iids{{
    {0xb8,0x2f,0xc2,0xae,0xf3,0x76,0x39,0x46,0x9b,0xe0,0x28,0xeb,0x43,0xa6,0x7a,0x2e},{0x79,0x03,0x3e,0x3d,0xde,0xf9,0x58,0x4d,0xbb,0x6c,0x18,0xd6,0x29,0x92,0xf1,0xa6},
    {0xa0,0x36,0x0d,0x31,0xe7,0xd2,0x0a,0x4c,0xaa,0x04,0x6a,0x9d,0x23,0xb8,0x88,0x6a},{0xf7,0x45,0x0a,0x79,0x42,0x0d,0x76,0x48,0x98,0x3a,0x0a,0x55,0xcf,0xe6,0xf4,0xaa},
    {0xc4,0x2a,0xbe,0xa8,0xf8,0x19,0x46,0x49,0xb3,0x31,0x79,0x59,0x9f,0xb9,0x8d,0xe7},{0xdb,0x9b,0xd9,0x94,0xf8,0xf1,0xb0,0x4a,0xb2,0x36,0x7d,0xa0,0x17,0x0e,0xda,0xb1},
    {0x5a,0x5d,0x58,0x3d,0x4a,0xbd,0x9e,0x48,0xb1,0xf4,0x3d,0xbc,0xb6,0x45,0x2f,0xfb}}}; // IDXGISwapChain4 {3D585D5A-BD4A-489E-B1F4-3DBCB6452FFB}
constexpr std::array<std::uint8_t,16> iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::array<std::uint8_t,16> iid_blob{0x08,0xfb,0xa5,0x8b,0x95,0x51,0xe2,0x40,0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02}; // ID3D10Blob。
std::array<std::uint8_t,16> read_guid(const GuestMemory& memory,std::uint64_t at){std::array<std::uint8_t,16> value{};memory.read(GuestAddress{at},value);return value;}
}
std::span<const WindowsServiceDescriptor> d3d12_export_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"d3d12.dll","D3D12CreateDevice",4},{"d3d12.dll","#101",4},{"d3d12.dll","D3D12GetDebugInterface",2},{"d3d12.dll","D3D12SerializeRootSignature",4},
        {"d3d12.dll","D3D12SerializeVersionedRootSignature",3},{"d3d12.dll","D3D12CreateRootSignatureDeserializer",4},
        {"d3d12.dll","D3D12CreateVersionedRootSignatureDeserializer",4},{"d3d12.dll","D3D12EnableExperimentalFeatures",4},{"d3d12.dll","D3D12GetInterface",3},
    };
    return table;
}
std::span<const WindowsServiceDescriptor> d3d12_object_service_descriptors(){
    static const auto descriptors=[]{
        static std::vector<std::string> names;std::vector<std::pair<std::size_t,unsigned>> rows;
        for(const auto& item:kinds()){const auto* abi=d3d12abi::find_interface(item.top);for(const auto& method:abi->methods){rows.push_back({names.size(),method.arguments+1});names.push_back(std::string(item.prefix)+std::string(method.name));}}
        for(const auto& [name,count]:swap_methods){rows.push_back({names.size(),count+1});names.push_back("Swap."+std::string(name));}
        rows.push_back({names.size(),4});names.push_back("DXGI.CreateSwapChain");rows.push_back({names.size(),7});names.push_back("DXGI.CreateSwapChainForHwnd");
        for(const auto* name:{"Blob.QueryInterface","Blob.AddRef","Blob.Release","Blob.GetBufferPointer","Blob.GetBufferSize"})names.push_back(name);
        std::vector<WindowsServiceDescriptor> out;
        for(const auto& [index,count]:rows)out.push_back(WindowsServiceDescriptor{"internal-d3d12",names[index],count});
        const auto base=rows.size();
        for(unsigned i=0;i<5;++i)out.push_back(WindowsServiceDescriptor{"internal-d3d12",names[base+i],i==0?3U:1U});
        return out;
    }();
    return descriptors;
}
bool WindowsServices::d3d12_queue(std::uint64_t object) const{return d3d12_ && d3d12_->queues.contains(object);}
bool WindowsServices::d3d12_record(std::uint64_t list,D3D12Replay replay){
    if(!d3d12_)return false;const auto found=d3d12_->lists.find(list);if(found==d3d12_->lists.end() || !found->second.open)return false;
    found->second.commands.push_back({"Callback",{found->second.callbacks.size()},{},{}});found->second.callbacks.push_back(std::move(replay));return true;
}
std::uint64_t WindowsServices::d3d12_device_of(std::uint64_t object) const{
    if(!d3d12_)return 0;if(d3d12_->devices.contains(object))return object;const auto found=d3d12_->objects.find(object);return found==d3d12_->objects.end()?0:found->second.device;
}
bool WindowsServices::d3d12_resource_width(std::uint64_t resource,std::uint64_t& width) const{
    if(!d3d12_)return false;const auto found=d3d12_->resources.find(resource);if(found==d3d12_->resources.end())return false;width=found->second.width;return true;
}
namespace {
// CPU 可見堆積（UPLOAD／READBACK／CPU 可見 CUSTOM）的零複製後備：匿名宿主分頁（初始為零、宿主分頁對齊），
// 同時映入客體位址空間並作為 Metal 緩衝內容，與 UMA 上實體 D3D12 的系統記憶體堆積相同。
// X64ARM_D3D12_ZERO_COPY=0 回到客體堆積後備加每次重播上傳／讀回。
bool d3d12_zero_copy_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_D3D12_ZERO_COPY");return !text || std::string_view(text)!="0";}();return value;}
std::shared_ptr<void> d3d12_host_pages(std::size_t size){
    void* data=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(data==MAP_FAILED)return nullptr;
    return std::shared_ptr<void>(data,[size](void* pages){munmap(pages,size);});
}
}
// fence 達到 value：設定完成值並觸發已達門檻的事件。
void WindowsServices::d3d12_signal_fence(std::uint64_t object,std::uint64_t value){
    if(!d3d12_)return;const auto found=d3d12_->fences.find(object);if(found==d3d12_->fences.end())return;auto& fence=found->second;
    std::atomic_ref(fence.value).store(value,std::memory_order_release); // worker 上的 GetCompletedValue 不取鎖讀取。
    std::erase_if(fence.events,[&](const auto& item){if(item.first>fence.value)return false;if(const auto event=runtime_.wait_objects.find(item.second);event!=runtime_.wait_objects.end())event->second.set_signal(true);return true;});
}
// 共用 GPU 完成通知：登記後立即檢查一次（已完成者直接套用）。
// 通知佇列與套用的回呼（fence、讀回、橋接層表格）以圖形鎖保護：worker 上的 D3D11／D3D12 服務會同時存取這些狀態
// （D3D11 fence 方法也會輪詢）。持圖形鎖的 worker 必已持共用映射鎖，協調者在此等待不會與映射鎖形成循環。
void WindowsServices::queue_gpu_signal(GuestMemory& memory,std::shared_ptr<const std::atomic<bool>> done,std::uint64_t order,std::function<void(GuestMemory&)> apply,bool poll){
    const std::lock_guard graphics_lock(graphics_mutex());
    gpu_signals_.push_back({std::move(done),order,std::move(apply)});std::atomic_ref(gpu_signal_count_).store(gpu_signals_.size(),std::memory_order_relaxed);
    if(poll)poll_gpu_signals(memory);
}
void WindowsServices::poll_gpu_signals(GuestMemory& memory,bool try_only){
    std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);
    if(!try_only)lock_graphics(graphics_lock);else if(!graphics_lock.try_lock())return;
    if(gpu_signals_.empty())return;std::set<std::uint64_t> blocked;
    for(auto it=gpu_signals_.begin();it!=gpu_signals_.end();){
        if(blocked.contains(it->order) || (it->done && !it->done->load(std::memory_order_acquire))){blocked.insert(it->order);++it;continue;}
        auto apply=std::move(it->apply);it=gpu_signals_.erase(it);if(apply)apply(memory);
    }
    std::atomic_ref(gpu_signal_count_).store(gpu_signals_.size(),std::memory_order_relaxed);
}
// 有待完成的 GPU 通知時，客體等待最多 X64ARM_GPU_SIGNAL_POLL_US（預設 250 µs）就重新檢查。worker 運算中協調者不監看外部喚醒，
// GPU 完成只能靠這個期限發現；每次期限都要在獨占映射鎖下解析等待（worker 服務同時等待），間隔越短協調者負擔越重。
std::uint64_t WindowsServices::gpu_signal_wakeup_ns() const {
    static const std::uint64_t interval=[] {const auto* value=std::getenv("X64ARM_GPU_SIGNAL_POLL_US");
        return value?std::max<std::uint64_t>(50,std::strtoull(value,nullptr,10))*1000:250000;}();
    return std::atomic_ref(const_cast<std::size_t&>(gpu_signal_count_)).load(std::memory_order_relaxed)?interval:0;
}
namespace {
bool d3d12_async_submit_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_D3D12_ASYNC_SUBMIT");return !text || std::string_view(text)!="0";}();return value;}
const WindowsServiceDescriptor d3d12_execute_method{"internal-d3d12","Queue.ExecuteCommandLists",4};
const WindowsServiceDescriptor d3d12_signal_method{"internal-d3d12","Queue.Signal",3};
const WindowsServiceDescriptor d3d12_present_method{"internal-d3d12","Swap.Present",3};
// X64ARM_D3D12_BIND_ALL=1：每次繪製解析根簽章描述元表的全部項目（舊行為，對照用）。
bool d3d12_bind_all(){static const bool value=[] {const auto* text=std::getenv("X64ARM_D3D12_BIND_ALL");return text && std::string_view(text)=="1";}();return value;}
// D3D12 上傳與描述元鏡像直接寫入共享記憶體（X64ARM_D3D12_DIRECT_UPLOAD=0 恢復排在命令流中的 staging 複製）。
bool d3d12_direct_uploads(){static const bool value=[] {const auto* text=std::getenv("X64ARM_D3D12_DIRECT_UPLOAD");return !text || std::string_view(text)!="0";}();return value;}
// 需要先前提交已重播完成的方法：呈現與交換鏈緩衝變動、裝置與佇列最後釋放。
bool d3d12_drains(std::string_view symbol){
    return symbol=="Swap.ResizeBuffers" || symbol=="Swap.ResizeBuffers1" ||
        symbol=="Swap.SetFullscreenState" || symbol=="Swap.Release" || symbol=="Device.Release" || symbol=="Queue.Release";
}
// bounded：在途上限，客體提交遠快於重播時讓提交端等候，避免快照無限累積（Present 每幀也會等佇列清空）。
// 只用於呼叫端確定不持圖形鎖的 ExecuteCommandLists（worker 免鎖路徑、已放開鎖的完整路徑）；Signal 可能來自持圖形鎖的
// 協調者本地路徑，等候會與需要圖形鎖的提交執行緒互鎖，因此不設上限（工作本身很小）。
std::uint64_t d3d12_enqueue(D3D12Submitter& submitter,D3D12Submitter::Job job,bool bounded){
    std::uint64_t ticket{};
    {
        std::unique_lock lock(submitter.mutex);
        if(bounded && submitter.queued-submitter.done>=256){
            const auto start=D3D12Submitter::tracing()?D3D12Submitter::now_ns():0;
            submitter.idle.wait(lock,[&]{return submitter.queued-submitter.done<256;});
            if(start)submitter.stats.enqueue_wait.fetch_add(D3D12Submitter::now_ns()-start,std::memory_order_relaxed);
        }
        submitter.jobs.push_back(std::move(job));ticket=++submitter.queued;
    }
    submitter.wake.notify_one();return ticket;
}
// 等提交執行緒完成到指定排入序號（呼叫端不得持圖形鎖：重播需要它）。
void d3d12_wait_ticket(D3D12Submitter& submitter,std::uint64_t ticket){
    std::unique_lock lock(submitter.mutex);if(submitter.done>=ticket)return;
    const auto start=D3D12Submitter::tracing()?D3D12Submitter::now_ns():0;
    submitter.idle.wait(lock,[&]{return submitter.done>=ticket;});
    if(start)submitter.stats.drain_wait.fetch_add(D3D12Submitter::now_ns()-start,std::memory_order_relaxed);
}
}
void WindowsServices::d3d12_start_submitter(GuestMemory& memory){
    if(d3d12_submitter_ || !d3d12_async_submit_enabled())return;
    auto submitter=std::make_shared<D3D12Submitter>();submitter->notify=mapping_wake();
    struct Start {WindowsServices* self;GuestMemory* memory;D3D12Submitter* submitter;};
    auto start=std::make_unique<Start>(Start{this,&memory,submitter.get()});
    // 重播含大型區域狀態（繪製狀態、根參數、資源快取）並深入 Metal：給 16 MiB 堆疊。
    pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,std::size_t{16}<<20);
    const auto created=pthread_create(&submitter->thread,&attributes,[](void* opaque)->void*{
        const std::unique_ptr<Start> start(static_cast<Start*>(opaque));start->self->d3d12_submit_loop(*start->memory,*start->submitter);return nullptr;},start.get());
    pthread_attr_destroy(&attributes);
    if(created!=0)return;
    (void)start.release();submitter->started=true;d3d12_submitter_=std::move(submitter);
}
void WindowsServices::d3d12_submit_loop(GuestMemory& memory,D3D12Submitter& submitter){
    pthread_setname_np("x64arm.d3d12.submit");pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    d3d12_worker_context()=true; // 程序事件先暫存於橋接層，下一次協調者端 D3D12 呼叫時併入。
    GuestState idle{};
    auto& stats=submitter.stats;const bool trace=D3D12Submitter::tracing();
    std::array<std::uint64_t,9> reported{};auto report_at=trace?D3D12Submitter::now_ns():0;
    for(;;){
        D3D12Submitter::Job job;
        {
            std::unique_lock lock(submitter.mutex);
            submitter.wake.wait(lock,[&]{return submitter.stopping || !submitter.jobs.empty();});
            if(submitter.jobs.empty())return;
            job=std::move(submitter.jobs.front());submitter.jobs.pop_front();
        }
        const bool signal=job.method==&d3d12_signal_method,present=job.method==&d3d12_present_method;
        const auto started=trace?D3D12Submitter::now_ns():0;const auto waited=graphics_wait_ns();
        if(trace){
            (signal?stats.signals:present?stats.presents:stats.executes).fetch_add(1,std::memory_order_relaxed);
            stats.lists.fetch_add(job.programs.size(),std::memory_order_relaxed);
        }
        // 圖形鎖由 invoke_d3d12 入口取得（不在此外層持有），重播才能在命令清單之間讓給等待者。
        d3d12_submit_context()=&job;
        try{(void)invoke_d3d12(idle,memory,*job.method);}
        catch(const std::exception& error){
            if(d3d12_){const std::lock_guard lock(d3d12_->notes_mutex);d3d12_->worker_notes.push_back(std::string("d3d12_note=提交執行緒失敗：")+error.what());}
        }
        d3d12_submit_context()=nullptr;
        if(trace){
            const auto finished=D3D12Submitter::now_ns();
            stats.busy.fetch_add(finished-started,std::memory_order_relaxed);stats.lock.fetch_add(graphics_wait_ns()-waited,std::memory_order_relaxed);
            if(finished-report_at>=UINT64_C(5'000'000'000)){
                // GPU 忙碌率：後端以命令緩衝的 GPU 起訖時間累計（statistics 的 opengl_metal_gpu_busy_ns）。遊戲的 CPU 工作主要是
                // 餵飽 GPU，提交執行緒忙碌而 GPU 忙碌率低，表示瓶頸在 CPU 端產生命令的速度。
                // 編碼同步：前端等 Metal 編碼執行緒清空佇列的次數與時間（metal_command_sync_*），量前端被同步方法卡住的程度。
                std::uint64_t gpu_busy=0,sync_waits=0,sync_ns=0,encoders=0,coalesced=0,resource_waits=0,timed=0,encoder_busy=0,encoder_tasks=0,clears_folded=0,clears_encoded=0,gpu_waits=0,gpu_wait_ns=0,submissions=0,inflight_limit=0,inflight_peak=0;std::string breaks;std::array<std::uint64_t,6> draw_ns{};
                if(d3d12_){
                    const std::unique_lock<std::recursive_mutex> guard(graphics_mutex());
                    const auto field=[](const std::string& text,std::string_view name)->std::uint64_t{
                        const auto at=text.find(name);return at==std::string::npos?0:std::strtoull(text.c_str()+at+name.size(),nullptr,10);};
                    for(const auto& [id,device]:d3d12_->devices)if(device.native){
                        const auto text=device.native->statistics();gpu_busy+=field(text,"opengl_metal_gpu_busy_ns=");
                        sync_waits+=field(text,"metal_command_sync_waits=");sync_ns+=field(text,"metal_command_sync_wait_ns=");
                        encoders+=field(text,"opengl_metal_render_encoders=");coalesced+=field(text,"opengl_metal_coalesced_draws=");resource_waits+=field(text,"opengl_metal_resource_waits=");
                        if(const auto at=text.find("opengl_metal_encoder_breaks=");at!=std::string::npos)breaks+=text.substr(at+28,text.find(' ',at)-at-28);
                        timed+=field(text,"metal_draw_timed=");clears_folded+=field(text,"metal_clears_folded=");clears_encoded+=field(text,"metal_clears_encoded=");
                        gpu_waits+=field(text,"opengl_metal_waits=");gpu_wait_ns+=field(text,"opengl_metal_wait_ns=");submissions+=field(text,"opengl_metal_submissions=");
                        inflight_limit=std::max(inflight_limit,field(text,"opengl_metal_inflight_limit="));inflight_peak=std::max(inflight_peak,field(text,"opengl_metal_inflight_peak="));encoder_busy+=field(text,"metal_command_busy_ns=");encoder_tasks+=field(text,"metal_command_tasks=");
                        if(auto at=text.find("metal_draw_ns=");at!=std::string::npos){at+=14;for(auto& value:draw_ns){char* end=nullptr;value+=std::strtoull(text.c_str()+at,&end,10);at=static_cast<std::size_t>(end-text.c_str())+1;}}
                    }
                }
                static std::uint64_t gpu_reported=0,sync_waits_reported=0,sync_ns_reported=0,encoders_reported=0,coalesced_reported=0,resource_waits_reported=0;
                // render encoder 數與合併繪製數、等 GPU 用完資源的次數（區間差）；中斷原因為累計值。
                std::fprintf(stderr,"d3d12_submit_encode 清除累計：併入pass=%llu 獨立pass=%llu\n",static_cast<unsigned long long>(clears_folded),static_cast<unsigned long long>(clears_encoded));
                {
                    // 編碼執行緒等 GPU 完成（在途命令緩衝達上限或同步讀回）的次數與時間占比，以及提交的命令緩衝數。
                    static std::uint64_t waits_reported=0,wait_ns_reported=0,submissions_reported=0;
                    std::fprintf(stderr,"d3d12_submit_gpu 提交命令緩衝=%llu 等GPU=%llu次/%.1f%% 在途上限=%llu 峰值=%llu\n",static_cast<unsigned long long>(submissions-submissions_reported),
                        static_cast<unsigned long long>(gpu_waits-waits_reported),static_cast<double>(gpu_wait_ns-wait_ns_reported)*100.0/static_cast<double>(finished-report_at),
                        static_cast<unsigned long long>(inflight_limit),static_cast<unsigned long long>(inflight_peak));
                    waits_reported=gpu_waits;wait_ns_reported=gpu_wait_ns;submissions_reported=submissions;
                }
                std::fprintf(stderr,"d3d12_submit_encode 編碼器=%llu 合併繪製=%llu 資源等待=%llu 中斷累計=%s\n",static_cast<unsigned long long>(encoders-encoders_reported),
                    static_cast<unsigned long long>(coalesced-coalesced_reported),static_cast<unsigned long long>(resource_waits-resource_waits_reported),breaks.c_str());
                encoders_reported=encoders;coalesced_reported=coalesced;resource_waits_reported=resource_waits;
                {
                    // 編碼端每次繪製的分段時間（區間差）：準備、管線、取得 encoder、狀態、綁定、繪製呼叫。
                    static std::uint64_t timed_reported=0;static std::array<std::uint64_t,6> draw_ns_reported{};
                    const auto count=static_cast<double>(std::max<std::uint64_t>(1,timed-timed_reported));std::array<double,6> part{};double total=0;
                    for(std::size_t i=0;i<part.size();++i){part[i]=static_cast<double>(draw_ns[i]-draw_ns_reported[i])/count/1000.0;total+=part[i];}
                    // 編碼執行緒整體忙碌（執行排入工作的時間）與其中 graphics_draw 所占比例。
                    static std::uint64_t busy_reported=0,tasks_reported=0;const double span=static_cast<double>(finished-report_at);
                    const double busy=static_cast<double>(encoder_busy-busy_reported),draw_total=total*1000.0*count;
                    std::fprintf(stderr,"d3d12_submit_encode_draw 每次繪製µs：準備=%.2f 管線=%.2f encoder=%.2f 狀態=%.2f 綁定=%.2f 繪製=%.2f 合計=%.2f；編碼執行緒忙碌=%.1f%% 其中繪製=%.1f%% 工作數=%llu\n",
                        part[0],part[1],part[2],part[3],part[4],part[5],total,busy*100.0/span,busy>0?draw_total*100.0/busy:0.0,static_cast<unsigned long long>(encoder_tasks-tasks_reported));
                    timed_reported=timed;draw_ns_reported=draw_ns;busy_reported=encoder_busy;tasks_reported=encoder_tasks;
                }
                const auto gpu_delta=gpu_busy>=gpu_reported?gpu_busy-gpu_reported:0;gpu_reported=gpu_busy;
                const auto sync_waits_delta=sync_waits>=sync_waits_reported?sync_waits-sync_waits_reported:0;sync_waits_reported=sync_waits;
                const auto sync_ns_delta=sync_ns>=sync_ns_reported?sync_ns-sync_ns_reported:0;sync_ns_reported=sync_ns;
                {
                    const auto draws=static_cast<double>(std::max<std::uint64_t>(1,stats.draws.load(std::memory_order_relaxed)));
                    const auto us=[&](std::atomic<std::uint64_t>& value){return static_cast<double>(value.exchange(0))/draws/1000.0;};
                    const double vs=us(stats.draw_vs),ps=us(stats.draw_ps),setup=us(stats.draw_setup),fill=us(stats.draw_fill),post=us(stats.draw_post),wait=us(stats.draw_fill_wait);
                    std::fprintf(stderr,"d3d12_submit_draw 每次繪製µs：VS綁定=%.2f PS綁定=%.2f 其他=%.2f 池複製=%.2f 等池格=%.2f 排入=%.2f 不含等待合計=%.2f\n",vs,ps,setup,fill,wait,post,vs+ps+setup+fill+post);
                }
                std::fprintf(stderr,"d3d12_submit_work GPU忙碌=%.1f%% 編碼同步=%llu次/%.1f%% 繪製=%llu 描述元表逐項=%llu 上傳範圍掃描=%llu UAV清除=%llu次/%.1fMB 緩衝紋理複製=%llu\n",static_cast<double>(gpu_delta)*100.0/static_cast<double>(finished-report_at),
                    static_cast<unsigned long long>(sync_waits_delta),static_cast<double>(sync_ns_delta)*100.0/static_cast<double>(finished-report_at),
                    static_cast<unsigned long long>(stats.draws.exchange(0)),
                    static_cast<unsigned long long>(stats.table_entries.exchange(0)),static_cast<unsigned long long>(stats.upload_steps.exchange(0)),
                    static_cast<unsigned long long>(stats.uav_clears.exchange(0)),static_cast<double>(stats.uav_clear_bytes.exchange(0))/1e6,static_cast<unsigned long long>(stats.texture_copies.exchange(0)));
                const std::array<std::uint64_t,9> now{stats.busy.load(),stats.lock.load(),stats.executes.load(),stats.lists.load(),stats.signals.load(),
                    stats.presents.load(),stats.present_wait.load(),stats.enqueue_wait.load(),stats.drain_wait.load()};
                std::array<double,9> d{};for(std::size_t i=0;i<d.size();++i)d[i]=static_cast<double>(now[i]-reported[i]);
                const double span=static_cast<double>(finished-report_at);
                std::fprintf(stderr,"d3d12_submit 區間=%.1f 秒 忙碌=%.1f%% 其中等圖形鎖=%.1f%% 提交=%llu 清單=%llu Signal=%llu Present=%llu "
                    "客體等待：在途Present=%.0f ms 佇列上限=%.0f ms 清空=%.0f ms\n",span/1e9,d[0]*100.0/span,d[1]*100.0/span,
                    static_cast<unsigned long long>(now[2]-reported[2]),static_cast<unsigned long long>(now[3]-reported[3]),
                    static_cast<unsigned long long>(now[4]-reported[4]),static_cast<unsigned long long>(now[5]-reported[5]),d[6]/1e6,d[7]/1e6,d[8]/1e6);
                reported=now;report_at=finished;
            }
        }
        {const std::lock_guard lock(submitter.mutex);++submitter.done;if(present && submitter.presents)--submitter.presents;}
        submitter.idle.notify_all();
        // 新登記的完成通知：喚醒閒置等待中的協調者，讓等 fence 事件的客體不必等到下一個計時器。
        if(signal && submitter.notify)submitter.notify->notify();
    }
}
void WindowsServices::d3d12_drain(){
    if(!d3d12_submitter_)return;auto& submitter=*d3d12_submitter_;
    std::unique_lock lock(submitter.mutex);const auto target=submitter.queued;
    if(submitter.done>=target)return;
    const auto start=D3D12Submitter::tracing()?D3D12Submitter::now_ns():0;
    submitter.idle.wait(lock,[&]{return submitter.done>=target;});
    if(start)submitter.stats.drain_wait.fetch_add(D3D12Submitter::now_ns()-start,std::memory_order_relaxed);
}
// Benchmark 期間 Resource.AddRef／Release 每秒約上千次，原本每次全停。呼叫端持共用映射鎖＋圖形鎖；
// 物件表的增刪只在全停或獨占映射鎖下發生，這裡只改既有物件的計數。
bool WindowsServices::d3d12_worker_reference(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(x86_ || !d3d12_)return false;
    const WindowsCall arg(state,memory,service);const bool add=service.symbol.ends_with(".AddRef");
    const auto found=d3d12_->objects.find(arg(0));if(found==d3d12_->objects.end())return false;
    // worker 間以原子操作改計數，不取圖形鎖（重播在協調者上不全停時仍可進行）；物件表增刪只在全停或獨占映射鎖下。
    std::atomic_ref<unsigned> references(found->second.references);
    if(add){auto value=references.load(std::memory_order_relaxed);
        do{if(value==UINT32_MAX)return false;}while(!references.compare_exchange_weak(value,value+1,std::memory_order_acq_rel,std::memory_order_relaxed));
        arg.return_to(state,value+1);return true;}
    auto value=references.load(std::memory_order_relaxed);
    do{if(value<=1)return false;}while(!references.compare_exchange_weak(value,value-1,std::memory_order_acq_rel,std::memory_order_relaxed));
    arg.return_to(state,value-1);return true;
}
// Benchmark 期間 Resource.Map／Unmap 每秒各數百次原本全停。零複製資源（UPLOAD／READBACK）沒有待完成的 GPU 寫入時，
// Map 只把客體位址寫到輸出指標；等 GPU、讀回與輸出落在寫入時複製頁（共用映射鎖下不得私有化）時交回協調者，未改任何狀態。
bool WindowsServices::d3d12_worker_map(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(x86_ || !d3d12_)return false;
    const WindowsCall arg(state,memory,service);
    const auto found=d3d12_->resources.find(arg(0));if(found==d3d12_->resources.end())return false;
    if(service.symbol=="Resource.Unmap"){arg.return_to(state,0);return true;}
    auto& r=found->second;
    if(const auto out=arg(3);out && (memory.is_copy_on_write(GuestAddress{out}) || memory.is_copy_on_write(GuestAddress{out+7})))return false;
    {
        // 只清除已完成的寫入旗標；未完成時交回協調者路徑等 GPU。
        const std::lock_guard lock(d3d12_->resource_lock(arg(0)));
        if(!r.cpu || (r.gpu_writes && !r.gpu_writes->load(std::memory_order_acquire)) || (r.reads_back() && r.gpu && !r.host_shared))return false;
        if(r.submit_ticket && d3d12_submitter_){const std::lock_guard pending(d3d12_submitter_->mutex);if(d3d12_submitter_->done<r.submit_ticket)return false;}
        r.gpu_writes.reset();
    }
    if(const auto out=arg(3))memory.store(GuestAddress{out},64,r.cpu);
    arg.return_to(state,0);return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_d3d12(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // 提交執行緒重播（d3d12_submit_context）沒有客體呼叫框架：參數取自工作，不核對堆疊、不寫回返回值。
    auto* const submit_job=static_cast<D3D12Submitter::Job*>(d3d12_submit_context());
    std::optional<WindowsCall> call;if(!submit_job)call.emplace(state,memory,service);
    const auto arg=[&](unsigned index)->std::uint64_t{return call?(*call)(index):submit_job->arguments[index&3U];};
    // d3d12.lib 以序號 101 匯入 D3D12CreateDevice。
    const std::string_view symbol=service.symbol=="#101"?"D3D12CreateDevice":service.symbol;
    // 圖形鎖：提交執行緒在鎖內重播，改動橋接層表格或後端的呼叫都與它串行化；worker 上不取鎖的方法（d3d12_unlocked_call）除外。
    // 需要先前提交已重播的方法（Present 等）先在鎖外等提交佇列清空：它們只走完整路徑，呼叫端不持圖形鎖。
    if(!submit_job && !d3d12_worker_context() && d3d12_drains(symbol))d3d12_drain();
    if(!submit_job && symbol=="Resource.Map" && d3d12_submitter_ && d3d12_){
        std::uint64_t ticket{};
        if(const auto found=d3d12_->resources.find(arg(0));found!=d3d12_->resources.end()){const std::lock_guard lock(d3d12_->resource_lock(found->first));ticket=found->second.submit_ticket;}
        if(ticket)d3d12_wait_ticket(*d3d12_submitter_,ticket);
    }
    std::unique_lock<std::recursive_mutex> graphics_guard(graphics_mutex(),std::defer_lock);
    if(!d3d12_unlocked_call())lock_graphics(graphics_guard);
    // 診斷：X64ARM_TRACE_GRAPHICS_ERRORS 時列出回傳失敗 HRESULT 的 D3D12 方法（32 位元且最高位元為 1 的回傳值）。
    static const bool trace_graphics_errors=std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS")!=nullptr;
    // 診斷：X64ARM_TRACE_D3D12 時即時列出未支援方法、無效錄製、重播失敗與 ExecuteIndirect 參數（有數量上限）。
    static const bool trace_d3d12=std::getenv("X64ARM_TRACE_D3D12")!=nullptr;
    // 診斷：重播中需要等 GPU 完成的同步讀回次數（依命令），隨 X64ARM_TRACE_GRAPHICS_STATS 的 Present 統計輸出。
    static std::map<std::string,std::uint64_t> sync_reads;
    const auto count_sync=[&](const char* why){static const bool on=std::getenv("X64ARM_TRACE_GRAPHICS_STATS")!=nullptr;if(on)++sync_reads[why];};
    // 診斷：同一統計開關下累計 UPLOAD 堆積每次重播實際上傳的位元組（依綁定來源），鍵前綴 upload_bytes_。
    static const bool trace_stats_uploads=std::getenv("X64ARM_TRACE_GRAPHICS_STATS")!=nullptr;
    static std::atomic<unsigned> trace_lines{};
    // 被 ExecuteIndirect 讀取過的參數資源；診斷時記錄寫入它們的清除／複製／立即寫入命令（每種命令與資源限幾次）。
    static const auto watched_arguments=[]()->std::set<std::uint64_t>&{static std::set<std::uint64_t> watched;return watched;};
    static std::map<std::pair<std::string,std::uint64_t>,unsigned> watch_counts;
    const auto watch=[&](std::string_view op,std::uint64_t resource)->bool{
        if(!trace_d3d12 || !watched_arguments().contains(resource))return false;return ++watch_counts[{std::string(op),resource}]<=6;};
    const auto trace=[&](const char* format,auto... values){if(trace_d3d12 && trace_lines.fetch_add(1,std::memory_order_relaxed)<4000)std::fprintf(stderr,format,values...);};
    const auto finish=[&](std::uint64_t value){
        if(trace_graphics_errors && value<=UINT32_MAX && (value&0x80000000U))std::fprintf(stderr,"[d3d12] %.*s HRESULT=%08x\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned>(value));
        if(call)call->return_to(state,value);return std::optional<std::uint32_t>{};};
    if(x86_)return finish(service.family=="d3d12.dll"?unsupported:not_implemented);
    if(!d3d12_)d3d12_=std::make_shared<D3D12State>();
    auto& bridge=*d3d12_;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto load=[&](std::uint64_t at,unsigned bits){return memory.load(GuestAddress{at},static_cast<std::uint8_t>(bits));};
    const auto store=[&](std::uint64_t at,unsigned bits,std::uint64_t value){memory.store(GuestAddress{at},static_cast<std::uint8_t>(bits),value);};
    const auto bytes=[&](std::uint64_t at,std::size_t size){std::vector<std::uint8_t> out(size);if(size)memory.read(GuestAddress{at},out);return out;};
    const auto note=[&](std::string text){if(d3d12_worker_context()){const std::lock_guard lock(bridge.notes_mutex);bridge.worker_notes.push_back(std::move(text));}else runtime_.module_events.push_back(std::move(text));};
    if(!d3d12_worker_context()){const std::lock_guard lock(bridge.notes_mutex);for(auto& text:bridge.worker_notes)runtime_.module_events.push_back(std::move(text));bridge.worker_notes.clear();}
    const auto report=[&](std::string_view name){if([&]{const std::lock_guard lock(bridge.notes_mutex);return bridge.reported.insert(std::string(name)).second;}()){note("d3d12_note=尚未支援 "+std::string(name));trace("[d3d12] 尚未支援 %.*s\n",static_cast<int>(name.size()),name.data());}};
    const auto create=[&](Kind kind,std::uint64_t device)->std::uint64_t{
        auto& table=bridge.tables[kind];
        if(!table){
            if(kind==Kind::SwapChain){
                table=runtime_allocate(memory,{},std::size(swap_methods)*8);std::size_t i=0;
                for(const auto& [name,count]:swap_methods){(void)count;store(table+8*i++,64,resolve("internal-d3d12","Swap."+std::string(name)).value);}
            }else if(kind==Kind::Blob){
                table=runtime_allocate(memory,{},5*8);std::size_t i=0;
                for(const auto* name:{"Blob.QueryInterface","Blob.AddRef","Blob.Release","Blob.GetBufferPointer","Blob.GetBufferSize"})store(table+8*i++,64,resolve("internal-d3d12",name).value);
            }else{
                const auto* item=info_of(kind);const auto* abi=d3d12abi::find_interface(item->top);table=runtime_allocate(memory,{},abi->methods.size()*8);
                for(std::size_t i=0;i<abi->methods.size();++i)store(table+i*8,64,resolve("internal-d3d12",std::string(item->prefix)+std::string(abi->methods[i].name)).value);
            }
        }
        const auto pointer=allocate_heap(memory,16);if(!pointer)throw std::bad_alloc();
        store(pointer,64,table);store(pointer+8,64,0);
        D3D12State::Object object;object.kind=kind;object.device=device;bridge.objects.emplace(pointer,std::move(object));
        if(device)++bridge.objects.at(device).references;
        return pointer;
    };
    const auto blob=[&](std::vector<std::uint8_t> data){const auto object=create(Kind::Blob,0);const auto copy=allocate_heap(memory,std::max<std::size_t>(data.size(),1));if(!copy)throw std::bad_alloc();
        if(!data.empty())memory.write(GuestAddress{copy},data);store(object+8,64,copy);bridge.blobs[object]=std::move(data);return object;};
    // QueryInterface 取得物件：IID 屬於該種類介面鏈才成功。
    const auto query=[&](std::uint64_t self,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        if(!out)return pointer_error;store(out,64,0);if(!riid)return invalid;
        const auto found=bridge.objects.find(self);if(found==bridge.objects.end())return invalid;
        const auto iid=read_guid(memory,riid);bool match=iid==iid_unknown || (found->second.kind==Kind::Blob && iid==iid_blob) ||
            (found->second.kind==Kind::SwapChain && std::find(swap_iids.begin(),swap_iids.end(),iid)!=swap_iids.end());
        if(const auto* item=info_of(found->second.kind))for(const auto name:item->chain)if(const auto* abi=d3d12abi::find_interface(name);abi && abi->iid==iid)match=true;
        if(!match){
            if(trace_graphics_errors){std::array<std::uint8_t,16> raw{};memory.read(GuestAddress{riid},raw);std::fprintf(stderr,"[d3d12] QueryInterface kind=%u iid=",static_cast<unsigned>(found->second.kind));for(const auto b:raw)std::fprintf(stderr,"%02x",b);std::fprintf(stderr,"\n");}
            return no_interface;
        }
        ++found->second.references;store(out,64,self);return ok;
    };
    const auto give=[&](std::uint64_t object,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        // 新物件參照由呼叫端取得；介面不符時釋放剛建立的物件。
        const auto result=query(object,riid,out);
        auto& entry=bridge.objects.at(object);--entry.references;return result;
    };
    // D3D12CreateDevice：功能層級最高 12_1；Metal 後端由 D3D11 相同工廠建立。
    if(symbol=="D3D12CreateDevice"){
        // 1_0_GENERIC（0x100）／1_0_CORE（0x1000）為計算專用層級，完整裝置亦滿足。
        const auto level=u32(1);
        if(level!=0x100 && level!=0x1000 && (level<0xb000 || level>0xc100))return finish(level>0xc100?unsupported:invalid);
        if(arg(0) && !arg(3))return finish(false_ok);
        std::shared_ptr<NativeOpenGlContext> native;
        try{native=make_metal_opengl(nullptr,1,1);}catch(const std::exception&){native.reset();}
        if(!native)return finish(unsupported);
        if(!arg(3))return finish(false_ok);
        // 每幀十餘次 ExecuteCommandLists：在途上限 3 讓 CPU 每三次提交就等 GPU（電馭判客 Benchmark 上限 8 時快約 8%）。
        native->graphics_submission_depth(8);
        d3d12_start_submitter(memory);
        std::shared_ptr<D3D12DrawPool> pool;
        // 只在有提交執行緒時啟用：重播都在提交執行緒上，其他執行緒的後端呼叫持圖形鎖並先等佇列清空。
        static const bool command_thread=[] {const auto* value=std::getenv("X64ARM_D3D12_COMMAND_THREAD");return !value || std::string_view(value)!="0";}();
        if(command_thread && d3d12_submitter_){
            // 編碼執行緒的錯誤記成 D3D12 註記（與重播失敗相同），不延到協調者或其他執行緒的後端呼叫才拋出。
            auto* const shared=d3d12_.get();
            native=make_metal_command_thread(std::move(native),[shared](const std::string& error){
                static const bool trace_errors=std::getenv("X64ARM_TRACE_D3D12")!=nullptr;
                if(trace_errors)std::fprintf(stderr,"[d3d12] 編碼執行緒失敗：%s\n",error.c_str());
                const std::lock_guard lock(shared->notes_mutex);if(shared->worker_notes.size()<4096)shared->worker_notes.push_back("d3d12_note=編碼執行緒失敗："+error);
            });
            pool=std::make_shared<D3D12DrawPool>();
        }
        const auto device=create(Kind::Device,0);bridge.devices[device]=D3D12State::Device{std::move(native),0xc100,std::move(pool)};
        return finish(give(device,arg(2),arg(3)));
    }
    if(symbol=="D3D12GetDebugInterface" || symbol=="D3D12GetInterface" || symbol=="D3D12EnableExperimentalFeatures"){
        if(symbol!="D3D12EnableExperimentalFeatures" && arg(symbol=="D3D12GetInterface"?2:1))store(arg(symbol=="D3D12GetInterface"?2:1),64,0);
        return finish(no_interface);
    }
    // 根簽章：客體 D3D12 描述轉成 vkd3d 1.1 描述後序列化為 DXBC RTS0。
    if(symbol=="D3D12SerializeRootSignature" || symbol=="D3D12SerializeVersionedRootSignature"){
        const bool versioned=symbol=="D3D12SerializeVersionedRootSignature";const auto out=arg(versioned?1:2),error_out=arg(versioned?2:3);
        if(!out)return finish(pointer_error);store(out,64,0);if(error_out)store(error_out,64,0);
        if(!arg(0))return finish(invalid);
        std::uint32_t version=versioned?static_cast<std::uint32_t>(load(arg(0),32)):u32(1);const auto desc=versioned?arg(0)+8:arg(0);
        if(version!=1 && version!=2)return finish(invalid);
        const bool v11=version==2;
        vkd3d_shader_versioned_root_signature_desc root{};root.version=VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1;
        std::vector<vkd3d_shader_root_parameter1> parameters(static_cast<std::size_t>(load(desc,32)));
        std::vector<std::vector<vkd3d_shader_descriptor_range1>> ranges(parameters.size());
        std::vector<vkd3d_shader_static_sampler_desc> samplers(static_cast<std::size_t>(load(desc+16,32)));
        if(parameters.size()>64 || samplers.size()>2032)return finish(invalid);
        const auto parameter_at=load(desc+8,64),sampler_at=load(desc+24,64);
        for(std::size_t i=0;i<parameters.size();++i){
            const auto at=parameter_at+i*32;auto& p=parameters[i];p.parameter_type=static_cast<vkd3d_shader_root_parameter_type>(load(at,32));p.shader_visibility=static_cast<vkd3d_shader_visibility>(load(at+24,32));
            if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
                const auto count=load(at+8,32),list=load(at+16,64);if(count>4096)return finish(invalid);
                for(std::uint64_t j=0;j<count;++j){const auto r=list+j*(v11?24:20);vkd3d_shader_descriptor_range1 range{};
                    range.range_type=static_cast<vkd3d_shader_descriptor_type>(load(r,32));range.descriptor_count=static_cast<unsigned>(load(r+4,32));range.base_shader_register=static_cast<unsigned>(load(r+8,32));
                    range.register_space=static_cast<unsigned>(load(r+12,32));range.flags=v11?static_cast<vkd3d_shader_descriptor_range_flags>(load(r+16,32)):VKD3D_SHADER_DESCRIPTOR_RANGE_FLAG_NONE;
                    range.descriptor_table_offset=static_cast<unsigned>(load(r+(v11?20:16),32));ranges[i].push_back(range);}
                p.u.descriptor_table.descriptor_range_count=static_cast<unsigned>(ranges[i].size());p.u.descriptor_table.descriptor_ranges=ranges[i].data();
            }else if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){
                p.u.constants.shader_register=static_cast<unsigned>(load(at+8,32));p.u.constants.register_space=static_cast<unsigned>(load(at+12,32));p.u.constants.value_count=static_cast<unsigned>(load(at+16,32));
            }else{
                p.u.descriptor.shader_register=static_cast<unsigned>(load(at+8,32));p.u.descriptor.register_space=static_cast<unsigned>(load(at+12,32));
                p.u.descriptor.flags=v11?static_cast<vkd3d_shader_root_descriptor_flags>(load(at+16,32)):VKD3D_SHADER_ROOT_DESCRIPTOR_FLAG_NONE;
            }
        }
        for(std::size_t i=0;i<samplers.size();++i){
            const auto at=sampler_at+i*52;auto& s=samplers[i];
            s.filter=static_cast<vkd3d_shader_filter>(load(at,32));s.address_u=static_cast<vkd3d_shader_texture_address_mode>(load(at+4,32));
            s.address_v=static_cast<vkd3d_shader_texture_address_mode>(load(at+8,32));s.address_w=static_cast<vkd3d_shader_texture_address_mode>(load(at+12,32));
            s.mip_lod_bias=std::bit_cast<float>(static_cast<std::uint32_t>(load(at+16,32)));s.max_anisotropy=static_cast<unsigned>(load(at+20,32));
            s.comparison_func=static_cast<vkd3d_shader_comparison_func>(load(at+24,32));s.border_colour=static_cast<vkd3d_shader_static_border_colour>(load(at+28,32));
            s.min_lod=std::bit_cast<float>(static_cast<std::uint32_t>(load(at+32,32)));s.max_lod=std::bit_cast<float>(static_cast<std::uint32_t>(load(at+36,32)));
            s.shader_register=static_cast<unsigned>(load(at+40,32));s.register_space=static_cast<unsigned>(load(at+44,32));s.shader_visibility=static_cast<vkd3d_shader_visibility>(load(at+48,32));
        }
        root.u.v_1_1.parameter_count=static_cast<unsigned>(parameters.size());root.u.v_1_1.parameters=parameters.data();
        root.u.v_1_1.static_sampler_count=static_cast<unsigned>(samplers.size());root.u.v_1_1.static_samplers=samplers.data();
        root.u.v_1_1.flags=static_cast<vkd3d_shader_root_signature_flags>(load(desc+32,32));
        vkd3d_shader_versioned_root_signature_desc converted{};const vkd3d_shader_versioned_root_signature_desc* serialized=&root;
        if(!v11){if(vkd3d_shader_convert_root_signature(&converted,VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_0,&root)<0)return finish(invalid);serialized=&converted;}
        vkd3d_shader_code code{};char* messages=nullptr;const auto status=vkd3d_shader_serialize_root_signature(serialized,&code,&messages);
        if(!v11)vkd3d_shader_free_root_signature(&converted);
        if(status<0){if(error_out){const std::string text=messages?messages:"";store(error_out,64,blob(std::vector<std::uint8_t>(text.begin(),text.end()+0)));}vkd3d_shader_free_messages(messages);return finish(invalid);}
        vkd3d_shader_free_messages(messages);
        std::vector<std::uint8_t> data(static_cast<const std::uint8_t*>(code.code),static_cast<const std::uint8_t*>(code.code)+code.size);vkd3d_shader_free_shader_code(&code);
        store(out,64,blob(std::move(data)));return finish(ok);
    }
    // 交換鏈背景緩衝：裝置上可作 RTV／SRV 的 2D 紋理，包成 ID3D12Resource。
    const auto swap_buffers=[&](D3D12State::SwapChain& swap,std::uint32_t count){
        auto& native=*bridge.devices.at(swap.device).native;
        for(const auto buffer:swap.buffers){bridge.resources.erase(buffer);bridge.objects.erase(buffer);}
        swap.buffers.clear();swap.index=0;
        for(std::uint32_t i=0;i<count;++i){
            D3D12State::Resource r;r.dimension=3;r.width=swap.width;r.height=swap.height;r.format=swap.format;r.flags=1;r.heap_type=1;r.desc.assign(56,0);
            const auto put=[&](unsigned at,std::uint64_t value,unsigned size){std::memcpy(r.desc.data()+at,&value,size);};
            put(0,3,4);put(16,swap.width,8);put(24,swap.height,4);put(28,1|(1U<<16),4);put(32,swap.format,4);put(36,1,4);put(48,1,4);
            GraphicsResourceDesc d;d.dimension=3;d.width=swap.width;d.height=swap.height;d.format=swap.format;d.bind=0x28;r.gpu=native.graphics_resource(d);
            const auto object=create(Kind::Resource,swap.device);bridge.resources[object]=std::move(r);swap.buffers.push_back(object);
        }
        // 無原生 GUI（headless）時以離屏表面呈現，與 D3D11 交換鏈相同。
        if(gui_.windows.contains(swap.window))swap.native->drawable(native_gui_?native_gui_->graphics_view(swap.window):nullptr,swap.width,swap.height);
    };
    // DXGI 工廠收到 D3D12 佇列時轉入此處；參數位置與原 API 相同（D3D12 只允許翻轉模型）。
    if(symbol=="DXGI.CreateSwapChain" || symbol=="DXGI.CreateSwapChainForHwnd"){
        const bool modern=symbol=="DXGI.CreateSwapChainForHwnd";const auto queue=arg(1),out=arg(modern?6:3),desc=arg(modern?3:2);
        if(!out || !desc)return finish(pointer_error);store(out,64,0);
        if(!bridge.queues.contains(queue))return finish(invalid);
        D3D12State::SwapChain swap;std::uint32_t count=0,effect=0,samples=0;
        if(modern){swap.window=arg(2);swap.width=static_cast<std::uint32_t>(load(desc,32));swap.height=static_cast<std::uint32_t>(load(desc+4,32));swap.format=static_cast<std::uint32_t>(load(desc+8,32));
            samples=static_cast<std::uint32_t>(load(desc+16,32));count=static_cast<std::uint32_t>(load(desc+28,32));effect=static_cast<std::uint32_t>(load(desc+36,32));swap.flags=static_cast<std::uint32_t>(load(desc+44,32));swap.desc1=bytes(desc,48);}
        else{swap.width=static_cast<std::uint32_t>(load(desc,32));swap.height=static_cast<std::uint32_t>(load(desc+4,32));swap.format=static_cast<std::uint32_t>(load(desc+16,32));
            samples=static_cast<std::uint32_t>(load(desc+28,32));count=static_cast<std::uint32_t>(load(desc+40,32));swap.window=load(desc+48,64);effect=static_cast<std::uint32_t>(load(desc+60,32));swap.flags=static_cast<std::uint32_t>(load(desc+64,32));
            swap.desc1.assign(48,0);const auto put=[&](unsigned at,std::uint32_t value){std::memcpy(swap.desc1.data()+at,&value,4);};
            put(8,swap.format);put(16,samples);put(24,static_cast<std::uint32_t>(load(desc+36,32)));put(28,count);put(36,effect);put(44,swap.flags);}
        const auto window=gui_.windows.find(swap.window);if(window==gui_.windows.end())return finish(invalid);
        if(!swap.width)swap.width=window->second.width;if(!swap.height)swap.height=window->second.height;
        if((effect!=3 && effect!=4) || count<2 || count>16 || samples!=1 || !swap.width || !swap.height)return finish(invalid);
        swap.queue=queue;swap.device=bridge.objects.at(queue).device;swap.factory=arg(0);
        try{swap.native=make_metal_opengl(nullptr,1,1);swap_buffers(swap,count);}
        catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=CreateSwapChain 失敗：")+error.what());return finish(invalid);}
        const auto object=create(Kind::SwapChain,swap.device);bridge.swaps[object]=std::move(swap);store(out,64,object);return finish(ok);
    }
    if(symbol=="D3D12CreateRootSignatureDeserializer" || symbol=="D3D12CreateVersionedRootSignatureDeserializer"){if(arg(3))store(arg(3),64,0);report(symbol);return finish(not_implemented);}
    const auto dot=symbol.find('.');if(dot==std::string_view::npos)return finish(not_implemented);
    const auto member=symbol.substr(dot+1);const auto self=arg(0);
    const auto found=bridge.objects.find(self);if(found==bridge.objects.end())return finish(invalid);
    const auto kind=found->second.kind;
    if(member=="QueryInterface")return finish(query(self,arg(1),arg(2)));
    if(member=="AddRef")return finish(++found->second.references);
    if(member=="Release"){
        // 物件與宿主資源在最後一個參照釋放時移除；客體記憶體區塊保留，避免懸置的 vtable 指標。
        if(found->second.references)--found->second.references;
        const auto remaining=found->second.references;
        if(!remaining){
            const auto device=found->second.device;
            // 零複製映射撤除客體範圍；宿主記憶體由 GPU 別名持有到在途命令完成。
            const auto unmap=[&](std::uint64_t address,std::uint64_t size){try{memory.unmap(GuestAddress{address},static_cast<std::size_t>(size));}catch(const std::exception&){}};
            // 位址索引同時移除：資源與描述元堆積常逐幀建立／釋放，只在查詢時清除失效項目會讓索引無限增長、查找越來越慢。
            if(kind==Kind::Resource)if(const auto r=bridge.resources.find(self);r!=bridge.resources.end() && r->second.address)
                if(const auto a=bridge.addresses.find(r->second.address);a!=bridge.addresses.end() && a->second==self)bridge.addresses.erase(a);
            if(kind==Kind::DescriptorHeap)if(const auto h=bridge.heaps.find(self);h!=bridge.heaps.end())
                for(const auto base:{h->second.cpu,h->second.gpu})if(const auto a=bridge.descriptor_ranges.find(base);base && a!=bridge.descriptor_ranges.end() && a->second==self)bridge.descriptor_ranges.erase(a);
            if(kind==Kind::Resource){if(const auto r=bridge.resources.find(self);r!=bridge.resources.end()){
                if(r->second.host_mapping)unmap(r->second.cpu,r->second.host_mapping);else if(r->second.cpu && r->second.owned_cpu)(void)free_heap(memory,r->second.cpu,state);
                bridge.forget_address_hits(r->second);bridge.resources.erase(r);}}
            if(kind==Kind::Heap){if(const auto h=bridge.memory_heaps.find(self);h!=bridge.memory_heaps.end()){
                if(h->second.host)unmap(h->second.cpu,h->second.host_bytes.size());else if(h->second.cpu && !h->second.external)(void)free_heap(memory,h->second.cpu,state);bridge.memory_heaps.erase(h);}}
            bridge.signatures.erase(self);bridge.query_heaps.erase(self);
            bridge.heaps.erase(self);bridge.roots.erase(self);bridge.pipelines.erase(self);bridge.lists.erase(self);bridge.fences.erase(self);bridge.queues.erase(self);
            if(const auto library=bridge.libraries.find(self);library!=bridge.libraries.end()){
                for(const auto& [name,pipeline]:library->second)if(const auto entry=bridge.objects.find(pipeline);pipeline && entry!=bridge.objects.end() && entry->second.references && !--entry->second.references){
                    const auto owner=entry->second.device;bridge.pipelines.erase(pipeline);bridge.objects.erase(entry);
                    if(const auto parent=bridge.objects.find(owner);parent!=bridge.objects.end() && parent->second.references)--parent->second.references;}
                bridge.libraries.erase(library);
            }
            if(const auto swap=bridge.swaps.find(self);swap!=bridge.swaps.end()){for(const auto buffer:swap->second.buffers){bridge.resources.erase(buffer);bridge.objects.erase(buffer);}bridge.swaps.erase(swap);}
            if(kind==Kind::Blob){if(const auto data=load(self+8,64))(void)free_heap(memory,data,state);bridge.blobs.erase(self);}
            if(kind==Kind::Device)bridge.devices.erase(self);
            bridge.objects.erase(found);
            if(device)if(const auto parent=bridge.objects.find(device);parent!=bridge.objects.end() && parent->second.references)--parent->second.references;
        }
        return finish(remaining);
    }
    if(kind==Kind::Blob){
        if(member=="GetBufferPointer")return finish(load(self+8,64));
        if(member=="GetBufferSize"){const auto data=bridge.blobs.find(self);return finish(data==bridge.blobs.end()?0:data->second.size());}
        return finish(not_implemented);
    }
    // ID3D12Object 共用方法。
    if(member=="SetName" || member=="SetPrivateDataInterface")return finish(ok);
    if(member=="SetPrivateData"){
        if(!arg(1))return finish(invalid);const auto guid=read_guid(memory,arg(1));
        if(!arg(3)){found->second.data.erase(guid);return finish(ok);}
        found->second.data[guid]=bytes(arg(3),u32(2));return finish(ok);
    }
    if(member=="GetPrivateData"){
        if(!arg(1) || !arg(2))return finish(invalid);const auto item=found->second.data.find(read_guid(memory,arg(1)));
        if(item==found->second.data.end()){store(arg(2),32,0);return finish(not_found);}
        const auto capacity=load(arg(2),32);store(arg(2),32,item->second.size());
        if(!arg(3))return finish(ok);if(capacity<item->second.size())return finish(0x887a0003U); // DXGI_ERROR_MORE_DATA。
        memory.write(GuestAddress{arg(3)},item->second);return finish(ok);
    }
    if(member=="GetDevice"){const auto device=kind==Kind::Device?self:found->second.device;return finish(query(device,arg(1),arg(2)));}
    // 目前物件已在上方查到：直接取所屬裝置，不再對物件表做第二次樹狀查找（每次服務呼叫都經過）。
    auto& device=bridge.devices.at(kind==Kind::Device?self:found->second.device);auto& native=*device.native;
    // 描述元控制代碼：CPU／GPU 位址皆為堆積基底加索引×32。
    // 依位址索引找出 handle 所屬堆積與項目索引（每次重播會為每個表項目查詢，不能逐一掃描所有堆積）。
    // 只讀：索引項在堆積 Release 時移除；worker 上不持圖形鎖的錄製也會查詢。
    const auto heap_at=[&](std::uint64_t handle,bool gpu)->std::pair<D3D12State::Heap*,std::uint64_t>{
        auto it=bridge.descriptor_ranges.upper_bound(handle);if(it==bridge.descriptor_ranges.begin())return {};--it;
        const auto found=bridge.heaps.find(it->second);
        if(found==bridge.heaps.end() || (it->first!=found->second.cpu && it->first!=found->second.gpu))return {};
        auto& heap=found->second;const auto base=gpu?heap.gpu:heap.cpu;
        if(!base || it->first!=base || handle>=base+std::uint64_t{heap.count}*descriptor_size || (handle-base)%descriptor_size)return {};
        return {&heap,(handle-base)/descriptor_size};
    };
    // 描述元寫入：在本地組好新值（value 由預設的空描述元開始），離開範圍時於分段鎖內與堆積項目交換，舊值在鎖外釋放；
    // cancel 表示保留原項目不寫入。只以 write_descriptor 回傳的暫值建立（保證複製省略，不會重複寫入）。
    struct DescriptorWrite {
        D3D12State& state;D3D12State::Heap* heap{};std::uint64_t index{};D3D12State::Descriptor value;bool active{};
        DescriptorWrite(D3D12State& owner,D3D12State::Heap* target,std::uint64_t slot):state(owner),heap(target),index(slot),active(target!=nullptr){}
        explicit operator bool() const noexcept {return heap!=nullptr;}
        void cancel() noexcept {active=false;}
        DescriptorWrite(const DescriptorWrite&)=delete;DescriptorWrite& operator=(const DescriptorWrite&)=delete;
        ~DescriptorWrite(){
            if(!heap || !active)return;
            {const std::lock_guard lock(state.descriptor_lock(heap,index));std::swap(heap->descriptors[index],value);}
            D3D12State::mark_dirty(*heap,index);
        }
    };
    const auto write_descriptor=[&](std::uint64_t handle)->DescriptorWrite{
        const auto [heap,index]=heap_at(handle,false);
        return DescriptorWrite(bridge,heap,index);
    };
    // 描述元讀取（錄製時解析 RTV／DSV／UAV、複製來源）：在分段鎖內複製。
    const auto read_descriptor=[&](std::uint64_t handle)->std::optional<D3D12State::Descriptor>{
        const auto [heap,index]=heap_at(handle,false);if(!heap)return std::nullopt;
        const std::lock_guard lock(bridge.descriptor_lock(heap,index));return heap->descriptors[index];
    };
    const auto resource_at=[&](std::uint64_t address)->std::pair<std::uint64_t,std::uint64_t>{
        const auto granule=address>>16;auto& hit=bridge.address_hits[granule&(bridge.address_hits.size()-1)];
        if(hit.granule==granule+1){
            const auto& resource=*hit.resource;
            if(address>=resource.address && address<resource.address+std::max<std::uint64_t>(resource.width,1))return {hit.object,address-resource.address};
        }
        // 不在查詢時刪除失效項目：資源釋放時已即時移除。
        auto it=bridge.addresses.upper_bound(address);if(it==bridge.addresses.begin())return {0,0};--it;
        const auto found=bridge.resources.find(it->second);
        if(found==bridge.resources.end() || found->second.address!=it->first)return {0,0};
        const auto& resource=found->second;if(address>=resource.address+std::max<std::uint64_t>(resource.width,1))return {0,0};
        hit={granule+1,found->first,&resource};
        return {found->first,address-resource.address};
    };
    // UPLOAD 堆積：GPU 讀取前以客體後備記憶體更新影子資源；READBACK：GPU 寫入後回寫。
    // 同一次服務呼叫（一次 ExecuteCommandLists 重播）期間客體不會改寫已提交的上傳資料：每個資源只上傳實際用到的範圍，
    // 已上傳的範圍不重複複製；下一次呼叫重新上傳（客體可能已寫入新資料）。
    std::map<std::uint64_t,std::vector<std::pair<std::uint64_t,std::uint64_t>>> uploaded;
    const auto gpu_resource_of=[&](D3D12State::Resource& r,std::uint64_t object,std::uint64_t first,std::uint64_t length,const char* why)->std::shared_ptr<GraphicsResource>{
        if(!r.gpu){GraphicsResourceDesc d;d.dimension=1;d.width=static_cast<std::uint32_t>(r.width);d.bind=0x8c;auto created=native.graphics_resource(d);
            const std::lock_guard lock(bridge.resource_lock(object));r.gpu=std::move(created);}
        if(r.cpu && r.uploads() && !r.host_shared && first<r.width){
            // 範圍對齊 4 位元組：Metal 緩衝 blit 要求位移與長度為 4 的倍數，否則後端改走同步等待後直寫。
            // 來源是完整的客體記憶體，多上傳邊界上的位元組不影響內容。
            auto end=length>r.width-first?r.width:first+length;first&=~UINT64_C(3);end=std::min<std::uint64_t>(r.width,(end+3)&~UINT64_C(3));auto& done=uploaded[object];
            if(D3D12Submitter::tracing() && d3d12_submitter_)d3d12_submitter_->stats.upload_steps.fetch_add(done.size(),std::memory_order_relaxed);
            if(std::none_of(done.begin(),done.end(),[&](const auto& range){return range.first<=first && end<=range.second;})){
                thread_local std::vector<std::uint8_t> scratch;const auto size=static_cast<std::size_t>(end-first);if(scratch.size()<size)scratch.resize(size);
                const std::span<std::uint8_t> data(scratch.data(),size);memory.read(GuestAddress{r.cpu+first},data);
                GraphicsView view{r.gpu};view.first=static_cast<std::uint32_t>(first);view.count=static_cast<std::uint32_t>(size);
                // D3D12 規範：ExecuteCommandLists 之後到 GPU 完成前，客體不得改寫被引用的上傳範圍；客體改寫的範圍因此不會被
                // 任何已編碼或在途的命令讀取（重播落後客體，但 fence 在重播與 GPU 完成後才觸發），可直接寫入影子緩衝，
                // 不必以 staging 複製排在命令流中（每次 blit 都會切斷進行中的 render pass）。與實體驅動的 UPLOAD 堆積語義相同。
                // 只限純 UPLOAD 堆積：CPU 可見的自訂堆積也可能被 GPU 寫入，仍依命令順序排入複製。
                if(d3d12_direct_uploads() && r.heap_type==2)native.graphics_write_unused(view,data);
                else native.graphics_write(view,data,static_cast<std::uint32_t>(size),static_cast<std::uint32_t>(size));
                done.emplace_back(first,end);
                if(trace_stats_uploads)sync_reads[std::string("upload_bytes_")+why]+=size;
            }
        }
        return r.gpu;
    };
    const auto gpu_resource=[&](std::uint64_t object,std::uint64_t first=0,std::uint64_t length=UINT64_MAX,const char* why="other")->std::shared_ptr<GraphicsResource>{
        return gpu_resource_of(bridge.resources.at(object),object,first,length,why);
    };
    const auto read_back=[&](std::uint64_t object){
        auto& r=bridge.resources.at(object);if(!r.cpu || !r.gpu || r.host_shared)return;
        const auto data=native.graphics_read(GraphicsView{r.gpu});memory.write(GuestAddress{r.cpu},std::span<const std::uint8_t>(data).first(std::min<std::size_t>(data.size(),static_cast<std::size_t>(r.width))));
    };
    if(kind==Kind::SwapChain){
        auto& swap=bridge.swaps.at(self);
        if(member=="Present" || member=="Present1"){
            if(trace_graphics_errors && (swap.frames<8 || !(swap.frames%64)))std::fprintf(stderr,"[d3d12] Present frame=%llu t=%.3f sync=%u flags=%#x window=%#llx known=%d\n",static_cast<unsigned long long>(swap.frames),
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),u32(1),u32(2),static_cast<unsigned long long>(swap.window),gui_.windows.contains(swap.window)?1:0);
            // 診斷：X64ARM_TRACE_GRAPHICS_STATS=1 每 64 幀輸出 Metal 後端累計統計（GPU 忙碌、CPU 等待、命令執行緒同步、encoder 斷點），與 D3D11 相同。
            static const bool trace_stats=[] {const auto* value=std::getenv("X64ARM_TRACE_GRAPHICS_STATS");return value && std::string_view(value)=="1";}();
            if(trace_stats && !(swap.frames%64)){
                std::string reads;for(const auto& [why,count]:sync_reads)reads+=why+":"+std::to_string(count)+",";
                std::fprintf(stderr,"[d3d12_sync] frames=%llu %s\n",static_cast<unsigned long long>(swap.frames),reads.empty()?"none":reads.c_str());
                std::fprintf(stderr,"[d3d12_metal] frames=%llu t=%.3f %s",static_cast<unsigned long long>(swap.frames),
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),native.statistics().c_str());
            }
            if(u32(2)&1)return finish(ok); // DXGI_PRESENT_TEST。
            // 非同步提交：Present 與 ExecuteCommandLists 同樣排入提交執行緒，依序在該幀的重播之後呈現；客體立即取得下一張
            // 背景緩衝。在途 Present 超過 3（DXGI 預設最大幀延遲）才等，原本在全停中等佇列清空，全部 worker 一起停 20 ms 以上。
            // 提交執行緒的工作以 arguments[3] 帶入當時的緩衝索引（bit 0～31）與視窗是否存在（bit 32）。
            const auto present=[&](std::uint32_t index,std::uint32_t interval,bool visible){
                auto& buffer=bridge.resources.at(swap.buffers[index]);GraphicsView view{buffer.gpu};
                native.graphics_handoff(*swap.native,view);swap.native->swap_interval(interval?1:0);
                if(visible)swap.native->graphics_present(view);
                swap.native->graphics_handoff(native,view);
            };
            if(submit_job){
                try{present(static_cast<std::uint32_t>(arg(3)),u32(1),(arg(3)>>32)&1U);}
                catch(const std::exception& error){note(std::string("d3d12_note=Present 失敗：")+error.what());}
                return std::nullopt;
            }
            if(d3d12_submitter_){
                auto& submitter=*d3d12_submitter_;
                D3D12Submitter::Job job;job.method=&d3d12_present_method;
                job.arguments={self,u32(1),u32(2),std::uint64_t{swap.index}|(gui_.windows.contains(swap.window)?UINT64_C(1)<<32:0)};
                if(graphics_guard.owns_lock())graphics_guard.unlock(); // 等在途 Present 時提交執行緒需要圖形鎖。
                {
                    std::unique_lock lock(submitter.mutex);
                    if(submitter.presents>=3){
                        const auto start=D3D12Submitter::tracing()?D3D12Submitter::now_ns():0;
                        submitter.idle.wait(lock,[&]{return submitter.presents<3;});
                        if(start)submitter.stats.present_wait.fetch_add(D3D12Submitter::now_ns()-start,std::memory_order_relaxed);
                    }
                    ++submitter.presents;
                }
                d3d12_enqueue(submitter,std::move(job),false);
                swap.index=(swap.index+1)%static_cast<std::uint32_t>(swap.buffers.size());++swap.frames;return finish(ok);
            }
            try{present(swap.index,u32(1),gui_.windows.contains(swap.window));}
            catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=Present 失敗：")+error.what());return finish(fail);}
            swap.index=(swap.index+1)%static_cast<std::uint32_t>(swap.buffers.size());++swap.frames;return finish(ok);
        }
        if(member=="GetBuffer"){if(!arg(3))return finish(pointer_error);store(arg(3),64,0);if(u32(1)>=swap.buffers.size())return finish(invalid);return finish(query(swap.buffers[u32(1)],arg(2),arg(3)));}
        if(member=="GetCurrentBackBufferIndex")return finish(swap.index);
        if(member=="GetDevice")return finish(query(swap.queue,arg(1),arg(2)));
        if(member=="GetHwnd"){if(!arg(1))return finish(pointer_error);store(arg(1),64,swap.window);return finish(ok);}
        if(member=="GetDesc1"){if(!arg(1))return finish(pointer_error);auto desc=swap.desc1;std::memcpy(desc.data(),&swap.width,4);std::memcpy(desc.data()+4,&swap.height,4);std::memcpy(desc.data()+8,&swap.format,4);memory.write(GuestAddress{arg(1)},desc);return finish(ok);}
        if(member=="GetDesc"){
            if(!arg(1))return finish(pointer_error);memory.fill(GuestAddress{arg(1)},0,72);
            const auto word=[&](unsigned at){std::uint32_t value;std::memcpy(&value,swap.desc1.data()+at,4);return value;};
            store(arg(1),32,swap.width);store(arg(1)+4,32,swap.height);store(arg(1)+8,32,60);store(arg(1)+12,32,1);store(arg(1)+16,32,swap.format);store(arg(1)+28,32,1);
            store(arg(1)+36,32,word(24));store(arg(1)+40,32,static_cast<std::uint32_t>(swap.buffers.size()));store(arg(1)+48,64,swap.window);store(arg(1)+56,32,1);store(arg(1)+60,32,word(36));store(arg(1)+64,32,swap.flags);return finish(ok);
        }
        if(member=="ResizeBuffers" || member=="ResizeBuffers1"){
            const auto count=u32(1)?u32(1):static_cast<std::uint32_t>(swap.buffers.size());const auto window=gui_.windows.find(swap.window);
            swap.width=u32(2)?u32(2):window!=gui_.windows.end()?window->second.width:swap.width;swap.height=u32(3)?u32(3):window!=gui_.windows.end()?window->second.height:swap.height;
            if(u32(4))swap.format=u32(4);if(count<2 || count>16)return finish(invalid);
            std::memcpy(swap.desc1.data()+28,&count,4);
            try{swap_buffers(swap,count);}catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=ResizeBuffers 失敗：")+error.what());return finish(invalid);}
            return finish(ok);
        }
        if(member=="GetFullscreenState"){if(arg(1))store(arg(1),32,0);if(arg(2))store(arg(2),64,0);return finish(ok);}
        if(member=="SetFullscreenState"){if(u32(1))report("SetFullscreenState(TRUE)");return finish(ok);}
        if(member=="GetLastPresentCount"){if(!arg(1))return finish(pointer_error);store(arg(1),32,swap.frames);return finish(ok);}
        if(member=="GetFrameStatistics")return finish(0x887a000bU); // DXGI_ERROR_FRAME_STATISTICS_DISJOINT。
        if(member=="GetContainingOutput" || member=="GetParent" || member=="GetRestrictToOutput" || member=="GetCoreWindow"){if(arg(member=="GetParent" || member=="GetCoreWindow"?2:1))store(arg(member=="GetParent" || member=="GetCoreWindow"?2:1),64,0);return finish(member=="GetParent"?no_interface:unsupported);}
        if(member=="CheckColorSpaceSupport"){if(!arg(2))return finish(pointer_error);store(arg(2),32,u32(1)==0?1:0);return finish(ok);}
        if(member=="SetColorSpace1")return finish(u32(1)==0?ok:invalid);
        if(member=="SetMaximumFrameLatency" || member=="SetBackgroundColor" || member=="SetRotation" || member=="SetSourceSize" || member=="SetMatrixTransform" || member=="SetHDRMetaData" || member=="ResizeTarget")return finish(ok);
        if(member=="GetMaximumFrameLatency"){if(arg(1))store(arg(1),32,3);return finish(ok);}
        if(member=="GetSourceSize"){if(arg(1))store(arg(1),32,swap.width);if(arg(2))store(arg(2),32,swap.height);return finish(ok);}
        if(member=="GetRotation"){if(arg(1))store(arg(1),32,1);return finish(ok);}
        if(member=="IsTemporaryMonoSupported")return finish(0);
        report("Swap."+std::string(member));return finish(not_implemented);
    }
    if(kind==Kind::Device){
        if(member=="CreatePipelineLibrary"){
            // 空資料建立新庫；資料須為本實作 Serialize 的格式，其他驅動或版本的快取回 DRIVER_VERSION_MISMATCH，損毀回 E_INVALIDARG。
            std::map<std::u16string,std::uint64_t> names;const auto size=arg(2);
            if(size){
                if(!arg(1))return finish(invalid);if(size<20)return finish(driver_version_mismatch);
                const auto data=bytes(arg(1),static_cast<std::size_t>(std::min<std::uint64_t>(size,UINT64_C(64)*1024*1024)));
                if(!std::equal(library_magic.begin(),library_magic.end(),data.begin()))return finish(driver_version_mismatch);
                std::uint32_t count;std::memcpy(&count,data.data()+16,4);std::size_t at=20;
                for(std::uint32_t i=0;i<count;++i){
                    if(at+4>data.size())return finish(invalid);std::uint32_t length;std::memcpy(&length,data.data()+at,4);at+=4;
                    if(length>32768 || at+std::size_t{length}*2>data.size())return finish(invalid);
                    std::u16string name(length,0);std::memcpy(name.data(),data.data()+at,std::size_t{length}*2);at+=std::size_t{length}*2;names.emplace(std::move(name),0);
                }
            }
            if(!arg(4))return finish(false_ok);
            const auto object=create(Kind::PipelineLibrary,self);bridge.libraries[object]=std::move(names);return finish(give(object,arg(3),arg(4)));
        }
        // 駐留管理：Apple Silicon 為統一記憶體，沒有可被換出的顯示記憶體預算，所有 pageable 物件恆駐留；
        // EnqueueMakeResident 因此立即把 fence 推進到要求的值。物件須屬於本裝置的 pageable 種類。
        if(member=="MakeResident" || member=="Evict" || member=="SetResidencyPriority" || member=="EnqueueMakeResident"){
            const bool enqueue=member=="EnqueueMakeResident";const std::uint32_t count=u32(enqueue?2:1);const std::uint64_t list=arg(enqueue?3:2);
            if(enqueue && (u32(1)&~1U))return finish(invalid);if(count && !list)return finish(invalid);
            for(std::uint32_t i=0;i<count;++i){const auto object=bridge.objects.find(load(list+i*8,64));
                if(object==bridge.objects.end() || object->second.kind==Kind::Device || object->second.kind==Kind::Blob || object->second.kind==Kind::SwapChain)return finish(invalid);}
            if(enqueue){
                const auto fence=bridge.fences.find(arg(4));if(fence==bridge.fences.end())return finish(invalid);
                fence->second.value=std::max(fence->second.value,arg(5));
                std::erase_if(fence->second.events,[&](const auto& item){if(item.first>fence->second.value)return false;if(const auto event=runtime_.wait_objects.find(item.second);event!=runtime_.wait_objects.end())event->second.set_signal(true);return true;});
            }
            return finish(ok);
        }
        if(member=="GetNodeCount")return finish(1);
        if(member=="GetDescriptorHandleIncrementSize")return finish(descriptor_size);
        if(member=="GetDeviceRemovedReason")return finish(ok);
        if(member=="GetAdapterLuid"){if(arg(1))store(arg(1),64,display_identity::adapter_luid);return finish(arg(1));}
        if(member=="CheckFeatureSupport"){
            const auto feature=u32(1);const auto data=arg(2);const auto size=u32(3);if(!data)return finish(invalid);
            const auto zero=[&](std::uint32_t expected){if(size!=expected)return false;memory.fill(GuestAddress{data},0,expected);return true;};
            switch(feature){
            case 0:if(!zero(60))return finish(invalid);store(data+16,32,2);store(data+36,32,40);store(data+56,32,1);return finish(ok); // OPTIONS：ResourceBindingTier 2、VA 40 位元、ResourceHeapTier 1。
            case 1:if(!zero(16))return finish(invalid);store(data+4,32,1);store(data+8,32,1);store(data+12,32,1);return finish(ok); // ARCHITECTURE：TBDR、UMA、cache coherent。
            case 2:{if(size!=24)return finish(invalid);const auto count=load(data,32),list=load(data+8,64);std::uint32_t best=0;
                for(std::uint64_t i=0;i<count;++i){const auto level=static_cast<std::uint32_t>(load(list+i*4,32));if(level<=0xc100)best=std::max(best,level);}store(data+16,32,best);return finish(best?ok:unsupported);}
            case 3:{
                // FORMAT_SUPPORT：共用後端可對應的格式（含 BC 區塊壓縮）回報支援；其餘格式回報 0（不擲出例外）。
                if(size!=12)return finish(invalid);const auto format=static_cast<std::uint32_t>(load(data,32));bool known=graphics_format_bc_family(format)!=0;
                if(!known)try{known=graphics_format_bytes(format)!=0;}catch(const std::invalid_argument&){known=false;}
                store(data+4,32,known?0x0fffffffU:0U);store(data+8,32,0);return finish(ok);}
            case 4:if(size!=16)return finish(invalid);store(data+12,32,load(data+4,32)==1 || native.graphics_samples_supported(static_cast<std::uint32_t>(load(data,32)),static_cast<std::uint32_t>(load(data+4,32)))?1:0);return finish(ok);
            case 5:if(size!=8)return finish(invalid);store(data+4,32,1);return finish(ok);
            case 6:if(!zero(8))return finish(invalid);store(data,32,40);store(data+4,32,40);return finish(ok);
            case 7:if(size!=4)return finish(invalid);store(data,32,std::min<std::uint64_t>(load(data,32),0x60));return finish(ok); // 最高 SM6.0。
            case 12:if(size!=4)return finish(invalid);store(data,32,std::min<std::uint64_t>(load(data,32),2));return finish(ok);
            case 16:if(size!=20)return finish(invalid);store(data+4,32,1);store(data+8,32,1);store(data+12,32,1);store(data+16,32,1);return finish(ok);
            case 8:if(!zero(24))return finish(invalid);store(data,32,1);store(data+4,32,32);store(data+8,32,32);store(data+12,32,1024);store(data+16,32,1);store(data+20,32,1);return finish(ok); // OPTIONS1：wave 寬 32（Metal SIMD group）、ExpandedComputeResourceStates、64 位元整數。
            case 20:if(size!=12)return finish(invalid);store(data+8,32,load(data+4,32)==0 || load(data+4,32)==100?1:0);return finish(ok); // 佇列優先權：NORMAL／HIGH。
            // 其餘選項結構：未支援的能力回報 FALSE／tier 0（大小依 d3d12.h）。
            case 18:case 39:if(!zero(8))return finish(invalid);return finish(ok);
            case 19:case 22:case 36:case 40:if(!zero(4))return finish(invalid);return finish(ok);
            case 21:case 30:if(!zero(20))return finish(invalid);return finish(ok);
            case 23:case 27:case 41:if(!zero(12))return finish(invalid);return finish(ok);
            case 32:if(!zero(8))return finish(invalid);return finish(ok);
            case 37:case 42:if(!zero(24))return finish(invalid);return finish(ok);
            default:report("CheckFeatureSupport("+std::to_string(feature)+")");return finish(invalid);
            }
        }
        if(member=="CreateCommandQueue"){
            if(!arg(1))return finish(invalid);const auto type=static_cast<std::uint32_t>(load(arg(1),32));if(type>3 && type!=7)return finish(invalid);
            if(!arg(3))return finish(false_ok);
            const auto queue=create(Kind::Queue,self);bridge.queues[queue]=type;return finish(give(queue,arg(2),arg(3)));
        }
        if(member=="CreateCommandAllocator"){if(!arg(3))return finish(false_ok);return finish(give(create(Kind::Allocator,self),arg(2),arg(3)));}
        if(member=="CreateCommandList" || member=="CreateCommandList1"){
            const bool closed=member=="CreateCommandList1";const auto out=arg(closed?5:6),riid=arg(closed?4:5);
            if(!out)return finish(false_ok);
            const auto list=create(Kind::List,self);D3D12State::List record;record.type=u32(2);record.allocator=closed?0:arg(3);record.open=!closed;
            if(!closed && arg(4))record.commands.push_back({"SetPipelineState",{arg(4)},{},{}});
            bridge.lists[list]=std::move(record);return finish(give(list,riid,out));
        }
        if(member=="CreateFence"){
            if(!arg(4))return finish(false_ok);
            const auto fence=create(Kind::Fence,self);bridge.fences[fence].value=arg(1);return finish(give(fence,arg(3),arg(4)));
        }
        if(member=="CreateDescriptorHeap"){
            if(!arg(1))return finish(invalid);
            D3D12State::Heap heap;heap.type=static_cast<std::uint32_t>(load(arg(1),32));heap.count=static_cast<std::uint32_t>(load(arg(1)+4,32));heap.visible=(load(arg(1)+8,32)&1)!=0;
            if(heap.type>3 || !heap.count || heap.count>1000000)return finish(invalid);
            if(!arg(3))return finish(false_ok);
            heap.cpu=bridge.next_descriptor;bridge.next_descriptor+=(std::uint64_t{heap.count}*descriptor_size+0xffffU)&~UINT64_C(0xffff);
            if(heap.visible){heap.gpu=bridge.next_descriptor;bridge.next_descriptor+=(std::uint64_t{heap.count}*descriptor_size+0xffffU)&~UINT64_C(0xffff);}
            heap.descriptors.resize(heap.count);
            if(heap.visible && heap.type==0){
                heap.dirty.assign(heap.count,1);heap.dirty_chunks.assign((heap.count+63U)/64U,~std::uint64_t{0});
                if(heap.count%64U)heap.dirty_chunks.back()=(std::uint64_t{1}<<(heap.count%64U))-1U;
                heap.dirty_summary.assign((heap.dirty_chunks.size()+63U)/64U,~std::uint64_t{0});
                if(heap.dirty_chunks.size()%64U)heap.dirty_summary.back()=(std::uint64_t{1}<<(heap.dirty_chunks.size()%64U))-1U;
                heap.any_dirty=true;}
            const auto object=create(Kind::DescriptorHeap,self);bridge.descriptor_ranges[heap.cpu]=object;if(heap.gpu)bridge.descriptor_ranges[heap.gpu]=object;
            bridge.heaps[object]=std::move(heap);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="CreateRootSignature"){
            if(!arg(2) || !arg(3))return finish(invalid);
            const auto data=bytes(arg(2),static_cast<std::size_t>(std::min<std::uint64_t>(arg(3),16U*1024U*1024U)));
            vkd3d_shader_code code{data.data(),data.size()};vkd3d_shader_versioned_root_signature_desc parsed{},converted{};char* messages=nullptr;
            if(vkd3d_shader_parse_root_signature(&code,&parsed,&messages)<0){vkd3d_shader_free_messages(messages);return finish(invalid);}
            vkd3d_shader_free_messages(messages);
            const auto* desc=&parsed;if(parsed.version!=VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1){if(vkd3d_shader_convert_root_signature(&converted,VKD3D_SHADER_ROOT_SIGNATURE_VERSION_1_1,&parsed)<0){vkd3d_shader_free_root_signature(&parsed);return finish(invalid);}desc=&converted;}
            D3D12State::RootSignature root;
            for(unsigned i=0;i<desc->u.v_1_1.parameter_count;++i){
                const auto& p=desc->u.v_1_1.parameters[i];D3D12State::Parameter parameter;parameter.type=p.parameter_type;parameter.visibility=static_cast<std::uint32_t>(p.shader_visibility);
                if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
                    std::uint32_t offset=0;
                    for(unsigned j=0;j<p.u.descriptor_table.descriptor_range_count;++j){
                        const auto& r=p.u.descriptor_table.descriptor_ranges[j];
                        if(r.descriptor_table_offset!=0xffffffffU)offset=r.descriptor_table_offset;
                        parameter.ranges.push_back({static_cast<std::uint32_t>(r.range_type),r.descriptor_count,r.base_shader_register,r.register_space,offset});
                        if(r.descriptor_count!=~0U)offset+=r.descriptor_count;
                    }
                }else if(p.parameter_type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){parameter.reg=p.u.constants.shader_register;parameter.space=p.u.constants.register_space;parameter.values=p.u.constants.value_count;}
                else {parameter.reg=p.u.descriptor.shader_register;parameter.space=p.u.descriptor.register_space;}
                root.parameters.push_back(std::move(parameter));
            }
            for(unsigned i=0;i<desc->u.v_1_1.static_sampler_count;++i){
                const auto& s=desc->u.v_1_1.static_samplers[i];std::vector<std::uint8_t> sampler(52);
                const auto put=[&](unsigned at,std::uint32_t value){std::memcpy(sampler.data()+at,&value,4);};
                put(0,s.filter);put(4,s.address_u);put(8,s.address_v);put(12,s.address_w);put(16,std::bit_cast<std::uint32_t>(s.mip_lod_bias));put(20,s.max_anisotropy);put(24,s.comparison_func);
                const float border=s.border_colour==VKD3D_SHADER_STATIC_BORDER_COLOUR_TRANSPARENT_BLACK?0.0F:s.border_colour==VKD3D_SHADER_STATIC_BORDER_COLOUR_OPAQUE_BLACK?0.0F:1.0F;
                const float alpha=s.border_colour==VKD3D_SHADER_STATIC_BORDER_COLOUR_TRANSPARENT_BLACK?0.0F:1.0F;
                put(28,std::bit_cast<std::uint32_t>(border));put(32,std::bit_cast<std::uint32_t>(border));put(36,std::bit_cast<std::uint32_t>(border));put(40,std::bit_cast<std::uint32_t>(alpha));
                put(44,std::bit_cast<std::uint32_t>(s.min_lod));put(48,std::bit_cast<std::uint32_t>(s.max_lod));
                root.samplers.push_back({s.shader_register,static_cast<std::uint32_t>(s.shader_visibility),native.graphics_sampler(sampler)});
            }
            vkd3d_shader_free_root_signature(&parsed);if(desc==&converted)vkd3d_shader_free_root_signature(&converted);
            if(!arg(5))return finish(false_ok);
            const auto object=create(Kind::RootSignature,self);bridge.roots[object]=std::move(root);return finish(give(object,arg(4),arg(5)));
        }
        if(member=="CreateGraphicsPipelineState"){
            // D3D12_GRAPHICS_PIPELINE_STATE_DESC（x64，656 bytes）轉為共用後端的 D3D11 狀態格式。
            if(!arg(1))return finish(invalid);
            const auto desc=bytes(arg(1),656);
            const auto word=[&](std::size_t at){std::uint32_t value;std::memcpy(&value,desc.data()+at,4);return value;};
            const auto quad=[&](std::size_t at){std::uint64_t value;std::memcpy(&value,desc.data()+at,8);return value;};
            for(const auto at:{40U,56U,72U})if(quad(at) && quad(at+8)){report("CreateGraphicsPipelineState(GS／HS／DS)");return finish(not_implemented);}
            if(quad(88) || !quad(8) || !quad(16))return finish(invalid);
            D3D12State::Pipeline pipeline;pipeline.root=quad(0);
            static std::atomic<std::uint64_t> pipeline_serials{0};pipeline.serial=pipeline_serials.fetch_add(1,std::memory_order_relaxed)+1;
            try{
                pipeline.shader=native.graphics_shader(0,bytes(quad(8),static_cast<std::size_t>(std::min<std::uint64_t>(quad(16),16U*1024U*1024U))));
                if(quad(24) && quad(32))pipeline.pixel=native.graphics_shader(1,bytes(quad(24),static_cast<std::size_t>(std::min<std::uint64_t>(quad(32),16U*1024U*1024U))));
            }catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=CreateGraphicsPipelineState 失敗：")+error.what());return finish(invalid);}
            // D3D12 render target blend（40 bytes）→ D3D11（32 bytes）。
            pipeline.blend.assign(8+8*32,0);std::memcpy(pipeline.blend.data(),desc.data()+120,8);
            for(unsigned i=0;i<8;++i){const auto from=128+i*40,to=8+i*32;std::memcpy(pipeline.blend.data()+to,desc.data()+from,4);std::memcpy(pipeline.blend.data()+to+4,desc.data()+from+8,24);pipeline.blend[to+28]=desc[from+36];}
            pipeline.sample_mask=word(448);
            // D3D12 rasterizer：ScissorEnable 固定開啟，其後為多重取樣與反鋸齒線。
            pipeline.raster.assign(40,0);std::memcpy(pipeline.raster.data(),desc.data()+452,28);const std::uint32_t scissor=1;std::memcpy(pipeline.raster.data()+28,&scissor,4);std::memcpy(pipeline.raster.data()+32,desc.data()+480,8);
            pipeline.depth.assign(desc.begin()+496,desc.begin()+548);
            std::map<std::uint32_t,std::uint32_t> append;
            for(std::uint32_t i=0;i<word(560);++i){
                const auto at=quad(552)+i*32;GraphicsElement element;element.semantic=[&]{std::string text;for(std::uint64_t n=load(at,64);text.size()<256;++n){const auto c=static_cast<char>(load(n,8));if(!c)break;text.push_back(c);}return text;}();
                element.index=static_cast<std::uint32_t>(load(at+8,32));element.format=static_cast<std::uint32_t>(load(at+12,32));element.slot=static_cast<std::uint32_t>(load(at+16,32));
                element.offset=static_cast<std::uint32_t>(load(at+20,32));element.classification=static_cast<std::uint32_t>(load(at+24,32));element.step=static_cast<std::uint32_t>(load(at+28,32));
                if(element.offset==UINT32_MAX)element.offset=append[element.slot];
                append[element.slot]=element.offset+std::max(1U,graphics_format_bytes(element.format));pipeline.layout.push_back(std::move(element));
            }
            if(!arg(3))return finish(false_ok);
            const auto object=create(Kind::Pipeline,self);bridge.pipelines[object]=std::move(pipeline);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="CreateRenderTargetView" || member=="CreateDepthStencilView"){
            const bool depth=member=="CreateDepthStencilView";auto write=write_descriptor(arg(3));if(!write)return finish(0);auto* const target=&write.value;
            const auto r=bridge.resources.find(arg(1));if(r==bridge.resources.end())return finish(0);
            auto viewed_gpu=[&]{const std::lock_guard lock(bridge.resource_lock(arg(1)));return r->second.gpu;}();if(!viewed_gpu)return finish(0);
            target->type=depth?D3D12State::Descriptor::Type::Depth:D3D12State::Descriptor::Type::Target;target->resource=arg(1);
            // 空描述＝整個資源：陣列取全部切片，3D 取全部 W 切片（mip 0）。
            const auto& viewed=r->second;GraphicsView v{std::move(viewed_gpu)};v.format=viewed.format;v.slices=std::max(1U,viewed.depth);
            if(arg(2)){
                const auto format=static_cast<std::uint32_t>(load(arg(2),32));if(format)v.format=format;const auto dimension=static_cast<std::uint32_t>(load(arg(2)+4,32));
                const auto base=depth?12U:8U;if(depth)v.flags=static_cast<std::uint32_t>(load(arg(2)+8,32))&3U;
                const auto field=[&](unsigned index){return static_cast<std::uint32_t>(load(arg(2)+base+index*4,32));};
                // RTV：1D=2、1D 陣列=3、2D=4、2D 陣列=5、MS=6、MS 陣列=7、3D=8；DSV：1D=1、1D 陣列=2、2D=3、2D 陣列=4、MS=5、MS 陣列=6。
                const unsigned view_kind=depth?dimension+1:dimension;v.slice=0;v.slices=1;
                if(view_kind==2 || view_kind==4)v.level=field(0);
                else if(view_kind==3 || view_kind==5){v.level=field(0);v.slice=field(1);v.slices=field(2);}
                else if(view_kind==7){v.slice=field(0);v.slices=field(1);}
                else if(view_kind==8 && !depth){
                    v.level=field(0);v.slice=field(1);const auto planes=std::max(1U,viewed.depth>>std::min(v.level,31U));
                    v.slices=field(2)==UINT32_MAX?(v.slice<planes?planes-v.slice:0):field(2);
                }
                const auto total=viewed.dimension==4?std::max(1U,viewed.depth>>std::min(v.level,31U)):std::max(1U,viewed.depth);
                if(v.slices==UINT32_MAX)v.slices=v.slice<total?total-v.slice:0;
            }
            target->view=v;return finish(0);
        }
        if(member=="CreateComputePipelineState"){
            if(!arg(1))return finish(invalid);
            const auto root=load(arg(1),64),code=load(arg(1)+8,64),size=load(arg(1)+16,64);
            if(!code || !size || size>16U*1024U*1024U)return finish(invalid);
            std::shared_ptr<GraphicsShader> shader;
            try{shader=native.graphics_shader(5,bytes(code,static_cast<std::size_t>(size)));}
            catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=CreateComputePipelineState 失敗：")+error.what());return finish(invalid);}
            if(!arg(3))return finish(false_ok);
            const auto object=create(Kind::Pipeline,self);D3D12State::Pipeline compute;compute.compute=true;compute.root=root;compute.shader=std::move(shader);bridge.pipelines[object]=std::move(compute);return finish(give(object,arg(2),arg(3)));
        }
        // 建立資源：cpu_base 為 placed 緩衝在 CPU 可見堆積中的位址（0 表示自行配置）。
        // 零複製映射：配置宿主分頁並映入客體（配置粒度 64 KiB）；失敗回傳位址 0，呼叫端改用客體堆積後備。
        const auto map_host_pages=[&](std::uint64_t size)->std::tuple<std::uint64_t,std::shared_ptr<void>,std::span<std::uint8_t>>{
            if(!d3d12_zero_copy_enabled() || !size || size>(UINT64_C(1)<<31))return {};
            const auto rounded=static_cast<std::size_t>((size+0xffffU)&~UINT64_C(0xffff));auto pages=d3d12_host_pages(rounded);if(!pages)return {};
            const std::span<std::uint8_t> bytes(static_cast<std::uint8_t*>(pages.get()),rounded);
            const auto address=map_host_memory(memory,pages,bytes);if(trace_stats_uploads)++sync_reads[address?"zero_copy_maps":"zero_copy_map_failures"];if(!address)return {};
            return {address,std::move(pages),bytes};
        };
        // placed：所屬堆積（零複製時以堆積宿主記憶體的對應範圍建立 GPU 別名）。
        const auto make_resource=[&](std::uint32_t heap_type,std::uint32_t cpu_page,std::uint64_t desc_at,std::uint64_t cpu_base,std::uint64_t cpu_limit,std::uint64_t riid,std::uint64_t out,
                                     const D3D12State::MemoryHeap* placed=nullptr)->std::uint32_t{
            D3D12State::Resource r;r.heap_type=heap_type;r.cpu_page=heap_type==4?cpu_page:0;r.desc=bytes(desc_at,56);
            if(heap_type==4 && (!cpu_page || cpu_page>3))return invalid;
            const auto word=[&](unsigned at){std::uint32_t value;std::memcpy(&value,r.desc.data()+at,4);return value;};
            r.dimension=word(0);std::memcpy(&r.width,r.desc.data()+16,8);r.height=word(24);r.depth=word(28)&0xffffU;r.levels=word(28)>>16;r.format=word(32);r.samples=word(36);r.flags=word(48);
            if(r.heap_type<1 || r.heap_type>4 || r.dimension<1 || r.dimension>4 || !r.width)return invalid;
            if(r.dimension==1){r.height=1;r.depth=1;r.levels=1;r.samples=1;if(r.width>(UINT64_C(1)<<31))return no_memory;}
            if(!r.levels){r.levels=1;for(auto size=std::max<std::uint64_t>({r.width,r.height,r.dimension==4?r.depth:1});size>1;size>>=1)++r.levels;}
            if(!out)return false_ok;
            if(r.dimension==1 && (r.uploads() || r.reads_back())){
                // CPU 可見緩衝：客體記憶體為後備，GPU 使用時再建立影子資源。
                if(cpu_base){
                    if(r.width>cpu_limit)return invalid;r.cpu=cpu_base;r.owned_cpu=false;
                    // 堆積內位移須為宿主分頁倍數（D3D12 緩衝放置對齊 64 KiB）；別名長度補到分頁倍數，不超出堆積映射。
                    if(placed && placed->host && cpu_base>=placed->cpu){
                        const auto offset=cpu_base-placed->cpu;const auto page=static_cast<std::uint64_t>(getpagesize());
                        if(!(offset%page) && offset<placed->host_bytes.size()){
                            const auto length=std::min<std::uint64_t>((r.width+page-1)&~(page-1),placed->host_bytes.size()-offset);
                            if(auto alias=native.graphics_buffer_alias(placed->host,placed->host_bytes.subspan(static_cast<std::size_t>(offset),static_cast<std::size_t>(length)))){r.gpu=std::move(alias);r.host_shared=true;}
                        }
                    }
                }else{
                    if(auto [address,pages,host]=map_host_pages(r.width);address){
                        if(auto alias=native.graphics_buffer_alias(pages,host)){r.cpu=address;r.gpu=std::move(alias);r.host_shared=true;r.host_mapping=host.size();}
                        else try{memory.unmap(GuestAddress{address},host.size());}catch(const std::exception&){}
                    }
                    if(!r.cpu){r.cpu=allocate_heap(memory,r.width);if(!r.cpu)return no_memory;memory.fill(GuestAddress{r.cpu},0,static_cast<std::size_t>(r.width));}
                }
                r.cpu_size=r.width;
            }else if(r.dimension==1){
                GraphicsResourceDesc d;d.dimension=1;d.width=static_cast<std::uint32_t>(r.width);d.bind=0x0c|((r.flags&4)?0x80U:0U);r.gpu=native.graphics_resource(d);
            }else{
                if(r.heap_type!=1 && !(r.heap_type==4 && r.cpu_page==1))return invalid;
                GraphicsResourceDesc d;d.dimension=r.dimension==2?2U:r.dimension==3?3U:4U;d.width=static_cast<std::uint32_t>(r.width);d.height=r.height;
                d.depth=r.dimension==4?r.depth:1;d.layers=r.dimension==4?1:r.depth;d.levels=r.levels;d.format=r.format;d.samples=std::max(1U,r.samples);
                d.bind=((r.flags&8)?0U:8U)|((r.flags&1)?0x20U:0U)|((r.flags&2)?0x40U:0U)|((r.flags&4)?0x80U:0U);
                try{r.gpu=native.graphics_resource(d);}catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=建立資源失敗：")+error.what());return invalid;}
            }
            if(r.dimension==1){r.address=bridge.next_address;bridge.next_address+=(r.width+0xffffU)&~UINT64_C(0xffff);}
            const auto object=create(Kind::Resource,self);if(r.address)bridge.addresses[r.address]=object;bridge.resources[object]=std::move(r);return give(object,riid,out);
        };
        if(member=="CreateCommittedResource" || member=="CreateCommittedResource1" || member=="CreateCommittedResource2" || member=="CreateCommittedResource3"){
            // CreateCommittedResource(heapProps, heapFlags, desc, initialState, clear, riid, out)。
            // 1／2 版多 ProtectedSession；3 版改以 layout 與可轉型格式清單取代初始狀態。
            const unsigned extra=member=="CreateCommittedResource"?0:member=="CreateCommittedResource3"?3:1;
            if(!arg(1) || !arg(3))return finish(invalid);
            return finish(make_resource(static_cast<std::uint32_t>(load(arg(1),32)),static_cast<std::uint32_t>(load(arg(1)+4,32)),arg(3),0,0,arg(6+extra),arg(7+extra)));
        }
        if(member=="CreateHeap" || member=="CreateHeap1"){
            // D3D12_HEAP_DESC：大小、堆積屬性、對齊與旗標（48 bytes）。DEFAULT 堆積的 placed 資源各自配置 GPU 記憶體，不模擬別名。
            const unsigned extra=member=="CreateHeap1"?1:0;if(!arg(1))return finish(invalid);
            D3D12State::MemoryHeap heap;heap.desc=bytes(arg(1),48);std::memcpy(&heap.size,heap.desc.data(),8);std::memcpy(&heap.type,heap.desc.data()+8,4);std::memcpy(&heap.cpu_page,heap.desc.data()+12,4);
            if(!heap.size || heap.type<1 || heap.type>4 || heap.size>(UINT64_C(1)<<31))return finish(heap.size>(UINT64_C(1)<<31)?no_memory:invalid);
            if(!arg(3+extra))return finish(false_ok);
            if(heap.type==2 || heap.type==3 || (heap.type==4 && heap.cpu_page>1)){
                if(auto [address,pages,host]=map_host_pages(heap.size);address){heap.cpu=address;heap.host=std::move(pages);heap.host_bytes=host;}
                else {heap.cpu=allocate_heap(memory,heap.size);if(!heap.cpu)return finish(no_memory);memory.fill(GuestAddress{heap.cpu},0,static_cast<std::size_t>(heap.size));}
            }
            const auto object=create(Kind::Heap,self);bridge.memory_heaps[object]=std::move(heap);return finish(give(object,arg(2+extra),arg(3+extra)));
        }
        if(member=="OpenExistingHeapFromAddress"){
            // 以應用程式 VirtualAlloc（已提交、PAGE_READWRITE）的整段配置作為 CUSTOM 堆積：WRITE_BACK、L0、跨轉接器共用旗標。
            // placed 資源直接讀寫這段客體記憶體（與 CPU 可見堆積相同的同步路徑）；大小取自 VirtualQuery 的同一區段。
            if(arg(3))store(arg(3),64,0);
            if(!arg(1) || (arg(1)&4095U))return finish(invalid);
            const auto info=virtual_memory_information(memory,arg(1));
            if(info.allocation!=arg(1) || info.base!=arg(1) || info.state!=0x1000U || (info.protection&0xffU)!=0x04U || !info.size)return finish(invalid);
            D3D12State::MemoryHeap heap;heap.size=info.size;heap.type=4;heap.cpu_page=3;heap.cpu=arg(1);heap.external=true;heap.desc.assign(48,0);
            const auto put=[&](std::size_t at,std::uint64_t value,unsigned width){for(unsigned b=0;b<width;++b)heap.desc[at+b]=static_cast<std::uint8_t>(value>>(8*b));};
            put(0,heap.size,8);put(8,4,4);put(12,3,4);put(16,1,4);put(20,1,4);put(24,1,4);put(32,65536,8);put(40,0x21,4);
            if(!arg(3))return finish(false_ok);
            const auto object=create(Kind::Heap,self);bridge.memory_heaps[object]=std::move(heap);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="CreatePlacedResource" || member=="CreatePlacedResource1" || member=="CreatePlacedResource2"){
            // CreatePlacedResource(heap, offset, desc, state, clear, riid, out)；2 版多可轉型格式清單。
            const unsigned extra=member=="CreatePlacedResource2"?2:0;
            const auto heap=bridge.memory_heaps.find(arg(1));if(heap==bridge.memory_heaps.end() || !arg(3))return finish(invalid);
            if(arg(2)>=heap->second.size)return finish(invalid);
            return finish(make_resource(heap->second.type,heap->second.cpu_page,arg(3),heap->second.cpu?heap->second.cpu+arg(2):0,heap->second.size-arg(2),arg(6+extra),arg(7+extra),&heap->second));
        }
        if(member=="CreateCommandSignature"){
            if(arg(4))store(arg(4),64,0);
            if(!arg(1) || !arg(3))return finish(invalid);
            D3D12State::CommandSignature signature;signature.stride=static_cast<std::uint32_t>(load(arg(1),32));
            const auto count=load(arg(1)+4,32),descs=load(arg(1)+8,64),node=load(arg(1)+16,32);
            if(!count || count>128 || !descs || !signature.stride || (signature.stride&3) || node>1)return finish(invalid);
            std::uint64_t packed=0;bool root_arguments=false;
            for(std::uint64_t i=0;i<count;++i){
                const auto at=descs+i*16;D3D12State::IndirectArgument a;
                a.type=static_cast<std::uint32_t>(load(at,32));a.index=static_cast<std::uint32_t>(load(at+4,32));
                a.offset=static_cast<std::uint32_t>(load(at+8,32));a.count=static_cast<std::uint32_t>(load(at+12,32));
                if(a.type<=2){if(i+1!=count)return finish(invalid);signature.operation=a.type;a.size=a.type==0?16:a.type==1?20:12;}
                else if(a.type==3){if(a.index>=32)return finish(invalid);a.size=16;}
                else if(a.type==4)a.size=16;
                else if(a.type<=8){
                    root_arguments=true;const auto root=bridge.roots.find(arg(2));
                    if(root==bridge.roots.end() || bridge.objects.at(arg(2)).device!=self || a.index>=root->second.parameters.size())return finish(invalid);
                    const auto& parameter=root->second.parameters[a.index];
                    const auto expected=a.type==5?1U:a.type-4;
                    if(parameter.type!=expected)return finish(invalid);
                    if(a.type==5){if(!a.count || a.offset>parameter.values || a.count>parameter.values-a.offset)return finish(invalid);a.size=a.count*4;}
                    else a.size=8;
                }else return finish(invalid);
                packed+=a.size;signature.arguments.push_back(a);
            }
            if(signature.arguments.back().type>2 || packed>signature.stride || (!root_arguments && arg(2)))return finish(invalid);
            if(signature.operation==2)for(const auto& a:signature.arguments)if(a.type==3 || a.type==4)return finish(invalid);
            signature.root=root_arguments?arg(2):0;
            const auto* abi=d3d12abi::find_interface("ID3D12CommandSignature");
            if(read_guid(memory,arg(3))!=abi->iid && read_guid(memory,arg(3))!=iid_unknown)return finish(no_interface);
            if(!arg(4))return finish(false_ok);
            const auto object=create(Kind::CommandSignature,self);bridge.signatures[object]=std::move(signature);
            return finish(give(object,arg(3),arg(4)));
        }
        if(member=="CreateQueryHeap"){
            if(!arg(1))return finish(invalid);
            D3D12State::QueryHeap heap;heap.type=static_cast<std::uint32_t>(load(arg(1),32));heap.count=static_cast<std::uint32_t>(load(arg(1)+4,32));
            if(!heap.count || heap.count>65536)return finish(invalid);
            if(heap.type!=0 && heap.type!=1 && heap.type!=2){report("CreateQueryHeap(type="+std::to_string(heap.type)+")");return finish(not_implemented);}
            if(!arg(3))return finish(false_ok);
            heap.timestamps.resize(heap.count);heap.futures.resize(heap.count);heap.occlusion.resize(heap.count);
            const auto object=create(Kind::QueryHeap,self);bridge.query_heaps[object]=std::move(heap);return finish(give(object,arg(2),arg(3)));
        }
        if(member=="GetResourceAllocationInfo"){
            // GetResourceAllocationInfo(RetVal, visibleMask, count, descs)。
            std::uint64_t total=0;
            for(std::uint64_t i=0;i<arg(3);++i){const auto d=arg(4)+i*56;const auto width=load(d+16,64),height=load(d+24,32),layers=load(d+28,32)&0xffffU;
                const auto dimension=load(d,32);const auto format=static_cast<std::uint32_t>(load(d+32,32));
                // 依列距（BC 以 4×4 區塊計）估計含 mip 鏈的大小（約為最上層的兩倍）。
                const auto pitch=dimension==1?std::array<std::uint32_t,2>{1U,1U}:graphics_format_pitch(format,static_cast<std::uint32_t>(std::min<std::uint64_t>(width,UINT32_MAX)),static_cast<std::uint32_t>(std::max<std::uint64_t>(height,1)));
                const auto size=dimension==1?width:std::uint64_t{pitch[0]}*pitch[1]*std::max<std::uint64_t>(layers,1)*2;
                total+=(size+0xffffU)&~UINT64_C(0xffff);}
            if(arg(1)){store(arg(1),64,total);store(arg(1)+8,64,0x10000);}return finish(arg(1));
        }
        if(member=="GetCopyableFootprints"){
            // (desc, first, count, base, layouts, rows, row_sizes, total)：列距 256、子資源偏移 512 對齊。
            if(!arg(1))return finish(0);
            const auto d=bytes(arg(1),56);const auto word=[&](unsigned at){std::uint32_t value;std::memcpy(&value,d.data()+at,4);return value;};
            std::uint64_t width;std::memcpy(&width,d.data()+16,8);const auto dimension=word(0),height=dimension==1?1U:word(24),depth_or_layers=dimension==1?1U:(word(28)&0xffffU),format=word(32);
            auto levels=dimension==1?1U:(word(28)>>16);if(!levels){levels=1;for(auto size=std::max<std::uint64_t>({width,height,dimension==4?depth_or_layers:1U});size>1;size>>=1)++levels;}
            std::uint64_t offset=arg(4),total=0;
            for(std::uint32_t i=0;i<u32(3);++i){
                const auto sub=u32(2)+i;const auto level=sub%levels;
                const std::uint64_t w=std::max<std::uint64_t>(1,width>>level);const std::uint32_t h=std::max(1U,height>>level),z=dimension==4?std::max(1U,depth_or_layers>>level):1U;
                const auto pitch=dimension==1?std::array<std::uint32_t,2>{static_cast<std::uint32_t>(w),1U}:graphics_format_pitch(format,static_cast<std::uint32_t>(w),h);
                const auto row=(std::uint64_t{pitch[0]}+255U)&~UINT64_C(255);offset=(offset+511U)&~UINT64_C(511);
                if(arg(5)){const auto at=arg(5)+i*32;store(at,64,offset);store(at+8,32,format);store(at+12,32,w);store(at+16,32,h);store(at+20,32,z);store(at+24,32,row);}
                if(arg(6))store(arg(6)+i*4,32,pitch[1]);if(arg(7))store(arg(7)+i*8,64,pitch[0]);
                const auto size=row*(std::uint64_t{pitch[1]}*z-1)+pitch[0];total=offset+size-arg(4);offset+=row*pitch[1]*z;
            }
            if(arg(8))store(arg(8),64,total);return finish(0);
        }
        // 描述元建立：寫入目的堆積的對應槽。
        if(member=="CreateConstantBufferView"){
            auto write=write_descriptor(arg(2));if(!write || !arg(1)){write.cancel();return finish(0);}auto* const target=&write.value;
            target->type=D3D12State::Descriptor::Type::Constant;target->address=load(arg(1),64);target->size=static_cast<std::uint32_t>(load(arg(1)+8,32));return finish(0);
        }
        if(member=="CreateShaderResourceView" || member=="CreateUnorderedAccessView"){
            const bool uav=member=="CreateUnorderedAccessView";const auto handle=arg(uav?4:3),desc=arg(uav?3:2);
            auto write=write_descriptor(handle);if(!write)return finish(0);auto* const target=&write.value;
            if(!arg(1)){target->type=uav?D3D12State::Descriptor::Type::Unordered:D3D12State::Descriptor::Type::Shader;return finish(0);} // 空描述元。
            const auto r=bridge.resources.find(arg(1));if(r==bridge.resources.end())return finish(0);
            auto& resource=r->second;target->type=uav?D3D12State::Descriptor::Type::Unordered:D3D12State::Descriptor::Type::Shader;target->resource=arg(1);
            if(static const char* const width_text=std::getenv("X64ARM_TRACE_D3D12_DST_WIDTH");width_text && uav && resource.width==std::strtoull(width_text,nullptr,10))
                std::fprintf(stderr,"d3d12_dst UAV 建立 資源=%llx 寬=%llu\n",static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(resource.width));
            GraphicsView v;v.format=resource.format;v.levels=resource.levels;v.slices=resource.dimension==4?1:resource.depth;
            if(desc){
                v.format=static_cast<std::uint32_t>(load(desc,32));if(!v.format)v.format=resource.format;
                const auto view_dimension=static_cast<std::uint32_t>(load(desc+4,32));v.dimension=view_dimension;
                if(view_dimension==1){
                    const auto base=uav?8U:16U;const std::uint64_t first=load(desc+base,64),count=load(desc+base+8,32);const std::uint32_t stride=static_cast<std::uint32_t>(load(desc+base+12,32)),flags=static_cast<std::uint32_t>(load(desc+base+(uav?24:16),32));
                    const auto element=stride?stride:(flags&1)?4U:std::max(1U,graphics_format_bytes(v.format));
                    v.first=static_cast<std::uint32_t>(first*element);v.count=static_cast<std::uint32_t>(count*element);v.flags=flags&1;if(stride)resource.structure=stride;
                    // 結構化／原始緩衝在 SPIR-V 路徑為 uint 紋理緩衝；HLSLcc 路徑以裝置緩衝綁定、不使用格式。
                    if(stride || (flags&1) || v.format==39)v.format=42;
                }else if(!uav){
                    v.level=static_cast<std::uint32_t>(load(desc+16,32));const auto levels=static_cast<std::uint32_t>(load(desc+20,32));v.levels=levels==UINT32_MAX?resource.levels-v.level:std::max(1U,levels);
                    if(view_dimension==3 || view_dimension==5 || view_dimension==10){v.slice=static_cast<std::uint32_t>(load(desc+24,32));v.slices=static_cast<std::uint32_t>(load(desc+28,32));}
                }else{
                    v.level=static_cast<std::uint32_t>(load(desc+8,32));v.levels=1;
                    if(view_dimension==3 || view_dimension==5 || view_dimension==8){v.slice=static_cast<std::uint32_t>(load(desc+12,32));v.slices=static_cast<std::uint32_t>(load(desc+16,32));
                        if(view_dimension==8 && v.slices==UINT32_MAX){const auto depth=std::max(1U,resource.depth>>std::min(v.level,31U));v.slices=v.slice<depth?depth-v.slice:0;}}
                }
            }else if(resource.dimension==1){v.first=0;v.count=static_cast<std::uint32_t>(resource.width);v.levels=1;v.format=42;}
            target->view=v;target->mirror=std::make_shared<D3D12State::MirrorCache>();return finish(0);
        }
        if(member=="CreateSampler"){
            auto write=write_descriptor(arg(2));if(!write || !arg(1)){write.cancel();return finish(0);}auto* const target=&write.value;
            target->type=D3D12State::Descriptor::Type::Sampler;target->sampler=native.graphics_sampler(bytes(arg(1),52));return finish(0);
        }
        // 描述元複製：參數只讀一次；每個範圍只解析一次所屬堆積，之後以索引遞增（同一範圍的描述元必在同一堆積）。
        // 超出堆積的項目與原本逐項查找相同：目的略過、來源視為空描述元。
        const auto copy_entry=[&](D3D12State::Heap* source_heap,std::uint64_t source_index,D3D12State::Heap* dest_heap,std::uint64_t dest_index,bool clear_missing){
            std::optional<D3D12State::Descriptor> value;
            if(source_heap && source_index<source_heap->count){const std::lock_guard lock(bridge.descriptor_lock(source_heap,source_index));value=source_heap->descriptors[source_index];}
            if(!dest_heap || dest_index>=dest_heap->count || (!value && !clear_missing))return;
            D3D12State::Descriptor incoming=value?std::move(*value):D3D12State::Descriptor{};
            {const std::lock_guard lock(bridge.descriptor_lock(dest_heap,dest_index));std::swap(dest_heap->descriptors[dest_index],incoming);}
            D3D12State::mark_dirty(*dest_heap,dest_index);
        };
        if(member=="CopyDescriptorsSimple"){
            const auto count=u32(1);const auto [dest_heap,dest_first]=heap_at(arg(2),false);const auto [source_heap,source_first]=heap_at(arg(3),false);
            for(std::uint32_t i=0;i<count;++i)copy_entry(source_heap,source_first+i,dest_heap,dest_first+i,false);
            return finish(0);
        }
        if(member=="CopyDescriptors"){
            // (dstRanges, dstStarts, dstSizes, srcRanges, srcStarts, srcSizes, type)。D3D12 規定來源與目的範圍不重疊：
            // 兩組範圍同步走訪、逐項直接複製；來源只讀、不標記鏡像。來源不足時其餘目的不變。
            const auto dest_ranges=u32(1),source_ranges=u32(4);const auto dest_starts=arg(2),dest_sizes=arg(3),source_starts=arg(5),source_sizes=arg(6);
            const auto source_size=[&](std::uint32_t range){return source_sizes?load(source_sizes+range*4,32):1;};
            std::uint32_t source_range=0;std::uint64_t source_index=0,source_count=source_ranges?source_size(0):0;
            auto source=source_ranges?heap_at(load(source_starts,64),false):std::pair<D3D12State::Heap*,std::uint64_t>{};
            for(std::uint32_t i=0;i<dest_ranges;++i){
                const auto count=dest_sizes?load(dest_sizes+i*4,32):1;const auto [dest_heap,dest_first]=heap_at(load(dest_starts+i*8,64),false);
                for(std::uint64_t j=0;j<count;++j){
                    while(source_range<source_ranges && source_index>=source_count){
                        if(++source_range<source_ranges){source_index=0;source_count=source_size(source_range);source=heap_at(load(source_starts+source_range*8,64),false);}}
                    if(source_range>=source_ranges)return finish(0);
                    copy_entry(source.first,source.second+source_index,dest_heap,dest_first+j,true);++source_index;
                }
            }
            return finish(0);
        }
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::DescriptorHeap){
        auto& heap=bridge.heaps.at(self);
        if(member=="GetDesc"){if(arg(1)){store(arg(1),32,heap.type);store(arg(1)+4,32,heap.count);store(arg(1)+8,32,heap.visible?1:0);store(arg(1)+12,32,0);}return finish(arg(1));}
        if(member=="GetCPUDescriptorHandleForHeapStart"){if(arg(1))store(arg(1),64,heap.cpu);return finish(arg(1));}
        if(member=="GetGPUDescriptorHandleForHeapStart"){if(arg(1))store(arg(1),64,heap.gpu);return finish(arg(1));}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Resource){
        auto& r=bridge.resources.at(self);
        if(member=="GetGPUVirtualAddress")return finish(r.address);
        if(member=="GetDesc"){if(arg(1))memory.write(GuestAddress{arg(1)},r.desc);return finish(arg(1));}
        if(member=="Map"){
            if(!r.cpu)return finish(invalid);
            // READBACK：映射時取得 GPU 最新內容；零複製資源只需等寫入它的提交完成。
            const auto writes=[&]{const std::lock_guard lock(bridge.resource_lock(self));return std::exchange(r.gpu_writes,nullptr);}();
            if(writes && !writes->load(std::memory_order_acquire)){count_sync("map_wait_gpu_writes");native.graphics_flush(true);}
            if(r.reads_back() && r.gpu)read_back(self);
            if(arg(3))store(arg(3),64,r.cpu);return finish(ok);
        }
        if(member=="Unmap")return finish(0);
        if(member=="GetHeapProperties"){if(arg(1)){store(arg(1),32,r.heap_type);store(arg(1)+4,32,r.cpu_page);store(arg(1)+8,32,r.heap_type==4?1:0);store(arg(1)+12,32,1);store(arg(1)+16,32,1);}if(arg(2))store(arg(2),32,0);return finish(ok);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Fence){
        // worker 上的 GetCompletedValue 只以原子讀取目前值，不套用待完成通知：套用會觸發 fence 事件，而喚醒等待者
        // 必須在協調者完成；協調者在有待完成通知時每 0.25 ms 輪詢套用。
        if(d3d12_worker_context() && member=="GetCompletedValue")
            return finish(std::atomic_ref(bridge.fences.at(self).value).load(std::memory_order_acquire));
        poll_gpu_signals(memory,d3d12_unlocked_call());
        auto& fence=bridge.fences.at(self);
        const auto signal=[&](std::uint64_t value){d3d12_signal_fence(self,value);};
        if(member=="GetCompletedValue")return finish(fence.value);
        if(member=="Signal"){signal(arg(1));return finish(ok);}
        if(member=="SetEventOnCompletion"){
            if(!arg(2)){if(fence.value>=arg(1))return finish(ok);report("SetEventOnCompletion(NULL) 阻塞等候");return finish(fail);}
            fence.events.push_back({arg(1),arg(2)});signal(fence.value);return finish(ok);
        }
        if(member=="GetCreationFlags")return finish(0);
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Allocator){if(member=="Reset")return finish(ok);report(symbol);return finish(not_implemented);}
    if(kind==Kind::PipelineLibrary){
        auto& library=bridge.libraries.at(self);
        const auto text=[&](std::uint64_t at){return read_utf16(memory,GuestAddress{at},32768);};
        if(member=="StorePipeline"){
            // 名稱不可重複；庫持有 PSO 參照直到釋放。
            if(!arg(1) || !arg(2))return finish(invalid);const auto pipeline=bridge.objects.find(arg(2));
            if(pipeline==bridge.objects.end() || pipeline->second.kind!=Kind::Pipeline)return finish(invalid);
            const auto name=text(arg(1));if(library.contains(name))return finish(invalid);
            ++pipeline->second.references;library.emplace(name,arg(2));return finish(ok);
        }
        if(member=="LoadGraphicsPipeline" || member=="LoadComputePipeline" || member=="LoadPipeline"){
            // 找不到名稱回 E_INVALIDARG；找到時依呼叫端描述建立 PSO（與驅動重用已編譯結果相同，回傳等價物件）。
            if(!arg(1) || !arg(2))return finish(invalid);if(arg(4))store(arg(4),64,0);
            if(!library.contains(text(arg(1))))return finish(invalid);
            const auto owner=found->second.device,desc=arg(2),riid=arg(3),out=arg(4);
            state.reg(Gpr::Rcx)=owner;state.reg(Gpr::Rdx)=desc;state.reg(Gpr::R8)=riid;state.reg(Gpr::R9)=out;
            const auto target=member=="LoadGraphicsPipeline"?"Device.CreateGraphicsPipelineState":member=="LoadComputePipeline"?"Device.CreateComputePipelineState":"Device.CreatePipelineState";
            return invoke_d3d12(state,memory,WindowsServiceDescriptor{"internal-d3d12",target,4});
        }
        std::vector<std::uint8_t> serialized(library_magic.begin(),library_magic.end());
        const auto append=[&](std::uint32_t value){for(unsigned b=0;b<4;++b)serialized.push_back(static_cast<std::uint8_t>(value>>(8*b)));};
        append(static_cast<std::uint32_t>(library.size()));
        for(const auto& [name,pipeline]:library){(void)pipeline;append(static_cast<std::uint32_t>(name.size()));for(const auto c:name){serialized.push_back(static_cast<std::uint8_t>(c));serialized.push_back(static_cast<std::uint8_t>(c>>8));}}
        if(member=="GetSerializedSize")return finish(serialized.size());
        if(member=="Serialize"){if(!arg(1) || arg(2)<serialized.size())return finish(invalid);memory.write(GuestAddress{arg(1)},serialized);return finish(ok);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Heap){
        auto& heap=bridge.memory_heaps.at(self);
        if(member=="GetDesc"){if(arg(1))memory.write(GuestAddress{arg(1)},heap.desc);return finish(arg(1));}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::RootSignature || kind==Kind::Pipeline || kind==Kind::QueryHeap){
        if(member=="GetCachedBlob"){if(arg(1))store(arg(1),64,0);return finish(not_implemented);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::List){
        // 錄製只讀描述元（RTV／DSV／UAV 清除在錄製時解析），不標記鏡像待更新：worker 上的錄製不持圖形鎖。
        auto& list=bridge.lists.at(self);
        if(member=="GetType")return finish(list.type);
        struct InvalidTrace {const D3D12State::List& list;bool before;std::string_view member;bool enabled;
            ~InvalidTrace(){if(enabled && !before && list.invalid_recording)std::fprintf(stderr,"[d3d12] 無效錄製：List.%.*s\n",static_cast<int>(member.size()),member.data());}
        } invalid_trace{list,list.invalid_recording,member,trace_d3d12};
        if(member=="Close"){if(!list.open)return finish(fail);list.open=false;return finish(list.invalid_recording?invalid:ok);}
        if(member=="Reset"){list.submitted.reset();list.commands.clear();list.callbacks.clear();list.readback_targets.clear();list.open=true;list.invalid_recording=false;list.allocator=arg(1);if(arg(2))list.commands.push_back({"SetPipelineState",{arg(2)},{},{}});return finish(ok);}
        if(!list.open)return finish(0);
        if(member=="ExecuteBundle"){
            const auto bundle=bridge.lists.find(arg(1));
            if(list.type!=0 || bundle==bridge.lists.end() || bundle->second.type!=1 || bundle->second.open || bundle->second.invalid_recording || bridge.objects.at(arg(1)).device!=found->second.device){list.invalid_recording=true;return finish(0);}
        }
        if(list.type==1 && member=="SetPredication"){list.invalid_recording=true;return finish(0);}
        if(list.type==1 && member=="ExecuteIndirect"){
            const auto signature=bridge.signatures.find(arg(1));
            if(arg(5) || signature==bridge.signatures.end() || signature->second.arguments.size()!=1 || signature->second.operation==2){list.invalid_recording=true;return finish(0);}
        }
        // 錄製：保留原始參數與需要立即讀取的客體資料。
        D3D12State::Command command;command.op=member;command.values.reserve(service.arguments);
        for(unsigned i=1;i<service.arguments;++i)command.values.push_back(arg(i));
        if(member=="SetComputeRoot32BitConstants" || member=="SetGraphicsRoot32BitConstants")command.data=bytes(arg(3),std::size_t{u32(2)}*4);
        else if(member=="SetDescriptorHeaps")for(std::uint32_t i=0;i<u32(1);++i)command.values.push_back(load(arg(2)+i*8,64));
        else if(member=="CopyTextureRegion"){command.data=bytes(arg(1),48);const auto src=bytes(arg(5),48);command.data.insert(command.data.end(),src.begin(),src.end());if(arg(6)){const auto box=bytes(arg(6),24);command.data.insert(command.data.end(),box.begin(),box.end());}}
        else if(member=="WriteBufferImmediate"){
            // D3D12_WRITEBUFFERIMMEDIATE_PARAMETER 陣列（GPU 位址＋32 位元值，16 位元組一筆）在錄製時複製；
            // 模式陣列（DEFAULT／MARKER_IN／MARKER_OUT）在依序重播下語義相同，不另保存。
            if(!u32(1) || u32(1)>65536 || !arg(2)){list.invalid_recording=true;return finish(0);}
            command.data=bytes(arg(2),std::size_t{u32(1)}*16);
        }
        else if(member=="ClearUnorderedAccessViewUint" || member=="ClearUnorderedAccessViewFloat"){
            const auto entry=read_descriptor(arg(2));
            if(!entry || entry->type!=D3D12State::Descriptor::Type::Unordered || entry->resource!=arg(3) || u32(5)>65536 || (u32(5) && !arg(6))){list.invalid_recording=true;return finish(0);}
            command.views.push_back(entry->view);command.data=bytes(arg(4),16);
            if(u32(5)){const auto rects=bytes(arg(6),std::size_t{u32(5)}*16);command.data.insert(command.data.end(),rects.begin(),rects.end());}
        }
        else if(member=="IASetVertexBuffers"){if(arg(3))command.data=bytes(arg(3),std::size_t{u32(2)}*16);}
        else if(member=="IASetIndexBuffer"){if(arg(1))command.data=bytes(arg(1),16);}
        else if(member=="RSSetViewports")command.data=bytes(arg(2),std::size_t{std::min(u32(1),16U)}*24);
        else if(member=="RSSetScissorRects")command.data=bytes(arg(2),std::size_t{std::min(u32(1),16U)}*16);
        else if(member=="OMSetBlendFactor"){if(arg(1))command.data=bytes(arg(1),16);else{const float one=1.0F;command.data.resize(16);for(unsigned i=0;i<4;++i)std::memcpy(command.data.data()+i*4,&one,4);}}
        else if(member=="OMSetRenderTargets"){
            // (count, handles, single_range, dsv)：單一範圍時 handles 指向第一個控制代碼。
            const auto count=std::min(u32(1),8U);command.data={static_cast<std::uint8_t>(count),static_cast<std::uint8_t>(arg(4)?1:0)};
            for(std::uint32_t i=0;i<count;++i){const auto handle=u32(3)?load(arg(2),64)+i*descriptor_size:load(arg(2)+i*8,64);const auto entry=read_descriptor(handle);command.views.push_back(entry?entry->view:GraphicsView{});}
            if(arg(4)){const auto entry=read_descriptor(load(arg(4),64));command.views.push_back(entry?entry->view:GraphicsView{});}
        }
        else if(member=="ClearRenderTargetView"){const auto entry=read_descriptor(arg(1));command.views.push_back(entry?entry->view:GraphicsView{});command.data=bytes(arg(2),16);}
        else if(member=="ClearDepthStencilView"){
            // x64：Depth 為第 4 個參數，以 XMM3 傳遞。
            const auto entry=read_descriptor(arg(1));command.views.push_back(entry?entry->view:GraphicsView{});command.data.assign(state.xmm[3].begin(),state.xmm[3].begin()+4);
        }
        // 寫入 CPU 讀回資源的複製與查詢解析：記下目的，提交排入時標上序號，讓 Map 等到該批重播完成。
        {
            std::uint64_t destination{};
            if(member=="CopyBufferRegion" || member=="CopyResource")destination=arg(1);
            else if(member=="CopyTextureRegion")destination=load(arg(1),64);
            else if(member=="ResolveQueryData")destination=arg(5);
            if(destination)if(const auto target=bridge.resources.find(destination);target!=bridge.resources.end() && target->second.reads_back())list.readback_targets.push_back(destination);
        }
        list.commands.push_back(std::move(command));return finish(0);
    }
    if(kind==Kind::Queue){
        if(member=="GetTimestampFrequency"){
            // 與 EndQuery(TIMESTAMP) 使用同一時基：後端支援 GPU 時戳時取其頻率，否則為宿主奈秒。
            if(!arg(1))return finish(pointer_error);std::uint64_t frequency=1000000000;
            try{if(native.graphics_timestamp_supported())frequency=native.graphics_timestamp().frequency;}catch(const std::exception&){}
            store(arg(1),64,frequency);return finish(ok);
        }
        if(member=="GetDesc"){if(arg(1)){store(arg(1),32,bridge.queues.at(self));store(arg(1)+4,32,0);store(arg(1)+8,32,0);store(arg(1)+12,32,0);}return finish(arg(1));}
        if(member=="Wait")return finish(ok);
        if(member=="Signal"){
            const auto fence=bridge.fences.find(arg(1));if(fence==bridge.fences.end())return finish(invalid);
            // 非同步提交：排在前面的重播之後，由提交執行緒掛上完成旗標並只登記通知（由客體端輪詢套用、觸發事件）。
            if(!submit_job && d3d12_submitter_){
                D3D12Submitter::Job job;job.arguments={self,arg(1),arg(2),0};job.method=&d3d12_signal_method;
                d3d12_enqueue(*d3d12_submitter_,std::move(job),false);return finish(ok);
            }
            // 佇列仍有未完成的提交（或待讀回、或更早的待完成訊號）：登記，GPU 完成後才生效，維持訊號順序。
            auto& queue_state=bridge.queue_states[self];
            const bool outstanding=(queue_state.done && !queue_state.done->load(std::memory_order_acquire)) || !queue_state.readbacks.empty();
            // 非同步時間戳的補正寫入：未就緒者保留暫定值（命令緩衝失敗等情況）。
            const auto apply_timestamps=[](GuestMemory& target,const std::vector<D3D12State::TimestampWrite>& writes){
                for(const auto& write:writes)for(std::size_t i=0;i<write.futures.size();++i)
                    if(const auto& future=write.futures[i];future && future->done.load(std::memory_order_acquire))
                        try{target.store(GuestAddress{write.address+i*8},64,future->value.ticks);}catch(const MemoryFault&){}
            };
            if(outstanding || !gpu_signals_.empty() || submit_job){
                auto readbacks=std::move(queue_state.readbacks);queue_state.readbacks.clear();
                auto timestamps=std::move(queue_state.timestamp_writes);queue_state.timestamp_writes.clear();
                queue_gpu_signal(memory,queue_state.done,bridge.objects.at(self).device,[this,readbacks=std::move(readbacks),timestamps=std::move(timestamps),apply_timestamps,fence=arg(1),value=arg(2)](GuestMemory& target){
                    apply_timestamps(target,timestamps);
                    if(!d3d12_)return;auto& state=*d3d12_;
                    // 先讀回 GPU 寫入的 READBACK 資源，再讓 fence 達到門檻（客體看到完成時資料已就緒）。
                    for(const auto object:readbacks){
                        const auto found=state.resources.find(object);if(found==state.resources.end() || !found->second.cpu || !found->second.gpu || found->second.host_shared)continue;
                        const auto owner=state.objects.find(object);if(owner==state.objects.end())continue;const auto device=state.devices.find(owner->second.device);if(device==state.devices.end())continue;
                        try{
                            const auto data=device->second.native->graphics_read(GraphicsView{found->second.gpu});auto& r=found->second;
                            target.write(GuestAddress{r.cpu},std::span<const std::uint8_t>(data).first(std::min<std::size_t>(data.size(),static_cast<std::size_t>(r.width))));
                        }catch(const std::exception& error){runtime_.module_events.push_back(std::string("d3d12_note=非同步讀回失敗：")+error.what());}
                    }
                    d3d12_signal_fence(fence,value);
                },!submit_job);
                return finish(ok);
            }
            apply_timestamps(memory,queue_state.timestamp_writes);queue_state.timestamp_writes.clear();
            d3d12_signal_fence(arg(1),arg(2));return finish(ok);
        }
        if(member=="ExecuteCommandLists"){
            // 命令名稱比對用 string_view 字面值：長度在編譯期已知，長度不同直接判否（原本每次比對都先算字面長度，重播取樣約 8%）。
            using namespace std::string_view_literals;
            // 客體呼叫：取各命令清單的提交快照；有提交執行緒時排入後即返回（不取圖形鎖，可在 worker 上執行），
            // 由提交執行緒以同一方法重播（submit_job）。沒有提交執行緒時照舊在呼叫端重播。
            std::vector<std::shared_ptr<const D3D12State::Program>> programs;
            if(submit_job)programs=std::move(submit_job->programs);
            else for(std::uint32_t l=0;l<u32(1);++l){
                const auto submitted_list=bridge.lists.find(load(arg(2)+l*8,64));if(submitted_list==bridge.lists.end())continue;auto& list=submitted_list->second;
                if(!list.submitted){
                    list.submitted=std::make_shared<const D3D12State::Program>(D3D12State::Program{list.type,std::move(list.commands),std::move(list.callbacks),std::move(list.readback_targets)});
                    list.commands.clear();list.callbacks.clear();list.readback_targets.clear();
                }
                programs.push_back(list.submitted);
            }
            if(!submit_job && d3d12_submitter_){
                D3D12Submitter::Job job;job.arguments={self,0,0,0};job.method=&d3d12_execute_method;job.programs=std::move(programs);
                std::vector<std::uint64_t> readbacks;for(const auto& program:job.programs)readbacks.insert(readbacks.end(),program->readback_targets.begin(),program->readback_targets.end());
                if(graphics_guard.owns_lock())graphics_guard.unlock(); // 佇列滿時等提交執行緒，它需要圖形鎖。
                const auto ticket=d3d12_enqueue(*d3d12_submitter_,std::move(job),true);
                for(const auto object:readbacks)if(const auto target=bridge.resources.find(object);target!=bridge.resources.end()){
                    const std::lock_guard lock(bridge.resource_lock(object));target->second.submit_ticket=std::max(target->second.submit_ticket,ticket);}
                return finish(0);
            }
            std::set<std::uint64_t> written;std::string failed_op;
            // 根常數暫存區：每段 256 位元組對齊（與 CBV 位移相同），每段只寫一次再綁定，直接寫入共享記憶體。原本每次繪製
            // 都為每個 32 位元常數參數新建一個 Metal 緩衝（核心配置＋清零）再上傳。暫存區跟著繪製被後端保留到 GPU 完成。
            constexpr std::uint32_t constant_arena_size=64U<<10;std::shared_ptr<GraphicsResource> constant_arena;std::uint32_t constant_arena_used=constant_arena_size;
            try{
                for(const auto& program:programs){
                    // 提交執行緒在命令清單之間讓出圖形鎖給等待者（協調者本地的 D3D12 方法、Map 等），每次最多讓 0.2 ms：
                    // 一次提交常含多條清單、重播數毫秒，連續持鎖會讓協調者與其他 worker 一起等。清單之間不保留重播快取。
                    if(submit_job && program!=programs.front() && graphics_waiters().load(std::memory_order_relaxed) && graphics_guard.owns_lock()){
                        graphics_guard.unlock();
                        const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(200);
                        while(graphics_waiters().load(std::memory_order_relaxed) && std::chrono::steady_clock::now()<until)std::this_thread::yield();
                        lock_graphics(graphics_guard);
                    }
                    // 重播狀態：運算與繪圖各自的根簽章與根參數（D3D12 兩者獨立），以及管線、IA／RS／OM 狀態。
                    // 根參數依索引存放：D3D12 根簽章最多 64 個 DWORD，參數數不超過 64；超出的索引在設定時略過（原本每次綁定都查 std::map）。
                    struct Roots {std::uint64_t root{};std::array<std::uint64_t,64> tables{},views{};std::array<std::vector<std::uint8_t>,64> constants;};
                    Roots compute_roots,graphics_roots;std::uint64_t pipeline=0;GraphicsDraw draw;std::uint32_t target_count=0;
                    // 繪製直接重用重播狀態 draw（不再每次整份複製約 62 KB 的 GraphicsDraw 再解構）：VS／PS stage 只清除上次寫入的槽位，
                    // 管線決定的 layout／blend 等只在管線切換時更新（draw_pipeline），每次繪製的參數於繪製前重設。
                    struct StageExtent {std::size_t constants{},resources{},unordered{},samplers{};};
                    std::array<StageExtent,2> draw_extents;std::uint64_t draw_pipeline=0;
                    const auto clear_stage=[](GraphicsStage& stage,StageExtent& extent){
                        stage.shader.reset();stage.tables.clear();
                        for(std::size_t i=0;i<extent.constants;++i){stage.constants[i].reset();stage.constant_offsets[i]=0;}
                        std::fill_n(stage.resources.begin(),extent.resources,GraphicsView{});std::fill_n(stage.unordered.begin(),extent.unordered,GraphicsView{});
                        for(std::size_t i=0;i<extent.samplers;++i)stage.samplers[i].reset();
                        extent={};
                    };
                    std::uint64_t occlusion_heap=0;std::uint32_t occlusion_index=0;constexpr std::uint32_t occlusion_slots=4096;
                    // 重播期間資源表與位址索引不增刪：每個描述元表項目的資源查找改用小型直接對應快取（原本是千筆級 std::map 的
                    // 樹狀查找、以快取未命中為主）；GPU 位址另記最近解析的資源範圍（常數緩衝多來自少數環狀緩衝）。
                    std::array<std::pair<std::uint64_t,D3D12State::Resource*>,1024> resource_slots{};
                    const auto resource_of=[&](std::uint64_t object)->D3D12State::Resource*{
                        auto& slot=resource_slots[((object>>4)^(object>>14))&1023U];
                        if(slot.second && slot.first==object)return slot.second;
                        const auto found=bridge.resources.find(object);if(found==bridge.resources.end())return nullptr;
                        slot={object,&found->second};return slot.second;
                    };
                    struct RecentRange {std::uint64_t begin{},end{},object{};};std::array<RecentRange,8> recent_ranges{};unsigned recent_next=0;
                    const auto replay_resource_at=[&](std::uint64_t address)->std::pair<std::uint64_t,std::uint64_t>{
                        for(const auto& hit:recent_ranges)if(hit.object && address>=hit.begin && address<hit.end)return {hit.object,address-hit.begin};
                        const auto [object,offset]=resource_at(address);
                        if(object)if(const auto* r=resource_of(object))recent_ranges[recent_next++%recent_ranges.size()]={r->address,r->address+std::max<std::uint64_t>(r->width,1),object};
                        return {object,offset};
                    };
                    // CBV 位址依 D3D12 規範 256 位元組對齊：綁定上層緩衝並設定位移，只上傳用到的範圍，不再為子範圍另建緩衝。
                    const auto constant_view=[&](std::uint64_t address,std::uint32_t size)->std::pair<std::shared_ptr<GraphicsResource>,std::uint32_t>{
                        const auto [object,offset]=replay_resource_at(address);if(!object || offset>UINT32_MAX)return {};auto* resource=resource_of(object);
                        if(!resource)throw std::out_of_range("CBV 資源不存在");if(!resource->cpu && !resource->gpu)return {};
                        auto view=gpu_resource_of(*resource,object,offset,size?size:UINT64_MAX,"cbv");
                        // 診斷（X64ARM_TRACE_D3D12_CBV_HEAP=<最小>-<最大>）：寬度落在範圍內的 CBV 資源各列一次堆積類型與後端資源，追查常數由 CPU 或 GPU 寫入。
                        if(static const char* const range=std::getenv("X64ARM_TRACE_D3D12_CBV_HEAP");range){
                            static const auto bounds=[]{const char* text=std::getenv("X64ARM_TRACE_D3D12_CBV_HEAP");char* end=nullptr;const auto low=std::strtoull(text,&end,10);return std::pair{low,*end=='-'?std::strtoull(end+1,nullptr,10):low};}();
                            thread_local std::set<std::uint64_t> logged;
                            if(resource->width>=bounds.first && resource->width<=bounds.second && logged.insert(object).second)
                                std::fprintf(stderr,"d3d12_cbv 物件=%llx 寬=%llu 位移=%llu heap=%u cpu=%d host_shared=%d 後端=%p\n",static_cast<unsigned long long>(object),static_cast<unsigned long long>(resource->width),
                                    static_cast<unsigned long long>(offset),resource->heap_type,resource->cpu?1:0,resource->host_shared?1:0,static_cast<const void*>(view.get()));
                        }
                        return {std::move(view),static_cast<std::uint32_t>(offset)};
                    };
                    // 描述元表鏡像：把待更新的描述元換成後端的資源 ID／緩衝位址，寫入 GPU 鏡像。
                    const auto refresh_mirror=[&](D3D12State::Heap& heap){
                        if(heap.dirty.empty() || (!std::atomic_ref<bool>(heap.any_dirty).load(std::memory_order_seq_cst) && heap.mirrors[0]))return;
                        std::atomic_ref<bool>(heap.any_dirty).store(false,std::memory_order_seq_cst);
                        for(unsigned m=0;m<2;++m)if(!heap.mirrors[m]){
                            GraphicsResourceDesc d;d.dimension=1;d.width=heap.count*(m?16U:8U);d.bind=0x8c;heap.mirrors[m]=native.graphics_resource(d);heap.shadow[m].assign(d.width,0);}
                        if(heap.keep.size()<heap.count)heap.keep.resize(heap.count);
                        const auto put=[](std::vector<std::uint8_t>& out,std::size_t at,std::uint64_t value){for(unsigned b=0;b<8;++b)out[at+b]=static_cast<std::uint8_t>(value>>(8*b));};
                        thread_local std::vector<std::uint32_t> pending;pending.clear();
                        // 先取摘要再取區塊：寫入方依區塊→摘要→any_dirty 設定，這次漏掉的位元必定留在摘要與 any_dirty，下次再處理。
                        for(std::size_t group=0;group<heap.dirty_summary.size();++group){
                            if(!std::atomic_ref<std::uint64_t>(heap.dirty_summary[group]).load(std::memory_order_relaxed))continue;
                            for(auto chunks=std::atomic_ref<std::uint64_t>(heap.dirty_summary[group]).exchange(0,std::memory_order_seq_cst);chunks;chunks&=chunks-1){
                                const auto chunk=group*64+static_cast<std::size_t>(std::countr_zero(chunks));
                                for(auto bits=std::atomic_ref<std::uint64_t>(heap.dirty_chunks[chunk]).exchange(0,std::memory_order_seq_cst);bits;bits&=bits-1){
                                    const auto i=static_cast<std::uint32_t>(chunk*64+static_cast<std::size_t>(std::countr_zero(bits)));
                                    if(std::atomic_ref<std::uint8_t>(heap.dirty[i]).exchange(0,std::memory_order_seq_cst))pending.push_back(i);
                                }
                            }
                        }
                        if(device.pool){
                            // 前端只解析資源（需要 bridge 表格），描述元值計算與鏡像寫入依序交給編碼執行緒。
                            struct Update {std::uint32_t index;bool uav;GraphicsView view;std::shared_ptr<D3D12State::MirrorCache> cache;};
                            auto updates=std::make_shared<std::vector<Update>>();updates->reserve(pending.size());
                            for(const auto i:pending){
                                const auto entry=[&]{const std::lock_guard lock(bridge.descriptor_lock(&heap,i));return heap.descriptors[i];}();
                                const bool uav=entry.type==D3D12State::Descriptor::Type::Unordered;GraphicsView view;
                                auto* const live=entry.resource?resource_of(entry.resource):nullptr;
                                if((entry.type==D3D12State::Descriptor::Type::Shader || uav) && live){
                                    auto& resource=*live;view=entry.view;view.resource=resource.dimension==1?gpu_resource_of(resource,entry.resource,entry.view.first,entry.view.count?entry.view.count:UINT64_MAX,"mirror"):resource.gpu;}
                                updates->push_back({i,uav,std::move(view),entry.mirror});
                            }
                            if(!heap.posted_keep)heap.posted_keep=std::make_shared<std::vector<std::shared_ptr<void>>>(heap.count);
                            native.graphics_post([updates,keeps=heap.posted_keep,textures=heap.mirrors[0],buffers=heap.mirrors[1]](NativeGraphics& backend){
                                static const bool use_cache=[] {const auto* text=std::getenv("X64ARM_D3D12_MIRROR_CACHE");return !text || std::string_view(text)!="0";}();
                                for(auto& update:*updates){
                                    std::array<std::uint64_t,2> texture{},buffer{};std::shared_ptr<void> keep,unused;
                                    auto* const cache=use_cache && update.view.resource?update.cache.get():nullptr;
                                    if(cache && cache->valid && cache->resource==update.view.resource.get() && cache->uav==update.uav){texture[0]=cache->texture;buffer=cache->buffer;keep=cache->keep;}
                                    else{
                                        if(!backend.graphics_descriptor(update.view,update.uav?7U:6U,texture,keep))backend.graphics_descriptor(GraphicsView{},6,texture,keep);
                                        if(!backend.graphics_descriptor(update.view,update.uav?9U:8U,buffer,unused))buffer={};
                                        if(cache){cache->resource=update.view.resource.get();cache->uav=update.uav;cache->texture=texture[0];cache->buffer=buffer;cache->keep=keep;cache->valid=true;}
                                    }
                                    const std::uint64_t buffer_entry[2]{buffer[0],buffer[1]&UINT32_MAX};
                                    GraphicsView at{textures};at.first=update.index*8U;at.count=8;
                                    backend.graphics_write_unused(at,std::span(reinterpret_cast<const std::uint8_t*>(&texture[0]),8));
                                    at=GraphicsView{buffers};at.first=update.index*16U;at.count=16;
                                    backend.graphics_write_unused(at,std::span(reinterpret_cast<const std::uint8_t*>(buffer_entry),16));
                                    (*keeps)[update.index]=std::move(keep);
                                }
                            });
                            return;
                        }
                        for(const auto i:pending){
                            const auto entry=[&]{const std::lock_guard lock(bridge.descriptor_lock(&heap,i));return heap.descriptors[i];}();
                            const bool uav=entry.type==D3D12State::Descriptor::Type::Unordered;GraphicsView view;
                            // 描述元可指向已釋放的資源（GPU 未存取即合法）：視為空描述元，不中止整批重播。
                            auto* const live=entry.resource?resource_of(entry.resource):nullptr;
                            if((entry.type==D3D12State::Descriptor::Type::Shader || uav) && live){
                                auto& resource=*live;view=entry.view;view.resource=resource.dimension==1?gpu_resource_of(resource,entry.resource,entry.view.first,entry.view.count?entry.view.count:UINT64_MAX,"mirror"):resource.gpu;}
                            std::array<std::uint64_t,2> texture{},buffer{};std::shared_ptr<void> keep,unused;
                            if(!native.graphics_descriptor(view,uav?7U:6U,texture,keep))native.graphics_descriptor(GraphicsView{},6,texture,keep);
                            if(!native.graphics_descriptor(view,uav?9U:8U,buffer,unused))buffer={};
                            put(heap.shadow[0],std::size_t{i}*8,texture[0]);put(heap.shadow[1],std::size_t{i}*16,buffer[0]);put(heap.shadow[1],std::size_t{i}*16+8,buffer[1]&UINT32_MAX);
                            heap.keep[i]=std::move(keep);
                        }
                        // 只寫回變動項目所在的連續區段（間隔不超過 256 項的變動合併為一段），不再每次整份上傳兩份鏡像。
                        const auto write_run=[&](std::uint32_t begin,std::uint32_t end){
                            // 被改寫的描述元依 D3D12 規範不會被在途命令引用（見影子緩衝上傳），鏡像直接寫入，與實體驅動的描述元堆積相同。
                            for(unsigned m=0;m<2;++m){const std::uint32_t stride=m?16U:8U;GraphicsView view{heap.mirrors[m]};view.first=begin*stride;view.count=(end-begin)*stride;
                                const auto bytes=std::span<const std::uint8_t>(heap.shadow[m]).subspan(view.first,view.count);
                                if(d3d12_direct_uploads())native.graphics_write_unused(view,bytes);else native.graphics_write(view,bytes,view.count,view.count);}
                        };
                        std::uint32_t run_begin=0,run_end=0;
                        for(const auto i:pending){
                            if(run_end && i<=run_end+256){run_end=std::max(run_end,i+1);continue;}
                            if(run_end)write_run(run_begin,run_end);
                            run_begin=i;run_end=i+1;
                        }
                        if(run_end)write_run(run_begin,run_end);
                    };
                    // 描述元表：依著色器的 (類型, space, register) 找到根簽章中涵蓋它的描述元表範圍，換算成鏡像位置。
                    // visibility：目前綁定階段的 D3D12_SHADER_VISIBILITY（VS＝1、PS＝5）；0 表示 compute，所有根參數皆可見。
                    const auto visible=[](std::uint32_t parameter,std::uint32_t stage){return !stage || !parameter || parameter==stage;};
                    const auto bind_tables=[&](GraphicsStage& stage,Roots& roots,const D3D12State::RootSignature& signature,std::uint32_t visibility){
                        stage.tables.clear();if(!stage.shader)return;
                        for(const auto& b:stage.shader->bindings){
                            if(b.type<6 || b.type>9)continue;
                            const std::uint32_t range_type=b.type==6 || b.type==8?VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;bool bound=false;
                            for(std::uint32_t index=0;index<signature.parameters.size() && !bound;++index){
                                const auto& parameter=signature.parameters[index];
                                if(parameter.type!=VKD3D_SHADER_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !visible(parameter.visibility,visibility))continue;
                                const auto base=index<roots.tables.size()?roots.tables[index]:0;if(!base)continue;
                                for(const auto& range:parameter.ranges){
                                    if(range.type!=range_type || range.space!=b.space || b.slot<range.base || (range.count!=~0U && b.slot-range.base>=range.count))continue;
                                    if(const auto [found_heap,start]=heap_at(base,true);found_heap && !found_heap->dirty.empty()){auto& heap=*found_heap;(void)start;
                                        const auto first=(base-heap.gpu)/descriptor_size+range.offset+(b.slot-range.base);
                                        if(first>=heap.count)throw std::invalid_argument("描述元表超出堆積範圍");
                                        refresh_mirror(heap);const bool buffer_table=b.type>=8;
                                        GraphicsDescriptorTable table{b.type,b.space,b.slot,heap.mirrors[buffer_table?1:0],first*(buffer_table?16U:8U),{}};
                                        // 有界小表（含超出 u7／8 張讀寫紋理而改走表的離散 UAV）：逐項解析資源，讓後端宣告使用並追蹤寫入。
                                        if(b.count && b.count<=16)for(std::uint64_t k=0;k<b.count && first+k<heap.count;++k){
                                            const auto& entry=heap.descriptors[first+k];
                                            if((entry.type!=D3D12State::Descriptor::Type::Shader && entry.type!=D3D12State::Descriptor::Type::Unordered) || !entry.resource)continue;
                                            auto* const live=resource_of(entry.resource);if(!live)continue;
                                            auto& resource=*live;auto view=entry.view;view.resource=resource.dimension==1?gpu_resource_of(resource,entry.resource,entry.view.first,entry.view.count?entry.view.count:UINT64_MAX,"mirror"):resource.gpu;
                                            table.views.push_back(view);if(entry.type==D3D12State::Descriptor::Type::Unordered)written.insert(entry.resource);
                                        }
                                        stage.tables.push_back(std::move(table));bound=true;break;
                                    }
                                    if(bound)break;
                                }
                            }
                            if(!bound)throw std::invalid_argument("著色器描述元表在根簽章中沒有對應範圍");
                        }
                    };
                    // 根參數 → 共用後端的 b／t／u／s 暫存器槽（register space 0）；非零 space 與大陣列走描述元表。
                    // extent 記錄 bind 寫入過的槽位上限（不含），重用繪製狀態時只清除這段範圍。
                    const auto bind=[&](GraphicsStage& stage,Roots& roots,const D3D12State::RootSignature& signature,std::uint32_t visibility,StageExtent* extent=nullptr){
                        const auto touch=[&](std::size_t StageExtent::*field,std::size_t reg){if(extent)extent->*field=std::max(extent->*field,reg+1);};
                        bind_tables(stage,roots,signature,visibility);
                        // 只解析著色器實際讀取的暫存器：根簽章的描述元表常宣告 t0–t127 等大範圍，著色器通常只用其中數個
                        // （電馭判客 Benchmark 每次繪製原本逐項解析約 530 項，GPU 只忙 35～50%）。依著色器綁定建立 b／t／s／u
                        // 使用位元：種類 0＝b、1／3＝t、2＝s、4／5＝u（UAV 重新配置時 source 為原暫存器），6～9 為描述元表模式
                        // 由 bind_tables 綁定鏡像。未知種類或暫存器超出 128 時全部解析；X64ARM_D3D12_BIND_ALL=1 恢復全部解析。
                        struct Used {std::array<std::uint64_t,2> b{},t{},s{},u{};bool all{};} used;used.all=d3d12_bind_all();
                        if(stage.shader && !used.all)for(const auto& binding:stage.shader->bindings){
                            const auto mark=[&](std::array<std::uint64_t,2>& mask,std::uint32_t reg){if(reg<128)mask[reg>>6]|=std::uint64_t{1}<<(reg&63U);else used.all=true;};
                            switch(binding.type){
                                case 0:mark(used.b,binding.slot);break;
                                case 1:case 3:mark(used.t,binding.slot);break;
                                case 2:mark(used.s,binding.slot);break;
                                case 4:case 5:mark(used.u,binding.source!=UINT32_MAX?binding.source:binding.slot);break;
                                case 6:case 7:case 8:case 9:break;
                                default:used.all=true;
                            }
                        }
                        const auto needs=[&](const std::array<std::uint64_t,2>& mask,std::uint32_t reg){return used.all || (reg<128 && ((mask[reg>>6]>>(reg&63U))&1U));};
                        // 超出 u7 的離散 UAV 緩衝：著色器已改用未占用的 slot（source 為原始暫存器），全部參數處理完再放入，避免被其他暫存器覆寫。
                        std::map<std::uint32_t,std::uint32_t> remapped;std::vector<std::pair<std::uint32_t,GraphicsView>> late;
                        if(stage.shader)for(const auto& b:stage.shader->bindings)if(b.source!=UINT32_MAX)remapped.emplace(b.source,b.slot);
                        const auto place_unordered=[&](std::uint32_t reg,const GraphicsView& view){
                            if(const auto found=remapped.find(reg);found!=remapped.end())late.emplace_back(found->second,view);
                            else if(reg<stage.unordered.size()){stage.unordered[reg]=view;touch(&StageExtent::unordered,reg);}
                        };
                        for(std::uint32_t index=0;index<signature.parameters.size();++index){
                            const auto& parameter=signature.parameters[index];
                            if(!visible(parameter.visibility,visibility))continue;
                            if(parameter.space)throw std::invalid_argument("根參數使用非零 register space");
                            if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){
                                if(parameter.reg>=stage.constants.size() || !needs(used.b,parameter.reg))continue;
                                const auto& values=index<roots.constants.size()?roots.constants[index]:std::vector<std::uint8_t>{};
                                const auto size=(std::max<std::size_t>(std::size_t{parameter.values}*4,values.size())+15U)&~std::size_t{15};
                                if(size>constant_arena_size)throw std::invalid_argument("根常數超過暫存區大小");
                                if(constant_arena_used+size>constant_arena_size){
                                    GraphicsResourceDesc d;d.dimension=1;d.width=constant_arena_size;d.bind=4;constant_arena=native.graphics_resource(d);constant_arena_used=0;
                                }
                                thread_local std::vector<std::uint8_t> padded;padded.assign(size,0);std::copy(values.begin(),values.end(),padded.begin());
                                GraphicsView slot{constant_arena};slot.first=constant_arena_used;slot.count=static_cast<std::uint32_t>(size);native.graphics_write_unused(slot,padded);
                                stage.constants[parameter.reg]=constant_arena;stage.constant_offsets[parameter.reg]=constant_arena_used;touch(&StageExtent::constants,parameter.reg);
                                constant_arena_used+=static_cast<std::uint32_t>((size+255U)&~std::size_t{255});
                            }else if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_CBV){
                                if(const auto address=index<roots.views.size()?roots.views[index]:0;address && parameter.reg<stage.constants.size() && needs(used.b,parameter.reg)){const auto [buffer,at]=constant_view(address,0);stage.constants[parameter.reg]=buffer;stage.constant_offsets[parameter.reg]=at;touch(&StageExtent::constants,parameter.reg);}
                            }else if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_SRV || parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_UAV){
                                if(!needs(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_UAV?used.u:used.t,parameter.reg))continue;
                                const auto [object,offset]=replay_resource_at(index<roots.views.size()?roots.views[index]:0);if(!object)continue;auto* const found_resource=resource_of(object);
                                if(!found_resource)throw std::out_of_range("根參數資源不存在");auto& resource=*found_resource;
                                GraphicsView view{gpu_resource_of(resource,object,offset,UINT64_MAX,"root_view")};view.first=static_cast<std::uint32_t>(offset);view.count=static_cast<std::uint32_t>(resource.width-offset);view.format=42;
                                if(parameter.type==VKD3D_SHADER_ROOT_PARAMETER_TYPE_UAV){place_unordered(parameter.reg,view);written.insert(object);}
                                else if(parameter.reg<stage.resources.size()){stage.resources[parameter.reg]=view;touch(&StageExtent::resources,parameter.reg);}
                            }else{
                                const auto base=index<roots.tables.size()?roots.tables[index]:0;if(!base)continue;
                                for(const auto& range:parameter.ranges){
                                    if(range.space)continue;
                                    // 共用後端最多 128 個 SRV 槽；更大的陣列只能經描述元表存取。
                                    const auto count=range.count==~0U?1U:std::min<std::uint32_t>(range.count,128);
                                    const auto& mask=range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER?used.s:range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV?used.b:
                                        range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV?used.u:used.t;
                                    // 範圍與著色器使用位元沒有交集就不查堆積（多數範圍屬於其他階段或其他著色器）。
                                    std::array<std::uint64_t,2> wanted{};
                                    if(!used.all)for(unsigned w=0;w<2;++w){
                                        const std::uint64_t low=std::max<std::uint64_t>(range.base,w*64U),high=std::min<std::uint64_t>(std::uint64_t{range.base}+count,(w+1)*64U);
                                        if(low>=high)continue;
                                        const auto from=low-w*64U,to=high-w*64U;
                                        wanted[w]=mask[w]&(to>=64?~std::uint64_t{0}:(std::uint64_t{1}<<to)-1U)&~((std::uint64_t{1}<<from)-1U);
                                    }
                                    if(!used.all && !wanted[0] && !wanted[1])continue;
                                    // 範圍內的描述元屬於同一個堆積：只查一次堆積，逐項以索引取用（超出堆積的項目視為空）。
                                    const auto [range_heap,range_first]=heap_at(base+std::uint64_t{range.offset}*descriptor_size,true);if(!range_heap)continue;
                                    std::uint64_t visited=0;
                                    // 回傳 false 表示已超出堆積，範圍內其後的項目都視為空。
                                    const auto visit=[&](std::uint32_t i)->bool{
                                        if(range_first+i>=range_heap->count)return false;
                                        ++visited;const auto entry=&range_heap->descriptors[range_first+i];const auto reg=range.base+i;
                                        if(range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER){if(reg<stage.samplers.size()){stage.samplers[reg]=entry->sampler;touch(&StageExtent::samplers,reg);}return true;}
                                        if(range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV){if(reg<stage.constants.size() && entry->address){const auto [buffer,at]=constant_view(entry->address,entry->size);stage.constants[reg]=buffer;stage.constant_offsets[reg]=at;touch(&StageExtent::constants,reg);}return true;}
                                        if(!entry->resource)return true;auto view=entry->view;auto* const found_resource=resource_of(entry->resource);if(!found_resource)throw std::out_of_range("描述元表資源不存在");
                                        auto& resource=*found_resource;view.resource=resource.dimension==1?gpu_resource_of(resource,entry->resource,entry->view.first,entry->view.count?entry->view.count:UINT64_MAX,"table"):resource.gpu;
                                        if(range.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV){place_unordered(reg,view);written.insert(entry->resource);}
                                        else if(reg<stage.resources.size()){stage.resources[reg]=view;touch(&StageExtent::resources,reg);}
                                        return true;
                                    };
                                    if(used.all){for(std::uint32_t i=0;i<count;++i)if(!visit(i))break;}
                                    else for(unsigned w=0;w<2;++w){
                                        bool more=true;
                                        for(auto bits=wanted[w];bits && more;bits&=bits-1)more=visit(static_cast<std::uint32_t>(w*64U+static_cast<unsigned>(std::countr_zero(bits))-range.base));
                                        if(!more)break;
                                    }
                                    if(D3D12Submitter::tracing() && d3d12_submitter_)d3d12_submitter_->stats.table_entries.fetch_add(visited,std::memory_order_relaxed);
                                }
                            }
                        }
                        for(const auto& [slot,view]:late){stage.unordered[slot]=view;touch(&StageExtent::unordered,slot);}
                        for(const auto& sampler:signature.samplers)if(visible(sampler.visibility,visibility) && sampler.reg<stage.samplers.size()){stage.samplers[sampler.reg]=sampler.sampler;touch(&StageExtent::samplers,sampler.reg);}
                    };
                    const auto signature_of=[&](const Roots& roots)->const D3D12State::RootSignature&{
                        const auto p=bridge.pipelines.find(pipeline);const auto r=bridge.roots.find(roots.root?roots.root:p!=bridge.pipelines.end()?p->second.root:0);
                        if(r==bridge.roots.end())throw std::invalid_argument("缺少根簽章");return r->second;
                    };
                    const auto read_buffer=[&](std::uint64_t object,std::uint64_t offset,std::uint64_t length){
                        const auto& r=bridge.resources.at(object);
                        if(r.dimension!=1 || offset>r.width || length>r.width-offset)throw std::invalid_argument("ExecuteIndirect 緩衝範圍無效");
                        if(r.cpu)return bytes(r.cpu+offset,static_cast<std::size_t>(length));
                        count_sync("indirect_or_predicate");native.graphics_flush(true);auto data=native.graphics_read(GraphicsView{r.gpu});
                        if(offset>data.size() || length>data.size()-offset)throw std::invalid_argument("ExecuteIndirect GPU 回讀範圍無效");
                        return std::vector<std::uint8_t>(data.begin()+static_cast<std::ptrdiff_t>(offset),data.begin()+static_cast<std::ptrdiff_t>(offset+length));
                    };
                    bool predicate_skip=false,in_bundle=false;
                    // 間接命令在執行時讀取，前面的 GPU 寫入／複製因此可產生命令與計數。
                    // 錄製的命令直接依序讀取；bundle 與間接命令展開的命令放 pending，排在目前位置之前執行（不整份複製命令清單）。
                    std::deque<D3D12State::Command> pending;const auto& recorded=program->commands;std::size_t cursor=0;D3D12State::Command held;
                    // 提交執行緒在清單內每 32 個命令檢查等待者：有人等圖形鎖（協調者的資源建立／釋放、DXGI 查詢、worker 的持鎖方法）
                    // 就讓出至多 0.2 ms。讓出期間表格可能增刪、物件位址可能被重用，恢復後清除資源指標與位址快取；
                    // 仍在使用的資源、堆積與管線依 D3D12 規範在 GPU 完成前不會釋放。
                    unsigned since_yield=0;
                    const auto yield_graphics=[&]{
                        if(!submit_job || ++since_yield<32 || !graphics_waiters().load(std::memory_order_relaxed) || !graphics_guard.owns_lock())return;
                        since_yield=0;graphics_guard.unlock();
                        const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(200);
                        while(graphics_waiters().load(std::memory_order_relaxed) && std::chrono::steady_clock::now()<until)std::this_thread::yield();
                        lock_graphics(graphics_guard);
                        resource_slots.fill({});recent_ranges.fill({});uploaded.clear();
                    };
                    while(!pending.empty() || cursor<recorded.size()){
                        yield_graphics();
                        const D3D12State::Command* current=&held;
                        if(!pending.empty()){held=std::move(pending.front());pending.pop_front();}else current=&recorded[cursor++];
                        const auto& c=*current;if(trace_d3d12)failed_op=std::string(c.op);
                        const auto& v=c.values;const bool graphics=c.op.find("Graphics")!=std::string::npos;auto& roots=graphics?graphics_roots:compute_roots;
                        const auto ends=[&](std::string_view suffix){return c.op.ends_with(suffix);};
                        if(predicate_skip && (c.op=="DrawInstanced"sv || c.op=="DrawIndexedInstanced"sv || c.op=="Dispatch"sv ||
                            c.op=="CopyTextureRegion"sv || c.op=="CopyBufferRegion"sv || c.op=="CopyResource"sv || c.op=="CopyTiles"sv ||
                            c.op=="ResolveSubresource"sv || c.op=="ClearDepthStencilView"sv || c.op=="ClearRenderTargetView"sv ||
                            c.op=="ClearUnorderedAccessViewUint"sv || c.op=="ClearUnorderedAccessViewFloat"sv || c.op=="ExecuteIndirect"sv))continue;
                        if(c.op=="BundleEnd"sv)in_bundle=false;
                        else if(c.op=="ExecuteBundle"sv){
                            const auto& bundle=bridge.lists.at(v[0]);
                            if(in_bundle || program->type!=0 || bundle.type!=1 || bundle.open || bundle.invalid_recording)throw std::invalid_argument("ExecuteBundle 目標無效");
                            pipeline=0;draw.topology=0;in_bundle=true;
                            pending.push_front({"BundleEnd",{},{},{}});
                            for(auto i=bundle.commands.rbegin();i!=bundle.commands.rend();++i)pending.push_front(*i);
                        }
                        else if(c.op=="SetPredication"sv){
                            predicate_skip=false;
                            if(v[0]){
                                // Operation 是 32 位元列舉：暫存器高 32 位元不保證為 0，只取低位。
                                const auto operation=static_cast<std::uint32_t>(v[2]);
                                if((v[1]&7) || operation>1 || program->type==1)throw std::invalid_argument("SetPredication 參數無效");
                                const auto data=read_buffer(v[0],v[1],8);std::uint64_t value;std::memcpy(&value,data.data(),8);
                                // 在 SetPredication 執行當下快照 64 位值，後續緩衝變動不改變判斷。
                                predicate_skip=operation==0?value==0:value!=0;
                            }
                        }
                        else if(c.op=="ExecuteIndirect"sv){
                            const auto& signature=bridge.signatures.at(v[0]);
                            if(program->type!=0 && !(program->type==2 && signature.operation==2))throw std::invalid_argument("ExecuteIndirect 命令清單種類不符");
                            if(in_bundle && (v[4] || signature.arguments.size()!=1 || signature.operation==2))throw std::invalid_argument("Bundle 間接命令不允許修改繫結或使用計數緩衝");
                            const auto& active=signature.operation==2?compute_roots:graphics_roots;
                            if(signature.root && active.root!=signature.root)throw std::invalid_argument("ExecuteIndirect 根簽章不符");
                            if((v[3]&3) || (v[5]&3))throw std::invalid_argument("ExecuteIndirect 位移未對齊");
                            const auto& argument_resource=bridge.resources.at(v[2]);
                            if(argument_resource.dimension!=1 || v[3]>argument_resource.width || std::uint64_t{static_cast<std::uint32_t>(v[1])}*signature.stride>argument_resource.width-v[3])throw std::invalid_argument("ExecuteIndirect 最大命令範圍無效");
                            if(const auto& only=signature.arguments.front();!v[4] && signature.arguments.size()==1 && only.type<=2 && !argument_resource.cpu &&
                               v[3]+std::uint64_t{static_cast<std::uint32_t>(v[1])}*signature.stride<=UINT32_MAX){
                                // 參數留在 GPU：簽章只有一個 Draw／DrawIndexed／Dispatch 且沒有計數緩衝時，D3D12 與 Metal 的間接參數排列相同，
                                // 直接展開成 GPU 間接命令，不必等前面的 GPU 工作完成再讀回（否則每次 ExecuteIndirect 都讓 CPU 與 GPU 互等）。
                                const char* op=only.type==2?"DispatchIndirectGpu":only.type==1?"DrawIndexedIndirectGpu":"DrawIndirectGpu";
                                for(auto i=static_cast<std::uint32_t>(v[1]);i-->0;)pending.push_front({op,{v[2],v[3]+std::uint64_t{i}*signature.stride},{},{}});
                                continue;
                            }
                            auto count=static_cast<std::uint32_t>(v[1]);
                            if(v[4]){const auto data=read_buffer(v[4],v[5],4);std::uint32_t limit;std::memcpy(&limit,data.data(),4);count=std::min(count,limit);}
                            const auto data=read_buffer(v[2],v[3],std::uint64_t{count}*signature.stride);
                            if(trace_d3d12 && count && !signature.arguments.empty()){
                                // 只在同一參數位置的值改變時輸出，避免每幀重複的固定參數占滿上限。
                                static std::map<std::pair<std::uint64_t,std::uint64_t>,std::array<std::uint32_t,3>> last;watched_arguments().insert(v[2]);
                                const auto& a=signature.arguments.back();std::array<std::uint32_t,3> w{};std::memcpy(w.data(),data.data()+(signature.stride-a.size),std::min<std::size_t>(12,a.size));
                                const auto [seen,added]=last.try_emplace({v[2],v[3]},w);
                                if(added || seen->second!=w)seen->second=w,trace("[d3d12] ExecuteIndirect op=%u 參數=%llx+%llu 計數=%llx 筆數=%u/%u 末參數型別=%u 值=%u,%u,%u\n",signature.operation,
                                    static_cast<unsigned long long>(v[2]),static_cast<unsigned long long>(v[3]),static_cast<unsigned long long>(v[4]),count,static_cast<std::uint32_t>(v[1]),a.type,w[0],w[1],w[2]);
                            }
                            const bool compute_signature=signature.operation==2;
                            std::vector<D3D12State::Command> expanded;
                            const auto decode=[&](const D3D12State::IndirectArgument& a,const std::uint8_t* data,bool reset){
                                D3D12State::Command next;const auto word=[&](unsigned at){std::uint32_t n=0;if(!reset)std::memcpy(&n,data+at,4);return n;};
                                if(a.type<=2){if(reset)return;next.op=a.type==0?"DrawInstanced":a.type==1?"DrawIndexedInstanced":"Dispatch";for(unsigned at=0;at<a.size;at+=4)next.values.push_back(word(at));}
                                else if(a.type==3 || a.type==4){next.op=a.type==3?"IASetVertexBuffers":"IASetIndexBuffer";next.values={a.index,1,0};next.data.resize(16);if(!reset)std::memcpy(next.data.data(),data,16);}
                                else if(a.type==5){next.op=compute_signature?"SetComputeRoot32BitConstants"sv:"SetGraphicsRoot32BitConstants"sv;next.values={a.index,a.count,0,a.offset};next.data.resize(a.size);if(!reset)std::memcpy(next.data.data(),data,a.size);}
                                else{next.op=compute_signature?(a.type==6?"SetComputeRootConstantBufferView"sv:a.type==7?"SetComputeRootShaderResourceView"sv:"SetComputeRootUnorderedAccessView"sv):
                                    (a.type==6?"SetGraphicsRootConstantBufferView"sv:a.type==7?"SetGraphicsRootShaderResourceView"sv:"SetGraphicsRootUnorderedAccessView"sv);std::uint64_t address=0;if(!reset)std::memcpy(&address,data,8);next.values={a.index,address};}
                                expanded.push_back(std::move(next));
                            };
                            for(std::uint32_t i=0;i<count;++i){std::size_t offset=std::size_t{i}*signature.stride;for(const auto& a:signature.arguments){decode(a,data.data()+offset,false);offset+=a.size;}}
                            // 契約要求被簽章修改的 IA／根參數在呼叫後歸零，未涉及的狀態維持原值。
                            for(const auto& a:signature.arguments)decode(a,nullptr,true);
                            for(auto i=expanded.rbegin();i!=expanded.rend();++i)pending.push_front(std::move(*i));
                        }
                        else if(c.op=="SetPipelineState"sv)pipeline=v[0];
                        else if(ends("RootSignature"))roots.root=v[0];
                        else if(ends("RootDescriptorTable")){if(static_cast<std::uint32_t>(v[0])<roots.tables.size())roots.tables[static_cast<std::uint32_t>(v[0])]=v[1];}
                        else if(ends("RootConstantBufferView") || ends("RootShaderResourceView") || ends("RootUnorderedAccessView")){if(static_cast<std::uint32_t>(v[0])<roots.views.size())roots.views[static_cast<std::uint32_t>(v[0])]=v[1];}
                        else if(ends("Root32BitConstant") && static_cast<std::uint32_t>(v[0])>=roots.constants.size()){}
                        else if(ends("Root32BitConstants") && static_cast<std::uint32_t>(v[0])>=roots.constants.size()){}
                        else if(ends("Root32BitConstant")){auto& data=roots.constants[static_cast<std::uint32_t>(v[0])];const auto offset=static_cast<std::uint32_t>(v[2])*4;if(data.size()<offset+4)data.resize(offset+4);const auto value=static_cast<std::uint32_t>(v[1]);std::memcpy(data.data()+offset,&value,4);}
                        else if(ends("Root32BitConstants")){auto& data=roots.constants[static_cast<std::uint32_t>(v[0])];const auto offset=static_cast<std::uint32_t>(v[3])*4;if(data.size()<offset+c.data.size())data.resize(offset+c.data.size());std::memcpy(data.data()+offset,c.data.data(),c.data.size());}
                        else if(c.op=="IASetPrimitiveTopology"sv)draw.topology=static_cast<std::uint32_t>(v[0]);
                        else if(c.op=="IASetVertexBuffers"sv){
                            const auto start_slot=static_cast<std::uint32_t>(v[0]); // 32 位元 StartSlot，忽略暫存器高位元。
                            for(std::size_t i=0;i*16<c.data.size() && start_slot+i<draw.vertices.size();++i){
                                std::uint64_t location;std::uint32_t size,stride;std::memcpy(&location,c.data.data()+i*16,8);std::memcpy(&size,c.data.data()+i*16+8,4);std::memcpy(&stride,c.data.data()+i*16+12,4);
                                const auto [object,offset]=resource_at(location);
                                draw.vertices[start_slot+i]=object?GraphicsVertexBuffer{gpu_resource(object,offset,size,"vertex"),stride,static_cast<std::uint32_t>(offset)}:GraphicsVertexBuffer{};
                            }
                        }
                        else if(c.op=="IASetIndexBuffer"sv){
                            if(c.data.size()<16){draw.indices.reset();continue;}
                            std::uint64_t location;std::uint32_t size,format;std::memcpy(&location,c.data.data(),8);std::memcpy(&size,c.data.data()+8,4);std::memcpy(&format,c.data.data()+12,4);
                            const auto [object,offset]=resource_at(location);draw.indices=object?gpu_resource(object,offset,size,"index"):nullptr;draw.index_offset=static_cast<std::uint32_t>(offset);draw.index_format=format;
                        }
                        else if(c.op=="RSSetViewports"sv){draw.viewports.clear();for(std::size_t i=0;i+24<=c.data.size();i+=24){std::array<float,6> viewport;std::memcpy(viewport.data(),c.data.data()+i,24);draw.viewports.push_back(viewport);}}
                        else if(c.op=="RSSetScissorRects"sv){draw.scissors.clear();for(std::size_t i=0;i+16<=c.data.size();i+=16){std::array<std::int32_t,4> rect;std::memcpy(rect.data(),c.data.data()+i,16);draw.scissors.push_back(rect);}}
                        else if(c.op=="OMSetBlendFactor"sv)std::memcpy(draw.blend_factor.data(),c.data.data(),16);
                        else if(c.op=="OMSetStencilRef"sv)draw.stencil_reference=static_cast<std::uint32_t>(v[0]);
                        else if(c.op=="OMSetRenderTargets"sv){
                            target_count=c.data[0];for(auto& target:draw.targets)target={};
                            for(std::uint32_t i=0;i<target_count;++i)draw.targets[i]=c.views[i];
                            draw.depth=c.data[1]?c.views[target_count]:GraphicsView{};
                        }
                        else if(c.op=="ClearUnorderedAccessViewUint"sv || c.op=="ClearUnorderedAccessViewFloat"sv){
                            auto view=c.views.at(0);view.resource=gpu_resource(v[2]);std::array<std::uint32_t,4> values;std::memcpy(values.data(),c.data.data(),16);
                            if(watch(c.op,v[2]))trace("[d3d12] 監看 %s 資源=%llx first=%u count=%u format=%u flags=%u 值=%u 矩形=%llu\n",std::string(c.op).c_str(),static_cast<unsigned long long>(v[2]),view.first,view.count,view.format,view.flags,values[0],static_cast<unsigned long long>(v[4]));
                            const bool floating=c.op.ends_with("Float");const auto& desc=view.resource->desc;
                            if(D3D12Submitter::tracing() && d3d12_submitter_){
                                auto& stats=d3d12_submitter_->stats;stats.uav_clears.fetch_add(1,std::memory_order_relaxed);
                                std::uint64_t element=4;try{element=std::max(1U,graphics_format_bytes(view.format?view.format:desc.format));}catch(const std::invalid_argument&){}
                                stats.uav_clear_bytes.fetch_add(desc.dimension==1?view.count:std::uint64_t{std::max(1U,desc.width>>view.level)}*std::max(1U,desc.height>>view.level)*
                                    (desc.dimension==4?view.slices:std::max(1U,view.slices))*element,std::memory_order_relaxed);
                            }
                            // NumRects 是 32 位元參數：暫存器高位元可能殘留垃圾，誤判成有矩形會整個略過清除。
                            if(!static_cast<std::uint32_t>(v[4])){
                                if(!native.graphics_clear_unordered(view,values,floating))throw std::invalid_argument("UAV 清除值與格式不相容");
                            }else{
                                // 共用 Metal 清除與複製：矩形只影響指定區域，其餘像素保持原值。
                                GraphicsView cleared=view;
                                if(desc.dimension!=1){cleared.resource=native.graphics_resource(desc);if(!native.graphics_clear_unordered(cleared,values,floating))throw std::invalid_argument("UAV 清除格式不相容");}
                                const auto element=std::max(1U,graphics_format_bytes(view.format?view.format:desc.format));
                                const auto width=desc.dimension==1?view.count/element:std::max(1U,desc.width>>view.level),height=desc.dimension==1?1U:std::max(1U,desc.height>>view.level);
                                for(std::size_t at=16;at<c.data.size();at+=16){
                                    std::array<std::int32_t,4> rect;std::memcpy(rect.data(),c.data.data()+at,16);
                                    const auto x=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[0],0,width)),y=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[1],0,height));
                                    const auto right=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[2],0,width)),bottom=static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect[3],0,height));
                                    if(right<=x || bottom<=y)continue;
                                    if(desc.dimension==1){auto region=view;region.first+=x*element;region.count=(right-x)*element;if(!native.graphics_clear_unordered(region,values,floating))throw std::invalid_argument("UAV 清除格式不相容");}
                                    else if(desc.dimension==4)native.graphics_copy(view,cleared,{x,y,view.slice},{x,y,view.slice,right,bottom,view.slice+view.slices});
                                    else for(std::uint32_t slice=0;slice<view.slices;++slice){auto to=view,from=cleared;to.slice+=slice;from.slice+=slice;native.graphics_copy(to,from,{x,y,0},{x,y,0,right,bottom,1});}
                                }
                            }
                            written.insert(v[2]);
                        }
                        else if(c.op=="ClearRenderTargetView"sv){std::array<float,4> color;std::memcpy(color.data(),c.data.data(),16);if(c.views[0].resource)native.graphics_clear(c.views[0],color,0);}
                        else if(c.op=="ClearDepthStencilView"sv){
                            float depth;std::memcpy(&depth,c.data.data(),4);const std::array<float,4> values{depth,static_cast<float>(v[3]&0xffU),0,0};
                            if(c.views[0].resource)native.graphics_clear(c.views[0],values,static_cast<std::uint32_t>(v[1]));
                        }
                        else if(c.op=="DrawInstanced"sv || c.op=="DrawIndexedInstanced"sv || c.op=="DrawIndirectGpu"sv || c.op=="DrawIndexedIndirectGpu"sv){
                            const auto p=bridge.pipelines.find(pipeline);if(p==bridge.pipelines.end() || p->second.compute)throw std::invalid_argument("Draw 缺少繪圖管線");
                            const bool timing=D3D12Submitter::tracing() && d3d12_submitter_;const auto t0=timing?D3D12Submitter::now_ns():0;
                            const auto& signature=signature_of(graphics_roots);auto& d=draw;
                            for(unsigned i=0;i<2;++i)clear_stage(d.stages[i],draw_extents[i]);
                            d.stages[0].shader=p->second.shader;bind(d.stages[0],graphics_roots,signature,1,&draw_extents[0]);
                            const auto t1=timing?D3D12Submitter::now_ns():0;
                            if(p->second.pixel){d.stages[1].shader=p->second.pixel;bind(d.stages[1],graphics_roots,signature,5,&draw_extents[1]);}
                            const auto t2=timing?D3D12Submitter::now_ns():0;
                            if(draw_pipeline!=pipeline){d.layout=p->second.layout;d.blend=p->second.blend;d.depth_state=p->second.depth;d.raster=p->second.raster;d.sample_mask=p->second.sample_mask;d.pipeline_serial=p->second.serial;draw_pipeline=pipeline;}
                            d.indirect.reset();d.indirect_offset=0;d.count=0;d.instances=1;d.start=0;d.base_vertex=0;d.start_instance=0;d.visibility.reset();d.visibility_offset=0;
                            const bool indexed=c.op=="DrawIndexedInstanced"sv || c.op=="DrawIndexedIndirectGpu"sv;d.indexed=indexed;
                            if(c.op.ends_with("IndirectGpu")){d.indirect=gpu_resource(v[0]);d.indirect_offset=static_cast<std::uint32_t>(v[1]);}
                            else {
                                d.count=static_cast<std::uint32_t>(v[0]);d.instances=static_cast<std::uint32_t>(v[1]);d.start=static_cast<std::uint32_t>(v[2]);
                                if(indexed){d.base_vertex=static_cast<std::int32_t>(static_cast<std::uint32_t>(v[3]));d.start_instance=static_cast<std::uint32_t>(v[4]);}else d.start_instance=static_cast<std::uint32_t>(v[3]);
                            }
                            if(occlusion_heap){
                                auto& occlusion=bridge.query_heaps.at(occlusion_heap).occlusion[occlusion_index];
                                if(occlusion.slots>=occlusion_slots)throw std::invalid_argument("單一遮蔽查詢內的繪製次數超過上限");
                                d.visibility=occlusion.buffer;d.visibility_offset=occlusion.slots++*8;
                            }
                            if(timing)d3d12_submitter_->stats.draws.fetch_add(1,std::memory_order_relaxed);
                            // 診斷（關鍵路徑敏感度）：X64ARM_DEBUG_FRONTEND_DELAY_NS=<奈秒> 每次繪製在前端忙等，看 fps 對前端成本的敏感程度。
                            if(static const auto delay=[] {const auto* text=std::getenv("X64ARM_DEBUG_FRONTEND_DELAY_NS");return text?std::strtoull(text,nullptr,10):0ULL;}();delay){
                                const auto until=std::chrono::steady_clock::now()+std::chrono::nanoseconds(delay);while(std::chrono::steady_clock::now()<until){}
                            }
                            const auto t3=timing?D3D12Submitter::now_ns():0;std::uint64_t t4=t3;
                            const auto pool_wait=device.pool?device.pool->wait_ns:0;
                            if(device.pool){auto& slot=device.pool->fill(native,d);if(timing)t4=D3D12Submitter::now_ns();native.graphics_draw_retained(slot.draw);slot.ticket=native.graphics_ticket();}
                            else native.graphics_draw(d);
                            if(timing){
                                auto& stats=d3d12_submitter_->stats;const auto t5=D3D12Submitter::now_ns();
                                stats.draw_vs.fetch_add(t1-t0,std::memory_order_relaxed);stats.draw_ps.fetch_add(t2-t1,std::memory_order_relaxed);stats.draw_setup.fetch_add(t3-t2,std::memory_order_relaxed);
                                const auto waited=device.pool?device.pool->wait_ns-pool_wait:0;
                                stats.draw_fill.fetch_add(t4-t3-std::min(waited,t4-t3),std::memory_order_relaxed);stats.draw_fill_wait.fetch_add(waited,std::memory_order_relaxed);stats.draw_post.fetch_add(t5-t4,std::memory_order_relaxed);
                            }
                        }
                        else if(c.op=="BeginQuery"sv || c.op=="EndQuery"sv){
                            const auto heap=bridge.query_heaps.find(v[0]);const auto type=static_cast<std::uint32_t>(v[1]),index=static_cast<std::uint32_t>(v[2]);
                            if(heap==bridge.query_heaps.end() || index>=heap->second.count)throw std::invalid_argument("查詢堆積或索引無效");
                            if(type==2){
                                // TIMESTAMP：取樣當下 GPU 時戳（與 GetTimestampFrequency 同一時基）。
                                // GPU 時戳以非同步取樣編進目前命令緩衝，不等 GPU 完成（同步取樣每次都讓 CPU 與 GPU 完全序列化）。
                                if(native.graphics_timestamp_supported())heap->second.futures[index]=native.graphics_timestamp_future();
                                else {heap->second.futures[index].reset();heap->second.timestamps[index]=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
                            }else if(c.op=="BeginQuery"sv){
                                auto& occlusion=heap->second.occlusion[index];
                                GraphicsResourceDesc d;d.dimension=1;d.width=occlusion_slots*8;d.bind=0x80;occlusion.buffer=native.graphics_resource(d);
                                native.graphics_write(GraphicsView{occlusion.buffer},std::vector<std::uint8_t>(d.width),d.width,d.width);occlusion.slots=0;
                                occlusion_heap=v[0];occlusion_index=index;
                            }else if(occlusion_heap==v[0] && occlusion_index==index)occlusion_heap=0;
                        }
                        else if(c.op=="ResolveQueryData"sv){
                            // (heap, type, start, count, destination, offset)：每個結果 8 bytes。
                            const auto heap=bridge.query_heaps.find(v[0]);const auto type=static_cast<std::uint32_t>(v[1]),start=static_cast<std::uint32_t>(v[2]),count=static_cast<std::uint32_t>(v[3]);
                            if(heap==bridge.query_heaps.end() || std::uint64_t{start}+count>heap->second.count)throw std::invalid_argument("ResolveQueryData 範圍無效");
                            std::vector<std::uint8_t> data(std::size_t{count}*8);bool unresolved=false;
                            for(std::uint32_t i=0;i<count;++i){
                                std::uint64_t value=0;
                                if(type==2){
                                    // 已就緒的非同步時戳寫回快取；未就緒先用上次的值，CPU 端目的地於 Signal 生效前補正。
                                    auto& future=heap->second.futures[start+i];
                                    if(future && future->done.load(std::memory_order_acquire)){heap->second.timestamps[start+i]=future->value.ticks;future.reset();}
                                    unresolved=unresolved || future;value=heap->second.timestamps[start+i];
                                }
                                else if(const auto& occlusion=heap->second.occlusion[start+i];occlusion.buffer){
                                    count_sync("resolve_occlusion");const auto samples=native.graphics_read(GraphicsView{occlusion.buffer});
                                    for(std::uint32_t slot=0;slot<occlusion.slots;++slot){std::uint64_t part;std::memcpy(&part,samples.data()+slot*8,8);value+=part;}
                                    if(type==1)value=value?1:0;
                                }
                                std::memcpy(data.data()+i*8,&value,8);
                            }
                            auto& destination=bridge.resources.at(v[4]);
                            if(destination.cpu && unresolved){
                                D3D12State::TimestampWrite write;write.address=destination.cpu+v[5];
                                write.futures.assign(heap->second.futures.begin()+start,heap->second.futures.begin()+start+count);
                                bridge.queue_states[self].timestamp_writes.push_back(std::move(write));
                            }
                            if(destination.cpu)memory.write(GuestAddress{destination.cpu+v[5]},data);
                            else native.graphics_write_region(GraphicsView{destination.gpu},data,static_cast<std::uint32_t>(data.size()),static_cast<std::uint32_t>(data.size()),{static_cast<std::uint32_t>(v[5]),0,0,static_cast<std::uint32_t>(v[5]+data.size()),1,1});
                        }
                        else if(c.op=="Callback"sv){
                            // 建立在 D3D12 上的 API（DirectML）：以同一共用後端與資源對應執行；寫入的資源視同 UAV 寫入。
                            program->callbacks.at(static_cast<std::size_t>(v[0]))(native,[&](std::uint64_t object)->std::shared_ptr<GraphicsResource>{
                                if(!bridge.resources.contains(object))return {};written.insert(object);return gpu_resource(object);});
                        }
                        else if(c.op=="Dispatch"sv || c.op=="DispatchIndirectGpu"sv){
                            const auto p=bridge.pipelines.find(pipeline);if(p==bridge.pipelines.end() || !p->second.compute)throw std::invalid_argument("Dispatch 缺少運算管線");
                            GraphicsStage stage;stage.shader=p->second.shader;bind(stage,compute_roots,signature_of(compute_roots),0);
                            if(c.op=="DispatchIndirectGpu"sv)native.graphics_dispatch_indirect(stage,gpu_resource(v[0]),static_cast<std::uint32_t>(v[1]));
                            else native.graphics_dispatch(stage,{static_cast<std::uint32_t>(v[0]),static_cast<std::uint32_t>(v[1]),static_cast<std::uint32_t>(v[2])});
                        }
                        else if(c.op=="WriteBufferImmediate"sv){
                            // 依命令順序把 32 位元值寫到 GPU 位址（遊戲常用來每幀歸零計數器／寫入標記）：CPU 端資源直接寫客體
                            // 記憶體，GPU 資源以排在目前命令流中的上傳完成，前面的 GPU 工作先執行、後面的命令看得到新值。
                            for(std::size_t at=0;at+16<=c.data.size();at+=16){
                                std::uint64_t location;std::uint32_t value;std::memcpy(&location,c.data.data()+at,8);std::memcpy(&value,c.data.data()+at+8,4);
                                const auto [object,offset]=resource_at(location);
                                if(!object)throw std::invalid_argument("WriteBufferImmediate 目的位址不屬於任何資源");
                                if(watch(c.op,object))trace("[d3d12] 監看 WriteBufferImmediate 資源=%llx+%llu 值=%u\n",static_cast<unsigned long long>(object),static_cast<unsigned long long>(offset),value);
                                const auto& d=bridge.resources.at(object);
                                if(d.dimension!=1 || (offset&3) || offset+4>d.width)throw std::invalid_argument("WriteBufferImmediate 目的範圍無效");
                                if(d.cpu){memory.store(GuestAddress{d.cpu+offset},32,value);continue;}
                                if(!d.gpu)throw std::invalid_argument("WriteBufferImmediate 目的資源沒有儲存");
                                std::array<std::uint8_t,4> data;std::memcpy(data.data(),&value,4);
                                native.graphics_write_region(GraphicsView{d.gpu},data,4,4,{static_cast<std::uint32_t>(offset),0,0,static_cast<std::uint32_t>(offset+4),1,1});
                            }
                        }
                        else if(c.op=="CopyBufferRegion"sv || c.op=="CopyResource"sv){
                            const bool whole=c.op=="CopyResource"sv;const auto dst=v[0],src=whole?v[1]:v[2];
                            auto& d=bridge.resources.at(dst);auto& s=bridge.resources.at(src);
                            const auto dst_offset=whole?0:v[1],src_offset=whole?0:v[3],size=whole?std::min(d.width,s.width):v[4];
                            // 診斷（X64ARM_TRACE_D3D12_DST_WIDTH=<位元組>）：複製到指定寬度的緩衝時列出來源堆積、位移與來源開頭值（追查 GPU 常數緩衝由誰填入）。
                            if(static const char* const width_text=std::getenv("X64ARM_TRACE_D3D12_DST_WIDTH");width_text){
                                static const auto width=std::strtoull(width_text,nullptr,10);
                                if(d.width==width){
                                    std::array<float,8> head{};if(s.cpu)memory.read(GuestAddress{s.cpu+src_offset},std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(head.data()),static_cast<std::size_t>(std::min<std::uint64_t>(32,size))));
                                    std::fprintf(stderr,"d3d12_dst %s 目的=%llx+%llu 來源=%llx+%llu 大小=%llu 來源heap=%u 來源CPU=%d 來源寬=%llu 開頭=%g,%g,%g,%g,%g,%g,%g,%g\n",std::string(c.op).c_str(),static_cast<unsigned long long>(dst),static_cast<unsigned long long>(dst_offset),
                                        static_cast<unsigned long long>(src),static_cast<unsigned long long>(src_offset),static_cast<unsigned long long>(size),s.heap_type,s.cpu?1:0,static_cast<unsigned long long>(s.width),head[0],head[1],head[2],head[3],head[4],head[5],head[6],head[7]);
                                }
                            }
                            // 診斷（X64ARM_TRACE_D3D12_READBACK=1）：GPU 緩衝小範圍複製到 READBACK 時，列出目的目前內容（上一次讀回的結果）。
                            static const bool trace_readback=std::getenv("X64ARM_TRACE_D3D12_READBACK")!=nullptr;
                            if(trace_readback && d.cpu && d.reads_back()){
                                std::array<std::uint32_t,8> head{};memory.read(GuestAddress{d.cpu+dst_offset},std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(head.data()),static_cast<std::size_t>(std::min<std::uint64_t>(32,size))));
                                std::fprintf(stderr,"d3d12_readback 緩衝 src=%llx+%llu 來源CPU=%d 目的=%llx+%llu 大小=%llu host_shared=%d 目前=%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",static_cast<unsigned long long>(src),static_cast<unsigned long long>(src_offset),s.cpu?1:0,
                                    static_cast<unsigned long long>(dst),static_cast<unsigned long long>(dst_offset),static_cast<unsigned long long>(size),d.host_shared?1:0,head[0],head[1],head[2],head[3],head[4],head[5],head[6]);
                            }
                            if(watch(c.op,dst)){
                                std::array<std::uint32_t,3> head{};if(s.cpu && size>=12)memory.read(GuestAddress{s.cpu+src_offset},std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(head.data()),12));
                                trace("[d3d12] 監看 %s 目的=%llx+%llu 來源=%llx+%llu 大小=%llu 來源CPU=%d 開頭=%u,%u,%u\n",std::string(c.op).c_str(),static_cast<unsigned long long>(dst),static_cast<unsigned long long>(dst_offset),
                                    static_cast<unsigned long long>(src),static_cast<unsigned long long>(src_offset),static_cast<unsigned long long>(size),s.cpu?1:0,head[0],head[1],head[2]);
                            }
                            if(d.dimension!=1 || s.dimension!=1){
                                if(!whole || !d.gpu || !s.gpu)throw std::invalid_argument("CopyResource 紋理需兩端皆為 GPU 資源");
                                for(std::uint32_t level=0;level<s.levels;++level)for(std::uint32_t layer=0;layer<(s.dimension==4?1:s.depth);++layer){
                                    GraphicsView from{s.gpu};from.level=level;from.slice=layer;GraphicsView to{d.gpu};to.level=level;to.slice=layer;
                                    native.graphics_copy(to,from,{0,0,0},{0,0,0,std::max(1U,static_cast<std::uint32_t>(s.width>>level)),std::max(1U,s.height>>level),s.dimension==4?std::max(1U,s.depth>>level):1U});
                                }
                                continue;
                            }
                            // 零複製目的（READBACK 等）與 GPU 資源相同走 GPU blit：依命令順序在 GPU 時間軸寫入，完成後客體直接可見。
                            const bool gpu_destination=d.gpu && (!d.cpu || d.host_shared);
                            if(s.cpu && d.cpu && !(gpu_destination && s.host_shared)){const auto data=bytes(s.cpu+src_offset,static_cast<std::size_t>(size));memory.write(GuestAddress{d.cpu+dst_offset},data);continue;}
                            // 目的在 GPU：來源只上傳用到的範圍（或本來就在 GPU），以 GPU blit 複製，不經 CPU 讀回。
                            if(gpu_destination && size && dst_offset+size<=UINT32_MAX && src_offset+size<=UINT32_MAX){
                                const auto source=gpu_resource(src,src_offset,size);
                                native.graphics_copy(GraphicsView{d.gpu},GraphicsView{source},{static_cast<std::uint32_t>(dst_offset),0,0},
                                    {static_cast<std::uint32_t>(src_offset),0,0,static_cast<std::uint32_t>(src_offset+size),1,1});
                                if(d.host_shared)written.insert(dst);
                                continue;
                            }
                            if(!s.cpu)count_sync(d.cpu?"buffer_to_readback":"buffer_gpu_roundtrip");
                            const auto data=s.cpu?bytes(s.cpu+src_offset,static_cast<std::size_t>(size)):[&]{GraphicsView range{s.gpu};range.first=static_cast<std::uint32_t>(src_offset);range.count=static_cast<std::uint32_t>(size);return native.graphics_read(range);}();
                            if(d.cpu){memory.write(GuestAddress{d.cpu+dst_offset},data);continue;}
                            native.graphics_write_region(GraphicsView{d.gpu},data,static_cast<std::uint32_t>(size),static_cast<std::uint32_t>(size),{static_cast<std::uint32_t>(dst_offset),0,0,static_cast<std::uint32_t>(dst_offset+size),1,1});
                        }
                        else if(c.op=="CopyTextureRegion"sv){
                            // 足跡（緩衝）↔ 紋理子資源；兩端皆為紋理時以 GPU 複製。
                            const auto location=[&](std::size_t at){std::uint64_t resource;std::memcpy(&resource,c.data.data()+at,8);std::uint32_t type,sub;std::memcpy(&type,c.data.data()+at+8,4);std::memcpy(&sub,c.data.data()+at+16,4);return std::tuple{resource,type,sub};};
                            const auto [dst,dst_type,dst_sub]=location(0);const auto [src,src_type,src_sub]=location(48);
                            const auto footprint=[&](std::size_t at){std::uint64_t offset;std::memcpy(&offset,c.data.data()+at+16,8);std::array<std::uint32_t,5> f;std::memcpy(f.data(),c.data.data()+at+24,20);return std::pair{offset,f};};
                            auto& d=bridge.resources.at(dst);auto& s=bridge.resources.at(src);
                            const auto x=static_cast<std::uint32_t>(v[1]),y=static_cast<std::uint32_t>(v[2]),z=static_cast<std::uint32_t>(v[3]);
                            if(dst_type==0 && src_type==1){
                                const auto [offset,f]=footprint(48);const auto level=dst_sub%d.levels,layer=dst_sub/d.levels;
                                const auto pitch=graphics_format_pitch(f[0],f[1],f[2]);
                                // 零複製來源（無來源範圍）：GPU 依命令順序直接從客體可見的緩衝複製，不經 CPU 讀入、不等紋理閒置。
                                if(s.host_shared && s.gpu && d.gpu && c.data.size()<120){
                                    GraphicsView view{d.gpu};view.level=level;view.slice=layer;
                                    if(D3D12Submitter::tracing() && d3d12_submitter_)d3d12_submitter_->stats.texture_copies.fetch_add(1,std::memory_order_relaxed);
                                    if(native.graphics_copy_buffer_texture(view,GraphicsView{s.gpu},offset,f[4],f[4]*pitch[1],{x,y,z,x+f[1],y+f[2],z+f[3]},true))continue;
                                }
                                std::vector<std::uint8_t> data(std::size_t{pitch[0]}*pitch[1]*f[3]);
                                // 來源在 GPU（DEFAULT 堆積）時讀回足跡範圍；CPU 後備則逐列直接讀入，不另配置每列暫存。
                                std::vector<std::uint8_t> staged;const std::uint64_t span_bytes=(std::uint64_t{f[3]}*pitch[1]-1)*f[4]+pitch[0];
                                if(!s.cpu && s.gpu){count_sync("upload_from_gpu_footprint");GraphicsView range{s.gpu};range.first=static_cast<std::uint32_t>(offset);range.count=static_cast<std::uint32_t>(span_bytes);staged=native.graphics_read(range);}
                                for(std::uint32_t slice=0;slice<f[3];++slice)for(std::uint32_t row=0;row<pitch[1];++row){
                                    const auto from=offset+(std::uint64_t{slice}*pitch[1]+row)*f[4];auto* target=data.data()+(std::size_t{slice}*pitch[1]+row)*pitch[0];
                                    if(s.cpu)memory.read(GuestAddress{s.cpu+from},std::span<std::uint8_t>(target,pitch[0]));
                                    else if(from-offset+pitch[0]<=staged.size())std::memcpy(target,staged.data()+(from-offset),pitch[0]);
                                }
                                GraphicsView view{d.gpu};view.level=level;view.slice=layer;
                                native.graphics_write_region(view,data,pitch[0],pitch[0]*pitch[1],{x,y,z,x+f[1],y+f[2],z+f[3]});
                            }else if(dst_type==1 && src_type==0){
                                const auto [offset,f]=footprint(0);const auto level=src_sub%s.levels,layer=src_sub/s.levels;
                                // 診斷（X64ARM_TRACE_D3D12_READBACK=1）：小紋理讀回到 CPU 可見緩衝時，列出目的位置目前的內容（上一次讀回的結果）。
                                static const bool trace_readback=std::getenv("X64ARM_TRACE_D3D12_READBACK")!=nullptr;
                                if(trace_readback && d.cpu){
                                    std::array<std::uint32_t,4> head{};memory.read(GuestAddress{d.cpu+offset},std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(head.data()),16));
                                    std::fprintf(stderr,"d3d12_readback 紋理 src=%llx %ux%u fmt=%u 目的=%llx+%llu host_shared=%d box=%d 目前=%08x,%08x,%08x,%08x\n",static_cast<unsigned long long>(src),static_cast<unsigned>(s.width),s.height,s.format,
                                        static_cast<unsigned long long>(dst),static_cast<unsigned long long>(offset),d.host_shared?1:0,c.data.size()>=120?1:0,head[0],head[1],head[2],head[3]);
                                }
                                // 零複製目的（整個子資源、無來源範圍）：GPU blit 寫入，提交完成後客體直接可見（Map 時等寫入完成）。
                                if(d.host_shared && d.gpu && s.gpu && c.data.size()<120 && !x && !y && !z){
                                    GraphicsView view{s.gpu};view.level=level;view.slice=layer;
                                    const auto w=std::max(1U,static_cast<std::uint32_t>(s.width>>level)),h=std::max(1U,s.height>>level);const auto rows=graphics_format_pitch(s.format,w,h)[1];
                                    if(native.graphics_copy_buffer_texture(view,GraphicsView{d.gpu},offset,f[4],f[4]*rows,{0,0,0,w,h,1},false)){written.insert(dst);continue;}
                                }
                                count_sync("texture_to_readback");GraphicsView view{s.gpu};view.level=level;view.slice=layer;const auto data=native.graphics_read(view);
                                const auto w=std::max(1U,static_cast<std::uint32_t>(s.width>>level)),h=std::max(1U,s.height>>level);const auto pitch=graphics_format_pitch(s.format,w,h);
                                for(std::uint32_t row=0;row<pitch[1] && std::size_t{row+1}*pitch[0]<=data.size();++row)if(d.cpu)memory.write(GuestAddress{d.cpu+offset+std::uint64_t{row}*f[4]},std::span<const std::uint8_t>(data).subspan(std::size_t{row}*pitch[0],pitch[0]));
                            }else if(dst_type==0 && src_type==0){
                                GraphicsView to{d.gpu};to.level=dst_sub%d.levels;to.slice=dst_sub/d.levels;GraphicsView from{s.gpu};from.level=src_sub%s.levels;from.slice=src_sub/s.levels;
                                std::array<std::uint32_t,6> box{0,0,0,std::max(1U,static_cast<std::uint32_t>(s.width>>from.level)),std::max(1U,s.height>>from.level),1};
                                if(c.data.size()>=120)std::memcpy(box.data(),c.data.data()+96,24);
                                native.graphics_copy(to,from,{x,y,z},box);
                            }
                        }
                        else if(c.op=="ResourceBarrier"sv || c.op=="SetDescriptorHeaps"sv || c.op=="SetName"sv || c.op=="SetMarker"sv || c.op=="BeginEvent"sv || c.op=="EndEvent"sv){}
                        else report("List."+std::string(c.op));
                    }
                }
                // 預設非同步：提交後不等 GPU（CPU 準備下一批與 GPU 執行重疊）；讀回延到此佇列下一個 Signal 完成時。
                // X64ARM_D3D12_SYNC_SUBMIT=1 回到每次提交都等 GPU 完成的同步模式。
                static const bool sync_submit=[] {const auto* value=std::getenv("X64ARM_D3D12_SYNC_SUBMIT");return value && std::string_view(value)=="1";}();
                if(sync_submit){
                    native.graphics_flush(true);
                    for(const auto object:written)if(bridge.resources.at(object).reads_back())read_back(object);
                }else{
                    auto& queue_state=bridge.queue_states[self];queue_state.done=native.graphics_fence();
                    for(const auto object:written)if(auto& r=bridge.resources.at(object);r.reads_back()){
                        if(r.host_shared){const std::lock_guard lock(bridge.resource_lock(object));r.gpu_writes=queue_state.done;}else queue_state.readbacks.push_back(object);}
                }
            }catch(const std::exception& error){
                trace("[d3d12] ExecuteCommandLists 失敗（%s）：%s\n",failed_op.c_str(),error.what());
                note(std::string("d3d12_note=ExecuteCommandLists 失敗：")+error.what());
            }
            return finish(0);
        }
        report(symbol);return finish(not_implemented);
    }
    report(symbol);return finish(not_implemented);
}
}
