// gdi32 其餘 DC／物件／初始化入口：SetObjectOwner、SetMagicColors、EnableEUDC、SetVirtualResolution、GdiConvertToDevmodeW、
// GdiDllInitialize、GdiInitializeLanguagePack 與 GdiEntry13（DdQueryDisplaySettingsUniqueness）。
// 物件與 DC 驗證沿用 GDI 既有狀態；虛擬解析度存於 DC，由 GetDeviceCaps 讀取；顯示設定唯一值隨 ChangeDisplaySettings 遞增。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_geometry.hpp"
#include "windows_wmf.hpp"
#include <algorithm>
#include <bit>
#include <utility>
#include <vector>

namespace x64arm {
namespace {
std::uint32_t display_device_cap(unsigned index,const std::array<std::int32_t,4>& screen,
                                 const std::optional<std::array<std::uint32_t,4>>& mode,
                                 const std::array<std::int32_t,4>& virtual_values){
    const auto millimeters=[](std::int64_t pixels){return static_cast<std::uint32_t>((pixels*254+480)/960);};
    const bool virtual_set=virtual_values[0]>0;
    switch(index){
    case 0:return 0x4000;                       // DRIVERVERSION
    case 2:return 1;                            // TECHNOLOGY：DT_RASDISPLAY
    case 4:return virtual_set?static_cast<std::uint32_t>(virtual_values[2]):millimeters(screen[2]); // HORZSIZE
    case 6:return virtual_set?static_cast<std::uint32_t>(virtual_values[3]):millimeters(screen[3]); // VERTSIZE
    case 8:return virtual_set?static_cast<std::uint32_t>(virtual_values[0]):static_cast<std::uint32_t>(screen[2]); // HORZRES
    case 118:return static_cast<std::uint32_t>(screen[2]); // DESKTOPHORZRES
    case 10:return virtual_set?static_cast<std::uint32_t>(virtual_values[1]):static_cast<std::uint32_t>(screen[3]); // VERTRES
    case 117:return static_cast<std::uint32_t>(screen[3]); // DESKTOPVERTRES
    case 12:return mode?(*mode)[2]:32; // BITSPIXEL
    case 14:return 1;                           // PLANES
    case 16:case 18:case 24:return UINT32_MAX;   // NUMBRUSHES、NUMPENS、NUMCOLORS：超過 8 位元色時為 -1
    case 28:return 0x1ff;                       // CURVECAPS
    case 30:return 0xfe;                        // LINECAPS
    case 32:return 0xff;                        // POLYGONALCAPS
    case 34:return 0x7e07;                      // TEXTCAPS
    case 36:return 1;                           // CLIPCAPS：CP_RECTANGLE
    case 38: // RASTERCAPS：顯示後端支援位元區塊傳輸、DIB 轉換、縮放及區域填色。
        return 0x0001 | 0x0080 | 0x0200 | 0x0800 | 0x1000 | 0x2000;
    case 40:case 42:return 36;                  // ASPECTX、ASPECTY
    case 44:return 51;                          // ASPECTXY
    case 88:case 90:return 96;                  // LOGPIXELSX、LOGPIXELSY
    case 108:return 24;                         // COLORRES
    case 116:return mode && (*mode)[3]?(*mode)[3]:60; // VREFRESH
    default:return 0;                           // 其餘未提供的裝置能力
    }
}
}
void WindowsServices::read_gdi_device_caps(std::uint64_t dc,unsigned first,std::span<std::uint32_t> output){
    const MetafileState::DeviceCaps* recorded=nullptr;
    if(metafile_playback_){
        const auto it=metafile_playback_->recorder_caps.find(dc);
        if(it!=metafile_playback_->recorder_caps.end())recorded=&it->second;
    }
    const bool printer=is_printer_dc(dc);
    const auto screen=printer || recorded?std::array<std::int32_t,4>{}:gui_screen_area();
    const auto context=gui_.contexts.find(dc);
    const auto values=context==gui_.contexts.end()?std::array<std::int32_t,4>{}:context->second.virtual_resolution;
    for(auto& value:output){
        const auto index=first++;
        if(recorded && values[0]>0 && (index==4 || index==6 || index==8 || index==10)){
            const auto slot=index<8?index/2:index/2-4;
            value=static_cast<std::uint32_t>(values[slot]);
        }else if(recorded)value=index<recorded->size()?(*recorded)[index]:0;
        else if(printer)value=*printer_device_cap(dc,index);
        else value=display_device_cap(index,screen,gui_.display_mode,values);
    }
}
std::span<const WindowsServiceDescriptor> gdi_misc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","GetDeviceCaps",2,0,0,2},{"win32u.dll","NtGdiGetDeviceCaps",2,0,0,2},
        {"win32u.dll","NtGdiGetDeviceCapsAll",2},{"win32u.dll","NtGdiGetDeviceWidth",1},
        {"win32u.dll","NtGdiGetDCPoint",3,0,0,2},{"win32u.dll","NtGdiGetDCDword",3,0,0,2},
        {"win32u.dll","NtGdiGetAndSetDCDword",4,0,0,6},
        {"gdi32.dll","SetObjectOwner",2},{"gdi32.dll","SetMagicColors",3},{"gdi32.dll","EnableEUDC",1},
        {"gdi32.dll","SetVirtualResolution",5},{"gdi32.dll","GdiConvertToDevmodeW",1},
        {"gdi32.dll","GdiDllInitialize",3},{"gdi32.dll","GdiInitializeLanguagePack",1},{"gdi32.dll","GdiEntry13",0}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](unsigned error,std::uint64_t value=0){last_error_=error;return finish(value);};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto known_dc=[&](std::uint64_t dc){
        return dc && (gui_.contexts.contains(dc) || gui_.dc_objects.contains(dc) || (dc>=0x10000000 && gui_.windows.contains(dc-0x10000000)));
    };
    if(name=="GetDeviceCaps" || name=="NtGdiGetDeviceCaps" || name=="NtGdiGetDeviceCapsAll"){
        const auto dc=arg(0);
        if(!known_dc(dc))return fail(6);
        if(name!="NtGdiGetDeviceCapsAll"){
            std::uint32_t value{};
            read_gdi_device_caps(dc,d(1),{&value,1});
            return finish(value);
        }
        if(!arg(1))return fail(87);
        MetafileState::DeviceCaps device{};
        read_gdi_device_caps(dc,0,device);
        const auto cap=[&](unsigned index){return device[index];};
        // DEVCAPS 由固定寬度欄位組成，兩種客體使用相同配置。實體長寬取自同一份裝置狀態。
        const auto width=cap(4),height=cap(6);
        const std::array<std::uint32_t,36> caps{
            cap(0),cap(2),(width+500U)/1000U,(height+500U)/1000U,width,height,
            cap(8),cap(10),cap(12),cap(14),cap(18),cap(22),cap(24),cap(38),
            cap(40),cap(42),cap(44),cap(88),cap(90),cap(104),cap(108),
            cap(110),cap(111),cap(112),cap(113),cap(34),cap(116),cap(118),cap(117),
            cap(119),0,0,0,0,cap(120),cap(121)};
        try{
            memory.check(GuestAddress{arg(1)},sizeof caps,Access::Write);
            memory.write(GuestAddress{arg(1)},{reinterpret_cast<const std::uint8_t*>(caps.data()),sizeof caps});
        }catch(const MemoryFault&){return fail(998);}
        return finish(1);
    }
    if(name=="NtGdiGetDeviceWidth"){
        const auto dc=arg(0);
        if(!known_dc(dc))return fail(6);
        if(metafile_playback_){
            const auto recorded=metafile_playback_->recorder_caps.find(dc);
            if(recorded!=metafile_playback_->recorder_caps.end())return finish(recorded->second[8]);
        }
        if(const auto width=printer_device_cap(dc,8))return finish(*width);
        const auto context=gui_.contexts.find(dc);
        if(dc>=0x10000000){
            const auto window=gui_.windows.find(dc-0x10000000);
            if(window!=gui_.windows.end()){
                const bool frame=context!=gui_.contexts.end() && context->second.window_frame;
                return finish(frame?gui_outer_size(window->second)[0]:window->second.width);
            }
        }
        if(dc==0x10000000 || (context!=gui_.contexts.end() && context->second.display))return finish(static_cast<std::uint32_t>(gui_screen_area()[2]));
        if(context!=gui_.contexts.end()){
            const auto bitmap=gui_.bitmaps.find(context->second.bitmap);
            if(bitmap!=gui_.bitmaps.end())return finish(bitmap->second.width);
        }
        return finish(1); // 新建記憶體 DC 選入的預設單色點陣圖。
    }
    if(name=="NtGdiGetDCDword" || name=="NtGdiGetAndSetDCDword"){
        const bool exchange=name=="NtGdiGetAndSetDCDword";
        const auto handle=arg(0),output=arg(exchange?3:2);
        const auto selector=d(1);
        if(!output)return fail(87);
        if(!known_dc(handle))return fail(6);
        if(selector>9 || (exchange && !selector))return fail(87);
        try{
            memory.check(GuestAddress{output},4,Access::Write);
            auto& dc=gui_.contexts[handle];
            std::uint32_t value=0;
            if(!exchange){
                switch(selector){
                case 0:break; // 本引擎的 DC 不啟用 GDI 列印日誌。
                case 1:value=dc.rel_abs;break;
                case 2:value=static_cast<std::uint32_t>(dc.break_extra);break;
                case 3:value=static_cast<std::uint32_t>(dc.break_count);break;
                case 4:value=dc.arc_direction;break;
                case 5:{
                    const auto saved=gui_.saved_contexts.find(handle);
                    value=saved==gui_.saved_contexts.end()?1:1+static_cast<std::uint32_t>(saved->second.size());
                    break;
                }
                case 6:value=static_cast<std::uint32_t>(nested_service(state,memory,"gdi32.dll","GetFontLanguageInfo",{handle}));break;
                case 7:{
                    if(dc.information){value=2;break;}
                    const auto object=nested_service(state,memory,"gdi32.dll","GetObjectType",{handle});
                    // 記憶體 DC 持有點陣圖；中繼檔 DC 記錄命令，使用參考裝置的資訊。
                    if(object==10)value=1;
                    else if(object==12 || object==4)value=2;
                    break;
                }
                case 8:value=dc.map_mode;break;
                case 9:value=static_cast<std::uint32_t>(dc.char_extra);break;
                }
            }else{
                const auto input=d(2);
                switch(selector){
                case 1:break; // PDF／點陣後端沒有待清除的 EPSPRINTING escape。
                case 2:value=std::exchange(dc.copy_count,input);break;
                case 3:value=std::exchange(dc.alignment,input);break;
                case 4:value=std::exchange(dc.rel_abs,input);break;
                case 5:value=static_cast<std::uint32_t>(std::exchange(dc.char_extra,std::bit_cast<std::int32_t>(input)));break;
                case 6:break; // 保留的選字型選擇器；實際字型控制代碼由 NtGdiSelectFont 處理。
                case 7:
                    if(input>1)return fail(87);
                    value=std::exchange(dc.mapper_flags,input);break;
                case 8:
                    value=static_cast<std::uint32_t>(nested_service(state,memory,"gdi32.dll","SetMapMode",{handle,input}));
                    break;
                case 9:
                    if(input!=1 && input!=2)return fail(87);
                    value=std::exchange(dc.arc_direction,input);break;
                }
            }
            memory.store(GuestAddress{output},32,value);
            return finish(1);
        }catch(const MemoryFault&){return fail(998);}
    }
    if(name=="NtGdiGetDCPoint"){
        constexpr std::array<std::string_view,6> queries{
            "GetViewportExtEx","GetWindowExtEx","GetViewportOrgEx",
            "GetWindowOrgEx","GetAspectRatioFilterEx","GetDCOrgEx"};
        if(!known_dc(arg(0)))return fail(6);
        const auto index=d(1);
        if(!index || index>queries.size() || !arg(2))return fail(87);
        try{memory.check(GuestAddress{arg(2)},8,Access::Write);}
        catch(const MemoryFault&){return fail(998);}
        return finish(nested_service(state,memory,"gdi32.dll",queries[index-1],{arg(0),arg(2)}));
    }
    if(name=="GdiDllInitialize" || name=="GdiInitializeLanguagePack")return finish(1); // 程序附加與語言套件字型連結初始化：狀態於服務啟動時已建立。
    if(name=="GdiEntry13")return finish(gui_.display_uniqueness);
    if(name=="EnableEUDC"){gui_.eudc_enabled=d(0)!=0;return finish(1);}
    if(name=="SetObjectOwner"){
        // NT 的 GDI 物件一律屬於建立它的程序；只驗證 handle，擁有者參數沒有可轉移的對象。
        if(!arg(0) || !nested_service(state,memory,"gdi32.dll","GetObjectType",{arg(0)}))return fail(6);
        return finish(1);
    }
    if(name=="SetMagicColors"){
        // 魔術色是調色盤裝置的系統索引 8、9、246、247；原生 true-color 裝置沒有系統調色盤（與 GetSystemPaletteEntries 一致）。
        if(!known_dc(arg(0)))return fail(6);
        const auto index=d(2);if(index!=8 && index!=9 && index!=246 && index!=247)return fail(87);
        return finish(0);
    }
    if(name=="SetVirtualResolution"){
        // 全部為 0 時回到實體裝置值；其餘情況四個值都必須為正。之後 GetDeviceCaps 的 HORZRES／VERTRES／HORZSIZE／VERTSIZE 依此回報。
        const auto context=gui_.contexts.find(arg(0));if(context==gui_.contexts.end())return fail(6);
        std::array<std::int32_t,4> values{};for(unsigned i=0;i<4;++i)values[i]=static_cast<std::int32_t>(d(i+1));
        const bool reset=std::all_of(values.begin(),values.end(),[](auto v){return v==0;});
        if(!reset && std::any_of(values.begin(),values.end(),[](auto v){return v<=0;}))return fail(87);
        context->second.virtual_resolution=values;return finish(1);
    }
    if(name=="GdiConvertToDevmodeW"){
        // DEVMODEA → DEVMODEW：裝置名稱與表單名稱（各 32 個 ANSI 位元組）轉為 UTF-16，其餘欄位依位移照搬，
        // 驅動私有資料接在 dmSize 之後；結果配置於程序 heap，由呼叫端 HeapFree。
        if(!arg(0))return finish(0);
        const auto source=arg(0);const auto size_a=static_cast<unsigned>(memory.load(GuestAddress{source+36},16)),extra=static_cast<unsigned>(memory.load(GuestAddress{source+38},16));
        constexpr unsigned name_a=32,form_a=70,form_end_a=102;
        if(size_a<form_a || size_a>156)return fail(87,0);
        std::vector<std::uint8_t> input(size_a+extra);memory.read(GuestAddress{source},input);
        const auto convert=[&](std::span<const std::uint8_t> field,std::vector<std::uint8_t>& out){
            const auto end=std::find(field.begin(),field.end(),std::uint8_t{0});
            auto text=utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(field.data()),static_cast<std::size_t>(end-field.begin())));
            if(text.size()>31)text.resize(31);
            const auto at=out.size();out.resize(at+64,0);
            for(std::size_t i=0;i<text.size();++i){out[at+2*i]=static_cast<std::uint8_t>(text[i]);out[at+2*i+1]=static_cast<std::uint8_t>(text[i]>>8);}
        };
        std::vector<std::uint8_t> output;output.reserve(220+extra);
        convert(std::span(input).first(name_a),output);
        output.insert(output.end(),input.begin()+name_a,input.begin()+std::min(size_a,form_a));
        if(size_a>form_a){
            convert(std::span(input).subspan(form_a,std::min(size_a,form_end_a)-form_a),output);
            if(size_a>form_end_a)output.insert(output.end(),input.begin()+form_end_a,input.begin()+size_a);
        }
        const auto size_w=static_cast<std::uint16_t>(output.size());output[68]=static_cast<std::uint8_t>(size_w);output[69]=static_cast<std::uint8_t>(size_w>>8); // DEVMODEW 的 dmSize 在 64 位元組裝置名稱之後。
        output.insert(output.end(),input.begin()+size_a,input.end());
        const auto result=allocate_heap(memory,output.size());if(!result)return fail(8);
        memory.write(GuestAddress{result},output);return finish(result);
    }
    return std::nullopt;
}
}
