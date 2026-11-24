// DirectShow 濾鏡圖（quartz.dll 的 CLSID_FilterGraph／CLSID_FilterGraphNoThread）與音訊轉譯濾鏡（CLSID_DSoundRender／CLSID_AudioRender）。
// - 濾鏡圖是宿主物件，一個客體配置區塊內依序放 IGraphBuilder（IFilterGraph2）、IMediaControl、IMediaEventEx、IMediaSeeking、
//   IBasicAudio、IMediaPosition 六個介面指標；各介面共用同一個參考計數。
// - RenderFile：檔案以 MCI／VFW 共用的預覽解碼器開啟（ASF／WMA／WMV、MPEG／MP3、MP4、AVI）或 RIFF WAVE，音訊解碼成 PCM 後交給共用音訊輸出；
//   視訊串流目前不轉譯（回報 VFW_S_VIDEO_NOT_RENDERED）。圖中加入來源濾鏡與音訊轉譯濾鏡兩個宿主濾鏡，EnumFilters／FindFilterByName 可列出。
// - 播放到結尾或停止位置時排入 EC_COMPLETE：事件 handle 設為有信號，SetNotifyWindow 指定的視窗收到通知訊息（lParam＝lInstanceData）。
// - 客體自己實作的濾鏡可以 AddFilter／RemoveFilter（呼叫其 JoinFilterGraph、AddRef／Release），但沒有 pin 連接模型：Connect／Render(pin) 回 VFW_E_CANNOT_CONNECT／VFW_E_CANNOT_RENDER。
// - 時間格式只支援 TIME_FORMAT_MEDIA_TIME（100 ns）；IMediaPosition 以秒為單位的 REFTIME。
#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/audio.hpp"
#include "mp4_preview.hpp"
#include "mpeg_preview.hpp"
#include "avi_preview.hpp"
#include "mci_avi.hpp"
#include "pcm_wave_file.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid make_guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    Guid g{};for(unsigned i=0;i<4;++i)g[i]=static_cast<std::uint8_t>(a>>(8*i));
    for(unsigned i=0;i<2;++i){g[4+i]=static_cast<std::uint8_t>(b>>(8*i));g[6+i]=static_cast<std::uint8_t>(c>>(8*i));}
    for(unsigned i=0;i<8;++i)g[8+i]=d[i];return g;
}
constexpr std::array<std::uint8_t,8> quartz_tail{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70};
constexpr Guid quartz_iid(std::uint32_t a){return make_guid(a,0x0ad4,0x11ce,quartz_tail);}
constexpr Guid iid_unknown=make_guid(0,0,0,{0xc0,0,0,0,0,0,0,0x46}),iid_dispatch=make_guid(0x00020400,0,0,{0xc0,0,0,0,0,0,0,0x46}),
    iid_class_factory=make_guid(1,0,0,{0xc0,0,0,0,0,0,0,0x46}),iid_persist=make_guid(0x10c,0,0,{0xc0,0,0,0,0,0,0,0x46});
constexpr Guid iid_filter_graph=quartz_iid(0x56a8689f),iid_graph_builder=quartz_iid(0x56a868a9),iid_media_control=quartz_iid(0x56a868b1),
    iid_media_position=quartz_iid(0x56a868b2),iid_basic_audio=quartz_iid(0x56a868b3),iid_media_event=quartz_iid(0x56a868b6),
    iid_media_event_ex=quartz_iid(0x56a868c0),iid_base_filter=quartz_iid(0x56a86895),iid_media_filter=quartz_iid(0x56a86899),
    iid_enum_filters=quartz_iid(0x56a86893),iid_video_window=quartz_iid(0x56a868b4),iid_basic_video=quartz_iid(0x56a868b5);
constexpr Guid iid_basic_video2=make_guid(0x329bb360,0xf6ea,0x11d1,{0x90,0x38,0x00,0xa0,0xc9,0x69,0x72,0x98});
constexpr Guid iid_am_direct_sound=make_guid(0x546f4260,0xd53e,0x11cf,{0xb3,0xf0,0x00,0xaa,0x00,0x37,0x61,0xc5});
constexpr Guid iid_filter_graph2=make_guid(0x36b73882,0xc2c8,0x11cf,{0x8b,0x46,0x00,0x80,0x5f,0x6c,0xef,0x60}),
    iid_media_seeking=make_guid(0x36b73880,0xc2c8,0x11cf,{0x8b,0x46,0x00,0x80,0x5f,0x6c,0xef,0x60});
constexpr Guid clsid_filter_graph=make_guid(0xe436ebb3,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}),
    clsid_filter_graph_no_thread=make_guid(0xe436ebb8,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}),
    clsid_async_reader=make_guid(0xe436ebb5,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}),
    clsid_dsound_render=make_guid(0x79376820,0x07d0,0x11cf,{0xa2,0x4d,0x00,0x20,0xaf,0xd7,0x97,0x67}),
    clsid_audio_render=make_guid(0xe30629d1,0x27e5,0x11ce,{0x87,0x5d,0x00,0x60,0x8c,0xb7,0x80,0x66});
constexpr Guid time_format_media_time=make_guid(0x7b785574,0x8c82,0x11cf,{0xbc,0x0c,0x00,0xaa,0x00,0xac,0x74,0xf6});
constexpr Guid time_format_none{};
constexpr std::uint32_t s_ok=0,s_false=1,e_notimpl=0x80004001U,e_nointerface=0x80004002U,e_pointer=0x80004003U,e_abort=0x80004004U,e_fail=0x80004005U,
    e_unexpected=0x8000ffffU,e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,class_e_noaggregation=0x80040110U,
    vfw_e_not_found=0x80040216U,vfw_e_cannot_connect=0x80040217U,vfw_e_cannot_render=0x80040218U,vfw_e_unknown_file_type=0x80040240U,
    vfw_e_unsupported_stream=0x80040265U,vfw_e_wrong_state=0x80040227U,vfw_e_not_stopped=0x80040224U,vfw_e_duplicate_name=0x8004022dU,
    vfw_s_duplicate_name=0x0004022dU,vfw_e_not_connected=0x80040209U,vfw_s_video_not_rendered=0x00040257U,
    disp_e_unknownname=0x80020006U,disp_e_membernotfound=0x80020003U,file_not_found=0x80070002U;
constexpr std::uint32_t ec_complete=1;
enum Interface : unsigned {graph_interface,control_interface,event_interface,seeking_interface,audio_interface,position_interface,video_window_interface,
    basic_video_interface,graph_interfaces};
constexpr std::array<std::string_view,graph_interfaces> graph_families{"internal-dshow-graph","internal-dshow-control","internal-dshow-event",
    "internal-dshow-seeking","internal-dshow-audio","internal-dshow-position","internal-dshow-videowindow","internal-dshow-basicvideo"};
}

// 濾鏡：宿主建立的來源或音訊轉譯濾鏡（[IBaseFilter][IBasicAudio][IAMDirectSound] 三個介面指標）。
struct DirectShowFilter {
    Guid clsid{};std::uint32_t references{1};std::uint64_t graph{};std::u16string name;std::uint32_t state{};
    long volume{},balance{};
    std::uint64_t focus_window{};bool focus_mixing{}; // IAMDirectSound::SetFocusWindow
};
struct DirectShowGraph {
    std::uint32_t references{1};
    std::vector<std::uint64_t> filters;          // IBaseFilter 指標（宿主或客體濾鏡），圖持有一個參考。
    std::vector<std::u16string> names;
    std::u16string file;std::uint64_t stream_source{}; // 已轉譯串流的來源濾鏡（RenderFile 建立的非同步讀取器）。
    std::shared_ptr<audio::Buffer> buffer;std::unique_ptr<audio::Output> output;
    std::uint64_t frames{};unsigned rate{};       // 解碼後的音訊長度（frame）與取樣率。
    std::uint64_t duration{};                    // 100 ns。
    std::uint32_t state{};                       // State_Stopped 0、State_Paused 1、State_Running 2。
    std::optional<std::uint64_t> stop;           // 停止位置（100 ns）；未設定為結尾。
    double speed{1};
    long volume{},balance{};
    std::uint64_t notify_window{},notify_message{},notify_instance{};std::uint32_t notify_flags{};
    std::deque<std::array<std::uint64_t,3>> events;std::uint64_t event_handle{},completion_handle{};
    bool completed{};
    // 視訊：以內部 MCI 影片裝置播放（共用解碼、時間軸、聲音與自動繪製）。IVideoWindow 的擁有者視窗與位置對應到裝置的繪製視窗與目的矩形；
    // 沒有擁有者時於第一次執行建立頂層視窗。
    std::uint32_t video{};unsigned video_width{},video_height{};std::uint64_t frame_time{};
    std::uint64_t owner{},own_window{},message_drain{};
    std::int32_t left{},top{},width{},height{};bool positioned{};
    std::uint32_t window_style{0x06cf0000U},window_style_ex{},window_state{1},border_color{},background_palette{1};
    bool visible{true},auto_show{true},cursor_hidden{},fullscreen{};
    std::u16string caption{u"ActiveMovie Window"};
    GuiRect source{},destination{};bool default_source{true},default_destination{true};
};
struct DirectShowEnum {std::uint32_t references{1};std::vector<std::uint64_t> filters;std::size_t cursor{};};
struct DirectShowState {
    std::map<std::uint64_t,DirectShowGraph> graphs;   // 鍵為客體物件位址（第一個介面指標）。
    std::map<std::uint64_t,DirectShowFilter> filters;
    std::map<std::uint64_t,DirectShowEnum> enums;
    std::map<std::uint64_t,std::pair<Guid,std::uint32_t>> factories;
    std::map<std::string,std::uint64_t> vtables;
    std::uint64_t video_class{};  // 已登錄「VideoRenderer」視窗類別的 atom
};

