#include "x64arm/windows_services.hpp"
#include "x64arm/audio_graph.hpp"
#include "x64arm/audio_channels.hpp"
#include "x64arm/audio_adpcm.hpp"
#include "x64arm/audio_frame_effects.hpp"
#include "x64arm/unicode.hpp"
#include "builtin_com_registry.hpp"
#include "xaudio_interfaces.hpp"
#include "audio_endpoints.hpp"
#include "pcm_wave_format.hpp"
#include "guest_xaudio.hpp"
#include "guest_xaudio_code.hpp"
#include "xaudio_layout.h"
#include "x64arm/pe_loader.hpp"
#include "x64arm/shared_wake.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <set>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace x64arm {
void xaudio_lowering::initialize(GuestMemory& memory,std::map<std::uint64_t,RuntimeImage>& images,bool x86,std::uint64_t gateway){
    const auto bytes=x86?std::span<const std::uint8_t>{image_x86}:std::span<const std::uint8_t>{image_x64};
    const auto base=gateway+image_offset;auto parsed=parse_pe_image(bytes,GuestAddress{base},true,memory.limit());
    if(!parsed.imports.empty() || !parsed.delay_imports.empty() || parsed.metadata.image_size>image_capacity)throw std::logic_error("XAudio2 常式有外部相依或超出映像範圍");
    for(const auto& [address,image]:images)if(base<address+image.size && address<base+parsed.metadata.image_size)throw std::logic_error("XAudio2 常式映像重疊");
    auto image=parsed.runtime;image.private_code=true;image.path=u"[guest-xaudio]";image.export_names.clear();image.export_ordinals.clear();
    for(const auto& [name,ordinal]:parsed.runtime.export_names)image.export_names.emplace(name,parsed.runtime.export_ordinals.at(ordinal));
    for(auto& mapping:parsed.mappings)mapping.permissions=(mapping.permissions&permission(Access::Execute))?read_execute:permission(Access::Read);
    map_pe_image(memory,parsed);images.emplace(base,std::move(image));
}
namespace {
using Graph=audio::VoiceGraph;
constexpr std::uint32_t invalid=0x88960001U,no_interface=0x80004002U,pointer_error=0x80004003U;
struct Method {std::string_view name;unsigned args;std::uint32_t narrow{};};
constexpr Method identity[]={{"QueryInterface",3},{"AddRef",1},{"Release",1}};
constexpr Method enumeration[]={{"GetDeviceCount",2},{"GetDeviceDetails",3,2},{"Initialize",3,6}};
constexpr Method engine_methods[]={{"RegisterForCallbacks",2},{"UnregisterForCallbacks",2},{"CreateSourceVoice",8,24},{"CreateSubmixVoice",8,60},{"CreateMasteringVoice27",7,60},{"StartEngine",1},{"StopEngine",1},{"CommitChanges",2,2},{"GetPerformanceData",2},{"SetDebugConfiguration",3}};
constexpr Method voice_methods[]={{"GetVoiceDetails",2},{"SetOutputVoices",2},{"SetEffectChain",2},{"EnableEffect",3,6},{"DisableEffect",3,6},{"GetEffectState",3,2},{"SetEffectParameters",5,26},{"GetEffectParameters",4,10},{"SetFilterParameters",3,4},{"GetFilterParameters",2},{"SetOutputFilterParameters",4,8},{"GetOutputFilterParameters",3},{"SetVolume",3,6},{"GetVolume",2},{"SetChannelVolumes",4,10},{"GetChannelVolumes",3,2},{"SetOutputMatrix",6,44},{"GetOutputMatrix",5,12},{"DestroyVoice",1}};
constexpr Method source_methods[]={{"Start",3,6},{"Stop",3,6},{"SubmitSourceBuffer",3},{"FlushSourceBuffers",1},{"Discontinuity",1},{"ExitLoop",2,2},{"GetState27",2},{"SetFrequencyRatio",3,6},{"GetFrequencyRatio",2},{"SetSourceSampleRate",2,2}};
constexpr Method factory_methods[]={{"CreateInstance",4},{"LockServer",2,2}};
// XAPOFX 效果物件：IXAPO 與 IXAPOParameters 各一張虛擬表（物件前兩個指標），方法順序依 xapo.h。
constexpr Method fx_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetRegistrationProperties",2},{"IsInputFormatSupported",4},{"IsOutputFormatSupported",4},{"Initialize",3,4},{"Reset",1},{"LockForProcess",5,10},{"UnlockForProcess",1},{"Process",6,42},{"CalcInputFrames",2,2},{"CalcOutputFrames",2,2}};
constexpr Method fx_parameter_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"SetParameters",3,4},{"GetParameters",3,4}};
constexpr std::uint32_t format_unsupported=0x88970001U,invalid_argument_error=0x80070057U;
// CLSID 有兩組：XAPOFX1_x（DirectX SDK，A90BC001-E897-E897-7439-4355000000NN）與 XAudio 2.8／2.9。兩組在所有版本都接受。
struct FxClass {xaudio::Guid legacy,current;audio::Effect::Type type;std::u16string_view name;unsigned maximum_channels,minimum_rate,maximum_rate;};
constexpr FxClass fx_classes[]={
    // 聲道與取樣率限制依 xapofx.h：全部只收 FLOAT32；FXEQ 取樣率 22000～48000 Hz；FXReverb 只有單聲道與立體聲。其餘沿用 XAPOBASE 的預設範圍。
    {{0x01,0xc0,0x0b,0xa9,0x97,0xe8,0x97,0xe8,0x74,0x39,0x43,0x55,0,0,0,0},{0x17,0x11,0xe0,0xf5,0xc4,0xd6,0x5a,0x48,0xa3,0xf5,0x69,0x51,0x96,0xf3,0xdb,0xfa},audio::Effect::Type::FxEq,u"FXEQ",64,22000,48000},
    {{0x01,0xc0,0x0b,0xa9,0x97,0xe8,0x97,0xe8,0x74,0x39,0x43,0x55,0,0,0,1},{0x16,0x79,0x13,0xc4,0xe1,0x2b,0xfd,0x46,0x85,0x99,0x44,0x15,0x36,0xf4,0x98,0x56},audio::Effect::Type::FxMasteringLimiter,u"FXMasteringLimiter",64,1000,200000},
    {{0x01,0xc0,0x0b,0xa9,0x97,0xe8,0x97,0xe8,0x74,0x39,0x43,0x55,0,0,0,2},{0x56,0xca,0x9a,0x7d,0x68,0xcb,0x07,0x48,0xb6,0x32,0xb1,0x37,0x35,0x2e,0x85,0x96},audio::Effect::Type::FxReverb,u"FXReverb",2,1000,200000},
    {{0x01,0xc0,0x0b,0xa9,0x97,0xe8,0x97,0xe8,0x74,0x39,0x43,0x55,0,0,0,3},{0x40,0xd7,0x39,0x50,0x36,0xf7,0x9a,0x44,0x84,0xd3,0xa5,0x62,0x02,0x55,0x7b,0x87},audio::Effect::Type::FxEcho,u"FXEcho",64,1000,200000}};
