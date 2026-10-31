#include "x64arm/windows_services.hpp"
#include "automation_gamepad.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/windows_pointer.hpp"
#include "dinput_interfaces.hpp"
#include <cstdlib>
#include "monotonic_clock.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <functional>
#include <set>

namespace x64arm {
namespace {
std::string read_string(const GuestMemory& memory,GuestAddress p){std::string text;for(unsigned i=0;i<32768;++i){const auto c=memory.load(GuestAddress{p.value+i},8);if(!c)return text;text.push_back(static_cast<char>(c));}throw std::invalid_argument("unterminated string");}
using namespace dinput;
constexpr std::uint32_t invalid=0x80070057U,unsupported=0x80004001U,no_interface=0x80004002U,
    not_initialized=0x80070015U,not_acquired=0x8007000cU,input_lost=0x8007001eU,
    acquired=0x800700aaU,not_found=0x80070002U,not_buffered=0x80040207U;
struct Method {std::string_view name;std::uint8_t args;std::uint16_t scalars{};};
constexpr Method roots[]{
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateDevice",4},{"EnumDevices",5,0x12},{"GetDeviceStatus",2},{"RunControlPanel",3,4},{"Initialize",3,4},{"FindDevice",4},{"CreateDeviceEx",5}};
constexpr Method root8[]{
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateDevice",4},{"EnumDevices",5,0x12},{"GetDeviceStatus",2},{"RunControlPanel",3,4},{"Initialize",3,4},{"FindDevice",4},{"EnumDevicesBySemantics",6,0x20},{"ConfigureDevices",5,8}};
constexpr Method device_methods[]{
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCapabilities",2},{"EnumObjects",4,8},{"GetProperty",3},{"SetProperty",3},{"Acquire",1},{"Unacquire",1},{"GetDeviceState",3,2},{"GetDeviceData",5,0x12},{"SetDataFormat",2},{"SetEventNotification",2},{"SetCooperativeLevel",3,4},{"GetObjectInfo",4,0xc},{"GetDeviceInfo",2},{"RunControlPanel",3,4},{"DeviceInitialize",4,4},
    {"CreateEffect",5},{"EnumEffects",4,8},{"GetEffectInfo",3},{"GetForceFeedbackState",2},{"SendForceFeedbackCommand",2,2},{"EnumCreatedEffectObjects",4,8},{"Escape",2},{"Poll",1},{"SendDeviceData",5,0x12},{"EnumEffectsInFile",5,0x10},{"WriteEffectToFile",5,0x12},{"BuildActionMap",4,8},{"SetActionMap",4,8},{"GetImageInfo",2}};
constexpr Method effects[]{
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"EffectInitialize",4,4},{"GetEffectGuid",2},{"GetParameters",3,4},{"SetParameters",3,4},{"Start",3,6},{"Stop",1},{"GetEffectStatus",2},{"Download",1},{"Unload",1},{"EffectEscape",2}};
constexpr Method factories[]{{"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateInstance",4},{"LockServer",2,2}};
constexpr std::array root_iids{IID_IDirectInputA,IID_IDirectInputW,IID_IDirectInput2A,IID_IDirectInput2W,IID_IDirectInput7A,IID_IDirectInput7W,IID_IDirectInput8A,IID_IDirectInput8W};
constexpr std::array device_iids{IID_IDirectInputDeviceA,IID_IDirectInputDeviceW,IID_IDirectInputDevice2A,IID_IDirectInputDevice2W,IID_IDirectInputDevice7A,IID_IDirectInputDevice7W,IID_IDirectInputDevice8A,IID_IDirectInputDevice8W};
constexpr std::array effect_guids{GUID_ConstantForce,GUID_RampForce,GUID_Square,GUID_Sine,GUID_Triangle,GUID_SawtoothUp,GUID_SawtoothDown,GUID_Spring,GUID_Damper,GUID_Inertia,GUID_Friction,GUID_CustomForce};
constexpr std::array axis_guids{GUID_XAxis,GUID_YAxis,GUID_ZAxis,GUID_RxAxis,GUID_RyAxis,GUID_RzAxis,GUID_Slider,GUID_Slider};
constexpr std::array<const char*,12> effect_names{"Constant Force","Ramp Force","Square","Sine","Triangle","Sawtooth Up","Sawtooth Down","Spring","Damper","Inertia","Friction","Custom Force"};
unsigned effect_type(unsigned index){return index<2?index+1:index<7?3:index<11?4:5;}
Guid object_guid(const input::Device& device,const input::Object& o){return o.kind==input::ObjectKind::Axis?axis_guids.at(o.axis):o.kind==input::ObjectKind::Pov?GUID_POV:device.kind==input::Kind::Keyboard?GUID_Key:GUID_Button;}
std::uint32_t di_object_type(const input::Object& o){return (o.kind==input::ObjectKind::Axis?(o.relative?1U:2U):o.kind==input::ObjectKind::Pov?0x10U:4U)|(o.instance<<8)|(o.actuator?0x1000000U:0U);}
Guid canonical_device(Guid guid){if(guid==GUID_SysKeyboardEm || guid==GUID_SysKeyboardEm2)return GUID_SysKeyboard;if(guid==GUID_SysMouseEm || guid==GUID_SysMouseEm2)return GUID_SysMouse;return guid;}
std::uint32_t device_type(const input::Device& d,bool v8){return (d.kind==input::Kind::Keyboard?(windows_keyboard_capabilities.dinput_subtype<<8):d.kind==input::Kind::Mouse?0x200U:d.kind==input::Kind::Gamepad?(v8?0x200U:0x400U):0x200U)|(d.kind==input::Kind::Keyboard?(v8?0x13U:3U):d.kind==input::Kind::Mouse?(v8?0x12U:2U):d.kind==input::Kind::Gamepad?(v8?0x15U:4U):(v8?0x14U:4U))|(d.kind==input::Kind::Joystick || d.kind==input::Kind::Gamepad?0x10000U:0U);}
bool device_filter(const input::Device& d,unsigned type,bool v8){if(d.kind==input::Kind::Hid)return false;if(!type)return true;if(!v8)return type==(device_type(d,false)&0xffU);if(type<=4)return type==1 || (type==2 && d.kind==input::Kind::Mouse) || (type==3 && d.kind==input::Kind::Keyboard) || (type==4 && (d.kind==input::Kind::Joystick || d.kind==input::Kind::Gamepad));return type==(device_type(d,true)&0xffU);}
struct Property {
    std::int32_t minimum{},maximum{65535},deadzone{},saturation{10000};bool raw{};
    std::int32_t cal_min{},cal_center{},cal_max{};
    std::uint64_t appdata{UINT64_MAX};std::vector<std::pair<std::int32_t,std::uint32_t>> points;
};
struct Mapping {int object{-1};std::uint32_t offset{},type{},flags{};std::uint8_t width{4};};
struct Data {std::uint32_t offset{},value{},time{},sequence{};std::uint64_t appdata{UINT64_MAX};};
struct Object {
    enum class Kind {Root,Device,Factory,Effect}kind{Kind::Root};
    unsigned references{1};std::uint64_t canonical{};std::map<unsigned,std::uint64_t> interfaces;
    bool version8{},initialized{},is_acquired{},lost{},relative{};Guid clsid{},device_id{};
    std::shared_ptr<Object> parent;input::Device device;
    std::vector<Property> properties;std::vector<Mapping> format;std::vector<std::int64_t> deltas,history;
    std::vector<std::int32_t> velocities,accelerations;std::vector<std::uint32_t> sample_times;
    std::uint32_t data_size{},capacity{},cooperative{},effect_index{},effect_flags{},ff_gain{10000};
    std::uint64_t window{},event{};bool overflow{};std::deque<Data> buffer;
    std::shared_ptr<input::Effect> effect;input::EffectParameters parameters;std::uint32_t parameter_mask{};
    std::string username;
};
struct Interface {std::shared_ptr<Object> object;unsigned version{};bool wide{};};
std::uint32_t converted(const Object& object,unsigned index,std::int64_t value){
    const auto& o=object.device.objects.at(index);const auto& p=object.properties.at(index);
    if(o.kind==input::ObjectKind::Button)return value?0x80U:0U;
    if(o.kind==input::ObjectKind::Pov)return static_cast<std::uint32_t>(value);
    if(o.relative || p.raw)return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value,INT32_MIN,INT32_MAX));
    if(!p.points.empty()){
        if(value<=p.points.front().first)return p.points.front().second;if(value>=p.points.back().first)return p.points.back().second;
        for(std::size_t i=1;i<p.points.size();++i)if(value<=p.points[i].first){const auto [x0,y0]=p.points[i-1];const auto [x1,y1]=p.points[i];return static_cast<std::uint32_t>(static_cast<std::int64_t>(y0)+(value-x0)*(static_cast<std::int64_t>(y1)-y0)/(static_cast<std::int64_t>(x1)-x0));}
    }
    const auto center=static_cast<double>(p.cal_center);const auto span=static_cast<double>(value)<center?center-p.cal_min:p.cal_max-center;
    double n=span>0?(static_cast<double>(value)-center)/span:0;const double dead=p.deadzone/10000.0,saturation=p.saturation/10000.0;
    if(std::abs(n)<=dead)n=0;else n=std::copysign(std::clamp((std::abs(n)-dead)/std::max(saturation-dead,0.0001),0.0,1.0),n);
    const double output=(static_cast<double>(p.minimum)+p.maximum)*0.5+n*(static_cast<double>(p.maximum)-p.minimum)*0.5;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::clamp(std::round(output),static_cast<double>(INT32_MIN),static_cast<double>(INT32_MAX))));
}
std::shared_ptr<Object> make_device(const input::Device& device,const std::shared_ptr<Object>& parent){
    auto o=std::make_shared<Object>();o->kind=Object::Kind::Device;o->parent=parent;o->version8=parent->version8;o->device=device;o->device_id=device.instance;o->initialized=true;o->relative=device.kind==input::Kind::Mouse;
    for(const auto& object:device.objects){Property p;p.cal_min=object.minimum;p.cal_max=object.maximum;p.cal_center=static_cast<std::int32_t>((static_cast<std::int64_t>(p.cal_min)+p.cal_max)/2);if(object.relative){p.minimum=INT32_MIN;p.maximum=INT32_MAX;}o->properties.push_back(p);}
    o->deltas.resize(device.objects.size());o->history.resize(device.objects.size());o->velocities.resize(device.objects.size());o->accelerations.resize(device.objects.size());o->sample_times.resize(device.objects.size());return o;
}
}
struct DirectInputState {
    std::shared_ptr<input::Backend> backend;std::map<std::uint64_t,Interface> objects;std::map<unsigned,std::uint64_t> tables;
    std::map<std::uint64_t,std::vector<std::shared_ptr<Object>>> enumerations;
    std::uint32_t sequence{},locks{};std::uint64_t joystick_format{};
    std::vector<std::pair<input::Event,std::uint32_t>> injected_events;
    std::array<std::optional<input::Guid>,16> legacy_joystick_slots;
    struct GamepadSlot {std::optional<input::Guid> device;std::array<std::uint8_t,12> state{};std::uint32_t packet{};std::uint16_t keystroke_buttons{};bool sampled{};};
    std::array<GamepadSlot,4> gamepad_slots;
    bool xinput_enabled{true}; // XInputEnable(FALSE) 後 GetState 回中性狀態、SetState 不震動。
    // 最近一次處理的裝置清單換代，以及當時更新的 raw input 狀態。
    std::uint64_t device_generation{};const void* raw_input_refreshed{};
    // 最近一次背景輪詢的宿主單調時間（100 ns）。
    std::uint64_t background_polled{};
};
void WindowsServices::set_input_backend(std::shared_ptr<input::Backend> backend){if(dinput_ && !dinput_->objects.empty())throw std::logic_error("DirectInput 物件建立後不能替換輸入後端");if(!dinput_)dinput_=std::make_shared<DirectInputState>();dinput_->backend=std::move(backend);dinput_->gamepad_slots={};dinput_->device_generation=0;dinput_->raw_input_refreshed=nullptr;}
std::span<const WindowsServiceDescriptor> dinput_service_descriptors(){
    static const auto table=[](){
        std::vector<WindowsServiceDescriptor> result{{"dinput.dll","DirectInputCreateA",4,0,0,2},{"dinput.dll","DirectInputCreateW",4,0,0,2},{"dinput.dll","DirectInputCreateEx",5,0,0,2},{"dinput.dll","GetdfDIJoystick",0},{"dinput8.dll","DirectInput8Create",5,0,0,2}};
        for(const auto& item:std::array<WindowsServiceDescriptor,5>{{{"winmm.dll","joyGetNumDevs",0},{"winmm.dll","joyGetDevCapsA",3},{"winmm.dll","joyGetDevCapsW",3},{"winmm.dll","joyGetPos",2},{"winmm.dll","joyGetPosEx",2}}})result.push_back(item);
        for(auto family:{"xinput1_3.dll","xinput1_4.dll"}){
            result.push_back({family,"XInputGetState",2,0,0,1});
            result.push_back({family,"XInputSetState",2,0,0,1});
            result.push_back({family,"XInputGetCapabilities",3,0,0,3});
            result.push_back({family,"XInputEnable",1,0,0,1});
            result.push_back({family,"XInputGetBatteryInformation",3,0,0,3});
            result.push_back({family,"XInputGetKeystroke",3,0,0,3});
            result.push_back({family,"#100",2,0,0,1}); // XInputGetStateEx：含 Guide 鍵。
        }
        result.push_back({"xinput1_3.dll","XInputGetDSoundAudioDeviceGuids",3,0,0,1});
        // XInput 9.1.0（Vista 起內建）只匯出四個入口，語義與 1_3 相同。
        for(const auto& item:std::array<WindowsServiceDescriptor,4>{{{"xinput9_1_0.dll","XInputGetState",2,0,0,1},{"xinput9_1_0.dll","XInputSetState",2,0,0,1},
            {"xinput9_1_0.dll","XInputGetCapabilities",3,0,0,3},{"xinput9_1_0.dll","XInputGetDSoundAudioDeviceGuids",3,0,0,1}}})result.push_back(item);
        result.push_back({"xinput1_4.dll","XInputGetAudioDeviceIds",5,0,0,1});
        result.push_back({"xinput1_4.dll","#108",4,0,0,7}); // XInputGetCapabilitiesEx。
        for(auto family:{"dinput.dll","dinput8.dll"}){result.push_back({family,"DllGetClassObject",3});result.push_back({family,"DllCanUnloadNow",0});result.push_back({family,"DllRegisterServer",0});result.push_back({family,"DllUnregisterServer",0});}
        std::set<std::string_view> names;for(auto methods:{std::span<const Method>(roots),std::span<const Method>(root8),std::span<const Method>(device_methods),std::span<const Method>(effects),std::span<const Method>(factories)})for(const auto& method:methods)if(names.insert(method.name).second)result.push_back({"internal-dinput",method.name,method.args,0,0,method.scalars});return result;
    }();return table;
}
void WindowsServices::dinput_mouse_wheel(std::int32_t delta,bool horizontal){if(dinput_ && dinput_->backend){if(horizontal)dinput_->backend->horizontal_wheel(delta);else dinput_->backend->mouse_wheel(delta);}}
void WindowsServices::dinput_cursor_warped(){if(dinput_ && dinput_->backend)dinput_->backend->cursor_warped();}
std::shared_ptr<input::Backend> WindowsServices::input_backend()const{return dinput_?dinput_->backend:nullptr;}
void WindowsServices::prepare_input_backend(){
    if(!runtime_.network.external_io)return;
    if(!dinput_)dinput_=std::make_shared<DirectInputState>();
    if(!dinput_->backend)dinput_->backend=detail::make_input_backend();
}
void WindowsServices::inject_dinput(bool keyboard,unsigned object,std::int64_t value,std::uint32_t time){
    if(keyboard && object<256)gui_.injected_di_keys[object]=value?1:2;
    if(!keyboard && object>=3 && object<11)gui_.injected_di_buttons[object-3]=value?1:2;
    if(dinput_ && dinput_->backend){
        const bool has_acquired=std::any_of(dinput_->objects.begin(),dinput_->objects.end(),[](const auto& entry){return entry.second.object->is_acquired;});
        if(has_acquired){if(dinput_->injected_events.size()>=65536)throw std::bad_alloc{};
            dinput_->injected_events.push_back({{keyboard?input::keyboard().instance:input::mouse().instance,object,value},time});}
    }
}
void WindowsServices::finish_dinput_enumeration(std::uint64_t scratch,GuestMemory& memory,const GuestState& state){
    if(!scratch)return;if(dinput_ && dinput_->enumerations.contains(scratch)){for(const auto& o:dinput_->enumerations.at(scratch))if(!--o->references){if(o->is_acquired && (o->cooperative&1))dinput_->backend->cooperative(o->device_id,false);o->is_acquired=false;for(const auto& [key,p]:o->interfaces){(void)key;dinput_->objects.erase(p);(void)free_heap(memory,p,state);}o->interfaces.clear();}dinput_->enumerations.erase(scratch);}(void)free_heap(memory,scratch,state);
}
bool WindowsServices::dinput_waiting()const{if(!dinput_)return false;for(const auto& [pointer,interface]:dinput_->objects)if(pointer==interface.object->canonical && interface.object->is_acquired && interface.object->event)return true;return false;}
void WindowsServices::poll_dinput(bool background){
    if(!dinput_ || !dinput_->backend)return;
    // 裝置盤點可能建立 backend；只有背景排程可略過無訂閱者的輪詢。
    // API 主動查詢仍立即更新，已開啟 HID／輸入介面及視窗保持原有輪詢。
    if(background && dinput_->objects.empty() && dinput_->injected_events.empty() &&
       gui_.windows.empty() && gui_.hotkeys.empty() &&
       (!raw_input_ || raw_input_->registrations.empty()) &&
       (!pointer_input_ || pointer_input_->registrations.empty()) &&
       std::none_of(files_.begin(),files_.end(),[](const auto& entry){return bool(entry.second.hid);}))return;
    auto& bridge=*dinput_;
    // 背景排程每個時間片都會進來，後端快照約每 4 ms 才換代；有 DirectInput 物件時每次都會整份複製裝置清單。
    // 背景輪詢至多每 2 ms 一次（X64ARM_DINPUT_POLL_US，0 不限）；事件由後端累積，下一次輪詢一併取出，不會遺失。
    // API 主動查詢（background=false）照常立即更新。
    if(background){
        static const std::uint64_t interval=[] {
            const auto* value=std::getenv("X64ARM_DINPUT_POLL_US");if(!value)return UINT64_C(20000);
            const auto parsed=std::strtoull(value,nullptr,10);return std::min<std::uint64_t>(parsed,1000000)*10;
        }();
        if(interval){
            const auto now=detail::monotonic_ticks();
            if(bridge.background_polled && now-bridge.background_polled<interval)return;
            bridge.background_polled=now;
        }
    }
    const auto native_events=bridge.backend->poll();
    // backend 保證同一非零換代的清單完全相同。沒有 DirectInput 物件時，
    // 前景與背景都可省略已對同一 raw input 狀態完成的清單複製；事件與報告仍每次收取。
    // 換代編號須在取得清單前讀取：之後才發布的新清單會留給下一次輪詢。
    static const bool legacy_foreground_refresh=[] {
        const auto* value=std::getenv("X64ARM_DINPUT_LEGACY_FOREGROUND_REFRESH");
        return value && std::string_view(value)=="1";
    }();
    const auto generation=bridge.backend->device_generation();
    std::vector<input::Device> devices;
    if((legacy_foreground_refresh && !background) || !generation || generation!=bridge.device_generation || !bridge.objects.empty() || bridge.raw_input_refreshed!=raw_input_.get()){
        devices=bridge.backend->devices();refresh_raw_input(devices);
        bridge.device_generation=generation;bridge.raw_input_refreshed=raw_input_.get();
    }
    poll_pointer_input();
    const auto now=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);
    std::vector<std::pair<input::Event,std::uint32_t>> events;events.reserve(native_events.size()+bridge.injected_events.size());
    static const auto keyboard_id=input::keyboard().instance,mouse_id=input::mouse().instance;
    for(const auto& event:native_events){
        if(event.device==keyboard_id && event.object<256){
            gui_.injected_di_keys[event.object]=0;
            {
                const auto scan=event.object==0xc5?0xe11dU:(event.object&127U)|((event.object&0x80U)?0xe000U:0);
                const auto vk=windows_map_virtual_key(scan,3);if(vk){GuiMessage key{0,vk,1|(std::uint64_t{scan&255U}<<16)|((scan&0xff00U)==0xe000U?0x1000000U:0),event.value?0x100U:0x101U};
                    key.modifiers=((gui_.input_keyboard[16]&0x80)?1U:0)|((gui_.input_keyboard[17]&0x80)?2U:0)|((gui_.input_keyboard[18]&0x80)?4U:0);
                    if(event.repeat)key.lparam|=0x40000000U;if(!event.value)key.lparam|=0xc0000000U;observe_gui_key(key,false);(void)match_gui_hotkey(key);}
            }
        }
        if(event.device==mouse_id && event.object>=3 && event.object<11){
            gui_.injected_di_buttons[event.object-3]=0;constexpr std::array<unsigned,8> buttons{1,2,4,5,6,0,0,0};const auto vk=buttons[event.object-3];
            if(vk){auto& value=gui_.async_keyboard[vk];if(event.value && !(value&0x80U))value|=1;value=static_cast<std::uint8_t>((value&1U)|(event.value?0x80U:0));gui_.input_keyboard[vk]=event.value?0x80:0;gui_.injected_keys[vk]=0;}
        }
        raw_input_event(event);events.push_back({event,now});
    }
    for(const auto& report:bridge.backend->reports())raw_input_report(report);
    for(const auto& [event,time]:bridge.injected_events){(void)time;
        if(event.device==keyboard_id && event.object<256)gui_.injected_di_keys[event.object]=event.value?1:2;
        if(event.device==mouse_id && event.object>=3 && event.object<11)gui_.injected_di_buttons[event.object-3]=event.value?1:2;
    }
    events.insert(events.end(),bridge.injected_events.begin(),bridge.injected_events.end());bridge.injected_events.clear();
    for(auto& device:devices){
        if(device.instance==keyboard_id)for(unsigned i=0;i<device.values.size() && i<256;++i)if(gui_.injected_di_keys[i])device.values[i]=gui_.injected_di_keys[i]==1?1:0;
        if(device.instance==mouse_id)for(unsigned i=3;i<device.values.size() && i<11;++i)if(gui_.injected_di_buttons[i-3])device.values[i]=gui_.injected_di_buttons[i-3]==1?1:0;
    }
    for(const auto& [pointer,interface]:bridge.objects){auto& o=*interface.object;if(pointer!=o.canonical || o.kind!=Object::Kind::Device)continue;
        const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.instance==o.device_id;});o.device.attached=found!=devices.end() && found->attached;
        const auto window=gui_.windows.find(o.window);
        if(o.is_acquired && (!o.device.attached || ((o.cooperative&4) && (window==gui_.windows.end() || (gui_.active!=o.window && gui_.focus!=o.window))))){if(o.cooperative&1)bridge.backend->cooperative(o.device_id,false);o.is_acquired=false;o.lost=true;o.buffer.clear();o.overflow=false;}
        if(found!=devices.end() && found->values.size()==o.device.values.size())o.device.values=found->values;
    }
    for(const auto& [event,time]:events){const auto sequence=++bridge.sequence;
        for(const auto& [pointer,interface]:bridge.objects){auto& o=*interface.object;if(pointer!=o.canonical || o.kind!=Object::Kind::Device || !o.is_acquired || event.device!=o.device_id || event.object>=o.device.objects.size())continue;
            const auto& item=o.device.objects[event.object];auto value=converted(o,event.object,event.value);
            if(item.kind==input::ObjectKind::Axis){
                if(item.relative)o.deltas[event.object]+=event.value;
                else {
                    // 一次事件的變化量用於相對格式，速度／加速度以 GetTickCount 時基計算。
                    const auto previous=static_cast<std::int32_t>(o.sample_times[event.object]);(void)previous;
                    const auto old=o.velocities[event.object];const auto dt=now-o.sample_times[event.object];
                    // snapshot 已更新；事件依序更新獨立的歷史值。
                    const auto last=o.history[event.object];const auto difference=static_cast<std::int64_t>(static_cast<std::int32_t>(value))-last;
                    o.history[event.object]=static_cast<std::int32_t>(value);o.deltas[event.object]+=difference;if(dt){o.velocities[event.object]=static_cast<std::int32_t>(std::clamp<std::int64_t>(difference*1000/dt,INT32_MIN,INT32_MAX));o.accelerations[event.object]=static_cast<std::int32_t>(std::clamp<std::int64_t>((static_cast<std::int64_t>(o.velocities[event.object])-old)*1000/dt,INT32_MIN,INT32_MAX));}o.sample_times[event.object]=now;
                    if(o.relative)value=static_cast<std::uint32_t>(difference);
                }
            }
            bool mapped=false;for(const auto& map:o.format)if(map.object==static_cast<int>(event.object)){
                mapped=true;std::uint32_t data=value;const auto aspect=map.flags&0xf00U;if(aspect==0x200)data=static_cast<std::uint32_t>(o.velocities[event.object]);else if(aspect==0x300)data=static_cast<std::uint32_t>(o.accelerations[event.object]);else if(aspect==0x400)data=0;
                if(o.capacity){if(o.buffer.size()>=o.capacity){o.buffer.pop_front();o.overflow=true;}o.buffer.push_back({map.offset,data,time,sequence,o.properties[event.object].appdata});}
            }
            if(mapped && o.event){const auto found=runtime_.wait_objects.find(o.event);if(found!=runtime_.wait_objects.end() && found->second.kind==WaitObject::Kind::Event)found->second.set_signal(true);}
        }
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_dinput(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("DirectInput 不能在 compare 模式使用");
    if(!dinput_)dinput_=std::make_shared<DirectInputState>();auto& bridge=*dinput_;if(!bridge.backend)bridge.backend=detail::make_input_backend();
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    if(service.family=="xinput1_3.dll" || service.family=="xinput1_4.dll" || service.family=="xinput9_1_0.dll"){
#include "windows_xinput.inc"
    }
    if(service.family=="winmm.dll"){
#include "windows_joystick.inc"
    }
    const auto read=[&](std::uint64_t p,unsigned offset=0,std::uint8_t width=32){return memory.load(GuestAddress{p+offset},width);};
    const auto write=[&](std::uint64_t p,std::uint64_t value,std::uint8_t width=32){if(!p)throw std::invalid_argument("null output");memory.store(GuestAddress{p},width,value);};
    const auto check=[&](std::uint64_t p,std::uint64_t size,Access access=Access::Write){if(!p)throw std::invalid_argument("null pointer");memory.check(GuestAddress{p},static_cast<std::size_t>(size),access);};
    const auto guid=[&](std::uint64_t p){Guid g{};memory.read(GuestAddress{p},g);return g;};
    const auto string=[&](std::uint64_t p,bool wide){return p?(wide?utf16_to_utf8(read_utf16(memory,GuestAddress{p})):read_string(memory,GuestAddress{p})):std::string{};};
    const auto put_string=[&](std::uint64_t p,std::string_view text,bool wide){const auto value=utf8_to_utf16(text);if(wide){for(unsigned i=0;i<260;++i)write(p+i*2,i<std::min<std::size_t>(value.size(),259)?value[i]:0,16);}else {const auto bytes=std::string(text);for(unsigned i=0;i<260;++i)write(p+i,i<std::min<std::size_t>(bytes.size(),259)?static_cast<std::uint8_t>(bytes[i]):0,8);}};
    const auto create=[&](const std::shared_ptr<Object>& o,unsigned version,bool wide){
        const auto key=o->kind==Object::Kind::Factory?40U:o->kind==Object::Kind::Effect?41U:(o->kind==Object::Kind::Device?16U:0U)+version*2+(wide?1U:0U);
        if(o->interfaces.contains(key))return o->interfaces.at(key);
        if(bridge.objects.size()>=1024)throw std::bad_alloc{};
        if(!bridge.tables.contains(key)){const auto methods=o->kind==Object::Kind::Factory?std::span<const Method>(factories):o->kind==Object::Kind::Effect?std::span<const Method>(effects):o->kind==Object::Kind::Device?std::span<const Method>(device_methods):(o->version8?std::span<const Method>(root8):std::span<const Method>(roots));
            const auto count=o->kind==Object::Kind::Device?(version==0?18U:version==1?27U:version==2?29U:32U):o->kind==Object::Kind::Root?(version==0?8U:version==1?9U:version==2?10U:11U):static_cast<unsigned>(methods.size());const auto table=runtime_allocate(memory,{},count*unit);for(unsigned i=0;i<count;++i)write(table+i*unit,resolve("internal-dinput",methods[i].name).value,bits);bridge.tables.emplace(key,table);}
        const auto p=allocate_heap(memory,unit);if(!p)throw std::bad_alloc{};write(p,bridge.tables.at(key),bits);bridge.objects.emplace(p,Interface{o,version,wide});o->interfaces.emplace(key,p);if(!o->canonical)o->canonical=p;return p;
    };
    const auto release=[&](const std::shared_ptr<Object>& o){if(--o->references)return o->references;for(const auto& [key,p]:o->interfaces){(void)key;bridge.objects.erase(p);(void)free_heap(memory,p,state);}o->interfaces.clear();if(o->is_acquired && (o->cooperative&1))bridge.backend->cooperative(o->device_id,false);o->is_acquired=false;return 0U;};
    const auto find_device=[&](Guid id)->std::optional<input::Device>{id=canonical_device(id);auto all=bridge.backend->devices();for(auto& d:all)if(d.instance==id && d.attached && d.kind!=input::Kind::Hid)return d;return {};};
    const auto iid_index=[](const auto& iids,const Guid& id){const auto found=std::find(iids.begin(),iids.end(),id);return found==iids.end()?UINT32_MAX:static_cast<unsigned>(found-iids.begin());};
    const auto instantiate=[&](Guid cls,const Guid& iid,bool factory_object){
        const bool v8=cls==CLSID_DirectInput8 || cls==CLSID_DirectInputDevice8;auto o=std::make_shared<Object>();o->clsid=cls;o->version8=v8;
        if(factory_object){if(iid!=factory && iid!=unknown)throw std::domain_error("IID");o->kind=Object::Kind::Factory;return create(o,0,false);}
        const bool device_object=cls==CLSID_DirectInputDevice || cls==CLSID_DirectInputDevice8;const auto index=iid==unknown?(v8?6U:0U):iid_index(device_object?device_iids:root_iids,iid);if(index==UINT32_MAX || (v8?index<6:index>=6))throw std::domain_error("IID");o->kind=device_object?Object::Kind::Device:Object::Kind::Root;return create(o,index/2,(index&1)!=0);
    };
    const auto device_info=[&](const input::Device& d,std::uint64_t p,bool wide,bool v8,std::uint32_t size){
        const unsigned basic=wide?1080U:560U,full=basic+20;if(size!=basic && size!=full)throw std::invalid_argument("device info size");check(p,size);memory.write(GuestAddress{p},std::vector<std::uint8_t>(size));write(p,size);memory.write(GuestAddress{p+4},d.instance);memory.write(GuestAddress{p+20},d.product);write(p+36,device_type(d,v8));put_string(p+40,d.name,wide);put_string(p+40+(wide?520:260),d.name,wide);if(size==full){if(d.ff_effects)memory.write(GuestAddress{p+basic},CLSID_DirectInputDevice8);write(p+basic+16,1,16);write(p+basic+18,d.kind==input::Kind::Keyboard?6:d.kind==input::Kind::Mouse?2:d.kind==input::Kind::Gamepad?5:4,16);}
    };
    const auto object_index=[&](const Object& o,std::uint32_t id,std::uint32_t how)->int{
        if(how==1){for(const auto& map:o.format)if(map.offset==id)return map.object;return -1;}
        for(unsigned i=0;i<o.device.objects.size();++i){const auto& item=o.device.objects[i];if((how==2 && (di_object_type(item)&0xfffff)==(id&0xfffff)) || (how==3 && ((static_cast<std::uint32_t>(item.usage_page)<<16)|item.usage)==id))return static_cast<int>(i);}return -1;
    };
    const auto object_info=[&](const Object& o,unsigned index,std::uint64_t p,bool wide,std::uint32_t size){
        const unsigned basic=wide?552U:292U,full=basic+24;if(size!=basic && size!=full)throw std::invalid_argument("object info size");check(p,size);memory.write(GuestAddress{p},std::vector<std::uint8_t>(size));const auto& item=o.device.objects.at(index);write(p,size);memory.write(GuestAddress{p+4},object_guid(o.device,item));auto offset=input::default_offset(o.device,item);for(const auto& map:o.format)if(map.object==static_cast<int>(index)){offset=map.offset;break;}write(p+20,offset);write(p+24,di_object_type(item));write(p+28,(item.actuator?1U:0U)|(item.kind==input::ObjectKind::Axis?0x100U:0U));put_string(p+32,item.name,wide);if(size==full){write(p+basic,item.actuator?10000:0);write(p+basic+4,item.actuator?1:0);write(p+basic+12,item.usage_page,16);write(p+basic+14,item.usage,16);}
    };
    const auto enumerate=[&](std::vector<GuestCallback> callbacks,std::uint64_t scratch){start_callbacks(state,memory,std::move(callbacks),Continuation::DirectInputEnumeration,scratch);};
    const auto state_error=[&](const Object& o){return !o.initialized?not_initialized:!o.device.attached?0x80040209U:!o.is_acquired?(o.lost?input_lost:not_acquired):0U;};
    const auto effect_parameters=[&](const Object& device,unsigned type,std::uint64_t p,std::uint32_t flags,input::EffectParameters current,bool objects=true){
        const auto size=read(p);if(size!=(x86_?56U:80U) && size!=(x86_?52U:72U))throw std::invalid_argument("effect size");check(p,size,Access::Read);
        const auto mode=static_cast<std::uint32_t>(read(p,4)),count=static_cast<unsigned>(read(p,28));
        if(((flags&0x28) && ((mode&3)!=1 && (mode&3)!=2)) || ((flags&0x40) && !std::has_single_bit(mode&0x70U)) || ((flags&0x60) && count>32))throw std::invalid_argument("effect coordinates");
        if(flags&0x40)current.flags=(mode&0x70U)|(objects?2U:mode&3U);
        if(flags&1)current.duration=static_cast<std::uint32_t>(read(p,8));if(flags&2)current.sample_period=static_cast<std::uint32_t>(read(p,12));if(flags&4){current.gain=static_cast<std::uint32_t>(read(p,16));if(current.gain>10000)throw std::invalid_argument("effect gain");}
        if(flags&8){const auto trigger=static_cast<std::uint32_t>(read(p,20));if(trigger==UINT32_MAX || !objects)current.trigger=trigger;else {const auto index=object_index(device,trigger,(mode&1)?2U:1U);if(index<0 || device.device.objects[static_cast<unsigned>(index)].kind!=input::ObjectKind::Button)throw std::invalid_argument("trigger button");current.trigger=input::default_offset(device.device,device.device.objects[static_cast<unsigned>(index)]);}}
        if(flags&0x10)current.repeat=static_cast<std::uint32_t>(read(p,24));
        if(flags&0x20){if(!count)throw std::invalid_argument("effect axes");const auto array=read(p,32,bits);check(array,count*4,Access::Read);current.axes.clear();std::set<unsigned> seen;for(unsigned i=0;i<count;++i){const auto value=static_cast<std::uint32_t>(read(array,i*4));if(!objects){current.axes.push_back(value);continue;}const auto index=object_index(device,value,(mode&1)?2U:1U);if(index<0 || !device.device.objects[static_cast<unsigned>(index)].actuator || !seen.insert(static_cast<unsigned>(index)).second)throw std::invalid_argument("actuator");current.axes.push_back(input::default_offset(device.device,device.device.objects[static_cast<unsigned>(index)]));}}
        if(flags&0x40){if(count!=current.axes.size())throw std::invalid_argument("effect directions");const auto array=read(p,32+unit,bits);if(count)check(array,count*4,Access::Read);current.direction.clear();for(unsigned i=0;i<count;++i)current.direction.push_back(static_cast<std::int32_t>(read(array,i*4)));}
        if(flags&0x80){const auto env=read(p,32+unit*2,bits);current.envelope.reset();if(env){if(read(env)!=20 || read(env,4)>10000 || read(env,12)>10000)throw std::invalid_argument("effect envelope");current.envelope=std::array{static_cast<std::uint32_t>(read(env,4)),static_cast<std::uint32_t>(read(env,8)),static_cast<std::uint32_t>(read(env,12)),static_cast<std::uint32_t>(read(env,16))};}}
        if(flags&0x100){const auto length=read(p,32+unit*3),array=read(p,x86_?48:64,bits);const auto expected=type==0?4U:type==1?8U:type<7?16U:type<11?24U:0U;
            if(type==11){if(length!=(x86_?16U:24U))throw std::invalid_argument("custom force");check(array,length,Access::Read);current.channels=static_cast<std::uint32_t>(read(array));current.custom_period=static_cast<std::uint32_t>(read(array,4));const auto samples=read(array,8),data=read(array,x86_?12:16,bits);if(!current.channels || current.channels>32 || samples>65536 || samples%current.channels)throw std::invalid_argument("custom samples");if(samples)check(data,samples*4,Access::Read);current.samples.clear();for(unsigned i=0;i<samples;++i)current.samples.push_back(static_cast<std::int32_t>(read(data,i*4)));current.specific.clear();}
            else {if(length>1048576 || (type<7 && length!=expected) || (type>=7 && type<11 && length!=expected && length!=expected*current.axes.size()))throw std::invalid_argument("specific effect size");if(length)check(array,length,Access::Read);current.specific.resize(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{array},current.specific);current.channels=0;}
        }
        if((flags&0x200) && size==(x86_?56U:80U))current.delay=static_cast<std::uint32_t>(read(p,x86_?52:72));
        return current;
    };
    const auto effect_output=[&](const Object& device,const input::EffectParameters& parameters,std::uint64_t p,std::uint32_t flags,bool objects=true){
        const auto size=read(p);if(size!=(x86_?56U:80U) && size!=(x86_?52U:72U))throw std::invalid_argument("effect size");check(p,size);
        const auto requested=static_cast<std::uint32_t>(read(p,4));if((requested&3)!=1 && (requested&3)!=2)throw std::invalid_argument("effect output coordinates");
        const auto translate=[&](std::uint32_t offset){if(offset==UINT32_MAX || !objects)return offset;for(unsigned i=0;i<device.device.objects.size();++i)if(input::default_offset(device.device,device.device.objects[i])==offset){if(requested&1)return di_object_type(device.device.objects[i]);for(const auto& map:device.format)if(map.object==static_cast<int>(i))return map.offset;}throw std::invalid_argument("effect output object");};
        const auto count=read(p,28),axis=read(p,32,bits),direction=read(p,32+unit,bits),env=read(p,32+unit*2,bits);const auto length=read(p,32+unit*3),specific=read(p,x86_?48:64,bits);
        if(flags&(0x20|0x40)){write(p+28,parameters.axes.size());if(count<parameters.axes.size())return 0x80040202U;if((flags&0x20) && !parameters.axes.empty()){check(axis,parameters.axes.size()*4);for(unsigned i=0;i<parameters.axes.size();++i)write(axis+i*4,translate(parameters.axes[i]));}if((flags&0x40) && !parameters.direction.empty()){check(direction,parameters.direction.size()*4);for(unsigned i=0;i<parameters.direction.size();++i)write(direction+i*4,static_cast<std::uint32_t>(parameters.direction[i]));}}
        write(p+4,(parameters.flags&0x70U)|(requested&3U));
        if(flags&1)write(p+8,parameters.duration);if(flags&2)write(p+12,parameters.sample_period);if(flags&4)write(p+16,parameters.gain);if(flags&8)write(p+20,translate(parameters.trigger));if(flags&0x10)write(p+24,parameters.repeat);
        if(flags&0x80){if(parameters.envelope){check(env,20);write(env,20);for(unsigned i=0;i<4;++i)write(env+4+i*4,(*parameters.envelope)[i]);}else write(p+32+unit*2,0,bits);}
        if(flags&0x100){const auto needed=parameters.channels?(x86_?16U:24U):static_cast<unsigned>(parameters.specific.size());write(p+32+unit*3,needed);if(length<needed)return 0x80040202U;if(needed)check(specific,needed);if(parameters.channels){const auto capacity=read(specific,8),data=read(specific,x86_?12:16,bits);write(specific,parameters.channels);write(specific+4,parameters.custom_period);write(specific+8,parameters.samples.size());if(capacity<parameters.samples.size())return 0x80040202U;if(!parameters.samples.empty())check(data,parameters.samples.size()*4);for(unsigned i=0;i<parameters.samples.size();++i)write(data+i*4,static_cast<std::uint32_t>(parameters.samples[i]));}else if(needed)memory.write(GuestAddress{specific},parameters.specific);}
        if((flags&0x200) && size==(x86_?56U:80U))write(p+(x86_?52:72),parameters.delay);return 0U;
    };
    [[maybe_unused]] const auto escape=[&](const Object& o,std::uint64_t p){
        if(read(p)!=(x86_?24U:40U))throw std::invalid_argument("escape size");const auto in=read(p,8,bits),in_size=read(p,8+unit),out=read(p,x86_?16:24,bits),out_size=read(p,x86_?20:32);if(in_size>1048576 || out_size>1048576)throw std::invalid_argument("escape capacity");if(in_size)check(in,in_size,Access::Read);if(out_size)check(out,out_size);std::vector<std::uint8_t> input(static_cast<std::size_t>(in_size)),output(static_cast<std::size_t>(out_size));if(in_size)memory.read(GuestAddress{in},input);const auto code=o.kind==Object::Kind::Effect?o.effect->escape(static_cast<std::uint32_t>(read(p,4)),input,output):bridge.backend->escape(o.device_id,static_cast<std::uint32_t>(read(p,4)),input,output);write(p+(x86_?20:32),output.size());if(!(code&0x80000000U) && !output.empty())memory.write(GuestAddress{out},std::span(output).first(std::min<std::size_t>(output.size(),static_cast<std::size_t>(out_size))));return code;
    };
    const auto action_map=[&](Object& o,std::uint64_t p,std::uint64_t user,std::uint32_t flags,bool set,bool map_wide){
        const auto size=read(p),stride=read(p,4),data_size=read(p,8),count=read(p,12),array=read(p,16,bits);
        const unsigned expected=x86_?(map_wide?588U:328U):(map_wide?600U:336U),expected_stride=x86_?40U:48U,instance_offset=x86_?16U:24U;
        if(size!=expected || stride!=expected_stride || !count || count>4096 || data_size!=count*4 || !array || flags>(set?3U:4U) || (!set && flags==3))throw std::invalid_argument("action format");
        check(p,size,set?Access::Read:Access::Write);check(array,count*stride,set?Access::Read:Access::Write);
        const unsigned genre_offset=16+unit+16;const auto buffer=read(p,genre_offset+4);const auto minimum=static_cast<std::int32_t>(read(p,genre_offset+8)),maximum=static_cast<std::int32_t>(read(p,genre_offset+12));
        if(buffer>65536 || minimum>=maximum)throw std::invalid_argument("action range");const auto user_name=user?string(user,map_wide):std::string{"User"};
        const auto id=guid(p+16+unit);const auto hex=[](const Guid& g){std::u16string s;constexpr char16_t digits[]=u"0123456789abcdef";for(auto b:g){s.push_back(digits[b>>4]);s.push_back(digits[b&15]);}return s;};
        const auto key=std::u16string(1,1)+u"\\software\\x64arm\\directinput\\"+hex(id)+u"\\"+hex(o.device_id)+u"\\"+utf8_to_utf16(user_name);
        RegistryTransaction transaction(runtime_.registry,registry_file_);if(transaction.error)return 0x8004020bU;
        std::vector<std::uint8_t> saved;if(runtime_.registry.keys.contains(key) && runtime_.registry.keys.at(key).contains(u"actions"))saved=runtime_.registry.keys.at(key).at(u"actions").bytes;
        std::vector<Mapping> maps;auto properties=o.properties;std::vector<std::uint8_t> bytes;unsigned mapped=0;
        const auto semantic_index=[&](std::uint32_t semantic)->int{
            const auto family=semantic>>24,kind=(semantic>>8)&7U;
            if(o.device.kind==input::Kind::Keyboard){if(family!=0x81 || kind!=4)return -1;const unsigned scan=semantic&0xffU;for(unsigned i=0;i<o.device.objects.size();++i)if(o.device.objects[i].instance==scan)return static_cast<int>(i);return -1;}
            if(o.device.kind==input::Kind::Mouse){if(family!=0x82)return -1;const auto offset=semantic&0xffU;for(unsigned i=0;i<o.device.objects.size();++i)if(input::default_offset(o.device,o.device.objects[i])==offset)return static_cast<int>(i);return -1;}
            if(family==0x81 || family==0x82)return -1;const auto object_kind=kind==4?input::ObjectKind::Button:kind==6?input::ObjectKind::Pov:input::ObjectKind::Axis;if(kind!=2 && kind!=3 && kind!=4 && kind!=6)return -1;
            const unsigned desired=kind==4 || kind==6?(semantic&0xffU)?(semantic&0xffU)-1:0:(semantic>>15)&0xfU;
            for(unsigned i=0;i<o.device.objects.size();++i){const auto& item=o.device.objects[i];if(item.kind==object_kind && item.instance==desired)return static_cast<int>(i);}return -1;
        };
        for(unsigned i=0;i<count;++i){const auto at=array+i*stride;const auto semantic=static_cast<std::uint32_t>(read(at,unit)),options=static_cast<std::uint32_t>(read(at,unit+4));const auto instance=guid(at+instance_offset);const auto requested=static_cast<std::uint32_t>(read(at,instance_offset+16));int index=-1;unsigned how=0;
            if(options&~0x1fU)throw std::invalid_argument("action flags");
            if(set){if(instance==o.device_id && !(options&4))index=object_index(o,requested,2);}
            else if(!(options&4)){
                if((flags==1 && instance!=Guid{}) || ((options&2) && flags!=2 && flags!=4) || (options&0x10)){if(instance==o.device_id){index=object_index(o,requested,2);how=2;}else continue;}
                else if(flags==0 && saved.size()==count*8){std::uint32_t id_value=0,saved_semantic=0;for(unsigned b=0;b<4;++b){id_value|=static_cast<std::uint32_t>(saved[i*8+b])<<(b*8);saved_semantic|=static_cast<std::uint32_t>(saved[i*8+b+4])<<(b*8);}if(saved_semantic==semantic)index=object_index(o,id_value,2);if(index>=0)how=1;}
                if(index<0){index=semantic_index(semantic);how=0x20;}
            }
            if(index>=0 && (options&1) && !o.device.objects[static_cast<unsigned>(index)].actuator)index=-1;
            if(index>=0){const auto object_id=di_object_type(o.device.objects[static_cast<unsigned>(index)]);++mapped;if(!set){memory.write(GuestAddress{at+instance_offset},o.device_id);write(at+instance_offset+16,object_id);write(at+instance_offset+20,how);}else {maps.push_back({index,i*4,object_id,0,4});auto& prop=properties[static_cast<unsigned>(index)];prop.appdata=read(at,0,bits);if(o.device.objects[static_cast<unsigned>(index)].kind==input::ObjectKind::Axis && !(options&8)){prop.minimum=minimum;prop.maximum=maximum;}}
                for(unsigned b=0;b<4;++b)bytes.push_back(static_cast<std::uint8_t>(object_id>>(b*8)));for(unsigned b=0;b<4;++b)bytes.push_back(static_cast<std::uint8_t>(semantic>>(b*8)));
            }else {if(!set && instance==o.device_id){memory.write(GuestAddress{at+instance_offset},Guid{});write(at+instance_offset+16,0);write(at+instance_offset+20,0);}bytes.insert(bytes.end(),8,0);}
        }
        if(set){
            if(!(flags&1)){const auto before=runtime_.registry;for(std::size_t position=1;;){const auto next=key.find(u'\\',position);runtime_.registry.keys.try_emplace(key.substr(0,next));if(next==key.npos)break;position=next+1;}auto& values=runtime_.registry.keys[key];if(!values.contains(u"actions"))++runtime_.registry.values;else runtime_.registry.bytes-=values.at(u"actions").bytes.size();runtime_.registry.bytes+=bytes.size();values[u"actions"]={3,std::move(bytes),u"actions"};runtime_.registry.modified_times[key]=runtime_.system_filetime();if(runtime_.registry.keys.size()>4096 || runtime_.registry.values>4096 || runtime_.registry.bytes>16*1024*1024 || transaction.commit()){runtime_.registry=before;return 0x8004020bU;}poll_registry_notifications(false,key,4);}
            o.format=std::move(maps);o.data_size=mapped?static_cast<std::uint32_t>(data_size):0;o.properties=std::move(properties);o.capacity=static_cast<std::uint32_t>(buffer);o.relative=o.device.kind==input::Kind::Mouse;o.buffer.clear();o.overflow=false;o.username=flags&1?std::string{}:user_name;
            return 0U;
        }
        return mapped?0U:1U;
    };
    std::uint32_t result=0;
    try {
        poll_dinput();
        if(name=="DllRegisterServer" || name=="DllUnregisterServer"){
            RegistryTransaction transaction(runtime_.registry,registry_file_);if(transaction.error)result=0x80070000U|(transaction.error&0xffffU);else {
                const auto before=runtime_.registry;const bool v8=service.family=="dinput8.dll";const std::array classes{v8?CLSID_DirectInput8:CLSID_DirectInput,v8?CLSID_DirectInputDevice8:CLSID_DirectInputDevice};
                const auto label=[](const Guid& id){constexpr char16_t digits[]=u"0123456789abcdef";constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};std::u16string out=u"{";for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)out.push_back(u'-');const auto b=id[order[i]];out.push_back(digits[b>>4]);out.push_back(digits[b&15]);}out.push_back(u'}');return out;};
                for(unsigned i=0;i<classes.size();++i){const auto key=registry_view_path(std::u16string(1,0)+u"\\clsid\\"+label(classes[i]),registry_view(0,x86_));if(name=="DllUnregisterServer"){
                        for(auto it=runtime_.registry.keys.begin();it!=runtime_.registry.keys.end();)if(it->first==key || it->first.starts_with(key+u"\\")){for(const auto& [n,value]:it->second){(void)n;runtime_.registry.bytes-=value.bytes.size();--runtime_.registry.values;}runtime_.registry.modified_times.erase(it->first);runtime_.registry.display_names.erase(it->first);it=runtime_.registry.keys.erase(it);}else ++it;
                    }else {for(std::size_t position=1;;){const auto next=key.find(u'\\',position);runtime_.registry.keys.try_emplace(key.substr(0,next));if(next==key.npos)break;position=next+1;}const auto server=key+u"\\inprocserver32";
                        const auto save=[&](const std::u16string& path,std::u16string value_name,std::u16string_view value){auto bytes=utf16_bytes(value);auto& values=runtime_.registry.keys[path];if(values.contains(value_name))runtime_.registry.bytes-=values.at(value_name).bytes.size();else ++runtime_.registry.values;runtime_.registry.bytes+=bytes.size();values[std::move(value_name)]={1,std::move(bytes),{}};runtime_.registry.modified_times[path]=runtime_.system_filetime();};
                        save(key,u"",i?(v8?u"DirectInputDevice8":u"DirectInputDevice"):(v8?u"DirectInput8":u"DirectInput"));save(server,u"",v8?u"dinput8.dll":u"dinput.dll");save(server,u"threadingmodel",u"Both");
                    }
                }
                if(runtime_.registry.keys.size()>4096 || runtime_.registry.values>4096 || runtime_.registry.bytes>16*1024*1024)result=0x8007000eU;else if(const auto error=transaction.commit())result=0x80070000U|(error&0xffffU);
                if(result)runtime_.registry=before;else for(const auto& id:classes)poll_registry_notifications(true,std::u16string(1,0)+u"\\clsid\\"+label(id),5);
            }
        }else if(name=="DllCanUnloadNow")result=bridge.objects.empty() && !bridge.locks?0:1;
        else if(name=="DllGetClassObject" || name=="CoGetClassObject" || name=="CoCreateInstance"){
            const bool factory_object=name!="CoCreateInstance";const auto out=arg(name=="DllGetClassObject"?2:4);check(out,unit);write(out,0,bits);const auto cls=guid(arg(0));const auto iid=guid(arg(name=="DllGetClassObject"?1:3));
            if(!dinput::is_class(memory,arg(0)))result=0x80040154U;else if(name=="CoCreateInstance" && arg(1))result=0x80040110U;else if(name!="DllGetClassObject" && !(arg(name=="CoCreateInstance"?2:1)&1))result=0x80040154U;else if(name=="CoCreateInstance" && (!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16))result=0x800401f0U;else write(out,instantiate(cls,iid,factory_object),bits);
        }else if(name.starts_with("DirectInput")){
            const bool ex=name=="DirectInputCreateEx" || name=="DirectInput8Create",v8=name=="DirectInput8Create";const auto out=arg(ex?3:2);check(out,unit);write(out,0,bits);const auto version=arg(1);const auto iid=ex?guid(arg(2)):(name.ends_with("W")?IID_IDirectInputW:IID_IDirectInputA);
            if(arg(ex?4:3))result=0x80040110U;else if(!arg(0) || !version)result=invalid;else if(version>(v8?0x800U:0x700U))result=0x80070481U;else if(v8 && version<0x800)result=0x8007047eU;else if(!v8 && version<0x300)result=0x8007047eU;else {const auto p=instantiate(v8?CLSID_DirectInput8:CLSID_DirectInput,iid,false);bridge.objects.at(p).object->initialized=true;write(out,p,bits);}
        }else if(name=="GetdfDIJoystick"){
            if(!bridge.joystick_format){const unsigned obj_size=x86_?16U:24U,format_size=x86_?24U:32U;const auto base=runtime_allocate(memory,{},format_size+44*obj_size+16*10);const auto objects=base+format_size,guids=objects+44*obj_size;std::array<Guid,10> ids{GUID_XAxis,GUID_YAxis,GUID_ZAxis,GUID_RxAxis,GUID_RyAxis,GUID_RzAxis,GUID_Slider,GUID_Slider,GUID_POV,GUID_Button};for(unsigned i=0;i<ids.size();++i)memory.write(GuestAddress{guids+i*16},ids[i]);write(base,format_size);write(base+4,obj_size);write(base+8,1);write(base+12,80);write(base+16,44);write(base+(x86_?20:24),objects,bits);for(unsigned i=0;i<44;++i){const auto p=objects+i*obj_size;write(p,guids+(i<8?i:i<12?8U:9U)*16,bits);write(p+unit,i<8?i*4:i<12?32+(i-8)*4:48+i-12);write(p+unit+4,(i<8?2U:i<12?0x10U:4U)|((i<8?i:i<12?i-8:i-12)<<8)|0x80000000U);write(p+unit+8,i<8?0x100U:0U);}bridge.joystick_format=base;}arg.return_to(state,bridge.joystick_format);return std::nullopt;
        }else {
            const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())throw std::invalid_argument("DirectInput object");const auto interface=found->second;const auto object=interface.object;auto& o=*object;const bool wide=interface.wide,v8=o.version8;
            if(name=="QueryInterface"){
                check(arg(2),unit);write(arg(2),0,bits);const auto iid=guid(arg(1));std::uint64_t p=0;if(iid==unknown)p=o.canonical;else if(o.kind==Object::Kind::Factory){if(iid==factory)p=o.canonical;}else if(o.kind==Object::Kind::Effect){if(iid==IID_IDirectInputEffect)p=o.canonical;}else {const auto index=iid_index(o.kind==Object::Kind::Root?root_iids:device_iids,iid);if(index!=UINT32_MAX && (v8?index>=6:index<6))p=create(object,index/2,(index&1)!=0);}if(p){++o.references;write(arg(2),p,bits);}else result=no_interface;
            }else if(name=="AddRef")result=++o.references;
            else if(name=="Release")result=release(object);
            else if(o.kind==Object::Kind::Factory){
                if(name=="LockServer"){if(arg(1))++bridge.locks;else if(bridge.locks)--bridge.locks;}
                else if(name=="CreateInstance"){check(arg(3),unit);write(arg(3),0,bits);if(arg(1))result=0x80040110U;else write(arg(3),instantiate(o.clsid,guid(arg(2)),false),bits);}
                else result=invalid;
            }else if(name=="Initialize" || name=="DeviceInitialize"){
                if(o.initialized)result=0x800704dfU;else if(!arg(1) || !arg(2))result=invalid;else if((v8 && arg(2)<0x800) || (!v8 && arg(2)<0x300))result=0x8007047eU;else if(arg(2)>(v8?0x800U:0x700U))result=0x80070481U;else if(o.kind==Object::Kind::Device){auto d=find_device(guid(arg(3)));if(!d)result=not_found;else {auto parent=std::make_shared<Object>();parent->version8=v8;auto replacement=make_device(*d,parent);o.device=replacement->device;o.device_id=replacement->device_id;o.properties=replacement->properties;o.deltas=replacement->deltas;o.history=replacement->history;o.velocities=replacement->velocities;o.accelerations=replacement->accelerations;o.sample_times=replacement->sample_times;o.relative=replacement->relative;o.initialized=true;}}else o.initialized=true;
            }else if(name=="RunControlPanel"){
                if(!o.initialized)result=not_initialized;else if(arg(2))result=invalid;else if(arg(1) && !gui_.windows.contains(arg(1)))result=0x80070006U;else result=bridge.backend->control_panel(o.kind==Object::Kind::Device?o.device_id:GUID_SysKeyboard);
            }else if(o.kind==Object::Kind::Root){
                if(!o.initialized)result=not_initialized;
                else if(name=="CreateDevice" || name=="CreateDeviceEx"){
                    const bool ex=name=="CreateDeviceEx";const auto out=arg(ex?3:2);check(out,unit);write(out,0,bits);if(arg(ex?4:3))result=0x80040110U;else {auto d=find_device(guid(arg(1)));if(!d)result=0x80040154U;else {const auto iid=ex?guid(arg(2)):(v8?(wide?IID_IDirectInputDevice8W:IID_IDirectInputDevice8A):(wide?IID_IDirectInputDeviceW:IID_IDirectInputDeviceA));const auto index=iid_index(device_iids,iid);if(index==UINT32_MAX || (v8?index<6:index>=6))result=no_interface;else write(out,create(make_device(*d,object),index/2,(index&1)!=0),bits);}}
                }else if(name=="GetDeviceStatus")result=find_device(guid(arg(1)))?0:1;
                else if(name=="ConfigureDevices"){
                    const auto p=arg(2),flags=arg(3);const unsigned size=x86_?64U:88U;check(p,size,Access::Read);
                    const auto users=read(p,4),formats=read(p,x86_?12:16),array=read(p,x86_?16:24,bits),window=read(p,x86_?20:32,bits),surface=read(p,x86_?60:80,bits);
                    if(read(p)!=size || !users || users>32 || !formats || formats>32 || flags>1 || !array || (window && !gui_.windows.contains(window)) || (arg(1) && !surface))result=invalid;
                    else if(arg(1) || !native_gui_)result=unsupported;
                    else {std::vector<input::Action> rows;std::vector<std::uint64_t> addresses;const auto all=bridge.backend->devices();const unsigned af_size=x86_?(wide?588U:328U):(wide?600U:336U),action_size=x86_?40U:48U,instance_offset=x86_?16U:24U;
                        for(unsigned f=0;f<formats;++f){const auto af=array+f*af_size;if(read(af)!=af_size || read(af,4)!=action_size || read(af,12)>4096)throw std::invalid_argument("configure actions");const auto count=read(af,12),actions=read(af,16,bits);check(actions,count*action_size,Access::Read);
                            for(unsigned i=0;i<count;++i){const auto at=actions+i*action_size,action_name=read(at,unit+8,bits);const auto semantic=static_cast<std::uint32_t>(read(at,unit)),options=static_cast<std::uint32_t>(read(at,unit+4));input::Action row;row.label=action_name>65535?string(action_name,wide):"動作 "+std::to_string(i+1);row.controls.push_back({{},0,"未指定"});const auto assigned=guid(at+instance_offset);const auto object_id=read(at,instance_offset+16);
                                for(const auto& d:all)if(d.attached && d.kind!=input::Kind::Hid)for(const auto& item:d.objects){const auto kind=(semantic>>8)&7U;const bool compatible=kind==4?item.kind==input::ObjectKind::Button:kind==6?item.kind==input::ObjectKind::Pov:item.kind==input::ObjectKind::Axis;if(!compatible || ((options&1) && !item.actuator) || ((options&0x10) && (assigned!=d.instance || object_id!=di_object_type(item))))continue;row.controls.push_back({d.instance,di_object_type(item),d.name+" / "+item.name});if(assigned==d.instance && object_id==di_object_type(item))row.selected=static_cast<unsigned>(row.controls.size()-1);}
                                rows.push_back(std::move(row));addresses.push_back(at);
                            }
                        }
                        result=bridge.backend->configure(rows,flags==1);if(!result && flags==1){for(unsigned i=0;i<rows.size();++i){const auto& row=rows[i];if(row.selected>=row.controls.size())throw std::invalid_argument("configured object");const auto& control=row.controls[row.selected];const auto at=addresses[i];memory.write(GuestAddress{at+instance_offset},control.device);write(at+instance_offset+16,control.object);write(at+instance_offset+20,control.device==Guid{}?0:1);}
                            auto user=read(p,8,bits);if(users>1 && !user)throw std::invalid_argument("configure users");for(unsigned u=0;u<users && !result;++u){for(unsigned f=0;f<formats && !result;++f)for(const auto& d:all)if(d.attached){auto device=make_device(d,object);const auto saved=action_map(*device,array+f*af_size,user,0,true,wide);if(saved&0x80000000U){result=saved;break;}}if(user)user+=(wide?read_utf16(memory,GuestAddress{user}).size()+1:read_string(memory,GuestAddress{user}).size()+1)*(wide?2U:1U);}
                        }
                    }
                }
                else if(name=="EnumDevicesBySemantics"){
                    if(!arg(3) || (arg(5)&~0x7110U))result=invalid;
                    else {const auto af_size=read(arg(2)),af_stride=read(arg(2),4),af_count=read(arg(2),12),af_actions=read(arg(2),16,bits);if(af_count>4096 || af_stride!=(x86_?40U:48U) || af_size!=(x86_?(wide?588U:328U):(wide?600U:336U)))throw std::invalid_argument("semantic format");check(af_actions,af_stride*af_count,Access::Read);
                        auto all=bridge.backend->devices();std::erase_if(all,[&](const auto& d){return d.kind==input::Kind::Hid || !d.attached || ((arg(5)&0x100) && !d.ff_effects);});
                        if(!all.empty()){const unsigned info_size=wide?1100U:580U;const auto per_device=info_size+af_size+af_stride*af_count;const auto scratch=allocate_heap(memory,per_device*all.size());if(!scratch)throw std::bad_alloc{};
                            std::vector<GuestCallback> callbacks;std::vector<std::shared_ptr<Object>> temporary;
                            try{for(unsigned i=0;i<all.size();++i){const auto p=scratch+i*per_device,af=p+info_size,actions=af+af_size;std::vector<std::uint8_t> bytes(static_cast<std::size_t>(af_size));memory.read(GuestAddress{arg(2)},bytes);memory.write(GuestAddress{af},bytes);bytes.resize(static_cast<std::size_t>(af_stride*af_count));memory.read(GuestAddress{af_actions},bytes);memory.write(GuestAddress{actions},bytes);write(af+16,actions,bits);
                                    auto d=make_device(all[i],object);const auto mapped=action_map(*d,af,arg(1),0,false,wide);if(mapped && !(arg(5)&0x4000U))continue;
                                    bool available=true;if(arg(5)&0x1000)for(const auto& [ptr,item]:bridge.objects)if(ptr==item.object->canonical && item.object->device_id==d->device_id && item.object->is_acquired && (item.object->cooperative&1))available=false;if(!available)continue;
                                    device_info(all[i],p,wide,true,info_size);const auto pointer=create(d,3,wide);temporary.push_back(d);callbacks.push_back({arg(3),{p,pointer,mapped?0U:1U,0,arg(4)}});
                                }
                                if(callbacks.empty()){(void)free_heap(memory,scratch,state);}else {for(unsigned i=0;i<callbacks.size();++i)callbacks[i].arguments[3]=callbacks.size()-i-1;bridge.enumerations.emplace(scratch,std::move(temporary));enumerate(std::move(callbacks),scratch);return std::nullopt;}
                            }catch(...){for(const auto& d:temporary)release(d);(void)free_heap(memory,scratch,state);throw;}
                        }
                    }
                }
                else if(name=="FindDevice"){
                    check(arg(3),16);const auto product=guid(arg(1));const auto label=string(arg(2),wide);result=not_found;for(const auto& d:bridge.backend->devices())if(d.kind!=input::Kind::Hid && d.attached && (product==Guid{} || product==d.product || canonical_device(product)==d.instance) && (label.empty() || label==d.name || label==d.path)){memory.write(GuestAddress{arg(3)},d.instance);result=0;break;}
                }else if(name=="EnumDevices"){
                    if(!arg(2) || (arg(4)&~0x70101U) || arg(1)>0xff)result=invalid;else {
                        auto all=bridge.backend->devices();all.erase(std::remove_if(all.begin(),all.end(),[&](const auto& d){return !device_filter(d,static_cast<unsigned>(arg(1)),v8) || ((arg(4)&1) && !d.attached) || ((arg(4)&0x100) && !d.ff_effects);}),all.end());
                        if(arg(4)&0x10000){const auto originals=all;for(const auto& d:originals)if(d.kind==input::Kind::Keyboard || d.kind==input::Kind::Mouse)for(unsigned alias=0;alias<2;++alias){auto extra=d;extra.instance=d.kind==input::Kind::Keyboard?(alias?GUID_SysKeyboardEm2:GUID_SysKeyboardEm):(alias?GUID_SysMouseEm2:GUID_SysMouseEm);all.push_back(std::move(extra));}}
                        if(!all.empty()){const unsigned size=wide?1100U:580U;const auto scratch=allocate_heap(memory,all.size()*size);if(!scratch)throw std::bad_alloc{};std::vector<GuestCallback> callbacks;for(unsigned i=0;i<all.size();++i){const auto p=scratch+i*size;device_info(all[i],p,wide,v8,size);callbacks.push_back({arg(2),{p,arg(3)}});}enumerate(std::move(callbacks),scratch);return std::nullopt;}
                    }
                }else result=unsupported;
            }else if(o.kind==Object::Kind::Effect){
                const auto& device=*o.parent;
                const auto exclusive=[&](){const auto error=state_error(device);return error?error:!(device.cooperative&1)?0x80040205U:0U;};
                if(name=="GetEffectGuid"){check(arg(1),16);memory.write(GuestAddress{arg(1)},effect_guids[o.effect_index]);}
                else if(name=="EffectInitialize")result=o.initialized?0x800704dfU:invalid;
                else if(name=="SetParameters"){
                    const auto flags=static_cast<std::uint32_t>(arg(2));if((flags&~0xe00003ffU) || std::popcount(flags&0xe0000000U)>1)result=invalid;
                    else {auto parameters=effect_parameters(device,o.effect_index,arg(1),flags,o.parameters);if((o.parameter_mask&0x20) && (flags&0x20) && parameters.axes!=o.parameters.axes)result=invalid;else {result=o.effect->set(parameters,flags);if(!(result&0x80000000U) || result==0x80040206U){o.parameters=std::move(parameters);o.parameter_mask|=flags&0x3ffU;}}}
                }else if(name=="GetParameters"){const auto flags=static_cast<std::uint32_t>(arg(2));if(flags&~0x3ffU)result=invalid;else if(flags&~o.parameter_mask)result=0x80040206U;else result=effect_output(device,o.parameters,arg(1),flags);}
                else if(name=="Start"){if(!arg(1) || (arg(2)&~0x80000001U))result=invalid;else if((result=exclusive())==0){if((o.parameter_mask&0x161)!=0x161)result=0x80040206U;else result=o.effect->start(static_cast<std::uint32_t>(arg(1)),static_cast<std::uint32_t>(arg(2)));}}
                else if(name=="Stop")result=o.effect->stop();
                else if(name=="Download"){if((result=exclusive())==0)result=(o.parameter_mask&0x161)==0x161?o.effect->download():0x80040206U;}
                else if(name=="Unload")result=o.effect->unload();
                else if(name=="GetEffectStatus"){check(arg(1),4);std::uint32_t value=0;result=o.effect->status(value);if(!(result&0x80000000U))write(arg(1),value);}
                else if(name=="EffectEscape")result=escape(o,arg(1));
                else result=invalid;
            }else if(o.kind==Object::Kind::Device){
                if(!o.initialized)result=not_initialized;
                else if(name=="GetCapabilities"){
                    const auto p=arg(1),size=read(p);if(size!=24 && size!=44)result=invalid;else {check(p,size);memory.write(GuestAddress{p},std::vector<std::uint8_t>(static_cast<std::size_t>(size)));write(p,size);write(p+4,(o.device.attached?1U:0U)|(o.device.ff_effects?0xff00U:0U)|((o.device.kind==input::Kind::Keyboard || o.device.kind==input::Kind::Mouse)?4U:0U));write(p+8,device_type(o.device,v8));for(unsigned i=0;i<3;++i)write(p+12+i*4,static_cast<std::uint32_t>(std::count_if(o.device.objects.begin(),o.device.objects.end(),[&](const auto& item){return static_cast<unsigned>(item.kind)==i;})));}
                }else if(name=="GetDeviceInfo")device_info(o.device,arg(1),wide,v8,static_cast<std::uint32_t>(read(arg(1))));
                else if(name=="GetImageInfo"){
                    const auto p=arg(1);if(read(p)!=(x86_?36U:40U) || read(p,4)!=(wide?612U:352U))result=invalid;
                    else {check(p,x86_?36U:40U);write(p+8,0);for(unsigned i=0;i<3;++i)write(p+12+i*4,static_cast<std::uint32_t>(std::count_if(o.device.objects.begin(),o.device.objects.end(),[&](const auto& item){return item.kind==(i==0?input::ObjectKind::Button:i==1?input::ObjectKind::Axis:input::ObjectKind::Pov);})));write(p+28,0);result=not_found;}
                }
                else if(name=="GetObjectInfo"){const auto index=object_index(o,static_cast<std::uint32_t>(arg(2)),static_cast<std::uint32_t>(arg(3)));if(arg(3)<1 || arg(3)>3)result=invalid;else if(index<0)result=not_found;else object_info(o,static_cast<unsigned>(index),arg(1),wide,static_cast<std::uint32_t>(read(arg(1))));}
                else if(name=="EnumObjects"){
                    if(!arg(1) || (arg(3)&~0xf3ffffffU))result=invalid;else {std::vector<unsigned> indices;for(unsigned i=0;i<o.device.objects.size();++i){const auto type=di_object_type(o.device.objects[i]);if((!(arg(3)&0xff) || (arg(3)&type&0xff)) && (!(arg(3)&0x1000000) || (type&0x1000000)))indices.push_back(i);}if(!indices.empty()){const unsigned size=wide?576U:316U;const auto scratch=allocate_heap(memory,indices.size()*size);if(!scratch)throw std::bad_alloc{};std::vector<GuestCallback> callbacks;for(unsigned i=0;i<indices.size();++i){const auto p=scratch+i*size;object_info(o,indices[i],p,wide,size);callbacks.push_back({arg(1),{p,arg(2)}});}enumerate(std::move(callbacks),scratch);return std::nullopt;}}
                }else if(name=="SetDataFormat"){
                    if(o.is_acquired)result=acquired;else {
                        const auto p=arg(1);const auto size=read(p),stride=read(p,4),flags=read(p,8),data_size=read(p,12),count=read(p,16),array=read(p,x86_?20:24,bits);
                        if(size!=(x86_?24U:32U) || stride!=(x86_?16U:24U) || (flags&~3U) || flags==3 || !data_size || data_size>65536 || count>4096 || (count && !array))result=invalid;
                        else {std::vector<Mapping> maps;std::set<std::pair<unsigned,unsigned>> selected;std::vector<bool> bytes(static_cast<std::size_t>(data_size));
                            for(unsigned i=0;i<count;++i){const auto at=array+i*stride;const auto id_pointer=read(at,0,bits),offset=read(at,unit),type=read(at,unit+4),aspect=read(at,unit+8);const auto id=id_pointer?guid(id_pointer):Guid{};const auto width=(type&0xc)?1U:4U;
                                if(!(type&0x1f) || offset+width>data_size || (width==4 && offset%4) || (aspect&~0x10f00U))throw std::invalid_argument("data format object");for(unsigned b=0;b<width;++b){if(bytes[static_cast<std::size_t>(offset)+b])throw std::invalid_argument("overlapping format");bytes[static_cast<std::size_t>(offset)+b]=true;}
                                int index=-1;for(unsigned j=0;j<o.device.objects.size();++j){const auto& item=o.device.objects[j];const auto kind=di_object_type(item)&0xffU;if(!(type&kind&0xffU) && !((type&3) && (kind&3)))continue;if((type&0xffff00)!=0xffff00 && ((type>>8)&0xffff)!=item.instance)continue;if(id_pointer && id!=object_guid(o.device,item))continue;if((type&0x1000000) && !item.actuator)continue;if(selected.contains({j,static_cast<unsigned>(aspect&0xf00)}))continue;index=static_cast<int>(j);selected.emplace(j,static_cast<unsigned>(aspect&0xf00));break;}
                                if(index<0 && !(type&0x80000000U))throw std::invalid_argument("required object missing");maps.push_back({index,static_cast<std::uint32_t>(offset),static_cast<std::uint32_t>(type),static_cast<std::uint32_t>(aspect),static_cast<std::uint8_t>(width)});
                            }
                            o.format=std::move(maps);o.data_size=static_cast<std::uint32_t>(data_size);o.relative=flags==2 || (!flags && o.device.kind==input::Kind::Mouse);o.buffer.clear();o.overflow=false;
                        }
                    }
                }else if(name=="SetCooperativeLevel"){
                    const auto flags=arg(2);if(o.is_acquired)result=acquired;else if((flags&~0x1fU) || ((flags&3)!=1 && (flags&3)!=2) || ((flags&0xc)!=4 && (flags&0xc)!=8) || ((flags&0x10) && o.device.kind!=input::Kind::Keyboard))result=invalid;
                    else if((flags&9)==9 && (o.device.kind==input::Kind::Mouse || o.device.kind==input::Kind::Keyboard))result=unsupported;
                    else if((flags&4) && (!arg(1) || !gui_.windows.contains(arg(1)) || gui_.windows.at(arg(1)).parent))result=0x80070006U;
                    else if(arg(1) && !gui_.windows.contains(arg(1)))result=0x80070006U;else {o.cooperative=static_cast<std::uint32_t>(flags);o.window=arg(1);}
                }else if(name=="Acquire"){
                    if(o.is_acquired)result=1;else if(!o.data_size || !o.cooperative)result=invalid;else if(!bridge.backend->available(o.device_id))result=0x80040209U;else if((o.cooperative&4) && gui_.active!=o.window && gui_.focus!=o.window)result=0x80070005U;else {
                        bool conflict=false;for(const auto& [p,other]:bridge.objects)if(p==other.object->canonical && other.object!=object && other.object->is_acquired && other.object->device_id==o.device_id && ((other.object->cooperative|o.cooperative)&1))conflict=true;
                        if(conflict)result=0x80070005U;else if((result=bridge.backend->cooperative(o.device_id,(o.cooperative&1)!=0))==0){o.is_acquired=true;o.lost=false;o.buffer.clear();o.overflow=false;std::fill(o.deltas.begin(),o.deltas.end(),0);for(unsigned i=0;i<o.device.objects.size();++i)if(!o.device.objects[i].relative && o.device.objects[i].kind==input::ObjectKind::Axis)o.history[i]=static_cast<std::int32_t>(converted(o,i,o.device.values[i]));}
                    }
                }else if(name=="Unacquire"){result=o.is_acquired?0:1;if(o.is_acquired && (o.cooperative&1))result=bridge.backend->cooperative(o.device_id,false);o.is_acquired=false;o.lost=false;o.buffer.clear();o.overflow=false;}
                else if(name=="Poll")result=state_error(o);
                else if(name=="SetEventNotification"){
                    if(o.is_acquired)result=acquired;else if(arg(1) && (!runtime_.wait_objects.contains(arg(1)) || runtime_.wait_objects.at(arg(1)).kind!=WaitObject::Kind::Event))result=0x80070006U;else o.event=arg(1);
                }else if(name=="GetDeviceState"){
                    result=state_error(o);if(!result){if(arg(1)!=o.data_size)result=invalid;else {check(arg(2),o.data_size);std::vector<std::uint8_t> bytes(o.data_size);for(const auto& map:o.format){std::uint32_t value=(map.type&0x10)?UINT32_MAX:0;if(map.object>=0){const auto index=static_cast<unsigned>(map.object);const auto& item=o.device.objects[index];value=converted(o,index,o.device.values[index]);const auto aspect=map.flags&0xf00U;if(aspect==0x200)value=static_cast<std::uint32_t>(o.velocities[index]);else if(aspect==0x300)value=static_cast<std::uint32_t>(o.accelerations[index]);else if(aspect==0x400)value=0;else if(item.kind==input::ObjectKind::Axis && o.relative)value=static_cast<std::uint32_t>(std::clamp<std::int64_t>(o.deltas[index],INT32_MIN,INT32_MAX));}for(unsigned b=0;b<map.width;++b)bytes[map.offset+b]=static_cast<std::uint8_t>(value>>(b*8));}memory.write(GuestAddress{arg(2)},bytes);if(o.relative)std::fill(o.deltas.begin(),o.deltas.end(),0);}}
                }else if(name=="GetDeviceData"){
                    const auto stride=arg(1);check(arg(3),4);const auto wanted=read(arg(3));if((stride!=16 && stride!=(x86_?20U:24U)) || (arg(4)&~1U))result=invalid;else if((result=state_error(o))==0){if(!o.capacity)result=not_buffered;else {const auto count=std::min<std::size_t>(static_cast<std::size_t>(wanted),o.buffer.size());if(arg(2)){check(arg(2),count*stride);for(unsigned i=0;i<count;++i){const auto& data=o.buffer[i];const auto p=arg(2)+i*stride;write(p,data.offset);write(p+4,data.value);write(p+8,data.time);write(p+12,data.sequence);if(stride>16)write(p+16,data.appdata,bits);}}write(arg(3),count);result=o.overflow?1:0;if(!(arg(4)&1)){o.buffer.erase(o.buffer.begin(),o.buffer.begin()+static_cast<std::ptrdiff_t>(count));o.overflow=false;}}}
                }else if(name=="GetProperty" || name=="SetProperty"){
                    const bool set=name=="SetProperty";const auto property=arg(1),p=arg(2);const auto size=read(p),header=read(p,4),id=read(p,8),how=read(p,12);const unsigned expected=property==4 || property==18 || property==19?24U:property==11?28U:property==12?552U:property==13 || property==14 || property==16 || property==20 || property==25 || property==26?536U:property==21?84U:property==22?(x86_?20U:24U):20U;
                    if(property>26 || property==0 || property==17)result=unsupported;else if(size!=expected || header!=16 || how>3 || (!how && id))result=invalid;else {check(p,size,set?Access::Read:Access::Write);std::vector<unsigned> indices;const auto selected=how?object_index(o,static_cast<std::uint32_t>(id),static_cast<std::uint32_t>(how)):-1;if(how && selected<0)result=not_found;else {if(how)indices.push_back(static_cast<unsigned>(selected));else for(unsigned i=0;i<o.properties.size();++i)if(o.device.objects[i].kind==input::ObjectKind::Axis)indices.push_back(i);const auto value=set?static_cast<std::uint32_t>(read(p,16)):0U;
                        if(set && o.is_acquired && property!=7 && property!=22)result=acquired;
                        else if(property==1){if(how)result=invalid;else if(set){if(value>65536)result=invalid;else {o.capacity=value;o.buffer.clear();o.overflow=false;}}else write(p+16,o.capacity);}
                        else if(property==2){if(how)result=invalid;else if(set){if(value>1)result=invalid;else {o.relative=value!=0;std::fill(o.deltas.begin(),o.deltas.end(),0);}}else write(p+16,o.relative?1:0);}
                        else if(property==4 || property==5 || property==6 || property==10 || property==11 || property==18 || property==19 || property==21){
                            if(indices.empty() || (how && o.device.objects[indices[0]].kind!=input::ObjectKind::Axis))result=unsupported;
                            else if(set && (property==18 || property==19))result=0x80070005U;
                            else if(set && ((property==5 || property==6) && value>10000))result=invalid;
                            else if(set && property==10 && value>1)result=invalid;
                            else if(set && property==4 && static_cast<std::int32_t>(read(p,16))>=static_cast<std::int32_t>(read(p,20)))result=invalid;
                            else if(set && property==11 && (static_cast<std::int32_t>(read(p,16))>=static_cast<std::int32_t>(read(p,20)) || static_cast<std::int32_t>(read(p,20))>=static_cast<std::int32_t>(read(p,24))))result=invalid;
                            else if(set){std::vector<std::pair<std::int32_t,std::uint32_t>> points;if(property==21){if(value<2 || value>8)throw std::invalid_argument("calibration points");for(unsigned i=0;i<value;++i){const auto x=static_cast<std::int32_t>(read(p,20+i*8));const auto y=static_cast<std::uint32_t>(read(p,24+i*8));if(i && x<=points.back().first)throw std::invalid_argument("calibration order");points.emplace_back(x,y);}}for(auto index:indices){auto& prop=o.properties[index];if(property==4){prop.minimum=static_cast<std::int32_t>(read(p,16));prop.maximum=static_cast<std::int32_t>(read(p,20));}else if(property==5)prop.deadzone=static_cast<std::int32_t>(value);else if(property==6)prop.saturation=static_cast<std::int32_t>(value);else if(property==10)prop.raw=value!=0;else if(property==11){prop.cal_min=static_cast<std::int32_t>(read(p,16));prop.cal_center=static_cast<std::int32_t>(read(p,20));prop.cal_max=static_cast<std::int32_t>(read(p,24));}else prop.points=points;}}
                            else {const auto index=indices.front();const auto& prop=o.properties[index];const auto& item=o.device.objects[index];if(property==4){write(p+16,static_cast<std::uint32_t>(prop.minimum));write(p+20,static_cast<std::uint32_t>(prop.maximum));}else if(property==5)write(p+16,static_cast<std::uint32_t>(prop.deadzone));else if(property==6)write(p+16,static_cast<std::uint32_t>(prop.saturation));else if(property==10)write(p+16,prop.raw);else if(property==11){write(p+16,static_cast<std::uint32_t>(prop.cal_min));write(p+20,static_cast<std::uint32_t>(prop.cal_center));write(p+24,static_cast<std::uint32_t>(prop.cal_max));}else if(property==18 || property==19){write(p+16,static_cast<std::uint32_t>(property==18?item.physical_minimum:item.minimum));write(p+20,static_cast<std::uint32_t>(property==18?item.physical_maximum:item.maximum));}else {write(p+16,prop.points.size());for(unsigned i=0;i<prop.points.size();++i){write(p+20+i*8,static_cast<std::uint32_t>(prop.points[i].first));write(p+24+i*8,prop.points[i].second);}}}
                        }else if(property==7){if(how || (set && value>10000))result=invalid;else {auto data=set?value:o.ff_gain;if(o.device.ff_effects)result=bridge.backend->ff_property(o.device_id,7,data,set);if(!result){o.ff_gain=data;if(!set)write(p+16,data);}}}
                        else if(property==8 || property==9){if(how)result=invalid;else if(!o.device.ff_effects)result=unsupported;else if(property==8){if(set)result=0x80070005U;else if((result=state_error(o))==0){if(!(o.cooperative&1))result=0x80040205U;else {unsigned count=0;for(const auto& [ptr,e]:bridge.objects)if(ptr==e.object->canonical && e.object->kind==Object::Kind::Effect && e.object->parent==object)++count;write(p+16,o.device.ff_storage?std::min(100U,count*100/o.device.ff_storage):0);}}}else if(set && value>1)result=invalid;else {auto data=value;result=bridge.backend->ff_property(o.device_id,9,data,set);if(!set && !result)write(p+16,data);}}
                        else if(property==22){if(!how)result=invalid;else if(set)o.properties[static_cast<unsigned>(selected)].appdata=read(p,16,bits);else write(p+16,o.properties[static_cast<unsigned>(selected)].appdata,bits);}
                        else if(property==3 || property==15 || property==23 || property==24){if(set)result=0x80070005U;else if(property==3)write(p+16,1);else if(property==24)write(p+16,(o.device.product_id<<16)|(o.device.vendor&0xffff));else if(property==23){if(!how || o.device.kind!=input::Kind::Keyboard)result=unsupported;else write(p+16,o.device.objects[static_cast<unsigned>(selected)].instance);}else {if(o.device.kind!=input::Kind::Joystick && o.device.kind!=input::Kind::Gamepad)result=unsupported;else {unsigned index=0;for(const auto& d:bridge.backend->devices())if(d.kind==input::Kind::Joystick || d.kind==input::Kind::Gamepad){if(d.instance==o.device_id)break;++index;}write(p+16,index);}}}
                        else if(property==12){if(set)result=0x80070005U;else {memory.write(GuestAddress{p+16},o.device.product);put_string(p+32,o.device.path,true);}}
                        else if(property==13 || property==14 || property==16 || property==20 || property==25 || property==26){if(set){if(property==13)o.device.name=string(p+16,true);else if(property==25)o.username=string(p+16,true);else result=0x80070005U;}else if(property==20){if(!how)result=invalid;else put_string(p+16,o.device.objects[static_cast<unsigned>(selected)].name,true);}else {put_string(p+16,property==25?o.username:property==26?(o.device.kind==input::Kind::Keyboard?"Keyboard":o.device.kind==input::Kind::Mouse?"Mouse":o.device.kind==input::Kind::Gamepad?"Gamepad":"Joystick"):o.device.name,true);if(property==16 || (property==25 && o.username.empty()))result=1;}}
                        else result=unsupported;
                    }}
                }else if(name=="SendDeviceData"){
                    check(arg(3),4);const auto count=read(arg(3)),stride=arg(1);if((stride!=16 && stride!=(x86_?20U:24U)) || arg(4) || count>65536)result=invalid;else if((result=state_error(o))==0){check(arg(2),count*stride,Access::Read);unsigned sent=0;for(;sent<count;++sent){const auto p=arg(2)+sent*stride;const auto index=object_index(o,static_cast<std::uint32_t>(read(p)),1);if(index<0){result=not_found;break;}result=bridge.backend->send(o.device_id,static_cast<unsigned>(index),static_cast<std::uint32_t>(read(p,4)));if(result&0x80000000U)break;}write(arg(3),sent);}
                }else if(name=="WriteEffectToFile" || name=="EnumEffectsInFile"){
                    const bool saving=name=="WriteEffectToFile";const auto flags=arg(4);const auto path=string(arg(1),wide);
                    if(path.empty() || (flags&~(saving?1U:0x11U)) || (!saving && !arg(2)))result=invalid;
                    else if(saving){
                        const auto count=arg(2),array=arg(3);const unsigned stride=x86_?284U:296U,parameter_offset=x86_?20U:24U;
                        if(count>4096 || (count && !array))result=invalid;else {if(count)check(array,count*stride,Access::Read);std::vector<input::FileEffect> entries;
                            for(unsigned i=0;i<count;++i){const auto p=array+i*stride;if(read(p)!=stride)throw std::invalid_argument("file effect size");const auto type=guid(p+4);const auto index=iid_index(effect_guids,type);if(index==UINT32_MAX && !(flags&1))continue;
                                input::FileEffect entry;entry.type=type;const auto label=p+parameter_offset+unit;for(unsigned n=0;n<260;++n){const auto c=read(label,n,8);if(!c)break;if(n==259)throw std::invalid_argument("file effect name");entry.name.push_back(static_cast<char>(c));}entry.parameters=effect_parameters(o,index,read(p,parameter_offset,bits),0x3ff,{},false);entries.push_back(std::move(entry));}
                            const auto bytes=input::write_effect_file(entries);auto file=filesystem_.open_host(path,0x40000000U,2,0x80);if(file.error)result=0x80070000U|(file.error&0xffffU);else {const auto saved=file.file->write(bytes,0);result=saved.error?0x80070000U|(saved.error&0xffffU):saved.bytes!=bytes.size()?0x8007001dU:file.file->flush();if(result && !(result&0x80000000U))result=0x80070000U|(result&0xffffU);}
                        }
                    }else {
                        auto file=filesystem_.open_reader(path,runtime_.application_source,utf16_to_utf8(runtime_.application_directory));if(file.error)result=0x80070000U|(file.error&0xffffU);else if(file.metadata.size>16*1024*1024)result=invalid;else {std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.metadata.size));const auto loaded=file.file->read(bytes,0);if(loaded.error || loaded.bytes!=bytes.size())result=0x8007001eU;else {
                            auto entries=input::read_effect_file(bytes);std::erase_if(entries,[&](const auto& e){return !(flags&1) && iid_index(effect_guids,e.type)==UINT32_MAX;});
                            if(flags&0x10){std::vector<unsigned> actuators;for(unsigned i=0;i<o.device.objects.size();++i)if(o.device.objects[i].actuator)actuators.push_back(i);if(actuators.empty())result=unsupported;else for(auto& entry:entries){auto& p=entry.parameters;const auto original_axes=p.axes;const auto original_directions=p.direction;const auto original_flags=p.flags;std::vector<unsigned> retained,assigned;std::set<unsigned> used;
                                    for(unsigned i=0;i<original_axes.size() && used.size()<actuators.size();++i){auto selected_axis=std::find_if(actuators.begin(),actuators.end(),[&](unsigned index){const auto& item=o.device.objects[index];return !used.contains(index) && ((p.flags&1)?(di_object_type(item)&0xffffff)==(original_axes[i]&0xffffff):input::default_offset(o.device,item)==original_axes[i]);});if(selected_axis==actuators.end())selected_axis=std::find_if(actuators.begin(),actuators.end(),[&](unsigned index){return !used.contains(index);});if(selected_axis!=actuators.end()){retained.push_back(i);assigned.push_back(*selected_axis);used.insert(*selected_axis);}}
                                    if(retained.empty()){result=unsupported;break;}p.axes.clear();for(auto index:assigned)p.axes.push_back(di_object_type(o.device.objects[index]));p.flags=(p.flags&0x70U)|1U;
                                    if(retained.size()!=original_axes.size()){
                                        std::vector<std::int32_t> cartesian=original_directions;
                                        if((p.flags&0x70)==0x20 && cartesian.size()>=2){const auto angle=cartesian[0]*3.14159265358979323846/18000.0;cartesian[0]=static_cast<std::int32_t>(std::llround(std::sin(angle)*10000));cartesian[1]=static_cast<std::int32_t>(std::llround(-std::cos(angle)*10000));}
                                        else if((p.flags&0x70)==0x40 && cartesian.size()>=2){double product=10000;for(std::size_t i=cartesian.size()-1;i>1;--i){const auto angle=original_directions[i-1]*3.14159265358979323846/18000.0;cartesian[i]=static_cast<std::int32_t>(std::llround(product*std::sin(angle)));product*=std::cos(angle);}const auto angle=original_directions[0]*3.14159265358979323846/18000.0;cartesian[0]=static_cast<std::int32_t>(std::llround(product*std::cos(angle)));cartesian[1]=static_cast<std::int32_t>(std::llround(product*std::sin(angle)));}
                                        p.direction.clear();for(auto index:retained)p.direction.push_back(cartesian[index]);if(p.direction.size()==1)p.direction[0]=0;p.flags=0x11;
                                        const auto type=iid_index(effect_guids,entry.type);if(type>=7 && type<11 && p.specific.size()==original_axes.size()*24){const auto specific=p.specific;p.specific.clear();for(auto index:retained)p.specific.insert(p.specific.end(),specific.begin()+index*24,specific.begin()+(index+1)*24);}
                                        if(p.channels==original_axes.size()){const auto samples=p.samples;p.samples.clear();for(std::size_t frame=0;frame<samples.size();frame+=p.channels)for(auto index:retained)p.samples.push_back(samples[frame+index]);p.channels=static_cast<std::uint32_t>(retained.size());}
                                    }
                                    if(p.trigger!=UINT32_MAX){const auto button=object_index(o,p.trigger,(original_flags&1)?2U:1U);p.trigger=button>=0?di_object_type(o.device.objects[static_cast<unsigned>(button)]):UINT32_MAX;}
                                }}
                            if(!result && !entries.empty()){
                                const unsigned file_size=x86_?284U:296U,effect_size=x86_?56U:80U;std::size_t total=0;for(const auto& entry:entries){const auto& p=entry.parameters;total+=(file_size+effect_size+p.axes.size()*8+20+24+p.specific.size()+p.samples.size()*4+7)&~std::size_t{7};}if(total>32*1024*1024)throw std::bad_alloc{};const auto scratch=allocate_heap(memory,total);if(!scratch)throw std::bad_alloc{};memory.write(GuestAddress{scratch},std::vector<std::uint8_t>(total));std::vector<GuestCallback> callbacks;std::uint64_t at=scratch;
                                try{for(const auto& entry:entries){const auto& p=entry.parameters;const auto effect_at=at+file_size;write(at,file_size);memory.write(GuestAddress{at+4},entry.type);write(at+(x86_?20:24),effect_at,bits);put_string(at+(x86_?24:32),entry.name,false);write(effect_at,effect_size);write(effect_at+4,p.flags);write(effect_at+28,p.axes.size());auto area=effect_at+effect_size;write(effect_at+32,area,bits);area+=p.axes.size()*4;write(effect_at+32+unit,area,bits);area+=p.axes.size()*4;if(p.envelope){write(effect_at+32+unit*2,area,bits);area+=20;}const unsigned specific_size=p.channels?(x86_?16U:24U):static_cast<unsigned>(p.specific.size());write(effect_at+32+unit*3,specific_size);write(effect_at+(x86_?48:64),specific_size?area:0,bits);if(p.channels){write(area+8,p.samples.size());write(area+(x86_?12:16),area+specific_size,bits);area+=p.samples.size()*4;}area+=specific_size;const auto output=effect_output(o,p,effect_at,0x3ff,false);if(output)throw std::invalid_argument("file effect output");callbacks.push_back({arg(2),{at,arg(3)}});at=(area+7)&~UINT64_C(7);}
                                    enumerate(std::move(callbacks),scratch);return std::nullopt;
                                }catch(...){(void)free_heap(memory,scratch,state);throw;}
                            }
                        }}
                    }
                }else if(name=="BuildActionMap" || name=="SetActionMap"){
                    if(o.is_acquired)result=acquired;else result=action_map(o,arg(1),arg(2),static_cast<std::uint32_t>(arg(3)),name=="SetActionMap",wide);
                }else if(name=="CreateEffect"){
                    check(arg(3),unit);write(arg(3),0,bits);const auto type=iid_index(effect_guids,guid(arg(1)));
                    if(arg(4))result=0x80040110U;else if(type==UINT32_MAX || !(o.device.ff_effects&(1U<<type)))result=unsupported;
                    else {auto e=std::make_shared<Object>();e->kind=Object::Kind::Effect;e->initialized=true;e->parent=object;e->effect_index=type;if(arg(2)){e->parameters=effect_parameters(o,type,arg(2),0x3ff,{});e->parameter_mask=0x3ff;}e->effect=bridge.backend->effect(o.device_id,type,e->parameter_mask?&e->parameters:nullptr,result);if(!(result&0x80000000U) && e->effect)write(arg(3),create(e,0,false),bits);else if(!(result&0x80000000U))result=0x80004005U;}
                }else if(name=="Escape")result=escape(o,arg(1));
                else if(name=="EnumEffects" || name=="GetEffectInfo"){
                    const auto info=[&](unsigned index,std::uint64_t p){const unsigned size=wide?552U:292U;check(p,size);memory.write(GuestAddress{p},std::vector<std::uint8_t>(size));write(p,size);memory.write(GuestAddress{p+4},effect_guids[index]);write(p+20,effect_type(index)|0xfe00U);write(p+24,0x3ff);write(p+28,0x3ff);put_string(p+32,effect_names[index],wide);};
                    if(name=="GetEffectInfo"){const auto id=guid(arg(2));const auto index=iid_index(effect_guids,id);if(read(arg(1))!=(wide?552U:292U))result=invalid;else if(index==UINT32_MAX || !(o.device.ff_effects&(1U<<index)))result=not_found;else info(index,arg(1));}
                    else if(!arg(1) || arg(3)>5)result=invalid;else {std::vector<unsigned> indices;for(unsigned i=0;i<12;++i)if((o.device.ff_effects&(1U<<i)) && (!arg(3) || effect_type(i)==arg(3)))indices.push_back(i);if(!indices.empty()){const unsigned size=wide?552U:292U;const auto scratch=allocate_heap(memory,size*indices.size());if(!scratch)throw std::bad_alloc{};std::vector<GuestCallback> callbacks;for(unsigned i=0;i<indices.size();++i){const auto p=scratch+i*size;info(indices[i],p);callbacks.push_back({arg(1),{p,arg(2)}});}enumerate(std::move(callbacks),scratch);return std::nullopt;}}
                }else if(name=="GetForceFeedbackState"){check(arg(1),4);std::uint32_t value=0;result=bridge.backend->ff_state(o.device_id,value);if(!result)write(arg(1),value);}
                else if(name=="SendForceFeedbackCommand"){if(!arg(1) || (arg(1)&~0x3fU) || !std::has_single_bit(arg(1)))result=invalid;else if((result=state_error(o))==0){if(!(o.cooperative&1))result=0x80040205U;else result=bridge.backend->ff_command(o.device_id,static_cast<std::uint32_t>(arg(1)));}}
                else if(name=="EnumCreatedEffectObjects"){if(!arg(1) || arg(3))result=invalid;else {std::vector<GuestCallback> callbacks;for(const auto& [p,item]:bridge.objects)if(p==item.object->canonical && item.object->kind==Object::Kind::Effect && item.object->parent==object)callbacks.push_back({arg(1),{p,arg(2)}});if(!callbacks.empty()){enumerate(std::move(callbacks),0);return std::nullopt;}}}
                else result=unsupported;
            }else result=unsupported;
        }
    }catch(const std::domain_error&){result=no_interface;}catch(const std::invalid_argument&){result=invalid;}catch(const MemoryFault&){result=invalid;}catch(const std::bad_alloc&){result=0x8007000eU;}
    arg.return_to(state,result);return std::nullopt;
}
}
