#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace x64arm::display_identity {
// 顯示配接器列在 PCI 列舉器下：程式常以 SetupDiGetClassDevs(…, L"PCI", …) 找顯示卡，再以 DXGI 描述比對硬體 ID。
// 廠商碼就是 DXGI 回報的 Apple，不冒充其他廠商。User32、SetupAPI、CM 與 DXGI 共用此身分。
inline constexpr std::u16string_view instance=u"PCI\\VEN_106B&DEV_0000&SUBSYS_00000000&REV_00\\0&0&0000";
// 顯示配接器的裝置介面路徑；devnode 與 EnumDisplayDevices 的 EDD_GET_DEVICE_INTERFACE_NAME 共用。
inline constexpr std::u16string_view adapter_interface_path=u"\\\\?\\PCI#VEN_106B&DEV_0000&SUBSYS_00000000&REV_00#0&0&0000#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}";
inline constexpr std::u16string_view class_name=u"{4d36e968-e325-11ce-bfc1-08002be10318}";
inline constexpr std::array<std::uint8_t,16> class_guid{0x68,0xe9,0x36,0x4d,0x25,0xe3,0xce,0x11,0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18};
inline constexpr std::u16string_view provider=u"x64arm Metal";
inline constexpr std::u16string_view version=u"0.1.0.0";
inline constexpr std::u16string_view date=u"10-06-2026";
// DriverDateData：與 date 相同日期（UTC 午夜）的 FILETIME。
inline constexpr std::uint64_t date_filetime=UINT64_C(0x01dd5525a1578000);
inline constexpr std::uint64_t version_number=UINT64_C(1)<<32;
// DXGI、DXCore 與 D3D12 使用相同的虛擬配接器 LUID。
inline constexpr std::uint64_t adapter_luid=0x78363461;
inline constexpr std::uint32_t vendor_id=0x106b;
// DXGI_ADAPTER_DESC 的 DeviceId、SubSysId、Revision 均為 0（配接器描述零初始化）。
// 程式常以描述組成 PnP 硬體 ID，再經 SetupAPI 找到裝置節點與驅動鍵，因此第一個硬體 ID 與執行個體前綴依相同數值組成。
inline constexpr std::u16string_view adapter_hardware_id=u"PCI\\VEN_106B&DEV_0000&SUBSYS_00000000&REV_00";
inline constexpr std::u16string_view adapter_name=u"Apple Metal / VideoToolbox 共用圖形裝置";
inline constexpr std::string_view adapter_name_utf8="Apple Metal / VideoToolbox 共用圖形裝置";

inline bool parallel_graphics_descriptions() noexcept {
    static const bool enabled=[] {
        const auto* value=std::getenv("X64ARM_PARALLEL_GRAPHICS_DESCRIPTIONS");
        return !value || std::string_view(value)!="0";
    }();
    return enabled;
}
inline constexpr std::size_t adapter_description_size(std::string_view symbol,bool x86) noexcept {
    if(symbol=="AdapterGetDesc")return x86?292U:304U;
    if(symbol=="AdapterGetDesc1")return x86?296U:312U;
    if(symbol=="AdapterGetDesc2" || symbol=="AdapterGetDesc3")return x86?304U:320U;
    return 0;
}
inline constexpr std::array<std::uint8_t,320> adapter_description(bool x86,std::uint64_t shared_memory) noexcept {
    std::array<std::uint8_t,320> result{};
    for(std::size_t i=0;i<adapter_name.size();++i){
        result[i*2]=static_cast<std::uint8_t>(adapter_name[i]);
        result[i*2+1]=static_cast<std::uint8_t>(adapter_name[i]>>8);
    }
    const auto word=[&](std::size_t at,std::uint32_t value){
        for(unsigned i=0;i<4;++i)result[at+i]=static_cast<std::uint8_t>(value>>(8*i));
    };
    // SIZE_T 依客體架構排列。UMA 沒有獨立顯存，搶占粒度為 DMA buffer 邊界。
    word(256,vendor_id);
    word(x86?280:288,static_cast<std::uint32_t>(x86?std::min<std::uint64_t>(shared_memory,UINT32_MAX):shared_memory));
    if(!x86)word(292,static_cast<std::uint32_t>(shared_memory>>32));
    word(x86?284:296,static_cast<std::uint32_t>(adapter_luid));
    word(x86?288:300,static_cast<std::uint32_t>(adapter_luid>>32));
    return result;
}

