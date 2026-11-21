#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <numeric>
#include "mf_interfaces.hpp"
#include "mf_image_layout.h"
#include "mf_fp16_layout.h"
#include "guest_mf_image.hpp"
#include "guest_mf_image_code.hpp"
#include "mf_buffer_layout.h"
#include "guest_mf_buffer.hpp"
#include "guest_mf_buffer_code.hpp"
#include "x64arm/pe_loader.hpp"
#include "mf_h264.hpp"
#include "mf_mp4_demux.hpp"
#include "mf_asf_demux.hpp"
#include "x64arm/media_decode.hpp"
#include "mf_audio_transcoder.hpp"
#include "mf_video_output.hpp"
#include "pcm_wave_file.hpp"
#include "mp4_video.hpp"
#include "x64arm/audio.hpp"
#include <future>
#include <chrono>
#include "d3d11_guids.hpp"
#include "x64arm/media_surface.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace x64arm {
void mf_buffer::initialize(GuestMemory& memory,std::map<std::uint64_t,RuntimeImage>& images,bool x86,std::uint64_t gateway){
    const auto bytes=x86?std::span<const std::uint8_t>{image_x86}:std::span<const std::uint8_t>{image_x64};
    const auto base=gateway+image_offset;auto parsed=parse_pe_image(bytes,GuestAddress{base},true,memory.limit());
    if(!parsed.imports.empty() || !parsed.delay_imports.empty() || parsed.metadata.image_size>0x10000)
        throw std::logic_error("MF buffer 常式包含外部相依或超出常駐映像範圍");
    for(const auto& [address,image]:images)if(base<address+image.size && address<base+parsed.metadata.image_size)
        throw std::logic_error("MF buffer 常式與既有常駐映像重疊");
    auto image=parsed.runtime;image.private_code=true;image.path=u"[guest-mf-buffer]";image.export_names.clear();image.export_ordinals.clear();
    for(const auto& [name,ordinal]:parsed.runtime.export_names)image.export_names.emplace(name,parsed.runtime.export_ordinals.at(ordinal));
    for(auto& mapping:parsed.mappings)mapping.permissions=(mapping.permissions&permission(Access::Execute))?read_execute:permission(Access::Read);
    map_pe_image(memory,parsed);images.emplace(base,std::move(image));
}
void mf_image::initialize(GuestMemory& memory,std::map<std::uint64_t,RuntimeImage>& images,bool x86,std::uint64_t gateway){
    const auto bytes=x86?std::span<const std::uint8_t>{image_x86}:std::span<const std::uint8_t>{image_x64};
    const auto base=gateway+image_offset;auto parsed=parse_pe_image(bytes,GuestAddress{base},true,memory.limit());
    if(!parsed.imports.empty() || !parsed.delay_imports.empty() || parsed.metadata.image_size>0x10000)
        throw std::logic_error("MF 影像常式包含外部相依或超出常駐映像範圍");
    for(const auto& [address,image]:images)if(base<address+image.size && address<base+parsed.metadata.image_size)
        throw std::logic_error("MF 影像常式與既有常駐映像重疊");
    auto image=parsed.runtime;image.private_code=true;image.path=u"[guest-mf-image]";image.export_names.clear();image.export_ordinals.clear();
    for(const auto& [name,ordinal]:parsed.runtime.export_names)image.export_names.emplace(name,parsed.runtime.export_ordinals.at(ordinal));
    for(auto& mapping:parsed.mappings)mapping.permissions=(mapping.permissions&permission(Access::Execute))?read_execute:permission(Access::Read);
    map_pe_image(memory,parsed);images.emplace(base,std::move(image));
}
namespace {
using mf::Guid;
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,
    missing=0xc00d36e6U,type_error=0xc00d36bdU,small=0xc00d36b1U,not_initialized=0xc00d36b6U,
    shutdown=0xc00d3e85U,no_events=0xc00d3e80U,invalid_index=0xc00d36bfU;
struct Failure {std::uint32_t code;};
struct Value {
    std::uint32_t type{};std::uint64_t scalar{};Guid id{};std::u16string text;std::vector<std::uint8_t> blob;
    bool operator==(const Value&)const=default;
};
// 屬性存放區的序列化（MFGetAttributesAsBlob／MFInitAttributesFromBlob、MFSerializeAttributesToStream 與 MF_MT_WRAPPED_TYPE 共用）：
// UINT32 筆數，之後每筆 {GUID 鍵, UINT32 型別, 內容}；UINT32 為 4 位元組、UINT64／double 為 8 位元組、GUID 為 16 位元組、
// 字串為 UINT32 字元數＋UTF-16、BLOB 為 UINT32 長度＋位元組；IUnknown 只在 by_reference 時寫入 8 位元組指標，否則略過。
std::vector<std::uint8_t> encode_attributes(const std::map<Guid,Value>& attributes,bool by_reference){
    std::vector<std::uint8_t> out(4);std::uint32_t count=0;
    const auto put=[&](std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
    for(const auto& [key,value]:attributes){
        if(value.type==13 && !by_reference)continue;
        out.insert(out.end(),key.begin(),key.end());put(value.type,4);++count;
        switch(value.type){
        case 19:put(value.scalar,4);break;case 21:case 5:case 13:put(value.scalar,8);break;
        case 72:out.insert(out.end(),value.id.begin(),value.id.end());break;
        case 31:put(value.text.size(),4);for(const auto c:value.text)put(c,2);break;
        case 0x1011:put(value.blob.size(),4);out.insert(out.end(),value.blob.begin(),value.blob.end());break;
        default:throw Failure{type_error};
        }
    }
    for(unsigned i=0;i<4;++i)out[i]=static_cast<std::uint8_t>(count>>(8*i));return out;
}
std::optional<std::vector<std::pair<Guid,Value>>> decode_attributes(std::span<const std::uint8_t> bytes){
    std::size_t at=0;
    const auto take=[&](unsigned size)->std::optional<std::uint64_t>{if(bytes.size()-at<size)return std::nullopt;std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[at+i]}<<(8*i);at+=size;return value;};
    const auto count=take(4);if(!count || *count>65536)return std::nullopt;
    std::vector<std::pair<Guid,Value>> items;
    for(std::uint64_t n=0;n<*count;++n){
        if(bytes.size()-at<20)return std::nullopt;Guid key{};std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),16,key.begin());at+=16;
        Value value;value.type=static_cast<std::uint32_t>(*take(4));
        switch(value.type){
        case 19:{const auto v=take(4);if(!v)return std::nullopt;value.scalar=*v;break;}
        case 21:case 5:case 13:{const auto v=take(8);if(!v)return std::nullopt;value.scalar=*v;break;}
        case 72:if(bytes.size()-at<16)return std::nullopt;std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),16,value.id.begin());at+=16;break;
        case 31:{const auto length=take(4);if(!length || *length>bytes.size())return std::nullopt;for(std::uint64_t i=0;i<*length;++i){const auto c=take(2);if(!c)return std::nullopt;value.text.push_back(static_cast<char16_t>(*c));}break;}
        case 0x1011:{const auto length=take(4);if(!length || bytes.size()-at<*length)return std::nullopt;value.blob.assign(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+*length));at+=*length;break;}
        default:return std::nullopt;
        }
        items.emplace_back(key,std::move(value));
    }
    return items;
}
struct ByteStreamState {
    std::shared_ptr<HostFile> file;
    std::uint64_t position{};std::uint32_t capabilities{};bool closed{};
    // 非零時後端為客體 IStream（MFCreateMFByteStreamOnStream），file 為空；引用由所屬物件的 parent 持有。
    std::uint64_t guest_stream{};
};
struct FileSourceState {
    std::shared_ptr<HostFile> file;PcmWaveStream wave;std::uint64_t base{},external_stream{};
    std::shared_ptr<const mf::Mp4Movie> movie; // 非空時為 MP4 來源，wave 不使用。
    // 非空時為 ASF 來源，wave 不使用；demux 是整個來源共用的封包解多工狀態（Reader 與 MediaStream 都由此取樣）。
    std::shared_ptr<const mf::AsfMovie> asf;std::shared_ptr<mf::AsfDemux> demux;
};
#include "mf_guest_bytestream.hpp"
#include "mf_session_state.hpp"
#include "mf_asf_decode.hpp"
struct SinkWriterState {
    std::shared_ptr<ByteStreamState> stream;
    std::uint64_t base{};
    std::optional<PcmWaveFormat> output,input;
    std::uint64_t bytes{},samples{},ticks{},last_time{},last_tick{},last_request{},started_at{};
    std::uint32_t header_size{},failure{};
    bool begun{},finalized{};
    bool mp4{},encode{};unsigned stride{};
    std::optional<video::EncoderConfig> video;
    std::unique_ptr<video::HardwareEncoder> encoder;
    std::vector<std::vector<std::uint8_t>> parameters;
    std::vector<mp4::Sample> video_samples;
    std::uint64_t encoded{},last_encoded{};
    struct Notice {bool final;std::uint64_t context;std::uint32_t status;};
    std::deque<Notice> notices;
};
// 宿主與常駐程式共用同一份 buffer 狀態，不在服務邊界另複製或掃描物件。
template<unsigned Bits> class BufferCell {
    GuestMemory* memory_{};std::uint64_t address_{},local_{};
public:
    BufferCell(std::uint64_t value=0):local_(value){}
    operator std::uint64_t()const{return memory_?memory_->load(GuestAddress{address_},Bits):local_;}
    BufferCell& operator=(std::uint64_t value){if constexpr(Bits==32)value=static_cast<std::uint32_t>(value);if(memory_)memory_->store(GuestAddress{address_},Bits,value);else local_=value;return *this;}
    BufferCell& operator=(const BufferCell& other){return *this=static_cast<std::uint64_t>(other);}
    void bind(GuestMemory& memory,std::uint64_t address){memory_=&memory;address_=address;memory_->store(GuestAddress{address_},Bits,local_);}
    std::uint32_t operator++(){if(memory_)return static_cast<std::uint32_t>(memory_->atomic_rmw(GuestAddress{address_},Bits,GuestMemory::AtomicRmw::Add,1)+1);return static_cast<std::uint32_t>(++local_);}
    std::uint32_t operator--(){if(memory_)return static_cast<std::uint32_t>(memory_->atomic_rmw(GuestAddress{address_},Bits,GuestMemory::AtomicRmw::Add,UINT32_MAX)-1);return static_cast<std::uint32_t>(--local_);}
};
// MFCreateVideoSampleAllocatorEx 的樣本池：owned 是配置器建立且仍歸它管理的樣本，free 是參照歸零後回收的閒置樣本。
struct SamplePool {std::vector<std::uint64_t> free;std::set<std::uint64_t> owned;std::uint64_t manager{},notify{},type{};
    std::uint32_t maximum{},width{},height{},code{},format{},bind{},usage{},misc{};bool initialized{},bottom_up{};};
