#include "x64arm/windows_services.hpp"
#include "x64arm/audio.hpp"
#include "x64arm/unicode.hpp"
#include "wasapi_interfaces.hpp"
#include "audio_endpoints.hpp"
#include "wave_device_message.hpp"
#include "pcm_wave_format.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <cstdlib>
#include <iostream>

namespace x64arm {
namespace {
using wasapi::Guid;
constexpr std::uint32_t pointer_error=0x80004003U,invalid=0x80070057U,no_interface=0x80004002U,
    not_initialized=0x88890001U,already_initialized=0x88890002U,not_stopped=0x88890005U,
    too_large=0x88890006U,out_of_order=0x88890007U,unsupported_format=0x88890008U,
    invalid_size=0x88890009U,operation_pending=0x8889000bU,exclusive_denied=0x8889000eU;
constexpr Guid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid factory_iid{1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid float_format{3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
enum Kind:unsigned {Enumerator,Collection,Device,Endpoint,Properties,Client,Render,Clock,Volume,SessionControl,Client2,Activation,Factory};
constexpr Guid interfaces[]{
    {0xd2,0x64,0x56,0xa9,0x14,0x96,0x35,0x4f,0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6},
    {0xbe,0xa1,0xd7,0x0b,0x1a,0x7a,0xdb,0x44,0x83,0x97,0xcc,0x53,0x92,0x38,0x7b,0x5e},
    {0x3f,0x06,0x66,0xd6,0x87,0x15,0x43,0x4e,0x81,0xf1,0xb9,0x48,0xe8,0x07,0x36,0x3f},
    {0x88,0x97,0xe0,0x1b,0x94,0x68,0x89,0x40,0x85,0x86,0x9a,0x2a,0x6c,0x26,0x5a,0xc5},
    {0xeb,0x8e,0x6d,0x88,0xf2,0x8c,0x46,0x44,0x8d,0x02,0xcd,0xba,0x1d,0xbd,0xcf,0x99},
    {0x4c,0xad,0xb9,0x1c,0xfa,0xdb,0x32,0x4c,0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2},
    {0xfc,0xac,0x94,0xf2,0x46,0x31,0x83,0x44,0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2},
    {0x4f,0x31,0x63,0xcd,0xba,0x3f,0x1b,0x4a,0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7},
    {0x87,0x48,0x01,0x93,0x2d,0x24,0x68,0x40,0x8a,0x15,0xcf,0x5e,0x93,0xb9,0x0f,0xe3},
    {0x99,0xa5,0xb1,0xf4,0x66,0x72,0x19,0x43,0xa8,0xca,0xe7,0x0a,0xcb,0x11,0xe8,0xcd},
    {0xcd,0x78,0x67,0x72,0x0a,0xf6,0xda,0x4e,0x82,0xde,0xe4,0x76,0x10,0xcd,0x78,0xaa},
    {0x78,0x2d,0xa2,0x72,0xe4,0xcd,0x1d,0x43,0xb8,0xcc,0x84,0x3a,0x71,0x19,0x9b,0x6d},factory_iid};
struct Method {std::string_view name;unsigned args;std::uint32_t wide{},narrow{};};
constexpr Method common[]={{"QueryInterface",3},{"AddRef",1},{"Release",1}};
constexpr Method enumerator[]={{"E_EnumAudioEndpoints",4,0,6},{"E_GetDefaultAudioEndpoint",4,0,6},{"E_GetDevice",3},{"E_Register",2},{"E_Unregister",2}};
constexpr Method collection[]={{"D_GetCount",2},{"D_Item",3,0,2}};
constexpr Method device[]={{"D_Activate",5,0,4},{"D_OpenPropertyStore",3,0,2},{"D_GetId",2},{"D_GetState",2}};
constexpr Method endpoint[]={{"D_GetDataFlow",2}};
constexpr Method properties[]={{"P_GetCount",2},{"P_GetAt",3,0,2},{"P_GetValue",3},{"P_SetValue",3},{"P_Commit",1}};
constexpr Method client[]={{"A_Initialize",7,24,6},{"A_GetBufferSize",2},{"A_GetStreamLatency",2},{"A_GetCurrentPadding",2},{"A_IsFormatSupported",4,0,2},{"A_GetMixFormat",2},{"A_GetDevicePeriod",3},{"A_Start",1},{"A_Stop",1},{"A_Reset",1},{"A_SetEventHandle",2},{"A_GetService",3}};
constexpr auto client2=[] {std::array<Method,std::size(client)+3> result{};std::copy(std::begin(client),std::end(client),result.begin());result[12]={"A_IsOffloadCapable",3,0,2};result[13]={"A_SetClientProperties",2};result[14]={"A_GetBufferSizeLimits",5,0,4};return result;}();
constexpr Method activation[]={{"O_GetActivateResult",3}};
constexpr Method render[]={{"R_GetBuffer",3,0,2},{"R_ReleaseBuffer",3,0,6}};
constexpr Method clock_methods[]={{"C_GetFrequency",2},{"C_GetPosition",3},{"C_GetCharacteristics",2}};
constexpr Method volume[]={{"V_GetChannelCount",2},{"V_SetChannelVolume",3,0,2},{"V_GetChannelVolume",3,0,2},{"V_SetAllVolumes",3,0,2},{"V_GetAllVolumes",3,0,2}};
constexpr Method session_control[]={{"S_GetState",2},{"S_GetDisplayName",2},{"S_SetDisplayName",3},{"S_GetIconPath",2},{"S_SetIconPath",3},{"S_GetGroupingParam",2},{"S_SetGroupingParam",3},{"S_Register",2},{"S_Unregister",2}};
constexpr Method factory[]={{"F_CreateInstance",4},{"F_LockServer",2,0,2}};
std::span<const Method> methods(Kind k){switch(k){case Enumerator:return enumerator;case Collection:return collection;case Device:return device;case Endpoint:return endpoint;case Properties:return properties;case Client:return client;case Client2:return client2;case Activation:return activation;case Render:return render;case Clock:return clock_methods;case Volume:return volume;case SessionControl:return session_control;case Factory:return factory;}return {};}
// 舊版固定 ID；WinMM 的 DRV_QUERYFUNCTIONINSTANCEID 仍回傳它，視為系統預設播放端點的別名。
constexpr std::u16string_view legacy_device_id=wave::output_id;
struct Property {Guid guid;std::uint32_t pid;};
constexpr Property property_keys[]{
    {{0x4e,0x25,0x5c,0xa4,0x1c,0xdf,0xfd,0x4e,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0},14}, // PKEY_Device_FriendlyName
    {{0x03,0xd8,0xa5,0x1d,0x92,0xd4,0xdd,0x4e,0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e},3},  // PKEY_AudioEndpoint_PhysicalSpeakers
    {{0x4d,0x06,0x9f,0xf1,0x2c,0x08,0x27,0x4e,0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c},0},  // PKEY_AudioEngine_DeviceFormat
    {{0xc8,0x4f,0xc3,0x78,0x4a,0x10,0xca,0x4a,0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57},256},// DEVPKEY_Device_InstanceId
    {{0x4e,0x25,0x5c,0xa4,0x1c,0xdf,0xfd,0x4e,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0},2},  // PKEY_Device_DeviceDesc
    {{0x03,0xd8,0xa5,0x1d,0x92,0xd4,0xdd,0x4e,0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e},0},  // PKEY_AudioEndpoint_FormFactor
    {{0x03,0xd8,0xa5,0x1d,0x92,0xd4,0xdd,0x4e,0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e},4},  // PKEY_AudioEndpoint_GUID
    {{0x6e,0x51,0x6e,0x02,0x14,0xb8,0x4b,0x41,0x83,0xcd,0x85,0x6d,0x6f,0xef,0x48,0x22},2}}; // PKEY_DeviceInterface_FriendlyName
// 端點目錄與 WinMM、DirectSound 共用（audio_endpoints.hpp）。
using EndpointInfo=audio_endpoint::Endpoint;
inline std::vector<EndpointInfo> render_endpoints(){return audio_endpoint::render();}
using audio_endpoint::form_factor;
struct PointerError {};
struct Stream;
struct Session {
    std::u16string display,icon;Guid grouping{};
    std::vector<std::weak_ptr<Stream>> streams;
    std::set<std::uint64_t> notifications;
    std::uint32_t state{2};unsigned controls{};
};
struct Stream {
    std::string device; // 空字串＝跟隨系統預設輸出
    std::shared_ptr<audio::Buffer> pcm=std::make_shared<audio::Buffer>();
    std::unique_ptr<audio::Output> output;
    PcmWaveFormat format;bool initialized{},started{},locked{},closed{};
    std::shared_ptr<Session> session;
    std::uint32_t flags{},capacity{},requested{},category{},options{};
    std::uint64_t data{},event{},clock_frames{},observed_frames{},event_frames{};
    std::array<float,2> volumes{1,1};
};
struct Object {
    Kind canonical{};std::uint32_t refs{1};std::map<Kind,std::uint64_t> pointers;std::shared_ptr<Stream> stream;std::shared_ptr<Session> session;unsigned count{};std::set<std::uint64_t> notifications;
    std::uint64_t activated{},handler{};std::uint32_t activation_result{};bool ready{};
    std::shared_ptr<EndpointInfo> endpoint;std::vector<EndpointInfo> items;
};
struct Entry {std::shared_ptr<Object> object;Kind kind;};
void advance(Stream& s){
    auto& p=*s.pcm;
    if(s.started)s.clock_frames+=p.rendered_frames-s.observed_frames;
    s.observed_frames=p.rendered_frames;
    const auto consumed=static_cast<std::size_t>(p.cursor);
    if(consumed){p.pcm.erase(p.pcm.begin(),p.pcm.begin()+static_cast<std::ptrdiff_t>(consumed*p.frame_bytes()));p.cursor-=static_cast<double>(consumed);}
}
}
struct WasapiState {std::map<std::uint64_t,Entry> objects;std::map<Kind,std::uint64_t> tables;std::map<Guid,std::weak_ptr<Session>> sessions;std::uint32_t locks{};};
std::span<const WindowsServiceDescriptor> wasapi_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{{"mmdevapi.dll","DllGetClassObject",3},{"mmdevapi.dll","DllCanUnloadNow",0},{"mmdevapi.dll","ActivateAudioInterfaceAsync",5},{"audioses.dll","DllGetClassObject",3},{"audioses.dll","DllCanUnloadNow",0},{"internal-wasapi","ActivateWorker",1},{"internal-wasapi","ActivateReady",1}};
        for(const auto& m:common)result.push_back({"internal-wasapi",m.name,m.args,m.wide,0,m.narrow});
        for(unsigned k=0;k<=Factory;++k)for(const auto& m:methods(static_cast<Kind>(k)))result.push_back({"internal-wasapi",m.name,m.args,m.wide,0,m.narrow});
        return result;
    }();return table;
}
bool WindowsServices::wasapi_waiting() const {
    if(!wasapi_)return false;
    for(const auto& [pointer,entry]:wasapi_->objects){(void)pointer;if(entry.kind==Client && entry.object->stream->started)return true;}
    return false;
}
void WindowsServices::poll_wasapi(){
    if(!wasapi_)return;
    for(const auto& [pointer,entry]:wasapi_->objects){
        (void)pointer;if(entry.kind!=Client)continue;auto& s=*entry.object->stream;
        std::lock_guard lock(s.pcm->mutex);advance(s);
        const auto period=std::max(1U,s.format.rate/100);
        if(s.started && s.event && s.clock_frames-s.event_frames>=period){
            s.event_frames=s.clock_frames;
            const auto event=runtime_.wait_objects.find(s.event);
            if(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event)event->second.set_signal(true);
        }
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_wasapi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("WASAPI 原生輸出不支援 deterministic compare");
    if(!wasapi_)wasapi_=std::make_shared<WasapiState>();auto& bridge=*wasapi_;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto check=[&](std::uint64_t p,std::size_t n){if(!p)throw PointerError{};memory.check(GuestAddress{p},n,Access::Write);};
    const auto put=[&](std::uint64_t p,std::uint64_t value,std::uint8_t width=32){check(p,width/8);memory.store(GuestAddress{p},width,value);};
    const auto out=[&](unsigned i){check(arg(i),unit);put(arg(i),0,bits);};
    const auto guid=[&](std::uint64_t p){
        if(!p)throw PointerError{};Guid g{};memory.read(GuestAddress{p},g);
        if(const auto trace=std::getenv("X64ARM_TRACE_AUDIO");trace && std::string_view(trace)=="1"){
            std::fprintf(stderr,"wasapi_guid method=%.*s bytes=",static_cast<int>(name.size()),name.data());
            for(const auto byte:g)std::fprintf(stderr,"%02x",static_cast<unsigned>(byte));std::fprintf(stderr,"\n");
        }
        return g;
    };
    const auto allocation=[&](std::span<const std::uint8_t> data){const auto p=allocate_heap(memory,data.size());if(!p)throw std::bad_alloc{};memory.write(GuestAddress{p},data);return p;};
    const auto text=[&](std::u16string_view value){return allocation(utf16_bytes(value));};
    const auto mix=[] {
        std::vector<std::uint8_t> bytes(40);const auto store=[&](unsigned at,std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        store(0,0xfffe,2);store(2,2,2);store(4,48000,4);store(8,384000,4);store(12,8,2);store(14,32,2);store(16,22,2);store(18,32,2);store(20,3,4);std::copy(float_format.begin(),float_format.end(),bytes.begin()+24);return bytes;
    };
    const auto format=[&](std::uint64_t p){if(!p)throw PointerError{};return read_pcm_wave_format(memory,p,true,true,8000,192000);};
    const auto create=[&](const std::shared_ptr<Object>& object,Kind k){
        if(object->pointers.contains(k))return object->pointers.at(k);
        if(bridge.objects.size()>=1024)throw std::bad_alloc{};
        if(!bridge.tables.contains(k)){
            const auto extra=methods(k);const auto table=runtime_allocate(memory,{},(3+extra.size())*unit);
            for(unsigned i=0;i<3;++i)put(table+i*unit,resolve("internal-wasapi",common[i].name).value,bits);
            for(std::size_t i=0;i<extra.size();++i)put(table+(i+3)*unit,resolve("internal-wasapi",extra[i].name).value,bits);
            bridge.tables.emplace(k,table);
        }
        const auto p=allocate_heap(memory,unit);if(!p)throw std::bad_alloc{};
        try{put(p,bridge.tables.at(k),bits);object->pointers.emplace(k,p);bridge.objects.emplace(p,Entry{object,k});}catch(...){object->pointers.erase(k);(void)free_heap(memory,p,state);throw;}return p;
    };
    const auto make=[&](Kind k,std::shared_ptr<Stream> stream={},unsigned count=0){auto object=std::make_shared<Object>();object->canonical=k;object->stream=std::move(stream);object->count=count;return create(object,k);};
    const auto make_device=[&](const EndpointInfo& info){auto object=std::make_shared<Object>();object->canonical=Device;object->endpoint=std::make_shared<EndpointInfo>(info);return create(object,Device);};
    const auto present=[&](const EndpointInfo& info){const auto list=render_endpoints();return std::any_of(list.begin(),list.end(),[&](const auto& item){return item.device.uid==info.device.uid;});};
    const auto find_endpoint=[&](std::u16string id)->std::optional<EndpointInfo>{
        for(auto& c:id)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c-u'A'+u'a');
        auto list=render_endpoints();
        if(id==legacy_device_id || id==u"{e6327cad-dcec-4949-ae8a-991e976a79d2}"){if(!list.empty() && list.front().device.default_output)return list.front();return std::nullopt;}
        for(auto& item:list)if(item.id==id)return std::move(item);return std::nullopt;
    };
    const auto make_client=[&](Kind k,std::string host={}){
        auto object=std::make_shared<Object>();object->canonical=Client;object->stream=std::make_shared<Stream>();object->stream->device=std::move(host);
        const auto p=create(object,Client);
        try{return k==Client?p:create(object,k);}catch(...){bridge.objects.erase(p);(void)free_heap(memory,p,state);throw;}
    };
    std::vector<GuestCallback> callbacks;std::uint64_t event_data=0;
    const auto foreign_method=[&](std::uint64_t object,unsigned slot){
        if(!object)throw PointerError{};
        const auto table=memory.load(GuestAddress{object},bits),target=memory.load(GuestAddress{table+slot*unit},bits);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        return target;
    };
    const auto callback=[&](std::uint64_t object,unsigned slot,std::vector<std::uint64_t> values){
        callbacks.push_back({foreign_method(object,slot),std::move(values)});
    };
    const auto notify=[&](const std::shared_ptr<Session>& session,unsigned slot,const std::vector<std::uint64_t>& values){
        if(!session->controls)return;
        // 先固定整批 listener 引用，再開始通知，避免重入改變後續 callback 的生命週期。
        for(const auto p:session->notifications)callback(p,1,{p});
        for(const auto p:session->notifications){std::vector<std::uint64_t> args{p};args.insert(args.end(),values.begin(),values.end());callback(p,slot,std::move(args));}
        for(const auto p:session->notifications)callback(p,2,{p});
    };
    const auto update_session=[&](const std::shared_ptr<Session>& session){
        if(!session)return;
        std::uint32_t next=2;
        std::erase_if(session->streams,[](const auto& stream){return stream.expired();});
        for(const auto& weak:session->streams)if(const auto stream=weak.lock();stream && stream->initialized && !stream->closed){
            if(stream->started){next=1;break;}next=0;
        }
        if(next!=session->state){notify(session,8,{next});session->state=next;}
    };
    std::uint64_t result=0;
    try{
        if(name=="DllCanUnloadNow")result=bridge.objects.empty() && !bridge.locks?0:1;
        else if(name=="ActivateAudioInterfaceAsync"){
            out(4);if(!arg(0) || !arg(3))throw PointerError{};
            const auto iid=guid(arg(1));auto path=read_utf16(memory,GuestAddress{arg(0)},32768);
            for(auto& c:path)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c-u'A'+u'a');
            const auto params=arg(2)?memory.load(GuestAddress{arg(2)},16):0;
            (void)foreign_method(arg(3),2);(void)foreign_method(arg(3),3);callback(arg(3),1,{arg(3)});
            const auto p=make(Activation);auto operation=bridge.objects.at(p).object;
            try{
                // 預設介面路徑跟隨系統預設輸出；端點 ID 綁定該 Core Audio 裝置。
                const bool follows_default=path==legacy_device_id || path==u"{e6327cad-dcec-4949-ae8a-991e976a79d2}";const auto target=find_endpoint(path);
                if(!target)operation->activation_result=0x80070490U;
                else if(iid!=interfaces[Client] && iid!=interfaces[Client2])operation->activation_result=no_interface;
                else if(params)operation->activation_result=invalid;
                else if(!audio::output_available())operation->activation_result=0x88890004U;
                else operation->activated=make_client(iid==interfaces[Client2]?Client2:Client,follows_default?std::string{}:target->device.uid);
                // caller 與 worker 各持一份 operation；先完成 handler AddRef 才喚醒 worker。
                callbacks.reserve(3);
                const auto worker=create_guest_thread(state,memory,resolve("internal-wasapi","ActivateWorker").value,p,65536,4);
                if(!worker)throw std::bad_alloc{};
                operation->refs=2;operation->handler=arg(3);
                callbacks.push_back({resolve("kernel32.dll","ResumeThread").value,{worker}});
                callbacks.push_back({resolve("kernel32.dll","CloseHandle").value,{worker}});
                put(arg(4),p,bits);
            }catch(...){
                if(operation->activated){auto client_object=bridge.objects.at(operation->activated).object;for(const auto& [k,ptr]:client_object->pointers){(void)k;bridge.objects.erase(ptr);(void)free_heap(memory,ptr,state);}}
                bridge.objects.erase(p);(void)free_heap(memory,p,state);throw;
            }
        }
        else if(name=="CoCreateInstance" || name=="CoGetClassObject" || name=="DllGetClassObject"){
            const bool dll=name=="DllGetClassObject",factory_object=name!="CoCreateInstance";const unsigned output=dll?2:4;out(output);
            const auto cls=guid(arg(0)),iid=guid(arg(dll?1:3));
            if(dll && (service.family=="audioses.dll" || cls!=wasapi::clsid))result=0x80040111U;
            else if(cls!=wasapi::clsid)result=0x80040154U;
            else if(!dll && !(d(factory_object?1:2)&1U))result=0x80040154U;
            else if(!factory_object && arg(1))result=0x80040110U;
            else if(!factory_object && (!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16))result=0x800401f0U;
            else if(iid!=unknown && iid!=interfaces[factory_object?Factory:Enumerator])result=no_interface;
            else put(arg(output),make(factory_object?Factory:Enumerator),bits);
        }else{
            const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())throw PointerError{};
            auto object=found->second.object;const auto kind=found->second.kind;
            if(kind==Activation && name=="ActivateWorker"){
                // 完成通知在實際 MTA guest worker 執行；不能在呼叫端同步重入冒充非同步。
                callbacks.push_back({resolve("ole32.dll","CoInitializeEx").value,{0,0}});
                callbacks.push_back({resolve("internal-wasapi","ActivateReady").value,{arg(0)}});
                callback(object->handler,3,{object->handler,arg(0)});
                callback(arg(0),2,{arg(0)});
                callbacks.push_back({resolve("ole32.dll","CoUninitialize").value,{}});
            }else if(kind==Activation && name=="ActivateReady")object->ready=true;
            else if(name=="QueryInterface"){
                out(2);const auto iid=guid(arg(1));std::uint64_t p=0;
                if(iid==unknown || iid==interfaces[object->canonical])p=object->pointers.at(object->canonical);
                else if(iid==interfaces[kind])p=arg(0);
                else if(object->canonical==Device && iid==interfaces[Endpoint])p=create(object,Endpoint);
                else if(object->canonical==Client && iid==interfaces[Client2])p=create(object,Client2);
                if(p){if(object->refs==UINT32_MAX)throw std::bad_alloc{};++object->refs;put(arg(2),p,bits);}else result=no_interface;
            }else if(name=="AddRef"){if(object->refs==UINT32_MAX)throw std::bad_alloc{};result=++object->refs;}
            else if(name=="Release"){
                result=--object->refs;if(!result){
                    if(object->canonical==Activation){if(object->activated)callback(object->activated,2,{object->activated});if(object->handler)callback(object->handler,2,{object->handler});}
                    for(const auto& [k,p]:object->pointers){(void)k;bridge.objects.erase(p);(void)free_heap(memory,p,state);}
                    if(object->canonical==SessionControl && !--object->session->controls){object->session->display.clear();object->session->icon.clear();}
                    if(object->stream && object->stream.use_count()==1){
                        auto& stream=*object->stream;stream.closed=true;stream.output.reset();
                        if(stream.data)(void)free_heap(memory,stream.data,state);update_session(stream.session);
                    }
                }
            }else if(kind==Activation && name=="O_GetActivateResult"){
                check(arg(1),4);out(2);
                if(!object->ready)result=0x8000000eU;
                else {put(arg(1),object->activation_result);if(object->activated){auto activated=bridge.objects.at(object->activated).object;if(activated->refs==UINT32_MAX)throw std::bad_alloc{};++activated->refs;put(arg(2),object->activated,bits);}}
            }else if(kind==SessionControl){
                auto& session=*object->session;
                if(name=="S_GetState")put(arg(1),session.state);
                else if(name=="S_GetDisplayName" || name=="S_GetIconPath"){out(1);put(arg(1),text(name=="S_GetDisplayName"?session.display:session.icon),bits);}
                else if(name=="S_GetGroupingParam"){check(arg(1),16);memory.write(GuestAddress{arg(1)},session.grouping);}
                else if(name=="S_SetDisplayName" || name=="S_SetIconPath"){
                    if(!arg(1))throw PointerError{};
                    auto value=read_utf16(memory,GuestAddress{arg(1)});const auto context=arg(2)?guid(arg(2)):Guid{};
                    auto& current=name=="S_SetDisplayName"?session.display:session.icon;
                    if(value!=current){
                        if(session.controls && !session.notifications.empty()){
                            const auto bytes=utf16_bytes(value);event_data=allocate_heap(memory,16+bytes.size());if(!event_data)throw std::bad_alloc{};
                            memory.write(GuestAddress{event_data},context);memory.write(GuestAddress{event_data+16},bytes);
                            notify(object->session,name=="S_SetDisplayName"?3:4,{event_data+16,arg(2)?event_data:0});
                        }
                        current=std::move(value);
                    }
                }else if(name=="S_SetGroupingParam"){
                    const auto value=guid(arg(1)),context=arg(2)?guid(arg(2)):Guid{};
                    if(value!=session.grouping){
                        if(session.controls && !session.notifications.empty()){
                            event_data=allocate_heap(memory,32);if(!event_data)throw std::bad_alloc{};
                            memory.write(GuestAddress{event_data},value);memory.write(GuestAddress{event_data+16},context);
                            notify(object->session,7,{event_data,arg(2)?event_data+16:0});
                        }
                        session.grouping=value;
                    }
                }else if(name=="S_Register" || name=="S_Unregister"){
                    const auto listener=arg(1);if(!listener)throw PointerError{};
                    if(name=="S_Register"){
                        if(!session.notifications.contains(listener)){callback(listener,1,{listener});session.notifications.insert(listener);}
                    }else if(!session.notifications.contains(listener))result=0x80070490U;
                    else {callback(listener,2,{listener});session.notifications.erase(listener);}
                }else result=invalid;
            }else if(kind==Factory){
                if(name=="F_CreateInstance"){out(3);if(arg(1))result=0x80040110U;else if(guid(arg(2))!=unknown && guid(arg(2))!=interfaces[Enumerator])result=no_interface;else put(arg(3),make(Enumerator),bits);}
                else if(name=="F_LockServer"){if(d(1)){if(bridge.locks==UINT32_MAX)result=invalid;else ++bridge.locks;}else if(bridge.locks)--bridge.locks;else result=invalid;}
                else result=invalid;
            }else if(kind==Enumerator){
                if(name=="E_EnumAudioEndpoints"){out(3);if(d(1)>2 || !d(2) || (d(2)&~15U))result=invalid;else {
                    auto list=std::make_shared<Object>();list->canonical=Collection;if(d(1)!=1 && (d(2)&1))list->items=render_endpoints();
                    list->count=static_cast<unsigned>(list->items.size());put(arg(3),create(list,Collection),bits);}}
                else if(name=="E_GetDefaultAudioEndpoint"){out(3);if(d(1)>1 || d(2)>2)result=invalid;else {const auto list=d(1)?std::vector<EndpointInfo>{}:render_endpoints();
                    if(list.empty() || !list.front().device.default_output)result=0x80070490U;else put(arg(3),make_device(list.front()),bits);}}
                else if(name=="E_GetDevice"){out(2);if(!arg(1))throw PointerError{};const auto target=find_endpoint(read_utf16(memory,GuestAddress{arg(1)},256));if(!target)result=0x80070490U;else put(arg(2),make_device(*target),bits);}
                else if(name=="E_Register"){if(!arg(1))throw PointerError{};memory.check(GuestAddress{arg(1)},unit,Access::Read);object->notifications.insert(arg(1));}
                else if(name=="E_Unregister"){if(!arg(1))throw PointerError{};if(!object->notifications.erase(arg(1)))result=0x80070490U;}
                else result=invalid;
            }else if(kind==Collection){
                if(name=="D_GetCount")put(arg(1),object->count);
                else if(name=="D_Item"){out(2);if(d(1)>=object->count)result=invalid;else put(arg(2),make_device(object->items[d(1)]),bits);}else result=invalid;
            }else if(kind==Device || kind==Endpoint){
                if(name=="D_GetDataFlow" && kind==Endpoint)put(arg(1),0);
                else if(name=="D_GetId"){out(1);put(arg(1),text(object->endpoint->id),bits);}
                else if(name=="D_GetState")put(arg(1),present(*object->endpoint)?1U:4U); // DEVICE_STATE_ACTIVE／NOTPRESENT
                else if(name=="D_OpenPropertyStore"){out(2);if(d(1)!=0)result=0x80070005U;else {auto store=std::make_shared<Object>();store->canonical=Properties;store->endpoint=object->endpoint;put(arg(2),create(store,Properties),bits);}}
                else if(name=="D_Activate"){out(4);const auto iid=guid(arg(1));if(!(d(2)&1U))result=invalid;else if(arg(3))result=0x80004001U;else if(iid!=interfaces[Client] && iid!=interfaces[Client2])result=no_interface;else if(!audio::output_available() || !present(*object->endpoint))result=0x88890004U;else put(arg(4),make_client(iid==interfaces[Client2]?Client2:Client,object->endpoint->device.uid),bits);}
                else result=invalid;
            }else if(kind==Properties){
                if(name=="P_GetCount")put(arg(1),std::size(property_keys));
                else if(name=="P_GetAt"){check(arg(2),20);if(d(1)>=std::size(property_keys))result=invalid;else {memory.write(GuestAddress{arg(2)},property_keys[d(1)].guid);put(arg(2)+16,property_keys[d(1)].pid);}}
                else if(name=="P_GetValue"){
                    if(!arg(1))throw PointerError{};check(arg(2),x86_?16:24);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(x86_?16:24));const auto key=guid(arg(1));const auto pid=memory.load(GuestAddress{arg(1)+16},32);
                    unsigned index=0;for(;index<std::size(property_keys);++index)if(key==property_keys[index].guid && pid==property_keys[index].pid)break;
                    const auto& info=*object->endpoint;const auto adapter=info.device.manufacturer.empty()?info.device.name:info.device.manufacturer;
                    if(index==0 || index==4){put(arg(2),31,16);put(arg(2)+8,text(info.device.name.empty()?u"Core Audio":info.device.name),bits);}
                    else if(index==3){put(arg(2),31,16);put(arg(2)+8,text(u"SWD\\MMDEVAPI\\"+info.id),bits);}
                    else if(index==6){put(arg(2),31,16);put(arg(2)+8,text(info.guid_text),bits);}
                    else if(index==7){put(arg(2),31,16);put(arg(2)+8,text(adapter),bits);}
                    else if(index==5){put(arg(2),19,16);put(arg(2)+8,form_factor(info.device.transport));}
                    else if(index==1){put(arg(2),19,16);put(arg(2)+8,info.device.output_channels==1?4U:3U);}
                    else if(index==2){const auto bytes=mix();const auto data=allocation(bytes);put(arg(2),65,16);put(arg(2)+8,bytes.size());put(arg(2)+8+unit,data,bits);}
                }else if(name=="P_SetValue" || name=="P_Commit")result=0x80070005U;else result=invalid;
            }else if(object->stream && std::any_of(methods(kind).begin(),methods(kind).end(),[&](const auto& method){return method.name==name;})){
                auto& s=*object->stream;auto& pcm=*s.pcm;std::lock_guard lock(pcm.mutex);advance(s);
                if(name=="A_GetMixFormat"){out(1);put(arg(1),allocation(mix()),bits);}
                else if(name=="A_IsOffloadCapable"){check(arg(2),4);if(d(1)>14)result=invalid;else put(arg(2),0);}
                else if(name=="A_SetClientProperties"){
                    if(!arg(1))throw PointerError{};
                    const auto size=memory.load(GuestAddress{arg(1)},32);
                    if(size!=12 && size!=16)result=invalid;
                    else{
                        const auto offload=memory.load(GuestAddress{arg(1)+4},32),category=memory.load(GuestAddress{arg(1)+8},32);
                        const auto options=size==16?memory.load(GuestAddress{arg(1)+12},32):0;
                        if(s.initialized)result=already_initialized;
                        else if(category>14 || (options&~UINT64_C(3)))result=invalid;
                        else if(offload)result=0x88890022U; // Core Audio 共用 PCM 路徑沒有硬體 offload。
                        else{s.category=static_cast<std::uint32_t>(category);s.options=static_cast<std::uint32_t>(options);}
                    }
                }else if(name=="A_GetBufferSizeLimits"){
                    check(arg(3),8);check(arg(4),8);
                    if(!format(arg(1)))result=unsupported_format;
                    else{put(arg(3),100000,64);put(arg(4),20000000,64);}
                }
                else if(name=="A_GetDevicePeriod"){if(!arg(1) && !arg(2))throw PointerError{};if(arg(1))check(arg(1),8);if(arg(2))check(arg(2),8);if(arg(1))put(arg(1),100000,64);if(arg(2))put(arg(2),100000,64);}
                else if(name=="A_IsFormatSupported"){
                    if(arg(3))out(3);if(d(1)>1)result=invalid;else if(d(1))result=exclusive_denied;else if(!arg(3))throw PointerError{};else if(!format(arg(2))){put(arg(3),allocation(mix()),bits);result=1;}
                }else if(name=="A_Initialize"){
                    if(s.initialized)result=already_initialized;else if(d(1)>1)result=invalid;else if(d(1))result=exclusive_denied;
                    else if((d(2)&~0x800c0000U) || arg(4) || static_cast<std::int64_t>(arg(3))<0)result=invalid;
                    else if(const auto f=format(arg(5));!f)result=unsupported_format;
                    else if(arg(3)>UINT64_C(20000000))result=too_large;
                    else {
                        const auto session_id=arg(6)?guid(arg(6)):Guid{};
                        std::erase_if(bridge.sessions,[](const auto& item){return item.second.expired();});
                        auto& weak=bridge.sessions[session_id];auto session=weak.lock();
                        if(!session){session=std::make_shared<Session>();weak=session;}
                        session->streams.reserve(session->streams.size()+1);
                        const auto frames=std::max<std::uint64_t>(f->rate/100,(arg(3)*f->rate+9999999)/10000000);
                        const auto data=allocate_heap(memory,frames*f->align);if(!data)throw std::bad_alloc{};
                        try{pcm.channels=f->channels;pcm.bits=f->bits;pcm.floating=f->floating;pcm.rate=pcm.frequency=f->rate;pcm.streaming=true;s.output=audio::open_output(s.pcm,s.device);}catch(const std::runtime_error&){(void)free_heap(memory,data,state);result=0x8889000fU;}
                        if(!result){s.data=data;s.capacity=static_cast<std::uint32_t>(frames);s.format=*f;s.flags=d(2);s.initialized=true;s.observed_frames=pcm.rendered_frames;s.session=std::move(session);s.session->streams.push_back(object->stream);update_session(s.session);}
                    }
                }else if(!s.initialized)result=not_initialized;
                else if(name=="A_GetBufferSize")put(arg(1),s.capacity);
                else if(name=="A_GetCurrentPadding")put(arg(1),pcm.pcm.size()/pcm.frame_bytes());
                else if(name=="A_GetStreamLatency")put(arg(1),(static_cast<std::uint64_t>(s.capacity)*10000000+s.format.rate-1)/s.format.rate,64);
                else if(name=="A_SetEventHandle"){
                    if(!(s.flags&0x40000U))result=0x88890011U;else if(s.started)result=not_stopped;else {const auto event=runtime_.wait_objects.find(arg(1));if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event || !(event->second.access&2U))result=invalid;else s.event=arg(1);}
                }else if(name=="A_Start"){
                    if(s.started)result=not_stopped;else if((s.flags&0x40000U) && !s.event)result=0x88890014U;else {s.started=true;pcm.playing=true;s.observed_frames=pcm.rendered_frames;s.event_frames=s.clock_frames;update_session(s.session);}
                }else if(name=="A_Stop"){
                    if(!s.started)result=1;else {
                        s.started=false;pcm.playing=false;
                        update_session(s.session);
                        if(const auto trace=std::getenv("X64ARM_TRACE_AUDIO");trace && std::string_view(trace)=="1")std::cerr<<"wasapi_output channels="<<s.format.channels<<" rate="<<s.format.rate<<" source_bits="<<s.format.bits<<" frames="<<s.clock_frames<<" peak_left="<<pcm.rendered_left_peak<<" peak_right="<<pcm.rendered_right_peak<<'\n';
                    }
                }
                else if(name=="A_Reset"){if(s.started)result=not_stopped;else if(s.locked)result=operation_pending;else {pcm.pcm.clear();pcm.cursor=0;s.clock_frames=s.event_frames=0;}}
                else if(name=="A_GetService"){
                    out(2);const auto iid=guid(arg(1));Kind service_kind=Factory;
                    for(Kind k:{Render,Clock,Volume,SessionControl})if(iid==interfaces[k])service_kind=k;
                    if(service_kind==Factory)result=no_interface;
                    else if(service_kind==SessionControl){
                        auto control=std::make_shared<Object>();control->canonical=SessionControl;control->session=s.session;
                        const auto p=create(control,SessionControl);++s.session->controls;put(arg(2),p,bits);
                    }else put(arg(2),make(service_kind,object->stream),bits);
                }else if(name=="R_GetBuffer"){
                    out(2);if(s.locked)result=out_of_order;else if(d(1)>s.capacity-pcm.pcm.size()/pcm.frame_bytes())result=too_large;
                    else if(d(1)){s.locked=true;s.requested=d(1);put(arg(2),s.data,bits);}
                }else if(name=="R_ReleaseBuffer"){
                    if(d(2)&~2U)result=invalid;else if(!s.locked){if(d(1))result=out_of_order;}else if(d(1)>s.requested)result=invalid_size;
                    else {
                        const auto frames=d(1);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(frames)*pcm.frame_bytes(),pcm.bits==8?128:0);
                        if(frames && !(d(2)&2U))memory.read(GuestAddress{s.data},bytes);
                        pcm.pcm.insert(pcm.pcm.end(),bytes.begin(),bytes.end());s.locked=false;s.requested=0;
                    }
                }else if(name=="C_GetFrequency")put(arg(1),48000,64);
                else if(name=="C_GetPosition"){check(arg(1),8);if(arg(2))check(arg(2),8);put(arg(1),s.clock_frames,64);if(arg(2))put(arg(2),runtime_.clock_ticks,64);}
                else if(name=="C_GetCharacteristics")put(arg(1),0);
                else if(name=="V_GetChannelCount")put(arg(1),s.format.channels);
                else if(name=="V_GetChannelVolume"){check(arg(2),4);if(d(1)>=s.format.channels)result=invalid;else put(arg(2),std::bit_cast<std::uint32_t>(s.volumes[d(1)]));}
                else if(name=="V_SetChannelVolume"){
                    std::uint32_t word=d(2);if(!x86_){word=0;for(unsigned b=0;b<4;++b)word|=static_cast<std::uint32_t>(state.xmm[2][b])<<(8*b);}const auto f=std::bit_cast<float>(word);
                    if(d(1)>=s.format.channels || !std::isfinite(f) || f<0 || f>1)result=invalid;else s.volumes[d(1)]=f;
                }else if(name=="V_GetAllVolumes" || name=="V_SetAllVolumes"){
                    if(!arg(2))throw PointerError{};if(d(1)!=s.format.channels)result=invalid;
                    else if(name=="V_GetAllVolumes"){check(arg(2),d(1)*4);for(unsigned i=0;i<d(1);++i)put(arg(2)+i*4,std::bit_cast<std::uint32_t>(s.volumes[i]));}
                    else {auto levels=s.volumes;for(unsigned i=0;i<d(1);++i){levels[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+i*4},32)));if(!std::isfinite(levels[i]) || levels[i]<0 || levels[i]>1)result=invalid;}if(!result)s.volumes=levels;}
                }else result=invalid;
                pcm.left=s.volumes[0];pcm.right=s.volumes[s.format.channels==1?0:1];
            }else result=invalid;
        }
    }catch(const PointerError&){result=pointer_error;}catch(const MemoryFault&){result=pointer_error;}catch(const std::bad_alloc&){result=0x8007000eU;}
    if(!result && !callbacks.empty()){
        try{start_callbacks(state,memory,std::move(callbacks),event_data?Continuation::ReturnZeroFreeHeap:Continuation::Return,event_data);}
        catch(...){if(event_data)(void)free_heap(memory,event_data,state);throw;}
        return std::nullopt;
    }
    if(event_data)(void)free_heap(memory,event_data,state);
    arg.return_to(state,result);return std::nullopt;
}
}
