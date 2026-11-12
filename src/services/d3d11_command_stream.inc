// D3D11 命令串流（X64ARM_D3D11_STREAM=0 關閉）：沒有回傳值的狀態設定、Draw／Dispatch、查詢 Begin／End、緩衝更新與持久 Unmap，由 worker 把參數複製進環狀緩衝後
// 立刻回到客體，另一條宿主執行緒依序執行。UE 系每幀兩萬多次 D3D11 呼叫都在 RenderThread 上，33 號遠征隊每幀 32 ms 中有 18 ms 是這些服務，其他執行緒都在等它。
//
// 契約：
// - 只有 x64 的 worker 路徑、單一擁有者（一條客體執行緒＋一個立即內容）會排入；其他執行緒或內容照舊同步，先 drain。
// - 排入的方法對客體沒有輸出；留在擁有者上同步的方法分兩種：不依賴佇列內容的（GetDesc、AddRef、裝置建立）不必等；其他 D3D11 方法在取鎖之前先 drain，對客體可觀察的狀態順序不變。
// - 最後一次 Release 也排入：佇列裡可能還有對該物件的參照；客體已放棄所有參照，回傳值固定是 0。裝置、內容與交換鏈不收。
// - 查詢：Begin／End 排入後，GetData 看到尚未處理就回 S_FALSE（規格允許，客體本來就要輪詢），不必等。
// - 持久 Unmap 的上傳在串流執行緒讀客體區塊：同一資源的下一次 Map 要等它處理完，客體才能再寫。
// - 緩衝更新的來源資料在入列時複製（客體可以立刻改寫來源）；只收持久映射不存在的資源，其他走同步路徑。
// - 槽內是合成的 x64 呼叫框架，指標參數改指向副本（放不下時放進共用區）；空指標保持空指標。放不下或讀取參數故障時不排入，drain 後走同步路徑，故障落在原呼叫點。
// - 鎖序與 worker 相同（映射共用 → 圖形）。drain 不可在持任一把鎖時呼叫：寫者已設旗標時串流執行緒進不了共用鎖，而寫者又在等持有者。
// - 全停路徑不取映射鎖就改區域表：全部 worker 停止後（d3d11_stream_quiesced）暫停串流執行緒，等它離開目前的命令；不等佇列清空，否則每次全停都要多等整段積壓。
//   完整服務若是 D3D11（invoke_d3d11 的非本地入口）才放行並等到清空。worker 或協調服務再次進入代表客體已恢復，就解除暫停。
// - 串流執行緒沒有退路可交回協調者：擁有者只認領裝置未開多執行緒保護的立即內容，SetMultithreadProtected、Enter 與內容銷毀都走完整服務，會清除擁有者。
// - 例外只保存第一個，之後的命令丟棄；由下一個完整服務邊界重新拋出。
namespace detail {const WindowsServiceDescriptor* runtime_stub_descriptor(GuestAddress stub_base,GuestAddress address) noexcept;std::size_t stub_service_count() noexcept;}
enum class D3D11StreamClass:std::uint8_t {Other,Sync,NoDrain,Release,Map,GetData,GetTargets,Query,Unmap,Update,Copy,Scalar,Shader,Constants,Pointers,VertexBuffers,Viewports,Scissors,Blend,Uavs,Targets,TargetsUavs,Clear,ClearDepth,Present};
// 目前這條宿主執行緒是串流執行緒（Present 的處理程式不能讓客體執行緒等待，也不是客體執行緒）。
thread_local bool d3d11_on_stream_thread=false;
struct D3D11CommandStream;
void d3d11_stream_resident_detach(const D3D11CommandStream*) noexcept;
// 持久映射前端路徑的對應表（擁有者私有）：固定 16384 格開放定址（乘法雜湊取高 shift 位元、線性探查），核心常駐入口（resident_d3d11_arm64.inc 類別 6／7）
// 直接查表，格式與位移固定（下方 static_assert），格數由描述元提供。只收探查距離在 probe_limit 內的位置，核心入口查 probe_limit 格找不到就交回服務端；
// 刪除以後移補位，不留墓碑。表太滿時不收，該資源每次都走完整路徑（計入 refused）：《崛起》地熱谷的持久映射資源超過 1024 格版本的上限時
// 2 vCPU 46→35 FPS，所以容量要遠大於遊戲同時存在的動態資源數。
// pending：之後的 Map 要等這個序號處理完（同資源排在環裡的 Unmap／Update／Copy，與 stream.pending 同步寫入；pending 表清除時保留，只會多一次保守的慢路徑）。
struct D3D11FrontMapTable {
    static constexpr std::size_t capacity=16384,probe_limit=16,shift=50;
    static constexpr std::uint64_t multiplier=UINT64_C(0x9e3779b97f4a7c15);
    struct Entry {std::uint64_t key{},pointer{};std::uint32_t size{},mapped{},row{},depth{};std::uint64_t pending{};std::array<std::uint8_t,24> reserved{};};
    std::unique_ptr<Entry[]> entries=std::make_unique<Entry[]>(capacity);
    std::size_t used{};
    // 已佔用的鍵：插入／刪除只在服務端（核心常駐入口只查表並改 mapped），走訪時只看這些鍵，不必掃 16384 格
    // （完整服務同步每次都要走訪；原本每次掃整張表，Ice Storm 的主執行緒七成時間耗在這裡）。
    std::vector<std::uint64_t> keys;
    std::uint64_t refused{};
    static std::size_t home(std::uint64_t key) noexcept {return static_cast<std::size_t>((key*multiplier)>>shift);}
    Entry* find(std::uint64_t key) noexcept {
        if(!key)return nullptr;
        for(std::size_t n=0,i=home(key);n<capacity;++n,i=(i+1)&(capacity-1)){auto& e=entries[i];if(e.key==key)return &e;if(!e.key)return nullptr;}
        return nullptr;
    }
    Entry* insert(std::uint64_t key) noexcept {
        if(auto* e=find(key))return e;
        if(!key || used>=capacity*3/4){++refused;return nullptr;}
        for(std::size_t n=0,i=home(key);n<probe_limit;++n,i=(i+1)&(capacity-1)){auto& e=entries[i];if(!e.key){e=Entry{};e.key=key;++used;keys.push_back(key);return &e;}}
        ++refused;return nullptr;
    }
    void erase(Entry* e) noexcept {
        if(const auto found=std::find(keys.begin(),keys.end(),e->key);found!=keys.end()){*found=keys.back();keys.pop_back();}
        auto i=static_cast<std::size_t>(e-entries.get());entries[i]=Entry{};--used;
        for(auto j=(i+1)&(capacity-1);entries[j].key;j=(j+1)&(capacity-1)){
            const auto k=home(entries[j].key);
            // k 在 (i, j] 之間（環狀）時 j 留在原位；否則移到 i 補洞。
            if(i<=j?(i<k && k<=j):(i<k || k<=j))continue;
            entries[i]=entries[j];entries[j]=Entry{};i=j;
        }
    }
    void clear() noexcept {if(used)for(std::size_t i=0;i<capacity;++i)entries[i]=Entry{};used=0;keys.clear();}
    // f 不可增刪鍵（只改欄位）。
    template<class F> void for_each(F&& f){for(std::size_t k=0;k<keys.size();++k)if(auto* const e=find(keys[k]))f(*e);}
};
static_assert(sizeof(D3D11FrontMapTable::Entry)==64 && offsetof(D3D11FrontMapTable::Entry,pointer)==8 && offsetof(D3D11FrontMapTable::Entry,size)==16 &&
              offsetof(D3D11FrontMapTable::Entry,mapped)==20 && offsetof(D3D11FrontMapTable::Entry,row)==24 && offsetof(D3D11FrontMapTable::Entry,depth)==28 &&
              offsetof(D3D11FrontMapTable::Entry,pending)==32 && (UINT64_C(1)<<(64-D3D11FrontMapTable::shift))==D3D11FrontMapTable::capacity);
struct D3D11CommandStream {
    // 環要裝得下一整幀（UE 系每幀兩萬多次）：爆發時客體才不會被壓到和串流執行緒同速。頁面用到才配置。
    static constexpr std::size_t slot_bytes=256,slot_capacity=32768,mapped_bytes=slot_capacity*slot_bytes;
    const std::size_t slots=[] {
        // X64ARM_D3D11_STREAM_SLOTS=<2 的次方>：對照環的大小。
        const auto* value=std::getenv("X64ARM_D3D11_STREAM_SLOTS");const auto parsed=value?std::strtoull(value,nullptr,10):0;
        return parsed>=1024 && parsed<=slot_capacity && !(parsed&(parsed-1))?static_cast<std::size_t>(parsed):std::size_t{8192};
    }();
    static constexpr std::size_t arena_bytes=std::size_t{4}<<20,arena_item_limit=std::size_t{1}<<20;
    struct Entry {const WindowsServiceDescriptor* service{};std::uint64_t rip{},rcx{},rdx{},r8{},r9{},rsp{},arena_end{},snapshot_at{UINT64_MAX},snapshot_end{};std::uint32_t snapshot_size{};std::array<std::uint8_t,4> xmm3{};bool present{};};
    // 擁有者私有的客體頁快取：排入不取映射鎖（否則每次撞到寫者都要等）。世代是 native_borrow_generation，所有映射、權限與 backing 變更之前都會遞增。
    // 只讀客體正在傳給這次呼叫的頁；讀取途中被別的執行緒解除映射屬於客體自身的競爭。
    struct Page {std::uint64_t page{UINT64_MAX},generation{};const std::uint8_t* host{};};
    // 建立後不變，或很少改變。
    std::shared_ptr<MemoryBacking> backing,arena;std::uint64_t guest_base{},arena_base{};std::uint8_t* host_base{},*arena_host{};
    std::vector<std::atomic<std::uint32_t>> drains_by_service;
    pthread_t thread{};
    std::exception_ptr failure;
    alignas(128) std::atomic<std::uint64_t> context{};
    std::atomic<std::uint64_t> owner{}; // 只在持映射鎖的認領與完整服務時改變。
    std::atomic<bool> failed{},stopping{},started{};
    // 擁有者。
    alignas(128) std::atomic<std::uint64_t> head{}; // 發布的序號。
    std::uint64_t cached_tail{},arena_head{},page_generation{},present_sequence{};
    std::unordered_map<std::uint64_t,std::uint64_t> pending; // 物件 → 需要處理完的序號（查詢的 Begin／End、持久 Unmap）。
    std::unordered_map<std::uint64_t,std::uint32_t> update_widths; // 可排入更新的緩衝大小；update_epoch 改變時清空。
    std::uint64_t update_epoch_seen{};
    std::array<std::uint64_t,8> shadow_targets{};std::uint64_t shadow_depth{};bool shadow_valid{}; // 最後排入的 OMSetRenderTargets，OMGetRenderTargets 不必等佇列。
    // 常數緩衝只快取已排入的完整綁定（普通 SetConstantBuffers 將子範圍復原）。同步命令可能更動綁定，由世代通知擁有者失效。
    struct ConstantBindings {std::array<std::uint64_t,14> values{};std::uint32_t valid{};};
    std::array<ConstantBindings,6> constant_bindings;
    std::uint64_t constant_epoch_seen{};
    std::atomic<std::uint64_t> constant_epoch{},constant_skips{};
    // 持久映射的前端路徑（X64ARM_D3D11_STREAM_FRONT_MAP=0 關閉）：WRITE_DISCARD 的 Map 直接回傳同一塊客體區塊，Unmap 當場把內容複製進快照區再排入，
    // 串流執行緒不必讀客體記憶體，下一次 Map 也不必等上傳完成。對應狀態只記在這張擁有者私有表；完整服務清除擁有者前轉回 subresource_maps。
    D3D11FrontMapTable front_maps;std::uint64_t front_epoch_seen{},snapshot_head{},front_owner{},front_context{};
    // 擁有者快取的 snapshot_tail：snapshot_tail 與 tail 同一條快取線、串流執行緒每筆都寫，每次 Unmap 都去讀會讓兩邊來回搶線
    //（地熱谷 2 vCPU 核心入口版每個繪製的重播從約 2.1 µs 變成 4.2 µs）。舊值只會算出較少的可用空間，不夠時才重讀。
    std::uint64_t snapshot_cached_tail{};
    std::mutex front_dropped_mutex;std::vector<std::uint64_t> front_dropped; // 持久映射被撤銷或銷毀的資源，擁有者下一次 Map 時移除。
    static constexpr std::size_t snapshot_bytes=std::size_t{16}<<20,snapshot_item_limit=std::size_t{1}<<20;
    std::unique_ptr<std::uint8_t[]> snapshot;
    std::atomic<std::uint64_t> front_maps_taken{},front_unmaps_taken{};
    std::atomic<std::uint64_t> full_enqueues{}; // 全停中由協調者代擁有者排入（d3d11_stream_full_enqueue）
    std::array<Page,256> pages{};
    std::atomic<std::uint64_t> enqueued{},rejected{},drain_waits{},drain_ns{},full_waits{},full_ns{},locked_enqueues{};
    // 串流執行緒。
    alignas(128) std::atomic<std::uint64_t> tail{}; // 完成的序號。
    std::atomic<std::uint64_t> arena_tail{},snapshot_tail{},paused_ns{},idle_ns{},present_ready_ns{},present_wait_ns{};
    alignas(128) std::atomic<std::uint32_t> sleeping{};
    alignas(128) std::atomic<std::uint32_t> busy{};  // 串流執行緒正在處理命令（Dekker，配對 paused）。
    alignas(128) std::atomic<std::uint32_t> paused{};
    alignas(128) std::atomic<std::uint64_t> update_epoch{}; // 物件銷毀與協調者路徑的方法會遞增。
    alignas(128) std::atomic<std::uint64_t> front_epoch{}; // 持久映射被撤銷或帶持久映射的資源銷毀時遞增；擁有者看到改變就清空 front_maps 中未對應的項目。
    std::atomic<std::uint64_t> pauses{},pause_ns{};
    // 交給命令執行緒的呈現：串流執行緒累計投遞數，命令執行緒做完才遞增完成數。
    alignas(128) std::uint64_t presents_posted{};
    std::shared_ptr<NativeOpenGlContext> present_producer;
    std::shared_ptr<std::atomic<std::uint64_t>> presents_done{std::make_shared<std::atomic<std::uint64_t>>(0)};
    // 獨立呈現執行緒的 Present：環只有單一生產者，放進這個信箱。串流執行緒處理完序號 after 之前的命令就執行它，順序與排入環相同；最多一筆。
    alignas(128) std::atomic<std::uint32_t> foreign_ready{};
    Entry foreign{};std::uint64_t foreign_after{};
    alignas(128) std::atomic<std::uint64_t> foreign_presents{};
    std::uint64_t foreign_presents_seen{}; // 擁有者私有：呈現會解除 back buffer 綁定，影子要失效。
    alignas(128) std::array<Entry,slot_capacity> entries;
    ~D3D11CommandStream(){
        d3d11_stream_resident_detach(this);
        if(!started.load(std::memory_order_relaxed))return;
        stopping.store(true,std::memory_order_seq_cst);
        paused.store(0,std::memory_order_seq_cst);paused.notify_all();
        sleeping.store(0,std::memory_order_seq_cst);sleeping.notify_all();
        pthread_join(thread,nullptr);
    }
};
// 核心的 D3D11 命令串流純量方法入口（resident_d3d11_arm64.inc）讀取的描述元：位移與組語約定一致，建立串流時填寫，銷毀時停用。
// 定義在不參與 LTO 的 resident_d3d11_descriptor.cpp；新核心另有弱定義（只給沒有服務端的核心測試），連結後核心組語與服務共用同一份。
// X64ARM_D3D11_STREAM_RESIDENT=0 關閉（stub 不標記、描述元不啟用）。
struct ResidentD3D11StreamDescriptor {
    std::uint64_t enabled{};const void* context{};const void* owner{};std::uint64_t gs_offset{};const void* failed{};const void* paused{};
    void* head{};const void* cached_tail{};std::uint64_t slots{};void* entries{};std::uint64_t entry_size{};void* host_base{};std::uint64_t guest_base{};
    const void* arena_head{};const void* snapshot_head{};const void* descriptors{};std::uint64_t descriptor_size{},arguments_offset{},stub_begin{};
    const void* sleeping{};void (*wake)() noexcept{};void* enqueued{};std::uint64_t permissions_offset{};
    std::uint64_t entries_seen{},direct_enqueues{},registered{}; // 核心入口的診斷計數（184、192；併發時為近似值）與核心登錄入口的 stub 數（200）
    std::uint64_t relaxed_publish{}; // 208：串流執行緒以輪詢等待時，入口不必以 seq_cst 讀 sleeping
    const std::uint8_t* classes{};std::uint64_t constant_dedup{};const void* constant_epoch{};const void* constant_epoch_seen{};void* constant_bindings{}; // 216～248
    std::uint64_t requests[8]{}; // 256：入口登錄請求槽（核心協調者取出）
    void* constant_skips{};       // 320
    // 328 起：持久映射前端路徑（類別 6 Map、7 Unmap）。
    void* front_entries{};std::uint64_t front_multiplier{};const void* front_epoch{};const void* front_epoch_seen{};const void* tail{}; // 328～360
    void* snapshot{};const void* snapshot_tail{};std::uint64_t snapshot_bytes{},snapshot_item_limit{};void* front_maps_taken{};void* front_unmaps_taken{}; // 368～408
    std::uint64_t kernel_features{}; // 416：核心協調者寫入的能力（bit 0：支援類別 6／7）；舊核心不寫，服務端就不把 Map／Unmap 交給核心入口。
    std::uint64_t front_shift{},front_mask{}; // 424、432：前端映射表的雜湊位移（64−log2 格數）與索引遮罩
    void* snapshot_cached_tail{}; // 440：擁有者快取的 snapshot_tail（不夠時才讀 376 的真正位置並更新）
};
static_assert(offsetof(ResidentD3D11StreamDescriptor,gs_offset)==24 && offsetof(ResidentD3D11StreamDescriptor,head)==48 &&
              offsetof(ResidentD3D11StreamDescriptor,slots)==64 && offsetof(ResidentD3D11StreamDescriptor,guest_base)==96 &&
              offsetof(ResidentD3D11StreamDescriptor,descriptors)==120 && offsetof(ResidentD3D11StreamDescriptor,stub_begin)==144 &&
              offsetof(ResidentD3D11StreamDescriptor,wake)==160 && offsetof(ResidentD3D11StreamDescriptor,permissions_offset)==176 &&
              offsetof(ResidentD3D11StreamDescriptor,entries_seen)==184 && offsetof(ResidentD3D11StreamDescriptor,direct_enqueues)==192 &&
              offsetof(ResidentD3D11StreamDescriptor,relaxed_publish)==208 && offsetof(ResidentD3D11StreamDescriptor,classes)==216 &&
              offsetof(ResidentD3D11StreamDescriptor,constant_bindings)==248 && offsetof(ResidentD3D11StreamDescriptor,requests)==256 &&
              offsetof(ResidentD3D11StreamDescriptor,constant_skips)==320 && offsetof(ResidentD3D11StreamDescriptor,front_entries)==328 &&
              offsetof(ResidentD3D11StreamDescriptor,tail)==360 && offsetof(ResidentD3D11StreamDescriptor,snapshot)==368 &&
              offsetof(ResidentD3D11StreamDescriptor,front_unmaps_taken)==408 && offsetof(ResidentD3D11StreamDescriptor,kernel_features)==416 &&
              offsetof(ResidentD3D11StreamDescriptor,front_mask)==432 && offsetof(ResidentD3D11StreamDescriptor,snapshot_cached_tail)==440 &&
              sizeof(ResidentD3D11StreamDescriptor)<=512);