std::span<const WindowsServiceDescriptor> directshow_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"internal-dshow-graph","QueryInterface",3},{"internal-dshow-graph","AddRef",1},{"internal-dshow-graph","Release",1},
        {"internal-dshow-graph","AddFilter",3},{"internal-dshow-graph","RemoveFilter",2},{"internal-dshow-graph","EnumFilters",2},
        {"internal-dshow-graph","FindFilterByName",3},{"internal-dshow-graph","ConnectDirect",4},{"internal-dshow-graph","Reconnect",2},
        {"internal-dshow-graph","Disconnect",2},{"internal-dshow-graph","SetDefaultSyncSource",1},{"internal-dshow-graph","Connect",3},
        {"internal-dshow-graph","Render",2},{"internal-dshow-graph","RenderFile",3},{"internal-dshow-graph","AddSourceFilter",4},
        {"internal-dshow-graph","SetLogFile",2},{"internal-dshow-graph","Abort",1},{"internal-dshow-graph","ShouldOperationContinue",1},
        {"internal-dshow-graph","AddSourceFilterForMoniker",5},{"internal-dshow-graph","ReconnectEx",3},{"internal-dshow-graph","RenderEx",4},
        {"internal-dshow-control","QueryInterface",3},{"internal-dshow-control","AddRef",1},{"internal-dshow-control","Release",1},
        {"internal-dshow-control","GetTypeInfoCount",2},{"internal-dshow-control","GetTypeInfo",4},{"internal-dshow-control","GetIDsOfNames",6},
        {"internal-dshow-control","Invoke",9},{"internal-dshow-control","Run",1},{"internal-dshow-control","Pause",1},{"internal-dshow-control","Stop",1},
        {"internal-dshow-control","GetState",3},{"internal-dshow-control","RenderFile",2},{"internal-dshow-control","AddSourceFilter",3},
        {"internal-dshow-control","get_FilterCollection",2},{"internal-dshow-control","get_RegFilterCollection",2},{"internal-dshow-control","StopWhenReady",1},
        {"internal-dshow-event","QueryInterface",3},{"internal-dshow-event","AddRef",1},{"internal-dshow-event","Release",1},
        {"internal-dshow-event","GetTypeInfoCount",2},{"internal-dshow-event","GetTypeInfo",4},{"internal-dshow-event","GetIDsOfNames",6},
        {"internal-dshow-event","Invoke",9},{"internal-dshow-event","GetEventHandle",2},{"internal-dshow-event","GetEvent",5},
        {"internal-dshow-event","WaitForCompletion",3},{"internal-dshow-event","CancelDefaultHandling",2},{"internal-dshow-event","RestoreDefaultHandling",2},
        {"internal-dshow-event","FreeEventParams",4},{"internal-dshow-event","SetNotifyWindow",4},{"internal-dshow-event","SetNotifyFlags",2},
        {"internal-dshow-event","GetNotifyFlags",2},
        {"internal-dshow-seeking","QueryInterface",3},{"internal-dshow-seeking","AddRef",1},{"internal-dshow-seeking","Release",1},
        {"internal-dshow-seeking","GetCapabilities",2},{"internal-dshow-seeking","CheckCapabilities",2},{"internal-dshow-seeking","IsFormatSupported",2},
        {"internal-dshow-seeking","QueryPreferredFormat",2},{"internal-dshow-seeking","GetTimeFormat",2},{"internal-dshow-seeking","IsUsingTimeFormat",2},
        {"internal-dshow-seeking","SetTimeFormat",2},{"internal-dshow-seeking","GetDuration",2},{"internal-dshow-seeking","GetStopPosition",2},
        {"internal-dshow-seeking","GetCurrentPosition",2},{"internal-dshow-seeking","ConvertTimeFormat",5,0x8},{"internal-dshow-seeking","SetPositions",5},
        {"internal-dshow-seeking","GetPositions",3},{"internal-dshow-seeking","GetAvailable",3},{"internal-dshow-seeking","SetRate",2,0x2},
        {"internal-dshow-seeking","GetRate",2},{"internal-dshow-seeking","GetPreroll",2},
        {"internal-dshow-audio","QueryInterface",3},{"internal-dshow-audio","AddRef",1},{"internal-dshow-audio","Release",1},
        {"internal-dshow-audio","GetTypeInfoCount",2},{"internal-dshow-audio","GetTypeInfo",4},{"internal-dshow-audio","GetIDsOfNames",6},
        {"internal-dshow-audio","Invoke",9},{"internal-dshow-audio","put_Volume",2},{"internal-dshow-audio","get_Volume",2},
        {"internal-dshow-audio","put_Balance",2},{"internal-dshow-audio","get_Balance",2},
        {"internal-dshow-position","QueryInterface",3},{"internal-dshow-position","AddRef",1},{"internal-dshow-position","Release",1},
        {"internal-dshow-position","GetTypeInfoCount",2},{"internal-dshow-position","GetTypeInfo",4},{"internal-dshow-position","GetIDsOfNames",6},
        {"internal-dshow-position","Invoke",9},{"internal-dshow-position","get_Duration",2},{"internal-dshow-position","put_CurrentPosition",2,0x2},
        {"internal-dshow-position","get_CurrentPosition",2},{"internal-dshow-position","get_StopTime",2},{"internal-dshow-position","put_StopTime",2,0x2},
        {"internal-dshow-position","get_PrerollTime",2},{"internal-dshow-position","put_PrerollTime",2,0x2},{"internal-dshow-position","put_Rate",2,0x2},
        {"internal-dshow-position","get_Rate",2},{"internal-dshow-position","CanSeekForward",2},{"internal-dshow-position","CanSeekBackward",2},
        {"internal-dshow-videowindow","QueryInterface",3},{"internal-dshow-videowindow","AddRef",1},{"internal-dshow-videowindow","Release",1},
        {"internal-dshow-videowindow","GetTypeInfoCount",2},{"internal-dshow-videowindow","GetTypeInfo",4},{"internal-dshow-videowindow","GetIDsOfNames",6},
        {"internal-dshow-videowindow","Invoke",9},{"internal-dshow-videowindow","put_Caption",2},{"internal-dshow-videowindow","get_Caption",2},
        {"internal-dshow-videowindow","put_WindowStyle",2},{"internal-dshow-videowindow","get_WindowStyle",2},{"internal-dshow-videowindow","put_WindowStyleEx",2},
        {"internal-dshow-videowindow","get_WindowStyleEx",2},{"internal-dshow-videowindow","put_AutoShow",2},{"internal-dshow-videowindow","get_AutoShow",2},
        {"internal-dshow-videowindow","put_WindowState",2},{"internal-dshow-videowindow","get_WindowState",2},{"internal-dshow-videowindow","put_BackgroundPalette",2},
        {"internal-dshow-videowindow","get_BackgroundPalette",2},{"internal-dshow-videowindow","put_Visible",2},{"internal-dshow-videowindow","get_Visible",2},
        {"internal-dshow-videowindow","put_Left",2},{"internal-dshow-videowindow","get_Left",2},{"internal-dshow-videowindow","put_Width",2},
        {"internal-dshow-videowindow","get_Width",2},{"internal-dshow-videowindow","put_Top",2},{"internal-dshow-videowindow","get_Top",2},
        {"internal-dshow-videowindow","put_Height",2},{"internal-dshow-videowindow","get_Height",2},{"internal-dshow-videowindow","put_Owner",2},
        {"internal-dshow-videowindow","get_Owner",2},{"internal-dshow-videowindow","put_MessageDrain",2},{"internal-dshow-videowindow","get_MessageDrain",2},
        {"internal-dshow-videowindow","get_BorderColor",2},{"internal-dshow-videowindow","put_BorderColor",2},{"internal-dshow-videowindow","get_FullScreenMode",2},
        {"internal-dshow-videowindow","put_FullScreenMode",2},{"internal-dshow-videowindow","SetWindowForeground",2},{"internal-dshow-videowindow","NotifyOwnerMessage",5},
        {"internal-dshow-videowindow","SetWindowPosition",5},{"internal-dshow-videowindow","GetWindowPosition",5},{"internal-dshow-videowindow","GetMinIdealImageSize",3},
        {"internal-dshow-videowindow","GetMaxIdealImageSize",3},{"internal-dshow-videowindow","GetRestorePosition",5},{"internal-dshow-videowindow","HideCursor",2},
        {"internal-dshow-videowindow","IsCursorHidden",2},
        {"internal-dshow-basicvideo","QueryInterface",3},{"internal-dshow-basicvideo","AddRef",1},{"internal-dshow-basicvideo","Release",1},
        {"internal-dshow-basicvideo","GetTypeInfoCount",2},{"internal-dshow-basicvideo","GetTypeInfo",4},{"internal-dshow-basicvideo","GetIDsOfNames",6},
        {"internal-dshow-basicvideo","Invoke",9},{"internal-dshow-basicvideo","get_AvgTimePerFrame",2},{"internal-dshow-basicvideo","get_BitRate",2},
        {"internal-dshow-basicvideo","get_BitErrorRate",2},{"internal-dshow-basicvideo","get_VideoWidth",2},{"internal-dshow-basicvideo","get_VideoHeight",2},
        {"internal-dshow-basicvideo","put_SourceLeft",2},{"internal-dshow-basicvideo","get_SourceLeft",2},{"internal-dshow-basicvideo","put_SourceWidth",2},
        {"internal-dshow-basicvideo","get_SourceWidth",2},{"internal-dshow-basicvideo","put_SourceTop",2},{"internal-dshow-basicvideo","get_SourceTop",2},
        {"internal-dshow-basicvideo","put_SourceHeight",2},{"internal-dshow-basicvideo","get_SourceHeight",2},{"internal-dshow-basicvideo","put_DestinationLeft",2},
        {"internal-dshow-basicvideo","get_DestinationLeft",2},{"internal-dshow-basicvideo","put_DestinationWidth",2},{"internal-dshow-basicvideo","get_DestinationWidth",2},
        {"internal-dshow-basicvideo","put_DestinationTop",2},{"internal-dshow-basicvideo","get_DestinationTop",2},{"internal-dshow-basicvideo","put_DestinationHeight",2},
        {"internal-dshow-basicvideo","get_DestinationHeight",2},{"internal-dshow-basicvideo","SetSourcePosition",5},{"internal-dshow-basicvideo","GetSourcePosition",5},
        {"internal-dshow-basicvideo","SetDefaultSourcePosition",1},{"internal-dshow-basicvideo","SetDestinationPosition",5},{"internal-dshow-basicvideo","GetDestinationPosition",5},
        {"internal-dshow-basicvideo","SetDefaultDestinationPosition",1},{"internal-dshow-basicvideo","GetVideoSize",3},{"internal-dshow-basicvideo","GetVideoPaletteEntries",5},
        {"internal-dshow-basicvideo","GetCurrentImage",3},{"internal-dshow-basicvideo","IsUsingDefaultSource",1},{"internal-dshow-basicvideo","IsUsingDefaultDestination",1},
        {"internal-dshow-filter","QueryInterface",3},{"internal-dshow-filter","AddRef",1},{"internal-dshow-filter","Release",1},
        {"internal-dshow-filter","GetClassID",2},{"internal-dshow-filter","Stop",1},{"internal-dshow-filter","Pause",1},{"internal-dshow-filter","Run",2,0x2},
        {"internal-dshow-filter","GetState",3},{"internal-dshow-filter","SetSyncSource",2},{"internal-dshow-filter","GetSyncSource",2},
        {"internal-dshow-filter","EnumPins",2},{"internal-dshow-filter","FindPin",3},{"internal-dshow-filter","QueryFilterInfo",2},
        {"internal-dshow-filter","JoinFilterGraph",3},{"internal-dshow-filter","QueryVendorInfo",2},
        {"internal-dshow-filter-audio","QueryInterface",3},{"internal-dshow-filter-audio","AddRef",1},{"internal-dshow-filter-audio","Release",1},
        {"internal-dshow-filter-audio","GetTypeInfoCount",2},{"internal-dshow-filter-audio","GetTypeInfo",4},{"internal-dshow-filter-audio","GetIDsOfNames",6},
        {"internal-dshow-filter-audio","Invoke",9},{"internal-dshow-filter-audio","put_Volume",2},{"internal-dshow-filter-audio","get_Volume",2},
        {"internal-dshow-filter-audio","put_Balance",2},{"internal-dshow-filter-audio","get_Balance",2},
        {"internal-dshow-filter-amds","QueryInterface",3},{"internal-dshow-filter-amds","AddRef",1},{"internal-dshow-filter-amds","Release",1},
        {"internal-dshow-filter-amds","GetDirectSoundInterface",2},{"internal-dshow-filter-amds","GetPrimaryBufferInterface",2},
        {"internal-dshow-filter-amds","GetSecondaryBufferInterface",2},{"internal-dshow-filter-amds","ReleaseDirectSoundInterface",2},
        {"internal-dshow-filter-amds","ReleasePrimaryBufferInterface",2},{"internal-dshow-filter-amds","ReleaseSecondaryBufferInterface",2},
        {"internal-dshow-filter-amds","SetFocusWindow",3},{"internal-dshow-filter-amds","GetFocusWindow",3},
        {"internal-dshow-enum","QueryInterface",3},{"internal-dshow-enum","AddRef",1},{"internal-dshow-enum","Release",1},
        {"internal-dshow-enum","Next",4},{"internal-dshow-enum","Skip",2},{"internal-dshow-enum","Reset",1},{"internal-dshow-enum","Clone",2},
        {"internal-dshow-factory","QueryInterface",3},{"internal-dshow-factory","AddRef",1},{"internal-dshow-factory","Release",1},
        {"internal-dshow-factory","CreateInstance",4},{"internal-dshow-factory","LockServer",2}};
    return table;
}

namespace {
bool directshow_class(const Guid& clsid){
    return clsid==clsid_filter_graph || clsid==clsid_filter_graph_no_thread || clsid==clsid_dsound_render || clsid==clsid_audio_render;
}
// IBasicAudio 的音量（1/100 dB，-10000～0）與平衡（-10000～10000）轉為左右增益。
std::pair<float,float> directshow_gain(long volume,long balance){
    const auto level=volume<=-10000?0.0:std::pow(10.0,static_cast<double>(volume)/2000.0);
    const auto attenuate=[](long value){return value<=-10000?0.0:std::pow(10.0,static_cast<double>(value)/2000.0);};
    const auto left=balance>0?attenuate(-balance):1.0,right=balance<0?attenuate(balance):1.0;
    return {static_cast<float>(level*left),static_cast<float>(level*right)};
}
}

