#pragma once
// 自動化測試用的虛擬 XInput 控制器：X64ARM_AUTOMATION_GAMEPAD=<狀態檔> 時，在原生輸入後端的手把清單後加入一個標準控制器。
// 狀態檔存在即視為已連接，內容為以空白分隔的 key=value：buttons（XInput 按鍵位元，0x1000 為 A）、
// lx／ly／rx／ry（搖桿 -1～1）、lt／rt（扳機 0～1）；刪除檔案即拔除。只在測試啟用，其餘方法全部轉交原生後端。
#include "x64arm/input.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace x64arm::detail {
class AutomationGamepadBackend final : public input::Backend {
    std::shared_ptr<input::Backend> inner_;
    std::string path_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point checked_{};
    struct timespec modified_{};bool present_{};
    input::Gamepad pad_{};
    static input::Guid guid(){input::Guid value{};const char text[]="AJ-AUTOMATIONPAD";for(unsigned i=0;i<16;++i)value[i]=static_cast<std::uint8_t>(text[i]);return value;}
    void refresh(){
        // 每 10 ms 最多檢查一次修改時間；內容變更才重新解析。
        const auto now=std::chrono::steady_clock::now();if(now-checked_<std::chrono::milliseconds(10))return;checked_=now;
        struct stat info{};
        if(::stat(path_.c_str(),&info)!=0){present_=false;return;}
        if(present_ && info.st_mtimespec.tv_sec==modified_.tv_sec && info.st_mtimespec.tv_nsec==modified_.tv_nsec)return;
        modified_=info.st_mtimespec;present_=true;
        input::Gamepad pad{};pad.device=guid();pad.supported_buttons=0xf7ffU;pad.supported_triggers={true,true};pad.supported_sticks={true,true,true,true};
        std::ifstream stream(path_);std::string token;
        while(stream>>token){
            const auto equal=token.find('=');if(equal==std::string::npos)continue;
            const auto key=token.substr(0,equal);const auto value=token.substr(equal+1);
            if(key=="buttons")pad.buttons=static_cast<std::uint16_t>(std::strtoul(value.c_str(),nullptr,0));
            else {const auto number=std::strtof(value.c_str(),nullptr);
                if(key=="lx")pad.sticks[0]=number;else if(key=="ly")pad.sticks[1]=number;else if(key=="rx")pad.sticks[2]=number;else if(key=="ry")pad.sticks[3]=number;
                else if(key=="lt")pad.triggers[0]=number;else if(key=="rt")pad.triggers[1]=number;}
        }
        pad_=pad;
    }
public:
    AutomationGamepadBackend(std::shared_ptr<input::Backend> inner,std::string path):inner_(std::move(inner)),path_(std::move(path)){}
    std::vector<input::Device> devices()override{return inner_->devices();}
    std::uint64_t device_generation()override{return inner_->device_generation();}
    std::vector<input::Event> poll()override{return inner_->poll();}
    std::vector<input::Report> reports()override{return inner_->reports();}
    std::vector<input::Gamepad> gamepads()override{
        auto pads=inner_->gamepads();std::lock_guard lock(mutex_);refresh();if(present_)pads.push_back(pad_);return pads;
    }
    std::uint32_t rumble(const input::Guid& device,std::uint16_t left,std::uint16_t right)override{return device==guid()?0:inner_->rumble(device,left,right);}
    std::vector<input::PointerDevice> pointer_devices()override{return inner_->pointer_devices();}
    std::vector<input::PointerSample> pointer_samples()override{return inner_->pointer_samples();}
    std::uint32_t set_report(const input::Guid& device,unsigned id,std::span<const std::uint8_t> bytes)override{return inner_->set_report(device,id,bytes);}
    std::uint32_t get_report(const input::Guid& device,unsigned id,std::span<std::uint8_t> bytes)override{return inner_->get_report(device,id,bytes);}
    bool available(const input::Guid& device)override{if(device==guid()){std::lock_guard lock(mutex_);refresh();return present_;}return inner_->available(device);}
    std::uint32_t cooperative(const input::Guid& device,bool exclusive)override{return inner_->cooperative(device,exclusive);}
    void mouse_wheel(std::int32_t delta)override{inner_->mouse_wheel(delta);}
    void horizontal_wheel(std::int32_t delta)override{inner_->horizontal_wheel(delta);}
    void cursor_warped()override{inner_->cursor_warped();}
    bool observes_wheel()const override{return inner_->observes_wheel();}
    std::uint32_t control_panel(const input::Guid& device)override{return inner_->control_panel(device);}
    std::uint32_t configure(std::vector<input::Action>& actions,bool edit)override{return inner_->configure(actions,edit);}
    std::uint32_t ff_property(const input::Guid& device,unsigned property,std::uint32_t& value,bool set)override{return inner_->ff_property(device,property,value,set);}
    std::uint32_t ff_state(const input::Guid& device,std::uint32_t& state)override{return inner_->ff_state(device,state);}
    std::uint32_t ff_command(const input::Guid& device,std::uint32_t command)override{return inner_->ff_command(device,command);}
    std::uint32_t escape(const input::Guid& device,std::uint32_t code,std::span<const std::uint8_t> in,std::vector<std::uint8_t>& out)override{return inner_->escape(device,code,in,out);}
    std::shared_ptr<input::Effect> effect(const input::Guid& device,unsigned object,const input::EffectParameters* parameters,std::uint32_t& error)override{return inner_->effect(device,object,parameters,error);}
    std::uint32_t send(const input::Guid& device,unsigned object,std::uint32_t value)override{return inner_->send(device,object,value);}
};
// 原生輸入後端；設定自動化手把狀態檔時再包一層虛擬控制器。
inline std::shared_ptr<input::Backend> make_input_backend(){
    auto backend=input::make_backend();
    if(const auto* path=std::getenv("X64ARM_AUTOMATION_GAMEPAD");path && *path)return std::make_shared<AutomationGamepadBackend>(std::move(backend),path);
    return backend;
}
}
