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
namespace detail {const WindowsServiceDescriptor* runtime_stub_descriptor(GuestAddress stub_base,GuestAddress address) noexcept;}
enum class D3D11StreamClass:std::uint8_t {Other,Sync,NoDrain,Release,Map,GetData,GetTargets,Query,Unmap,Update,Copy,Scalar,Shader,Pointers,VertexBuffers,Viewports,Scissors,Blend,Uavs,Targets,TargetsUavs,Clear,ClearDepth,Present};
// 目前這條宿主執行緒是串流執行緒（Present 的處理程式不能讓客體執行緒等待，也不是客體執行緒）。
thread_local bool d3d11_on_stream_thread=false;
struct D3D11CommandStream {
    // 環要裝得下一整幀（UE 系每幀兩萬多次）：爆發時客體才不會被壓到和串流執行緒同速。頁面用到才配置。
    static constexpr std::size_t slot_bytes=256,slot_capacity=32768,mapped_bytes=slot_capacity*slot_bytes;
    const std::size_t slots=[] {
        // X64ARM_D3D11_STREAM_SLOTS=<2 的次方>：對照環的大小。
        const auto* value=std::getenv("X64ARM_D3D11_STREAM_SLOTS");const auto parsed=value?std::strtoull(value,nullptr,10):0;
        return parsed>=1024 && parsed<=slot_capacity && !(parsed&(parsed-1))?static_cast<std::size_t>(parsed):std::size_t{8192};
    }();
    static constexpr std::size_t arena_bytes=std::size_t{4}<<20,arena_item_limit=std::size_t{1}<<20;
    struct Entry {const WindowsServiceDescriptor* service{};std::uint64_t rip{},rcx{},rdx{},r8{},r9{},rsp{},arena_end{};std::array<std::uint8_t,4> xmm3{};bool present{};};
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
    std::array<Page,256> pages{};
    std::atomic<std::uint64_t> enqueued{},rejected{},drain_waits{},drain_ns{},full_waits{},full_ns{},locked_enqueues{};
    // 串流執行緒。
    alignas(128) std::atomic<std::uint64_t> tail{}; // 完成的序號。
    std::atomic<std::uint64_t> arena_tail{},paused_ns{},idle_ns{},present_ready_ns{},present_wait_ns{};
    alignas(128) std::atomic<std::uint32_t> sleeping{};
    alignas(128) std::atomic<std::uint32_t> busy{};  // 串流執行緒正在處理命令（Dekker，配對 paused）。
    alignas(128) std::atomic<std::uint32_t> paused{};
    alignas(128) std::atomic<std::uint64_t> update_epoch{}; // 物件銷毀與協調者路徑的方法會遞增。
    std::atomic<std::uint64_t> pauses{},pause_ns{};
    // 交給命令執行緒的呈現：串流執行緒累計投遞數，命令執行緒做完才遞增完成數。
    alignas(128) std::uint64_t presents_posted{};
    std::shared_ptr<NativeOpenGlContext> present_producer;
    std::shared_ptr<std::atomic<std::uint64_t>> presents_done{std::make_shared<std::atomic<std::uint64_t>>(0)};
    alignas(128) std::array<Entry,slot_capacity> entries;
    ~D3D11CommandStream(){
        if(!started.load(std::memory_order_relaxed))return;
        stopping.store(true,std::memory_order_seq_cst);
        paused.store(0,std::memory_order_seq_cst);paused.notify_all();
        sleeping.store(0,std::memory_order_seq_cst);sleeping.notify_all();
        pthread_join(thread,nullptr);
    }
};
namespace {
D3D11StreamClass d3d11_stream_class(std::string_view symbol) noexcept {
    constexpr std::string_view scalar[]{"IASetIndexBuffer","IASetInputLayout","IASetPrimitiveTopology","RSSetState","OMSetDepthStencilState",
        "Draw","DrawIndexed","DrawInstanced","DrawIndexedInstanced","DrawInstancedIndirect","DrawIndexedInstancedIndirect","Dispatch","DispatchIndirect"};
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
        if(what=="SetConstantBuffers" || what=="SetShaderResources" || what=="SetSamplers")return D3D11StreamClass::Pointers;
    }
    // 只讀建立時就固定的描述或物件自己的欄位，與佇列裡的命令無關；裝置建立只新增物件。協調者路徑另依 d3d11_stream_needs_drain 判斷。
    constexpr std::string_view no_drain[]{"BufferGetDesc","Texture1DGetDesc","Texture3DGetDesc","GraphicsGetDesc","TextureGetDesc","GetResource","SetPrivateData","GetFullscreenState",
        "QueryVideoMemoryInfo","SetVideoMemoryReservation","AddRef","QueryInterface","GetDevice","GetParent","GetDesc","GetImmediateContext"};
    if(std::find(std::begin(no_drain),std::end(no_drain),symbol)!=std::end(no_drain))return D3D11StreamClass::NoDrain;
    return D3D11StreamClass::Sync;
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
    entry.service=&service;entry.rip=state.rip.value;entry.rcx=values[0];entry.rdx=values[1];entry.r8=values[2];entry.r9=values[3];entry.rsp=slot+8;entry.arena_end=stream.arena_head;
    // ClearDepthStencilView 的深度是第 4 個參數，x64 放在 XMM3。
    if(kind==D3D11StreamClass::ClearDepth)for(unsigned i=0;i<4;++i)entry.xmm3[i]=state.xmm[3][i];
    entry.present=kind==D3D11StreamClass::Present;
    state.rip=GuestAddress{return_target};state.reg(Gpr::Rax)=0;state.reg(Gpr::Rsp)=sp+8;
    if(kind==D3D11StreamClass::Query || kind==D3D11StreamClass::Unmap || kind==D3D11StreamClass::Update || kind==D3D11StreamClass::Copy){
        // 舊項目在處理完之後就沒有作用，偶爾清掉避免無限成長（位址重用時最多多等到已完成的序號，不影響正確性）。
        if(stream.pending.size()>4096){const auto done=stream.tail.load(std::memory_order_acquire);std::erase_if(stream.pending,[&](const auto& item){return item.second<=done;});}
        stream.pending[values[1]]=head+1;
        // Copy 的來源：之後 Map 寫入來源要排在這個 Copy 之後。CopyResource 的來源是第 3 個參數，CopySubresourceRegion 是第 7 個。
        if(kind==D3D11StreamClass::Copy)stream.pending[values[service.arguments>3?6:2]]=head+1;
    }
    stream.head.store(head+1,std::memory_order_seq_cst);
    if(stream.sleeping.load(std::memory_order_seq_cst)){stream.sleeping.store(0,std::memory_order_seq_cst);stream.sleeping.notify_one();}
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
    if(stream.tail.load(std::memory_order_acquire)==target)return;
    const auto started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    stream.drain_waits.fetch_add(1,std::memory_order_relaxed);
    d3d11_stream_wait(stream,target);
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
    // 串流執行緒失敗：交回協調者，由完整服務邊界重新拋出。
    if(stream.failed.load(std::memory_order_relaxed))return D3D11StreamStep::Refuse;
    const bool owner=state.reg(Gpr::Rcx) && state.reg(Gpr::Rcx)==stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed);
    const auto sync=[&]{
        // 同步方法之後的輸出目標可能變了（例如 ClearState），影子失效到下一次排入 OMSetRenderTargets。
        if(owner || state.reg(Gpr::Rcx)==stream.context.load(std::memory_order_relaxed))stream.shadow_valid=false;
        if(stream.head.load(std::memory_order_relaxed)!=stream.tail.load(std::memory_order_acquire))stream.drains_by_service[service_index].fetch_add(1,std::memory_order_relaxed);
        d3d11_stream_drain();return D3D11StreamStep::Continue;
    };
    if(kind==D3D11StreamClass::Release){
        // 非最後一次釋放只改計數。最後一次：佇列裡可能還有對它的參照，要排在那些命令之後；客體已經沒有參照，回傳值固定是 0。
        // 擁有者以外的執行緒不能排入，先清空。
        bool done=false;
        {
            const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
            std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
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
        if(enabled && stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed) &&
           !(static_cast<std::uint32_t>(state.reg(Gpr::R8))&~0x200U) && !graphics_close_frame_limit()){
            d3d11_stream_wait(stream,stream.present_sequence);
            const auto head=stream.head.load(std::memory_order_relaxed);
            if(head-stream.cached_tail>=stream.slots){stream.cached_tail=stream.tail.load(std::memory_order_acquire);if(head-stream.cached_tail>=stream.slots)d3d11_stream_wait(stream,head-stream.slots+64);}
            stream.page_generation=memory.native_borrow_generation();
            bool done=false;
            {
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
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
        // 同一資源的 Unmap 還沒處理時，客體不能拿到（同一塊）區塊來寫。Map 本身照常同步，不依賴其他命令。
        if(const auto found=stream.pending.find(state.reg(Gpr::Rdx));found!=stream.pending.end())d3d11_stream_wait(stream,found->second);
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
        if(stream.shadow_valid){
            bool done=false;
            {
                const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
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
            d3d11_stream_wait(stream,head-stream.slots+64);
            stream.full_ns.store(stream.full_ns.load(std::memory_order_relaxed)+clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started,std::memory_order_relaxed);
            stream.cached_tail=stream.tail.load(std::memory_order_acquire);
        }
    }
    stream.page_generation=memory.native_borrow_generation();
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
        if(queued==D3D11StreamQueued::Done){state.rep_compare={};return D3D11StreamStep::Done;}
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
                std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
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
    if(done){state.rep_compare={};return D3D11StreamStep::Done;}
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
        std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
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
            if(head==tail){
                if(stream.stopping.load(std::memory_order_acquire))return;
                const auto until=ticks()+ticks_per_20us;
                while((head=stream.head.load(std::memory_order_acquire))==tail && ticks()<until)d3d11_stream_pause_cpu();
                if(head==tail){
                    const auto idle_started=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    stream.sleeping.store(1,std::memory_order_seq_cst);
                    if(stream.head.load(std::memory_order_seq_cst)==tail && !stream.stopping.load(std::memory_order_seq_cst))stream.sleeping.wait(1,std::memory_order_seq_cst);
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
        const auto batch_end=std::min(head,tail+16);
        while(tail<batch_end){
            const auto& entry=stream.entries[tail&(stream.slots-1)];
            if(!stream.failed.load(std::memory_order_relaxed)){
                synthetic.reg(Gpr::Rcx)=entry.rcx;synthetic.reg(Gpr::Rdx)=entry.rdx;synthetic.reg(Gpr::R8)=entry.r8;synthetic.reg(Gpr::R9)=entry.r9;
                synthetic.reg(Gpr::Rsp)=entry.rsp;synthetic.rip=GuestAddress{entry.rip};
                for(unsigned i=0;i<4;++i)synthetic.xmm[3][i]=entry.xmm3[i];
                const auto started=service_counts_enabled_?clock_gettime_nsec_np(CLOCK_UPTIME_RAW):0;
                try{
                    if(entry.present)d3d11_stream_prepare_present(stream,entry.rcx);
                    std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
                    std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);lock_graphics(graphics_lock);
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
                if(service_counts_enabled_){const auto elapsed=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-started;std::lock_guard lock(*worker_service_counts_mutex_);auto& count=worker_service_counts_[entry.rip];++count.calls;count.ns+=elapsed;}
            }
            ++tail;
            stream.arena_tail.store(entry.arena_end,std::memory_order_release);
            stream.tail.store(tail,std::memory_order_release);
            // 有人等圖形鎖就讓一下（重入鎖不公平，串流執行緒否則會立刻搶回；對方的呼叫都很短）。
            for(unsigned spins=0;spins<256 && graphics_waiters().load(std::memory_order_relaxed);++spins)d3d11_stream_pause_cpu();
        }
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
    d3d11_stream_=std::move(stream);
}
void WindowsServices::d3d11_stream_trace() const noexcept {
    if(!d3d11_stream_)return;
    const auto& stream=*d3d11_stream_;
    std::fprintf(stderr,"[d3d11_stream] head=%llu tail=%llu sleeping=%u paused=%u context=0x%llx owner=0x%llx failed=%d enqueued=%llu locked=%llu rejected=%llu drains=%llu drain_ns=%llu full_waits=%llu full_ns=%llu pauses=%llu pause_ns=%llu paused_ns=%llu idle_ns=%llu present_ready_ns=%llu present_wait_ns=%llu top:",
        static_cast<unsigned long long>(stream.head.load()),static_cast<unsigned long long>(stream.tail.load()),stream.sleeping.load(),stream.paused.load(),
        static_cast<unsigned long long>(stream.context.load()),static_cast<unsigned long long>(stream.owner.load()),stream.failed.load()?1:0,
        static_cast<unsigned long long>(stream.enqueued.load()),static_cast<unsigned long long>(stream.locked_enqueues.load()),static_cast<unsigned long long>(stream.rejected.load()),static_cast<unsigned long long>(stream.drain_waits.load()),
        static_cast<unsigned long long>(stream.drain_ns.load()),static_cast<unsigned long long>(stream.full_waits.load()),static_cast<unsigned long long>(stream.full_ns.load()),
        static_cast<unsigned long long>(stream.pauses.load()),static_cast<unsigned long long>(stream.pause_ns.load()),
        static_cast<unsigned long long>(stream.paused_ns.load()),static_cast<unsigned long long>(stream.idle_ns.load()),static_cast<unsigned long long>(stream.present_ready_ns.load()),static_cast<unsigned long long>(stream.present_wait_ns.load()));
    const auto descriptors=runtime_service_descriptors();std::vector<std::pair<std::uint32_t,std::size_t>> rows;
    for(std::size_t i=0;i<stream.drains_by_service.size();++i)if(const auto value=stream.drains_by_service[i].load(std::memory_order_relaxed))rows.emplace_back(value,i);
    std::sort(rows.rbegin(),rows.rend());
    for(std::size_t i=0;i<std::min<std::size_t>(rows.size(),12);++i)std::fprintf(stderr," %.*s=%u",static_cast<int>(descriptors[rows[i].second].symbol.size()),descriptors[rows[i].second].symbol.data(),rows[i].first);
    std::fprintf(stderr,"\n");
}
bool WindowsServices::d3d11_stream_failed() const noexcept {return d3d11_stream_ && d3d11_stream_->failed.load(std::memory_order_acquire);}
namespace {bool stream_head_differs(const D3D11CommandStream& stream) noexcept {return stream.head.load(std::memory_order_acquire)!=stream.tail.load(std::memory_order_acquire);}}
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
    if(stream.head.load(std::memory_order_acquire)!=stream.tail.load(std::memory_order_acquire)){
        stream.paused.store(0,std::memory_order_seq_cst);stream.paused.notify_one();
        d3d11_stream_drain();
        d3d11_stream_quiesced();
    }
    // 交換鏈方法會直接用呈現也在用的物件：先等命令執行緒上的呈現做完。串流執行緒已暫停，不會再投遞；present_producer 只有它會寫。
    if(stream.presents_done->load(std::memory_order_acquire)<stream.presents_posted && stream.present_producer){
        try{stream.present_producer->graphics_sync();}
        catch(...){if(!stream.failure)stream.failure=std::current_exception();stream.failed.store(true,std::memory_order_release);}
    }
    stream.context.store(0,std::memory_order_relaxed);stream.owner.store(0,std::memory_order_relaxed);
}
// 協調者本地的 D3D11 方法（持獨占映射鎖之前）：裝置建立只新增物件，不依賴佇列；只讀物件自身狀態的方法也一樣；
// 立即內容的 Map／Unmap 只要等同一資源上排隊的命令：可對應的資源（staging、dynamic）只會被 Copy／Update／Unmap 寫入，不能當輸出目標，這些都記在 pending；讀回本身會等命令執行緒與 GPU。
// 對應期間客體不能對這個資源發出命令，Unmap 的上傳因此排在佇列裡較早的命令之前也不會被看到。其他方法要看到前面命令的結果。
void WindowsServices::d3d11_stream_coordinated(const GuestState& state,const bool device_creation) noexcept {
    if(!d3d11_stream_ || device_creation)return;
    auto& stream=*d3d11_stream_;
    const auto* const descriptor=detail::runtime_stub_descriptor(stub_base_,state.rip);
    const auto service_index=descriptor?static_cast<std::size_t>(descriptor-runtime_service_descriptors().data()):SIZE_MAX;
    const auto kind=descriptor?d3d11_stream_classes()[service_index]:D3D11StreamClass::Sync;
    if(kind==D3D11StreamClass::NoDrain)return;
    // 擁有者正停在這次服務等回覆，這裡可以讀它的私有表。
    if((kind==D3D11StreamClass::Map || kind==D3D11StreamClass::Unmap) && state.reg(Gpr::Rcx)==stream.context.load(std::memory_order_relaxed) && state.gs_base==stream.owner.load(std::memory_order_relaxed)){
        if(const auto found=stream.pending.find(state.reg(Gpr::Rdx));found!=stream.pending.end())d3d11_stream_wait(stream,found->second);
        stream.update_epoch.fetch_add(1,std::memory_order_release);
        return;
    }
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