std::uint64_t WindowsServices::directshow_vtable(GuestMemory& memory,std::string_view family){
    auto& state=*directshow_;const auto key=std::string(family);
    if(const auto found=state.vtables.find(key);found!=state.vtables.end())return found->second;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    std::vector<std::uint64_t> entries;for(const auto& d:directshow_service_descriptors())if(d.family==family)entries.push_back(resolve(d.family,d.symbol).value);
    const auto table=runtime_allocate(memory,{},entries.size()*unit);
    for(std::size_t i=0;i<entries.size();++i)memory.store(GuestAddress{table+i*unit},bits,entries[i]);
    state.vtables.emplace(key,table);return table;
}

std::uint64_t WindowsServices::create_directshow_object(GuestMemory& memory,const std::array<std::uint8_t,16>& clsid){
    if(!directshow_)directshow_=std::make_shared<DirectShowState>();
    auto& state=*directshow_;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(state.graphs.size()+state.filters.size()>=4096)throw std::bad_alloc{};
    if(clsid==clsid_filter_graph || clsid==clsid_filter_graph_no_thread){
        const auto object=allocate_heap(memory,graph_interfaces*unit);if(!object)throw std::bad_alloc{};
        for(unsigned i=0;i<graph_interfaces;++i)memory.store(GuestAddress{object+i*unit},bits,directshow_vtable(memory,graph_families[i]));
        state.graphs.emplace(object,DirectShowGraph{});return object;
    }
    const auto object=allocate_heap(memory,3*unit);if(!object)throw std::bad_alloc{};
    memory.store(GuestAddress{object},bits,directshow_vtable(memory,"internal-dshow-filter"));
    memory.store(GuestAddress{object+unit},bits,directshow_vtable(memory,"internal-dshow-filter-audio"));
    memory.store(GuestAddress{object+2*unit},bits,directshow_vtable(memory,"internal-dshow-filter-amds"));
    DirectShowFilter filter;filter.clsid=clsid;
    filter.name=clsid==clsid_async_reader?u"Source":clsid==clsid_audio_render?u"Audio Renderer":u"Default DirectSound Device";
    state.filters.emplace(object,std::move(filter));return object;
}

bool WindowsServices::begin_directshow_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);if(!arg(0))return false;
    Guid clsid{};memory.read(GuestAddress{arg(0)},clsid);if(!directshow_class(clsid))return false;
    const bool create=service.symbol=="CoCreateInstance";const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto output=arg(4);const auto done=[&](std::uint32_t hr){arg.return_to(state,hr);return true;};
    if(!output)return done(e_pointer);memory.store(GuestAddress{output},bits,0);if(!arg(3))return done(e_invalidarg);
    if(!(static_cast<std::uint32_t>(arg(create?2:1))&1U))return done(0x80040154U); // quartz 只有 in-process 伺服器
    Guid iid{};memory.read(GuestAddress{arg(3)},iid);
    if(!directshow_)directshow_=std::make_shared<DirectShowState>();
    if(!create){
        if(iid!=iid_unknown && iid!=iid_class_factory)return done(e_nointerface);
        const auto factory=allocate_heap(memory,unit);if(!factory)return done(e_outofmemory);
        memory.store(GuestAddress{factory},bits,directshow_vtable(memory,"internal-dshow-factory"));
        directshow_->factories.emplace(factory,std::pair<Guid,std::uint32_t>{clsid,1});memory.store(GuestAddress{output},bits,factory);return done(s_ok);
    }
    if(arg(1))return done(class_e_noaggregation);
    std::uint64_t object=0;
    try{object=create_directshow_object(memory,clsid);}catch(const std::bad_alloc&){return done(e_outofmemory);}
    // 以 QueryInterface 規則取得要求的介面；失敗時釋放剛建立的物件。
    std::uint64_t pointer=0;
    if(const auto graph=directshow_->graphs.find(object);graph!=directshow_->graphs.end()){
        const std::array<std::pair<Guid,unsigned>,10> map{{{iid_unknown,0},{iid_filter_graph,0},{iid_graph_builder,0},{iid_filter_graph2,0},{iid_media_control,1},
            {iid_dispatch,1},{iid_media_event,2},{iid_media_event_ex,2},{iid_media_seeking,3},{iid_basic_audio,4}}};
        for(const auto& [id,index]:map)if(id==iid){pointer=object+index*unit;break;}
        if(!pointer && iid==iid_media_position)pointer=object+position_interface*unit;
        if(!pointer && iid==iid_video_window)pointer=object+video_window_interface*unit;
        if(!pointer && (iid==iid_basic_video || iid==iid_basic_video2))pointer=object+basic_video_interface*unit;
        if(!pointer){directshow_->graphs.erase(graph);(void)free_heap(memory,object,state);return done(e_nointerface);}
    }else{
        if(iid==iid_unknown || iid==iid_persist || iid==iid_media_filter || iid==iid_base_filter)pointer=object;
        else if(iid==iid_basic_audio && clsid!=clsid_async_reader)pointer=object+unit;
        else if(iid==iid_am_direct_sound && clsid==clsid_dsound_render)pointer=object+2*unit;
        if(!pointer){directshow_->filters.erase(object);(void)free_heap(memory,object,state);return done(e_nointerface);}
    }
    memory.store(GuestAddress{output},bits,pointer);return done(s_ok);
}

// X64ARM_TRACE_DSHOW：1 列出 RenderFile，2 另列每個方法呼叫與完成通知（診斷用）。
static int directshow_trace_level(){static const int level=[]{const char* value=std::getenv("X64ARM_TRACE_DSHOW");return value && *value?std::atoi(value):0;}();return level;}

bool WindowsServices::directshow_active()const{
    if(!directshow_)return false;
    return std::any_of(directshow_->graphs.begin(),directshow_->graphs.end(),[](const auto& item){return item.second.state==2 && !item.second.completed && (item.second.buffer || item.second.video);});
}