static_assert(offsetof(D3D11CommandStream::Entry,service)==0 && offsetof(D3D11CommandStream::Entry,rip)==8 && offsetof(D3D11CommandStream::Entry,rcx)==16 &&
              offsetof(D3D11CommandStream::Entry,r9)==40 && offsetof(D3D11CommandStream::Entry,rsp)==48 && offsetof(D3D11CommandStream::Entry,arena_end)==56 &&
              offsetof(D3D11CommandStream::Entry,snapshot_at)==64 && offsetof(D3D11CommandStream::Entry,snapshot_end)==72 && offsetof(D3D11CommandStream::Entry,snapshot_size)==80 &&
              offsetof(D3D11CommandStream::Entry,present)==88);
static_assert(sizeof(D3D11CommandStream::ConstantBindings)==120 && offsetof(D3D11CommandStream::ConstantBindings,valid)==112);
static_assert(D3D11CommandStream::slot_bytes==256 && sizeof(std::atomic<std::uint64_t>)==8 && sizeof(std::atomic<std::uint32_t>)==4 && sizeof(std::atomic<bool>)==1);
extern "C" { extern std::uint8_t x64arm_resident_d3d11_stream[512]; } // resident_d3d11_descriptor.cpp（不參與 LTO）
std::atomic<D3D11CommandStream*> d3d11_resident_stream{};
extern "C" void x64arm_d3d11_stream_resident_wake() noexcept {
    if(auto* stream=d3d11_resident_stream.load(std::memory_order_acquire)){stream->sleeping.store(0,std::memory_order_seq_cst);stream->sleeping.notify_one();}
}
ResidentD3D11StreamDescriptor& d3d11_resident_descriptor() noexcept {return *reinterpret_cast<ResidentD3D11StreamDescriptor*>(x64arm_resident_d3d11_stream);}
void d3d11_stream_resident_detach(const D3D11CommandStream* stream) noexcept {
    if(d3d11_resident_stream.load(std::memory_order_acquire)!=stream)return;
    if(std::getenv("X64ARM_TRACE_GRAPHICS_STATS"))
        std::fprintf(stderr,"[d3d11_stream_resident] registered=%llu entries=%llu direct=%llu\n",static_cast<unsigned long long>(d3d11_resident_descriptor().registered),
            static_cast<unsigned long long>(d3d11_resident_descriptor().entries_seen),static_cast<unsigned long long>(d3d11_resident_descriptor().direct_enqueues));
    std::atomic_ref(d3d11_resident_descriptor().enabled).store(0,std::memory_order_seq_cst);
    d3d11_resident_stream.store(nullptr,std::memory_order_release);
}
// C++ 排入了有常駐入口的純量方法（入口尚未登錄）：把 stub RIP 放進描述元 256 起的 8 個請求槽，核心協調者取回信箱事件後登錄入口。
// 每個 stub 只請求一次；槽滿時清除標記，下次再請求。
// 送出請求的這次 worker 服務改回報 CompletedResolve（d3d11_resident_request_pending），核心隨即 poke 協調者處理；否則要等下一次協調者事件。
thread_local bool d3d11_resident_request_pending=false;
void d3d11_stream_resident_request(std::size_t runtime_index,std::uint64_t rip) noexcept {
    static std::vector<std::atomic<std::uint8_t>> requested(runtime_service_descriptors().size());
    if(runtime_index>=requested.size() || requested[runtime_index].exchange(1,std::memory_order_relaxed))return;
    auto* slots=reinterpret_cast<std::uint64_t*>(x64arm_resident_d3d11_stream+256);
    for(unsigned i=0;i<8;++i){std::uint64_t expected=0;
        if(std::atomic_ref<std::uint64_t>(slots[i]).compare_exchange_strong(expected,rip,std::memory_order_acq_rel)){d3d11_resident_request_pending=true;return;}}
    requested[runtime_index].store(0,std::memory_order_relaxed);
}
bool d3d11_stream_resident_enabled() noexcept {
    static const bool enabled=[] {
        const auto* value=std::getenv("X64ARM_D3D11_STREAM_RESIDENT");const auto* stream=std::getenv("X64ARM_D3D11_STREAM");
        return (!value || std::string_view(value)!="0") && (!stream || std::string_view(stream)!="0");
    }();
    return enabled;
}
namespace {
D3D11StreamClass d3d11_stream_class(std::string_view symbol) noexcept {
    constexpr std::string_view scalar[]{"IASetIndexBuffer","IASetInputLayout","IASetPrimitiveTopology","RSSetState","OMSetDepthStencilState","GenerateMips",
        "Draw","DrawIndexed","DrawInstanced","DrawIndexedInstanced","DrawInstancedIndirect","DrawIndexedInstancedIndirect","Dispatch","DispatchIndirect",
        // 只帶物件指標與純量、沒有回傳值：Fire Strike 每幀數十次，原本每次都等佇列清空（一輪約 4.8 萬次排空）。
        "DiscardView","DiscardResource","CopyStructureCount"};
    if(std::find(std::begin(scalar),std::end(scalar),symbol)!=std::end(scalar))return D3D11StreamClass::Scalar;
    if(symbol=="Begin" || symbol=="End")return D3D11StreamClass::Query;
    if(symbol=="Present")return D3D11StreamClass::Present;
    if(symbol=="GetData")return D3D11StreamClass::GetData;
    if(symbol=="OMGetRenderTargets")return D3D11StreamClass::GetTargets;
    if(symbol=="Map")return D3D11StreamClass::Map;
    if(symbol=="Unmap")return D3D11StreamClass::Unmap;
    if(symbol=="UpdateSubresource")return D3D11StreamClass::Update;
    if(symbol=="CopyResource" || symbol=="CopySubresourceRegion")return D3D11StreamClass::Copy;
    if(symbol=="Release")return D3D11StreamClass::Release;
    if(symbol=="IASetVertexBuffers")return D3D11StreamClass::VertexBuffers;
    if(symbol=="RSSetViewports")return D3D11StreamClass::Viewports;
    if(symbol=="RSSetScissorRects")return D3D11StreamClass::Scissors;
    if(symbol=="OMSetBlendState")return D3D11StreamClass::Blend;
    if(symbol=="CSSetUnorderedAccessViews")return D3D11StreamClass::Uavs;
    if(symbol=="OMSetRenderTargets")return D3D11StreamClass::Targets;
    if(symbol=="OMSetRenderTargetsAndUnorderedAccessViews")return D3D11StreamClass::TargetsUavs;
    if(symbol=="ClearRenderTargetView" || symbol=="ClearUnorderedAccessViewUint" || symbol=="ClearUnorderedAccessViewFloat")return D3D11StreamClass::Clear;
    if(symbol=="ClearDepthStencilView")return D3D11StreamClass::ClearDepth;
    if(symbol.size()>2 && symbol[1]=='S' && std::string_view("VPGHDC").find(symbol[0])!=std::string_view::npos){
        const auto what=symbol.substr(2);
        if(what=="SetShader")return D3D11StreamClass::Shader;
        if(what=="SetConstantBuffers")return D3D11StreamClass::Constants;
        if(what=="SetShaderResources" || what=="SetSamplers")return D3D11StreamClass::Pointers;
    }
    // 只讀建立時就固定的描述或物件自己的欄位，與佇列裡的命令無關；裝置建立只新增物件。協調者路徑另依 d3d11_stream_needs_drain 判斷。
    constexpr std::string_view no_drain[]{"BufferGetDesc","Texture1DGetDesc","Texture3DGetDesc","GraphicsGetDesc","TextureGetDesc","GetResource","SetPrivateData","GetFullscreenState",
        "QueryVideoMemoryInfo","SetVideoMemoryReservation","AddRef","QueryInterface","GetDevice","GetParent","GetDesc","GetImmediateContext","CreateBuffer"};
    if(display_identity::adapter_description_size(symbol,false) && display_identity::parallel_graphics_descriptions())return D3D11StreamClass::NoDrain;
    if(std::find(std::begin(no_drain),std::end(no_drain),symbol)!=std::end(no_drain))return D3D11StreamClass::NoDrain;
    return D3D11StreamClass::Sync;
}
// Map 是否帶 D3D11_MAP_FLAG_DO_NOT_WAIT 且為 READ／WRITE／READ_WRITE：這種呼叫不等同一資源排隊中的命令，由處理程式在 GPU 仍存取時
// 回 DXGI_ERROR_WAS_STILL_DRAWING。旗標在 [rsp+40]；worker 上持映射共用鎖讀取，協調者（擁有者停在這次服務）直接讀。
bool d3d11_map_do_not_wait(const GuestState& state,GuestMemory& memory,const bool lock) noexcept {
    const auto type=static_cast<std::uint32_t>(state.reg(Gpr::R9));
    if(type<1 || type>3)return false;
    try{
        std::optional<std::shared_lock<detail::ParallelMappingLock>> guard;
        if(lock)guard.emplace(detail::parallel_mapping_mutex());
        return (static_cast<std::uint32_t>(memory.load(GuestAddress{state.reg(Gpr::Rsp)+40},32))&0x100000U)!=0;
    }catch(...){return false;}
}
// 依 stub 索引查表：D3D11 以外的服務不受串流影響，不必每次比對名稱。
const std::vector<D3D11StreamClass>& d3d11_stream_classes(){
    static const std::vector<D3D11StreamClass> table=[]{
        const auto descriptors=runtime_service_descriptors();std::vector<D3D11StreamClass> result(descriptors.size(),D3D11StreamClass::Other);
        for(std::size_t i=0;i<descriptors.size();++i)if(descriptors[i].family=="internal-d3d11")result[i]=d3d11_stream_class(descriptors[i].symbol);
        return result;
    }();
    return table;
}
}
// 核心常駐入口的 stub 類別表（resident_d3d11_arm64.inc）：低 4 位元 1 純量、2 Shader、3 指標陣列、4 常數緩衝綁定（高 4 位元是 stage）、
// 5 IASetVertexBuffers；0 不走常駐入口。
// 客體 vCPU 數（建立串流時設定）：Map／Unmap 常駐入口的預設值依它決定。
std::atomic<unsigned>& d3d11_stream_guest_processors() noexcept {static std::atomic<unsigned> value{};return value;}
// 類別表是否含 Map／Unmap（6／7）：只看前端路徑與明確關閉；實際是否登錄常駐入口由 d3d11_stream_resident_map_active 決定。
bool d3d11_stream_resident_map_enabled() noexcept {
    static const bool enabled=[] {
        const auto* front=std::getenv("X64ARM_D3D11_STREAM_FRONT_MAP");const auto* value=std::getenv("X64ARM_D3D11_STREAM_RESIDENT_MAP");
        return (!front || std::string_view(front)!="0") && !(value && std::string_view(value)=="0");}();
    return enabled;
}
// Map(WRITE_DISCARD)／Unmap 留在轉譯碼內完成（核心類別 6／7）。微量測每次呼叫快約 4 倍。2026-11-11《古墓奇兵：崛起》地熱谷
//（中畫質 720p、每款程式獨立快取）：4 vCPU 50.1 → 67.7 FPS（中位 18.8 → 13.7 ms、P99 41.0 → 28.5 ms）；2 vCPU 50.4 → 47.7／47.8
//（變慢：vCPU 少於可執行客體執行緒時，服務離開點兼任排程點，渲染執行緒每幀少約 1.1 萬次離開後其他執行緒要等時間片輪替）。
// 3 vCPU 持平（58.2 對 58.0，敘利亞 77.2 對 71.3）。4 vCPU 重複兩輪 67.7／67.1，預設同日各輪 50.1～60.3。
// 預設在客體 vCPU 數 ≥ 4 時開啟；X64ARM_D3D11_STREAM_RESIDENT_MAP=1 一律開啟、=0 一律關閉。
bool d3d11_stream_resident_map_active() noexcept {
    if(!d3d11_stream_resident_map_enabled())return false;
    static const int forced=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_RESIDENT_MAP");return value && std::string_view(value)=="1"?1:0;}();
    return forced || d3d11_stream_guest_processors().load(std::memory_order_relaxed)>=4;
}
const std::vector<std::uint8_t>& d3d11_stream_resident_classes(){
    static const std::vector<std::uint8_t> table=[]{
        const auto descriptors=runtime_service_descriptors();std::vector<std::uint8_t> result(descriptors.size());
        for(std::size_t i=0;i<descriptors.size();++i){
            if(descriptors[i].family!="internal-d3d11")continue;
            switch(d3d11_stream_classes()[i]){
            // CopyStructureCount 要登記目的緩衝的待處理序號（見 d3d11_stream_enqueue），不走核心入口直接排入。
            case D3D11StreamClass::Scalar:if(descriptors[i].arguments<=6 && descriptors[i].symbol!="CopyStructureCount")result[i]=1;break;
            case D3D11StreamClass::Shader:if(descriptors[i].arguments==4)result[i]=2;break;
            case D3D11StreamClass::Pointers:if(descriptors[i].arguments==4)result[i]=3;break;
            case D3D11StreamClass::VertexBuffers:if(descriptors[i].arguments==6)result[i]=5;break;
            // 持久映射前端路徑的 Map(WRITE_DISCARD)／Unmap（X64ARM_D3D11_STREAM_RESIDENT_MAP=0 關閉，回到服務端的前端路徑）。
            case D3D11StreamClass::Map:if(descriptors[i].arguments==6 && d3d11_stream_resident_map_enabled())result[i]=6;break;
            case D3D11StreamClass::Unmap:if(descriptors[i].arguments==3 && d3d11_stream_resident_map_enabled())result[i]=7;break;
            case D3D11StreamClass::Constants:{
                const auto stage=d3d11_setter_method(descriptors[i]).stage;
                if(descriptors[i].arguments==4 && stage<6)result[i]=static_cast<std::uint8_t>(4U|(static_cast<unsigned>(stage)<<4U));break;}
            default:break;
            }
        }
        return result;
    }();
    return table;
}
// 核心常駐入口的資格（resident_clock_recipe 以配方值 4 標記）。
bool d3d11_stream_resident_scalar(std::size_t runtime_index) noexcept {
    if(!d3d11_stream_resident_enabled())return false;
    const auto& classes=d3d11_stream_resident_classes();
    if(runtime_index>=classes.size() || !classes[runtime_index])return false;
    // 類別 6／7 要核心入口認得：舊核心會把它們當成其他類別處理。
    return (classes[runtime_index]&15U)<6 || (d3d11_stream_resident_map_active() &&
        (std::atomic_ref(d3d11_resident_descriptor().kernel_features).load(std::memory_order_acquire)&1U));
}
namespace {
// 共用區配置（環狀，只有擁有者配置；串流執行緒處理完一筆就推進 arena_tail）。head==tail 代表空，所以不能剛好填滿。
std::optional<std::uint64_t> d3d11_stream_arena_alloc(D3D11CommandStream& stream,std::size_t bytes){
    bytes=(bytes+15)&~std::size_t{15};
    if(bytes>D3D11CommandStream::arena_item_limit)return std::nullopt;
    const auto head=stream.arena_head,tail=stream.arena_tail.load(std::memory_order_acquire);
    std::uint64_t at=head;
    if(head>=tail){
        if(D3D11CommandStream::arena_bytes-head<bytes){if(tail<=bytes)return std::nullopt;at=0;}
    }else if(tail-head<=bytes)return std::nullopt;
    stream.arena_head=at+bytes;
    return at;
}
// 快照區配置（規則同共用區，只存在宿主、客體看不到）。先用擁有者快取的 tail，不夠才重讀（見 snapshot_cached_tail）。
std::optional<std::uint64_t> d3d11_stream_snapshot_alloc(D3D11CommandStream& stream,std::size_t bytes){
    bytes=(bytes+15)&~std::size_t{15};
    const auto head=stream.snapshot_head;
    const auto place=[&](std::uint64_t tail)->std::optional<std::uint64_t>{
        std::uint64_t at=head;
        if(head>=tail){
            if(D3D11CommandStream::snapshot_bytes-head<bytes){if(tail<=bytes)return std::nullopt;at=0;}
        }else if(tail-head<=bytes)return std::nullopt;
        return at;
    };
    auto at=place(stream.snapshot_cached_tail);
    if(!at){stream.snapshot_cached_tail=stream.snapshot_tail.load(std::memory_order_acquire);at=place(stream.snapshot_cached_tail);}
    if(!at)return std::nullopt;
    stream.snapshot_head=*at+bytes;
    return at;
}
// 讀客體記憶體。locked=false 時只用頁快取，沒命中回傳 false，呼叫端取映射共用鎖後以 locked=true 重來（順便填快取；不能走原生視窗的頁用一般讀取，故障落在原呼叫點）。
bool d3d11_stream_read(D3D11CommandStream& stream,GuestMemory& memory,std::uint64_t address,void* output,std::size_t bytes,const bool locked){
    auto* destination=static_cast<std::uint8_t*>(output);
    constexpr std::uint64_t page_size=GuestMemory::guest_page_size;
    while(bytes){
        const auto page=address/page_size;const auto offset=address%page_size;const auto chunk=std::min<std::size_t>(bytes,page_size-offset);
        auto& entry=stream.pages[page%stream.pages.size()];
        if(entry.page!=page || entry.generation!=stream.page_generation){
            if(!locked)return false;
            const auto window=memory.native_window(GuestAddress{page*page_size},1);
            if(window.data && window.after>=page_size){entry.page=page;entry.generation=stream.page_generation;entry.host=window.data;}
            else{
                memory.read(GuestAddress{address},std::span<std::uint8_t>(destination,chunk));
                address+=chunk;destination+=chunk;bytes-=chunk;continue;
            }
        }
        std::memcpy(destination,entry.host+offset,chunk);
        address+=chunk;destination+=chunk;bytes-=chunk;
    }
    return true;
}
// 實驗（X64ARM_D3D11_STREAM_RELAXED_PUBLISH=1）：排入以 release 發布 head、relaxed 讀 sleeping，不做每筆 seq_cst 的 Dekker 往返
// （寫入後緊接讀取要等 head 的快取線從串流執行緒的核心搶回並對外可見）；串流執行緒閒置時改為 50 µs 輪詢，不會漏掉喚醒。
bool d3d11_stream_relaxed_publish() noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_RELAXED_PUBLISH");return value && std::string_view(value)=="1";}();
    return enabled;
}
enum class D3D11StreamQueued:std::uint8_t {Done,Miss,Reject};
// 把合成框架寫進槽內。沒有發布就沒有任何作用：Miss 由呼叫端持鎖重來，Reject（放不下、數量超限）走同步路徑重新讀取，故障落在原呼叫點。
D3D11StreamQueued d3d11_stream_enqueue(D3D11CommandStream& stream,GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,const D3D11StreamClass kind,const std::size_t update_bytes,const bool locked){
    const auto head=stream.head.load(std::memory_order_relaxed);
    const auto index=head&(stream.slots-1);
    const auto slot=stream.guest_base+index*D3D11CommandStream::slot_bytes;
    auto* const host=stream.host_base+index*D3D11CommandStream::slot_bytes;
    // x64 框架：sp≡8 (mod 16)；[sp] 返回位址、[sp+8,+40) home space、sp+40 起第 5 個以後的參數（最多 5 個）；陣列接在後面。
    constexpr std::size_t stack_offset=48,arrays_offset=96,capacity=D3D11CommandStream::slot_bytes-arrays_offset;
    std::array<std::uint64_t,9> values{};
    if(service.arguments>values.size())return D3D11StreamQueued::Reject;
    const auto arena_mark=stream.arena_head;
    bool miss=false;std::size_t used=0;
    const auto sp=state.reg(Gpr::Rsp);
    std::uint64_t return_target=0;
    values[0]=state.reg(Gpr::Rcx);values[1]=state.reg(Gpr::Rdx);values[2]=state.reg(Gpr::R8);values[3]=state.reg(Gpr::R9);
    if(!d3d11_stream_read(stream,memory,sp,&return_target,8,locked))return D3D11StreamQueued::Miss;
    if(service.arguments>4 && !d3d11_stream_read(stream,memory,sp+40,values.data()+4,std::size_t{service.arguments-4U}*8,locked))return D3D11StreamQueued::Miss;
    // 來源非空時複製 bytes 位元組並把參數改指向副本；放不下槽位就放共用區。
    const auto copy=[&](unsigned which,std::size_t bytes){
        if(!values[which] || !bytes)return true;
        if(bytes<=capacity-used){
            if(!d3d11_stream_read(stream,memory,values[which],host+arrays_offset+used,bytes,locked)){miss=true;return false;}
            values[which]=slot+arrays_offset+used;used+=(bytes+7)&~std::size_t{7};used=std::min(used,capacity);
            return true;
        }
        const auto at=d3d11_stream_arena_alloc(stream,bytes);if(!at)return false;
        if(!d3d11_stream_read(stream,memory,values[which],stream.arena_host+*at,bytes,locked)){miss=true;return false;}
        values[which]=stream.arena_base+*at;
        return true;
    };
    const auto count=[&](unsigned which){return static_cast<std::uint32_t>(values[which]);};
    const auto failed=[&]{stream.arena_head=arena_mark;return miss?D3D11StreamQueued::Miss:D3D11StreamQueued::Reject;};
    switch(kind){
    case D3D11StreamClass::Scalar:case D3D11StreamClass::Query:case D3D11StreamClass::Unmap:case D3D11StreamClass::ClearDepth:case D3D11StreamClass::Release:case D3D11StreamClass::Present:break;
    case D3D11StreamClass::Shader:if(count(3))return D3D11StreamQueued::Reject;break; // ClassInstance 要核對 linkage，走同步。
    case D3D11StreamClass::Constants:{
        const auto first=count(1),n=count(2);
        if(first>14 || n>14-first || !copy(3,std::size_t{n}*8))return failed();
        static const bool enabled=[] {
            const auto* value=std::getenv("X64ARM_D3D11_STREAM_CONSTANT_DEDUP");return !value || std::string_view(value)!="0";
        }();
        if(!enabled)break;
        const auto epoch=stream.constant_epoch.load(std::memory_order_acquire);
        if(epoch!=stream.constant_epoch_seen){
            for(auto& stage:stream.constant_bindings)stage.valid=0;
            stream.constant_epoch_seen=epoch;
        }
        auto& stage=stream.constant_bindings[d3d11_setter_method(service).stage];
        const auto mask=((1U<<n)-1U)<<first;
        std::array<std::uint64_t,14> buffers{};
        // 這種命令只有一組最多 14 個指標的陣列，必定放在槽內的 arrays_offset。
        if(values[3] && n)std::memcpy(buffers.data(),host+arrays_offset,std::size_t{n}*8);
        if((stage.valid&mask)==mask && std::equal(buffers.begin(),buffers.begin()+n,stage.values.begin()+first)){
            state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)=sp+8;
            stream.constant_skips.store(stream.constant_skips.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
            return D3D11StreamQueued::Done;
        }
        std::copy_n(buffers.begin(),n,stage.values.begin()+first);stage.valid|=mask;
        break;}
    case D3D11StreamClass::Pointers:if(count(2)>128 || !copy(3,std::size_t{count(2)}*8))return failed();break;
    case D3D11StreamClass::VertexBuffers:{
        const auto n=std::size_t{count(2)};if(n>32 || !copy(3,n*8) || !copy(4,n*4) || !copy(5,n*4))return failed();break;}
    case D3D11StreamClass::Viewports:if(count(1)>16 || !copy(2,std::size_t{count(1)}*24))return failed();break;
    case D3D11StreamClass::Scissors:if(count(1)>16 || !copy(2,std::size_t{count(1)}*16))return failed();break;
    case D3D11StreamClass::Blend:if(!copy(2,16))return failed();break;
    case D3D11StreamClass::Clear:if(!copy(2,16))return failed();break;
    case D3D11StreamClass::Uavs:{const auto n=std::size_t{count(2)};if(n>64 || !copy(3,n*8) || !copy(4,n*4))return failed();break;}
    case D3D11StreamClass::Targets:{
        const auto n=std::size_t{count(1)};if(n>8)return failed();
        std::array<std::uint64_t,8> targets{};
        if(values[2] && n && !d3d11_stream_read(stream,memory,values[2],targets.data(),n*8,locked))return D3D11StreamQueued::Miss;
        if(!copy(2,n*8))return failed();
        stream.shadow_targets=targets;stream.shadow_depth=values[3];stream.shadow_valid=true;
        break;}
    case D3D11StreamClass::TargetsUavs:{
        // 數量 0xffffffff 是 KEEP 常數，不讀對應陣列（規格要求傳空指標，這裡也不複製）。
        const auto rtv=count(1),uav=count(5);
        if(rtv!=UINT32_MAX && (rtv>8 || !copy(2,std::size_t{rtv}*8)))return failed();
        if(uav!=UINT32_MAX && (uav>64 || !copy(6,std::size_t{uav}*8) || !copy(7,std::size_t{uav}*4)))return failed();
        break;}
    case D3D11StreamClass::Update:if(!copy(3,24) || !copy(4,update_bytes))return failed();break;
    case D3D11StreamClass::Copy:if(service.arguments>3 && !copy(8,24))return failed();break;
    default:return D3D11StreamQueued::Reject;
    }
    // UAV 可能讓輸出目標被解除綁定：這之後 OMGetRenderTargets 要看實際狀態。
    if(kind==D3D11StreamClass::Uavs || kind==D3D11StreamClass::TargetsUavs)stream.shadow_valid=false;
    for(unsigned i=4;i<service.arguments;++i)std::memcpy(host+stack_offset+(i-4)*8,&values[i],8);
    auto& entry=stream.entries[index];
    entry.service=&service;entry.rip=state.rip.value;entry.rcx=values[0];entry.rdx=values[1];entry.r8=values[2];entry.r9=values[3];entry.rsp=slot+8;entry.arena_end=stream.arena_head;entry.snapshot_at=UINT64_MAX;entry.snapshot_end=stream.snapshot_head;
    // ClearDepthStencilView 的深度是第 4 個參數，x64 放在 XMM3。
    if(kind==D3D11StreamClass::ClearDepth)for(unsigned i=0;i<4;++i)entry.xmm3[i]=state.xmm[3][i];
    entry.present=kind==D3D11StreamClass::Present;
    state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)=sp+8;
    // CopyStructureCount 寫入目的緩衝（第 2 個參數）：之後 Map 這個緩衝要排在它之後，和 Copy 的目的相同。
    const bool structure_count=kind==D3D11StreamClass::Scalar && service.arguments==4 && service.symbol=="CopyStructureCount";
    if(kind==D3D11StreamClass::Query || kind==D3D11StreamClass::Unmap || kind==D3D11StreamClass::Update || kind==D3D11StreamClass::Copy || structure_count){
        // 舊項目在處理完之後就沒有作用，偶爾清掉避免無限成長（位址重用時最多多等到已完成的序號，不影響正確性）。
        if(stream.pending.size()>4096){const auto done=stream.tail.load(std::memory_order_acquire);std::erase_if(stream.pending,[&](const auto& item){return item.second<=done;});}
        stream.pending[values[1]]=head+1;if(auto* const front=stream.front_maps.find(values[1]))front->pending=head+1;
        // Copy 的來源：之後 Map 寫入來源要排在這個 Copy 之後。CopyResource 的來源是第 3 個參數，CopySubresourceRegion 是第 7 個。
        if(kind==D3D11StreamClass::Copy){const auto source=values[service.arguments>3?6:2];stream.pending[source]=head+1;if(auto* const front=stream.front_maps.find(source))front->pending=head+1;}
    }
    if(d3d11_stream_relaxed_publish()){
        stream.head.store(head+1,std::memory_order_release);
        if(stream.sleeping.load(std::memory_order_relaxed)){stream.sleeping.store(0,std::memory_order_relaxed);stream.sleeping.notify_one();}
    }else{
        stream.head.store(head+1,std::memory_order_seq_cst);
        if(stream.sleeping.load(std::memory_order_seq_cst)){stream.sleeping.store(0,std::memory_order_seq_cst);stream.sleeping.notify_one();}
    }
    stream.enqueued.store(stream.enqueued.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
    return D3D11StreamQueued::Done;
}
inline void d3d11_stream_pause_cpu() noexcept {
#if defined(__aarch64__)
    __builtin_arm_yield();
#endif
}
// 等到序號處理完。不可持映射或圖形鎖。
void d3d11_stream_wait(D3D11CommandStream& stream,const std::uint64_t target) noexcept {
    for(unsigned spins=0;stream.tail.load(std::memory_order_acquire)<target;++spins){
        if(spins<4096){d3d11_stream_pause_cpu();continue;}
        std::this_thread::yield();
    }
}
}
void WindowsServices::d3d11_stream_drain() noexcept {
    if(!d3d11_stream_)return;
    auto& stream=*d3d11_stream_;
    const auto target=stream.head.load(std::memory_order_acquire);
    if(stream.tail.load(std::memory_order_acquire)==target && !stream.foreign_ready.load(std::memory_order_acquire))return;
    const auto started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    stream.drain_waits.fetch_add(1,std::memory_order_relaxed);
    d3d11_stream_wait(stream,target);
    for(unsigned spins=0;stream.foreign_ready.load(std::memory_order_acquire);++spins){
        if(spins<4096){d3d11_stream_pause_cpu();continue;}
        std::this_thread::yield();
    }
    stream.drain_ns.fetch_add(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started,std::memory_order_relaxed);
}
// 客體已恢復執行（worker 或協調服務進入）：解除全停時的暫停。
void WindowsServices::d3d11_stream_resume() noexcept {
    if(!d3d11_stream_)return;
    auto& stream=*d3d11_stream_;
    if(!stream.paused.load(std::memory_order_relaxed))return;
    if(detail::parallel_mapping_mutex().held_exclusive())return;
    stream.paused.store(0,std::memory_order_seq_cst);stream.paused.notify_one();
}
WindowsServices::D3D11StreamStep WindowsServices::d3d11_stream_entry(GuestState& state,GuestMemory& memory){
    auto& stream=*d3d11_stream_;
    d3d11_stream_resume();
    const auto* const descriptor=detail::runtime_stub_descriptor(stub_base_,state.rip);
    if(!descriptor)return D3D11StreamStep::Continue;
    const auto service_index=static_cast<std::size_t>(descriptor-runtime_service_descriptors().data());
    const auto kind=d3d11_stream_classes()[service_index];
    if(kind==D3D11StreamClass::Other || kind==D3D11StreamClass::NoDrain)return D3D11StreamStep::Continue;
    const auto stub_rip=state.rip.value;
    // 客體 vCPU 數在建立串流之後才設定（set_processor_count），在這裡跟上目前的值；只在改變時寫入。
    if(const auto processors=static_cast<unsigned>(runtime_.processor_count);d3d11_stream_guest_processors().load(std::memory_order_relaxed)!=processors)
        d3d11_stream_guest_processors().store(processors,std::memory_order_relaxed);
    // 純量方法改由核心入口直接排入：C++ 排入一次後請求登錄（d3d11_stream_resident_request）。
    const auto scalar_done=[&]{if(std::atomic_ref(d3d11_resident_descriptor().enabled).load(std::memory_order_relaxed) &&
        d3d11_stream_resident_scalar(service_index))d3d11_stream_resident_request(service_index,stub_rip);};
    // 前端 Map／Unmap 成功後同樣請求登錄核心入口（類別 6／7）。
    const auto front_done=scalar_done;
    // 串流執行緒失敗：交回協調者，由完整服務邊界重新拋出。
    if(stream.failed.load(std::memory_order_relaxed))return D3D11StreamStep::Refuse;
    const bool owner=state.reg(Gpr::Rcx) && state.reg(Gpr::Rcx)==stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed);
    const auto sync=[&]{
        // 同步方法之後的輸出目標可能變了（例如 ClearState），影子失效到下一次排入 OMSetRenderTargets。
        if(owner || state.reg(Gpr::Rcx)==stream.context.load(std::memory_order_relaxed)){
            stream.shadow_valid=false;
        }
        // 新版介面的 this 可能是同一個 context 的另一個指標；同步路徑一律讓綁定快取失效。
        stream.constant_epoch.fetch_add(1,std::memory_order_release);
        if(stream.head.load(std::memory_order_relaxed)!=stream.tail.load(std::memory_order_acquire))stream.drains_by_service[service_index].fetch_add(1,std::memory_order_relaxed);
        d3d11_stream_drain();return D3D11StreamStep::Continue;
    };
    if(kind==D3D11StreamClass::Release){
        // 非最後一次釋放只改計數。最後一次：佇列裡可能還有對它的參照，要排在那些命令之後；客體已經沒有參照，回傳值固定是 0。
        // 擁有者以外的執行緒不能排入，先清空。
        bool done=false;
        {
            const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
            std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
            try{
                const auto release=d3d11_stream_release(state,memory,*descriptor);
                if(release==1)done=true;
                else if(release==2 && stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed) &&
                        stream.head.load(std::memory_order_relaxed)-stream.tail.load(std::memory_order_acquire)<stream.slots){
                    stream.page_generation=memory.native_borrow_generation();
                    done=d3d11_stream_enqueue(stream,state,memory,*descriptor,kind,0,true)==D3D11StreamQueued::Done;
                }
            }catch(const MemoryFault&){}catch(const HostCallFault&){}
        }
        if(done){state.rep_compare={};return D3D11StreamStep::Done;}
        return sync();
    }
    if(kind==D3D11StreamClass::Present){
        // Present 也排入：客體不必等這一幀做完才開始下一幀（DXGI 本來就允許排隊，預設最多 3 幀），佇列裡最多一個 Present。
        // 不合資格（別的執行緒、TEST／DO_NOT_WAIT、測試用關閉幀數）照舊走全停的完整服務，但先在這裡等佇列清空，其他執行緒不必跟著等。
        static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_PRESENT");return !value || std::string_view(value)!="0";}();
        // 有些引擎用獨立執行緒呈現，它不是環的生產者：走信箱，不必排空也不必全停。X64ARM_D3D11_STREAM_FOREIGN_PRESENT=0 關閉。
        static const bool foreign_enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_FOREIGN_PRESENT");return !value || std::string_view(value)!="0";}();
        if(enabled && foreign_enabled && state.gs_base!=stream.owner.load(std::memory_order_relaxed) &&
           !(static_cast<std::uint32_t>(state.reg(Gpr::R8))&~0x200U) && !graphics_close_frame_limit()){
            // DXGI 的排隊上限由這裡與命令執行緒的「最多一個呈現」共同擋住。全停要等全部 worker 停下才會暫停串流執行緒，這裡不會互等。
            for(unsigned spins=0;stream.foreign_ready.load(std::memory_order_acquire);++spins){
                if(stream.failed.load(std::memory_order_relaxed))return D3D11StreamStep::Refuse;
                if(spins<4096){d3d11_stream_pause_cpu();continue;}
                std::this_thread::yield();
            }
            bool done=false;
            {
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
                try{
                    if(d3d11_stream_present_eligible(state)){
                        const auto sp=state.reg(Gpr::Rsp);
                        const auto return_target=memory.load(GuestAddress{sp},64);
                        auto& entry=stream.foreign;
                        entry=D3D11CommandStream::Entry{};
                        entry.service=descriptor;entry.rip=state.rip.value;entry.rcx=state.reg(Gpr::Rcx);entry.rdx=state.reg(Gpr::Rdx);entry.r8=state.reg(Gpr::R8);entry.r9=state.reg(Gpr::R9);
                        // Present 只有三個暫存器參數，處理程式只從 sp 讀返回位址：指向不會被環寫入的槽就好。
                        entry.rsp=stream.guest_base+(D3D11CommandStream::slot_capacity-1)*D3D11CommandStream::slot_bytes+8;
                        entry.present=true;
                        stream.foreign_after=stream.head.load(std::memory_order_acquire);
                        state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)=sp+8;
                        done=true;
                    }
                }catch(const MemoryFault&){}catch(const HostCallFault&){}
            }
            if(done){
                present_thread_=resident_context_key(state);
                stream.foreign_presents.fetch_add(1,std::memory_order_release);
                stream.foreign_ready.store(1,std::memory_order_seq_cst);
                if(stream.sleeping.load(std::memory_order_seq_cst)){stream.sleeping.store(0,std::memory_order_seq_cst);stream.sleeping.notify_one();}
                state.rep_compare={};return D3D11StreamStep::Done;
            }
        }
        if(enabled && stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed) &&
           !(static_cast<std::uint32_t>(state.reg(Gpr::R8))&~0x200U) && !graphics_close_frame_limit()){
            d3d11_stream_wait(stream,stream.present_sequence);
            const auto head=stream.head.load(std::memory_order_relaxed);
            if(head-stream.cached_tail>=stream.slots){stream.cached_tail=stream.tail.load(std::memory_order_acquire);if(head-stream.cached_tail>=stream.slots)d3d11_stream_wait(stream,head-stream.slots+64);}
            stream.page_generation=memory.native_borrow_generation();
            bool done=false;
            {
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
                try{if(d3d11_stream_present_eligible(state))done=d3d11_stream_enqueue(stream,state,memory,*descriptor,kind,0,true)==D3D11StreamQueued::Done;}
                catch(const MemoryFault&){}catch(const HostCallFault&){}
            }
            if(done){
                // 處理程式在串流執行緒上不更新：著色器背景編譯要認得呈現執行緒。
                present_thread_=resident_context_key(state);
                stream.present_sequence=head+1;stream.shadow_valid=false;state.rep_compare={};return D3D11StreamStep::Done;
            }
        }
        d3d11_stream_drain();
        return D3D11StreamStep::Coordinator;
    }
    if(!owner || kind==D3D11StreamClass::Sync)return sync();
    if(kind==D3D11StreamClass::Map){
        // 同一資源的 Unmap 還沒處理時，客體不能拿到（同一塊）區塊來寫。Map 本身照常同步，不依賴其他命令。DO_NOT_WAIT 不等（見 d3d11_map_do_not_wait）。
        if(const auto found=stream.pending.find(state.reg(Gpr::Rdx));found!=stream.pending.end() && stream.tail.load(std::memory_order_acquire)<found->second &&
           !d3d11_map_do_not_wait(state,memory,true))d3d11_stream_wait(stream,found->second);
        if(!static_cast<std::uint32_t>(state.reg(Gpr::R8)) && static_cast<std::uint32_t>(state.reg(Gpr::R9))==4){
            if(const auto epoch=stream.front_epoch.load(std::memory_order_acquire);epoch!=stream.front_epoch_seen){
                const std::lock_guard lock(stream.front_dropped_mutex);
                for(const auto pointer:stream.front_dropped)if(auto* const dropped=stream.front_maps.find(pointer);dropped && !dropped->mapped)stream.front_maps.erase(dropped);
                stream.front_dropped.clear();stream.front_epoch_seen=epoch;
            }
            if(auto* const found=stream.front_maps.find(state.reg(Gpr::Rdx));found && !found->mapped){
                // [sp] 返回位址、[sp+40] MapFlags、[sp+48] D3D11_MAPPED_SUBRESOURCE*。旗標只接受 0 與 DO_NOT_WAIT（這裡不會等 GPU）。
                const auto sp=state.reg(Gpr::Rsp);std::uint64_t return_target=0;std::array<std::uint64_t,2> stack{};bool done=false;
                {
                    const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                    try{
                        stream.page_generation=memory.native_borrow_generation();
                        if(d3d11_stream_read(stream,memory,sp,&return_target,8,true) && d3d11_stream_read(stream,memory,sp+40,stack.data(),16,true) &&
                           !(static_cast<std::uint32_t>(stack[0])&~0x100000U) && stack[1] && stack[1]<=UINT64_MAX-15 &&
                           !memory.is_copy_on_write(GuestAddress{stack[1]}) && !memory.is_copy_on_write(GuestAddress{stack[1]+15})){
                            const std::array<std::uint64_t,2> result{found->pointer,found->row|(std::uint64_t{found->depth}<<32)};
                            memory.write(GuestAddress{stack[1]},{reinterpret_cast<const std::uint8_t*>(result.data()),sizeof(result)});
                            done=true;
                        }
                    }catch(const MemoryFault&){done=false;}catch(const HostCallFault&){done=false;}
                }
                if(done){
                    found->mapped=1;
                    stream.front_maps_taken.store(stream.front_maps_taken.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
                    front_done();
                    state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)=sp+8;state.rep_compare={};
                    return D3D11StreamStep::Done;
                }
            }
        }
        return D3D11StreamStep::Continue;
    }
    if(kind==D3D11StreamClass::GetData){
        if(const auto found=stream.pending.find(state.reg(Gpr::Rdx));found!=stream.pending.end() && stream.tail.load(std::memory_order_acquire)<found->second){
            // 尚未處理到這個查詢的 End：回 S_FALSE。讀返回位址要持映射鎖；故障時走同步路徑。
            bool done=false;
            {
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                try{const WindowsCall arg(state,memory,*descriptor);arg.return_to(state,1);done=true;}
                catch(const MemoryFault&){}catch(const HostCallFault&){}
            }
            if(done){state.rep_compare={};return D3D11StreamStep::Done;}
            return sync();
        }
        return D3D11StreamStep::Continue;
    }
    if(kind==D3D11StreamClass::GetTargets){
        if(const auto presents=stream.foreign_presents.load(std::memory_order_acquire);presents!=stream.foreign_presents_seen){stream.foreign_presents_seen=presents;stream.shadow_valid=false;}
        if(stream.shadow_valid){
            bool done=false;
            {
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
                try{done=d3d11_stream_get_targets(state,memory,*descriptor);}
                catch(const MemoryFault&){}catch(const HostCallFault&){}
            }
            if(done){state.rep_compare={};return D3D11StreamStep::Done;}
        }
        return sync();
    }
    // 環滿：不持鎖等一個空位，不退回同步（同步要等整段積壓，還會打亂節奏）。多等一些，避免每排一筆就和串流執行緒搶一次快取線。
    const auto head=stream.head.load(std::memory_order_relaxed);
    if(head-stream.cached_tail>=stream.slots){
        stream.cached_tail=stream.tail.load(std::memory_order_acquire);
        if(head-stream.cached_tail>=stream.slots){
            const auto started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            stream.full_waits.store(stream.full_waits.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
            // 等出八分之一的空位再繼續：只等 64 格時生產端（尤其核心入口）每排幾十筆就與串流執行緒搶一次 tail 的快取線。
            d3d11_stream_wait(stream,head-stream.slots+stream.slots/8);
            stream.full_ns.store(stream.full_ns.load(std::memory_order_relaxed)+clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started,std::memory_order_relaxed);
            stream.cached_tail=stream.tail.load(std::memory_order_acquire);
        }
    }
    stream.page_generation=memory.native_borrow_generation();
    if(kind==D3D11StreamClass::Unmap && !static_cast<std::uint32_t>(state.reg(Gpr::R8))){
        if(auto* const found=stream.front_maps.find(state.reg(Gpr::Rdx));found && found->mapped){
            const auto sp=state.reg(Gpr::Rsp);std::uint64_t return_target=0;
            if(!d3d11_stream_read(stream,memory,sp,&return_target,8,false)){
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                (void)d3d11_stream_read(stream,memory,sp,&return_target,8,true);
            }
            // 快照區滿：等串流執行緒消化（與環滿相同，不持鎖；全停要等全部 worker 停下才會暫停串流執行緒，不會互等）。
            std::optional<std::uint64_t> at;
            for(unsigned spins=0;!(at=d3d11_stream_snapshot_alloc(stream,found->size));++spins){
                if(stream.failed.load(std::memory_order_relaxed))return D3D11StreamStep::Refuse;
                if(spins<4096){d3d11_stream_pause_cpu();continue;}
                std::this_thread::yield();
            }
            auto* const copy_to=stream.snapshot.get()+*at;
            if(!d3d11_stream_read(stream,memory,found->pointer,copy_to,found->size,false)){
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                (void)d3d11_stream_read(stream,memory,found->pointer,copy_to,found->size,true);
            }
            auto& entry=stream.entries[head&(stream.slots-1)];
            entry.service=descriptor;entry.rip=state.rip.value;entry.rcx=state.reg(Gpr::Rcx);entry.rdx=state.reg(Gpr::Rdx);entry.r8=0;entry.r9=0;entry.rsp=0;entry.arena_end=stream.arena_head;
            entry.snapshot_at=*at;entry.snapshot_size=found->size;entry.snapshot_end=stream.snapshot_head;entry.present=false;
            found->mapped=0;
            state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)=sp+8;state.rep_compare={};
            stream.head.store(head+1,std::memory_order_seq_cst);
            if(stream.sleeping.load(std::memory_order_seq_cst)){stream.sleeping.store(0,std::memory_order_seq_cst);stream.sleeping.notify_one();}
            stream.enqueued.store(stream.enqueued.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
            stream.front_unmaps_taken.store(stream.front_unmaps_taken.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
            front_done();
            return D3D11StreamStep::Done;
        }
    }
    if(const auto epoch=stream.update_epoch.load(std::memory_order_acquire);epoch!=stream.update_epoch_seen){stream.update_widths.clear();stream.update_epoch_seen=epoch;}
    // 更新緩衝的大小：快取命中時不取任何鎖。區域超出範圍或空的交回同步路徑（錯誤回報與 no-op 語義都在那裡）。
    const auto update_size=[&](std::uint32_t width,std::size_t& bytes,const bool locked)->std::optional<bool> {
        std::uint32_t first=0,last=width;
        if(const auto box=state.reg(Gpr::R9)){
            std::array<std::uint32_t,6> values{};
            if(!d3d11_stream_read(stream,memory,box,values.data(),24,locked))return std::nullopt;
            first=values[0];last=values[3];
        }
        if(first>=last || last>width)return false;
        bytes=last-first;return true;
    };
    std::size_t update_bytes=0;bool lock_free=true,eligible=true;
    if(kind==D3D11StreamClass::Update){
        const auto cached=static_cast<std::uint32_t>(state.reg(Gpr::R8))?stream.update_widths.end():stream.update_widths.find(state.reg(Gpr::Rdx));
        if(cached==stream.update_widths.end())lock_free=false;
        else if(const auto sized=update_size(cached->second,update_bytes,false);!sized)lock_free=false;
        else eligible=*sized;
    }else if(kind==D3D11StreamClass::Unmap || kind==D3D11StreamClass::Copy)lock_free=false;
    if(!eligible)return sync();
    if(lock_free){
        const auto queued=d3d11_stream_enqueue(stream,state,memory,*descriptor,kind,update_bytes,false);
        if(queued==D3D11StreamQueued::Done){state.rep_compare={};scalar_done();return D3D11StreamStep::Done;}
        if(queued==D3D11StreamQueued::Reject){stream.rejected.store(stream.rejected.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return sync();}
    }
    // 頁快取沒命中或要看物件狀態：持共用映射鎖重來（順便填快取）；故障時放棄這一槽，走同步路徑。
    bool done=false;
    {
        const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
        try{
            stream.locked_enqueues.store(stream.locked_enqueues.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
            if(kind==D3D11StreamClass::Update || kind==D3D11StreamClass::Unmap || kind==D3D11StreamClass::Copy){
                // 這幾種要看物件狀態（持久映射、緩衝大小），持圖形鎖核對；資格與大小之外不必，但很短，一起持鎖比較單純。
                std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
                std::uint32_t width=0;
                eligible=d3d11_stream_object_eligible(state,memory,*descriptor,kind==D3D11StreamClass::Update,width);
                if(eligible && kind==D3D11StreamClass::Update){
                    const auto sized=update_size(width,update_bytes,true);
                    eligible=sized && *sized;
                    if(eligible && !static_cast<std::uint32_t>(state.reg(Gpr::R8)))stream.update_widths[state.reg(Gpr::Rdx)]=width;
                }
            }
            if(eligible)done=d3d11_stream_enqueue(stream,state,memory,*descriptor,kind,update_bytes,true)==D3D11StreamQueued::Done;
        }
        catch(const MemoryFault&){}catch(const HostCallFault&){}
    }
    if(done){state.rep_compare={};scalar_done();return D3D11StreamStep::Done;}
    stream.rejected.store(stream.rejected.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
    // 非持久的 Unmap 交給協調者本地路徑（要釋放 heap）；與 Map 相同只等同一資源上排隊的命令，不等整條佇列。
    if(kind==D3D11StreamClass::Unmap){
        if(const auto found=stream.pending.find(state.reg(Gpr::Rdx));found!=stream.pending.end())d3d11_stream_wait(stream,found->second);
        return D3D11StreamStep::Continue;
    }
    return sync();
}
// Present 能否交給串流執行緒：本機視窗的交換鏈（遠端串流要在協調者發布）、後緩衝已建立。呼叫端持映射共用鎖與圖形鎖。
bool WindowsServices::d3d11_stream_present_eligible(const GuestState& state) const {
    if(!d3d11_)return false;
    const auto found=d3d11_->objects.find_fast(state.reg(Gpr::Rcx));
    if(found==d3d11_->objects.end() || found->second.kind!=Kind::SwapChain || !found->second.identity)return false;
    const auto& swap=*found->second.identity;
    if(swap.api10 || !swap.native || !gui_.windows.contains(swap.window) || !swap.bindings.contains("Backbuffers"))return false;
    const auto device=d3d11_->objects.find_fast(swap.device);
    return device!=d3d11_->objects.end() && device->second.identity && device->second.identity->native;
}
// 串流執行緒處理 Present 之前：等顯示緩衝與上一個呈現時不持鎖（等待可能到一次垂直同步），也不擋全停。顯示緩衝約兩幀後照舊往下走。
void WindowsServices::d3d11_stream_prepare_present(D3D11CommandStream& stream,const std::uint64_t swap_pointer){
    std::shared_ptr<NativeOpenGlContext> swap;
    {
        std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
        if(const auto found=d3d11_->objects.find_fast(swap_pointer);found!=d3d11_->objects.end() && found->second.identity)swap=found->second.identity->native;
    }
    if(!swap)return;
    // 等待期間不碰客體記憶體：放下 busy，全停不必等這段；結束後若正在全停就等它結束才繼續。
    const auto outside_busy=[&](const auto& work){
        stream.busy.store(0,std::memory_order_seq_cst);
        std::exception_ptr work_failure;
        try{work();}catch(...){work_failure=std::current_exception();}
        for(;;){
            stream.busy.store(1,std::memory_order_seq_cst);
            if(!stream.paused.load(std::memory_order_seq_cst) || stream.stopping.load(std::memory_order_acquire))break;
            stream.busy.store(0,std::memory_order_seq_cst);
            stream.paused.wait(1,std::memory_order_acquire);
        }
        if(work_failure)std::rethrow_exception(work_failure);
    };
    const auto deadline=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)+40000000;
    while(!swap->graphics_present_ready() && clock_gettime_nsec_np(CLOCK_UPTIME_RAW)<deadline && !stream.stopping.load(std::memory_order_acquire))
        outside_busy([]{std::this_thread::sleep_for(std::chrono::microseconds(100));});
    const auto ready_at=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    stream.present_ready_ns.store(stream.present_ready_ns.load(std::memory_order_relaxed)+ready_at-(deadline-40000000),std::memory_order_relaxed);
    // 命令執行緒上最多一個呈現：GPU 跟不上時由這裡擋住，客體最多領先一幀。
    while(stream.presents_done->load(std::memory_order_acquire)<stream.presents_posted && !stream.stopping.load(std::memory_order_acquire))
        outside_busy([]{std::this_thread::sleep_for(std::chrono::microseconds(50));});
    stream.present_wait_ns.store(stream.present_wait_ns.load(std::memory_order_relaxed)+clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-ready_at,std::memory_order_relaxed);
}
// 串流執行緒把整段呈現投遞給命令執行緒，依序接在前面的繪製之後。
template<class Work>
void WindowsServices::d3d11_stream_post_present(const std::shared_ptr<NativeOpenGlContext>& producer,Work work){
    auto& stream=*d3d11_stream_;
    ++stream.presents_posted;stream.present_producer=producer;
    producer->graphics_post([work=std::move(work),done=stream.presents_done](NativeGraphics&){
        struct Done {std::atomic<std::uint64_t>& value;~Done(){value.fetch_add(1,std::memory_order_release);}} finished{*done};
        work();
    });
}
// OMGetRenderTargets 由影子回答：與 get_views 相同，每個傳回的介面都加一次參照。呼叫端持映射共用鎖與圖形鎖。
bool WindowsServices::d3d11_stream_get_targets(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    auto& stream=*d3d11_stream_;
    if(!d3d11_ || !runtime_.network.external_io || !d3d11_local_eligible(state,memory,service))return false;
    const WindowsCall arg(state,memory,service);
    const auto count=static_cast<std::uint32_t>(arg(1));const auto targets=arg(2),depth=arg(3);
    if(count>8)return false;
    // 共用映射鎖下不能做寫入時複製；輸出落在這種頁就走同步路徑。
    const auto writable=[&](std::uint64_t address,std::size_t bytes){return !memory.is_copy_on_write(GuestAddress{address}) && !memory.is_copy_on_write(GuestAddress{address+bytes-1});};
    if((targets && count && !writable(targets,std::size_t{count}*8)) || (depth && !writable(depth,8)))return false;
    const auto add=[&](std::uint64_t pointer){
        if(!pointer)return true;
        const auto found=d3d11_->objects.find_fast(pointer);
        return found!=d3d11_->objects.end() && found->second.identity && adjust_references(memory,*found->second.identity,true).has_value();
    };
    // 先核對全部物件都存在才開始加參照：中途失敗不能留下多的參照。
    const auto exists=[&](std::uint64_t pointer){
        if(!pointer)return true;
        const auto found=d3d11_->objects.find_fast(pointer);
        return found!=d3d11_->objects.end() && found->second.identity;
    };
    if(targets)for(std::uint32_t i=0;i<count;++i)if(!exists(stream.shadow_targets[i]))return false;
    if(depth && !exists(stream.shadow_depth))return false;
    if(targets)for(std::uint32_t i=0;i<count;++i){if(!add(stream.shadow_targets[i]))throw HostCallFault("D3D11 物件引用計數溢位");memory.store(GuestAddress{targets+i*8},64,stream.shadow_targets[i]);}
    if(depth){if(!add(stream.shadow_depth))throw HostCallFault("D3D11 物件引用計數溢位");memory.store(GuestAddress{depth},64,stream.shadow_depth);}
    arg.return_to(state,0);
    return true;
}
// COM Release：非最後一次與 invoke_d3d11 前置路徑相同，只改計數，回傳 1。最後一次且物件可以交給串流執行緒銷毀（與 worker 相同進延後清單）回傳 2；其他情況回傳 0。
// 裝置、內容與交換鏈的銷毀要走完整服務。呼叫端持映射共用鎖與圖形鎖。
unsigned WindowsServices::d3d11_stream_release(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& descriptor){
    if(!d3d11_ || !runtime_.network.external_io)return 0;
    const WindowsCall arg(state,memory,descriptor);
    const auto found=d3d11_->objects.find_fast(arg(0));
    if(found==d3d11_->objects.end() || !found->second.identity)return 0;
    const auto& id=*found->second.identity;
    if(const auto count=adjust_references(memory,id,false,1)){arg.return_to(state,*count);return 1;}
    switch(found->second.kind){
    case Kind::Texture:case Kind::BufferObject:case Kind::Texture1D:case Kind::Texture3D:
    case Kind::ShaderResource:case Kind::Unordered:case Kind::RenderTarget:case Kind::DepthView:
    case Kind::VertexShader:case Kind::PixelShader:case Kind::ComputeShader:case Kind::GeometryShader:case Kind::HullShader:case Kind::DomainShader:
    case Kind::Layout:case Kind::Blend:case Kind::DepthState:case Kind::Raster:case Kind::Sampler:case Kind::Query:break;
    default:return 0;
    }
    return reference_count(memory,id)==1?2U:0U;
}
// Update／Unmap／Copy 的資格：與 worker 上同步執行相同（d3d11_local_eligible），另外 Update 只收緩衝（來源大小不必套用貼圖的列距規則）。呼叫端持映射共用鎖與圖形鎖。
bool WindowsServices::d3d11_stream_object_eligible(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,const bool update,std::uint32_t& width){
    if(!d3d11_ || !d3d11_local_eligible(state,memory,service))return false;
    if(!update)return true;
    const WindowsCall arg(state,memory,service);
    const auto found=d3d11_->objects.find_fast(arg(1));
    if(found==d3d11_->objects.end() || !found->second.identity || !found->second.identity->resource)return false;
    const auto& id=*found->second.identity;const auto& desc=id.resource->desc;
    if(desc.dimension!=1 || id.subresource_maps.contains(static_cast<std::uint32_t>(arg(2))))return false;
    width=desc.width;
    return true;
}
// 在 worker 同步路徑成功之後、持映射共用鎖與圖形鎖時呼叫。完整服務會清除擁有者，下一個呼叫者重新認領。
void WindowsServices::d3d11_stream_claim(GuestMemory& memory,std::uint64_t context,std::uint64_t teb){
    auto& stream=*d3d11_stream_;
    if(stream.context.load(std::memory_order_relaxed) || !d3d11_ || parallel_workers()<2)return;
    const auto found=d3d11_->objects.find(context);
    if(found==d3d11_->objects.end() || found->second.kind!=Kind::Context || !found->second.identity)return;
    const auto& id=*found->second.identity;
    if(id.kind!=Kind::Context || id.context_type || id.api10 || id.weak_bindings || !id.native)return;
    const auto device=id.device?d3d11_->objects.find(id.device):found;
    if(device==d3d11_->objects.end() || !device->second.identity || (device->second.identity->protected_threads && device->second.identity->lock_address))return;
    if(!stream.started.load(std::memory_order_relaxed)){
        struct Start {WindowsServices* self;GuestMemory* memory;D3D11CommandStream* stream;};
        auto start=std::make_unique<Start>(Start{this,&memory,&stream});
        // 完整分派與銷毀遞迴、深入 Metal 的呼叫：與 D3D12 提交執行緒相同給 16 MiB 堆疊。
        pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,std::size_t{16}<<20);
        const auto created=pthread_create(&stream.thread,&attributes,[](void* opaque)->void*{
            const std::unique_ptr<Start> start(static_cast<Start*>(opaque));start->self->d3d11_stream_loop(*start->memory,*start->stream);return nullptr;},start.get());
        pthread_attr_destroy(&attributes);
        if(created!=0)return;
        (void)start.release();stream.started.store(true,std::memory_order_relaxed);
    }
    stream.pending.clear();stream.shadow_valid=false;
    for(auto& stage:stream.constant_bindings)stage.valid=0;
    // 同一執行緒重新認領（完整服務會清除擁有者）沿用快取；換人就清空，這張表是擁有者私有。
    if(stream.front_owner!=teb || stream.front_context!=context){stream.front_maps.clear();stream.front_owner=teb;stream.front_context=context;}
    stream.owner.store(teb,std::memory_order_relaxed);stream.context.store(context,std::memory_order_relaxed);
}
void WindowsServices::d3d11_stream_loop(GuestMemory& memory,D3D11CommandStream& stream){
    pthread_setname_np("x64arm.d3d11.stream");pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    graphics_local()=true; // 與 worker 本地服務相同：最後一次釋放進延後清單，不配置或釋放 heap。
    d3d11_on_stream_thread=true;
    const auto synthetic_storage=std::make_unique<GuestState>();
    auto& synthetic=*synthetic_storage;
    std::uint64_t tail=0,head=0;
    std::uint64_t ticks_per_20us=0;
#if defined(__aarch64__)
    {std::uint64_t frequency=0;asm volatile("mrs %0, cntfrq_el0":"=r"(frequency));ticks_per_20us=frequency/50000U;}
#endif
    const auto ticks=[]{std::uint64_t now=0;
#if defined(__aarch64__)
        asm volatile("mrs %0, cntvct_el0":"=r"(now));
#else
        now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
        return now;};
    if(!ticks_per_20us)ticks_per_20us=20000;
    for(;;){
        // 平均每 1 µs 一筆，睡眠的喚醒延遲會吃掉效益：先短暫自旋（約 20 µs），有上限遊戲閒置時才睡。
        if(head==tail){
            head=stream.head.load(std::memory_order_acquire);
            if(head==tail && !stream.foreign_ready.load(std::memory_order_acquire)){
                if(stream.stopping.load(std::memory_order_acquire))return;
                const auto until=ticks()+ticks_per_20us;
                while((head=stream.head.load(std::memory_order_acquire))==tail && !stream.foreign_ready.load(std::memory_order_acquire) && ticks()<until)d3d11_stream_pause_cpu();
                if(head==tail && !stream.foreign_ready.load(std::memory_order_acquire)){
                    const auto idle_started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    stream.sleeping.store(1,std::memory_order_seq_cst);
                    if(d3d11_stream_relaxed_publish()){
                        // 生產端不做 Dekker 往返：以短間隔輪詢，最多延遲一個間隔。
                        while(stream.head.load(std::memory_order_acquire)==tail && !stream.foreign_ready.load(std::memory_order_acquire) && !stream.stopping.load(std::memory_order_acquire))
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }else if(stream.head.load(std::memory_order_seq_cst)==tail && !stream.foreign_ready.load(std::memory_order_seq_cst) && !stream.stopping.load(std::memory_order_seq_cst))stream.sleeping.wait(1,std::memory_order_seq_cst);
                    stream.sleeping.store(0,std::memory_order_seq_cst);
                    stream.idle_ns.store(stream.idle_ns.load(std::memory_order_relaxed)+clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-idle_started,std::memory_order_relaxed);
                    continue;
                }
            }
        }
        // 全停（Dekker）：先公告正在處理再看暫停旗標；暫停者先設旗標再等 busy 歸零。一次處理一小批，暫停最多多等幾筆。
        stream.busy.store(1,std::memory_order_seq_cst);
        if(stream.paused.load(std::memory_order_seq_cst)){
            stream.busy.store(0,std::memory_order_seq_cst);
            const auto paused_started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            const auto until=ticks()+ticks_per_20us;
            while(stream.paused.load(std::memory_order_acquire) && ticks()<until)d3d11_stream_pause_cpu();
            if(stream.paused.load(std::memory_order_acquire) && !stream.stopping.load(std::memory_order_acquire))stream.paused.wait(1,std::memory_order_acquire);
            stream.paused_ns.store(stream.paused_ns.load(std::memory_order_relaxed)+clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-paused_started,std::memory_order_relaxed);
            continue;
        }
        // 整批上鎖：連續的非繪製命令共用一次映射共用鎖與圖形鎖（鎖序不變：映射 → 圖形），不再每筆各取放一次。
        // 重播執行緒是 4 vCPU 下滿載的一段（地熱谷每幀約 2.7 萬筆、閒置 0.26 ms，繪製執行緒每幀卡在環滿 3.9 ms），每筆兩把鎖約占它 14%。
        // 繪製與呈現照舊逐筆上鎖（繪製要能提早放開映射鎖、呈現的前置不可持鎖）；有人等圖形鎖、有寫者或全停要求時立刻放開。
        // X64ARM_D3D11_STREAM_BATCH_LOCKS=0 改回逐筆上鎖。
        static const bool batch_locks=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_BATCH_LOCKS");return !value || std::string_view(value)!="0";}();
        std::shared_lock<detail::ParallelMappingLock> outer_mapping(detail::parallel_mapping_mutex(),std::defer_lock);
        std::unique_lock<GraphicsMutex> outer_graphics(graphics_mutex(),std::defer_lock);
        const auto release_outer=[&]{if(outer_graphics.owns_lock())outer_graphics.unlock();if(outer_mapping.owns_lock())outer_mapping.unlock();};
        const auto execute=[&](const D3D11CommandStream::Entry& entry){
            if(stream.failed.load(std::memory_order_relaxed))return;
            // 抽樣模式只分帳客體 worker 的同步呼叫，串流重播不混入其母體。
            const bool count_replay=service_counts_enabled_ && !worker_service_sample_bits_;
            if(entry.snapshot_at!=UINT64_MAX){
                // 前端路徑的 Unmap：內容已在快照區，不讀客體記憶體、不改對應狀態。
                const auto started=count_replay?clock_gettime_nsec_np(CLOCK_UPTIME_RAW):0;
                try{
                    std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);if(!outer_graphics.owns_lock())lock_graphics(graphics_lock);
                    const auto context=d3d11_->objects.find_fast(entry.rcx);const auto resource=d3d11_->objects.find_fast(entry.rdx);
                    if(context==d3d11_->objects.end() || resource==d3d11_->objects.end() || !resource->second.identity->resource)throw HostCallFault("D3D11 命令串流的 Unmap 物件已不存在");
                    const auto& target=resource->second.identity->resource;
                    ++d3d11_->unmap_calls[4];d3d11_->unmap_bytes[4]+=entry.snapshot_size;
                    context->second.identity->native->graphics_write_discard(GraphicsView{target,target->desc.format,0,1,0,1},std::span<const std::uint8_t>(stream.snapshot.get()+entry.snapshot_at,entry.snapshot_size),0,0);
                }catch(...){
                    stream.failure=std::current_exception();
                    stream.failed.store(true,std::memory_order_release);
                }
                if(count_replay){const auto elapsed=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started;std::lock_guard lock(*worker_service_counts_mutex_);auto& count=worker_service_counts_[entry.rip];++count.calls;count.ns+=elapsed;}
                return;
            }
                synthetic.reg(Gpr::Rcx)=entry.rcx;synthetic.reg(Gpr::Rdx)=entry.rdx;synthetic.reg(Gpr::R8)=entry.r8;synthetic.reg(Gpr::R9)=entry.r9;
                synthetic.reg(Gpr::Rsp)=entry.rsp;synthetic.rip=GuestAddress{entry.rip};
                for(unsigned i=0;i<4;++i)synthetic.xmm[3][i]=entry.xmm3[i];
                const auto started=count_replay?clock_gettime_nsec_np(CLOCK_UPTIME_RAW):0;
                try{
                    if(entry.present)d3d11_stream_prepare_present(stream,entry.rcx);
                    std::shared_lock<detail::ParallelMappingLock> mapping_guard(detail::parallel_mapping_mutex(),std::defer_lock);
                    if(!outer_mapping.owns_lock())mapping_guard.lock();
                    std::unique_lock<GraphicsMutex> graphics_lock(graphics_mutex(),std::defer_lock);if(!outer_graphics.owns_lock())lock_graphics(graphics_lock);
                    const bool may_release_mapping=entry.service->symbol.front()=='D';
                    struct GuardScope {bool set;~GuardScope(){if(set)detail::d3d11_worker_mapping_guard()=nullptr;}} guard_scope{may_release_mapping};
                    if(may_release_mapping)detail::d3d11_worker_mapping_guard()=&mapping_guard;
                    try{
                        if(invoke_d3d11(synthetic,memory,*entry.service))throw HostCallFault("D3D11 命令串流的方法意外終止客體");
                    }catch(const detail::D3D11RelockMapping&){
                        detail::d3d11_worker_mapping_guard()=nullptr;
                        graphics_lock.unlock();mapping_guard.lock();lock_graphics(graphics_lock);
                        if(invoke_d3d11(synthetic,memory,*entry.service))throw HostCallFault("D3D11 命令串流的方法意外終止客體");
                    }
                }catch(const MemoryFault&){
                    // 槽內的參數都已複製，這裡的故障不屬於任何客體呼叫點；不派送成客體存取違規。
                    stream.failure=std::make_exception_ptr(HostCallFault("D3D11 命令串流讀取參數故障："+std::string(entry.service->symbol)));
                    stream.failed.store(true,std::memory_order_release);
                }catch(...){
                    stream.failure=std::current_exception();
                    stream.failed.store(true,std::memory_order_release);
                }
                if(count_replay){const auto elapsed=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started;std::lock_guard lock(*worker_service_counts_mutex_);auto& count=worker_service_counts_[entry.rip];++count.calls;count.ns+=elapsed;}
        };
        // 信箱裡的呈現：序號 after 之前的命令都做完才執行，之後的命令等它。
        const bool foreign=stream.foreign_ready.load(std::memory_order_acquire)!=0;
        if(foreign && tail>=stream.foreign_after){
            execute(stream.foreign);
            stream.foreign_ready.store(0,std::memory_order_release);
            stream.busy.store(0,std::memory_order_release);
            continue;
        }
        const auto batch_end=std::min({head,tail+16,foreign?stream.foreign_after:head});
        while(tail<batch_end){
            const auto& entry=stream.entries[tail&(stream.slots-1)];
            // 快照 Unmap 只需要圖形鎖，放在外層兩把鎖之下也符合鎖序；繪製（D 開頭）與呈現先放開外層，照原本逐筆流程執行。
            const bool plain=batch_locks && !entry.present && (entry.snapshot_at!=UINT64_MAX || entry.service->symbol.front()!='D');
            if(!plain)release_outer();
            else if(!outer_mapping.owns_lock()){outer_mapping.lock();lock_graphics(outer_graphics);}
            execute(entry);
            ++tail;
            stream.arena_tail.store(entry.arena_end,std::memory_order_release);
            stream.snapshot_tail.store(entry.snapshot_end,std::memory_order_release);
            stream.tail.store(tail,std::memory_order_release);
            // 有人等圖形鎖就讓一下（重入鎖不公平，串流執行緒否則會立刻搶回；對方的呼叫都很短）。
            if(outer_mapping.owns_lock() && (graphics_waiters().load(std::memory_order_relaxed) || detail::parallel_mapping_mutex().writer_waiting() ||
               stream.paused.load(std::memory_order_relaxed)))release_outer();
            for(unsigned spins=0;spins<256 && graphics_waiters().load(std::memory_order_relaxed);++spins)d3d11_stream_pause_cpu();
        }
        release_outer();
        stream.busy.store(0,std::memory_order_release);
    }
}
void WindowsServices::d3d11_stream_setup(GuestMemory& memory,const std::uint64_t guest_base){
    const auto arena_base=guest_base+D3D11CommandStream::mapped_bytes;
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM");return !value || std::string_view(value)!="0";}();
    if(!enabled)return;
    if(!memory.range_available(GuestAddress{guest_base},D3D11CommandStream::mapped_bytes+D3D11CommandStream::arena_bytes))throw std::runtime_error("D3D11 命令串流的環狀緩衝位址已被占用");
    auto stream=std::make_shared<D3D11CommandStream>();
    stream->backing=std::make_shared<MemoryBacking>(D3D11CommandStream::mapped_bytes);
    memory.map_shared(GuestAddress{guest_base},D3D11CommandStream::mapped_bytes,read_write,stream->backing);
    stream->guest_base=guest_base;stream->host_base=stream->backing->data();
    stream->arena=std::make_shared<MemoryBacking>(D3D11CommandStream::arena_bytes);
    memory.map_shared(GuestAddress{arena_base},D3D11CommandStream::arena_bytes,read_write,stream->arena);
    stream->arena_base=arena_base;stream->arena_host=stream->arena->data();
    stream->drains_by_service=std::vector<std::atomic<std::uint32_t>>(runtime_service_descriptors().size());
    stream->snapshot=std::make_unique_for_overwrite<std::uint8_t[]>(D3D11CommandStream::snapshot_bytes);
    d3d11_stream_guest_processors().store(static_cast<unsigned>(runtime_.processor_count),std::memory_order_relaxed);
    if(!x86_ && d3d11_stream_resident_enabled() && !d3d11_resident_stream.load(std::memory_order_acquire)){
        auto& descriptor=d3d11_resident_descriptor();
        const GuestState probe{};const WindowsServiceDescriptor sample{};
        descriptor.context=&stream->context;descriptor.owner=&stream->owner;
        descriptor.gs_offset=static_cast<std::uint64_t>(reinterpret_cast<const std::uint8_t*>(&probe.gs_base)-reinterpret_cast<const std::uint8_t*>(&probe));
        descriptor.failed=&stream->failed;descriptor.paused=&stream->paused;descriptor.head=&stream->head;descriptor.cached_tail=&stream->cached_tail;
        descriptor.slots=stream->slots;descriptor.entries=stream->entries.data();descriptor.entry_size=sizeof(D3D11CommandStream::Entry);
        descriptor.host_base=stream->host_base;descriptor.guest_base=stream->guest_base;
        descriptor.arena_head=&stream->arena_head;descriptor.snapshot_head=&stream->snapshot_head;
        descriptor.descriptors=runtime_service_descriptors().data();descriptor.descriptor_size=sizeof(WindowsServiceDescriptor);
        descriptor.arguments_offset=static_cast<std::uint64_t>(reinterpret_cast<const std::uint8_t*>(&sample.arguments)-reinterpret_cast<const std::uint8_t*>(&sample));
        descriptor.stub_begin=stub_base_.value+(detail::stub_service_count()-runtime_service_descriptors().size())*16;descriptor.sleeping=&stream->sleeping;descriptor.wake=&x64arm_d3d11_stream_resident_wake;
        descriptor.enqueued=&stream->enqueued;descriptor.permissions_offset=GuestMemory::native_layout().mmu_permissions;
        descriptor.relaxed_publish=d3d11_stream_relaxed_publish()?1:0;
        static const bool constant_dedup=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_CONSTANT_DEDUP");return !value || std::string_view(value)!="0";}();
        descriptor.classes=d3d11_stream_resident_classes().data();descriptor.constant_dedup=constant_dedup?1:0;
        descriptor.constant_epoch=&stream->constant_epoch;descriptor.constant_epoch_seen=&stream->constant_epoch_seen;
        descriptor.constant_bindings=stream->constant_bindings.data();descriptor.constant_skips=&stream->constant_skips;
        descriptor.front_entries=stream->front_maps.entries.get();descriptor.front_multiplier=D3D11FrontMapTable::multiplier;
        descriptor.front_shift=D3D11FrontMapTable::shift;descriptor.front_mask=D3D11FrontMapTable::capacity-1;
        descriptor.snapshot_cached_tail=&stream->snapshot_cached_tail;
        descriptor.front_epoch=&stream->front_epoch;descriptor.front_epoch_seen=&stream->front_epoch_seen;descriptor.tail=&stream->tail;
        descriptor.snapshot=stream->snapshot.get();descriptor.snapshot_tail=&stream->snapshot_tail;
        descriptor.snapshot_bytes=D3D11CommandStream::snapshot_bytes;descriptor.snapshot_item_limit=D3D11CommandStream::snapshot_item_limit;
        descriptor.front_maps_taken=&stream->front_maps_taken;descriptor.front_unmaps_taken=&stream->front_unmaps_taken;
        d3d11_resident_stream.store(stream.get(),std::memory_order_release);
        std::atomic_ref(descriptor.enabled).store(1,std::memory_order_seq_cst);
    }
    d3d11_stream_=std::move(stream);
}
void WindowsServices::d3d11_stream_trace() const noexcept {
    if(!d3d11_stream_)return;
    const auto& stream=*d3d11_stream_;
    std::fprintf(stderr,"[d3d11_stream] head=%llu tail=%llu sleeping=%u paused=%u context=0x%llx owner=0x%llx failed=%d enqueued=%llu locked=%llu rejected=%llu drains=%llu drain_ns=%llu full_waits=%llu full_ns=%llu pauses=%llu pause_ns=%llu paused_ns=%llu idle_ns=%llu present_ready_ns=%llu present_wait_ns=%llu front_maps=%llu front_unmaps=%llu front_used=%zu front_refused=%llu constant_skips=%llu full_enqueues=%llu resident_entries=%llu resident_direct=%llu top:",
        static_cast<unsigned long long>(stream.head.load()),static_cast<unsigned long long>(stream.tail.load()),stream.sleeping.load(),stream.paused.load(),
        static_cast<unsigned long long>(stream.context.load()),static_cast<unsigned long long>(stream.owner.load()),stream.failed.load()?1:0,
        static_cast<unsigned long long>(stream.enqueued.load()),static_cast<unsigned long long>(stream.locked_enqueues.load()),static_cast<unsigned long long>(stream.rejected.load()),static_cast<unsigned long long>(stream.drain_waits.load()),
        static_cast<unsigned long long>(stream.drain_ns.load()),static_cast<unsigned long long>(stream.full_waits.load()),static_cast<unsigned long long>(stream.full_ns.load()),
        static_cast<unsigned long long>(stream.pauses.load()),static_cast<unsigned long long>(stream.pause_ns.load()),
        static_cast<unsigned long long>(stream.paused_ns.load()),static_cast<unsigned long long>(stream.idle_ns.load()),static_cast<unsigned long long>(stream.present_ready_ns.load()),static_cast<unsigned long long>(stream.present_wait_ns.load()),
        static_cast<unsigned long long>(stream.front_maps_taken.load()),static_cast<unsigned long long>(stream.front_unmaps_taken.load()),stream.front_maps.used,static_cast<unsigned long long>(stream.front_maps.refused),static_cast<unsigned long long>(stream.constant_skips.load()),static_cast<unsigned long long>(stream.full_enqueues.load()),
        static_cast<unsigned long long>(d3d11_resident_descriptor().entries_seen),static_cast<unsigned long long>(d3d11_resident_descriptor().direct_enqueues));
    const auto descriptors=runtime_service_descriptors();std::vector<std::pair<std::uint32_t,std::size_t>> rows;
    for(std::size_t i=0;i<stream.drains_by_service.size();++i)if(const auto value=stream.drains_by_service[i].load(std::memory_order_relaxed))rows.emplace_back(value,i);
    std::sort(rows.rbegin(),rows.rend());
    for(std::size_t i=0;i<std::min<std::size_t>(rows.size(),12);++i)std::fprintf(stderr," %.*s=%u",static_cast<int>(descriptors[rows[i].second].symbol.size()),descriptors[rows[i].second].symbol.data(),rows[i].first);
    std::fprintf(stderr,"\n");
}
bool WindowsServices::d3d11_stream_failed() const noexcept {return d3d11_stream_ && d3d11_stream_->failed.load(std::memory_order_acquire);}
namespace {bool stream_head_differs(const D3D11CommandStream& stream) noexcept {return stream.head.load(std::memory_order_acquire)!=stream.tail.load(std::memory_order_acquire) || stream.foreign_ready.load(std::memory_order_acquire);}}
void WindowsServices::d3d11_stream_rethrow(){
    if(!d3d11_stream_ || !d3d11_stream_->failed.load(std::memory_order_acquire))return;
    if(detail::parallel_mapping_mutex().held_exclusive())return;
    if(stream_head_differs(*d3d11_stream_)){d3d11_stream_->paused.store(0,std::memory_order_seq_cst);d3d11_stream_->paused.notify_one();d3d11_stream_drain();d3d11_stream_quiesced();}
    auto& stream=*d3d11_stream_;
    if(stream.failure){const auto pending=std::exchange(stream.failure,nullptr);stream.failed.store(false,std::memory_order_release);std::rethrow_exception(pending);}
}
// 全部 worker 都已停止之後呼叫（協調者上、不持 provider 鎖）：等串流執行緒離開目前的命令並擋住下一筆；佇列留著，worker 恢復後繼續。
// 呼叫點：完整排程開頭（after_resident_parallel）、完整服務入口、例外派送入口；全停之後到這些點之間只有唯讀與 native MMU 重整。
void WindowsServices::d3d11_stream_quiesced() noexcept {
    if(!d3d11_stream_)return;
    auto& stream=*d3d11_stream_;
    // 呼叫者持獨占鎖時串流執行緒已被擋在共用鎖外（不碰記憶體）；在這裡等它會互等。
    if(detail::parallel_mapping_mutex().held_exclusive())return;
    if(stream.paused.load(std::memory_order_relaxed))return;
    stream.paused.store(1,std::memory_order_seq_cst);
    stream.pauses.fetch_add(1,std::memory_order_relaxed);
    if(!stream.busy.load(std::memory_order_seq_cst))return;
    const auto started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for(unsigned spins=0;stream.busy.load(std::memory_order_seq_cst);++spins){
        if(spins<4096){d3d11_stream_pause_cpu();continue;}
        std::this_thread::yield();
    }
    stream.pause_ns.fetch_add(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started,std::memory_order_relaxed);
}
// 完整服務要碰 D3D11 狀態：放行串流執行緒直到佇列清空，再擋回去。worker 都已停止，不會有新命令。清除擁有者讓下一個呼叫者重新認領（可能換執行緒或內容）。
void WindowsServices::d3d11_stream_full_service_sync(const WindowsServiceDescriptor& service) noexcept {
    if(!d3d11_stream_)return;
    auto& stream=*d3d11_stream_;
    if(detail::parallel_mapping_mutex().held_exclusive())return;
    if(service.family=="internal-d3d11" && d3d11_stream_class(service.symbol)==D3D11StreamClass::NoDrain)return;
    if(stream.head.load(std::memory_order_acquire)!=stream.tail.load(std::memory_order_acquire) || stream.foreign_ready.load(std::memory_order_acquire)){
        stream.paused.store(0,std::memory_order_seq_cst);stream.paused.notify_one();
        d3d11_stream_drain();
        d3d11_stream_quiesced();
    }
    // 交換鏈方法會直接用呈現也在用的物件：先等命令執行緒上的呈現做完。串流執行緒已暫停，不會再投遞；present_producer 只有它會寫。
    if(stream.presents_done->load(std::memory_order_acquire)<stream.presents_posted && stream.present_producer){
        try{stream.present_producer->graphics_sync();}
        catch(...){if(!stream.failure)stream.failure=std::current_exception();stream.failed.store(true,std::memory_order_release);}
    }
    // 前端路徑對應中的資源：下一次 Unmap 可能來自別的執行緒或走完整路徑，對應狀態轉回物件上。
    if(d3d11_)stream.front_maps.for_each([&](D3D11FrontMapTable::Entry& map){
        if(!map.mapped)return;
        const auto found=d3d11_->objects.find_fast(map.key);
        if(found!=d3d11_->objects.end() && found->second.identity)found->second.identity->subresource_maps.emplace(0U,Identity::SubresourceMap{map.pointer,map.size,4U});
        map.mapped=0;
    });
    stream.context.store(0,std::memory_order_relaxed);stream.owner.store(0,std::memory_order_relaxed);
}
// 完整路徑上擁有者自己的立即內容 Map／Unmap：與協調者本地路徑（d3d11_stream_coordinated）相同，只等同一資源排在佇列裡的命令，
// 不排空整條佇列、不清除擁有者。可對應的資源（staging、dynamic）只會被 Copy／Update／Unmap 寫入，這些都記在 pending；讀回本身會等命令執行緒與 GPU。
// 全停期間擁有者的待辦服務在這裡執行（《崛起》地熱谷 4 vCPU 繪製執行緒每秒約 300 次 Map(DISCARD)、20 次 Unmap 走完整路徑，每次排空 0.25～3 ms，
// 並清除擁有者使緊接的設定也落到完整路徑）。這個資源若仍是前端對應中，先把對應狀態轉回物件，Unmap 才找得到。
// 繪製執行緒完整路徑 Map 250→22 µs、Unmap 2～3 ms→0.2 ms；地熱谷 2 vCPU 交錯各三輪 43.5／43.6／43.6 對 43.2／43.7／41.7（平均 +1.6%，較穩定）。X64ARM_D3D11_STREAM_FULL_MAP=0 關閉。
bool WindowsServices::d3d11_stream_owner_resource_sync(const GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_FULL_MAP");return !value || std::string_view(value)!="0";}();
    if(!enabled || !d3d11_stream_ || service.family!="internal-d3d11")return false;
    auto& stream=*d3d11_stream_;
    if(detail::parallel_mapping_mutex().held_exclusive())return false;
    const auto kind=d3d11_stream_class(service.symbol);
    if(kind!=D3D11StreamClass::Map && kind!=D3D11StreamClass::Unmap)return false;
    const auto owner=stream.owner.load(std::memory_order_relaxed),context=stream.context.load(std::memory_order_relaxed);
    if(!owner || !context || state.reg(Gpr::Rcx)!=context || state.gs_base!=owner)return false;
    const auto resource=state.reg(Gpr::Rdx);
    // 擁有者正停在這次服務，可以讀它的私有表；串流執行緒暫停中，要等待就先放行，等完再擋回去。DO_NOT_WAIT 的 Map 不等。
    if(const auto found=stream.pending.find(resource);found!=stream.pending.end() && stream.tail.load(std::memory_order_acquire)<found->second &&
       !(kind==D3D11StreamClass::Map && d3d11_map_do_not_wait(state,memory,false))){
        stream.paused.store(0,std::memory_order_seq_cst);stream.paused.notify_one();
        d3d11_stream_wait(stream,found->second);
        d3d11_stream_quiesced();
    }
    if(d3d11_)if(auto* const map=stream.front_maps.find(resource);map && map->mapped){
        const auto found=d3d11_->objects.find_fast(resource);
        if(found!=d3d11_->objects.end() && found->second.identity)found->second.identity->subresource_maps.emplace(0U,Identity::SubresourceMap{map->pointer,map->size,4U});
        map->mapped=0;
    }
    stream.update_epoch.fetch_add(1,std::memory_order_release);
    return true;
}
// 串流擁有者在全停中交給協調者的呼叫（worker 到服務邊界時已有全停請求，就不在 worker 執行，見 aot_engine 的 worker 服務出口）：
// 能排入命令環的方法由協調者代擁有者排入，與 worker 上的 d3d11_stream_entry 同一流程，不排空整條佇列、不清除擁有者。
// 原本走 d3d11_stream_full_service_sync：《崛起》地熱谷 4 vCPU 繪製執行緒的設定方法每秒十多次落到這裡，每次排空約 2 ms，
// 擁有者被清除後緊接的設定也要重新認領。擁有者停在這次服務，生產端狀態此時只有協調者會碰；排入可能要等環或快照區有空位，
// 期間放行串流執行緒（與排空相同只在協調者等待時放行），完成後再擋回。同步類、Present 與其他方法照舊。X64ARM_D3D11_STREAM_FULL_ENQUEUE=0 關閉。
bool WindowsServices::d3d11_stream_full_enqueue(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_D3D11_STREAM_FULL_ENQUEUE");return !value || std::string_view(value)!="0";}();
    if(!enabled || !d3d11_stream_ || service.family!="internal-d3d11" || detail::parallel_mapping_mutex().held_exclusive())return false;
    auto& stream=*d3d11_stream_;
    const auto owner=stream.owner.load(std::memory_order_relaxed),context=stream.context.load(std::memory_order_relaxed);
    if(!owner || !context || state.gs_base!=owner || state.reg(Gpr::Rcx)!=context || stream.failed.load(std::memory_order_acquire))return false;
    if(detail::runtime_stub_descriptor(stub_base_,state.rip)!=&service)return false;
    switch(d3d11_stream_class(service.symbol)){
    case D3D11StreamClass::Scalar:case D3D11StreamClass::Shader:case D3D11StreamClass::Constants:case D3D11StreamClass::Pointers:
    case D3D11StreamClass::VertexBuffers:case D3D11StreamClass::Viewports:case D3D11StreamClass::Scissors:case D3D11StreamClass::Blend:
    case D3D11StreamClass::Uavs:case D3D11StreamClass::Targets:case D3D11StreamClass::TargetsUavs:case D3D11StreamClass::Clear:
    case D3D11StreamClass::ClearDepth:case D3D11StreamClass::Query:case D3D11StreamClass::Update:case D3D11StreamClass::Copy:
    case D3D11StreamClass::Unmap:case D3D11StreamClass::Map:case D3D11StreamClass::Release:case D3D11StreamClass::GetData:break;
    default:return false;
    }
    const auto step=d3d11_stream_entry(state,memory);
    d3d11_stream_quiesced();
    if(step!=D3D11StreamStep::Done)return false;
    stream.full_enqueues.fetch_add(1,std::memory_order_relaxed);
    return true;
}
// 協調者本地的 D3D11 方法（持獨占映射鎖之前）：裝置建立只新增物件，不依賴佇列；只讀物件自身狀態的方法也一樣；
// 立即內容的 Map／Unmap 只要等同一資源上排隊的命令：可對應的資源（staging、dynamic）只會被 Copy／Update／Unmap 寫入，不能當輸出目標，這些都記在 pending；讀回本身會等命令執行緒與 GPU。
// 對應期間客體不能對這個資源發出命令，Unmap 的上傳因此排在佇列裡較早的命令之前也不會被看到。其他方法要看到前面命令的結果。
void WindowsServices::d3d11_stream_coordinated(const GuestState& state,GuestMemory& memory,const bool device_creation) noexcept {
    if(!d3d11_stream_ || device_creation)return;
    auto& stream=*d3d11_stream_;
    const auto* const descriptor=detail::runtime_stub_descriptor(stub_base_,state.rip);
    const auto service_index=descriptor?static_cast<std::size_t>(descriptor-runtime_service_descriptors().data()):SIZE_MAX;
    const auto kind=descriptor?d3d11_stream_classes()[service_index]:D3D11StreamClass::Sync;
    if(kind==D3D11StreamClass::NoDrain)return;
    // 擁有者正停在這次服務等回覆，這裡可以讀它的私有表。
    if((kind==D3D11StreamClass::Map || kind==D3D11StreamClass::Unmap) && state.reg(Gpr::Rcx)==stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed)){
        if(const auto found=stream.pending.find(state.reg(Gpr::Rdx));found!=stream.pending.end() &&
           !(kind==D3D11StreamClass::Map && d3d11_map_do_not_wait(state,memory,false)))d3d11_stream_wait(stream,found->second);
        stream.update_epoch.fetch_add(1,std::memory_order_release);
        return;
    }
    stream.constant_epoch.fetch_add(1,std::memory_order_release);
    if(descriptor && stream.head.load(std::memory_order_relaxed)!=stream.tail.load(std::memory_order_acquire))stream.drains_by_service[service_index].fetch_add(1,std::memory_order_relaxed);
    d3d11_stream_drain();
    // 非持久 Map 可能建立持久映射：可排入更新的緩衝要重新核對。
    d3d11_stream_->update_epoch.fetch_add(1,std::memory_order_release);
}
// Present 的 stub：讓 worker 先經過 d3d11_stream_entry（排入，或不合資格時先等佇列清空再交回協調者）。
bool WindowsServices::d3d11_stream_worker_stub(const GuestAddress address) const noexcept {
    const auto* const descriptor=detail::runtime_stub_descriptor(stub_base_,address);
    return descriptor && d3d11_stream_classes()[static_cast<std::size_t>(descriptor-runtime_service_descriptors().data())]==D3D11StreamClass::Present;
}
