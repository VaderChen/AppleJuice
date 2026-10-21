#pragma once
#include "x64arm/desktop_notifications.hpp"
#include <memory>
#include <vector>
namespace x64arm {
class NativeDesktopNotifications {
public:
    virtual ~NativeDesktopNotifications()=default;
    // 時間以單調時鐘的毫秒表示，與 Windows 客體時鐘分開。
    virtual bool update(const std::map<std::uint64_t,desktop::Icon>&,std::uint64_t now)=0;
    virtual bool focus(std::uint64_t)=0;
    virtual bool attention(std::uint64_t,void* native_window,bool caption,bool tray)=0;
    virtual std::optional<std::uint32_t> state()const=0;
    virtual std::vector<desktop::Event> poll(std::uint64_t now)=0;
    // 原生繪圖表面，供 UI 整合及快照使用；不轉移 AppKit 物件所有權。
    virtual void* icon_view(std::uint64_t)const=0;
    virtual void* balloon_window()const=0;
};
std::shared_ptr<NativeDesktopNotifications> make_native_desktop_notifications();
}