// WARP（IDXGIFactory2+::EnumWarpAdapter）：Windows 8 以後永遠存在的軟體配接器，有程式假設一定拿得到（3DMark Time Spy 不檢查失敗）。
// 依 Windows 的身分回報（Microsoft Basic Render Driver、廠商 0x1414、裝置 0x8C、DXGI_ADAPTER_FLAG_SOFTWARE），LUID 與硬體配接器不同；
// 沒有另一套軟體光柵器，在它上面建立的裝置仍由同一個 Metal 裝置執行。沒有顯示輸出。
inline constexpr std::uint64_t warp_adapter_luid=0x78363462;
inline constexpr std::u16string_view warp_adapter_name=u"Microsoft Basic Render Driver";
inline constexpr std::array<std::uint8_t,320> warp_adapter_description(bool x86,std::uint64_t shared_memory) noexcept {
    std::array<std::uint8_t,320> result{};
    for(std::size_t i=0;i<warp_adapter_name.size();++i){result[i*2]=static_cast<std::uint8_t>(warp_adapter_name[i]);result[i*2+1]=static_cast<std::uint8_t>(warp_adapter_name[i]>>8);}
    const auto word=[&](std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)result[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    word(256,0x1414);word(260,0x8c);
    word(x86?280:288,static_cast<std::uint32_t>(x86?std::min<std::uint64_t>(shared_memory,UINT32_MAX):shared_memory));
    if(!x86)word(292,static_cast<std::uint32_t>(shared_memory>>32));
    word(x86?284:296,static_cast<std::uint32_t>(warp_adapter_luid));word(x86?288:300,static_cast<std::uint32_t>(warp_adapter_luid>>32));
    word(x86?292:304,2); // DXGI_ADAPTER_FLAG_SOFTWARE（DESC1 起）
    return result;
}

// 監視器：GUID_DEVCLASS_MONITOR 與 GUID_DEVINTERFACE_MONITOR。User32、SetupAPI 與 CM 共用下列身分。
inline constexpr std::u16string_view monitor_class_name=u"{4d36e96e-e325-11ce-bfc1-08002be10318}";
inline constexpr std::array<std::uint8_t,16> monitor_class_guid{0x6e,0xe9,0x36,0x4d,0x25,0xe3,0xce,0x11,0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18};
inline constexpr std::array<std::uint8_t,16> monitor_interface_guid{0x5f,0x7b,0xf0,0xe6,0x97,0xee,0x90,0x4a,0xb0,0x76,0x33,0xf5,0x7b,0xf4,0xea,0xa7};
inline constexpr std::u16string_view monitor_name=u"x64arm Virtual Monitor";
// 以下的 number 為 1 起算的顯示器序號，與 \\.\DISPLAYn 相同。
inline std::u16string monitor_digits(std::uint32_t value,std::uint32_t base,std::size_t minimum){
    std::u16string text;
    do{const auto digit=value%base;text.insert(text.begin(),static_cast<char16_t>(digit<10?u'0'+digit:u'A'+digit-10));value/=base;}while(value || text.size()<minimum);
    return text;
}
// 製造商碼 XMA 為相容層自用，不冒充真實廠商；格式與 PnP 硬體 ID 相同（三字母加四位十六進位）。
inline std::u16string monitor_hardware_id(std::uint32_t number){return u"XMA"+monitor_digits(number,16,4);}
inline std::u16string monitor_instance(std::uint32_t number){return u"DISPLAY\\"+monitor_hardware_id(number)+u"\\1&0&UID"+monitor_digits(number,10,1);}
// 驅動鍵位於 Control\Class\{類別}\000n；EnumDisplayDevices 的 DeviceID 與 DeviceKey 指向同一個編號。
inline std::u16string monitor_driver_instance(std::uint32_t number){return monitor_digits(number,10,4);}
inline std::u16string monitor_device_id(std::uint32_t number){return u"MONITOR\\"+monitor_hardware_id(number)+u"\\"+std::u16string(monitor_class_name)+u"\\"+monitor_driver_instance(number);}
inline std::u16string monitor_device_key(std::uint32_t number){return u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\"+std::u16string(monitor_class_name)+u"\\"+monitor_driver_instance(number);}
inline std::u16string monitor_interface_path(std::uint32_t number){return u"\\\\?\\DISPLAY#"+monitor_hardware_id(number)+u"#1&0&UID"+monitor_digits(number,10,1)+u"#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";}
// 上述執行個體與介面路徑的共同前綴（已摺疊為小寫），判斷呼叫是否指向監視器時使用。
inline constexpr std::u16string_view monitor_instance_prefix=u"display\\";
inline constexpr std::u16string_view monitor_interface_prefix=u"\\\\?\\display#";
// EDID 1.4 基本區塊。第 54 位元組起的 DTD 為偏好時序，寬高即顯示器尺寸；遮沒區間依 CVT-RB（水平 160、最短垂直遮沒 460 微秒）。
// DTD 的寬高欄只有 12 位元，超過 4095 時等比減半；像素時脈欄上限 655.35 MHz，超過時截在上限，推得的更新率因而偏低。
// 實體尺寸未知（與 GetDeviceCaps(HORZSIZE)＝0 一致），依 EDID 規格填 0 表示未定義，不編造尺寸。
constexpr std::array<std::uint8_t,128> monitor_edid(std::uint32_t number,std::uint32_t width,std::uint32_t height,std::uint32_t refresh){
    std::array<std::uint8_t,128> edid{};
    const auto put=[&edid](std::size_t at,std::uint64_t value){edid[at]=static_cast<std::uint8_t>(value);};
    constexpr std::array<std::uint8_t,8> signature{0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00};
    // sRGB 原色與 D65 白點的 10 位元座標編碼，與第 24 位元組的 sRGB 預設旗標一致。
    constexpr std::array<std::uint8_t,10> chromaticity{0xee,0x91,0xa3,0x54,0x4c,0x99,0x26,0x0f,0x50,0x54};
    constexpr std::array<std::uint8_t,13> label{'x','6','4','a','r','m',0x0a,0x20,0x20,0x20,0x20,0x20,0x20};
    std::copy(signature.begin(),signature.end(),edid.begin());
    // XMA 每字母 5 位元（A＝1），大端序；產品碼與序號依顯示器序號區分多台。
    put(8,0x61);put(9,0xa1);put(10,number);put(11,number>>8);
    for(std::size_t at=0;at<4;++at)put(12+at,number>>(8*at));
    // 製造週未指定；製造年與 display_identity::date 相同（2026－1990）。版本 1.4。
    put(17,2026-1990);put(18,1);put(19,4);
    // 數位輸入、每色 8 位元、介面未定義；gamma 2.2；RGB 4:4:4、sRGB 預設、偏好時序含原生格式。
    put(20,0xa0);put(23,120);put(24,0x06);
    std::copy(chromaticity.begin(),chromaticity.end(),edid.begin()+25);
    for(std::size_t at=38;at<54;++at)put(at,1); // 標準時序全部未使用。
    while(width>4095 || height>4095){width=(width+1)/2;height=(height+1)/2;}
    const std::uint32_t rate=std::clamp<std::uint32_t>(refresh?refresh:60,1,500);
    constexpr std::uint32_t hblank=160,hfront=48,hsync=32,vfront=3,vsync=5,vback=6;
    const std::uint32_t idle=1000000-460*rate;
    const std::uint32_t vblank=std::max<std::uint32_t>(vfront+vsync+vback,static_cast<std::uint32_t>((UINT64_C(460)*height*rate+idle-1)/idle));
    const std::uint64_t clock=std::clamp<std::uint64_t>((std::uint64_t{width+hblank}*(height+vblank)*rate+5000)/10000,1,65535);
    constexpr std::size_t dtd=54;
    put(dtd,clock);put(dtd+1,clock>>8);
    put(dtd+2,width);put(dtd+3,hblank);put(dtd+4,((width>>8)<<4)|(hblank>>8));
    put(dtd+5,height);put(dtd+6,vblank);put(dtd+7,((height>>8)<<4)|(vblank>>8));
    put(dtd+8,hfront);put(dtd+9,hsync);put(dtd+10,(vfront<<4)|vsync);
    put(dtd+17,0x1a); // 非交錯、數位分離同步、水平正極性、垂直負極性。
    // 監視器名稱描述子；其餘兩個描述子為 dummy（標記 0x10）。
    put(75,0xfc);std::copy(label.begin(),label.end(),edid.begin()+77);
    put(93,0x10);put(111,0x10);
    std::uint32_t sum=0;for(std::size_t at=0;at<127;++at)sum+=edid[at];
    put(127,0U-sum);
    return edid;
}
}
