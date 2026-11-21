#pragma once
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
// 系統視窗程序的名稱與 參考實作 的內建類別拼法一致，使用者註冊名稱保留原大小寫。
inline std::u16string_view builtin_class_name(std::u16string_view key){
    static constexpr std::pair<std::u16string_view,std::u16string_view> names[]{
        {u"button",u"Button"},{u"static",u"Static"},{u"edit",u"Edit"},{u"listbox",u"ListBox"},
        {u"combobox",u"ComboBox"},{u"combolbox",u"ComboLBox"},{u"scrollbar",u"ScrollBar"},
        {u"mdiclient",u"MDIClient"},{u"message",u"Message"},{u"ime",u"IME"},
        {u"#32768",u"#32768"},{u"#32769",u"#32769"},{u"#32770",u"#32770"},{u"#32772",u"#32772"}
    };
    for(const auto& [stored,canonical]:names)if(key==stored)return canonical;
    return {};
}
inline std::u16string_view window_class_name(const GuiWindow& window,bool builtin,bool real){
    if(real){
        if(window.dialog)return u"#32770";
        if(const auto base=builtin_class_name(window.control_class);!base.empty())return base;
    }
    if(builtin){
        if(const auto name=builtin_class_name(window.class_name);!name.empty())return name;
    }
    return window.class_name;
}
inline std::uint64_t write_window_class_name(GuestMemory& memory,const WindowsCall& arg,
        std::string_view symbol,bool x86,std::u16string_view name,std::uint32_t& error){
    const bool native=symbol=="NtUserGetClassName", wide=native || symbol.ends_with('W');
    auto output=arg(1);
    auto capacity=static_cast<std::uint32_t>(arg(2));
    if(native){
        const auto descriptor=arg(2);
        if(!descriptor){error=87;return 0;}
        const auto bytes=memory.load(GuestAddress{descriptor+2},16);
        if(bytes<=sizeof(char16_t)){error=122;return 0;}
        capacity=static_cast<std::uint32_t>(bytes/sizeof(char16_t));
        output=memory.load(GuestAddress{descriptor+(x86?4:8)},x86?32:64);
    }else if(static_cast<std::int32_t>(capacity)<=0){
        if(wide)error=122;
        return 0;
    }
    if(wide && !native){
        capacity=static_cast<std::uint16_t>(capacity*sizeof(char16_t))/sizeof(char16_t);
        if(capacity<=1){error=122;return 0;}
    }
    if(!output){error=87;return 0;}
    if(wide){
        const auto n=std::min<std::size_t>(name.size(),capacity-1);
        memory.write(GuestAddress{output},utf16_bytes(name.substr(0,n)));
        return n;
    }
    const auto bytes=utf16_to_utf8_lossy(name);
    auto n=std::min<std::size_t>(bytes.size(),capacity-1);
    while(n && n<bytes.size() && (static_cast<unsigned char>(bytes[n])&0xc0U)==0x80U)--n;
    memory.write(GuestAddress{output},{reinterpret_cast<const std::uint8_t*>(bytes.data()),n});
    memory.store(GuestAddress{output+n},8,0);
    return n;
}
}