constexpr xaudio::Guid xapo27{0x01,0xc0,0x0b,0xa9,0x97,0xe8,0x97,0xe8,0x55,0xe4,0x9e,0x47,0,0,0,0},xapo_parameters27{0x01,0xc0,0x0b,0xa9,0x97,0xe8,0x97,0xe8,0x55,0xe4,0x9e,0x47,0,0,0,1};
constexpr xaudio::Guid xapo28{0x84,0xb9,0x10,0xa4,0x39,0x98,0x19,0x48,0xa0,0xbe,0x28,0x56,0xae,0x6b,0x3a,0xdb},xapo_parameters28{0x66,0x5c,0xd9,0x26,0xf2,0x80,0x9a,0x49,0xad,0x54,0x5a,0xe7,0xf0,0x1c,0x6d,0x98};
// XAudio2 內建效果（xaudio2fx.h）。2.7 經 CLSID 建立，2.8／2.9 由 CreateAudioReverb／CreateAudioVolumeMeter 建立。
enum class Builtin:unsigned {None,Reverb,Meter};
// XAUDIO2FX_REVERB_PARAMETERS（pack 1）：DirectX SDK 與 Windows 8 為 52 位元組；Windows 10（2.9）多 SideDelay 與 DisableLateField，共 57 位元組。
constexpr unsigned reverb_bytes=52,reverb_bytes_win10=57;
audio::ReverbParameters parse_reverb(std::span<const std::uint8_t> b){
    const bool w10=b.size()==reverb_bytes_win10;audio::ReverbParameters p;unsigned o=0;
    const auto f32=[&]{float v;std::memcpy(&v,b.data()+o,4);o+=4;return v;};
    const auto u32=[&]{std::uint32_t v;std::memcpy(&v,b.data()+o,4);o+=4;return v;};
    const auto u8=[&]{return b[o++];};
    p.wet_dry_mix=f32();p.reflections_delay=u32();p.reverb_delay=u8();p.rear_delay=u8();if(w10)p.side_delay=u8();
    p.position_left=u8();p.position_right=u8();p.position_matrix_left=u8();p.position_matrix_right=u8();
    p.early_diffusion=u8();p.late_diffusion=u8();p.low_eq_gain=u8();p.low_eq_cutoff=u8();p.high_eq_gain=u8();p.high_eq_cutoff=u8();
    p.room_filter_freq=f32();p.room_filter_main=f32();p.room_filter_hf=f32();p.reflections_gain=f32();p.reverb_gain=f32();
    p.decay_time=f32();p.density=f32();p.room_size=f32();if(w10)p.disable_late_field=u32()!=0;
    return p;
}
std::vector<std::uint8_t> serialize_reverb(const audio::ReverbParameters& p,unsigned size){
    const bool w10=size==reverb_bytes_win10;std::vector<std::uint8_t> b;b.reserve(size);
    const auto f32=[&](float v){std::uint8_t t[4];std::memcpy(t,&v,4);b.insert(b.end(),t,t+4);};
    const auto u32=[&](std::uint32_t v){std::uint8_t t[4];std::memcpy(t,&v,4);b.insert(b.end(),t,t+4);};
    f32(p.wet_dry_mix);u32(p.reflections_delay);b.push_back(p.reverb_delay);b.push_back(p.rear_delay);if(w10)b.push_back(p.side_delay);
    for(auto v:{p.position_left,p.position_right,p.position_matrix_left,p.position_matrix_right,p.early_diffusion,p.late_diffusion,p.low_eq_gain,p.low_eq_cutoff,p.high_eq_gain,p.high_eq_cutoff})b.push_back(v);
    for(auto v:{p.room_filter_freq,p.room_filter_main,p.room_filter_hf,p.reflections_gain,p.reverb_gain,p.decay_time,p.density,p.room_size})f32(v);
    if(w10)u32(p.disable_late_field?1:0);
    return b;
}
// 宿主 DSP 與目前鎖定的格式。直接呼叫 IXAPO::Process 與掛進語音效果鏈用的是同一組 audio::Effect。
struct Fx {
    const FxClass* info{};xaudio::Guid clsid{};
    Builtin builtin{};unsigned version{7};std::shared_ptr<audio::FrameEffect> dsp{}; // 內建效果：整幀 DSP，可改變聲道數
    std::vector<std::shared_ptr<audio::Effect>> pairs; // 每兩聲道一個；[0] 永遠存在並保存目前參數
    unsigned rate{48000},channels{2},maximum_frames{};bool locked{};
    std::uint64_t voice{}; // 非零：由這個語音的效果鏈鎖定，應用程式不能自行解鎖
    float maximum_delay{2000};
    Fx(const FxClass& value,const xaudio::Guid& id):info(&value),clsid(id){pairs.push_back(std::make_shared<audio::Effect>(value.type,rate));}
    Fx(Builtin kind,unsigned engine_version):builtin(kind),version(engine_version){
        const bool reverb=kind==Builtin::Reverb;
        clsid=engine_version<=7?xaudio::classic_guids[engine_version][reverb?2:3]:
            (reverb?xaudio::reverb28:xaudio::meter28);
        if(reverb)dsp=std::make_shared<audio::Reverb>(engine_version>=9);else dsp=std::make_shared<audio::VolumeMeter>();
        (void)dsp->configure(48000,reverb?2:1,reverb?2:1);channels=dsp->output_channels();
    }
    audio::Reverb& reverb()const {return static_cast<audio::Reverb&>(*dsp);}
    audio::VolumeMeter& meter()const {return static_cast<audio::VolumeMeter&>(*dsp);}
    // 內建效果的聲道與取樣率限制：reverb 依 XAudio2CreateReverb 的組合與 20～48 kHz；volume meter 輸入輸出相同。
    bool builtin_supports(unsigned in,unsigned out,unsigned new_rate)const {
        if(builtin==Builtin::Reverb)return audio::Reverb::supports(in,out,version>=9) && new_rate>=20000 && new_rate<=48000;
        return in && in==out && in<=64 && new_rate>=1000 && new_rate<=200000;
    }
    bool parameter_size(unsigned size)const {return builtin==Builtin::Reverb && (size==reverb_bytes || (version>=9 && size==reverb_bytes_win10));}
    std::span<const std::uint32_t> words()const {return pairs.front()->parameters();}
    void set(std::span<const std::uint32_t> values){
        std::vector<std::uint32_t> copy(values.begin(),values.end());
        // FXECHO_INITDATA 的 MaxDelay 是 Delay 的上限。
        if(info->type==audio::Effect::Type::FxEcho && copy.size()==3){const auto delay=std::bit_cast<float>(copy[2]);if(std::isfinite(delay) && delay>maximum_delay)copy[2]=std::bit_cast<std::uint32_t>(maximum_delay);}
        for(auto& effect:pairs)effect->set_parameters_clamped(copy);
    }
    bool supports(unsigned new_channels,unsigned new_rate)const {return new_channels && new_channels<=info->maximum_channels && new_rate>=info->minimum_rate && new_rate<=info->maximum_rate;}
    void configure(unsigned new_rate,unsigned new_channels){
        const std::vector<std::uint32_t> current(words().begin(),words().end());std::vector<std::shared_ptr<audio::Effect>> next;
        for(unsigned i=0;i<(new_channels+1)/2;++i){next.push_back(std::make_shared<audio::Effect>(info->type,new_rate));next.back()->set_parameters(current);}
        pairs=std::move(next);rate=new_rate;channels=new_channels;
    }
    void process(std::span<float> pcm){
        for(std::size_t sample=0;sample+channels<=pcm.size();sample+=channels)for(unsigned pair=0;pair<pairs.size();++pair){
            const auto left=pair*2,other=left+1<channels?left+1:left;float right=pcm[sample+other];pairs[pair]->process(pcm[sample+left],right);if(other!=left)pcm[sample+other]=right;}
    }
};
// 應用程式自寫的 XAPO（客體 COM 物件）。XAudio2 對它做 QueryInterface、GetRegistrationProperties、LockForProcess，
// 每個處理量子呼叫 Process，移除時 UnlockForProcess 與 Release。語音圖逐幀收集效果輸入，量子結束時在客體處理執行緒呼叫
// Process，結果在下一個量子播出（這條路徑延遲一個量子）。scratch 是本效果專用的客體記憶體（介面輸出、鎖定與處理參數、格式、緩衝）。
struct GuestXapo {
    std::uint64_t unknown{},xapo{},parameters{},scratch{},input{},output{};
    std::shared_ptr<audio::GuestBlock> block;
    unsigned in_channels{2},out_channels{2},rate{48000},max_frames{481},index{};
    bool ready{},in_place{},locked{},failed{};
};
struct Engine {
    std::shared_ptr<Graph> graph=std::make_shared<Graph>();
    // 移除的客體 XAPO：下一個處理量子開頭（處理執行緒、沒有尚未執行的 Process）才 Unlock／Release；再下一個量子釋放 scratch。
    std::vector<std::shared_ptr<GuestXapo>> guest_release,guest_free;
    std::shared_ptr<audio::Buffer> pcm=std::make_shared<audio::Buffer>();
    std::unique_ptr<audio::Output> output;
    std::vector<audio_endpoint::Endpoint> devices;
    std::set<std::uint64_t> callbacks;
    std::vector<float> quantum;
    std::size_t produced{};unsigned rate{48000},channels{2},fraction{},version{7};
    unsigned lead_quanta{3};
    std::uint64_t observed_starved_runs{};
    bool initialized{},active{true},busy{},audible_reported{};
    std::vector<std::pair<std::uint64_t,Graph::Voice>> pass_voices;
    std::uint32_t ticket{};
    std::uint64_t handle{},master{},rendered{},cpu_ns{},last_cpu{},last_wall{},last_perf_starved_runs{};
    // X64ARM_TRACE_AUDIO：每 2 秒輸出一次區間缺糧，附 unix 毫秒，供量測區間對照載入與遊玩。
    std::uint64_t stats_wall{},stats_starved{},stats_runs{},stats_quanta{};
    std::array<std::uint32_t,6> debug{};
};
struct Object {enum Kind {Factory,EngineObject,Voice,Effect} kind{};std::shared_ptr<Engine> engine;Graph::Voice voice{};std::uint64_t callback{};std::uint32_t references{1};std::optional<audio::AdpcmFormat> adpcm{};
    std::shared_ptr<Fx> fx{};            // kind==Effect（內建）
    std::shared_ptr<GuestXapo> guest{};  // kind==Effect（應用程式自寫的 XAPO；物件位址只是本層的識別碼）
    std::vector<std::uint64_t> chain{};  // kind==Voice：效果鏈上的效果物件，各持有一個參考
    xaudio::ClassicClass factory_class{7,0};
};
void require(bool valid){if(!valid)throw std::invalid_argument("XAudio2 呼叫參數無效");}
std::uint64_t now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
std::uint64_t thread_cpu(){timespec value{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value))throw HostCallFault("XAudio2 無法讀取音訊處理 CPU 時鐘");return static_cast<std::uint64_t>(value.tv_sec)*1000000000ULL+static_cast<std::uint64_t>(value.tv_nsec);}
bool trace_audio(){static const bool enabled=std::getenv("X64ARM_TRACE_AUDIO")!=nullptr;return enabled;}
bool void_method(std::string_view name){
    for(auto method:{"GetVoiceDetails","GetVolume","GetFrequencyRatio","GetFilterParameters","GetChannelVolumes","GetEffectState","GetEffectParameters","GetOutputFilterParameters","GetOutputMatrix","GetState27","GetState28","DestroyVoice","GetPerformanceData","SetDebugConfiguration","StopEngine","UnregisterForCallbacks"})if(name==method)return true;
    return false;
}
}
struct XAudioState {
    std::map<std::uint64_t,Object> objects;
    std::vector<std::uint64_t> guest_setup; // 待在呼叫端返回前完成 QueryInterface／LockForProcess 的客體 XAPO（識別碼）
    std::map<unsigned,std::uint64_t> tables;
    std::array<std::uint32_t,8> locks{};
    std::uint32_t next_ticket{1};
};
std::span<const WindowsServiceDescriptor> xaudio_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{{"xaudio2_8.dll","XAudio2Create",3,0,0,6},{"xaudio2_9.dll","XAudio2Create",3,0,0,6},
            // XAudio2CreateWithVersionInfo：多一個 ntddiVersion（標頭版本），引擎行為與 XAudio2Create 相同。
            {"xaudio2_8.dll","XAudio2CreateWithVersionInfo",4,0,0,0xe},{"xaudio2_9.dll","XAudio2CreateWithVersionInfo",4,0,0,0xe}};
        for(const auto module:xaudio::classic_dlls)for(const auto entry:{
            WindowsServiceDescriptor{module,"DllGetClassObject",3},{module,"DllCanUnloadNow",0},
            {module,"DllRegisterServer",0},{module,"DllUnregisterServer",0}})result.push_back(entry);
        result.push_back({"internal-xaudio","GetOutputMatrix20",5,0,0,12});
        for(auto methods:{std::span<const Method>(identity),std::span<const Method>(enumeration),std::span<const Method>(engine_methods),std::span<const Method>(voice_methods),std::span<const Method>(source_methods),std::span<const Method>(factory_methods)})for(const auto& m:methods)result.push_back({"internal-xaudio",m.name,m.args,0,0,m.narrow});
        result.push_back({"internal-xaudio","CreateMasteringVoice28",8,0,0,156});result.push_back({"internal-xaudio","GetState28",3,0,0,4});result.push_back({"internal-xaudio","GetChannelMask",2});
        for(auto name:{"Work","Render","Complete"})result.push_back({"internal-xaudio",name,2,0,0,2});
        for(auto name:{"XapoStep","XapoCollect"})result.push_back({"internal-xaudio",name,2,0,0,2});
        // CreateFX 是 cdecl（STDAPIVCALLTYPE）。XAPOFX1_x 兩個參數；XAudio 2.8／2.9 多了初始化資料與長度。
        for(auto module:{"xapofx1_0.dll","xapofx1_1.dll","xapofx1_2.dll","xapofx1_3.dll","xapofx1_4.dll","xapofx1_5.dll","xapofxd1_5.dll"})result.push_back({module,"CreateFX",2,0,0,0,true});
        for(auto module:{"xaudio2_8.dll","xaudio2_9.dll"})result.push_back({module,"CreateFX",4,0,0,8,true});
        for(auto module:{"xaudio2_8.dll","xaudio2_9.dll"})for(auto symbol:{"CreateAudioReverb","CreateAudioVolumeMeter"})result.push_back({module,symbol,1});
        for(const auto& m:fx_methods)result.push_back({"internal-xapofx",m.name,m.args,0,0,m.narrow});
        for(std::size_t i=3;i<std::size(fx_parameter_methods);++i)result.push_back({"internal-xapofx",fx_parameter_methods[i].name,fx_parameter_methods[i].args,0,0,fx_parameter_methods[i].narrow});
        return result;
    }();return table;
}
bool WindowsServices::xaudio_waiting()const {
    if(!xaudio_)return false;for(const auto& [p,o]:xaudio_->objects)if(o.kind==Object::EngineObject && o.engine->active && o.engine->output)return true;return false;
}
void WindowsServices::poll_xaudio(){
    if(!xaudio_)return;
    static const bool adaptive_lead=[] {
        const auto* value=std::getenv("X64ARM_AUDIO_XAUDIO_ADAPTIVE_LEAD");
        return !value || std::string_view(value)!="0";
    }();
    for(auto& [p,o]:xaudio_->objects)if(o.kind==Object::EngineObject){auto& e=*o.engine;if(!e.active || !e.output || e.busy)continue;
        std::lock_guard lock(e.pcm->mutex);auto& b=*e.pcm;
        if(trace_audio() && !e.audible_reported && (b.rendered_left_peak>0 || b.rendered_right_peak>0)){
            e.audible_reported=true;const auto time=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::fprintf(stderr,"xaudio_first_audible unix_us=%lld pid=%d stage=host_render frames=%llu peak_left=%g peak_right=%g\n",static_cast<long long>(time),static_cast<int>(::getpid()),static_cast<unsigned long long>(b.rendered_frames),double(b.rendered_left_peak),double(b.rendered_right_peak));
        }
        const auto consumed=std::min(b.pcm.size(),static_cast<std::size_t>(b.cursor)*b.frame_bytes());
        if(consumed){b.pcm.erase(b.pcm.begin(),b.pcm.begin()+static_cast<std::ptrdiff_t>(consumed));b.cursor-=static_cast<double>(consumed/b.frame_bytes());}
        if(adaptive_lead && b.starved_runs!=e.observed_starved_runs){
            // 忽略開始播放時的預填缺口。穩定供應後若再欠載，逐次增加 10 ms 餘裕，最多預填 60 ms。
            if(e.rendered>=e.rate && e.lead_quanta<6)++e.lead_quanta;
            e.observed_starved_runs=b.starved_runs;
        }
        if(b.pcm.size()/b.frame_bytes()>=e.rate*e.lead_quanta/100)continue;
        if(!xaudio_->next_ticket)throw HostCallFault("XAudio2 工作序號耗盡");
        e.ticket=xaudio_->next_ticket++;wave_callbacks_.push_back({resolve("internal-xaudio","Work").value,{p,e.ticket}});e.busy=true;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_xaudio(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("XAudio2 不能在 compare 模式使用");
    if(!xaudio_)xaudio_=std::make_shared<XAudioState>();auto& bridge=*xaudio_;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto get=[&](std::uint64_t p,unsigned offset=0,std::uint8_t width=32){return memory.load(GuestAddress{p+offset},width);};
    const auto put=[&](std::uint64_t p,std::uint64_t v,std::uint8_t width=32){memory.store(GuestAddress{p},width,v);};
    const auto check=[&](std::uint64_t p,std::size_t count){memory.check(GuestAddress{p},count,Access::Write);};
    const auto floats=[&](std::uint64_t p,unsigned count){require(p && count<=4096);std::vector<float> v(count);memory.read(GuestAddress{p},{reinterpret_cast<std::uint8_t*>(v.data()),v.size()*4});return v;};
    const auto fp=[&](unsigned i){auto word=d(i);if(!x86_ && i<4)std::memcpy(&word,state.xmm[i].data(),4);return std::bit_cast<float>(word);};
    const auto object=[&](std::uint64_t p)->Object&{const auto i=bridge.objects.find(p);require(i!=bridge.objects.end());return i->second;};
    const auto method=[&](std::uint64_t p,unsigned slot){require(p!=0);return get(get(p,0,bits),slot*unit,bits);};
    const auto publish=[&](std::uint64_t p,const Object& o){if(o.kind!=Object::Voice)return;const auto info=o.engine->graph->state(o.voice);
        put(p+XA_VERSION,o.engine->version);put(p+XA_FLAGS,info.creation_flags);put(p+XA_ACTIVE,info.flags);put(p+XA_CHANNELS,info.format.channels);put(p+XA_RATE,info.format.rate);
        put(p+XA_VOLUME,std::bit_cast<std::uint32_t>(info.volume));put(p+XA_RATIO,std::bit_cast<std::uint32_t>(info.ratio));put(p+XA_FILTER,info.filter.type);put(p+XA_FILTER+4,std::bit_cast<std::uint32_t>(info.filter.frequency));put(p+XA_FILTER+8,std::bit_cast<std::uint32_t>(info.filter.one_over_q));put(p+XA_MASK,info.format.channel_mask?info.format.channel_mask:audio::channel_mask(info.format.channels));
        memory.write(GuestAddress{p+XA_VOLUMES},{reinterpret_cast<const std::uint8_t*>(info.channel_volumes.data()),info.channel_volumes.size()*4});
        put(p+XA_CONTEXT,info.context,64);put(p+XA_QUEUED,info.queued);put(p+XA_SAMPLES,info.samples,64);
    };
    // host_volumes：效果鏈改變聲道數的語音，GetChannelVolumes 改走宿主（常駐常式比對的是輸入聲道數）。
    const auto table_for=[&](unsigned version,unsigned kind,bool host_volumes){
        const auto key=version*8+kind+(host_volumes?4096U:0U);
        if(!bridge.tables.contains(key)){
            std::vector<Method> methods;
            if(kind<2)methods.insert(methods.end(),std::begin(identity),std::end(identity));
            if(kind==0)methods.insert(methods.end(),std::begin(factory_methods),std::end(factory_methods));
            else if(kind==1){if(version<=7)methods.insert(methods.end(),std::begin(enumeration),std::end(enumeration));for(auto m:engine_methods){if(m.name=="CreateMasteringVoice27" && version>=8)m={"CreateMasteringVoice28",8,156};methods.push_back(m);}}
            else {
                for(auto m:voice_methods){
                    if(version<=3 && (m.name=="SetOutputFilterParameters" || m.name=="GetOutputFilterParameters"))continue;
                    if(version==0 && m.name=="GetOutputMatrix")m.name="GetOutputMatrix20";
                    methods.push_back(m);
                }
                if(kind==2)for(auto m:source_methods){
                    if(version<=3 && m.name=="SetSourceSampleRate")continue;
                    if(m.name=="GetState27" && version>=8)m={"GetState28",3,4};
                    methods.push_back(m);
                }
                if(kind==4 && version>=8)methods.push_back({"GetChannelMask",2});
            }
            const auto table=runtime_allocate(memory,{},methods.size()*unit);
            const auto image=runtime_.builtin_images.find(callback_return_.value+xaudio_lowering::image_offset);
            for(unsigned i=0;i<methods.size();++i){auto target=resolve("internal-xaudio",methods[i].name).value;
                if(image!=runtime_.builtin_images.end() && !(host_volumes && methods[i].name=="GetChannelVolumes"))if(const auto routine=image->second.export_names.find(std::string(methods[i].name));routine!=image->second.export_names.end())target=image->first+routine->second;
                put(table+i*unit,target,bits);
            }bridge.tables.emplace(key,table);
        }
        return bridge.tables.at(key);
    };
    const auto object_kind=[&](const Object& o){return o.kind==Object::Factory?0U:o.kind==Object::EngineObject?1U:2U+static_cast<unsigned>(o.engine->graph->state(o.voice).kind);};
    const auto create=[&](Object o){
        const auto version=o.engine?o.engine->version:o.factory_class.version;const auto table=table_for(version,object_kind(o),false);
        const auto pointer=allocate_heap(memory,XA_OBJECT_BYTES);if(!pointer)throw std::bad_alloc{};memory.write(GuestAddress{pointer},std::array<std::uint8_t,XA_OBJECT_BYTES>{});put(pointer,table,bits);publish(pointer,o);bridge.objects.emplace(pointer,std::move(o));return pointer;
    };
    const auto new_engine=[&](unsigned version,bool initialized){auto e=std::make_shared<Engine>();e->version=version;e->initialized=initialized;e->devices=audio_endpoint::render();e->last_wall=now();const auto p=create({Object::EngineObject,e});e->handle=p;return p;};
    const auto iid=[&](std::uint64_t p){xaudio::Guid g{};memory.read(GuestAddress{p},g);return g;};
    const auto sends=[&](const std::shared_ptr<Engine>& engine,std::uint64_t p){std::vector<Graph::Send> result;
        if(!p){require(engine->master!=0);result.push_back({object(engine->master).voice,{},{}});return result;}
        const auto count=get(p),items=get(p,4,bits);require(count<=64 && (!count || items));
        const bool pointers=engine->version<=3;
        const unsigned stride=unit+(pointers?0U:4U);
        for(unsigned i=0;i<count;++i){
            const auto at=items+i*stride,flags=pointers?0:get(at),target=get(at,pointers?0:4,bits);
            require(!(flags&~128U));auto& o=object(target);require(o.kind==Object::Voice && o.engine==engine);
            result.push_back({o.voice,{},bool(flags&128U)});
        }
        return result;
    };
    // 效果物件可由 IXAPO 或 IXAPOParameters 介面指標（物件位址＋一個指標）辨識。
    const auto fx_base=[&](std::uint64_t p)->std::uint64_t{
        for(const auto base:{p,p-unit})if(const auto i=bridge.objects.find(base);i!=bridge.objects.end() && i->second.kind==Object::Effect && i->second.fx)return base;
        return 0;
    };
    const auto release_fx=[&](std::uint64_t base){auto& item=object(base);const auto left=--item.references;
        if(!left){if(item.guest){if(item.engine)item.engine->guest_release.push_back(item.guest);std::erase(bridge.guest_setup,base);}
            bridge.objects.erase(base);(void)free_heap(memory,base,state);}return left;};
    struct ChainItem {std::uint64_t base{};bool enabled{};unsigned output{};};
    using Chain=std::vector<ChainItem>;
    // XAUDIO2_EFFECT_CHAIN（pack 1）。內建 XAPOFX 與 XAudio2 內建效果直接以宿主 DSP 掛進語音圖；應用程式自寫的客體 XAPO 仍是明確缺口。
    const auto read_chain=[&](std::uint64_t p,unsigned channels,unsigned rate,std::uint64_t voice){Chain result;
        // 先前建立後未掛上的客體 XAPO 識別碼（建立語音失敗）不再保留，免得擋住同一物件重新掛入。
        for(auto i=bridge.objects.begin();i!=bridge.objects.end();)if(i->second.guest && !i->second.references){std::erase(bridge.guest_setup,i->first);(void)free_heap(memory,i->first,state);i=bridge.objects.erase(i);}else ++i;
        if(!p)return result;
        const auto count=get(p),items=get(p,4,bits);require(count<=64 && (!count || items));
        unsigned current=channels;
        for(unsigned i=0;i<count;++i){const auto at=items+i*(unit+8);const auto pointer=get(at,0,bits);auto base=fx_base(pointer);
            const auto output=static_cast<unsigned>(get(at,unit+4));
            if(!base){
                // 應用程式自寫的 XAPO：依 XAPO 契約處理任意聲道數（原地處理時輸入輸出相同，於鎖定時核對）。同一物件不得重複掛入或同時掛在別的語音。
                require(pointer && output && output<=64 && rate>=1000 && rate<=200000);
                require(std::none_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& entry){return entry.second.guest && entry.second.guest->unknown==pointer;}));
                (void)method(pointer,0);const auto id=allocate_heap(memory,16);if(!id)throw std::bad_alloc{};
                Object item;item.kind=Object::Effect;item.references=0;item.guest=std::make_shared<GuestXapo>();item.guest->unknown=pointer;
                bridge.objects.emplace(id,std::move(item));base=id;
                result.push_back({base,get(at,unit)!=0,output});current=output;continue;
            }
            const auto& fx=*object(base).fx;
            // 每個效果的輸入是前一個效果的輸出（XAUDIO2_EFFECT_DESCRIPTOR.OutputChannels）。XAPOFX 不改變聲道數；
            // 已被應用程式或其他語音鎖定的效果不能再掛入。
            if(fx.builtin==Builtin::None)require(output==current && fx.supports(current,rate));else require(fx.builtin_supports(current,output,rate));
            require(!fx.locked || (voice && fx.voice==voice));
            require(std::none_of(result.begin(),result.end(),[&](const auto& v){return v.base==base;}));result.push_back({base,get(at,unit)!=0,output});current=output;
        }return result;
    };
    // 語音輸出聲道數與輸入不同時，GetChannelVolumes 改用宿主入口的虛擬表。
    const auto retable=[&](std::uint64_t pointer){
        const auto& o=object(pointer);const auto info=o.engine->graph->state(o.voice);
        put(pointer,table_for(o.engine->version,object_kind(o),info.output_channels!=info.format.channels),bits);
    };
    const auto set_chain=[&](std::uint64_t pointer,const Chain& chain,unsigned rate,unsigned channels){
        auto& voice=object(pointer);std::vector<Graph::EffectSlot> slots;unsigned current=channels;
        for(const auto& item:chain){
            if(const auto& guest=object(item.base).guest){
                guest->in_channels=current;guest->out_channels=item.output;guest->rate=rate;guest->max_frames=rate/100+1;guest->index=static_cast<unsigned>(slots.size());
                guest->block=std::make_shared<audio::GuestBlock>(current,item.output,guest->max_frames);
                slots.push_back({{},item.enabled,{},guest->block});current=item.output;continue;
            }
            auto& fx=*object(item.base).fx;
            if(fx.builtin==Builtin::None){fx.configure(rate,current);slots.push_back({fx.pairs,item.enabled,{}});}
            else {require(fx.dsp->configure(rate,current,item.output));fx.rate=rate;fx.channels=item.output;slots.push_back({{},item.enabled,fx.dsp});}
            current=item.output;
        }
        voice.engine->graph->set_effect_chain(voice.voice,std::move(slots));
        const auto old=std::move(voice.chain);voice.chain.clear();
        for(const auto base:old)if(auto& fx=object(base).fx){fx->locked=false;fx->voice=0;}
        for(const auto& item:chain){auto& entry=object(item.base);entry.engine=voice.engine;
            if(entry.fx){entry.fx->locked=true;entry.fx->voice=pointer;entry.fx->maximum_frames=0;}
            else if(!entry.guest->ready && std::find(bridge.guest_setup.begin(),bridge.guest_setup.end(),item.base)==bridge.guest_setup.end())bridge.guest_setup.push_back(item.base);
            ++entry.references;voice.chain.push_back(item.base);}
        for(const auto base:old)(void)release_fx(base);
        retable(pointer);
    };
    const auto drop_chain=[&](Object& voice){const auto old=std::move(voice.chain);voice.chain.clear();for(const auto base:old){if(auto& fx=object(base).fx){fx->locked=false;fx->voice=0;}(void)release_fx(base);}};
    const auto fx_format=[&](const Fx& fx,std::uint64_t p)->std::optional<PcmWaveFormat>{
        if(!p)return std::nullopt;
        const unsigned low=fx.builtin==Builtin::None?fx.info->minimum_rate:fx.builtin==Builtin::Reverb?20000U:1000U,high=fx.builtin==Builtin::None?fx.info->maximum_rate:fx.builtin==Builtin::Reverb?48000U:200000U;
        const unsigned most=fx.builtin==Builtin::None?fx.info->maximum_channels:fx.builtin==Builtin::Reverb?8U:64U;
        auto format=read_pcm_wave_format(memory,p,true,true,low,high,{most,32,true,true});
        if(format && !format->floating)format.reset();return format;
    };
    const auto task_allocate=[&](std::size_t size){const auto p=allocate_heap(memory,size);if(!p)throw std::bad_alloc{};gui_.task_allocations.insert(p);memory.write(GuestAddress{p},std::vector<std::uint8_t>(size));return p;};
    const auto words_from=[&](std::uint64_t p,std::size_t count){std::vector<std::uint32_t> values(count);memory.read(GuestAddress{p},{reinterpret_cast<std::uint8_t*>(values.data()),count*4});return values;};
    const auto words_to=[&](std::uint64_t p,std::span<const std::uint32_t> values){check(p,values.size()*4);memory.write(GuestAddress{p},{reinterpret_cast<const std::uint8_t*>(values.data()),values.size()*4});};
    // 效果物件：IXAPO 與 IXAPOParameters 兩個介面指標，XAPOFX 與 XAudio2 內建效果共用方法表。
    const auto install_fx=[&](std::shared_ptr<Fx> fx){
        for(unsigned key=0;key<2;++key)if(!bridge.tables.contains(1000+key)){
            const auto methods=key?std::span<const Method>(fx_parameter_methods):std::span<const Method>(fx_methods);const auto table=runtime_allocate(memory,{},methods.size()*unit);
            for(unsigned i=0;i<methods.size();++i)put(table+i*unit,resolve("internal-xapofx",methods[i].name).value,bits);bridge.tables.emplace(1000+key,table);
        }
        const auto pointer=allocate_heap(memory,2*unit);if(!pointer)throw std::bad_alloc{};put(pointer,bridge.tables.at(1000),bits);put(pointer+unit,bridge.tables.at(1001),bits);
        Object item;item.kind=Object::Effect;item.fx=std::move(fx);bridge.objects.emplace(pointer,std::move(item));return pointer;
    };
    // 2.0 至 2.7 的效果工廠共用 IXAPO ABI，各版保留自己的 CLSID。
    const auto create_builtin=[&](xaudio::ClassicClass klass,std::uint64_t riid,std::uint64_t out){
        const auto query=iid(riid);
        if(query!=xaudio::unknown && query!=xapo27 && query!=xapo_parameters27)return no_interface;
        const auto base=install_fx(std::make_shared<Fx>(klass.kind==2?Builtin::Reverb:Builtin::Meter,klass.version));
        put(out,query==xapo_parameters27?base+unit:base,bits);return 0U;
    };
    // XAUDIO2FX_VOLUMEMETER_LEVELS（pack 1）：兩個由應用程式配置的陣列指標與 ChannelCount；只填入非空的陣列。
    const unsigned levels_bytes=2*unit+4;
    const auto write_levels=[&](const Fx& fx,std::uint64_t p){
        const auto peak=get(p,0,bits),rms=get(p,unit,bits);const auto count=static_cast<unsigned>(get(p,2*unit));
        if(count!=fx.dsp->output_channels() || (!peak && !rms))return false;
        const auto levels=fx.meter().levels();
        if(peak){check(peak,count*4);memory.write(GuestAddress{peak},{reinterpret_cast<const std::uint8_t*>(levels.peak.data()),count*4U});}
        if(rms){check(rms,count*4);memory.write(GuestAddress{rms},{reinterpret_cast<const std::uint8_t*>(levels.rms.data()),count*4U});}
        return true;
    };
    const auto legacy_dll=std::find(xaudio::classic_dlls.begin(),xaudio::classic_dlls.end(),service.family);
    const auto legacy_version=static_cast<unsigned>(legacy_dll-xaudio::classic_dlls.begin());
    std::uint64_t result=0;
    std::vector<GuestCallback> guest_calls; // 呼叫端返回前要執行的客體 XAPO 方法（效果參數轉送）
    // 客體 XAPO 的 scratch 版面：+0 QueryInterface(IXAPO) 輸出、+8 註冊屬性指標、+16 QueryInterface(IXAPOParameters) 輸出、
    // +32／+48 輸入／輸出 XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS、+64／+80 輸入／輸出 XAPO_PROCESS_BUFFER_PARAMETERS（pack 1）、
    // +96／+144 輸入／輸出 WAVEFORMATEXTENSIBLE（IEEE float 32 位元）、+192／+208 IID_IXAPO／IID_IXAPOParameters，+256 起輸入與輸出緩衝。
    const auto xapo_setup_calls=[&](std::uint64_t id,std::vector<GuestCallback>& calls){
        auto& entry=object(id);auto& g=*entry.guest;require(entry.engine!=nullptr);
        const std::uint64_t in_bytes=std::uint64_t{g.max_frames}*g.in_channels*4,out_bytes=std::uint64_t{g.max_frames}*g.out_channels*4;
        if(!g.scratch){g.scratch=allocate_heap(memory,static_cast<std::size_t>(256+in_bytes+out_bytes));if(!g.scratch)throw std::bad_alloc{};memory.fill(GuestAddress{g.scratch},0,256);}
        g.input=g.scratch+256;g.output=g.input+in_bytes;
        memory.write(GuestAddress{g.scratch+192},entry.engine->version<=7?xapo27:xapo28);memory.write(GuestAddress{g.scratch+208},entry.engine->version<=7?xapo_parameters27:xapo_parameters28);
        calls.push_back({method(g.unknown,0),{g.unknown,g.scratch+192,g.scratch}});
        calls.push_back({resolve("internal-xaudio","XapoStep").value,{id,1}});
    };
    try {
        if(name=="XapoStep" || name=="XapoCollect"){
            const auto found=bridge.objects.find(arg(0));
            if(found==bridge.objects.end() || !found->second.guest || !found->second.guest->scratch){arg.return_to(state,0);return std::nullopt;}
            auto& g=*found->second.guest;std::vector<GuestCallback> next;
            if(name=="XapoCollect"){
                // Process 寫回的輸出（XAPO_BUFFER_SILENT 時為靜音）接到語音圖，下一個量子播出。
                const auto frames=std::min<std::uint32_t>(d(1),g.max_frames);const auto flags=get(g.scratch+80+unit);
                std::vector<float> samples(std::size_t{frames}*g.out_channels);
                if(frames && flags)memory.read(GuestAddress{g.in_place?g.input:g.output},{reinterpret_cast<std::uint8_t*>(samples.data()),samples.size()*4});
                if(g.block)g.block->append_output(samples);
            }else if(d(1)==1){
                g.xapo=get(g.scratch,0,bits);
                if(!g.xapo)g.failed=true;
                else {next.push_back({method(g.xapo,3),{g.xapo,g.scratch+8}});next.push_back({resolve("internal-xaudio","XapoStep").value,{arg(0),2}});}
            }else if(d(1)==2){
                // 註冊屬性（pack 1：CLSID、兩個 256 字元字串、主次版本後是 Flags）由 XAPO 以 CoTaskMemAlloc 配置，讀完即釋放。
                const auto properties=get(g.scratch+8,0,bits);const auto flags=properties?static_cast<std::uint32_t>(get(properties,1048)):0x10U;
                if(properties)next.push_back({resolve("ole32.dll","CoTaskMemFree").value,{properties}});
                // XAPO_FLAG_INPLACE_REQUIRED（0x20）只能原地處理；INPLACE_SUPPORTED（0x10）且聲道數相同時也原地處理，其餘用獨立輸出緩衝。
                const bool required=(flags&0x20U)!=0;
                if(required && g.in_channels!=g.out_channels)g.failed=true;
                else {
                    g.in_place=required || ((flags&0x10U) && g.in_channels==g.out_channels);
                    const auto format=[&](std::uint64_t at,unsigned channels){
                        std::array<std::uint8_t,40> w{};const auto u16=[&](unsigned o,unsigned v){w[o]=static_cast<std::uint8_t>(v);w[o+1]=static_cast<std::uint8_t>(v>>8);};
                        const auto u32=[&](unsigned o,std::uint32_t v){for(unsigned i=0;i<4;++i)w[o+i]=static_cast<std::uint8_t>(v>>(8*i));};
                        u16(0,0xfffe);u16(2,channels);u32(4,g.rate);u32(8,g.rate*channels*4);u16(12,channels*4);u16(14,32);u16(16,22);u16(18,32);u32(20,audio::channel_mask(channels));
                        constexpr std::array<std::uint8_t,16> ieee_float{3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};std::copy(ieee_float.begin(),ieee_float.end(),w.begin()+24);
                        memory.write(GuestAddress{at},w);
                    };
                    format(g.scratch+96,g.in_channels);format(g.scratch+144,g.out_channels);
                    put(g.scratch+32,g.scratch+96,bits);put(g.scratch+32+unit,g.max_frames);put(g.scratch+48,g.scratch+144,bits);put(g.scratch+48+unit,g.max_frames);
                    next.push_back({method(g.xapo,8),{g.xapo,1,g.scratch+32,1,g.scratch+48}});
                    next.push_back({method(g.xapo,0),{g.xapo,g.scratch+208,g.scratch+16}});
                    next.push_back({resolve("internal-xaudio","XapoStep").value,{arg(0),3}});
                }
            }else if(d(1)==3){g.parameters=get(g.scratch+16,0,bits);g.locked=true;g.ready=!g.failed;}
            if(!next.empty()){start_callbacks(state,memory,std::move(next),Continuation::Return,0);return std::nullopt;}
            arg.return_to(state,0);return std::nullopt;
        }
        if(name=="DllRegisterServer" || name=="DllUnregisterServer"){
#include "xaudio_classic_registration.inc"
        }else if(name=="DllCanUnloadNow"){
            const bool active=std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& item){
                const auto& value=item.second;
                if(value.engine)return value.engine->version==legacy_version;
                if(value.fx)return value.fx->builtin!=Builtin::None && value.fx->version==legacy_version;
                return value.factory_class.version==legacy_version;
            });
            result=active || bridge.locks.at(legacy_version)?1:0;
        }
        else if(name=="DllGetClassObject" || name=="CoGetClassObject"){
            const bool com=name=="CoGetClassObject";const auto out=arg(com?4:2);check(out,unit);put(out,0,bits);
            const auto klass=xaudio::classic_class(memory,arg(0));
            if(!klass || (com?!(d(1)&1):klass->version!=legacy_version))result=0x80040111U;
            else if(iid(arg(com?3:1))!=xaudio::factory && iid(arg(com?3:1))!=xaudio::unknown)result=no_interface;
            else {Object factory{Object::Factory,{}};factory.factory_class=*klass;put(out,create(std::move(factory)),bits);}
        }else if(name=="CoCreateInstance" || name=="XAudio2Create" || name=="XAudio2CreateWithVersionInfo"){
            const bool com=name=="CoCreateInstance";const auto out=arg(com?4:0);check(out,unit);put(out,0,bits);
            if(com){
                const auto klass=xaudio::classic_class(memory,arg(0));
                if(arg(1))result=0x80040110U;
                else if(!klass || !(d(2)&1))result=0x80040154U;
                else if(!com_apartment(threads_.current))result=0x800401f0U;
                else if(klass->kind>=2)result=create_builtin(*klass,arg(3),out);
                else if(iid(arg(3))!=xaudio::iid27 && iid(arg(3))!=xaudio::unknown)result=no_interface;
                else put(out,new_engine(klass->version,false),bits);
            }else{
                // XAudio2Create 的旗標：DEBUG_ENGINE、STOP_ENGINE_WHEN_IDLE、1024_QUANTUM、NO_VIRTUAL_AUDIO_CLIENT。
                require(!(d(1)&~UINT32_C(0x1a001)));
                put(out,new_engine(service.family=="xaudio2_8.dll"?8:9,true),bits);
            }
        }else if(name=="CreateFX"){
            const auto out=arg(1);check(out,unit);put(out,0,bits);require(arg(0)!=0);const auto id=iid(arg(0));
            const auto found=std::find_if(std::begin(fx_classes),std::end(fx_classes),[&](const auto& v){return v.legacy==id || v.current==id;});
            if(found==std::end(fx_classes))result=0x80040111U;
            else {
                auto fx=std::make_shared<Fx>(*found,id);
                if(service.arguments==4 && arg(2) && d(3)){
                    // 2.8 以後的初始化資料：FXEcho 是 FXECHO_INITDATA{MaxDelay}；其餘（以及長度等於參數結構時）當作初始參數。
                    const auto bytes=fx->words().size()*4;
                    if(found->type==audio::Effect::Type::FxEcho && d(3)==4){const auto delay=std::bit_cast<float>(static_cast<std::uint32_t>(get(arg(2))));require(std::isfinite(delay) && delay>=1 && delay<=2000);fx->maximum_delay=delay;fx->set(std::vector<std::uint32_t>(fx->words().begin(),fx->words().end()));}
                    else {require(d(3)==bytes);fx->set(words_from(arg(2),fx->words().size()));}
                }
                put(out,install_fx(std::move(fx)),bits);
            }
        }else if(name=="CreateAudioReverb" || name=="CreateAudioVolumeMeter"){
            // XAudio 2.8／2.9（STDAPI）：XAudio2CreateReverb／XAudio2CreateVolumeMeter 的內嵌函式直接呼叫這兩個匯出。
            const auto out=arg(0);check(out,unit);put(out,0,bits);
            put(out,install_fx(std::make_shared<Fx>(name=="CreateAudioReverb"?Builtin::Reverb:Builtin::Meter,service.family=="xaudio2_8.dll"?8U:9U)),bits);
        }else if(service.family=="internal-xapofx"){
            const auto base=fx_base(arg(0));require(base!=0);auto& o=object(base);auto& fx=*o.fx;const auto count=fx.builtin==Builtin::None?fx.words().size():0;
            if(name=="QueryInterface"){
                check(arg(2),unit);put(arg(2),0,bits);const auto query=iid(arg(1));
                if(query==xaudio::unknown || query==xapo27 || query==xapo28){++o.references;put(arg(2),base,bits);}
                else if(query==xapo_parameters27 || query==xapo_parameters28){++o.references;put(arg(2),base+unit,bits);}
                else result=no_interface;
            }else if(name=="AddRef")result=++o.references;
            else if(name=="Release")result=release_fx(base);
            else if(fx.builtin!=Builtin::None){
                // XAudio2 內建 reverb／volume meter 的 IXAPO 與 IXAPOParameters。
                const bool reverb=fx.builtin==Builtin::Reverb;
                if(name=="GetRegistrationProperties"){
                    check(arg(1),unit);const auto p=task_allocate(1068);memory.write(GuestAddress{p},fx.clsid);memory.write(GuestAddress{p+16},utf16_bytes(reverb?u"AudioReverb":u"AudioVolumeMeter"));
                    // reverb 會改變聲道數：只要求取樣率、位元數與緩衝數相同，不支援原地處理；volume meter 與 XAPOFX 相同。
                    put(p+1040,1);put(p+1048,reverb?0x0eU:0x1fU);for(unsigned i=0;i<4;++i)put(p+1052+i*4,1);put(arg(1),p,bits);
                }else if(name=="IsInputFormatSupported" || name=="IsOutputFormatSupported"){
                    const bool input=name=="IsInputFormatSupported";const auto other=fx_format(fx,arg(1)),requested=fx_format(fx,arg(2));
                    bool supported=requested && (!arg(1) || (other && other->rate==requested->rate));
                    if(supported){
                        const unsigned in=input?requested->channels:(other?other->channels:0U),out=input?(other?other->channels:0U):requested->channels;
                        if(reverb)supported=in<=2 && (!out || out<=2 || out==6 || (fx.version>=9 && out==8)) && (!in || !out || audio::Reverb::supports(in,out,fx.version>=9));
                        else supported=!in || !out || in==out;
                    }
                    if(!supported){
                        result=format_unsupported;
                        if(arg(3)){
                            // 回報最接近的格式：取樣率依另一端或夾入支援範圍；聲道依支援的組合。
                            check(arg(3),unit);require(arg(2)!=0);
                            const unsigned low=reverb?20000U:1000U,high=reverb?48000U:200000U;
                            const unsigned rate=other?other->rate:std::clamp<unsigned>(static_cast<unsigned>(get(arg(2),4)),low,high);
                            const unsigned asked=std::max(1U,static_cast<unsigned>(get(arg(2),2,16)));unsigned channels=asked;
                            if(!reverb)channels=other?other->channels:std::min(asked,64U);
                            else if(input)channels=other && other->channels<=2?other->channels:std::min(asked,2U);
                            else channels=other?other->channels:(asked>2?6U:asked);
                            const auto p=task_allocate(18);put(p,3,16);put(p+2,channels,16);put(p+4,rate);put(p+8,rate*channels*4);put(p+12,channels*4,16);put(p+14,32,16);put(arg(3),p,bits);
                        }
                    }
                }else if(name=="Initialize"){
                    // 初始化資料等同初始參數；volume meter 沒有初始化資料。
                    if(arg(1) && d(2)){require(fx.parameter_size(d(2)));std::vector<std::uint8_t> bytes(d(2));memory.read(GuestAddress{arg(1)},bytes);fx.reverb().set(parse_reverb(bytes));}
                }else if(name=="Reset")fx.dsp->reset();
                else if(name=="LockForProcess"){
                    if(fx.locked || d(1)!=1 || d(3)!=1 || !arg(2) || !arg(4))result=invalid_argument_error;
                    else {
                        const auto input=fx_format(fx,get(arg(2),0,bits)),output=fx_format(fx,get(arg(4),0,bits));
                        if(!input || !output || input->rate!=output->rate || !fx.builtin_supports(input->channels,output->channels,input->rate) || !fx.dsp->configure(input->rate,input->channels,output->channels))result=format_unsupported;
                        else {
                            fx.rate=input->rate;fx.channels=output->channels;fx.maximum_frames=static_cast<unsigned>(std::min(get(arg(2),unit),get(arg(4),unit)));fx.locked=true;
                            if(!reverb)fx.meter().set_pass(UINT32_MAX); // 直接呼叫 Process：整次呼叫為一個處理段
                        }
                    }
                }else if(name=="UnlockForProcess"){if(!fx.voice)fx.locked=false;}
                else if(name=="Process"){
                    // 只在鎖定後處理；輸入與輸出聲道數可不同，先讀完整段輸入再寫出，同一塊緩衝區也安全。
                    if(fx.locked && !fx.voice && d(1)==1 && d(3)==1 && arg(2) && arg(4)){
                        const auto source=get(arg(2),0,bits),flags=get(arg(2),unit),frames=get(arg(2),unit+4),target=get(arg(4),0,bits);
                        const unsigned in=fx.dsp->input_channels(),out=fx.dsp->output_channels();
                        require(flags<=1 && frames<=fx.maximum_frames && frames<=(UINT64_C(1)<<24)/std::max(in,out));
                        const bool silent=!flags;std::vector<float> input(static_cast<std::size_t>(frames)*in),output(static_cast<std::size_t>(frames)*out);
                        if(frames && !silent){require(source!=0);memory.read(GuestAddress{source},{reinterpret_cast<std::uint8_t*>(input.data()),input.size()*4});}
                        std::array<float,64> frame{};
                        for(std::size_t i=0;i<frames;++i){
                            frame.fill(0);std::copy_n(input.begin()+static_cast<std::ptrdiff_t>(i*in),in,frame.begin());
                            fx.dsp->process(frame.data(),d(5)!=0);std::copy_n(frame.begin(),out,output.begin()+static_cast<std::ptrdiff_t>(i*out));
                        }
                        if(!reverb)fx.meter().finish_pass();
                        // reverb 有尾音，輸出一律有效；volume meter 原樣通過，靜音輸入不寫入緩衝區。
                        std::uint64_t output_flags=flags;
                        if(frames && (reverb || !silent)){require(target!=0);memory.write(GuestAddress{target},{reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4});output_flags=reverb?1:flags;}
                        put(arg(4)+unit,output_flags);put(arg(4)+unit+4,frames);
                    }
                }else if(name=="CalcInputFrames" || name=="CalcOutputFrames")result=d(1);
                else if(name=="SetParameters"){if(reverb && arg(1) && fx.parameter_size(d(2))){std::vector<std::uint8_t> bytes(d(2));memory.read(GuestAddress{arg(1)},bytes);fx.reverb().set(parse_reverb(bytes));}}
                else if(name=="GetParameters"){
                    if(reverb){if(arg(1) && fx.parameter_size(d(2))){const auto bytes=serialize_reverb(fx.reverb().parameters(),d(2));check(arg(1),bytes.size());memory.write(GuestAddress{arg(1)},bytes);}}
                    else if(arg(1) && d(2)==levels_bytes)(void)write_levels(fx,arg(1));
                }
                else throw HostCallFault("XAudio2 內建效果方法不合法："+std::string(name));
            }
            else if(name=="GetRegistrationProperties"){
                // XAPO_REGISTRATION_PROPERTIES（pack 1，1068 位元組），由呼叫端以 XAPOFree（CoTaskMemFree）釋放。
                check(arg(1),unit);const auto p=task_allocate(1068);memory.write(GuestAddress{p},fx.clsid);memory.write(GuestAddress{p+16},utf16_bytes(fx.info->name));
                put(p+1040,1);put(p+1048,0x1f);for(unsigned i=0;i<4;++i)put(p+1052+i*4,1);put(arg(1),p,bits);
            }else if(name=="IsInputFormatSupported" || name=="IsOutputFormatSupported"){
                // 輸入與輸出格式必須相同（聲道、取樣率、FLOAT32）。不支援時依 CXAPOBase 回報最接近的格式。
                const auto other=fx_format(fx,arg(1)),requested=fx_format(fx,arg(2));
                if(!requested || (arg(1) && (!other || other->channels!=requested->channels || other->rate!=requested->rate))){
                    result=format_unsupported;
                    if(arg(3)){
                        check(arg(3),unit);unsigned channels=other?other->channels:0,rate=other?other->rate:0;
                        if(!other){require(arg(2)!=0);channels=std::clamp<unsigned>(static_cast<unsigned>(get(arg(2),2,16)),1,fx.info->maximum_channels);rate=std::clamp<unsigned>(static_cast<unsigned>(get(arg(2),4)),fx.info->minimum_rate,fx.info->maximum_rate);}
                        const auto p=task_allocate(18);put(p,3,16);put(p+2,channels,16);put(p+4,rate);put(p+8,rate*channels*4);put(p+12,channels*4,16);put(p+14,32,16);put(arg(3),p,bits);
                    }
                }
            }else if(name=="Initialize"){
                // IXAPO::Initialize 的資料與 CreateFX 的初始化資料同義；沒有資料時維持目前參數。
                if(arg(1) && d(2)){
                    if(fx.info->type==audio::Effect::Type::FxEcho && d(2)==4){const auto delay=std::bit_cast<float>(static_cast<std::uint32_t>(get(arg(1))));require(std::isfinite(delay) && delay>=1 && delay<=2000);fx.maximum_delay=delay;fx.set(std::vector<std::uint32_t>(fx.words().begin(),fx.words().end()));}
                    else {require(d(2)==count*4);fx.set(words_from(arg(1),count));}
                }
            }else if(name=="Reset"){for(auto& effect:fx.pairs)effect->reset();}
            else if(name=="LockForProcess"){
                if(fx.locked || d(1)!=1 || d(3)!=1 || !arg(2) || !arg(4))result=invalid_argument_error;
                else {
                    const auto input=fx_format(fx,get(arg(2),0,bits)),output=fx_format(fx,get(arg(4),0,bits));
                    if(!input || !output || input->channels!=output->channels || input->rate!=output->rate)result=format_unsupported;
                    else {fx.configure(input->rate,input->channels);fx.maximum_frames=static_cast<unsigned>(std::min(get(arg(2),unit),get(arg(4),unit)));fx.locked=true;}
                }
            }else if(name=="UnlockForProcess"){if(!fx.voice)fx.locked=false;}
            else if(name=="Process"){
                // 只在鎖定後處理；輸入輸出各一個緩衝區，可為同一塊（原地處理）。
                if(fx.locked && !fx.voice && d(1)==1 && d(3)==1 && arg(2) && arg(4)){
                    const auto source=get(arg(2),0,bits),flags=get(arg(2),unit),frames=get(arg(2),unit+4),target=get(arg(4),0,bits);
                    require(flags<=1 && frames<=fx.maximum_frames && frames<=(UINT64_C(1)<<24)/fx.channels);
                    const bool silent=!flags,tail=fx.info->type==audio::Effect::Type::FxReverb || fx.info->type==audio::Effect::Type::FxEcho;
                    std::vector<float> pcm(static_cast<std::size_t>(frames)*fx.channels);const std::span<std::uint8_t> bytes{reinterpret_cast<std::uint8_t*>(pcm.data()),pcm.size()*4};
                    std::uint64_t output_flags=flags;
                    if(frames && !silent){require(source && target);memory.read(GuestAddress{source},bytes);}
                    if(!frames){}
                    else if(!d(5)){if(!silent && source!=target)memory.write(GuestAddress{target},bytes);} // 停用：原樣通過
                    else if(silent && !tail){}                                                         // 無尾音的效果：靜音進、靜音出，不讀寫緩衝區
                    else {require(target!=0);fx.process(pcm);memory.write(GuestAddress{target},bytes);output_flags=1;}
                    put(arg(4)+unit,output_flags);put(arg(4)+unit+4,frames);
                }
            }else if(name=="CalcInputFrames" || name=="CalcOutputFrames")result=d(1);
            else if(name=="SetParameters"){if(arg(1) && d(2)==count*4)fx.set(words_from(arg(1),count));}
            else if(name=="GetParameters"){if(arg(1) && d(2)==count*4)words_to(arg(1),fx.words());}
            else throw HostCallFault("XAPOFX 方法不合法："+std::string(name));
        }else {
            if(name=="Work" || name=="Render" || name=="Complete"){
                const auto found=bridge.objects.find(arg(0));
                if(found==bridge.objects.end() || found->second.kind!=Object::EngineObject || found->second.engine->ticket!=d(1)){arg.return_to(state,0);return std::nullopt;}
            }
            auto& o=object(arg(0));auto e=o.engine;
            if(name=="QueryInterface"){
                check(arg(2),unit);put(arg(2),0,bits);const auto query=iid(arg(1));require(o.kind!=Object::Voice);
                const auto accepted=o.kind==Object::Factory?xaudio::factory:e->version<=7?xaudio::iid27:e->version==8?xaudio::iid28:xaudio::iid29;
                if(query!=xaudio::unknown && query!=accepted)result=no_interface;else {++o.references;put(arg(2),arg(0),bits);}
            }else if(name=="AddRef")result=++o.references;
            else if(name=="Release"){
                require(o.kind!=Object::Voice);result=--o.references;if(!result){
                    if(e){e->output.reset();if(trace_audio()){std::lock_guard lock(e->pcm->mutex);const auto& pcm=*e->pcm;
                        std::fprintf(stderr,"xaudio_output pid=%d version=%u rendered_frames=%llu starved_frames=%llu starved_runs=%llu peak_left=%g peak_right=%g\n",static_cast<int>(::getpid()),e->version,static_cast<unsigned long long>(pcm.rendered_frames),static_cast<unsigned long long>(pcm.starved_frames),static_cast<unsigned long long>(pcm.starved_runs),double(pcm.rendered_left_peak),double(pcm.rendered_right_peak));}
                        std::vector<std::uint64_t> voices;for(const auto& [p,item]:bridge.objects)if(item.engine==e && item.kind==Object::Voice)voices.push_back(p);for(auto p:voices){drop_chain(object(p));bridge.objects.erase(p);(void)free_heap(memory,p,state);}}
                    bridge.objects.erase(arg(0));(void)free_heap(memory,arg(0),state);
                }
            }else if(o.kind==Object::Factory){
                if(name=="LockServer"){
                    auto& locks=bridge.locks.at(o.factory_class.version);
                    if(d(1)){require(locks<UINT32_MAX);++locks;}
                    else if(locks)--locks;
                }
                else if(name=="CreateInstance"){
                    check(arg(3),unit);put(arg(3),0,bits);
                    if(arg(1))result=0x80040110U;
                    else if(o.factory_class.kind>=2)result=create_builtin(o.factory_class,arg(2),arg(3));
                    else if(iid(arg(2))!=xaudio::iid27 && iid(arg(2))!=xaudio::unknown)result=no_interface;
                    else put(arg(3),new_engine(o.factory_class.version,false),bits);
                }
                else throw HostCallFault("XAudio2 工廠方法不合法");
            }else if(name=="Work" || name=="Render" || name=="Complete"){
                require(o.kind==Object::EngineObject);std::vector<GuestCallback> callbacks;
                const auto call=[&](std::uint64_t target,unsigned slot,std::vector<std::uint64_t> values){callbacks.push_back({method(target,slot),std::move(values)});};
                if(!e->active || !e->output){e->busy=false;arg.return_to(state,0);return std::nullopt;}
                if(name=="Work"){
                    // 移除的客體 XAPO：上一個量子排入的 Process 都已執行，這裡 UnlockForProcess／Release；scratch 再下一個量子才釋放。
                    for(auto& g:e->guest_free)if(g->scratch){(void)free_heap(memory,g->scratch,state);g->scratch=0;}
                    e->guest_free.clear();
                    for(auto& g:e->guest_release){
                        if(g->locked && g->xapo)call(g->xapo,9,{g->xapo});
                        if(g->parameters)call(g->parameters,2,{g->parameters});
                        if(g->xapo)call(g->xapo,2,{g->xapo});
                        g->ready=false;e->guest_free.push_back(g);
                    }
                    e->guest_release.clear();
                    e->fraction+=e->rate;const auto frames=e->fraction/100;e->fraction%=100;e->quantum.assign(frames*e->channels,0);e->produced=0;e->pass_voices.clear();
                    for(const auto cb:e->callbacks)call(cb,0,{cb});
                    for(const auto& [p,voice]:bridge.objects)if(voice.engine==e && voice.kind==Object::Voice && voice.callback){const auto info=e->graph->state(voice.voice);if(info.kind==Graph::Kind::Source && info.playing){
                        auto bytes=e->graph->required_bytes(voice.voice,frames,e->rate);
                        if(voice.adpcm){const auto& f=*voice.adpcm;const std::uint64_t decoded_block=f.samples_per_block*f.channels*2;bytes=static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX,((bytes+decoded_block-1)/decoded_block)*f.block_bytes));}
                        e->pass_voices.emplace_back(p,voice.voice);
                        if(e->version==0)call(voice.callback,0,{voice.callback});
                        else call(voice.callback,0,{voice.callback,bytes});
                    }}
                    callbacks.push_back({resolve("internal-xaudio","Render").value,{arg(0),e->ticket}});
                }else if(name=="Render"){
                    const auto began=thread_cpu();e->produced+=e->graph->render_until_event(std::span(e->quantum).subspan(e->produced*e->channels),e->rate,e->channels);e->cpu_ns+=thread_cpu()-began;
                    for(const auto& event:e->graph->take_events()){
                        if(event.kind==Graph::EventKind::CriticalError){e->active=false;e->busy=false;{std::lock_guard lock(e->pcm->mutex);e->pcm->playing=false;}for(const auto cb:e->callbacks)call(cb,2,{cb,0x88960001U});continue;}
                        for(const auto& [p,voice]:bridge.objects)if(voice.engine==e && voice.kind==Object::Voice && voice.voice==event.voice && voice.callback){
                            const unsigned slot=event.kind==Graph::EventKind::BufferStart?3:event.kind==Graph::EventKind::BufferEnd?4:event.kind==Graph::EventKind::LoopEnd?5:2;
                            if(slot==2)call(voice.callback,slot,{voice.callback});else call(voice.callback,slot,{voice.callback,event.context});break;
                        }
                    }
                    if(e->active && e->produced*e->channels<e->quantum.size())callbacks.push_back({resolve("internal-xaudio","Render").value,{arg(0),e->ticket}});
                    else if(e->active){
                        for(const auto& [p,id]:e->pass_voices){const auto found=bridge.objects.find(p);if(found!=bridge.objects.end() && found->second.kind==Object::Voice && found->second.engine==e && found->second.voice==id)call(found->second.callback,1,{found->second.callback});}
                        // 客體 XAPO：本量子累積的效果輸入交給 Process（處理執行緒上依序執行），XapoCollect 把輸出接回語音圖。
                        for(const auto& [p,item]:bridge.objects)if(item.engine==e && item.kind==Object::Voice)for(const auto id:item.chain){
                            const auto found=bridge.objects.find(id);if(found==bridge.objects.end() || !found->second.guest)continue;auto& g=*found->second.guest;
                            auto input=g.block?g.block->take_input():std::vector<float>{};if(!g.ready || g.failed || !g.scratch)continue;
                            const auto frames=static_cast<std::uint32_t>(std::min<std::size_t>(input.size()/g.in_channels,g.max_frames));
                            if(frames)memory.write(GuestAddress{g.input},{reinterpret_cast<const std::uint8_t*>(input.data()),std::size_t{frames}*g.in_channels*4});
                            put(g.scratch+64,g.input,bits);put(g.scratch+64+unit,frames?1U:0U);put(g.scratch+64+unit+4,frames);
                            put(g.scratch+80,g.in_place?g.input:g.output,bits);put(g.scratch+80+unit,0);put(g.scratch+80+unit+4,frames);
                            call(g.xapo,10,{g.xapo,1,g.scratch+64,1,g.scratch+80,e->graph->effect_enabled(item.voice,g.index)?1U:0U});
                            callbacks.push_back({resolve("internal-xaudio","XapoCollect").value,{id,frames}});
                        }
                        for(const auto cb:e->callbacks)call(cb,1,{cb});callbacks.push_back({resolve("internal-xaudio","Complete").value,{arg(0),e->ticket}});
                    }
                }else {
                    std::uint64_t starved=0,runs=0,queued=0;bool report=false;
                    {std::lock_guard lock(e->pcm->mutex);const auto* bytes=reinterpret_cast<const std::uint8_t*>(e->quantum.data());e->pcm->pcm.insert(e->pcm->pcm.end(),bytes,bytes+e->quantum.size()*4);e->rendered+=e->produced;e->busy=false;
                     if(trace_audio()){++e->stats_quanta;const auto t=now();if(!e->stats_wall){e->stats_wall=t;e->stats_starved=e->pcm->starved_frames;e->stats_runs=e->pcm->starved_runs;}
                        else if(t-e->stats_wall>=2000000000ULL){report=true;starved=e->pcm->starved_frames-e->stats_starved;runs=e->pcm->starved_runs-e->stats_runs;queued=e->pcm->pcm.size()/e->pcm->frame_bytes();
                            e->stats_wall=t;e->stats_starved=e->pcm->starved_frames;e->stats_runs=e->pcm->starved_runs;}}}
                    if(report){std::fprintf(stderr,"xaudio_stats pid=%d starved_frames=%llu starved_runs=%llu queued_frames=%llu quanta=%llu unix_ms=%lld\n",static_cast<int>(::getpid()),
                        static_cast<unsigned long long>(starved),static_cast<unsigned long long>(runs),static_cast<unsigned long long>(queued),static_cast<unsigned long long>(e->stats_quanta),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));e->stats_quanta=0;}
                }
                if(!callbacks.empty()){
                    // 回呼內的 resident getter 必須看見同一個 BufferStart／End 邊界。
                    for(const auto& [p,voice]:bridge.objects)if(voice.engine==e)publish(p,voice);
                    if(name=="Render" && !runtime_.callbacks.empty()){
                        auto& frame=runtime_.callbacks.back();
                        // Render 是本量子 callback 批次的尾端。後續音訊事件接回同一個框架，
                        // 讓極短迴圈仍可逐次回呼並觀察狀態，同時避免每個事件增加一次巢狀深度。
                        if(frame.continuation==Continuation::Return && frame.index+1==frame.callbacks.size() &&
                           frame.callbacks[frame.index].target==state.rip.value){
                            for(auto& callback:callbacks){
                                classify_callback(callback,frame.continuation);
                                retain_shell_callback(frame,callback);
                            }
                            frame.callbacks.insert(frame.callbacks.end(),std::make_move_iterator(callbacks.begin()),
                                std::make_move_iterator(callbacks.end()));
                            arg.return_to(state,0);
                            return std::nullopt;
                        }
                    }
                    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
                }
            }else if(o.kind==Object::EngineObject){
                if(name=="GetDeviceCount")put(arg(1),e->devices.size());
                else if(name=="GetDeviceDetails"){
                    require(d(1)<e->devices.size());check(arg(2),1068);std::vector<std::uint8_t> bytes(1068);memory.write(GuestAddress{arg(2)},bytes);const auto& device=e->devices[d(1)];
                    memory.write(GuestAddress{arg(2)},utf16_bytes(device.id.substr(0,255)));memory.write(GuestAddress{arg(2)+512},utf16_bytes(device.device.name.substr(0,255)));put(arg(2)+1024,device.device.default_output?15:0);
                    const auto channels=audio::environment_enabled()?8U:2U;const auto p=arg(2)+1028;put(p,0xfffe,16);put(p+2,channels,16);put(p+4,48000);put(p+8,48000*channels*4);put(p+12,channels*4,16);put(p+14,32,16);put(p+16,22,16);put(p+18,32,16);put(p+20,audio::channel_mask(channels));memory.write(GuestAddress{p+24},std::array<std::uint8_t,16>{3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71});
                }else if(name=="Initialize"){require(!e->initialized && !(d(1)&~1U) && d(2));e->initialized=true;}
                else {require(e->initialized);
                    if(name=="RegisterForCallbacks"){for(unsigned i=0;i<3;++i)(void)method(arg(1),i);e->callbacks.insert(arg(1));}
                    else if(name=="UnregisterForCallbacks")e->callbacks.erase(arg(1));
                    else if(name=="StartEngine" || name=="StopEngine"){e->active=name=="StartEngine";if(!e->active){e->ticket=0;e->busy=false;}e->graph->start(e->active);std::lock_guard lock(e->pcm->mutex);e->pcm->playing=e->active;}
                    else if(name=="CommitChanges"){require(d(1)!=UINT32_MAX);e->graph->commit(d(1));}
                    else if(name=="SetDebugConfiguration"){require(!arg(2));if(arg(1)){std::array<std::uint32_t,6> config{};memory.read(GuestAddress{arg(1)},{reinterpret_cast<std::uint8_t*>(config.data()),24});e->debug=config;}else e->debug={};}
                    else if(name=="GetPerformanceData"){
                        const auto bytes=e->version<=2?60U:64U;
                        check(arg(1),bytes);const std::array<std::uint8_t,64> zero{};
                        memory.write(GuestAddress{arg(1)},std::span(zero).first(bytes));const auto time=now();put(arg(1),e->cpu_ns-e->last_cpu,64);put(arg(1)+8,time-e->last_wall,64);e->last_wall=time;e->last_cpu=e->cpu_ns;
                        unsigned active=0,total=0,submix=0;for(const auto& [p,v]:bridge.objects)if(v.kind==Object::Voice && v.engine==e){const auto vs=e->graph->state(v.voice);if(vs.kind==Graph::Kind::Source){++total;if(vs.playing)++active;}else if(vs.kind==Graph::Kind::Submix)++submix;}
                        std::lock_guard lock(e->pcm->mutex);
                        put(arg(1)+24,e->pcm->pcm.size());put(arg(1)+28,e->pcm->pcm.size()/e->pcm->frame_bytes());
                        put(arg(1)+32,e->version==0?e->pcm->starved_runs-e->last_perf_starved_runs:e->pcm->starved_runs);
                        e->last_perf_starved_runs=e->pcm->starved_runs;
                        put(arg(1)+36,active);put(arg(1)+40,total);put(arg(1)+44,submix);
                        if(e->version<=2)put(arg(1)+48,submix);
                    }else if(name=="CreateSourceVoice" || name=="CreateSubmixVoice" || name.starts_with("CreateMasteringVoice")){
                        check(arg(1),unit);put(arg(1),0,bits);Graph::Format format;Graph::Kind kind;unsigned flags=0,stage=0;float ratio=2;std::uint64_t callback=0,send=0,effects=0;std::string device;std::optional<audio::AdpcmFormat> adpcm;
                        if(name=="CreateSourceVoice"){
                        const auto max_rate=e->version<=7?200000U:384000U;
                            if(get(arg(2),0,16)==2){
                                std::array<std::uint8_t,50> encoded{};require(get(arg(2),16,16)==32);memory.read(GuestAddress{arg(2)},encoded);adpcm=audio::parse_adpcm_format(encoded);
                                require(adpcm && adpcm->rate>=1000 && adpcm->rate<=max_rate && adpcm->samples_per_block>=32 && adpcm->samples_per_block<=512 && std::ranges::equal(adpcm->coefficients,audio::adpcm_coefficients));
                                format={adpcm->channels,adpcm->rate,16,false};
                            }else {const auto parsed=read_pcm_wave_format(memory,arg(2),true,true,1000,max_rate,{64,32,true,true});require(bool(parsed));format={parsed->channels,parsed->rate,parsed->bits,parsed->floating,parsed->channel_mask};}
                            kind=Graph::Kind::Source;flags=d(3);ratio=fp(4);callback=arg(5);send=arg(6);effects=arg(7);if(callback)for(unsigned i=0;i<7;++i)(void)method(callback,i);
                        }else if(name=="CreateSubmixVoice"){format={d(2),d(3),32,true};kind=Graph::Kind::Submix;flags=d(4);stage=d(5);send=arg(6);effects=arg(7);}
                        else {require(!e->master && !d(4));format={d(2)?d(2):(audio::environment_enabled()?8U:2U),d(3)?d(3):48000U,32,true};kind=Graph::Kind::Master;effects=arg(6);
                            if(e->version<=7){require(d(5)<e->devices.size());device=e->devices[d(5)].device.uid;}
                            else {if(arg(5)){const auto id=read_utf16(memory,GuestAddress{arg(5)});const auto i=std::find_if(e->devices.begin(),e->devices.end(),[&](const auto& v){return v.id==id;});require(i!=e->devices.end());device=i->device.uid;}require(d(7)<=11);}
                        }
                        require(format.rate<=(e->version<=7?200000U:384000U));
                        if(kind==Graph::Kind::Master)require(format.rate%100==0);
                        // 效果在混音取樣率上逐幀處理（語音圖先重取樣再套效果）。
                        const auto effect_rate=kind==Graph::Kind::Master?format.rate:e->rate;const auto chain=read_chain(effects,format.channels,effect_rate,0);
                        const auto id=e->graph->create(kind,format,flags,stage,ratio);std::uint64_t p=0;
                        try {
                            if(kind!=Graph::Kind::Master)e->graph->set_sends(id,sends(e,send));
                            p=create({Object::Voice,e,id,callback});
                            bridge.objects.at(p).adpcm=std::move(adpcm);
                            if(!chain.empty())set_chain(p,chain,effect_rate,format.channels);
                            if(kind==Graph::Kind::Master){(void)mapping_session();e->pcm->progress_notify=[wake=mapping_wake()]()noexcept{if(wake)wake->notify();};e->pcm->rate=e->pcm->frequency=e->rate=format.rate;e->pcm->channels=e->channels=format.channels;e->pcm->channel_mask=format.channel_mask;e->pcm->bits=32;e->pcm->floating=e->pcm->streaming=true;e->pcm->playing=e->active;e->pcm->pcm.reserve(e->rate*e->pcm->frame_bytes()/10);e->output=audio::open_output(e->pcm,device);require(bool(e->output));e->master=p;}
                            put(arg(1),p,bits);
                        }catch(...){if(p){drop_chain(object(p));bridge.objects.erase(p);(void)free_heap(memory,p,state);}e->graph->destroy(id);throw;}
                    }else throw HostCallFault("尚未實作 XAudio2 引擎方法："+std::string(name));
                }
            }else {
                auto info=e->graph->state(o.voice);Graph::Change c;c.voice=o.voice;
                const auto dest=[&](std::uint64_t p){if(!p)p=e->master;auto& target=object(p);require(target.kind==Object::Voice && target.engine==e);return target.voice;};
                const auto filter=[&](std::uint64_t p){return Graph::Filter{static_cast<unsigned>(get(p)),std::bit_cast<float>(static_cast<std::uint32_t>(get(p,4))),std::bit_cast<float>(static_cast<std::uint32_t>(get(p,8)))};};
                const auto store_filter=[&](std::uint64_t p,Graph::Filter f){check(p,12);put(p,f.type);put(p+4,std::bit_cast<std::uint32_t>(f.frequency));put(p+8,std::bit_cast<std::uint32_t>(f.one_over_q));};
                if(name=="GetVoiceDetails"){check(arg(1),e->version<=7?12:16);put(arg(1),info.creation_flags);if(e->version>=8)put(arg(1)+4,info.flags);put(arg(1)+(e->version<=7?4:8),info.format.channels);put(arg(1)+(e->version<=7?8:12),info.format.rate);}
                else if(name=="GetChannelMask")put(arg(1),info.format.channel_mask?info.format.channel_mask:audio::channel_mask(info.format.channels));
                else if(name=="SetOutputVoices")e->graph->set_sends(o.voice,sends(e,arg(1)));
                else if(name=="SetEffectChain"){const auto rate=e->master?e->rate:info.format.rate;set_chain(arg(0),read_chain(arg(1),info.format.channels,rate,arg(0)),rate,info.format.channels);}
                else if(name=="EnableEffect" || name=="DisableEffect"){require(d(1)<o.chain.size() && d(2)!=UINT32_MAX);c.operation=name=="EnableEffect"?Graph::Operation::EnableEffect:Graph::Operation::DisableEffect;c.effect=d(1);e->graph->change(std::move(c),d(2));}
                else if(name=="GetEffectState"){check(arg(2),4);put(arg(2),d(1)<o.chain.size() && e->graph->effect_enabled(o.voice,d(1))?1:0);}
                else if(name=="SetEffectParameters" || name=="GetEffectParameters"){
                    // 參數直接寫入效果物件，與 IXAPOParameters 共用同一份；OperationSet 不延後（立即生效）。
                    require(d(1)<o.chain.size());auto& entry=object(o.chain[d(1)]);
                    if(entry.guest){
                        // 客體 XAPO：在呼叫端返回前轉給它的 IXAPOParameters::SetParameters／GetParameters（OperationSet 同樣立即生效）。
                        const auto& g=*entry.guest;require(g.parameters && arg(2) && d(3));
                        if(name=="SetEffectParameters")require(d(4)!=UINT32_MAX);
                        guest_calls.push_back({method(g.parameters,name=="SetEffectParameters"?3:4),{g.parameters,arg(2),d(3)}});
                    }else if(auto& fx=*entry.fx;fx.builtin==Builtin::Reverb){
                        require(arg(2) && fx.parameter_size(d(3)));
                        if(name=="SetEffectParameters"){require(d(4)!=UINT32_MAX);std::vector<std::uint8_t> bytes(d(3));memory.read(GuestAddress{arg(2)},bytes);fx.reverb().set(parse_reverb(bytes));}
                        else {const auto bytes=serialize_reverb(fx.reverb().parameters(),d(3));check(arg(2),bytes.size());memory.write(GuestAddress{arg(2)},bytes);}
                    }else if(fx.builtin==Builtin::Meter){
                        // volume meter 只能讀取；ChannelCount 必須等於該效果處理的聲道數。
                        require(name=="GetEffectParameters" && arg(2) && d(3)==levels_bytes && write_levels(fx,arg(2)));
                    }else {
                        const auto count=fx.words().size();require(arg(2) && d(3)==count*4);
                        if(name=="SetEffectParameters"){require(d(4)!=UINT32_MAX);fx.set(words_from(arg(2),count));}else words_to(arg(2),fx.words());
                    }
                }
                else if(name=="GetVolume")put(arg(1),std::bit_cast<std::uint32_t>(info.volume));
                else if(name=="GetFrequencyRatio")put(arg(1),std::bit_cast<std::uint32_t>(info.ratio));
                else if(name=="SetVolume" || name=="SetFrequencyRatio"){c.operation=name=="SetVolume"?Graph::Operation::Volume:Graph::Operation::Frequency;c.value=fp(1);require(d(2)!=UINT32_MAX);e->graph->change(std::move(c),d(2));}
                else if(name=="SetChannelVolumes"){require(d(1)==info.output_channels);c.operation=Graph::Operation::ChannelVolumes;c.values=floats(arg(2),d(1));e->graph->change(std::move(c),d(3));}
                else if(name=="GetChannelVolumes"){require(d(1)==info.output_channels);check(arg(2),d(1)*4);memory.write(GuestAddress{arg(2)},{reinterpret_cast<const std::uint8_t*>(info.channel_volumes.data()),d(1)*4U});}
                else if(name=="SetOutputMatrix" || name=="GetOutputMatrix" || name=="GetOutputMatrix20"){
                    c.destination=dest(arg(1));const auto target=e->graph->state(c.destination);require(d(2)==info.output_channels && d(3)==target.format.channels);
                    if(name=="SetOutputMatrix"){c.operation=Graph::Operation::Matrix;c.values=floats(arg(4),d(2)*d(3));e->graph->change(std::move(c),d(5));}
                    else {const auto v=e->graph->matrix(o.voice,c.destination);check(arg(4),v.size()*4);memory.write(GuestAddress{arg(4)},{reinterpret_cast<const std::uint8_t*>(v.data()),v.size()*4});}
                }else if(name=="SetFilterParameters" || name=="SetOutputFilterParameters"){const bool output=name=="SetOutputFilterParameters";c.operation=output?Graph::Operation::OutputFilter:Graph::Operation::Filter;if(output)c.destination=dest(arg(1));c.filter=filter(arg(output?2:1));e->graph->change(std::move(c),d(output?3:2));}
                else if(name=="GetFilterParameters"){require(info.flags&8U);store_filter(arg(1),info.filter);}
                else if(name=="GetOutputFilterParameters")store_filter(arg(2),e->graph->output_filter(o.voice,dest(arg(1))));
                else if(name=="Start" || name=="Stop"){require(!(d(1)&~(name=="Stop"?32U:0U)));c.operation=name=="Start"?Graph::Operation::Start:Graph::Operation::Stop;c.tails=d(1)!=0;e->graph->change(std::move(c),d(2));}
                else if(name=="SubmitSourceBuffer"){
                    require(!arg(2));const auto p=arg(1);Graph::Packet packet;const auto flags=get(p),size=get(p,4),data=get(p,8,bits);require(!(flags&~64U) && size && size<=0x80000000ULL && data);packet.end_stream=(flags&64U)!=0;const unsigned fields=8+unit;
                    packet.play_begin=static_cast<unsigned>(get(p,fields));packet.play_length=static_cast<unsigned>(get(p,fields+4));packet.loop_begin=static_cast<unsigned>(get(p,fields+8));packet.loop_length=static_cast<unsigned>(get(p,fields+12));packet.loop_count=static_cast<unsigned>(get(p,fields+16));packet.context=get(p,fields+20,bits);
                    if(o.adpcm)require(packet.play_length%o.adpcm->samples_per_block==0 && packet.loop_length%o.adpcm->samples_per_block==0);
                    memory.check(GuestAddress{data},size,Access::Read);packet.pcm.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{data},packet.pcm);
                    if(o.adpcm)packet.pcm=audio::decode_adpcm(packet.pcm,*o.adpcm);
                    packet.legacy_loop_count=e->version==0;
                    e->graph->submit(o.voice,std::move(packet));
                }else if(name=="FlushSourceBuffers")e->graph->flush(o.voice);
                else if(name=="ExitLoop"){c.operation=Graph::Operation::ExitLoop;e->graph->change(std::move(c),d(1));}
                else if(name=="Discontinuity")e->graph->discontinuity(o.voice);
                else if(name=="GetState27" || name=="GetState28"){if(name=="GetState28")require(!(d(2)&~256U));check(arg(1),unit+12);put(arg(1),info.context,bits);put(arg(1)+unit,info.queued);put(arg(1)+unit+4,info.samples,64);}
                else if(name=="SetSourceSampleRate"){require(d(1)<=(e->version<=7?200000U:384000U));e->graph->set_sample_rate(o.voice,d(1));}
                else if(name=="DestroyVoice"){e->graph->destroy(o.voice);drop_chain(o);if(arg(0)==e->master){e->output.reset();e->master=0;}bridge.objects.erase(arg(0));(void)free_heap(memory,arg(0),state);}
                else throw HostCallFault("尚未實作 XAudio2 音源方法："+std::string(name));
            }
        }
    }catch(const MemoryFault&){if(void_method(name))throw;result=pointer_error;}catch(const std::invalid_argument&){result=invalid;}catch(const std::bad_alloc&){result=0x8007000eU;}
    if(trace_audio() && ((result&0x80000000U) || name=="CoCreateInstance" || name=="CreateInstance" || name=="XAudio2Create" || name=="Initialize" || name.starts_with("CreateMasteringVoice")))
        std::fprintf(stderr,"xaudio_call pid=%d method=%.*s result=0x%08x\n",static_cast<int>(::getpid()),static_cast<int>(name.size()),name.data(),static_cast<unsigned>(result));
    for(const auto& [p,o]:bridge.objects)publish(p,o);
    poll_xaudio();
    if(!bridge.guest_setup.empty() || !guest_calls.empty()){
        std::vector<GuestCallback> calls;if(!(result&0x80000000U))calls=std::move(guest_calls);
        for(const auto id:std::exchange(bridge.guest_setup,{}))if(bridge.objects.contains(id))xapo_setup_calls(id,calls);
        if(!calls.empty()){start_callbacks(state,memory,std::move(calls),Continuation::Return,result);return std::nullopt;}
    }
    arg.return_to(state,result);return std::nullopt;
}
}