void WindowsServices::poll_directshow(GuestMemory&){
    if(!directshow_)return;
    for(auto& [object,graph]:directshow_->graphs){
        if(graph.state!=2 || graph.completed || (!graph.buffer && !graph.video))continue;
        bool ended=false;
        if(graph.video){
            // 內部 MCI 影片裝置播放到結尾後由 poll_mci 停止；停止位置早於結尾時在此停止。
            auto& bridge=mci_state();const auto device=bridge.videos.find(graph.video);
            if(device==bridge.videos.end())ended=true;
            else{
                auto& video=*device->second;
                if(video.mode==mci::Mode::Playing && graph.stop && video.clip->time_at(video.current(runtime_.clock_ticks))>=*graph.stop)video.stop(runtime_.clock_ticks);
                ended=video.mode==mci::Mode::Stopped;
            }
        }else{
            std::lock_guard lock(graph.buffer->mutex);
            const auto position=static_cast<std::uint64_t>(graph.buffer->cursor);
            const auto stop_frame=graph.stop && graph.rate?std::min<std::uint64_t>(graph.frames,*graph.stop*graph.rate/10000000):graph.frames;
            if(position>=stop_frame){graph.buffer->playing=false;graph.buffer->cursor=static_cast<double>(stop_frame);}
            ended=!graph.buffer->playing;
        }
        if(!ended)continue;
        graph.completed=true;
        if(directshow_trace_level()>=2)std::fprintf(stderr,"[dshow] complete graph=%llx video=%d flags=%x window=%llx message=%x event=%llx completion=%llx\n",
            static_cast<unsigned long long>(object),graph.video?1:0,graph.notify_flags,static_cast<unsigned long long>(graph.notify_window),static_cast<unsigned>(graph.notify_message),
            static_cast<unsigned long long>(graph.event_handle),static_cast<unsigned long long>(graph.completion_handle));
        if(graph.completion_handle)if(auto event=runtime_.wait_objects.find(graph.completion_handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);
        if(graph.notify_flags&1U)continue; // AM_MEDIAEVENT_NONOTIFY：不排入事件
        if(graph.events.size()<256)graph.events.push_back({ec_complete,s_ok,0});
        if(graph.event_handle)if(auto event=runtime_.wait_objects.find(graph.event_handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);
        if(graph.notify_window && graph.notify_message)
            wave_callbacks_.push_back({resolve("user32.dll","PostMessageW").value,{graph.notify_window,graph.notify_message,0,graph.notify_instance}});
    }
}

std::optional<std::uint32_t> WindowsServices::invoke_directshow(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"DirectShow",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        using service_callback::Task;
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){
            if(directshow_trace_level()>=2)std::fprintf(stderr,"[dshow] ret %.*s thread=%llu value=%llx\n",static_cast<int>(service.symbol.size()),service.symbol.data(),
                static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(value));
            call.return_to(s,value);return std::optional<std::uint32_t>{};};
        if(!directshow_)directshow_=std::make_shared<DirectShowState>();
        auto& ds=*directshow_;
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto family=service.family;const auto name=service.symbol;
        const auto self=call(0);
        if(directshow_trace_level()>=2)std::fprintf(stderr,"[dshow] call %.*s::%.*s self=%llx\n",static_cast<int>(family.size()),family.data(),static_cast<int>(name.size()),name.data(),static_cast<unsigned long long>(self));
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+std::uint64_t{index}*unit},bits);};
        const auto com=[&](std::uint64_t object,unsigned index,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            arguments.insert(arguments.begin(),object);co_return co_await service_callback::call_value(method(object,index),std::move(arguments));};
        const auto guid_at=[&](std::uint64_t pointer){Guid g{};m.read(GuestAddress{pointer},g);return g;};
        const auto store32=[&](std::uint64_t pointer,std::uint64_t value){m.store(GuestAddress{pointer},32,value);};
        const auto store64=[&](std::uint64_t pointer,std::uint64_t value){m.store(GuestAddress{pointer},64,value);};
        const auto store_pointer=[&](std::uint64_t pointer,std::uint64_t value){m.store(GuestAddress{pointer},bits,value);};
        const auto double_arg=[&](unsigned index){
            std::uint64_t word=call(index);
            if(!x86_ && index<4)std::memcpy(&word,s.xmm[index].data(),8);
            return std::bit_cast<double>(word);};
        const auto store_double=[&](std::uint64_t pointer,double value){m.store(GuestAddress{pointer},64,std::bit_cast<std::uint64_t>(value));};
        const auto read_text=[&](std::uint64_t pointer){return pointer?read_utf16(m,GuestAddress{pointer},32768):std::u16string{};};
        const auto filter_of=[&](std::uint64_t pointer)->DirectShowFilter*{const auto found=ds.filters.find(pointer);return found==ds.filters.end()?nullptr:&found->second;};
        // 濾鏡（宿主或客體）的參考計數。
        const auto addref_filter=[&](std::uint64_t filter)->Task<std::uint32_t>{if(auto* ours=filter_of(filter)){++ours->references;co_return 0;}(void)co_await com(filter,1,{});co_return 0;};
        const auto release_filter=[&](std::uint64_t filter)->Task<std::uint32_t>{
            if(auto* ours=filter_of(filter)){if(!--ours->references){ds.filters.erase(filter);(void)free_heap(m,filter,s);}co_return 0;}
            (void)co_await com(filter,2,{});co_return 0;};
        const auto join_filter=[&](std::uint64_t filter,std::uint64_t graph,const std::u16string& label)->Task<std::uint32_t>{
            if(auto* ours=filter_of(filter)){ours->graph=graph;if(graph)ours->name=label;co_return s_ok;}
            std::uint64_t text=0;std::vector<std::uint64_t> blocks;
            if(graph && !label.empty()){const auto bytes=utf16_bytes(label);text=allocate_heap(m,bytes.size()+2);if(!text)co_return e_outofmemory;
                m.fill(GuestAddress{text},0,bytes.size()+2);m.write(GuestAddress{text},bytes);}
            const auto result=static_cast<std::uint32_t>(co_await com(filter,13,{graph,text}));
            if(text)(void)free_heap(m,text,s);co_return result;
        };
        const auto video_device=[&](const DirectShowGraph& graph)->mci::AviDevice*{
            if(!graph.video)return nullptr;auto& bridge=mci_state();const auto found=bridge.videos.find(graph.video);return found==bridge.videos.end()?nullptr:found->second.get();};
        // 影片裝置的繪製視窗、可見性與來源／目的矩形（GuiRect 為 左、上、右、下）。
        const auto bind_video=[&](DirectShowGraph& graph){
            auto* device=video_device(graph);if(!device)return;
            const auto window=graph.owner?graph.owner:graph.own_window;device->window=window;
            device->hidden=!graph.visible || graph.window_state==0 || graph.window_state==2 || graph.window_state==6;
            const auto w=static_cast<std::int32_t>(graph.video_width),h=static_cast<std::int32_t>(graph.video_height);
            device->source=graph.default_source?GuiRect{0,0,w,h}:graph.source;
            if(graph.fullscreen || (!graph.owner && graph.default_destination))device->fixed_target=false;
            else{
                const GuiRect frame=graph.owner?GuiRect{graph.left,graph.top,graph.left+graph.width,graph.top+graph.height}:GuiRect{0,0,graph.width,graph.height};
                device->target=graph.default_destination?frame:GuiRect{frame[0]+graph.destination[0],frame[1]+graph.destination[1],frame[0]+graph.destination[2],frame[1]+graph.destination[3]};
                device->fixed_target=true;
            }
            device->presented_frame=UINT64_MAX;};
        // 影片視窗是擁有者的子視窗：顯示中的影片被隱藏、換擁有者或銷毀時，擁有者原本被蓋住的區域失效並清除背景。
        // 影片畫面直接畫在擁有者上，沒有這一步會殘留最後一格；等 WM_PAINT 才前進的訊息迴圈（WaitMessage）也會一直停住。
        const auto shown_owner_area=[&](const DirectShowGraph& graph)->std::optional<std::pair<std::uint64_t,std::optional<GuiRect>>>{
            const auto* device=video_device(graph);
            if(!device || device->hidden || !graph.owner || device->window!=graph.owner || !gui_.windows.contains(graph.owner))return std::nullopt;
            if(graph.fullscreen)return std::pair<std::uint64_t,std::optional<GuiRect>>{graph.owner,std::nullopt};
            return std::pair<std::uint64_t,std::optional<GuiRect>>{graph.owner,GuiRect{graph.left,graph.top,graph.left+graph.width,graph.top+graph.height}};};
        const auto expose_owner=[&](std::pair<std::uint64_t,std::optional<GuiRect>> area)->Task<std::uint32_t>{
            if(!gui_.windows.contains(area.first) || (area.second && gui_rect_empty(*area.second)))co_return 0;
            std::uint64_t block=0;
            if(area.second){block=allocate_heap(m,16);if(!block)co_return 0;for(unsigned i=0;i<4;++i)store32(block+i*4,static_cast<std::uint32_t>((*area.second)[i]));}
            (void)co_await service_callback::call_value(resolve("user32.dll","InvalidateRect").value,{area.first,block,1});
            if(block)(void)free_heap(m,block,s);co_return 0;};
        const auto apply_volume=[&](DirectShowGraph& graph){
            long volume=graph.volume,balance=graph.balance;
            for(const auto filter:graph.filters)if(auto* ours=filter_of(filter)){volume=std::max<long>(-10000,volume+ours->volume);if(ours->balance)balance=ours->balance;}
            const auto [left,right]=directshow_gain(volume,balance);
            if(auto* device=video_device(graph)){
                device->left=static_cast<unsigned>(std::lround(left*1000));device->right=static_cast<unsigned>(std::lround(right*1000));
                if(device->wave.buffer){std::lock_guard lock(device->wave.buffer->mutex);device->wave.buffer->left=left;device->wave.buffer->right=right;}
            }
            if(!graph.buffer)return;std::lock_guard lock(graph.buffer->mutex);graph.buffer->left=left;graph.buffer->right=right;};
        const auto position_of=[&](const DirectShowGraph& graph)->std::uint64_t{
            // 播放到結尾（EC_COMPLETE）後目前位置即停止位置。
            if(graph.completed)return graph.stop.value_or(graph.duration);
            if(const auto* device=video_device(graph))return device->clip->time_at(device->current(runtime_.clock_ticks));
            if(!graph.buffer || !graph.rate)return 0;std::lock_guard lock(graph.buffer->mutex);
            return static_cast<std::uint64_t>(static_cast<__int128>(static_cast<std::uint64_t>(graph.buffer->cursor))*10000000/graph.rate);};
        const auto seek=[&](DirectShowGraph& graph,std::uint64_t time){
            if(auto* device=video_device(graph)){
                const auto frame=std::min<std::uint64_t>(device->clip->frame_at(time),device->length());
                if(device->mode==mci::Mode::Playing && frame<device->length())device->play(frame,device->length(),false,false,runtime_.clock_ticks);
                else {device->stop(runtime_.clock_ticks);device->position=frame;}
                device->presented_frame=UINT64_MAX;graph.completed=false;
                if(graph.completion_handle)if(auto event=runtime_.wait_objects.find(graph.completion_handle);event!=runtime_.wait_objects.end())event->second.set_signal(false);
                return;
            }
            if(!graph.buffer || !graph.rate)return;const auto frame=std::min<std::uint64_t>(graph.frames,static_cast<std::uint64_t>(static_cast<__int128>(time)*graph.rate/10000000));
            std::lock_guard lock(graph.buffer->mutex);graph.buffer->cursor=static_cast<double>(frame);
            if(graph.state==2){graph.buffer->playing=frame<graph.frames;graph.completed=false;}
            else graph.completed=false;
            if(graph.completion_handle)if(auto event=runtime_.wait_objects.find(graph.completion_handle);event!=runtime_.wait_objects.end())event->second.set_signal(false);};
        const auto make_event=[&](std::uint64_t& handle,bool signaled){
            if(handle)return handle;handle=runtime_.allocate_wait_handle();WaitObject event;event.kind=WaitObject::Kind::Event;event.manual_reset=true;event.access=0x1f0003U;
            event.set_signal(signaled);runtime_.wait_objects.emplace(handle,std::move(event));return handle;};
        // 拆除已轉譯的串流（音訊輸出或內部影片裝置）：圖表銷毀，或串流依賴的來源／轉譯濾鏡被移除時共用。
        const auto release_stream=[&](DirectShowGraph& graph){
            if(graph.buffer){std::lock_guard lock(graph.buffer->mutex);graph.buffer->playing=false;}
            graph.output.reset();graph.buffer.reset();
            if(graph.video){auto& bridge=mci_state();if(auto device=bridge.videos.find(graph.video);device!=bridge.videos.end()){device->second->stop(runtime_.clock_ticks);bridge.videos.erase(device);}}
            graph.video=0;graph.video_width=graph.video_height=0;graph.frame_time=0;
            graph.frames=0;graph.rate=0;graph.duration=0;graph.file.clear();graph.stream_source=0;graph.completed=false;graph.stop.reset();};
        const auto destroy_graph=[&](std::uint64_t object)->Task<std::uint32_t>{
            auto found=ds.graphs.find(object);if(found==ds.graphs.end())co_return 0;
            const auto exposed=shown_owner_area(found->second);
            auto filters=std::move(found->second.filters);
            release_stream(found->second);
            const auto own_window=found->second.own_window;
            for(const auto handle:{found->second.event_handle,found->second.completion_handle})if(handle)runtime_.wait_objects.erase(handle);
            ds.graphs.erase(found);
            if(own_window && gui_.windows.contains(own_window))(void)co_await service_callback::call_value(resolve("user32.dll","DestroyWindow").value,{own_window});
            if(exposed)(void)co_await expose_owner(*exposed);
            for(const auto filter:filters){(void)co_await join_filter(filter,0,{});(void)co_await release_filter(filter);}
            (void)free_heap(m,object,s);co_return 0;
        };

        // 自建的頂層視訊視窗：類別以 DefWindowProcW 處理訊息且不清除背景，客戶區大小約為影片大小。
        const auto create_video_window=[&](std::uint64_t graph_object)->Task<std::uint64_t>{
            const auto text_block=[&](std::u16string_view text){const auto bytes=utf16_bytes(text);const auto block=allocate_heap(m,bytes.size()+2);
                if(block){m.fill(GuestAddress{block},0,bytes.size()+2);m.write(GuestAddress{block},bytes);}return block;};
            const auto class_name=text_block(u"VideoRenderer");if(!class_name)co_return 0;
            if(!ds.video_class){
                const unsigned size=x86_?40U:72U;const auto record=allocate_heap(m,size);if(!record){(void)free_heap(m,class_name,s);co_return 0;}
                m.fill(GuestAddress{record},0,size);
                if(x86_){m.store(GuestAddress{record+4},32,resolve("user32.dll","DefWindowProcW").value);m.store(GuestAddress{record+36},32,class_name);}
                else {m.store(GuestAddress{record+8},64,resolve("user32.dll","DefWindowProcW").value);m.store(GuestAddress{record+64},64,class_name);}
                ds.video_class=co_await service_callback::call_value(resolve("user32.dll","RegisterClassW").value,{record});(void)free_heap(m,record,s);
            }
            const auto current=ds.graphs.find(graph_object);if(current==ds.graphs.end()){(void)free_heap(m,class_name,s);co_return 0;}
            const auto caption=text_block(current->second.caption);
            const auto style=(current->second.window_style&~0x10000000U)|(current->second.visible?0x10000000U:0U);
            const auto width=static_cast<std::uint64_t>(std::max<std::int32_t>(current->second.width,1))+16,height=static_cast<std::uint64_t>(std::max<std::int32_t>(current->second.height,1))+39;
            const auto window=co_await service_callback::call_value(resolve("user32.dll","CreateWindowExW").value,
                {current->second.window_style_ex,class_name,caption,style,0x80000000U,0x80000000U,width,height,0,0,0,0});
            (void)free_heap(m,class_name,s);if(caption)(void)free_heap(m,caption,s);
            co_return window;
        };
        // IDispatch：本實作不提供型別資訊與自動化名稱。
        if(name=="GetTypeInfoCount"){if(!call(1))co_return finish(e_pointer);store32(call(1),0);co_return finish(s_ok);}
        if(name=="GetTypeInfo"){if(call(3))store_pointer(call(3),0);co_return finish(e_notimpl);}
        if(name=="GetIDsOfNames")co_return finish(disp_e_unknownname);
        if(name=="Invoke")co_return finish(disp_e_membernotfound);

        if(family=="internal-dshow-factory"){
            const auto found=ds.factories.find(self);if(found==ds.factories.end())co_return finish(e_unexpected);
            if(name=="QueryInterface"){if(!call(2))co_return finish(e_pointer);store_pointer(call(2),0);const auto iid=guid_at(call(1));
                if(iid!=iid_unknown && iid!=iid_class_factory)co_return finish(e_nointerface);++found->second.second;store_pointer(call(2),self);co_return finish(s_ok);}
            if(name=="AddRef")co_return finish(++found->second.second);
            if(name=="Release"){const auto left=--found->second.second;if(!left){ds.factories.erase(found);(void)free_heap(m,self,s);}co_return finish(left);}
            if(name=="LockServer")co_return finish(s_ok);
            if(name=="CreateInstance"){
                if(!call(3))co_return finish(e_pointer);store_pointer(call(3),0);if(call(1))co_return finish(class_e_noaggregation);if(!call(2))co_return finish(e_invalidarg);
                const auto clsid=found->second.first;const auto iid=guid_at(call(2));std::uint64_t object=0;
                try{object=create_directshow_object(m,clsid);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
                // 共用 QueryInterface：轉到該物件第一個介面的 QueryInterface 規則。
                std::uint64_t pointer=0;
                if(ds.graphs.contains(object)){
                    if(iid==iid_unknown || iid==iid_filter_graph || iid==iid_graph_builder || iid==iid_filter_graph2)pointer=object;
                    else if(iid==iid_media_control || iid==iid_dispatch)pointer=object+control_interface*unit;
                    else if(iid==iid_media_event || iid==iid_media_event_ex)pointer=object+event_interface*unit;
                    else if(iid==iid_media_seeking)pointer=object+seeking_interface*unit;
                    else if(iid==iid_basic_audio)pointer=object+audio_interface*unit;
                    else if(iid==iid_media_position)pointer=object+position_interface*unit;
                    else if(iid==iid_video_window)pointer=object+video_window_interface*unit;
                    else if(iid==iid_basic_video || iid==iid_basic_video2)pointer=object+basic_video_interface*unit;
                    if(!pointer){ds.graphs.erase(object);(void)free_heap(m,object,s);co_return finish(e_nointerface);}
                }else{
                    if(iid==iid_unknown || iid==iid_persist || iid==iid_media_filter || iid==iid_base_filter)pointer=object;
                    else if(iid==iid_basic_audio && clsid!=clsid_async_reader)pointer=object+unit;
                    else if(iid==iid_am_direct_sound && clsid==clsid_dsound_render)pointer=object+2*unit;
                    if(!pointer){ds.filters.erase(object);(void)free_heap(m,object,s);co_return finish(e_nointerface);}
                }
                store_pointer(call(3),pointer);co_return finish(s_ok);
            }
            co_return finish(e_notimpl);
        }

        if(family=="internal-dshow-enum"){
            const auto found=ds.enums.find(self);if(found==ds.enums.end())co_return finish(e_unexpected);auto& e=found->second;
            if(name=="QueryInterface"){if(!call(2))co_return finish(e_pointer);store_pointer(call(2),0);const auto iid=guid_at(call(1));
                if(iid!=iid_unknown && iid!=iid_enum_filters)co_return finish(e_nointerface);++e.references;store_pointer(call(2),self);co_return finish(s_ok);}
            if(name=="AddRef")co_return finish(++e.references);
            if(name=="Release"){
                const auto left=--e.references;
                if(!left){auto filters=std::move(e.filters);ds.enums.erase(found);(void)free_heap(m,self,s);for(const auto filter:filters)(void)co_await release_filter(filter);}
                co_return finish(left);}
            if(name=="Next"){
                const auto count=static_cast<std::uint32_t>(call(1));if(!call(2))co_return finish(e_pointer);if(count>1 && !call(3))co_return finish(e_pointer);
                std::uint32_t fetched=0;
                while(fetched<count && e.cursor<e.filters.size()){const auto filter=e.filters[e.cursor++];(void)co_await addref_filter(filter);store_pointer(call(2)+std::uint64_t{fetched}*unit,filter);++fetched;}
                if(call(3))store32(call(3),fetched);co_return finish(fetched==count?s_ok:s_false);}
            if(name=="Skip"){const auto count=static_cast<std::uint32_t>(call(1));const auto left=e.filters.size()-e.cursor;e.cursor+=std::min<std::size_t>(count,left);co_return finish(count<=left?s_ok:s_false);}
            if(name=="Reset"){e.cursor=0;co_return finish(s_ok);}
            if(name=="Clone"){
                if(!call(1))co_return finish(e_pointer);const auto copy=allocate_heap(m,unit);if(!copy)co_return finish(e_outofmemory);
                m.store(GuestAddress{copy},bits,directshow_vtable(m,"internal-dshow-enum"));DirectShowEnum clone;clone.filters=e.filters;clone.cursor=e.cursor;
                for(const auto filter:clone.filters)(void)co_await addref_filter(filter);
                ds.enums.emplace(copy,std::move(clone));store_pointer(call(1),copy);co_return finish(s_ok);}
            co_return finish(e_notimpl);
        }

        if(family=="internal-dshow-filter" || family=="internal-dshow-filter-audio" || family=="internal-dshow-filter-amds"){
            const auto object=family=="internal-dshow-filter"?self:family=="internal-dshow-filter-audio"?self-unit:self-2*unit;
            auto* filter=filter_of(object);if(!filter)co_return finish(e_unexpected);
            if(name=="QueryInterface"){
                if(!call(2))co_return finish(e_pointer);store_pointer(call(2),0);const auto iid=guid_at(call(1));std::uint64_t pointer=0;
                if(iid==iid_unknown || iid==iid_persist || iid==iid_media_filter || iid==iid_base_filter)pointer=object;
                else if(iid==iid_basic_audio && filter->clsid!=clsid_async_reader)pointer=object+unit;
                else if(iid==iid_am_direct_sound && filter->clsid==clsid_dsound_render)pointer=object+2*unit;
                if(!pointer)co_return finish(e_nointerface);++filter->references;store_pointer(call(2),pointer);co_return finish(s_ok);}
            if(name=="AddRef")co_return finish(++filter->references);
            if(name=="Release"){const auto left=--filter->references;if(!left){ds.filters.erase(object);(void)free_heap(m,object,s);}co_return finish(left);}
            if(name=="put_Volume"){const auto value=static_cast<long>(static_cast<std::int32_t>(call(1)));if(value<-10000 || value>0)co_return finish(e_invalidarg);
                filter->volume=value;if(auto graph=ds.graphs.find(filter->graph);graph!=ds.graphs.end())apply_volume(graph->second);co_return finish(s_ok);}
            if(name=="get_Volume"){if(!call(1))co_return finish(e_pointer);store32(call(1),static_cast<std::uint32_t>(filter->volume));co_return finish(s_ok);}
            if(name=="put_Balance"){const auto value=static_cast<long>(static_cast<std::int32_t>(call(1)));if(value<-10000 || value>10000)co_return finish(e_invalidarg);
                filter->balance=value;if(auto graph=ds.graphs.find(filter->graph);graph!=ds.graphs.end())apply_volume(graph->second);co_return finish(s_ok);}
            if(name=="get_Balance"){if(!call(1))co_return finish(e_pointer);store32(call(1),static_cast<std::uint32_t>(filter->balance));co_return finish(s_ok);}
            if(name=="GetClassID"){if(!call(1))co_return finish(e_pointer);m.write(GuestAddress{call(1)},filter->clsid);co_return finish(s_ok);}
            // IAMDirectSound：焦點視窗設定保存並回報；轉譯濾鏡的 DirectSound 物件與緩衝區不對外公開。
            if(name=="SetFocusWindow"){filter->focus_window=call(1);filter->focus_mixing=static_cast<std::uint32_t>(call(2))!=0;co_return finish(s_ok);}
            if(name=="GetFocusWindow"){if(!call(1) || !call(2))co_return finish(e_pointer);store_pointer(call(1),filter->focus_window);store32(call(2),filter->focus_mixing?1U:0U);co_return finish(s_ok);}
            if(name=="GetDirectSoundInterface" || name=="GetPrimaryBufferInterface" || name=="GetSecondaryBufferInterface"){if(call(1))store_pointer(call(1),0);co_return finish(e_notimpl);}
            if(name=="ReleaseDirectSoundInterface" || name=="ReleasePrimaryBufferInterface" || name=="ReleaseSecondaryBufferInterface")co_return finish(e_invalidarg);
            if(name=="Stop"){filter->state=0;co_return finish(s_ok);}
            if(name=="Pause"){filter->state=1;co_return finish(s_ok);}
            if(name=="Run"){filter->state=2;co_return finish(s_ok);}
            if(name=="GetState"){if(!call(2))co_return finish(e_pointer);store32(call(2),filter->state);co_return finish(s_ok);}
            if(name=="SetSyncSource")co_return finish(s_ok);
            if(name=="GetSyncSource"){if(!call(1))co_return finish(e_pointer);store_pointer(call(1),0);co_return finish(s_ok);}
            if(name=="EnumPins"){if(call(1))store_pointer(call(1),0);co_return finish(e_notimpl);}
            if(name=="FindPin"){if(call(2))store_pointer(call(2),0);co_return finish(vfw_e_not_found);}
            if(name=="QueryFilterInfo"){
                if(!call(1))co_return finish(e_pointer);std::vector<std::uint8_t> bytes(256+unit);
                const auto text=utf16_bytes(filter->name.substr(0,127));std::copy(text.begin(),text.end(),bytes.begin());m.write(GuestAddress{call(1)},bytes);
                // FILTER_INFO 的 pGraph 會多一個參考，呼叫端負責 Release。
                const auto graph=filter->graph;if(graph){if(auto found=ds.graphs.find(graph);found!=ds.graphs.end())++found->second.references;}
                m.store(GuestAddress{call(1)+256},bits,graph);co_return finish(s_ok);}
            if(name=="JoinFilterGraph"){
                filter->graph=call(1)?call(1):0;if(call(1))filter->name=read_text(call(2)).substr(0,127);co_return finish(s_ok);}
            if(name=="QueryVendorInfo"){if(call(1))store_pointer(call(1),0);co_return finish(e_notimpl);}
            co_return finish(e_notimpl);
        }

        // 濾鏡圖：由介面指標換回物件位址。
        unsigned index=graph_interfaces;for(unsigned i=0;i<graph_interfaces;++i)if(graph_families[i]==family){index=i;break;}
        if(index==graph_interfaces)co_return finish(e_unexpected);
        const auto object=self-std::uint64_t{index}*unit;
        auto found=ds.graphs.find(object);if(found==ds.graphs.end())co_return finish(e_unexpected);
        auto& graph=found->second;
        if(name=="QueryInterface"){
            if(!call(2))co_return finish(e_pointer);store_pointer(call(2),0);const auto iid=guid_at(call(1));std::uint64_t pointer=0;
            if(iid==iid_unknown || iid==iid_filter_graph || iid==iid_graph_builder || iid==iid_filter_graph2)pointer=object;
            else if(iid==iid_media_control || iid==iid_dispatch)pointer=object+control_interface*unit;
            else if(iid==iid_media_event || iid==iid_media_event_ex)pointer=object+event_interface*unit;
            else if(iid==iid_media_seeking)pointer=object+seeking_interface*unit;
            else if(iid==iid_basic_audio)pointer=object+audio_interface*unit;
            else if(iid==iid_media_position)pointer=object+position_interface*unit;
            else if(iid==iid_video_window)pointer=object+video_window_interface*unit;
            else if(iid==iid_basic_video || iid==iid_basic_video2)pointer=object+basic_video_interface*unit;
            if(!pointer)co_return finish(e_nointerface);++graph.references;store_pointer(call(2),pointer);co_return finish(s_ok);
        }
        if(name=="AddRef")co_return finish(++graph.references);
        if(name=="Release"){const auto left=--graph.references;if(!left)(void)co_await destroy_graph(object);co_return finish(left);}

        // RenderFile 的共用流程：解碼音訊、建立輸出與兩個宿主濾鏡。
        const auto render_file=[&](std::u16string file)->Task<std::uint32_t>{
            if(file.empty())co_return e_invalidarg;
            if(graph.buffer || graph.video)co_return vfw_e_wrong_state; // 本圖已經轉譯過檔案
            std::u16string path;try{path=normalized_windows_path(std::move(file),runtime_.current_directory);}catch(const std::invalid_argument&){co_return file_not_found;}
            const auto host=utf16_to_utf8(path);const auto info=filesystem_.information(host);if(info.error)co_return 0x80070000U|info.error;
            std::optional<VideoPreview::AudioTrack> track;bool visual=false;
            try{
                const auto source=filesystem_.open_reader(host);if(source.error)co_return 0x80070000U|source.error;
                // RIFF WAVE 直接讀取；其他容器交給 MCI／VFW 共用的預覽解碼器。
                if(source.file && source.metadata.size>=12 && source.metadata.size<=256U*1024U*1024U){
                    std::array<std::uint8_t,12> head{};const auto got=source.file->read(head,0);
                    if(!got.error && got.bytes==12 && std::memcmp(head.data(),"RIFF",4)==0 && std::memcmp(head.data()+8,"WAVE",4)==0){
                        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.metadata.size));std::size_t at=0;
                        while(at<bytes.size()){const auto part=source.file->read(std::span(bytes).subspan(at),at);if(part.error || !part.bytes)break;at+=static_cast<std::size_t>(part.bytes);}
                        if(const auto wave=parse_pcm_wave_file(bytes)){track=VideoPreview::AudioTrack{wave->format,{wave->samples.begin(),wave->samples.end()}};}
                    }
                }
                if(!track){
                    bool avi=false;
                    std::unique_ptr<VideoPreview> clip=Mp4Preview::open(source);
                    if(!clip)clip=MpegPreview::open(source);
                    if(!clip){clip=AviPreviewClip::open(filesystem_,host,true);avi=clip!=nullptr;}
                    if(!clip)co_return vfw_e_unknown_file_type;
                    visual=clip->visual();
                    if(visual){
                        // 視訊與其音軌交給內部 MCI 影片裝置（解碼、時間軸、聲音與自動繪製共用 MCI 流程）。
                        auto& bridge=mci_state();
                        auto device=std::make_shared<mci::AviDevice>(std::move(clip));
                        device->quartz=!avi;if(device->quartz)device->format=0;
                        device->file=path;device->creator=threads_.current;device->presentation=bridge.next_presentation++;
                        graph.video_width=device->clip->width();graph.video_height=device->clip->height();graph.frame_time=device->clip->interval();
                        graph.duration=device->clip->time_at(device->length());
                        graph.width=static_cast<std::int32_t>(graph.video_width);graph.height=static_cast<std::int32_t>(graph.video_height);
                        graph.source=graph.destination=GuiRect{0,0,graph.width,graph.height};
                        graph.video=bridge.allocate_device();bridge.videos.emplace(graph.video,std::move(device));
                    }else track=clip->take_audio();
                }
            }catch(const std::bad_alloc&){co_return e_outofmemory;}catch(const std::length_error&){co_return e_outofmemory;}catch(const std::exception&){co_return vfw_e_unsupported_stream;}
            if(graph.video){
                graph.file=path;graph.completed=false;graph.state=0;graph.stop.reset();
                std::vector<std::pair<Guid,std::u16string>> added{{clsid_async_reader,path}};
                const bool has_renderer=std::any_of(graph.filters.begin(),graph.filters.end(),[&](std::uint64_t filter){
                    const auto* ours=filter_of(filter);return ours && (ours->clsid==clsid_dsound_render || ours->clsid==clsid_audio_render);});
                if(!has_renderer)added.emplace_back(clsid_dsound_render,u"Default DirectSound Device");
                for(const auto& [clsid,label]:added){
                    std::uint64_t filter=0;try{filter=create_directshow_object(m,clsid);}catch(const std::bad_alloc&){co_return e_outofmemory;}
                    auto& ours=ds.filters.at(filter);ours.graph=object;ours.name=label.substr(0,127);graph.filters.push_back(filter);graph.names.push_back(ours.name);
                    if(clsid==clsid_async_reader)graph.stream_source=filter;
                }
                bind_video(graph);apply_volume(graph);
                if(std::getenv("X64ARM_TRACE_DSHOW"))std::fprintf(stderr,"[dshow] RenderFile path=%s video=%ux%u duration=%llu\n",utf16_to_utf8(path).c_str(),graph.video_width,graph.video_height,
                    static_cast<unsigned long long>(graph.duration));
                co_return s_ok;
            }
            if(!track || track->pcm.empty() || !track->format.align || !track->format.rate){
                if(std::getenv("X64ARM_TRACE_DSHOW"))std::fprintf(stderr,"[dshow] RenderFile path=%s visual=%d 沒有可播放的音訊\n",utf16_to_utf8(path).c_str(),visual?1:0);
                co_return visual?vfw_e_cannot_render:vfw_e_unsupported_stream;}
            if(!audio::output_available())co_return vfw_e_cannot_render;
            auto buffer=std::make_shared<audio::Buffer>();
            buffer->channels=track->format.channels;buffer->bits=track->format.bits;buffer->rate=buffer->frequency=track->format.rate;
            buffer->floating=track->format.floating;buffer->channel_mask=track->format.channel_mask;
            graph.frames=track->pcm.size()/track->format.align;graph.rate=track->format.rate;
            graph.duration=static_cast<std::uint64_t>(static_cast<__int128>(graph.frames)*10000000/graph.rate);
            buffer->pcm=std::move(track->pcm);buffer->playing=false;buffer->looping=false;
            try{graph.output=audio::open_output(buffer);}catch(const std::runtime_error&){co_return vfw_e_cannot_render;}
            if(!graph.output)co_return vfw_e_cannot_render;
            graph.buffer=std::move(buffer);graph.file=path;graph.completed=false;graph.state=0;graph.stop.reset();
            // 圖中沒有音訊轉譯濾鏡時加入預設的 DirectSound 轉譯濾鏡；來源濾鏡以檔名命名。
            const bool has_renderer=std::any_of(graph.filters.begin(),graph.filters.end(),[&](std::uint64_t filter){
                const auto* ours=filter_of(filter);return ours && (ours->clsid==clsid_dsound_render || ours->clsid==clsid_audio_render);});
            std::vector<std::pair<Guid,std::u16string>> added{{clsid_async_reader,path}};
            if(!has_renderer)added.emplace_back(clsid_dsound_render,u"Default DirectSound Device");
            for(const auto& [clsid,label]:added){
                std::uint64_t filter=0;try{filter=create_directshow_object(m,clsid);}catch(const std::bad_alloc&){co_return e_outofmemory;}
                auto& ours=ds.filters.at(filter);ours.graph=object;ours.name=label.substr(0,127);graph.filters.push_back(filter);graph.names.push_back(ours.name);
                if(clsid==clsid_async_reader)graph.stream_source=filter;
            }
            apply_volume(graph);
            if(std::getenv("X64ARM_TRACE_DSHOW"))std::fprintf(stderr,"[dshow] RenderFile path=%s visual=%d frames=%llu rate=%u\n",utf16_to_utf8(path).c_str(),visual?1:0,
                static_cast<unsigned long long>(graph.frames),graph.rate);
            co_return visual?vfw_s_video_not_rendered:s_ok;
        };
        const auto set_state=[&](std::uint32_t next){
            graph.state=next;
            if(auto* device=video_device(graph)){
                const auto now=runtime_.clock_ticks;
                if(next==2){
                    if(device->mode==mci::Mode::Paused)device->resume(now);
                    else if(device->mode==mci::Mode::Stopped && !graph.completed && device->position<device->length())device->play(device->position,device->length(),false,false,now);
                }else if(next==1){if(device->mode==mci::Mode::Playing)device->pause(now);}
                else if(device->mode!=mci::Mode::Stopped)device->stop(now);
                apply_volume(graph);
            }
            if(graph.buffer){std::lock_guard lock(graph.buffer->mutex);
                if(next==2){
                    const auto at_end=static_cast<std::uint64_t>(graph.buffer->cursor)>=graph.frames;
                    graph.buffer->playing=!at_end && !graph.completed;
                }else graph.buffer->playing=false;}
            for(const auto filter:graph.filters)if(auto* ours=filter_of(filter))ours->state=next;
        };

        if(index==graph_interface){
            if(name=="AddFilter"){
                const auto filter=call(1);if(!filter)co_return finish(e_pointer);
                if(std::find(graph.filters.begin(),graph.filters.end(),filter)!=graph.filters.end())co_return finish(vfw_e_duplicate_name); // 同一濾鏡重複加入
                auto label=read_text(call(2)).substr(0,127);
                if(label.empty())label=u"Filter "+utf8_to_utf16(std::to_string(graph.filters.size()+1));
                const bool duplicate=std::find(graph.names.begin(),graph.names.end(),label)!=graph.names.end();
                if(duplicate)label+=u" 0001";
                (void)co_await addref_filter(filter);
                const auto joined=co_await join_filter(filter,object,label);
                if(static_cast<std::int32_t>(joined)<0){(void)co_await release_filter(filter);co_return finish(joined);}
                // 參考計數期間圖可能已被釋放；重新查找。
                auto again=ds.graphs.find(object);if(again==ds.graphs.end())co_return finish(e_unexpected);
                again->second.filters.push_back(filter);again->second.names.push_back(label);apply_volume(again->second);
                co_return finish(duplicate?vfw_s_duplicate_name:s_ok);
            }
            if(name=="RemoveFilter"){
                const auto filter=call(1);const auto at=std::find(graph.filters.begin(),graph.filters.end(),filter);
                if(at==graph.filters.end())co_return finish(e_fail);
                // 移除已轉譯串流的來源（RenderFile 建立的非同步讀取器）或轉譯濾鏡時，連線隨之中斷：拆除串流，之後可再 RenderFile 新檔
                // （BGM 程式庫以 Stop→列舉並移除全部濾鏡→RenderFile 換曲；原本回 VFW_E_WRONG_STATE，呼叫端的錯誤路徑未解鎖而卡死）。
                const auto* ours=filter_of(filter);
                const bool stream_filter=(graph.buffer || graph.video) && (filter==graph.stream_source || (ours && (ours->clsid==clsid_dsound_render || ours->clsid==clsid_audio_render)));
                const auto position=static_cast<std::size_t>(at-graph.filters.begin());graph.filters.erase(at);graph.names.erase(graph.names.begin()+static_cast<std::ptrdiff_t>(position));
                std::optional<std::pair<std::uint64_t,std::optional<GuiRect>>> exposed;std::uint64_t own_window=0;
                if(stream_filter){exposed=shown_owner_area(graph);if(graph.video){own_window=graph.own_window;graph.own_window=0;}release_stream(graph);}
                (void)co_await join_filter(filter,0,{});(void)co_await release_filter(filter);
                if(own_window && gui_.windows.contains(own_window))(void)co_await service_callback::call_value(resolve("user32.dll","DestroyWindow").value,{own_window});
                if(exposed)(void)co_await expose_owner(*exposed);
                co_return finish(s_ok);
            }
            if(name=="EnumFilters"){
                if(!call(1))co_return finish(e_pointer);const auto enumerator=allocate_heap(m,unit);if(!enumerator)co_return finish(e_outofmemory);
                m.store(GuestAddress{enumerator},bits,directshow_vtable(m,"internal-dshow-enum"));
                DirectShowEnum e;e.filters=graph.filters;for(const auto filter:e.filters)(void)co_await addref_filter(filter);
                ds.enums.emplace(enumerator,std::move(e));store_pointer(call(1),enumerator);co_return finish(s_ok);
            }
            if(name=="FindFilterByName"){
                if(!call(2))co_return finish(e_pointer);store_pointer(call(2),0);const auto label=read_text(call(1));
                for(std::size_t i=0;i<graph.names.size();++i)if(graph.names[i]==label){const auto filter=graph.filters[i];(void)co_await addref_filter(filter);store_pointer(call(2),filter);co_return finish(s_ok);}
                co_return finish(vfw_e_not_found);
            }
            if(name=="ConnectDirect" || name=="Connect")co_return finish(vfw_e_cannot_connect);
            if(name=="Reconnect" || name=="ReconnectEx")co_return finish(vfw_e_not_connected);
            if(name=="Disconnect")co_return finish(s_false);
            if(name=="Render" || name=="RenderEx")co_return finish(vfw_e_cannot_render);
            if(name=="SetDefaultSyncSource" || name=="SetLogFile" || name=="Abort" || name=="ShouldOperationContinue")co_return finish(s_ok);
            if(name=="RenderFile"){const auto result=co_await render_file(read_text(call(1)));co_return finish(result);}
            if(name=="AddSourceFilter" || name=="AddSourceFilterForMoniker"){
                const auto out=name=="AddSourceFilter"?call(3):call(4);if(!out)co_return finish(e_pointer);store_pointer(out,0);
                if(name=="AddSourceFilterForMoniker")co_return finish(e_notimpl);
                auto path=read_text(call(1));if(path.empty())co_return finish(e_invalidarg);
                try{path=normalized_windows_path(std::move(path),runtime_.current_directory);}catch(const std::invalid_argument&){co_return finish(file_not_found);}
                if(filesystem_.information(utf16_to_utf8(path)).error)co_return finish(file_not_found);
                std::uint64_t filter=0;try{filter=create_directshow_object(m,clsid_async_reader);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
                auto label=read_text(call(2));if(label.empty())label=path;
                auto& ours=ds.filters.at(filter);ours.graph=object;ours.name=label.substr(0,127);ours.references=2; // 圖與呼叫端各一個參考
                graph.filters.push_back(filter);graph.names.push_back(ours.name);store_pointer(out,filter);co_return finish(s_ok);
            }
            co_return finish(e_notimpl);
        }
        if(index==control_interface){
            if(name=="Run"){
                if(graph.video && !graph.owner && !graph.own_window && graph.auto_show){
                    // 沒有擁有者的視訊轉譯：建立頂層「VideoRenderer」視窗（不清除背景，畫面由影片繪製填滿）。
                    const auto window=co_await create_video_window(object);
                    found=ds.graphs.find(object);if(found==ds.graphs.end())co_return finish(e_unexpected);
                    found->second.own_window=window;bind_video(found->second);
                }
                auto& current=ds.graphs.at(object);current.state=2;
                if(auto* device=video_device(current)){
                    const auto now=runtime_.clock_ticks;
                    if(device->mode==mci::Mode::Paused)device->resume(now);
                    else if(device->mode==mci::Mode::Stopped && !current.completed && device->position<device->length())device->play(device->position,device->length(),false,false,now);
                    apply_volume(current);
                }else set_state(2);
                co_return finish(s_ok);}
            if(name=="Pause"){set_state(1);co_return finish(s_ok);}
            if(name=="Stop" || name=="StopWhenReady"){set_state(0);co_return finish(s_ok);}
            if(name=="GetState"){if(!call(2))co_return finish(e_pointer);store32(call(2),graph.state);co_return finish(s_ok);}
            if(name=="RenderFile"){
                // BSTR：前置長度（位元組）＋UTF-16。
                const auto text=call(1);if(!text)co_return finish(e_invalidarg);
                const auto bytes=static_cast<std::uint32_t>(m.load(GuestAddress{text-4},32));
                const auto file=read_utf16_counted(m,GuestAddress{text},std::min<std::uint32_t>(bytes/2,32767));
                co_return finish(co_await render_file(file));
            }
            if(name=="AddSourceFilter"){if(call(2))store_pointer(call(2),0);co_return finish(e_notimpl);}
            if(name=="get_FilterCollection" || name=="get_RegFilterCollection"){if(call(1))store_pointer(call(1),0);co_return finish(e_notimpl);}
            co_return finish(e_notimpl);
        }
        if(index==event_interface){
            if(name=="GetEventHandle"){if(!call(1))co_return finish(e_pointer);store_pointer(call(1),make_event(graph.event_handle,!graph.events.empty()));co_return finish(s_ok);}
            if(name=="GetEvent"){
                if(!call(1) || !call(2) || !call(3))co_return finish(e_pointer);
                if(graph.events.empty()){
                    // 指定逾時時等待事件（以事件 handle）；0 立即回傳 E_ABORT。
                    const auto timeout=static_cast<std::uint32_t>(call(4));
                    if(timeout){const auto handle=make_event(graph.event_handle,false);(void)co_await service_callback::call_value(resolve("kernel32.dll","WaitForSingleObject").value,{handle,timeout});}
                    found=ds.graphs.find(object);if(found==ds.graphs.end() || found->second.events.empty())co_return finish(e_abort);
                }
                auto& current=ds.graphs.at(object);const auto event=current.events.front();current.events.pop_front();
                store32(call(1),event[0]);store_pointer(call(2),event[1]);store_pointer(call(3),event[2]);
                if(current.events.empty() && current.event_handle)if(auto handle=runtime_.wait_objects.find(current.event_handle);handle!=runtime_.wait_objects.end())handle->second.set_signal(false);
                co_return finish(s_ok);
            }
            if(name=="WaitForCompletion"){
                if(!call(2))co_return finish(e_pointer);store32(call(2),0);
                if(graph.state!=2 && !graph.completed)co_return finish(vfw_e_wrong_state);
                if(!graph.completed){
                    const auto timeout=static_cast<std::uint32_t>(call(1));if(!timeout)co_return finish(e_abort);
                    const auto handle=make_event(graph.completion_handle,false);
                    (void)co_await service_callback::call_value(resolve("kernel32.dll","WaitForSingleObject").value,{handle,timeout});
                    found=ds.graphs.find(object);if(found==ds.graphs.end() || !found->second.completed)co_return finish(e_abort);
                }
                store32(call(2),ec_complete);co_return finish(s_ok);
            }
            if(name=="CancelDefaultHandling" || name=="RestoreDefaultHandling" || name=="FreeEventParams")co_return finish(s_ok);
            if(name=="SetNotifyWindow"){graph.notify_window=call(1);graph.notify_message=static_cast<std::uint32_t>(call(2));graph.notify_instance=call(3);co_return finish(s_ok);}
            if(name=="SetNotifyFlags"){const auto flags=static_cast<std::uint32_t>(call(1));if(flags>1)co_return finish(e_invalidarg);graph.notify_flags=flags;co_return finish(s_ok);}
            if(name=="GetNotifyFlags"){if(!call(1))co_return finish(e_pointer);store32(call(1),graph.notify_flags);co_return finish(s_ok);}
            co_return finish(e_notimpl);
        }
        if(index==audio_interface){
            if(name=="put_Volume"){const auto value=static_cast<long>(static_cast<std::int32_t>(call(1)));if(value<-10000 || value>0)co_return finish(e_invalidarg);graph.volume=value;apply_volume(graph);co_return finish(s_ok);}
            if(name=="get_Volume"){if(!call(1))co_return finish(e_pointer);store32(call(1),static_cast<std::uint32_t>(graph.volume));co_return finish(s_ok);}
            if(name=="put_Balance"){const auto value=static_cast<long>(static_cast<std::int32_t>(call(1)));if(value<-10000 || value>10000)co_return finish(e_invalidarg);graph.balance=value;apply_volume(graph);co_return finish(s_ok);}
            if(name=="get_Balance"){if(!call(1))co_return finish(e_pointer);store32(call(1),static_cast<std::uint32_t>(graph.balance));co_return finish(s_ok);}
            co_return finish(e_notimpl);
        }
        const auto media_time=[&](std::uint64_t pointer){const auto id=guid_at(pointer);return id==time_format_media_time;};
        if(index==seeking_interface){
            constexpr std::uint32_t capabilities=0x3fU;const bool media=graph.buffer || graph.video;
            if(name=="GetCapabilities"){if(!call(1))co_return finish(e_pointer);store32(call(1),media?capabilities:0U);co_return finish(s_ok);}
            if(name=="CheckCapabilities"){
                if(!call(1))co_return finish(e_pointer);const auto wanted=static_cast<std::uint32_t>(m.load(GuestAddress{call(1)},32));const auto have=media?capabilities:0U;
                store32(call(1),wanted&have);co_return finish((wanted&have)==wanted?s_ok:(wanted&have)?s_false:e_fail);}
            if(name=="IsFormatSupported"){if(!call(1))co_return finish(e_pointer);co_return finish(media_time(call(1))?s_ok:s_false);}
            if(name=="QueryPreferredFormat" || name=="GetTimeFormat"){if(!call(1))co_return finish(e_pointer);m.write(GuestAddress{call(1)},time_format_media_time);co_return finish(s_ok);}
            if(name=="IsUsingTimeFormat"){if(!call(1))co_return finish(e_pointer);co_return finish(media_time(call(1))?s_ok:s_false);}
            if(name=="SetTimeFormat"){if(!call(1))co_return finish(e_pointer);const auto id=guid_at(call(1));
                if(graph.state!=0)co_return finish(vfw_e_not_stopped);co_return finish(id==time_format_media_time || id==time_format_none?s_ok:e_invalidarg);}
            if(name=="GetDuration"){if(!call(1))co_return finish(e_pointer);if(!media)co_return finish(e_notimpl);store64(call(1),graph.duration);co_return finish(s_ok);}
            if(name=="GetStopPosition"){if(!call(1))co_return finish(e_pointer);if(!media)co_return finish(e_notimpl);store64(call(1),graph.stop.value_or(graph.duration));co_return finish(s_ok);}
            if(name=="GetCurrentPosition"){if(!call(1))co_return finish(e_pointer);if(!media)co_return finish(e_notimpl);store64(call(1),position_of(graph));co_return finish(s_ok);}
            if(name=="ConvertTimeFormat"){
                if(!call(1))co_return finish(e_pointer);
                const bool target=!call(2) || media_time(call(2)),source=!call(4) || media_time(call(4));
                if(!target || !source)co_return finish(e_invalidarg);store64(call(1),call(3));co_return finish(s_ok);}
            if(name=="SetPositions"){
                if(!media)co_return finish(e_notimpl);
                const auto current_flags=static_cast<std::uint32_t>(call(2)),stop_flags=static_cast<std::uint32_t>(call(4));
                const auto resolve_position=[&](std::uint64_t pointer,std::uint32_t flags,std::uint64_t base)->std::optional<std::uint64_t>{
                    const auto mode=flags&3U;if(!mode || !pointer)return std::nullopt;
                    const auto value=static_cast<std::int64_t>(m.load(GuestAddress{pointer},64));
                    const auto absolute=mode==1?value:static_cast<std::int64_t>(base)+value;
                    return static_cast<std::uint64_t>(std::clamp<std::int64_t>(absolute,0,static_cast<std::int64_t>(graph.duration)));};
                const auto now=position_of(graph);
                if(const auto stop=resolve_position(call(3),stop_flags,graph.stop.value_or(graph.duration)))graph.stop=*stop>=graph.duration?std::optional<std::uint64_t>{}:stop;
                if(const auto start=resolve_position(call(1),current_flags,now))seek(graph,*start);
                if((current_flags&8U) && call(1))store64(call(1),position_of(graph));
                if((stop_flags&8U) && call(3))store64(call(3),graph.stop.value_or(graph.duration));
                co_return finish(s_ok);
            }
            if(name=="GetPositions"){if(call(1))store64(call(1),position_of(graph));if(call(2))store64(call(2),graph.stop.value_or(graph.duration));co_return finish(media?s_ok:e_notimpl);}
            if(name=="GetAvailable"){if(call(1))store64(call(1),0);if(call(2))store64(call(2),graph.duration);co_return finish(media?s_ok:e_notimpl);}
            if(name=="SetRate"){
                const auto rate=double_arg(1);if(!(rate>0) || rate>16)co_return finish(e_invalidarg);graph.speed=rate;
                if(graph.buffer){std::lock_guard lock(graph.buffer->mutex);graph.buffer->frequency=std::max(1U,static_cast<unsigned>(std::lround(graph.rate*rate)));}
                co_return finish(s_ok);}
            if(name=="GetRate"){if(!call(1))co_return finish(e_pointer);store_double(call(1),graph.speed);co_return finish(s_ok);}
            if(name=="GetPreroll"){if(!call(1))co_return finish(e_pointer);store64(call(1),0);co_return finish(s_ok);}
            co_return finish(e_notimpl);
        }
        if(index==position_interface){
            const auto seconds=[](std::uint64_t time){return static_cast<double>(time)/10000000.0;};
            const auto ticks=[&](double value)->std::optional<std::uint64_t>{if(!std::isfinite(value) || value<0)return std::nullopt;return static_cast<std::uint64_t>(std::min(value*10000000.0,static_cast<double>(graph.duration)));};
            if(!graph.buffer && !graph.video && name!="get_Rate" && name!="put_Rate")co_return finish(e_notimpl);
            if(name=="get_Duration"){if(!call(1))co_return finish(e_pointer);store_double(call(1),seconds(graph.duration));co_return finish(s_ok);}
            if(name=="put_CurrentPosition"){const auto value=ticks(double_arg(1));if(!value)co_return finish(e_invalidarg);seek(graph,*value);co_return finish(s_ok);}
            if(name=="get_CurrentPosition"){if(!call(1))co_return finish(e_pointer);store_double(call(1),seconds(position_of(graph)));co_return finish(s_ok);}
            if(name=="get_StopTime"){if(!call(1))co_return finish(e_pointer);store_double(call(1),seconds(graph.stop.value_or(graph.duration)));co_return finish(s_ok);}
            if(name=="put_StopTime"){const auto value=ticks(double_arg(1));if(!value)co_return finish(e_invalidarg);graph.stop=*value>=graph.duration?std::optional<std::uint64_t>{}:value;co_return finish(s_ok);}
            if(name=="get_PrerollTime"){if(!call(1))co_return finish(e_pointer);store_double(call(1),0);co_return finish(s_ok);}
            if(name=="put_PrerollTime")co_return finish(s_ok);
            if(name=="put_Rate"){const auto rate=double_arg(1);if(!(rate>0) || rate>16)co_return finish(e_invalidarg);graph.speed=rate;
                if(graph.buffer){std::lock_guard lock(graph.buffer->mutex);graph.buffer->frequency=std::max(1U,static_cast<unsigned>(std::lround(graph.rate*rate)));}co_return finish(s_ok);}
            if(name=="get_Rate"){if(!call(1))co_return finish(e_pointer);store_double(call(1),graph.speed);co_return finish(s_ok);}
            if(name=="CanSeekForward" || name=="CanSeekBackward"){if(!call(1))co_return finish(e_pointer);store32(call(1),UINT32_MAX);co_return finish(s_ok);} // OATRUE
            co_return finish(e_notimpl);
        }
        // IVideoWindow／IBasicVideo：沒有視訊時與 Windows 相同回 E_NOINTERFACE。
        if(!graph.video)co_return finish(e_nointerface);
        const auto value32=[&](unsigned index){return static_cast<std::int32_t>(static_cast<std::uint32_t>(call(index)));};
        const auto get32=[&](std::uint64_t value)->std::uint32_t{if(!call(1))return e_pointer;store32(call(1),value);return s_ok;};
        const auto rebind=[&]{bind_video(graph);
            if(graph.own_window && gui_.windows.contains(graph.own_window))present_gui(graph.own_window);};
        if(index==video_window_interface){
            if(name=="put_Caption"){
                const auto text=call(1);graph.caption=text?read_utf16_counted(m,GuestAddress{text},std::min<std::uint32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{text-4},32))/2,32767)):std::u16string{};
                if(graph.own_window && gui_.windows.contains(graph.own_window)){
                    const auto bytes=utf16_bytes(graph.caption);const auto block=allocate_heap(m,bytes.size()+2);if(!block)co_return finish(e_outofmemory);
                    m.fill(GuestAddress{block},0,bytes.size()+2);m.write(GuestAddress{block},bytes);
                    (void)co_await service_callback::call_value(resolve("user32.dll","SetWindowTextW").value,{graph.own_window,block});(void)free_heap(m,block,s);}
                co_return finish(s_ok);}
            if(name=="get_Caption"){
                if(!call(1))co_return finish(e_pointer);const auto caption=ds.graphs.at(object).caption;const auto bytes=utf16_bytes(caption);
                const auto block=allocate_heap(m,bytes.size()+2);if(!block)co_return finish(e_outofmemory);m.fill(GuestAddress{block},0,bytes.size()+2);m.write(GuestAddress{block},bytes);
                const auto bstr=co_await service_callback::call_value(resolve("oleaut32.dll","SysAllocString").value,{block});(void)free_heap(m,block,s);
                store_pointer(call(1),bstr);co_return finish(bstr?s_ok:e_outofmemory);}
            if(name=="put_WindowStyle"){graph.window_style=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
            if(name=="get_WindowStyle")co_return finish(get32(graph.window_style));
            if(name=="put_WindowStyleEx"){graph.window_style_ex=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
            if(name=="get_WindowStyleEx")co_return finish(get32(graph.window_style_ex));
            if(name=="put_AutoShow"){graph.auto_show=value32(1)!=0;co_return finish(s_ok);}
            if(name=="get_AutoShow")co_return finish(get32(graph.auto_show?UINT32_MAX:0U));
            if(name=="put_WindowState"){const auto before=shown_owner_area(graph);graph.window_state=static_cast<std::uint32_t>(call(1));graph.visible=graph.window_state!=0;rebind();
                if(before && !shown_owner_area(graph))(void)co_await expose_owner(*before);co_return finish(s_ok);}
            if(name=="get_WindowState")co_return finish(get32(graph.window_state));
            if(name=="put_BackgroundPalette"){graph.background_palette=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
            if(name=="get_BackgroundPalette")co_return finish(get32(graph.background_palette));
            if(name=="put_Visible"){
                const auto before=shown_owner_area(graph);
                graph.visible=value32(1)!=0;if(graph.visible && !graph.window_state)graph.window_state=1;rebind();
                const auto own_window=graph.own_window;const bool visible=graph.visible;const auto after=shown_owner_area(graph);
                if(own_window && gui_.windows.contains(own_window))(void)co_await service_callback::call_value(resolve("user32.dll","ShowWindow").value,{own_window,visible?5U:0U});
                if(before && !after)(void)co_await expose_owner(*before);
                co_return finish(s_ok);}
            if(name=="get_Visible")co_return finish(get32(graph.visible?UINT32_MAX:0U));
            if(name=="put_Left"){graph.left=value32(1);graph.positioned=true;rebind();co_return finish(s_ok);}
            if(name=="get_Left")co_return finish(get32(static_cast<std::uint32_t>(graph.left)));
            if(name=="put_Top"){graph.top=value32(1);graph.positioned=true;rebind();co_return finish(s_ok);}
            if(name=="get_Top")co_return finish(get32(static_cast<std::uint32_t>(graph.top)));
            if(name=="put_Width"){if(value32(1)<=0)co_return finish(e_invalidarg);graph.width=value32(1);graph.positioned=true;rebind();co_return finish(s_ok);}
            if(name=="get_Width")co_return finish(get32(static_cast<std::uint32_t>(graph.width)));
            if(name=="put_Height"){if(value32(1)<=0)co_return finish(e_invalidarg);graph.height=value32(1);graph.positioned=true;rebind();co_return finish(s_ok);}
            if(name=="get_Height")co_return finish(get32(static_cast<std::uint32_t>(graph.height)));
            if(name=="put_Owner"){const auto before=shown_owner_area(graph);graph.owner=gui_expand_short_window(gui_,call(1));rebind();
                if(before && before->first!=graph.owner)(void)co_await expose_owner(*before);co_return finish(s_ok);}
            if(name=="get_Owner"){if(!call(1))co_return finish(e_pointer);store_pointer(call(1),graph.owner);co_return finish(s_ok);}
            if(name=="put_MessageDrain"){graph.message_drain=call(1);co_return finish(s_ok);}
            if(name=="get_MessageDrain"){if(!call(1))co_return finish(e_pointer);store_pointer(call(1),graph.message_drain);co_return finish(s_ok);}
            if(name=="get_BorderColor")co_return finish(get32(graph.border_color));
            if(name=="put_BorderColor"){graph.border_color=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
            if(name=="get_FullScreenMode")co_return finish(get32(graph.fullscreen?UINT32_MAX:0U));
            // 全螢幕模式：畫面延展填滿擁有者（或自建）視窗的客戶區，不切換宿主解析度。
            if(name=="put_FullScreenMode"){graph.fullscreen=value32(1)!=0;rebind();co_return finish(s_ok);}
            if(name=="SetWindowForeground" || name=="NotifyOwnerMessage")co_return finish(s_ok);
            if(name=="SetWindowPosition"){
                if(value32(3)<=0 || value32(4)<=0)co_return finish(e_invalidarg);
                graph.left=value32(1);graph.top=value32(2);graph.width=value32(3);graph.height=value32(4);graph.positioned=true;rebind();co_return finish(s_ok);}
            if(name=="GetWindowPosition" || name=="GetRestorePosition"){
                if(!call(1) || !call(2) || !call(3) || !call(4))co_return finish(e_pointer);
                store32(call(1),static_cast<std::uint32_t>(graph.left));store32(call(2),static_cast<std::uint32_t>(graph.top));
                store32(call(3),static_cast<std::uint32_t>(graph.width));store32(call(4),static_cast<std::uint32_t>(graph.height));co_return finish(s_ok);}
            if(name=="GetMinIdealImageSize" || name=="GetMaxIdealImageSize"){
                if(!call(1) || !call(2))co_return finish(e_pointer);store32(call(1),graph.video_width);store32(call(2),graph.video_height);co_return finish(s_ok);}
            if(name=="HideCursor"){graph.cursor_hidden=value32(1)!=0;co_return finish(s_ok);}
            if(name=="IsCursorHidden")co_return finish(get32(graph.cursor_hidden?UINT32_MAX:0U));
            co_return finish(e_notimpl);
        }
        if(index==basic_video_interface){
            const auto w=static_cast<std::int32_t>(graph.video_width),h=static_cast<std::int32_t>(graph.video_height);
            const auto default_destination=GuiRect{0,0,graph.width,graph.height};
            auto source=graph.default_source?GuiRect{0,0,w,h}:graph.source;
            auto destination=graph.default_destination?default_destination:graph.destination;
            const auto set_source=[&](GuiRect next)->std::uint32_t{
                if(next[0]<0 || next[1]<0 || next[0]>=next[2] || next[1]>=next[3] || next[2]>w || next[3]>h)return e_invalidarg;
                graph.source=next;graph.default_source=next==GuiRect{0,0,w,h};rebind();return s_ok;};
            const auto set_destination=[&](GuiRect next)->std::uint32_t{
                if(next[0]>=next[2] || next[1]>=next[3])return e_invalidarg;graph.destination=next;graph.default_destination=false;rebind();return s_ok;};
            if(name=="get_AvgTimePerFrame"){if(!call(1))co_return finish(e_pointer);store_double(call(1),static_cast<double>(graph.frame_time)/10000000.0);co_return finish(s_ok);}
            if(name=="get_BitRate" || name=="get_BitErrorRate")co_return finish(get32(0));
            if(name=="get_VideoWidth")co_return finish(get32(graph.video_width));
            if(name=="get_VideoHeight")co_return finish(get32(graph.video_height));
            if(name=="get_SourceLeft")co_return finish(get32(static_cast<std::uint32_t>(source[0])));
            if(name=="get_SourceTop")co_return finish(get32(static_cast<std::uint32_t>(source[1])));
            if(name=="get_SourceWidth")co_return finish(get32(static_cast<std::uint32_t>(source[2]-source[0])));
            if(name=="get_SourceHeight")co_return finish(get32(static_cast<std::uint32_t>(source[3]-source[1])));
            if(name=="put_SourceLeft"){const auto width=source[2]-source[0];co_return finish(set_source({value32(1),source[1],value32(1)+width,source[3]}));}
            if(name=="put_SourceTop"){const auto height=source[3]-source[1];co_return finish(set_source({source[0],value32(1),source[2],value32(1)+height}));}
            if(name=="put_SourceWidth")co_return finish(set_source({source[0],source[1],source[0]+value32(1),source[3]}));
            if(name=="put_SourceHeight")co_return finish(set_source({source[0],source[1],source[2],source[1]+value32(1)}));
            if(name=="get_DestinationLeft")co_return finish(get32(static_cast<std::uint32_t>(destination[0])));
            if(name=="get_DestinationTop")co_return finish(get32(static_cast<std::uint32_t>(destination[1])));
            if(name=="get_DestinationWidth")co_return finish(get32(static_cast<std::uint32_t>(destination[2]-destination[0])));
            if(name=="get_DestinationHeight")co_return finish(get32(static_cast<std::uint32_t>(destination[3]-destination[1])));
            if(name=="put_DestinationLeft"){const auto width=destination[2]-destination[0];co_return finish(set_destination({value32(1),destination[1],value32(1)+width,destination[3]}));}
            if(name=="put_DestinationTop"){const auto height=destination[3]-destination[1];co_return finish(set_destination({destination[0],value32(1),destination[2],value32(1)+height}));}
            if(name=="put_DestinationWidth")co_return finish(set_destination({destination[0],destination[1],destination[0]+value32(1),destination[3]}));
            if(name=="put_DestinationHeight")co_return finish(set_destination({destination[0],destination[1],destination[2],destination[1]+value32(1)}));
            if(name=="SetSourcePosition")co_return finish(set_source({value32(1),value32(2),value32(1)+value32(3),value32(2)+value32(4)}));
            if(name=="SetDestinationPosition")co_return finish(set_destination({value32(1),value32(2),value32(1)+value32(3),value32(2)+value32(4)}));
            if(name=="GetSourcePosition" || name=="GetDestinationPosition"){
                const auto& rect=name=="GetSourcePosition"?source:destination;if(!call(1) || !call(2) || !call(3) || !call(4))co_return finish(e_pointer);
                store32(call(1),static_cast<std::uint32_t>(rect[0]));store32(call(2),static_cast<std::uint32_t>(rect[1]));
                store32(call(3),static_cast<std::uint32_t>(rect[2]-rect[0]));store32(call(4),static_cast<std::uint32_t>(rect[3]-rect[1]));co_return finish(s_ok);}
            if(name=="SetDefaultSourcePosition"){graph.default_source=true;rebind();co_return finish(s_ok);}
            if(name=="SetDefaultDestinationPosition"){graph.default_destination=true;rebind();co_return finish(s_ok);}
            if(name=="GetVideoSize"){if(!call(1) || !call(2))co_return finish(e_pointer);store32(call(1),graph.video_width);store32(call(2),graph.video_height);co_return finish(s_ok);}
            if(name=="GetVideoPaletteEntries")co_return finish(0x80040220U); // VFW_E_NO_PALETTE_AVAILABLE
            if(name=="GetCurrentImage")co_return finish(e_notimpl);
            if(name=="IsUsingDefaultSource")co_return finish(graph.default_source?s_ok:s_false);
            if(name=="IsUsingDefaultDestination")co_return finish(graph.default_destination?s_ok:s_false);
            co_return finish(e_notimpl);
        }
        co_return finish(e_notimpl);
    });
}
}
