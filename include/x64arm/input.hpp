#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x64arm::input {
using Guid = std::array<std::uint8_t,16>;
enum class Kind { Keyboard, Mouse, Joystick, Gamepad, Hid };
enum class ObjectKind { Axis, Button, Pov };
struct Object {
    ObjectKind kind{};
    unsigned instance{}, axis{};
    std::uint16_t usage_page{}, usage{};
    std::int32_t minimum{}, maximum{}, physical_minimum{}, physical_maximum{};
    bool relative{}, actuator{};
    std::string name;
    bool operator==(const Object&) const = default;
};
struct Device {
    Guid instance{}, product{};
    Kind kind{};
    std::string name, path;
    std::uint32_t vendor{}, product_id{}, ff_effects{}, ff_storage{};
    bool attached{true};
    std::vector<Object> objects;
    std::vector<std::int64_t> values;
    std::uint16_t usage_page{},usage{},version{};
    std::string manufacturer{},serial{};
    // USB 字串描述元索引（iManufacturer／iProduct／iSerialNumber）；0 表示裝置未提供。
    std::array<std::uint8_t,3> string_indices{};
    std::vector<std::uint8_t> hid_descriptor{};
    bool operator==(const Device&) const = default;
};
struct Event { Guid device{}; unsigned object{}; std::int64_t value{};std::uint32_t extra{};bool repeat{}; };
// 標準控制器 profile；軸為 [-1,1]、扳機為 [0,1]，不在 API 層猜測 HID 按鈕順序。
struct Gamepad {
    Guid device{};
    std::uint16_t buttons{},supported_buttons{};
    std::array<float,2> triggers{};
    std::array<float,4> sticks{};
    std::array<bool,2> supported_triggers{},rumble{};
    std::array<bool,4> supported_sticks{};
};
// IOHID 原始 wire report：具 ID 的報告保留第一個 ID byte；ID 0 不含 Windows 的補零 byte。
struct Report {Guid device{};std::uint32_t id{};std::vector<std::uint8_t> bytes{};};
struct Control {Guid device{};std::uint32_t object{};std::string label;};
struct Action {std::string label;std::vector<Control> controls;unsigned selected{};};
struct EffectParameters {
    std::uint32_t flags{0x12}, duration{UINT32_MAX}, sample_period{}, gain{10000}, trigger{UINT32_MAX}, repeat{}, delay{};
    std::vector<std::uint32_t> axes;
    std::vector<std::int32_t> direction;
    std::optional<std::array<std::uint32_t,4>> envelope;
    std::vector<std::uint8_t> specific;
    std::vector<std::int32_t> samples;
    std::uint32_t channels{}, custom_period{};
};
struct FileEffect {Guid type{};std::string name;EffectParameters parameters;};
std::vector<FileEffect> read_effect_file(std::span<const std::uint8_t>);
std::vector<std::uint8_t> write_effect_file(std::span<const FileEffect>);
class Effect {
public:
    virtual ~Effect()=default;
    virtual std::uint32_t set(const EffectParameters&,std::uint32_t)=0;
    virtual std::uint32_t start(std::uint32_t,std::uint32_t)=0;
    virtual std::uint32_t stop()=0;
    virtual std::uint32_t download()=0;
    virtual std::uint32_t unload()=0;
    virtual std::uint32_t status(std::uint32_t&)=0;
    virtual std::uint32_t escape(std::uint32_t,std::span<const std::uint8_t>,std::vector<std::uint8_t>&)=0;
};
// 指標後端提供已能逐接觸辨識的裝置；一般滑鼠不列入 digitizer 清單。
struct PointerDevice {
    Guid device{};
    std::uint32_t type{}; // 1 integrated pen、2 external pen、3 touch、4 touchpad。
    std::string name;
    std::array<std::int32_t,4> logical_rectangle{};
    std::optional<std::array<std::int32_t,4>> himetric_rectangle{};
    std::uint16_t max_contacts{1};
};
struct PointerSample {
    Guid device{};std::uint32_t contact{};
    std::int32_t x{},y{};
    bool in_range{},in_contact{},barrel{},inverted{},eraser{};
    std::uint32_t mask{},pressure{},rotation{};
    std::int32_t tilt_x{},tilt_y{};
    std::array<std::int32_t,4> contact_rectangle{};
};
class Backend {
public:
    virtual ~Backend()=default;
    virtual std::vector<Device> devices()=0;
    // devices() 內容的換代編號：相同編號保證清單完全相同；0 表示不提供，呼叫端須每次重新取得。
    virtual std::uint64_t device_generation(){return 0;}
    virtual std::vector<Event> poll()=0;
    virtual std::vector<Report> reports(){return {};}
    virtual std::vector<Gamepad> gamepads(){return {};}
    virtual std::uint32_t rumble(const Guid&,std::uint16_t,std::uint16_t){return 50;}
    virtual std::vector<PointerDevice> pointer_devices(){return {};}
    virtual std::vector<PointerSample> pointer_samples(){return {};}
    virtual std::uint32_t set_report(const Guid&,unsigned,std::span<const std::uint8_t>){return 50;}
    // 首位元組為 report ID；成功時覆寫其餘報告內容。
    virtual std::uint32_t get_report(const Guid&,unsigned,std::span<std::uint8_t>){return 50;}
    virtual bool available(const Guid&)=0;
    virtual std::uint32_t cooperative(const Guid&,bool){return 0;}
    virtual void mouse_wheel(std::int32_t){}
    virtual void horizontal_wheel(std::int32_t){}
    // 宿主游標 warp 已由相容層注入；同步取樣基準，避免再當成實體位移。
    virtual void cursor_warped(){}
    virtual bool observes_wheel()const{return false;}
    virtual std::uint32_t control_panel(const Guid&){return 0x80004001U;}
    virtual std::uint32_t configure(std::vector<Action>&,bool){return 0x80004001U;}
    virtual std::uint32_t ff_property(const Guid&,unsigned,std::uint32_t&,bool){return 0x80004001U;}
    virtual std::uint32_t ff_state(const Guid&,std::uint32_t&){return 0x80004001U;}
    virtual std::uint32_t ff_command(const Guid&,std::uint32_t){return 0x80004001U;}
    virtual std::uint32_t escape(const Guid&,std::uint32_t,std::span<const std::uint8_t>,std::vector<std::uint8_t>&){return 0x80004001U;}
    virtual std::shared_ptr<Effect> effect(const Guid&,unsigned,const EffectParameters*,std::uint32_t& error){error=0x80004001U;return {};}
    virtual std::uint32_t send(const Guid&,unsigned,std::uint32_t){return 0x80004001U;}
};
std::shared_ptr<Backend> make_backend();
Device keyboard();
Device mouse();
std::uint32_t default_offset(const Device&,const Object&);
std::string key_name(unsigned);
}
