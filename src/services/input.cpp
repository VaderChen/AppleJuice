#include "x64arm/input.hpp"
#include "dinput_interfaces.hpp"
#include <cstdio>
namespace x64arm::input {
std::string key_name(unsigned scan){
    static constexpr std::array<const char*,89> names{
        "Unknown","Escape","1","2","3","4","5","6","7","8","9","0","Minus","Equals","Backspace","Tab",
        "Q","W","E","R","T","Y","U","I","O","P","Left bracket","Right bracket","Enter","Left Control","A","S",
        "D","F","G","H","J","K","L","Semicolon","Apostrophe","Grave","Left Shift","Backslash","Z","X","C","V",
        "B","N","M","Comma","Period","Slash","Right Shift","Numpad multiply","Left Alt","Space","Caps Lock","F1","F2","F3","F4","F5",
        "F6","F7","F8","F9","F10","Num Lock","Scroll Lock","Numpad 7","Numpad 8","Numpad 9","Numpad subtract","Numpad 4","Numpad 5","Numpad 6","Numpad add","Numpad 1",
        "Numpad 2","Numpad 3","Numpad 0","Numpad decimal","Unknown","Unknown","OEM 102","F11","F12"};
    if(scan<names.size() && std::string_view(names[scan])!="Unknown")return names[scan];
    switch(scan){case 0x9c:return "Numpad Enter";case 0x9d:return "Right Control";case 0xb5:return "Numpad divide";case 0xb7:return "Print Screen";case 0xb8:return "Right Alt";case 0xc5:return "Pause";case 0xc7:return "Home";case 0xc8:return "Up";case 0xc9:return "Page Up";case 0xcb:return "Left";case 0xcd:return "Right";case 0xcf:return "End";case 0xd0:return "Down";case 0xd1:return "Page Down";case 0xd2:return "Insert";case 0xd3:return "Delete";case 0xdb:return "Left Windows";case 0xdc:return "Right Windows";case 0xdd:return "Application";default:break;}
    // 未命名 scan code 僅需十六進位，不讓裝置輪詢反覆進入 printf 格式化器。
    constexpr char hex[]="0123456789ABCDEF";char digits[2*sizeof(unsigned)]{};unsigned used=0;
    do{digits[used++]=hex[scan&15U];scan>>=4;}while(scan);
    if(used<2)digits[used++]='0';std::string label="Scan code ";
    while(used)label.push_back(digits[--used]);return label;
}
Device keyboard(){
    Device d;d.instance=d.product=dinput::GUID_SysKeyboard;d.kind=Kind::Keyboard;d.name="macOS System Keyboard";d.path="system:keyboard";
    d.usage_page=1;d.usage=6;
    // 本引擎聚合鍵盤：修飾鍵八個 bits、六鍵 array 與五個 LED output。
    d.hid_descriptor={0x05,1,0x09,6,0xa1,1,0x05,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
        0x95,1,0x75,8,0x81,1,0x95,5,0x75,1,0x05,8,0x19,1,0x29,5,0x91,2,0x95,1,0x75,3,0x91,1,
        0x95,6,0x75,8,0x15,0,0x25,0x65,0x05,7,0x19,0,0x29,0x65,0x81,0,0xc0};
    for(unsigned i=0;i<256;++i){Object o;o.kind=ObjectKind::Button;o.instance=i;o.maximum=1;o.name=key_name(i);d.objects.push_back(std::move(o));}
    d.values.resize(d.objects.size());return d;
}
Device mouse(){
    Device d;d.instance=d.product=dinput::GUID_SysMouse;d.kind=Kind::Mouse;d.name="macOS System Mouse";d.path="system:mouse";
    d.usage_page=1;d.usage=2;
    d.hid_descriptor={0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
        0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,0x17,0,0,0,0x80,0x27,0xff,0xff,0xff,0x7f,0x75,32,0x95,3,0x81,6,0xc0,0xc0};
    for(unsigned i=0;i<3;++i){Object o;o.kind=ObjectKind::Axis;o.instance=o.axis=i;o.usage_page=1;o.usage=static_cast<std::uint16_t>(i==2?0x38:0x30+i);o.minimum=INT32_MIN;o.maximum=INT32_MAX;o.relative=true;o.name=i==0?"X axis":i==1?"Y axis":"Wheel";d.objects.push_back(std::move(o));}
    for(unsigned i=0;i<8;++i){Object o;o.kind=ObjectKind::Button;o.instance=i;o.usage_page=9;o.usage=static_cast<std::uint16_t>(i+1);o.maximum=1;o.name="Button "+std::to_string(i+1);d.objects.push_back(std::move(o));}
    d.values.resize(d.objects.size());return d;
}
std::uint32_t default_offset(const Device& d,const Object& o){
    if(d.kind==Kind::Keyboard)return o.instance;
    if(d.kind==Kind::Mouse)return o.kind==ObjectKind::Axis?o.axis*4:12+o.instance;
    return o.kind==ObjectKind::Axis?o.axis*4:o.kind==ObjectKind::Pov?32+o.instance*4:48+o.instance;
}
}