struct Object {
    mf::Kind kind{};BufferCell<32> refs{1};std::map<mf::Kind,std::uint64_t> pointers;std::map<Guid,Value> attributes;
    std::uint64_t allocation{},parent{},state{},callback{},result_object{},queue_owner{};BufferCell<64> data{};
    BufferCell<32> maximum{},length{},locks{};std::uint32_t flags{},status{},identifier{};
    std::optional<std::uint64_t> time,duration;
    Guid extended{};Value value;
    std::vector<std::uint64_t> children;std::vector<bool> selected;std::uint64_t current_type{};
    std::deque<std::uint64_t> events;bool closed{};
    std::optional<MediaSurface> surface;bool bottom_up{};std::uint32_t locks_2d{},lock_flags{},reset_token{};
    std::uint64_t generation{},lock_owner{},lock_handle{};
    std::map<std::uint64_t,std::uint64_t> device_handles;
    std::uint64_t attribute_store{},input_attributes{},output_attributes{},output_type{};
    std::shared_ptr<video::HardwareDecoder> decoder;unsigned nal_length_size{4};
    std::future<std::vector<video::DecodedFrame>> decoding;std::deque<video::DecodedFrame> frames;
    std::shared_ptr<video::HardwareEncoder> encoder;
    std::future<std::vector<video::EncodedPacket>> encoding;std::deque<video::EncodedPacket> packets;
    bool streaming{},draining{},drain_pending{},codec_drained{},job_held{},poll_queued{},request_pending{},disconnect_source{};
    unsigned input_credit{},output_credit{};std::int64_t lower_bound{INT64_MIN},upper_bound{INT64_MAX};
    std::shared_ptr<audio::Buffer> capture;std::unique_ptr<audio::Input> input;std::uint64_t read_frames{},pending_token{};
    std::shared_ptr<SinkWriterState> writer;
    std::shared_ptr<FileSourceState> file_source;
    // ASF Source Reader 要求過解碼輸出後建立；之後此 Reader 的讀取都經由各串流的宿主輸出佇列。
    std::shared_ptr<AsfReaderDecode> asf_decode;
    bool file_read_active{},flush_pending{};std::uint64_t file_read_generation{};
    std::uint32_t request_stream{}; // 多串流來源的非同步 ReadSample 所要求的串流索引；UINT32_MAX 為任一串流。
    std::shared_ptr<ByteStreamState> byte_stream;
    std::vector<std::uint64_t> media_streams; // MP4 來源的 MediaStream，與 Source 互持到 Shutdown。
    // Topology／TopologyNode：輸出持有下游引用，輸入是下游對上游的弱參照。
    struct NodeLink {std::uint64_t node{};std::uint32_t index{};};
    std::uint64_t topo_id{};std::uint32_t node_type{};std::vector<NodeLink> outputs,inputs;std::map<std::uint32_t,std::uint64_t> output_prefs,input_prefs;
    std::shared_ptr<SessionState> session;bool event_dispatching{};
    // MFCreateSystemTimeSource：state 為 MFCLOCK_STATE（0 無效、1 執行、2 停止、3 暫停），clock／system 是最近一次轉換時的時鐘與系統時間。
    struct Timing {std::uint32_t state{};std::int64_t clock{},system{};float rate{1};} timing;
    // IMFTrackedSample（tracked 為 MFCreateTrackedSample 建立）與樣本池歸屬。
    bool tracked{};std::uint64_t tracker{},tracker_state{},pool_owner{};std::shared_ptr<SamplePool> pool;
    // MFCreateD3D12SynchronizationObject：就緒與釋放各一個 ID3D12Fence，數值單調遞增。
    struct D3D12Sync {std::uint64_t device{},ready{},release{},ready_value{},release_value{};};std::shared_ptr<D3D12Sync> d3d12;
};
struct Entry {std::shared_ptr<Object> object;mf::Kind kind;};
}
struct MediaFoundationState {
    std::map<std::uint64_t,Entry> objects;std::map<mf::Kind,std::uint64_t> tables;
    std::vector<std::uint32_t> startups;
    std::uint64_t shared_manager{},next_device_handle{0x10000},next_query{1};std::uint32_t manager_locks{},next_reset_token{1};
    struct Query {std::shared_ptr<Object> manager;std::uint64_t handle{},generation{},output{},owner{};bool locking{};std::uint64_t scratch{},service_pointer{};unsigned service_stage{};std::uint32_t service_status{};};
    std::map<std::uint64_t,Query> queries;
    std::uint64_t next_topology_id{1};
    // MFPutWorkItem 系列：由 poll_media_foundation 派送到客體工作執行緒。
    struct WorkItem {std::uint64_t result{},invoke{},callback{};std::uint32_t queue{};};
    std::deque<WorkItem> work_items;
    // MFAllocateWorkQueue 系列的使用者佇列：type 0 標準、1 視窗、2 多執行緒；非多執行緒佇列一次只派送一個工作（busy）。
    // mmcss 非空時為 MMCSS 工作佇列：task 是工作索引，mmcss_task 在派送工作時套用到工作執行緒（MmcssEnter／MmcssLeave）。
    struct WorkQueue {std::uint32_t type{},target{},locks{1},task{};bool busy{};std::u16string mmcss;MultimediaThreadTask mmcss_task{};};
    std::map<std::uint32_t,WorkQueue> queues;std::map<std::u16string,std::uint32_t> shared_queues;
    std::uint32_t next_queue{0x10001},platform_locks{};
    // MFRegisterPlatformWithMMCSS：標準佇列（識別碼 0～8）共用的 MMCSS 設定；mmcss_threads 記錄目前套用中的工作執行緒與工作 handle。
    WorkQueue platform;std::map<std::uint64_t,std::uint64_t> mmcss_threads;
    // MFTRegisterLocal／MFTRegisterLocalByCLSID 的本機登錄（factory 非零時持有類別工廠的參照）；登錄表中的 MFT 也以同一結構讀出。
    struct LocalTransform {mf::Guid clsid{},category{};std::u16string name;std::uint32_t flags{};std::vector<std::uint8_t> inputs,outputs,attributes;std::uint64_t factory{};};
    std::vector<LocalTransform> local_transforms;
    // MFRegisterLocalSchemeHandler／MFRegisterLocalByteStreamHandler：scheme（含冒號）、副檔名（不含點）與 MIME 皆為小寫；後登錄者優先。
    struct LocalHandler {std::u16string scheme,extension,mime;std::uint64_t activate{};};
    std::vector<LocalHandler> local_handlers;
    // MFJoinWorkQueue：檔案以完成鍵＝cookie 繫結到 MF 的內部完成埠；封包的 OVERLAPPED 位於 MFASYNCRESULT 的介面指標之後，完成時放入加入的工作佇列。
    struct Join {std::uint32_t queue{};std::uint64_t file{};};
    std::uint64_t io_port{},next_join{1};std::map<std::uint64_t,Join> joins;std::set<std::uint64_t> bound_files;
    // MFGetPluginControl：程序內唯一物件；偏好以 {類型, 小寫選擇字串} 為鍵（值為原字串與 CLSID），停用清單為 {類型, CLSID}。
    std::uint64_t plugin_control{};
    struct Plugins {std::map<std::pair<std::uint32_t,std::u16string>,std::pair<std::u16string,mf::Guid>> preferred;std::vector<std::pair<std::uint32_t,mf::Guid>> disabled;std::uint32_t policy{};} plugins;
    // 排程、等待與週期工作：handle 是計時器佇列計時器或 RegisterWaitForSingleObject 的等待物件。
    struct Timed {std::uint64_t handle{},result{},callback{},context{};bool waiting{},periodic{};};
    std::map<std::uint64_t,Timed> timed;std::uint64_t next_key{1};
};
bool WindowsServices::media_foundation_ready(std::uint64_t pointer,std::uint64_t thread) const {
    (void)thread;
    if(!media_foundation_)return true;
    const auto found=media_foundation_->objects.find(pointer);
    if(found==media_foundation_->objects.end())return true;const auto& item=*found->second.object;
    if(item.kind==mf::Reader){const auto source=media_foundation_->objects.find(item.parent);if(source==media_foundation_->objects.end() || source->second.object->closed)return true;const auto& capture=source->second.object->capture;if(!capture)return true;std::lock_guard lock(capture->mutex);return capture->capture_failed || capture->captured_frames>item.read_frames;}
    return item.kind==mf::Manager?!item.lock_handle:item.closed || !item.events.empty();
}
bool WindowsServices::media_foundation_waiting() const {
    if(!media_foundation_)return false;if(!media_foundation_->work_items.empty())return true;
    if(media_foundation_->io_port)if(const auto port=runtime_.completion_ports.find(media_foundation_->io_port);port!=runtime_.completion_ports.end() && !port->second.packets.empty())return true;for(const auto& [id,thread]:threads_.items){(void)id;if(thread.status==ThreadStatus::Waiting && thread.wait.kind==ThreadWaitKind::MediaFoundation && !thread.wait.handles.empty()){const auto found=media_foundation_->objects.find(thread.wait.handles.front());if(found!=media_foundation_->objects.end() && (found->second.object->kind==mf::Reader || found->second.object->kind==mf::Session))return true;}}for(const auto& [pointer,entry]:media_foundation_->objects){(void)pointer;if(entry.kind==entry.object->kind && entry.object->job_held)return true;}return false;
}
void WindowsServices::poll_media_foundation(GuestMemory& memory){
    if(!media_foundation_ || threads_.items.empty() || registered_waits_.size()>=64)return;
    const auto dispatch=[&](std::vector<GuestCallback> batch){
        if(registered_waits_.size()>=64)return false;const auto saved_error=last_error_;
        const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));last_error_=saved_error;if(!worker)return false;
        RegisteredWait work;work.worker=worker;work.callback_batch=std::move(batch);work.armed=false;work.ready=true;work.closing=true;work.work_item=true;
        const auto id=next_registered_wait_++;registered_waits_.emplace(id,std::move(work));wait_workers_[worker]=id;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};return true;
    };
    auto& work_items=media_foundation_->work_items;
    // MFJoinWorkQueue 的 I/O 完成：依 OVERLAPPED 找回 MFASYNCRESULT（本引擎的非同步結果物件），寫回 hrStatusResult／dwBytesTransferred 後交給加入的佇列。
    // 已解除加入的 cookie 或不是本引擎非同步結果的 OVERLAPPED 不處理。
    if(media_foundation_->io_port)if(const auto port=runtime_.completion_ports.find(media_foundation_->io_port);port!=runtime_.completion_ports.end()){
        const unsigned unit=x86_?4U:8U;const std::uint8_t width=x86_?32:64;
        while(!port->second.packets.empty()){
            const auto packet=port->second.packets.front();port->second.packets.pop_front();
            const auto join=media_foundation_->joins.find(packet.key);if(join==media_foundation_->joins.end() || packet.overlapped<unit)continue;
            const auto result=packet.overlapped-unit;const auto found=media_foundation_->objects.find(result);
            if(found==media_foundation_->objects.end() || found->second.object->kind!=mf::Async || !found->second.object->callback)continue;
            auto& item=*found->second.object;const auto error=completion_packet_error(packet.status);item.status=error?0x80070000U|(error&0xffffU):0U;
            memory.store(GuestAddress{result+(x86_?28U:48U)},32,item.status);memory.store(GuestAddress{result+(x86_?32U:52U)},32,static_cast<std::uint32_t>(packet.bytes));
            ++item.refs;const auto table=memory.load(GuestAddress{item.callback},width);
            work_items.push_back({result,memory.load(GuestAddress{table+4*unit},width),item.callback,join->second.queue});
        }
    }
    // 非多執行緒的使用者佇列依提交順序一次派送一個，工作結束時由 internal-mf!QueueDone 解除；其他佇列可並行。
    for(auto it=work_items.begin();it!=work_items.end();){
        const auto item=*it;const auto queue=media_foundation_->queues.find(item.queue);
        const bool serial=queue!=media_foundation_->queues.end() && queue->second.type!=2;
        if(serial && queue->second.busy){++it;continue;}
        std::vector<GuestCallback> batch{{item.invoke,{item.callback,item.result}},{resolve("internal-mf","Release").value,{item.result}}};
        // MMCSS 工作佇列：工作執行緒在 Invoke 前套用該佇列的 MMCSS 工作設定，結束後還原（工作執行緒由多個佇列共用）。
        const auto& owner=item.queue<=8?media_foundation_->platform:queue!=media_foundation_->queues.end()?queue->second:media_foundation_->platform;
        if((item.queue<=8 || queue!=media_foundation_->queues.end()) && owner.mmcss_task.handle){
            batch.insert(batch.begin(),{resolve("internal-mf","MmcssEnter").value,{item.queue}});batch.push_back({resolve("internal-mf","MmcssLeave").value,{item.queue}});
        }
        if(serial)batch.push_back({resolve("internal-mf","QueueDone").value,{item.queue}});
        if(!dispatch(std::move(batch)))return;
        if(serial)queue->second.busy=true;
        it=work_items.erase(it);
    }
    std::vector<std::uint64_t> ready,session_pumps,session_events;
    for(const auto& [pointer,entry]:media_foundation_->objects){
        const auto& item=*entry.object;if(entry.kind!=item.kind || !item.job_held)continue;
        if(item.kind==mf::Session){
            if(item.closed)continue;
            if(item.callback && !item.events.empty() && !item.event_dispatching)session_events.push_back(pointer);
            if(!item.poll_queued && item.session->pump_ready(runtime_.clock_ticks))session_pumps.push_back(pointer);
            continue;
        }
        if(item.poll_queued)continue;
        bool complete=item.closed || !item.request_pending;
        if(item.kind==mf::Transform)complete=(!item.decoding.valid() || item.decoding.wait_for(std::chrono::seconds(0))==std::future_status::ready) && (!item.encoding.valid() || item.encoding.wait_for(std::chrono::seconds(0))==std::future_status::ready);
        else if(item.request_pending){const auto found=media_foundation_->objects.find(item.parent);if(found==media_foundation_->objects.end() || found->second.object->closed)complete=true;
            else if(found->second.object->file_source)complete=true;
            else if(const auto capture=found->second.object->capture){std::lock_guard lock(capture->mutex);complete=capture->capture_failed || capture->captured_frames>item.read_frames;}}
        if(complete)ready.push_back(pointer);
    }
    for(const auto pointer:session_events){
        if(!dispatch({{resolve("internal-mf","SessionDeliver").value,{pointer}}}))return;
        media_foundation_->objects.at(pointer).object->event_dispatching=true;
    }
    for(const auto pointer:session_pumps){
        if(!dispatch({{resolve("internal-mf","SessionPump").value,{pointer}}}))return;
        media_foundation_->objects.at(pointer).object->poll_queued=true;
    }
    for(const auto pointer:ready){
        if(!dispatch({{resolve("internal-mf","PipelinePoll").value,{pointer}}}))break;
        media_foundation_->objects.at(pointer).object->poll_queued=true;
    }
}
void WindowsServices::discard_media_foundation_query(std::uint64_t token,GuestMemory& memory,GuestState& state){
    if(!media_foundation_)return;const auto found=media_foundation_->queries.find(token);if(found==media_foundation_->queries.end())return;
    const auto query=found->second;media_foundation_->queries.erase(found);
    if(query.scratch)(void)free_heap(memory,query.scratch,state);
    if(query.locking && query.manager->generation==query.generation && query.manager->lock_handle==query.handle && query.manager->lock_owner==query.owner)query.manager->lock_owner=query.manager->lock_handle=0;
}
std::optional<std::uint32_t> WindowsServices::resume_media_foundation_query(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint32_t status){
    const auto found=media_foundation_->queries.find(token);if(found==media_foundation_->queries.end())throw HostCallFault("MF 裝置查詢續行不存在");const auto query=found->second;
    if(query.scratch){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64),unit=static_cast<std::uint8_t>(bits/8);
        const auto method=[&](std::uint64_t pointer,unsigned slot){const auto table=memory.load(GuestAddress{pointer},bits);return memory.load(GuestAddress{table+slot*unit},bits);};
        try {
            if(query.service_stage==0 && !(status&0x80000000U)){
                const auto pointer=memory.load(GuestAddress{query.scratch+16},bits);
                if(pointer){auto& pending=found->second;pending.service_pointer=pointer;pending.service_stage=1;
                    start_callbacks(state,memory,{{method(pointer,3),{pointer,query.scratch+24,query.scratch+40,query.output}}},Continuation::MediaFoundationQuery,token);return std::nullopt;}
                status=0x80004002U;
            }else if(query.service_stage==1){
                found->second.service_status=status;found->second.service_stage=2;
                start_callbacks(state,memory,{{method(query.service_pointer,2),{query.service_pointer}}},Continuation::MediaFoundationQuery,token);return std::nullopt;
            }else if(query.service_stage==2)status=query.service_status;
        }catch(...){discard_media_foundation_query(token,memory,state);throw;}
        discard_media_foundation_query(token,memory,state);complete_call(state,memory,status);return std::nullopt;
    }
    if(!(status&0x80000000U) && query.manager->generation!=query.generation){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto pointer=memory.load(GuestAddress{query.output},bits);memory.store(GuestAddress{query.output},bits,0);status=0x80041001U;discard_media_foundation_query(token,memory,state);
        if(pointer){const auto table=memory.load(GuestAddress{pointer},bits),release=memory.load(GuestAddress{table+2*(bits/8U)},bits);start_callbacks(state,memory,{{release,{pointer}}},Continuation::Return,status);return std::nullopt;}
    }else if(status&0x80000000U)discard_media_foundation_query(token,memory,state);
    else media_foundation_->queries.erase(found);
    complete_call(state,memory,status);return std::nullopt;
}
std::span<const WindowsServiceDescriptor> media_foundation_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{
            {"mfplat.dll","MFStartup",2,0,0,3},{"mfplat.dll","MFShutdown",0},
            {"mfplat.dll","MFGetSystemTime",0},{"mfplat.dll","MFFrameRateToAverageTimePerFrame",3,0,0,3},
            {"mfplat.dll","MFGetStrideForBitmapInfoHeader",3,0,0,3},{"mfplat.dll","MFGetPlaneSize",4,0,0,7},
            {"mfplat.dll","MFCalculateImageSize",4,0,0,6},{"mfplat.dll","MFCopyImage",6,0,0,0x3a},
            {"mfplat.dll","MFConvertFromFP16Array",3,0,0,4},{"mfplat.dll","MFConvertToFP16Array",3,0,0,4},
            {"mfplat.dll","MFCreateAttributes",2,0,0,2},{"mfplat.dll","MFCreateMediaType",1},
            {"mfplat.dll","MFCreateSample",1},{"mfplat.dll","MFCreateMemoryBuffer",2,0,0,1},
            {"mfplat.dll","MFCreateAlignedMemoryBuffer",3,0,0,3},{"mfplat.dll","MFCreateMediaBufferWrapper",4,0,0,6},
            {"mfplat.dll","MFCreateAsyncResult",4},{"mfplat.dll","MFInvokeCallback",1},
            {"mfplat.dll","MFCreateMediaEvent",5,0,0,5},{"mfplat.dll","MFCreateEventQueue",1},
            {"mfplat.dll","MFCreateStreamDescriptor",4,0,0,3},{"mfplat.dll","MFCreatePresentationDescriptor",3,0,0,1},
            {"mfplat.dll","MFInitMediaTypeFromWaveFormatEx",3,0,0,4},{"mfplat.dll","MFCreateWaveFormatExFromMFMediaType",4,0,0,8},
            {"mfplat.dll","MFCreateDXGIDeviceManager",2},{"mfplat.dll","MFLockDXGIDeviceManager",2},{"mfplat.dll","MFUnlockDXGIDeviceManager",0},
            {"mfplat.dll","MFCreateDXGISurfaceBuffer",5,0,0,12},
            {"mf.dll","MFGetService",4},{"mfplat.dll","MFTEnum",7,0,1,2},{"mfplat.dll","MFTEnumEx",6,0,1,2},{"mfplat.dll","MFTEnum2",7,0,1,2},
            {"mfplat.dll","MFTRegister",9,0,3,0x58},{"mfplat.dll","MFTUnregister",1,0,1},{"mfplat.dll","MFTGetInfo",7,0,1},{"mfplat.dll","MFTRegisterLocal",8,0,0,0x58},
            {"mfplat.dll","MFTRegisterLocalByCLSID",8,0,0,0x58},{"mfplat.dll","MFTUnregisterLocal",1},{"mfplat.dll","MFTUnregisterLocalByCLSID",1,0,1},{"mf.dll","MFEnumDeviceSources",3},{"mf.dll","MFCreateDeviceSource",2},
            {"mfplat.dll","MFCreateSourceResolver",1},{"mfreadwrite.dll","MFCreateSourceReaderFromURL",3},{"mfreadwrite.dll","MFCreateSourceReaderFromByteStream",3},{"mfreadwrite.dll","MFCreateSourceReaderFromMediaSource",3},{"mfreadwrite.dll","MFCreateSinkWriterFromURL",4},{"mfplat.dll","MFCreateFile",5,0,0,7},{"mfplat.dll","MFCreateMFByteStreamOnStream",2},{"mfplat.dll","MFCreateMFByteStreamOnStreamEx",2},{"mfplat.dll","MFCreateStreamOnMFByteStream",2},{"mfplat.dll","MFCreateStreamOnMFByteStreamEx",3},{"internal-mf","PipelinePoll",1},
            {"mf.dll","MFCreateMediaSession",2},{"mf.dll","MFCreateTopology",1},{"mf.dll","MFCreateTopologyNode",2,0,0,1},{"mf.dll","MFCreateSampleGrabberSinkActivate",3},{"mf.dll","MFCreateAudioRendererActivate",1},
            {"mfplat.dll","MFCreateCollection",1},{"mfplat.dll","MFPutWorkItem",3,0,0,1},{"mfplat.dll","MFPutWorkItem2",4,0,0,3},{"mfplat.dll","MFPutWorkItemEx",2,0,0,1},{"mfplat.dll","MFPutWorkItemEx2",3,0,0,3},
            {"internal-mf","SessionPump",1},{"internal-mf","SessionPumpDone",1},{"internal-mf","SessionDeliver",1},
            {"mfplat.dll","MFAllocateWorkQueue",1},{"mfplat.dll","MFAllocateWorkQueueEx",2,0,0,1},{"mfplat.dll","MFAllocateSerialWorkQueue",2,0,0,1},
            {"mfplat.dll","MFLockWorkQueue",1,0,0,1},{"mfplat.dll","MFUnlockWorkQueue",1,0,0,1},{"mfplat.dll","MFLockSharedWorkQueue",4,0,0,2},
            {"mfplat.dll","MFLockPlatform",0},{"mfplat.dll","MFUnlockPlatform",0},{"mfplat.dll","MFGetTimerPeriodicity",1},
            {"mfplat.dll","MFScheduleWorkItem",4,0x4},{"mfplat.dll","MFScheduleWorkItemEx",3,0x2},{"mfplat.dll","MFCancelWorkItem",1,0x1},
            {"mfplat.dll","MFPutWaitingWorkItem",4,0,0,2},{"mfplat.dll","MFAddPeriodicCallback",3},{"mfplat.dll","MFRemovePeriodicCallback",1,0,0,1},
            {"mfplat.dll","MFGetAttributesAsBlobSize",2},{"mfplat.dll","MFGetAttributesAsBlob",3,0,0,4},{"mfplat.dll","MFInitAttributesFromBlob",3,0,0,4},
            {"mfplat.dll","MFSerializeAttributesToStream",3,0,0,2},{"mfplat.dll","MFDeserializeAttributesFromStream",3,0,0,2},
            {"mfplat.dll","MFWrapMediaType",4},{"mfplat.dll","MFUnwrapMediaType",2},{"mfplat.dll","MFCompareFullToPartialMediaType",2},
            {"mfplat.dll","MFAverageTimePerFrameToFrameRate",3,0x1},{"mfplat.dll","MFllMulDiv",4,0xf},{"mfplat.dll","MFHeapAlloc",5,0,0,0x1a},{"mfplat.dll","MFHeapFree",1},
            {"mfplat.dll","MFCreatePathFromURL",2},
            {"mfplat.dll","MFCreateVideoMediaTypeFromSubtype",2},{"mfplat.dll","MFCreateVideoMediaType",2},{"mfplat.dll","MFCreateAudioMediaType",2},
            {"mfplat.dll","MFCreateVideoMediaTypeFromVideoInfoHeader",8,0x20,0,0x1e},{"mfplat.dll","MFInitMediaTypeFromVideoInfoHeader",4,0,0,4},
            {"mfplat.dll","MFInitMediaTypeFromVideoInfoHeader2",4,0,0,4},{"mfplat.dll","MFInitMediaTypeFromMPEG1VideoInfo",4,0,0,4},{"mfplat.dll","MFInitMediaTypeFromMPEG2VideoInfo",4,0,0,4},
            {"mfplat.dll","MFInitMediaTypeFromMFVideoFormat",3,0,0,4},{"mfplat.dll","MFInitMediaTypeFromAMMediaType",2},{"mfplat.dll","MFInitAMMediaTypeFromMFMediaType",3,0,2},
            {"mfplat.dll","MFCreateAMMediaTypeFromMFMediaType",3,0,2},{"mfplat.dll","MFCreateMFVideoFormatFromMFMediaType",3},{"mfplat.dll","MFCreateMediaTypeFromRepresentation",3,0,1},
            {"mfplat.dll","MFInitVideoFormat_RGB",4,0,0,0xe},{"mfplat.dll","MFConvertColorInfoToDXVA",2},{"mfplat.dll","MFMapDX9FormatToDXGIFormat",1,0,0,1},
            {"mfplat.dll","MFMapDXGIFormatToDX9Format",1,0,0,1},
            {"mfplat.dll","MFBeginCreateFile",7,0,0,7},{"mfplat.dll","MFEndCreateFile",2},{"mfplat.dll","MFCancelCreateFile",1},{"mfplat.dll","MFCreateTempFile",4,0,0,7},
            {"mfplat.dll","MFCreate2DMediaBuffer",5,0,0,0xf},{"mfplat.dll","MFCreateMediaBufferFromMediaType",5,0x2,0,0xc},{"mfplat.dll","MFCreateLegacyMediaBufferOnMFMediaBuffer",4,0,0,4},
            {"mfplat.dll","CreatePropertyStore",1},{"mfplat.dll","MFCreateTransformActivate",1},{"mfplat.dll","MFCreateMFByteStreamWrapper",2},
            {"mfplat.dll","MFGetPluginControl",1},{"mfplat.dll","MFCreateSystemTimeSource",1},{"mfplat.dll","MFCreateTrackedSample",1},{"mfplat.dll","MFCreateD3D12SynchronizationObject",3},{"mfplat.dll","MFCreateDXSurfaceBuffer",4,0,0,4},{"mfplat.dll","MFJoinWorkQueue",3,0,0,1},{"mfplat.dll","MFUnjoinWorkQueue",2,0,0,1},{"mfplat.dll","MFRegisterLocalSchemeHandler",2},{"mfplat.dll","MFRegisterLocalByteStreamHandler",3},
            {"internal-mf","ResolverDeliver",4,0,0,0xc},{"mfplat.dll","MFCreateVideoSampleAllocatorEx",2},
            {"internal-mf","TimedFire",2,0,0,2},{"internal-mf","PeriodicFire",2,0,0,2},{"internal-mf","QueueDone",1,0,0,1},{"internal-mf","MmcssEnter",1,0,0,1},{"internal-mf","MmcssLeave",1,0,0,1},
            {"mfplat.dll","MFBeginRegisterWorkQueueWithMMCSS",5,0,0,5},{"mfplat.dll","MFBeginRegisterWorkQueueWithMMCSSEx",6,0,0,0xd},
            {"mfplat.dll","MFEndRegisterWorkQueueWithMMCSS",2},{"mfplat.dll","MFBeginUnregisterWorkQueueWithMMCSS",3,0,0,1},{"mfplat.dll","MFEndUnregisterWorkQueueWithMMCSS",1},
            {"mfplat.dll","MFGetWorkQueueMMCSSClass",3,0,0,1},{"mfplat.dll","MFGetWorkQueueMMCSSTaskId",2,0,0,1},{"mfplat.dll","MFGetWorkQueueMMCSSPriority",2,0,0,1},
            {"mfplat.dll","MFRegisterPlatformWithMMCSS",3,0,0,4},{"mfplat.dll","MFUnregisterPlatformFromMMCSS",0},
            {"internal-mf","BufferDestroy",1},{"internal-mf","B_LockHost",4},{"internal-mf","B_UnlockHost",1}};
        std::set<std::string_view> names;const auto append=[&](const auto& methods){for(const auto& m:methods)if(names.insert(m.name).second)result.push_back({"internal-mf",m.name,m.args,m.wide,m.aggregate,m.narrow});};
        append(mf::common);for(unsigned k=0;k<=mf::D3D12SyncObject;++k)append(mf::methods(static_cast<mf::Kind>(k)));
        return result;
    }();return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_media_foundation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if (service.symbol == "MFConvertFromFP16Array" || service.symbol == "MFConvertToFP16Array") {
        const WindowsCall call(state, memory, service);
        const auto destination = call(0);
        const auto source = call(1);
        const auto count = static_cast<std::uint32_t>(call(2));
        const bool expand = service.symbol == "MFConvertFromFP16Array";
        MfFp16Plan plan;
        const auto status = mf_fp16_plan(destination, source, count, x86_ ? UINT32_MAX : UINT64_MAX, expand, &plan);
        if (!status && count) {
            if (expand) {
                const auto convert = [&](std::uint64_t index) {
                    const auto value = static_cast<std::uint16_t>(memory.load(GuestAddress{source + 2 * index}, 16));
                    memory.store(GuestAddress{destination + 4 * index}, 32, mf_fp16_expand(value));
                };
                const auto split = plan.overlaps ? plan.split : count;
                for (std::uint32_t index = 0; index < split; ++index) convert(index);
                for (std::uint32_t index = count; index > split; --index) convert(index - 1);
            } else {
                const auto convert = [&](std::uint64_t index) {
                    const auto value = static_cast<std::uint32_t>(memory.load(GuestAddress{source + 4 * index}, 32));
                    memory.store(GuestAddress{destination + 2 * index}, 16, mf_fp16_narrow(value));
                };
                for (std::uint32_t index = plan.split; index > 0; --index) convert(index - 1);
                for (std::uint32_t index = plan.split; index < count; ++index) convert(index);
            }
        }
        call.return_to(state, status);
        return std::nullopt;
    }
    // 純影像工具保留完整宿主回退，不配置 COM／解碼器狀態。
    if(service.symbol=="MFGetStrideForBitmapInfoHeader" || service.symbol=="MFGetPlaneSize" || service.symbol=="MFCalculateImageSize" || service.symbol=="MFCopyImage"){
        const WindowsCall a(state,memory,service);const auto d=[&](unsigned i){return static_cast<std::uint32_t>(a(i));};std::uint32_t status=0;
        if(service.symbol=="MFCopyImage"){
            if(d(4) && d(5)){
                if(!a(0) || !a(2))status=MF_IMAGE_POINTER;
                else{
                    auto dest=a(0),src=a(2);const auto mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;
                    const auto ds=static_cast<std::int64_t>(static_cast<std::int32_t>(d(1))),ss=static_cast<std::int64_t>(static_cast<std::int32_t>(d(3)));
                    std::array<std::uint8_t,4096> bytes{};
                    for(std::uint32_t y=0;y<d(5);++y){
                        for(std::uint64_t x=0;x<d(4);){const auto count=std::min<std::uint64_t>(bytes.size(),d(4)-x);const auto span=std::span<std::uint8_t>{bytes}.first(count);
                            memory.read(GuestAddress{(src+x)&mask},span);memory.write(GuestAddress{(dest+x)&mask},span);x+=count;}
                        dest=(dest+static_cast<std::uint64_t>(ds))&mask;src=(src+static_cast<std::uint64_t>(ss))&mask;
                    }
                }
            }
        }else if(service.symbol=="MFGetStrideForBitmapInfoHeader"){
            if(!a(2))status=MF_IMAGE_POINTER;else {MfImageLong value=0;status=mf_image_stride(d(0),d(1),&value);if(!status)memory.store(GuestAddress{a(2)},32,static_cast<std::uint32_t>(value));}
        }else if(!a(3))status=MF_IMAGE_POINTER;
        else {
            memory.store(GuestAddress{a(3)},32,0);MfImageWord format=d(0),size=0;const bool bitmap=service.symbol=="MFCalculateImageSize";
            if(bitmap){if(!a(0))status=MF_IMAGE_POINTER;else {std::array<std::uint8_t,16> id{};memory.read(GuestAddress{a(0)},id);format=mf_image_guid_format(id.data());}}
            if(!status){status=mf_image_size(format,d(1),d(2),bitmap,&size);if(!status)memory.store(GuestAddress{a(3)},32,size);}
        }
        a.return_to(state,status);return std::nullopt;
    }
    if(!media_foundation_)media_foundation_=std::make_shared<MediaFoundationState>();auto& bridge=*media_foundation_;
    if(service.symbol=="V_ActivateObject"){
        // 登錄或本機登錄的 MFT（identifier 6：CoCreateInstance；7：本機類別工廠）：建立後保留，之後以該物件的 QueryInterface 取得要求的介面。
        const WindowsCall probe(state,memory,service);
        if(const auto found=bridge.objects.find(probe(0));found!=bridge.objects.end() && found->second.object->kind==mf::Activate && (found->second.object->identifier==6 || found->second.object->identifier==7)){
            auto item=found->second.object;
            return nested_continuation(state,memory,service.symbol,[this,service,item](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const std::uint8_t width=x86_?32:64;const unsigned size_of_pointer=width/8U;
                const auto finish=[&](std::uint32_t hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
                const auto method=[&](std::uint64_t object,unsigned slot){const auto table=m.load(GuestAddress{object},width);return m.load(GuestAddress{table+slot*size_of_pointer},width);};
                if(!call(1) || !call(2))co_return finish(0x80004003U);
                m.store(GuestAddress{call(2)},width,0);
                if(!item->result_object){
                    const auto clsid=item->attributes.find(mf::MFT_TRANSFORM_CLSID_Attribute);if(clsid==item->attributes.end() || clsid->second.type!=72)co_return finish(0xc00d36e6U);
                    const auto block=allocate_heap(m,48);if(!block)co_return finish(0x8007000eU);
                    m.write(GuestAddress{block},clsid->second.id);m.write(GuestAddress{block+16},mf::unknown);m.store(GuestAddress{block+32},width,0);
                    const auto hr=static_cast<std::uint32_t>(item->identifier==6?
                        co_await service_callback::call_value(resolve("ole32.dll","CoCreateInstance").value,{block,0,1,block+16,block+32}):
                        co_await service_callback::call_value(method(item->callback,3),{item->callback,0,block+16,block+32}));
                    const auto created=hr?0:m.load(GuestAddress{block+32},width);(void)free_heap(m,block,s);
                    if(hr)co_return finish(hr);if(!created)co_return finish(0x80004003U);
                    if(item->result_object)(void)co_await service_callback::call_value(method(created,2),{created});else item->result_object=created;
                }
                const auto target=item->result_object;
                co_return finish(static_cast<std::uint32_t>(co_await service_callback::call_value(method(target,0),{target,call(1),call(2)})));
            });
        }
    }
    if(service.symbol=="MFSerializeAttributesToStream" || service.symbol=="MFDeserializeAttributesFromStream"){
        // (pAttr, dwOptions, pStm)：串流內容為 UINT32 長度＋encode_attributes 的格式，經 ISequentialStream::Write／Read 讀寫。
        // MF_ATTRIBUTE_SERIALIZE_UNKNOWN_BYREF（1）時 IUnknown 以指標寫入並由串流持有一個參照，讀回時接收該參照；否則略過 IUnknown。
        return nested_continuation(state,memory,service.symbol,[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool serialize=service.symbol=="MFSerializeAttributesToStream";const std::uint8_t width=x86_?32:64;const unsigned size_of_pointer=width/8U;
            const auto finish=[&](std::uint32_t hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
            std::vector<std::uint64_t> blocks;
            struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(memory,block,state);}} cleanup{*this,m,s,blocks};
            const auto scratch=[&](std::size_t length)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(length,8));if(!block)throw std::bad_alloc{};blocks.push_back(block);return block;};
            const auto method=[&](std::uint64_t object,unsigned slot){const auto table=m.load(GuestAddress{object},width);return m.load(GuestAddress{table+slot*size_of_pointer},width);};
            const auto attributes=call(0),stream=call(2);const bool by_reference=(static_cast<std::uint32_t>(call(1))&1U)!=0;
            const auto store=[&]()->Object*{const auto found=media_foundation_->objects.find(attributes);return found==media_foundation_->objects.end() || !mf::attributes(found->second.object->kind)?nullptr:found->second.object.get();};
            if(!attributes || !stream)co_return finish(pointer_error);if(!store())co_return finish(no_interface);
            if(serialize){
                std::vector<std::uint8_t> body;try{body=encode_attributes(store()->attributes,by_reference);}catch(const Failure& failure){co_return finish(failure.code);}
                std::vector<std::uint64_t> shared;if(by_reference)for(const auto& [key,value]:store()->attributes)if(value.type==13 && value.scalar)shared.push_back(value.scalar);
                std::vector<std::uint8_t> bytes(4);for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(body.size()>>(8*i));bytes.insert(bytes.end(),body.begin(),body.end());
                const auto buffer=scratch(bytes.size()+4);m.write(GuestAddress{buffer},bytes);const auto written=buffer+bytes.size();m.store(GuestAddress{written},32,0);
                const auto hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(stream,4),{stream,buffer,bytes.size(),written}));
                if(hr)co_return finish(hr);if(m.load(GuestAddress{written},32)!=bytes.size())co_return finish(0x80030070U);
                for(const auto object:shared)(void)co_await service_callback::call_value(method(object,1),{object});
                co_return finish(0);
            }
            const auto header=scratch(8);
            auto hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(stream,3),{stream,header,4,header+4}));
            if(hr)co_return finish(hr);if(m.load(GuestAddress{header+4},32)!=4)co_return finish(0x80004005U);
            const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{header},32));if(length>16U*1024U*1024U)co_return finish(invalid);
            const auto body=scratch(static_cast<std::size_t>(length)+4);
            hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(stream,3),{stream,body,length,body+length}));
            if(hr)co_return finish(hr);if(m.load(GuestAddress{body+length},32)!=length)co_return finish(0x80004005U);
            std::vector<std::uint8_t> bytes(length);if(length)m.read(GuestAddress{body},bytes);const auto items=decode_attributes(bytes);if(!items)co_return finish(invalid);
            for(const auto& [key,value]:*items)if(value.type==13 && !by_reference)co_return finish(invalid);
            if(!store())co_return finish(no_interface);
            std::vector<std::uint64_t> previous;for(const auto& [key,value]:store()->attributes)if(value.type==13 && value.scalar)previous.push_back(value.scalar);
            store()->attributes.clear();for(const auto& [key,value]:*items)store()->attributes.insert_or_assign(key,value);
            for(const auto object:previous)(void)co_await service_callback::call_value(method(object,2),{object});
            co_return finish(0);
        });
    }
    const WindowsCall arg(state,memory,service);const auto name=service.symbol=="B_LockHost"?std::string_view{"B_Lock"}:service.symbol=="B_UnlockHost"?std::string_view{"B_Unlock"}:service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=bits/8U;
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto require=[](bool test,std::uint32_t code=invalid){if(!test)throw Failure{code};};
    const auto check=[&](std::uint64_t p,std::size_t size){require(p!=0,pointer_error);memory.check(GuestAddress{p},size,Access::Write);};
    const auto put=[&](std::uint64_t p,std::uint64_t v,std::uint8_t width=32){check(p,width/8);memory.store(GuestAddress{p},width,v);};
    const auto out=[&](std::uint64_t p){put(p,0,bits);};
    const auto guid=[&](std::uint64_t p){require(p!=0,pointer_error);Guid id{};memory.read(GuestAddress{p},id);return id;};
    const auto data=[&](std::uint64_t p,std::size_t size){require(size<=64U*1024U*1024U);std::vector<std::uint8_t> bytes(size);if(size){require(p!=0,pointer_error);memory.read(GuestAddress{p},bytes);}return bytes;};
    const auto allocate=[&](std::size_t size){require(size<=256U*1024U*1024U);const auto p=allocate_heap(memory,std::max<std::size_t>(size,1));if(!p)throw std::bad_alloc{};return p;};
    const auto entry=[&](std::uint64_t p)->Entry&{const auto found=bridge.objects.find(p);require(found!=bridge.objects.end(),no_interface);return found->second;};
    const auto object=[&](std::uint64_t p,mf::Kind kind){auto item=entry(p).object;require(item->kind==kind,no_interface);return item;};
    std::vector<GuestCallback> callbacks;
    const auto foreign_method=[&](std::uint64_t p,unsigned slot){require(p!=0,pointer_error);const auto table=memory.load(GuestAddress{p},bits);return memory.load(GuestAddress{table+slot*unit},bits);};
    const auto hold=[&](std::uint64_t p){if(!p)return;if(const auto found=bridge.objects.find(p);found!=bridge.objects.end()){require(found->second.object->refs<UINT32_MAX);++found->second.object->refs;}else callbacks.push_back({foreign_method(p,1),{p}});};
    std::function<void(std::uint64_t)> release;
    std::function<void(const std::shared_ptr<Object>&)> close_source,close_session;
    const auto destroy=[&](std::uint64_t p){
        auto item=entry(p).object;
        if(item->session && close_session)close_session(item);
        if(item->kind==mf::Reader && !item->disconnect_source && item->parent && bridge.objects.contains(item->parent) && close_source)close_source(bridge.objects.at(item->parent).object);
        for(const auto& [kind,pointer]:item->pointers){(void)kind;bridge.objects.erase(pointer);runtime_.critical_sections.erase(pointer);(void)free_heap(memory,pointer,state);}
        for(const auto& [key,value]:item->attributes){(void)key;if(value.type==13)release(value.scalar);}
        if(item->value.type==13)release(item->value.scalar);
        release(item->attribute_store);release(item->input_attributes);release(item->output_attributes);release(item->output_type);release(item->pending_token);
        release(item->parent);release(item->state);release(item->callback);release(item->result_object);release(item->current_type);
        for(const auto child:item->children)release(child);for(const auto event:item->events)release(event);
        for(const auto stream:item->media_streams)release(stream);
        for(const auto& link:item->outputs)if(link.node){
            if(const auto target=bridge.objects.find(link.node);target!=bridge.objects.end() && link.index<target->second.object->inputs.size())target->second.object->inputs[link.index]={};
            release(link.node);
        }
        for(const auto& [index,type]:item->output_prefs){(void)index;release(type);}for(const auto& [index,type]:item->input_prefs){(void)index;release(type);}
        if(item->allocation)(void)free_heap(memory,item->allocation,state);
        release(item->tracker);release(item->tracker_state);
        if(item->d3d12){const auto sync=item->d3d12;item->d3d12.reset();release(sync->ready);release(sync->release);release(sync->device);}
        if(item->pool){
            // 配置器銷毀：閒置樣本一併銷毀，仍在使用的樣本脫離（之後照一般樣本釋放）。
            auto pool=item->pool;item->pool.reset();
            for(const auto sample:pool->owned)if(const auto found=bridge.objects.find(sample);found!=bridge.objects.end())found->second.object->pool_owner=0;
            for(const auto sample:pool->free)if(const auto found=bridge.objects.find(sample);found!=bridge.objects.end()){found->second.object->refs=1;release(sample);}
            release(pool->manager);release(pool->notify);release(pool->type);
        }
    };
    const auto create=[&](const std::shared_ptr<Object>& item,mf::Kind kind){
        if(item->pointers.contains(kind))return item->pointers.at(kind);
        require(bridge.objects.size()<16384,0x8007000eU);
        if(!bridge.tables.contains(kind)){
            std::vector<mf::Method> methods(mf::common,mf::common+3);
            if(mf::attributes(kind) && kind!=mf::Attributes)methods.insert(methods.end(),std::begin(mf::methods_Attributes),std::end(mf::methods_Attributes));
            const auto extra=mf::methods(kind);methods.insert(methods.end(),extra.begin(),extra.end());
            const auto table=runtime_allocate(memory,{},methods.size()*unit);
            const auto image=runtime_.builtin_images.find(callback_return_.value+mf_buffer::image_offset);
            for(std::size_t n=0;n<methods.size();++n){
                auto address=resolve("internal-mf",methods[n].name).value;
                if(kind==mf::Buffer && image!=runtime_.builtin_images.end()){
                    const auto method=n<3?std::string{"Buffer"}+std::string(methods[n].name):std::string(methods[n].name);
                    if(const auto found=image->second.export_names.find(method);found!=image->second.export_names.end())address=image->first+found->second;
                }
                put(table+n*unit,address,bits);
            }
            bridge.tables.emplace(kind,table);
        }
        // 非同步結果配置 MFASYNCRESULT 的大小（介面指標＋OVERLAPPED＋pCallback＋hrStatusResult＋dwBytesTransferred＋hEvent），供 MFJoinWorkQueue 的 I/O 使用。
        const std::size_t object_bytes=kind==mf::Buffer?MF_BUFFER_BYTES:kind==mf::Async?(x86_?40U:64U):unit;
        const auto p=allocate(object_bytes);if(kind==mf::Async)memory.fill(GuestAddress{p},0,object_bytes);put(p,bridge.tables.at(kind),bits);
        if(kind==mf::Buffer){
            memory.fill(GuestAddress{p+8},0,MF_BUFFER_BYTES-8);
            item->refs.bind(memory,p+MF_BUFFER_REFS);item->locks.bind(memory,p+MF_BUFFER_LOCKS);item->data.bind(memory,p+MF_BUFFER_DATA);
            item->maximum.bind(memory,p+MF_BUFFER_MAXIMUM);item->length.bind(memory,p+MF_BUFFER_LENGTH);
            put(p+MF_BUFFER_DESTROY,resolve("internal-mf","BufferDestroy").value,bits);
            put(p+MF_BUFFER_LOCK_HOST,resolve("internal-mf","B_LockHost").value,bits);
            put(p+MF_BUFFER_UNLOCK_HOST,resolve("internal-mf","B_UnlockHost").value,bits);
            put(p+MF_BUFFER_QUERY_HOST,resolve("internal-mf","QueryInterface").value,bits);
        }
        item->pointers.emplace(kind,p);bridge.objects.emplace(p,Entry{item,kind});return p;
    };
    const auto make=[&](mf::Kind kind){auto item=std::make_shared<Object>();item->kind=kind;return create(item,kind);};
    release=[&](std::uint64_t p){
        if(!p)return;const auto found=bridge.objects.find(p);if(found==bridge.objects.end()){callbacks.push_back({foreign_method(p,2),{p}});return;}
        auto& item=*found->second.object;if(--item.refs)return;
        // 樣本配置器的樣本：參照歸零時回到配置器的閒置清單（不銷毀），並通知 IMFVideoSampleAllocatorNotify::NotifyRelease。
        if(item.kind==mf::Sample && item.pool_owner){
            if(const auto owner=bridge.objects.find(item.pool_owner);owner!=bridge.objects.end() && owner->second.object->pool && owner->second.object->pool->owned.contains(p)){
                auto& pool=*owner->second.object->pool;pool.free.push_back(p);if(pool.notify)callbacks.push_back({foreign_method(pool.notify,3),{pool.notify}});return;}
            item.pool_owner=0;
        }
        // IMFTrackedSample：參照歸零時不銷毀，改由非同步結果持有並經標準工作佇列呼叫 SetAllocator 給的回呼（一次性）。
        if(item.kind==mf::Sample && item.tracker){
            const auto callback=item.tracker,context=item.tracker_state;item.tracker=item.tracker_state=0;item.refs=1;
            const auto r=make(mf::Async);auto completion=object(r,mf::Async);completion->result_object=item.pointers.at(mf::Sample);completion->callback=callback;completion->state=context;
            bridge.work_items.push_back({r,foreign_method(callback,4),callback,1});return;
        }
        destroy(p);
    };
    const auto query=[&](std::uint64_t p,const Guid& iid,std::uint64_t output){
        out(output);const auto& source=entry(p);auto item=source.object;std::uint64_t pointer=0;
        if(iid==mf::unknown || iid==mf::iid(item->kind) || (iid==mf::iid_Attributes && mf::attributes(item->kind)))pointer=item->pointers.at(item->kind);
        // IMFAudioMediaType：只有主類型為音訊的媒體類型提供，與 IMFMediaType 是同一個物件（GetAudioFormat 接在 IMFMediaType 的方法之後）。
        else if(iid==mf::iid_AudioMediaType && item->kind==mf::MediaType && item->attributes.contains(mf::MF_MT_MAJOR_TYPE) && item->attributes.at(mf::MF_MT_MAJOR_TYPE).id==mf::MFMediaType_Audio)pointer=item->pointers.at(mf::MediaType);
        // IMFVideoMediaType：主要型別為視訊時同一個物件（GetVideoFormat 與 GetAudioFormat 佔同一個方法位置，之後是 GetVideoRepresentation）。
        // DMO IMediaBuffer 包裝：建立時給了樣本則可查出 IMFSample。
        else if(item->kind==mf::LegacyBuffer && iid==mf::iid(mf::Sample) && item->state)pointer=item->state;
        else if(item->kind==mf::PluginControl && iid==mf::iid_PluginControl)pointer=item->pointers.at(mf::PluginControl);
        else if(item->kind==mf::Sample && item->tracked && iid==mf::iid_TrackedSample)pointer=create(item,mf::TrackedSample);
        else if(item->kind==mf::SampleAllocator && iid==mf::iid_SampleAllocator)pointer=item->pointers.at(mf::SampleAllocator);
        else if(item->kind==mf::SampleAllocator && iid==mf::iid_AllocatorCallback)pointer=create(item,mf::AllocatorCallback);
        else if(item->kind==mf::TimeSource && iid==mf::iid_Clock)pointer=item->pointers.at(mf::TimeSource);
        else if(item->kind==mf::D3D12Sync && iid==mf::iid_D3D12SyncObject)pointer=create(item,mf::D3D12SyncObject);
        else if(item->kind==mf::TimeSource && iid==mf::iid_StateSink)pointer=create(item,mf::StateSink);
        else if(iid==mf::iid_VideoMediaType && item->kind==mf::MediaType && item->attributes.contains(mf::MF_MT_MAJOR_TYPE) && item->attributes.at(mf::MF_MT_MAJOR_TYPE).id==mf::MFMediaType_Video)pointer=item->pointers.at(mf::MediaType);
        else if(iid==mf::iid_Generator && (item->kind==mf::Transform || item->kind==mf::Source || item->kind==mf::MediaStream))pointer=create(item,mf::Generator);
        else if(iid==mf::iid_Shutdown && item->kind==mf::Transform)pointer=create(item,mf::Shutdown);
        else if(item->kind==mf::Stream && iid==mf::iid_TypeHandler)pointer=create(item,mf::TypeHandler);
        else if(item->kind==mf::ComStream && iid==mf::iid_SequentialStream)pointer=item->pointers.at(mf::ComStream);
        else if(item->kind==mf::Session && (iid==mf::iid_Clock || iid==mf::iid_PresentationClock))pointer=create(item,mf::Clock);
        else if(item->kind==mf::Session && iid==mf::iid_Generator)pointer=item->pointers.at(mf::Session);
        else if(item->kind==mf::Session && iid==mf::iid_RateControl)pointer=create(item,mf::RateControl);
        else if(item->kind==mf::Session && iid==mf::iid_RateSupport)pointer=create(item,mf::RateSupport);
        else if(item->kind==mf::Session && iid==mf::iid_GetService)pointer=create(item,mf::GetService);
        // D3D9 表面緩衝提供 IMFGetService（MR_BUFFER_SERVICE），不提供 IMFDXGIBuffer。
        else if(item->kind==mf::Buffer && item->extended==mf::iid_D3D9Surface && iid==mf::iid_GetService)pointer=create(item,mf::GetService);
        else if(item->surface && ((iid==mf::iid_Surface && item->extended!=mf::iid_D3D9Surface) || iid==mf::iid_TwoD || iid==mf::iid_TwoD2))pointer=create(item,iid==mf::iid_Surface?mf::Surface:iid==mf::iid_TwoD?mf::TwoD:mf::TwoD2);
        else throw Failure{no_interface};hold(pointer);put(output,pointer,bits);
    };
    const auto read_value=[&](std::uint64_t p){
        require(p!=0,pointer_error);memory.check(GuestAddress{p},x86_?16:24,Access::Read);Value v;v.type=static_cast<std::uint32_t>(memory.load(GuestAddress{p},16));
        switch(v.type){case 0:break;case 4:case 19:v.scalar=memory.load(GuestAddress{p+8},32);break;case 20:case 21:case 5:v.scalar=memory.load(GuestAddress{p+8},64);break;
        case 13:v.scalar=memory.load(GuestAddress{p+8},bits);break;case 72:v.id=guid(memory.load(GuestAddress{p+8},bits));break;
        case 31:v.text=read_utf16(memory,GuestAddress{memory.load(GuestAddress{p+8},bits)});break;
        case 0x1011:{const auto size=memory.load(GuestAddress{p+8},32);v.blob=data(memory.load(GuestAddress{p+(x86_?12:16)},bits),size);break;}
        default:throw Failure{type_error};}return v;
    };
    const auto write_value=[&](std::uint64_t p,const Value& v){
        check(p,x86_?16:24);memory.write(GuestAddress{p},std::vector<std::uint8_t>(x86_?16:24));put(p,v.type,16);
        switch(v.type){case 0:break;case 4:case 19:put(p+8,v.scalar);break;case 20:case 21:case 5:put(p+8,v.scalar,64);break;case 13:hold(v.scalar);put(p+8,v.scalar,bits);break;
        case 72:{const auto id=allocate(16);memory.write(GuestAddress{id},v.id);put(p+8,id,bits);break;}
        case 31:{const auto bytes=utf16_bytes(v.text);const auto text=allocate(bytes.size());memory.write(GuestAddress{text},bytes);put(p+8,text,bits);break;}
        case 0x1011:{const auto blob=allocate(v.blob.size());memory.write(GuestAddress{blob},v.blob);put(p+8,v.blob.size());put(p+(x86_?12:16),blob,bits);break;}
        default:throw Failure{type_error};}
    };
    const auto set=[&](Object& item,const Guid& key,Value value){
        require(value.type==19 || value.type==21 || value.type==5 || value.type==72 || value.type==31 || value.type==0x1011 || value.type==13,type_error);
        require(item.attributes.size()<65536 || item.attributes.contains(key),0x8007000eU);
        if(value.type==13)hold(value.scalar);if(const auto old=item.attributes.find(key);old!=item.attributes.end() && old->second.type==13)release(old->second.scalar);
        item.attributes.insert_or_assign(key,std::move(value));
    };
    const auto get=[&](const Object& item,const Guid& key,std::uint32_t type=0)->const Value&{const auto found=item.attributes.find(key);require(found!=item.attributes.end(),missing);require(!type || type==found->second.type,type_error);return found->second;};
    const auto memory_buffer=[&](std::uint32_t size,std::uint32_t mask){
        require(mask<=8191 && (mask&(mask+1))==0);const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);item->allocation=allocate(static_cast<std::size_t>(size)+mask);item->data=(item->allocation+mask)&~static_cast<std::uint64_t>(mask);item->maximum=size;put(p+MF_BUFFER_PLAIN,1);return p;
    };
    const auto flip=[](std::vector<std::uint8_t>& bytes,const Object& item){if(item.bottom_up && item.surface->format!=103){const auto pitch=item.surface->row_pitch,height=item.surface->height;for(unsigned y=0;y<height/2;++y)for(unsigned x=0;x<pitch;++x)std::swap(bytes[static_cast<std::size_t>(y)*pitch+x],bytes[static_cast<std::size_t>(height-y-1)*pitch+x]);}};
    std::function<void(Object&,bool,bool)> map_buffer;
    map_buffer=[&](Object& item,bool linear,bool write_only){
        if(item.surface){
            if(!item.allocation){item.allocation=allocate(item.maximum);item.data=item.allocation;}
            if(!item.locks && !item.locks_2d && !write_only){auto bytes=item.surface->read();require(bytes.size()==item.maximum,0x80004005U);if(linear)flip(bytes,item);memory.write(GuestAddress{item.data},bytes);}
        }else if(item.parent && bridge.objects.contains(item.parent) && entry(item.parent).object->kind==mf::Buffer){auto parent=entry(item.parent).object;map_buffer(*parent,linear,write_only);item.data=parent->data+item.identifier;}
    };
    std::function<void(Object&,bool)> flush_buffer;
    flush_buffer=[&](Object& item,bool linear){if(item.surface){auto bytes=data(item.data,item.maximum);if(linear)flip(bytes,item);item.surface->write(bytes);}else if(item.parent && bridge.objects.contains(item.parent) && entry(item.parent).object->kind==mf::Buffer)flush_buffer(*entry(item.parent).object,linear);};
    const auto sample_bytes=[&](const Object& sample){std::vector<std::uint8_t> result;for(const auto p:sample.children){const auto buffer=object(p,mf::Buffer);map_buffer(*buffer,true,false);require(result.size()+buffer->length<=256U*1024U*1024U);const auto part=data(buffer->data,buffer->length);result.insert(result.end(),part.begin(),part.end());}return result;};
    const auto event=[&](std::uint32_t type,const Guid& extended,std::uint32_t status,Value value){const auto p=make(mf::Event);auto item=object(p,mf::Event);item->identifier=type;item->extended=extended;item->status=status;item->value=std::move(value);if(item->value.type==13)hold(item->value.scalar);return p;};
    const auto deliver=[&](std::uint64_t queue_pointer,const std::shared_ptr<Object>& queue){
        if(!queue->callback || queue->events.empty())return;
        const auto p=make(mf::Async);auto result=object(p,mf::Async);result->queue_owner=queue_pointer;result->result_object=queue->events.front();queue->events.pop_front();result->state=queue->state;result->callback=queue->callback;queue->state=queue->callback=0;
        callbacks.push_back({foreign_method(result->callback,4),{result->callback,p}});
        // callback 期間保留 result；callback 結束後才釋放佇列持有的引用。
        callbacks.push_back({resolve("internal-mf","Release").value,{p}});
    };
    #include "mf_guest_bytestream_helpers.inc"
    #include "mf_pipeline_helpers.inc"
    #include "mf_sink_helpers.inc"
    #include "mf_asf_source_helpers.inc"
    #include "mf_session_source_helpers.inc"
    #include "mf_file_source_helpers.inc"
    #include "mf_session_helpers.inc"
    #include "mf_format_helpers.inc"
    std::uint64_t result=0;
    try {
        #include "mf_file_source_exports.inc"
        else
        #include "mf_session_exports.inc"
        else
        #include "mf_file_exports.inc"
        else
        #include "mf_bytestream_exports.inc"
        else
        #include "mf_sink_exports.inc"
        else
        #include "mf_pipeline_exports.inc"
        else
        #include "mf_work_queue_exports.inc"
        else
        #include "mf_attribute_exports.inc"
        else
        #include "mf_format_exports.inc"
        else
        #include "mf_transform_exports.inc"
        else
        #include "mf_runtime_exports.inc"
        else
        if(name=="MFGetService"){
            out(arg(3));require(arg(0)!=0,pointer_error);const auto service_id=guid(arg(1)),iid=guid(arg(2));
            const auto scratch=allocate(56);const auto token=bridge.next_query++;
            MediaFoundationState::Query pending;pending.output=arg(3);pending.scratch=scratch;bridge.queries.emplace(token,pending);
            try {
                memory.write(GuestAddress{scratch},mf::guid(0xfa993888,0x4383,0x415a,{0xa9,0x30,0xdd,0x47,0x2a,0x8c,0xf6,0xf7}));
                put(scratch+16,0,bits);memory.write(GuestAddress{scratch+24},service_id);memory.write(GuestAddress{scratch+40},iid);
                start_callbacks(state,memory,{{foreign_method(arg(0),0),{arg(0),scratch,scratch+16}}},Continuation::MediaFoundationQuery,token);
            }catch(...){discard_media_foundation_query(token,memory,state);throw;}return std::nullopt;
        }else if(name=="MFStartup"){
            require(d(0)==0x20070U || d(0)==0x10070U,0xc00d36e3U);require(d(1)<=1);require(bridge.startups.size()<65536,0x8007000eU);
            if(!d(1)){require(runtime_.network.startups<UINT32_MAX,0x8007000eU);++runtime_.network.startups;}
            bridge.startups.push_back(d(1));
        }else if(name=="MFShutdown"){
            require(!bridge.startups.empty(),not_initialized);const auto flags=bridge.startups.back();bridge.startups.pop_back();if(!flags && runtime_.network.startups)--runtime_.network.startups;
        }else if(name=="MFGetSystemTime")result=runtime_.clock_ticks;
        else if(name=="MFFrameRateToAverageTimePerFrame"){require(d(0)!=0 && d(1)!=0);put(arg(2),(UINT64_C(10000000)*d(1)+d(0)/2)/d(0),64);}
        else if(name=="MFCreateAttributes" || name=="MFCreateMediaType" || name=="MFCreateSample" || name=="MFCreateEventQueue"){
            out(arg(0));if(name=="MFCreateAttributes")require(d(1)<=65536);
            put(arg(0),make(name=="MFCreateAttributes"?mf::Attributes:name=="MFCreateMediaType"?mf::MediaType:name=="MFCreateSample"?mf::Sample:mf::Queue),bits);
        }else if(name=="MFCreateMemoryBuffer" || name=="MFCreateAlignedMemoryBuffer"){
            const auto output=arg(name=="MFCreateMemoryBuffer"?1:2);out(output);put(output,memory_buffer(d(0),name=="MFCreateMemoryBuffer"?0:d(1)),bits);
        }else if(name=="MFCreateMediaBufferWrapper"){
            out(arg(3));auto parent=object(arg(0),mf::Buffer);require(d(1)<=parent->length && d(2)<=parent->length-d(1));map_buffer(*parent,true,false);const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);hold(arg(0));item->parent=arg(0);item->identifier=d(1);item->data=parent->data+d(1);item->length=item->maximum=d(2);put(p+MF_BUFFER_PLAIN,memory.load(GuestAddress{arg(0)+MF_BUFFER_PLAIN},32));put(arg(3),p,bits);
        }else if(name=="MFCreateAsyncResult"){
            out(arg(3));const auto p=make(mf::Async);auto item=object(p,mf::Async);hold(arg(0));hold(arg(1));hold(arg(2));item->result_object=arg(0);item->callback=arg(1);item->state=arg(2);
            put(p+(x86_?24U:40U),arg(1),bits);put(arg(3),p,bits);
        }else if(name=="MFInvokeCallback"){
            auto item=object(arg(0),mf::Async);require(item->callback!=0);callbacks.push_back({foreign_method(item->callback,4),{item->callback,arg(0)}});
        }else if(name=="MFCreateMediaEvent"){out(arg(4));put(arg(4),event(d(0),guid(arg(1)),d(2),arg(3)?read_value(arg(3)):Value{}),bits);}
        else if(name=="MFCreateStreamDescriptor" || name=="MFCreatePresentationDescriptor"){
            const bool stream=name=="MFCreateStreamDescriptor";const auto count=d(stream?1:0);const auto list=arg(stream?2:1),output=arg(stream?3:2);out(output);require(count && count<=65536);memory.check(GuestAddress{list},count*unit,Access::Read);
            std::vector<std::uint64_t> children;for(unsigned n=0;n<count;++n){const auto child=memory.load(GuestAddress{list+n*unit},bits);(void)object(child,stream?mf::MediaType:mf::Stream);children.push_back(child);}
            const auto p=make(stream?mf::Stream:mf::Presentation);auto item=entry(p).object;item->identifier=stream?d(0):0;item->children=children;item->selected.resize(count,false);for(const auto child:children)hold(child);if(stream){item->current_type=children.front();hold(item->current_type);}put(output,p,bits);
        }else if(name=="MFInitMediaTypeFromWaveFormatEx"){
            auto item=object(arg(0),mf::MediaType);require(d(2)>=16);init_wave(*item,data(arg(1),d(2)));
        }else if(name=="MFCreateWaveFormatExFromMFMediaType"){
            out(arg(1));put(arg(2),0);auto item=object(arg(0),mf::MediaType);require(d(3)<=1);const auto bytes=pcm_wave_bytes(*item,!d(3));
            const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);put(arg(1),p,bits);put(arg(2),bytes.size());
        }else if(name=="MFCreateDXGIDeviceManager" || name=="MFLockDXGIDeviceManager"){
            check(arg(0),4);out(arg(1));const bool shared=name=="MFLockDXGIDeviceManager";require(!shared || bridge.manager_locks<UINT32_MAX,0x8007000eU);
            auto p=shared?bridge.shared_manager:0;if(!p){require(bridge.next_reset_token!=0,0x8007000eU);p=make(mf::Manager);object(p,mf::Manager)->reset_token=bridge.next_reset_token++;if(shared)bridge.shared_manager=p;}
            if(shared){++bridge.manager_locks;hold(p);}put(arg(0),object(p,mf::Manager)->reset_token);put(arg(1),p,bits);
        }else if(name=="MFUnlockDXGIDeviceManager"){
            require(bridge.manager_locks!=0,0xc00d36b2U);if(!--bridge.manager_locks){const auto p=bridge.shared_manager;bridge.shared_manager=0;release(p);}
        }else if(name=="MFCreateDXSurfaceBuffer"){
            // (riid, punkSurface, fBottomUpWhenLinear, ppBuffer)：riid 必須是 IID_IDirect3DSurface9；以 media_d3d9_surface 的讀寫提供 IMF2DBuffer／IMF2DBuffer2。
            out(arg(3));require(guid(arg(0))==mf::iid_D3D9Surface,no_interface);auto surface=media_d3d9_surface(arg(1));require(surface.has_value(),invalid);
            const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);item->surface=std::move(surface);item->bottom_up=d(2)!=0;item->parent=arg(1);item->extended=mf::iid_D3D9Surface;
            item->maximum=item->length=item->surface->size;hold(arg(1));put(arg(3),p,bits);
        }else if(name=="MFCreateDXGISurfaceBuffer"){
            out(arg(4));require(guid(arg(0))==d3d11_guid::IID_ID3D11Texture2D,no_interface);auto surface=media_graphics_surface(arg(1),d(2));require(surface.has_value(),invalid);const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);item->surface=std::move(surface);item->bottom_up=d(3)!=0;item->parent=arg(1);item->identifier=d(2);item->maximum=item->length=item->surface->size;hold(arg(1));put(arg(4),p,bits);
        }else {
            auto item=entry(arg(0)).object;
            if(name=="QueryInterface")query(arg(0),guid(arg(1)),arg(2));
            else if(name=="AddRef"){hold(arg(0));result=item->refs;}
            else if(name=="Release"){result=item->refs-1;release(arg(0));}
            else if(name=="BufferDestroy"){require(item->kind==mf::Buffer && !item->refs);destroy(arg(0));}
            #include "mf_bytestream_methods.inc"
            #include "mf_sink_methods.inc"
            #include "mf_pipeline_methods.inc"
            #include "mf_session_methods.inc"
            else if(name.starts_with("A_")){
                require(mf::attributes(item->kind),no_interface);
                auto& lock=runtime_.critical_sections[item->pointers.at(item->kind)];
                if(lock.owner && lock.owner!=threads_.current){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Critical;thread.wait.handles={item->pointers.at(item->kind)};schedule(state,memory);return std::nullopt;}
                const auto method=name.substr(2);
                if(method=="LockStore"){lock.owner=threads_.current;require(lock.recursion<UINT32_MAX);++lock.recursion;}
                else if(method=="UnlockStore"){require(lock.owner==threads_.current && lock.recursion,0x8000ffffU);if(!--lock.recursion)lock.owner=0;}
                else if(method=="GetCount")put(arg(1),item->attributes.size());
                else if(method=="DeleteAllItems"){for(const auto& [key,value]:item->attributes){(void)key;if(value.type==13)release(value.scalar);}item->attributes.clear();}
                else if(method=="DeleteItem"){const auto found=item->attributes.find(guid(arg(1)));if(found!=item->attributes.end()){if(found->second.type==13)release(found->second.scalar);item->attributes.erase(found);}}
                else if(method=="GetItemByIndex"){require(d(1)<item->attributes.size(),invalid_index);check(arg(2),16);const auto at=std::next(item->attributes.begin(),d(1));memory.write(GuestAddress{arg(2)},at->first);write_value(arg(3),at->second);}
                else if(method=="CopyAllItems"){auto destination=entry(arg(1)).object;require(mf::attributes(destination->kind),no_interface);const auto copy=item->attributes;for(const auto& [key,value]:copy)set(*destination,key,value);}
                else if(method=="Compare"){
                    auto theirs=entry(arg(1)).object;require(mf::attributes(theirs->kind),no_interface);require(d(2)<=4);const auto subset=[](const auto& a,const auto& b){return std::all_of(a.begin(),a.end(),[&](const auto& value){const auto found=b.find(value.first);return found!=b.end() && found->second==value.second;});};bool equal=false;
                    switch(d(2)){case 0:equal=subset(item->attributes,theirs->attributes);break;case 1:equal=subset(theirs->attributes,item->attributes);break;case 2:equal=item->attributes==theirs->attributes;break;case 3:equal=item->attributes.size()<theirs->attributes.size()?subset(item->attributes,theirs->attributes):subset(theirs->attributes,item->attributes);break;case 4:equal=std::all_of(item->attributes.begin(),item->attributes.end(),[&](const auto& value){const auto found=theirs->attributes.find(value.first);return found==theirs->attributes.end() || found->second==value.second;});break;}put(arg(3),equal);
                }else {
                    const auto key=guid(arg(1));
                    if(method.starts_with("Set")){
                        Value v;if(method=="SetItem")v=read_value(arg(2));else if(method=="SetUINT32"){v.type=19;v.scalar=d(2);}else if(method=="SetUINT64"){v.type=21;v.scalar=arg(2);}else if(method=="SetDouble"){v.type=5;if(x86_)v.scalar=arg(2);else std::memcpy(&v.scalar,state.xmm[2].data(),8);}else if(method=="SetGUID"){v.type=72;v.id=guid(arg(2));}else if(method=="SetString"){v.type=31;v.text=read_utf16(memory,GuestAddress{arg(2)});}else if(method=="SetBlob"){v.type=0x1011;v.blob=data(arg(2),d(3));}else if(method=="SetUnknown"){require(arg(2)!=0);v.type=13;v.scalar=arg(2);}else throw Failure{invalid};set(*item,key,std::move(v));
                    }else if(method=="CompareItem"){put(arg(3),get(*item,key)==read_value(arg(2)));}
                    else if(method=="GetItem"){const auto& v=get(*item,key);if(arg(2))write_value(arg(2),v);}
                    else if(method=="GetItemType")put(arg(2),get(*item,key).type);
                    else if(method=="GetUINT32")put(arg(2),get(*item,key,19).scalar);
                    else if(method=="GetUINT64" || method=="GetDouble")put(arg(2),get(*item,key,method=="GetUINT64"?21:5).scalar,64);
                    else if(method=="GetGUID"){check(arg(2),16);memory.write(GuestAddress{arg(2)},get(*item,key,72).id);}
                    else if(method=="GetStringLength")put(arg(2),get(*item,key,31).text.size());
                    else if(method=="GetBlobSize")put(arg(2),get(*item,key,0x1011).blob.size());
                    else if(method=="GetString" || method=="GetBlob"){
                        const bool string=method=="GetString";const auto& v=get(*item,key,string?31:0x1011);const auto bytes=string?utf16_bytes(v.text):v.blob;const auto size=string?v.text.size():v.blob.size();if(arg(4))put(arg(4),size);require(d(3)>=(string?size+1:size),small);if(!bytes.empty()){check(arg(2),bytes.size());memory.write(GuestAddress{arg(2)},bytes);}
                    }else if(method=="GetAllocatedString" || method=="GetAllocatedBlob"){
                        const bool string=method=="GetAllocatedString";out(arg(2));const auto& v=get(*item,key,string?31:0x1011);if(arg(3))put(arg(3),string?v.text.size():v.blob.size());else require(string,pointer_error);const auto bytes=string?utf16_bytes(v.text):v.blob;const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);put(arg(2),p,bits);
                    }else if(method=="GetUnknown"){
                        out(arg(3));const auto pointer=get(*item,key,13).scalar;const auto iid=guid(arg(2));if(bridge.objects.contains(pointer))query(pointer,iid,arg(3));else {start_callbacks(state,memory,{{foreign_method(pointer,0),{pointer,arg(2),arg(3)}}},Continuation::ReturnCallback);return std::nullopt;}
                    }else throw Failure{invalid};
                }
            }else if(name.starts_with("B_")){
                require(item->kind==mf::Buffer,no_interface);
                if(name=="B_Lock"){check(arg(1),unit);if(arg(2))check(arg(2),4);if(arg(3))check(arg(3),4);require(item->locks<UINT32_MAX && !item->locks_2d,0xc00d36b2U);map_buffer(*item,true,false);++item->locks;put(arg(1),item->data,bits);if(arg(2))put(arg(2),item->maximum);if(arg(3))put(arg(3),item->length);}
                else if(name=="B_Unlock"){require(item->locks);if(!--item->locks)flush_buffer(*item,true);}
                else if(name=="B_GetCurrentLength")put(arg(1),item->length);
                else if(name=="B_GetMaxLength")put(arg(1),item->maximum);
                else if(name=="B_SetCurrentLength"){require(d(1)<=item->maximum);item->length=d(1);}
            }else if(name.starts_with("S_")){
                require(item->kind==mf::Sample,no_interface);
                if(name=="S_GetSampleFlags")put(arg(1),item->flags);else if(name=="S_SetSampleFlags")item->flags=d(1);
                else if(name=="S_SetSampleTime")item->time=arg(1);else if(name=="S_SetSampleDuration")item->duration=arg(1);
                else if(name=="S_GetSampleTime"){require(item->time.has_value(),0xc00d36c8U);put(arg(1),*item->time,64);}else if(name=="S_GetSampleDuration"){require(item->duration.has_value(),0xc00d36c9U);put(arg(1),*item->duration,64);}
                else if(name=="S_GetBufferCount")put(arg(1),item->children.size());
                else if(name=="S_AddBuffer"){(void)object(arg(1),mf::Buffer);require(item->children.size()<65536);hold(arg(1));item->children.push_back(arg(1));}
                else if(name=="S_GetBufferByIndex"){out(arg(2));require(d(1)<item->children.size(),invalid_index);hold(item->children[d(1)]);put(arg(2),item->children[d(1)],bits);}
                else if(name=="S_RemoveBufferByIndex"){require(d(1)<item->children.size(),invalid_index);const auto p=item->children[d(1)];item->children.erase(item->children.begin()+d(1));release(p);}
                else if(name=="S_RemoveAllBuffers"){auto children=std::move(item->children);item->children.clear();for(const auto p:children)release(p);}
                else if(name=="S_GetTotalLength"){std::uint64_t size=0;for(const auto p:item->children)size+=object(p,mf::Buffer)->length;require(size<=UINT32_MAX);put(arg(1),size);}
                else if(name=="S_CopyToBuffer"){auto buffer=object(arg(1),mf::Buffer);const auto bytes=sample_bytes(*item);require(bytes.size()<=buffer->maximum,small);map_buffer(*buffer,true,false);memory.write(GuestAddress{buffer->data},bytes);buffer->length=static_cast<std::uint32_t>(bytes.size());flush_buffer(*buffer,true);}
                else if(name=="S_ConvertToContiguousBuffer"){
                    out(arg(1));require(!item->children.empty(),0x8000ffffU);if(item->children.size()==1){hold(item->children.front());put(arg(1),item->children.front(),bits);}else {const auto bytes=sample_bytes(*item);const auto p=memory_buffer(static_cast<std::uint32_t>(bytes.size()),0);auto buffer=object(p,mf::Buffer);memory.write(GuestAddress{buffer->data},bytes);buffer->length=static_cast<std::uint32_t>(bytes.size());const auto old=std::move(item->children);item->children={p};for(const auto child:old)release(child);hold(p);put(arg(1),p,bits);}
                }
            }else if(name.starts_with("T_")){
                require(item->kind==mf::MediaType,no_interface);
                if(name=="T_GetMajorType"){check(arg(1),16);memory.write(GuestAddress{arg(1)},get(*item,mf::MF_MT_MAJOR_TYPE,72).id);}
                else if(name=="T_IsCompressedFormat"){const auto found=item->attributes.find(mf::MF_MT_COMPRESSED);put(arg(1),found!=item->attributes.end() && found->second.scalar!=0);}
                else if(name=="T_IsEqual"){auto theirs=object(arg(1),mf::MediaType);const auto equal=[&](const Guid& key){const auto a=item->attributes.find(key),b=theirs->attributes.find(key);return a!=item->attributes.end() && b!=theirs->attributes.end() && a->second==b->second;};std::uint32_t flags=0;if(equal(mf::MF_MT_MAJOR_TYPE))flags|=1;if(equal(mf::MF_MT_SUBTYPE))flags|=2;auto a=item->attributes,b=theirs->attributes;a.erase(mf::MF_MT_MAJOR_TYPE);a.erase(mf::MF_MT_SUBTYPE);b.erase(mf::MF_MT_MAJOR_TYPE);b.erase(mf::MF_MT_SUBTYPE);if(a==b)flags|=4;if(item->attributes==theirs->attributes)flags|=8;put(arg(2),flags);result=flags==15?0:1;}
                else if(name=="T_GetRepresentation" || name=="T_GetVideoRepresentation"){
                    // AM_MEDIA_TYPE_REPRESENTATION 與 FORMAT_* 都回傳 AM_MEDIA_TYPE（格式區塊依要求的型別，見 mf_format_helpers.inc）；
                    // IMFVideoMediaType::GetVideoRepresentation 的 lStride 非零時取代預設步幅（負值為由下而上）。
                    out(arg(2));const auto requested=guid(arg(1));
                    require(requested==fmt_am_representation || requested==fmt_wave || requested==fmt_video_info || requested==fmt_video_info2 || requested==fmt_mpeg1 ||
                        requested==fmt_mpeg2 || requested==fmt_mf_video,unsupported_representation);
                    Object view;view.kind=mf::MediaType;view.attributes=item->attributes;
                    if(name=="T_GetVideoRepresentation"){require_video(view);if(const auto stride=static_cast<std::int32_t>(d(3)))view.attributes.insert_or_assign(mf::MF_MT_DEFAULT_STRIDE,value32(static_cast<std::uint32_t>(stride)));}
                    const auto p=allocate(am_size);gui_.task_allocations.insert(p);
                    try{write_am(p,view,requested);}catch(...){gui_.task_allocations.erase(p);(void)free_heap(memory,p,state);throw;}
                    put(arg(2),p,bits);
                }else if(name=="T_FreeRepresentation")free_am(arg(2));
                else if(name=="T_GetAudioFormat" && item->attributes.contains(mf::MF_MT_MAJOR_TYPE) && item->attributes.at(mf::MF_MT_MAJOR_TYPE).id==mf::MFMediaType_Video){
                    // IMFVideoMediaType::GetVideoFormat：回傳由媒體類型持有的 MFVIDEOFORMAT 指標，隨媒體類型釋放；每次呼叫依目前屬性重新產生，失敗時回 NULL。
                    result=0;
                    try{const auto bytes=mf_video_bytes(*item);if(item->allocation){(void)free_heap(memory,item->allocation,state);item->allocation=0;}
                        item->allocation=allocate(bytes.size());memory.write(GuestAddress{item->allocation},bytes);result=item->allocation;}catch(const Failure&){}
                }
                else if(name=="T_GetAudioFormat"){
                    // IMFAudioMediaType::GetAudioFormat：回傳由媒體類型持有的 WAVEFORMATEX 指標（不是 HRESULT）；不是音訊類型或缺少必要屬性時回 NULL。
                    // 格式標籤取自子類型 GUID 的前 32 位元（{XXXXXXXX-0000-0010-8000-00AA00389B71} 家族），編碼器私有資料（MF_MT_USER_DATA）接在後面當 cbSize 部分。
                    // 區塊隨媒體類型釋放；屬性可能已變更，每次呼叫重新產生。
                    result=0;
                    const auto scalar=[&](const Guid& key)->std::uint64_t {const auto found=item->attributes.find(key);return found==item->attributes.end()?0:found->second.scalar;};
                    const auto major=item->attributes.find(mf::MF_MT_MAJOR_TYPE),subtype=item->attributes.find(mf::MF_MT_SUBTYPE);
                    if(major!=item->attributes.end() && major->second.id==mf::MFMediaType_Audio && subtype!=item->attributes.end()){
                        std::array<std::uint8_t,16> id{},base{};std::memcpy(id.data(),&subtype->second.id,16);std::memcpy(base.data(),&mf::pcm,16);
                        std::uint32_t tag=0;std::memcpy(&tag,id.data(),4);
                        const auto user=item->attributes.find(mf::MF_MT_USER_DATA);
                        const std::vector<std::uint8_t> empty;const auto& extra=user!=item->attributes.end() && user->second.type==0x1011?user->second.blob:empty;
                        if(std::equal(id.begin()+4,id.end(),base.begin()+4) && tag<=UINT16_MAX && extra.size()<=UINT16_MAX){
                            const auto channels=scalar(mf::MF_MT_AUDIO_NUM_CHANNELS),rate=scalar(mf::MF_MT_AUDIO_SAMPLES_PER_SECOND),align=scalar(mf::MF_MT_AUDIO_BLOCK_ALIGNMENT),
                                depth=scalar(mf::MF_MT_AUDIO_BITS_PER_SAMPLE),average=scalar(mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND);
                            std::vector<std::uint8_t> bytes(18+extra.size());
                            const auto word=[&](std::size_t at,std::uint64_t value,std::size_t size){for(std::size_t i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                            word(0,tag,2);word(2,channels,2);word(4,rate,4);word(8,average?average:rate*align,4);word(12,align,2);word(14,depth,2);word(16,extra.size(),2);
                            std::copy(extra.begin(),extra.end(),bytes.begin()+18);
                            if(item->allocation){(void)free_heap(memory,item->allocation,state);item->allocation=0;}
                            item->allocation=allocate(bytes.size());memory.write(GuestAddress{item->allocation},bytes);result=item->allocation;
                        }
                    }
                }
                else throw Failure{invalid};
            }else if(name.starts_with("R_")){
                require(item->kind==mf::Async,no_interface);
                if(name=="R_GetStatus")result=item->status;else if(name=="R_SetStatus")item->status=d(1);else if(name=="R_GetStateNoAddRef")result=item->state;
                else {out(arg(1));const auto p=name=="R_GetState"?item->state:item->result_object;require(p!=0,pointer_error);hold(p);put(arg(1),p,bits);}
            }else if(name.starts_with("E_")){
                require(item->kind==mf::Event,no_interface);if(name=="E_GetType")put(arg(1),item->identifier);else if(name=="E_GetStatus")put(arg(1),item->status);else if(name=="E_GetExtendedType"){check(arg(1),16);memory.write(GuestAddress{arg(1)},item->extended);}else if(name=="E_GetValue")write_value(arg(1),item->value);
            }else if(name.starts_with("Q_")){
                require(item->kind==mf::Queue || item->kind==mf::Transform || item->kind==mf::Source || item->kind==mf::MediaStream || item->kind==mf::Session,no_interface);require(!item->closed,shutdown);
                if(name=="Q_GetEvent"){
                    out(arg(2));require(d(1)<=1);require(!item->callback,0xc00d36daU);
                    if(item->events.empty()){
                        require(d(1)==0,no_events);auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::MediaFoundation;thread.wait.handles={arg(0)};schedule(state,memory);return std::nullopt;
                    }
                    const auto p=item->events.front();item->events.pop_front();put(arg(2),p,bits);
                }
                else if(name=="Q_BeginGetEvent"){require(arg(1)!=0);require(!item->callback,0xc00d36daU);hold(arg(1));hold(arg(2));item->callback=arg(1);item->state=arg(2);if(item->kind==mf::Session)session_sync_job(item);else deliver(arg(0),item);}
                else if(name=="Q_EndGetEvent"){out(arg(2));auto async=object(arg(1),mf::Async);require(bridge.objects.contains(async->queue_owner) && entry(async->queue_owner).object==item && async->result_object);const auto p=async->result_object;async->result_object=0;put(arg(2),p,bits);}
                else if(name=="Q_QueueEvent"){(void)object(arg(1),mf::Event);hold(arg(1));item->events.push_back(arg(1));deliver(arg(0),item);}
                else if(name=="Q_QueueEventParamVar" || name=="Q_QueueEventParamUnk"){Value value;if(name=="Q_QueueEventParamVar")value=arg(4)?read_value(arg(4)):Value{};else {value.type=13;value.scalar=arg(4);}item->events.push_back(event(d(1),guid(arg(2)),d(3),std::move(value)));if(item->kind==mf::Session)session_sync_job(item);else deliver(arg(0),item);}
                else if(name=="Q_Shutdown"){require(item->kind==mf::Queue,no_interface);item->closed=true;for(const auto p:item->events)release(p);item->events.clear();release(item->state);release(item->callback);item->state=item->callback=0;}
            }else if(name.starts_with("G_")){
                require(item->kind==mf::Manager,no_interface);
                if(name=="G_ResetDevice"){
                    require(d(2)==item->reset_token);require(media_graphics_device(arg(1)),no_interface);hold(arg(1));release(item->parent);item->parent=arg(1);++item->generation;item->lock_handle=item->lock_owner=0;
                }else if(name=="G_OpenDeviceHandle"){
                    out(arg(1));require(item->parent!=0,0x80041000U);require(item->device_handles.size()<65536,0x8007000eU);const auto handle=bridge.next_device_handle++;item->device_handles.emplace(handle,item->generation);put(arg(1),handle,bits);
                }else {
                    const auto handle=arg(1);const auto found=item->device_handles.find(handle);require(found!=item->device_handles.end(),0x80070006U);
                    if(name=="G_CloseDeviceHandle"){if(item->lock_handle==handle)item->lock_handle=item->lock_owner=0;item->device_handles.erase(found);}
                    else {require(item->parent!=0,0x80041000U);require(found->second==item->generation,0x80041001U);
                        if(name=="G_UnlockDevice"){require(item->lock_handle==handle,invalid);item->lock_handle=item->lock_owner=0;}
                        else if(name=="G_TestDevice"){}
                        else {
                            out(arg(3));const bool locking=name=="G_LockDevice";
                            if(locking && item->lock_handle){require(d(4)!=0,0x80041002U);auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::MediaFoundation;thread.wait.handles={arg(0)};schedule(state,memory);return std::nullopt;}
                            if(locking){item->lock_handle=handle;item->lock_owner=threads_.current;}
                            const auto token=bridge.next_query++;bridge.queries.emplace(token,MediaFoundationState::Query{item,handle,item->generation,arg(3),threads_.current,locking});
                            try{start_callbacks(state,memory,{{foreign_method(item->parent,0),{item->parent,arg(2),arg(3)}}},Continuation::MediaFoundationQuery,token);}catch(...){discard_media_foundation_query(token,memory,state);throw;}return std::nullopt;
                        }
                    }
                }
            }else if(name.starts_with("X_")){
                require(item->surface.has_value(),no_interface);
                if(name=="X_GetSubresourceIndex")put(arg(1),item->identifier);
                else if(name=="X_GetResource"){out(arg(2));start_callbacks(state,memory,{{foreign_method(item->parent,0),{item->parent,arg(1),arg(2)}}},Continuation::ReturnCallback);return std::nullopt;}
                else if(name=="X_SetUnknown"){const auto key=guid(arg(1));if(!arg(2)){const auto found=item->attributes.find(key);if(found!=item->attributes.end()){release(found->second.scalar);item->attributes.erase(found);}}else {require(!item->attributes.contains(key),0x80071392U);Value value;value.type=13;value.scalar=arg(2);set(*item,key,value);}}
                else if(name=="X_GetUnknown"){out(arg(3));const auto found=item->attributes.find(guid(arg(1)));require(found!=item->attributes.end(),0xc00d36d5U);const auto p=found->second.scalar;if(bridge.objects.contains(p))query(p,guid(arg(2)),arg(3));else {start_callbacks(state,memory,{{foreign_method(p,0),{p,arg(2),arg(3)}}},Continuation::ReturnCallback);return std::nullopt;}}
            }else if(name.starts_with("HB_")){
                // 交給客體 handler 的 IMFAsyncCallback：同步解析時保存結果並設定事件；非同步解析時在回呼中呼叫 EndCreateObject，再把結果送回解析器的完成佇列。
                require(item->kind==mf::HostCallback,no_interface);
                if(name=="HB_GetParameters")throw Failure{0x80004001U};
                if(!(item->identifier&1U)){
                    hold(arg(1));release(item->callback);item->callback=arg(1);(void)nested_service(state,memory,"kernel32.dll","SetEvent",{item->generation});
                }else {
                    if(service_callback::completed(this,threads_.current,service.symbol,state))return nested_continuation(state,memory,service.symbol,[](GuestState&,GuestMemory&)->service_callback::Task<std::optional<std::uint32_t>>{throw HostCallFault("服務續行重播不應重新建立");});
                    const auto handler=item->result_object,completion=item->state,host=item->pointers.at(mf::HostCallback);require(handler && completion,0x8000ffffU);hold(host);
                    return nested_continuation(state,memory,service.symbol,[this,service,handler,completion,host](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall call(s,m,service);const std::uint8_t width=x86_?32:64;const unsigned size_of_pointer=width/8U;
                        const auto method=[&](std::uint64_t object,unsigned slot){const auto table=m.load(GuestAddress{object},width);return m.load(GuestAddress{table+slot*size_of_pointer},width);};
                        const auto block=allocate_heap(m,16);std::uint32_t hr=0x8007000eU;std::uint64_t created=0;std::uint32_t type=2;
                        if(block){m.store(GuestAddress{block},32,2);m.store(GuestAddress{block+8},width,0);
                            hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(handler,4),{handler,call(1),block,block+8}));
                            if(!hr){type=static_cast<std::uint32_t>(m.load(GuestAddress{block},32));created=m.load(GuestAddress{block+8},width);}(void)free_heap(m,block,s);}
                        (void)co_await service_callback::call_value(resolve("internal-mf","ResolverDeliver").value,{completion,hr?0:created,hr?2:type,hr});
                        (void)co_await service_callback::call_value(resolve("internal-mf","Release").value,{host});
                        call.return_to(s,0);co_return std::nullopt;
                    });
                }
            }else if(name.starts_with("DS_") || name.starts_with("DO_")){
                // D3D12 同步物件：就緒＝生產端佇列 Signal(就緒 fence, ++n)，消費端 Wait(就緒 fence, n)；釋放＝消費端 Signal(釋放 fence, ++m)；
                // 事件以 fence 的 SetEventOnCompletion 通知。Reset 不改 fence（數值持續遞增）。客體 D3D12 介面以續行呼叫；續行前會改計數，重播時直接取回結果。
                require(item->kind==mf::D3D12Sync,no_interface);
                if(service_callback::completed(this,threads_.current,service.symbol,state))return nested_continuation(state,memory,service.symbol,[](GuestState&,GuestMemory&)->service_callback::Task<std::optional<std::uint32_t>>{throw HostCallFault("服務續行重播不應重新建立");});
                const auto sync=item->d3d12;require(sync && sync->ready && sync->release,not_initialized);
                if(name!="DO_Reset"){
                require(arg(1)!=0,name.find("Event")!=std::string_view::npos?invalid:pointer_error);
                std::uint64_t target=0,value=0;unsigned slot=0;std::uint64_t fence=0;
                if(name=="DS_EnqueueResourceReady"){target=arg(1);slot=14;fence=sync->ready;value=++sync->ready_value;}
                else if(name=="DS_EnqueueResourceReadyWait"){target=arg(1);slot=15;fence=sync->ready;value=sync->ready_value;}
                else if(name=="DS_EnqueueResourceRelease"){target=arg(1);slot=14;fence=sync->release;value=++sync->release_value;}
                else if(name=="DS_SignalEventOnResourceReady"){target=sync->ready;slot=9;value=sync->ready_value;}
                else {target=sync->release;slot=9;value=sync->release_value;}
                std::vector<std::uint64_t> args{target};if(fence)args.push_back(fence);
                if(x86_){args.push_back(value&UINT32_MAX);args.push_back(value>>32);}else args.push_back(value);
                if(!fence)args.push_back(arg(1));
                return nested_continuation(state,memory,service.symbol,[this,service,target,slot,args](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall call(s,m,service);const std::uint8_t width=x86_?32:64;const unsigned size_of_pointer=width/8U;
                    const auto table=m.load(GuestAddress{target},width);const auto entry_point=m.load(GuestAddress{table+slot*size_of_pointer},width);
                    call.return_to(s,static_cast<std::uint32_t>(co_await service_callback::call_value(entry_point,args)));co_return std::nullopt;
                });
                }
            }else if(name.starts_with("TK_")){
                // IMFTrackedSample::SetAllocator：已有擁有者時回報 MF_E_NOTACCEPTING。
                require(item->kind==mf::Sample && item->tracked,no_interface);require(arg(1)!=0,pointer_error);require(!item->tracker,0xc00d36b5U);
                (void)foreign_method(arg(1),4);hold(arg(1));hold(arg(2));item->tracker=arg(1);item->tracker_state=arg(2);
            }else if(name.starts_with("SA_") || name.starts_with("SC_")){
                // IMFVideoSampleAllocatorEx：每個樣本一個緩衝；有 DXGI 裝置管理員時為 D3D11 材質（DXGI surface 緩衝），否則為記憶體 2D 緩衝。
                require(item->kind==mf::SampleAllocator,no_interface);if(!item->pool)item->pool=std::make_shared<SamplePool>();auto& pool=*item->pool;
                const auto allocator=item->pointers.at(mf::SampleAllocator);
                const auto uninitialize=[&]{
                    for(const auto sample:pool.owned)if(const auto found=bridge.objects.find(sample);found!=bridge.objects.end())found->second.object->pool_owner=0;
                    auto idle=std::move(pool.free);pool.free.clear();pool.owned.clear();
                    for(const auto sample:idle)if(const auto found=bridge.objects.find(sample);found!=bridge.objects.end()){found->second.object->refs=1;release(sample);}
                    release(pool.type);pool.type=0;pool.initialized=false;};
                const auto new_sample=[&]{
                    std::uint64_t buffer=0;
                    if(pool.manager){
                        const auto device=object(pool.manager,mf::Manager)->parent;require(device!=0,0xc00d6d76U);
                        GuestScratch scratch(*this,memory,state);const auto desc=scratch.allocate(44),slot=scratch.allocate(8);require(desc && slot,0x8007000eU);
                        const std::array<std::uint32_t,11> words{pool.width,pool.height,1,1,pool.format,1,0,pool.usage,pool.bind,0,pool.misc};
                        for(unsigned i=0;i<11;++i)memory.store(GuestAddress{desc+4*i},32,words[i]);
                        const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"internal-d3d11","CreateTexture2D",{device,desc,0,slot}));require(!status,status);
                        const auto texture=memory.load(GuestAddress{slot},bits);auto surface=media_graphics_surface(texture,0);
                        if(!surface){release(texture);throw Failure{0xc00d6d76U};}
                        buffer=make(mf::Buffer);auto target=object(buffer,mf::Buffer);target->surface=std::move(surface);target->parent=texture;target->maximum=target->length=target->surface->size;
                    }else buffer=buffer_2d(pool.width,pool.height,pool.code,pool.bottom_up);
                    std::uint64_t sample=0;try{sample=make(mf::Sample);}catch(...){release(buffer);throw;}
                    auto created=object(sample,mf::Sample);created->children.push_back(buffer);created->pool_owner=allocator;pool.owned.insert(sample);return sample;};
                if(name=="SA_SetDirectXManager"){
                    if(arg(1)){require(bridge.objects.contains(arg(1)) && entry(arg(1)).object->kind==mf::Manager,0xc00d6d76U);}
                    if(arg(1)!=pool.manager){uninitialize();hold(arg(1));release(pool.manager);pool.manager=arg(1);}
                }else if(name=="SA_UninitializeSampleAllocator")uninitialize();
                else if(name=="SA_InitializeSampleAllocator" || name=="SA_InitializeSampleAllocatorEx"){
                    // InitializeSampleAllocator(cRequestedFrames, pMediaType)＝Ex(cRequestedFrames, cRequestedFrames, NULL, pMediaType)。
                    const bool extended=name.ends_with("Ex");const auto initial=d(1),maximum=extended?d(2):d(1);const auto attributes=extended?arg(3):0,type_pointer=arg(extended?4:2);
                    require(maximum && initial<=maximum && maximum<=4096);auto type=object(type_pointer,mf::MediaType);require_video(*type);
                    const auto frame=attribute_scalar(*type,mf::MF_MT_FRAME_SIZE);const auto code=media_code(subtype_of(*type));require(frame>>32 && (frame&UINT32_MAX) && code,invalid_media_type);
                    std::uint32_t format=0;for(const auto& [dx9,dxgi_format]:dxgi)if(dx9==code){format=dxgi_format;break;}
                    std::uint32_t bind=8,usage=0,misc=0;
                    if(attributes){auto store=entry(attributes).object;require(mf::attributes(store->kind),no_interface);
                        const Guid bind_key=mf::guid(0xeacf97ad,0x065c,0x4408,{0xbe,0xe3,0xfd,0xcb,0xfd,0x12,0x8b,0xe2}),usage_key=mf::guid(0xe85fe442,0x2ca3,0x486e,{0xa9,0xc7,0x10,0x9d,0xda,0x60,0x98,0x80}),
                            shared_key=mf::guid(0x7b8f32c3,0x6d96,0x4b89,{0x92,0x03,0xdd,0x38,0xb6,0x14,0x14,0xf3});
                        bind=static_cast<std::uint32_t>(attribute_scalar(*store,bind_key,bind));usage=static_cast<std::uint32_t>(attribute_scalar(*store,usage_key,usage));if(attribute_scalar(*store,shared_key))misc|=2;}
                    if(pool.manager)require(format!=0,0xc00d6d76U);
                    uninitialize();pool.width=static_cast<std::uint32_t>(frame>>32);pool.height=static_cast<std::uint32_t>(frame);pool.code=code;pool.format=format;pool.bind=bind;pool.usage=usage;pool.misc=misc;
                    pool.bottom_up=static_cast<std::int32_t>(attribute_scalar(*type,mf::MF_MT_DEFAULT_STRIDE,1))<0;pool.maximum=maximum;hold(type_pointer);pool.type=type_pointer;pool.initialized=true;
                    try{for(std::uint32_t i=0;i<initial;++i){const auto sample=new_sample();object(sample,mf::Sample)->refs=0;pool.free.push_back(sample);}}catch(...){uninitialize();throw;}
                }else if(name=="SA_AllocateSample"){
                    out(arg(1));require(pool.initialized,not_initialized);std::uint64_t sample=0;
                    if(!pool.free.empty()){sample=pool.free.back();pool.free.pop_back();object(sample,mf::Sample)->refs=1;}
                    else {require(pool.owned.size()<pool.maximum,0xc00d4a3eU);sample=new_sample();}
                    put(arg(1),sample,bits);
                }else if(name=="SC_SetCallback"){if(arg(1))(void)foreign_method(arg(1),3);hold(arg(1));release(pool.notify);pool.notify=arg(1);}
                else if(name=="SC_GetFreeSampleCount"){require(arg(1)!=0,pointer_error);put(arg(1),static_cast<std::uint32_t>(pool.free.size()));}
            }else if(name.starts_with("PC_")){
                // IMFPluginControl：找不到偏好或未停用時回報 HRESULT_FROM_WIN32(ERROR_NOT_FOUND)，索引超出範圍為 MF_E_OUT_OF_RANGE。
                require(item->kind==mf::PluginControl,no_interface);auto& plugins=bridge.plugins;const auto type=d(1);
                const auto folded=[&](std::uint64_t p){require(p!=0,pointer_error);auto text=read_utf16(memory,GuestAddress{p},1024);for(auto& c:text)c=unicode_case_unit(c,false);return text;};
                if(name=="PC_GetPreferredClsid"){
                    require(arg(3)!=0,pointer_error);const auto found=plugins.preferred.find({type,folded(arg(2))});require(found!=plugins.preferred.end(),0x80070490U);memory.write(GuestAddress{arg(3)},found->second.second);
                }else if(name=="PC_GetPreferredClsidByIndex"){
                    require(arg(3)!=0 && arg(4)!=0,pointer_error);std::uint32_t index=0;bool located=false;
                    for(const auto& [key,value]:plugins.preferred)if(key.first==type && index++==d(2)){
                        const auto bytes=utf16_bytes(value.first);const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);gui_.task_allocations.insert(p);
                        put(arg(3),p,bits);memory.write(GuestAddress{arg(4)},value.second);located=true;break;}
                    require(located,0xc00d3702U);
                }else if(name=="PC_SetPreferredClsid"){
                    const auto key=std::pair{type,folded(arg(2))};
                    if(arg(3)){require(plugins.preferred.size()<65536 || plugins.preferred.contains(key),0x8007000eU);plugins.preferred.insert_or_assign(key,std::pair{read_utf16(memory,GuestAddress{arg(2)},1024),guid(arg(3))});}
                    else plugins.preferred.erase(key);
                }else if(name=="PC_IsDisabled"){
                    require(std::find(plugins.disabled.begin(),plugins.disabled.end(),std::pair{type,guid(arg(2))})!=plugins.disabled.end(),0x80070490U);
                }else if(name=="PC_GetDisabledByIndex"){
                    require(arg(3)!=0,pointer_error);std::uint32_t index=0;bool located=false;
                    for(const auto& [kind,id]:plugins.disabled)if(kind==type && index++==d(2)){memory.write(GuestAddress{arg(3)},id);located=true;break;}
                    require(located,0xc00d3702U);
                }else if(name=="PC_SetDisabled"){
                    const auto key=std::pair{type,guid(arg(2))};const auto found=std::find(plugins.disabled.begin(),plugins.disabled.end(),key);
                    if(d(3)){if(found==plugins.disabled.end()){require(plugins.disabled.size()<65536,0x8007000eU);plugins.disabled.push_back(key);}}
                    else if(found!=plugins.disabled.end())plugins.disabled.erase(found);
                }else if(name=="PC_SetPolicy"){require(d(1)<=3);plugins.policy=d(1);}
            }else if(name.starts_with("TS_") || name.starts_with("SS_")){
                // 系統時間來源：時鐘時間＝基準時鐘＋（系統時間−基準系統時間）×速率；暫停時凍結，停止與無效為 0。
                require(item->kind==mf::TimeSource,no_interface);auto& timing=item->timing;const auto now=static_cast<std::int64_t>(runtime_.clock_ticks);
                const auto clock_at=[&](std::int64_t system)->std::int64_t{
                    if(timing.state!=1)return timing.state==3?timing.clock:0;return timing.clock+static_cast<std::int64_t>(static_cast<double>(system-timing.system)*timing.rate);};
                const auto system=static_cast<std::int64_t>(arg(1));
                if(name=="TS_GetClockCharacteristics"){require(arg(1)!=0,pointer_error);put(arg(1),0xaU);}
                else if(name=="TS_GetCorrelatedTime"){require(!d(1));require(arg(2)!=0 && arg(3)!=0,pointer_error);put(arg(2),static_cast<std::uint64_t>(clock_at(now)),64);put(arg(3),static_cast<std::uint64_t>(now),64);}
                else if(name=="TS_GetContinuityKey"){require(arg(1)!=0,pointer_error);put(arg(1),0);}
                else if(name=="TS_GetState"){require(arg(2)!=0,pointer_error);put(arg(2),timing.state);}
                else if(name=="TS_GetProperties"){
                    // MFCLOCK_PROPERTIES：相關速率 0、時鐘 GUID_NULL、MFCLOCK_RELATIVE_FLAG_JITTER_NEVER_AHEAD、10 MHz、容差未知（50000）、抖動 1。
                    check(arg(1),48);std::vector<std::uint8_t> bytes(48);fmt_put(bytes,24,1,4);fmt_put(bytes,32,10000000,8);fmt_put(bytes,40,50000,4);fmt_put(bytes,44,1,4);memory.write(GuestAddress{arg(1)},bytes);
                }else if(name=="TS_GetUnderlyingClock"){out(arg(1));const auto p=item->pointers.at(mf::TimeSource);hold(p);put(arg(1),p,bits);}
                else if(name=="SS_OnClockStart"){
                    const auto offset=static_cast<std::int64_t>(arg(2));timing.clock=offset==INT64_MAX?clock_at(system):offset;timing.system=system;timing.state=1;
                }else if(name=="SS_OnClockStop"){timing.clock=0;timing.system=system;timing.state=2;}
                else if(name=="SS_OnClockPause"){timing.clock=clock_at(system);timing.system=system;timing.state=3;}
                else if(name=="SS_OnClockRestart"){require(timing.state==3,0xc00d36b2U);timing.system=system;timing.state=1;}
                else if(name=="SS_OnClockSetRate"){const auto rate=float_argument(2);require(rate>=0.0f,0xc00d36d0U);timing.clock=clock_at(system);timing.system=system;timing.rate=rate;}
            }else if(name.starts_with("K_")){
                // IMediaBuffer：長度與指標自 cbOffset 起算；GetBufferAndLength 以線性配置對應 MF 緩衝（surface 緩衝先讀回）。
                require(item->kind==mf::LegacyBuffer,no_interface);auto buffer=object(item->parent,mf::Buffer);const auto offset=item->identifier;
                if(name=="K_SetLength"){require(d(1)<=buffer->maximum-offset);buffer->length=offset+d(1);}
                else if(name=="K_GetMaxLength"){require(arg(1)!=0,pointer_error);put(arg(1),buffer->maximum-offset);}
                else {require(arg(1) || arg(2),pointer_error);map_buffer(*buffer,true,false);if(arg(1))put(arg(1),buffer->data+offset,bits);
                    if(arg(2))put(arg(2),buffer->length>offset?buffer->length-offset:0U);}
            }else if(name.starts_with("Y_")){
                require(item->surface.has_value(),no_interface);
                if(name=="Y_Lock2D" || name=="Y_Lock2DSize"){
                    const bool sized=name=="Y_Lock2DSize";const auto flags=sized?d(1):3U;require(flags>=1 && flags<=3);const auto scan=arg(sized?2:1),pitch=arg(sized?3:2);check(scan,unit);check(pitch,4);if(sized){check(arg(4),unit);check(arg(5),4);}require(!item->locks && !item->locks_2d,0xc00d36b2U);map_buffer(*item,false,flags==2);item->locks_2d=1;item->lock_flags=flags;put(scan,item->data,bits);put(pitch,item->surface->row_pitch);if(sized){put(arg(4),item->data,bits);put(arg(5),item->maximum);}
                }else if(name=="Y_Unlock2D"){require(item->locks_2d!=0,0xc00d36b2U);item->locks_2d=0;if(item->lock_flags&2)flush_buffer(*item,false);}
                else if(name=="Y_GetScanline0AndPitch"){require(item->locks_2d!=0,0xc00d36b2U);put(arg(1),item->data,bits);put(arg(2),item->surface->row_pitch);}
                else if(name=="Y_IsContiguousFormat")put(arg(1),1);
                else if(name=="Y_GetContiguousLength")put(arg(1),item->maximum);
                else if(name=="Y_ContiguousCopyTo"){require(d(2)>=item->maximum,small);check(arg(1),item->maximum);map_buffer(*item,true,false);memory.write(GuestAddress{arg(1)},data(item->data,item->maximum));}
                else if(name=="Y_ContiguousCopyFrom"){require(d(2)>=item->maximum,small);const auto bytes=data(arg(1),item->maximum);map_buffer(*item,true,true);memory.write(GuestAddress{item->data},bytes);flush_buffer(*item,true);}
                else if(name=="Y_Copy2DTo"){auto target=object(arg(1),mf::Buffer);require(target->surface.has_value() && target->maximum==item->maximum && target->surface->row_pitch==item->surface->row_pitch && target->surface->height==item->surface->height);map_buffer(*item,false,false);const auto bytes=data(item->data,item->maximum);map_buffer(*target,false,true);memory.write(GuestAddress{target->data},bytes);flush_buffer(*target,false);}
            }else if(name.starts_with("D_") || name.starts_with("H_")){
                require(item->kind==mf::Stream,no_interface);
                if(name=="D_GetStreamIdentifier")put(arg(1),item->identifier);else if(name=="D_GetMediaTypeHandler"){out(arg(1));const auto p=create(item,mf::TypeHandler);hold(p);put(arg(1),p,bits);}
                else if(name=="H_GetMediaTypeCount")put(arg(1),item->children.size());else if(name=="H_GetMediaTypeByIndex"){out(arg(2));require(d(1)<item->children.size(),0xc00d36b9U);hold(item->children[d(1)]);put(arg(2),item->children[d(1)],bits);}
                else if(name=="H_GetCurrentMediaType"){out(arg(1));require(item->current_type!=0,0xc00d6d60U);hold(item->current_type);put(arg(1),item->current_type,bits);}
                else if(name=="H_GetMajorType"){check(arg(1),16);memory.write(GuestAddress{arg(1)},get(*object(item->current_type,mf::MediaType),mf::MF_MT_MAJOR_TYPE,72).id);}
                else if(name=="H_SetCurrentMediaType" || name=="H_IsMediaTypeSupported"){
                    auto type=object(arg(1),mf::MediaType);const auto supported=std::any_of(item->children.begin(),item->children.end(),[&](const auto p){return object(p,mf::MediaType)->attributes==type->attributes;});if(name=="H_IsMediaTypeSupported" && arg(2))out(arg(2));require(supported,0xc00d36b4U);if(name=="H_SetCurrentMediaType"){hold(arg(1));release(item->current_type);item->current_type=arg(1);}
                }
            }else if(name.starts_with("P_")){
                require(item->kind==mf::Presentation,no_interface);
                if(name=="P_GetStreamDescriptorCount")put(arg(1),item->children.size());else if(name=="P_GetStreamDescriptorByIndex"){out(arg(3));check(arg(2),4);require(d(1)<item->children.size(),invalid_index);hold(item->children[d(1)]);put(arg(2),item->selected[d(1)]);put(arg(3),item->children[d(1)],bits);}
                else if(name=="P_SelectStream" || name=="P_DeselectStream"){require(d(1)<item->children.size(),invalid_index);item->selected[d(1)]=name=="P_SelectStream";}
                else if(name=="P_Clone"){out(arg(1));const auto p=make(mf::Presentation);auto clone=object(p,mf::Presentation);clone->children=item->children;clone->selected=item->selected;for(const auto child:clone->children)hold(child);for(const auto& [key,value]:item->attributes)set(*clone,key,value);put(arg(1),p,bits);}
            }else throw Failure{no_interface};
        }
    }catch(const Failure& error){result=error.code;}catch(const MemoryFault&){result=pointer_error;}catch(const std::bad_alloc&){result=0x8007000eU;}catch(const std::invalid_argument&){result=invalid;}catch(const std::logic_error&){result=0xc00d36b2U;}catch(const std::runtime_error&){result=0x80004005U;}
    // 診斷：X64ARM_TRACE_MEDIA_ERRORS=1 時列出回傳失敗 HRESULT 的 Media Foundation 匯出與方法（不改變行為）。
    static const bool trace_failures=[] {const auto* value=std::getenv("X64ARM_TRACE_MEDIA_ERRORS");return value && std::string_view(value)=="1";}();
    if(trace_failures && (result&0x80000000U) && name!="MFGetSystemTime" && name!="T_GetAudioFormat")
        std::fprintf(stderr,"[media-foundation] %.*s 失敗 hr=0x%08x\n",static_cast<int>(name.size()),name.data(),static_cast<unsigned>(result));
    if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;}
    arg.return_to(state,result,name=="MFGetSystemTime" || name=="T_GetAudioFormat" || name=="MFllMulDiv");return std::nullopt;
}
}
