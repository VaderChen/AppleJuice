#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_userdraw.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> theme_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"uxtheme.dll","IsThemeActive",0},{"uxtheme.dll","IsAppThemed",0},{"uxtheme.dll","IsCompositionActive",0},
        {"uxtheme.dll","OpenThemeData",2},{"uxtheme.dll","OpenThemeDataEx",3},{"uxtheme.dll","OpenThemeDataForDpi",3},
        {"uxtheme.dll","GetWindowTheme",1},{"uxtheme.dll","CloseThemeData",1},{"uxtheme.dll","SetWindowTheme",3},
        {"uxtheme.dll","SetWindowThemeAttribute",4},{"uxtheme.dll","GetThemeAppProperties",0},{"uxtheme.dll","SetThemeAppProperties",1},
        {"uxtheme.dll","EnableTheming",1},{"uxtheme.dll","EnableThemeDialogTexture",2},{"uxtheme.dll","IsThemeDialogTextureEnabled",1},{"uxtheme.dll","GetCurrentThemeName",6},
        {"uxtheme.dll","DrawThemeBackground",6},{"uxtheme.dll","DrawThemeBackgroundEx",6},
        {"uxtheme.dll","DrawThemeText",9},{"uxtheme.dll","DrawThemeTextEx",9},
        {"uxtheme.dll","GetThemeBackgroundContentRect",6},{"uxtheme.dll","GetThemePartSize",7},
        {"uxtheme.dll","IsThemePartDefined",3},{"uxtheme.dll","IsThemeBackgroundPartiallyTransparent",3},
        {"uxtheme.dll","GetThemeColor",5},{"uxtheme.dll","GetThemeSysColor",2},
        {"uxtheme.dll","GetThemeMargins",7},{"uxtheme.dll","GetThemeBackgroundExtent",6},{"uxtheme.dll","GetThemeBackgroundRegion",6},{"uxtheme.dll","GetThemeTextMetrics",5},
        {"uxtheme.dll","GetThemeSysSize",2},{"uxtheme.dll","GetThemeSysColorBrush",2},{"uxtheme.dll","GetThemeSysBool",2},{"uxtheme.dll","GetThemeSysFont",3},{"uxtheme.dll","GetThemeSysInt",3},
        {"uxtheme.dll","GetThemeInt",5},{"uxtheme.dll","GetThemeBool",5},{"uxtheme.dll","GetThemeEnumValue",5},{"uxtheme.dll","GetThemeMetric",6},{"uxtheme.dll","GetThemePosition",5},
        {"uxtheme.dll","GetThemeRect",5},{"uxtheme.dll","GetThemeFont",6},{"uxtheme.dll","GetThemeString",6},{"uxtheme.dll","GetThemeFilename",6},{"uxtheme.dll","GetThemeIntList",5},
        {"uxtheme.dll","GetThemeStream",7},{"uxtheme.dll","GetThemeBitmap",6},{"uxtheme.dll","GetThemePropertyOrigin",5},{"uxtheme.dll","GetThemeTransitionDuration",6},
        {"uxtheme.dll","GetThemeSysString",4},{"uxtheme.dll","GetThemeDocumentationProperty",4},{"uxtheme.dll","GetThemeTextExtent",9},
        {"uxtheme.dll","HitTestThemeBackground",9,0x80},{"uxtheme.dll","DrawThemeEdge",8},{"uxtheme.dll","DrawThemeIcon",7},
        // 僅註冊目前虛擬 Windows 10 19045 ABI 的已知 ordinal；135 是 PreferredAppMode。
        {"uxtheme.dll","OpenNcThemeData",2},{"uxtheme.dll","RefreshImmersiveColorPolicyState",0},
        {"uxtheme.dll","GetIsImmersiveColorUsingHighContrast",1},{"uxtheme.dll","ShouldAppsUseDarkMode",0},
        {"uxtheme.dll","AllowDarkModeForWindow",2},{"uxtheme.dll","SetPreferredAppMode",1},
        {"uxtheme.dll","FlushMenuThemes",0},{"uxtheme.dll","IsDarkModeAllowedForWindow",1},
        {"uxtheme.dll","ShouldSystemUseDarkMode",0},{"uxtheme.dll","IsDarkModeAllowedForApp",0},
        {"user32.dll","SetWindowCompositionAttribute",2}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_theme(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    constexpr std::uint32_t e_handle=0x80070006U,e_pointer=0x80004003U,e_notimpl=0x80004001U,e_prop=0x80070490U;
    std::uint64_t result=0;const auto window=gui_.windows.find(arg(0));
    const bool native=native_gui_ && native_gui_->theme_metrics(u"BUTTON",0,0,96,{}).has_value();
    const bool active=native && gui_.theme_enabled;
    const auto valid_dc=[&](std::uint64_t handle){
        if(!handle)return true; // 主題尺寸查詢允許沒有 HDC。
        const auto owner=handle>=0x10000000?gui_.windows.find(handle-0x10000000):gui_.windows.end();
        if(!gui_.contexts.contains(handle) && !gui_.dc_objects.contains(handle) && owner==gui_.windows.end())return false;
        auto [context,inserted]=gui_.contexts.try_emplace(handle);
        if(inserted && owner!=gui_.windows.end() && !gui_.dc_objects.contains(handle)){context->second.color=owner->second.text_color;context->second.mode=owner->second.background_mode;}
        return true;
    };
    // GetThemeBitmap(GBF_DIRECT) 的點陣圖屬於主題 handle，關閉或批次移除主題時一併刪除。
    const auto release_bitmaps=[&](const GuiThemeData& data){for(const auto& [key,bitmap]:data.bitmaps){(void)key;(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{bitmap});}};
    if(name=="IsThemeActive" || name=="IsAppThemed"){result=active && (name=="IsThemeActive" || (gui_.theme_app_properties&2));last_error_=0;}
    else if(name=="IsCompositionActive"){last_error_=120;}
    else if(name=="ShouldAppsUseDarkMode" || name=="ShouldSystemUseDarkMode" || name=="IsDarkModeAllowedForApp"){}
    else if(name=="GetIsImmersiveColorUsingHighContrast"){if(arg(0)>1)last_error_=87;}
    else if(name=="RefreshImmersiveColorPolicyState" || name=="FlushMenuThemes"){}
    else if(name=="GetThemeAppProperties")result=gui_.theme_app_properties;
    else if(name=="SetThemeAppProperties")gui_.theme_app_properties=static_cast<std::uint32_t>(arg(0));
    else if(name=="SetPreferredAppMode"){
        result=gui_.preferred_theme_mode;
        if(arg(0)>3)last_error_=87;
        else if(arg(0)==2)last_error_=50; // ForceDark 無實際暗色 renderer，不接受偽成功。
        else gui_.preferred_theme_mode=static_cast<std::uint32_t>(arg(0));
    } else if(name=="AllowDarkModeForWindow" || name=="IsDarkModeAllowedForWindow"){
        if(window==gui_.windows.end())last_error_=1400;
        else if(name=="AllowDarkModeForWindow"){
            if(arg(1))last_error_=50; // light-only policy：不能把 opt-in 當已支援 dark mode。
            else {window->second.dark_mode_allowed=false;result=1;}
        }else result=window->second.dark_mode_allowed;
    } else if(name=="OpenThemeData" || name=="OpenThemeDataEx" || name=="OpenThemeDataForDpi" || name=="OpenNcThemeData"){
        if(!arg(1))last_error_=e_pointer;
        else if(arg(0) && window==gui_.windows.end())last_error_=e_handle;
        else {
            auto classes=read_utf16(memory,GuestAddress{arg(1)});
            const auto flags=name=="OpenNcThemeData"?2U:name=="OpenThemeDataEx"?static_cast<std::uint32_t>(arg(2)):0U;
            const auto dpi=name=="OpenThemeDataForDpi"?static_cast<std::uint32_t>(arg(2)):96U;
            if(flags&~3U || !dpi || dpi>768)last_error_=0x80070057U;
            else if(!active || !(gui_.theme_app_properties&(flags&2?1U:2U)))last_error_=e_prop;
            else if(window!=gui_.windows.end() && ((window->second.theme_classes && window->second.theme_classes->empty()) || (window->second.theme_application && window->second.theme_application->empty())))last_error_=e_prop;
            else {
                if(window!=gui_.windows.end() && window->second.theme_classes)classes=*window->second.theme_classes;
                std::u16string selected;
                for(std::size_t offset=0;offset<classes.size();) {
                    const auto end=classes.find(u';',offset);auto candidate=classes.substr(offset,end==classes.npos?classes.size()-offset:end-offset);
                    if(const auto prefix=candidate.rfind(u"::");prefix!=candidate.npos)candidate.erase(0,prefix+2);
                    for(auto& c:candidate)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
                    if(native_gui_->theme_metrics(candidate,0,0,dpi,{})){selected=std::move(candidate);break;}
                    if(end==classes.npos)break;offset=end+1;
                }
                if(selected.empty())last_error_=e_prop;
                else if(gui_.themes.size()>=8192)last_error_=0x8007000eU;
                else {result=gui_.next_object++;gui_.themes.emplace(result,GuiThemeData{std::move(selected),arg(0),dpi,flags});if(arg(0))gui_.window_themes[arg(0)]=result;last_error_=0;}
            }
        }
    } else if(name=="GetWindowTheme") {if(window==gui_.windows.end())last_error_=e_handle;else if(const auto theme=gui_.window_themes.find(arg(0));theme!=gui_.window_themes.end())result=theme->second;}
    else if(name=="SetWindowTheme"){
        if(window==gui_.windows.end())result=e_handle;
        else {
            try {
                auto app=arg(1)?std::optional{read_utf16(memory,GuestAddress{arg(1)})}:std::nullopt;
                auto classes=arg(2)?std::optional{read_utf16(memory,GuestAddress{arg(2)})}:std::nullopt;
                window->second.theme_application=std::move(app);window->second.theme_classes=std::move(classes);
                for(const auto& [handle,data]:gui_.themes){(void)handle;if(data.window==arg(0))release_bitmaps(data);}
                std::erase_if(gui_.themes,[&](const auto& item){return item.second.window==arg(0);});gui_.window_themes.erase(arg(0));
                // 使用既有同步 SendMessage 路由，跨 thread 時必須在接收者 TLS/stack 執行。
                start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{arg(0),0x31a,0,0}}},Continuation::Return,0);
                return std::nullopt;
            }catch(const std::bad_alloc&){result=0x8007000eU;}
        }
    }else if(name=="SetWindowThemeAttribute")result=window==gui_.windows.end()?e_handle:e_notimpl;
    else if(name=="SetWindowCompositionAttribute"){
        if(window==gui_.windows.end())last_error_=1400;
        else if(!arg(1))last_error_=87;
        else {memory.check(GuestAddress{arg(1)},24,Access::Read);last_error_=50;}
    }else if(name=="EnableThemeDialogTexture") {
        // ETDT_DISABLE 1、ETDT_ENABLE 2、ETDT_USETABTEXTURE 4、ETDT_USEAEROWIZARDTABTEXTURE 8；停用與啟用不可同時指定。
        const auto flags=static_cast<std::uint32_t>(arg(1));
        if(!gui_.windows.contains(arg(0)))result=0x80070006U; // E_HANDLE
        else if((flags&~0xfU) || (flags&3U)==3U)result=0x80070057U; // E_INVALIDARG
        else {gui_.dialog_textures[arg(0)]=flags;result=0;}
    }else if(name=="IsThemeDialogTextureEnabled") {
        const auto found=gui_.dialog_textures.find(arg(0));result=found!=gui_.dialog_textures.end() && (found->second&2U);
    }else if(name=="EnableTheming") {
        if(arg(0) && !native)result=e_notimpl;
        else if(gui_.theme_enabled!=(arg(0)!=0)){
            for(const auto& [handle,data]:gui_.themes){(void)handle;release_bitmaps(data);}
            gui_.theme_enabled=arg(0)!=0;gui_.themes.clear();gui_.window_themes.clear();std::vector<GuestCallback> callbacks;
            for(const auto& [handle,model]:gui_.windows){(void)model;callbacks.push_back({resolve("user32.dll","SendMessageW").value,{handle,0x31a,0,0}});}
            start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
        }
    }else if(name=="GetCurrentThemeName") {
        if(!active)result=e_prop;
        else {
            const std::array<std::u16string_view,3> names{u"AppKit/NSAppearanceNameAqua",u"NormalColor",u"NormalSize"};
            for(unsigned i=0;i<3;++i)if(arg(i*2)){if(arg(i*2+1)<=names[i].size())result=0x8007007aU;else memory.check(GuestAddress{arg(i*2)},(names[i].size()+1)*2,Access::Write);}
            if(!result)for(unsigned i=0;i<3;++i)if(arg(i*2))memory.write(GuestAddress{arg(i*2)},utf16_bytes(names[i]));
        }
    }else if(name=="GetThemeSysColor"){
        // 主題系統色與目前系統色同源；hTheme 可為 NULL。
        result=gui_system_color(static_cast<unsigned>(arg(1)));
    }else if(name=="GetThemeColor"){
        // TMT_TEXTCOLOR 取原生控制項文字色；TMT_FILLCOLOR／TMT_FILLCOLORHINT 取原生背景色；主題未定義的屬性回報 E_PROP_ID_UNSUPPORTED。
        const auto theme=gui_.themes.find(arg(0));
        if(theme==gui_.themes.end())result=e_handle;
        else if(!arg(4))result=e_pointer;
        else {
            const auto info=native_gui_?native_gui_->theme_metrics(theme->second.class_name,std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1))),std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2))),theme->second.dpi,{}):std::nullopt;
            const auto property=static_cast<std::uint32_t>(arg(3));
            if(!info || (property!=3803 && property!=3802 && property!=3821))result=e_prop;
            else {memory.store(GuestAddress{arg(4)},32,(property==3803?info->text_color:info->background_color)&0xffffffU);}
        }
    }else if(name=="GetThemeSysSize"){
        // 只支援 MSDN 列出的系統尺寸；有主題 handle 時依主題的 DPI 換算。
        constexpr std::array<std::uint32_t,11> supported{5,2,3,21,30,31,52,53,54,55,92};const auto id=static_cast<std::uint32_t>(arg(1));
        const auto theme=gui_.themes.find(arg(0));const auto dpi=theme!=gui_.themes.end()?theme->second.dpi:96U;
        result=std::find(supported.begin(),supported.end(),id)!=supported.end()?system_metric(id,dpi):0U;
    }else if(name=="GetThemeSysColorBrush"){
        // 主題系統色刷與 GetSysColorBrush 同源。
        result=nested_service(state,memory,"user32.dll","GetSysColorBrush",{static_cast<std::uint32_t>(arg(1))});
    }else if(name=="GetThemeSysBool"){
        // TMT_FLATMENUS（1001）取 SPI_GETFLATMENU；其他識別碼為 STG_E_INVALIDPARAMETER。
        last_error_=0;
        if(static_cast<std::uint32_t>(arg(1))==1001){GuestScratch scratch(*this,memory,state);const auto value=scratch.allocate(4);if(!value)last_error_=8;
            else{memory.store(GuestAddress{value},32,0);if(nested_service(state,memory,"user32.dll","SystemParametersInfoW",{0x1022,0,value,0})&UINT32_MAX)result=memory.load(GuestAddress{value},32)!=0;last_error_=0;}}
        else last_error_=0x80030057U;
    }else if(name=="GetThemeSysFont"){
        // TMT_CAPTIONFONT～TMT_ICONTITLEFONT（801～806）：NONCLIENTMETRICSW 的各字型與 SPI_GETICONTITLELOGFONT。
        const auto id=static_cast<std::uint32_t>(arg(1));
        if(!arg(2))result=e_pointer;else if(id<801 || id>806)result=0x80070057U;
        else{GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(504);if(!buffer)result=0x8007000eU;
            else if(id==806){if(!(nested_service(state,memory,"user32.dll","SystemParametersInfoW",{0x1f,92,buffer,0})&UINT32_MAX))result=0x80004005U;}
            else{memory.write(GuestAddress{buffer},std::vector<std::uint8_t>(504));memory.store(GuestAddress{buffer},32,504);if(!(nested_service(state,memory,"user32.dll","SystemParametersInfoW",{0x29,504,buffer,0})&UINT32_MAX))result=0x80004005U;}
            if(!result){constexpr std::array<unsigned,5> offsets{24,124,224,316,408};const auto at=id==806?buffer:buffer+offsets[id-801];
                std::vector<std::uint8_t> font(92);memory.read(GuestAddress{at},font);memory.check(GuestAddress{arg(2)},92,Access::Write);memory.write(GuestAddress{arg(2)},font);}}
        last_error_=0;
    }else if(name=="GetThemeSysInt"){
        // TMT_MINCOLORDEPTH（1301）：Windows 主題的系統度量為 15 位元；其他識別碼為 E_PROP_ID_UNSUPPORTED。
        if(!gui_.themes.contains(arg(0)))result=e_handle;else if(!arg(2))result=e_pointer;
        else if(static_cast<std::uint32_t>(arg(1))!=1301)result=e_prop;else memory.store(GuestAddress{arg(2)},32,15);
    }else if(name=="GetThemeMargins" || name=="GetThemeBackgroundExtent" || name=="GetThemeBackgroundRegion" || name=="GetThemeTextMetrics"){
        // 由原生控制項的內容區計算邊界：content 是在指定尺寸下內容區的左、上、右、下座標。
        const auto theme=gui_.themes.find(arg(0));
        if(theme==gui_.themes.end())result=e_handle;
        else if(!valid_dc(arg(1)))result=e_handle;
        else{
            const auto& data=theme->second;const auto part=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2))),status=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(3)));
            const auto read_rect=[&](std::uint64_t at){std::array<std::int32_t,4> rect{};memory.check(GuestAddress{at},16,Access::Read);for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4},32)));return rect;};
            const auto margins=[&](std::array<std::int32_t,2> size)->std::optional<std::array<std::int32_t,4>>{
                const auto info=native_gui_?native_gui_->theme_metrics(data.class_name,part,status,data.dpi,size):std::nullopt;if(!info)return std::nullopt;
                if(!size[0] && !size[1])size=info->intrinsic;const auto measured=native_gui_->theme_metrics(data.class_name,part,status,data.dpi,size);if(!measured)return std::nullopt;
                return std::array<std::int32_t,4>{std::max(0,measured->content[0]),std::max(0,size[0]-measured->content[2]),std::max(0,measured->content[1]),std::max(0,size[1]-measured->content[3])};
            };
            if(name=="GetThemeMargins"){
                // (hTheme, hdc, iPartId, iStateId, iPropId, prc, pMargins)：TMT_SIZINGMARGINS（3601）與 TMT_CONTENTMARGINS（3602）；MARGINS 為左、右、上、下。
                const auto property=static_cast<std::uint32_t>(arg(4));
                if(!arg(6))result=e_pointer;else if(property!=3601 && property!=3602)result=e_prop;
                else{std::array<std::int32_t,2> size{};if(arg(5)){const auto rect=read_rect(arg(5));size={rect[2]-rect[0],rect[3]-rect[1]};if(size[0]<0 || size[1]<0)result=0x80070057U;}
                    if(!result){const auto value=margins(size);if(!value)result=e_prop;else{memory.check(GuestAddress{arg(6)},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(6)+i*4},32,static_cast<std::uint32_t>((*value)[i]));}}}
            }else if(name=="GetThemeBackgroundExtent"){
                // (hTheme, hdc, iPartId, iStateId, pContentRect, pExtentRect)：內容區向外加上邊界。
                if(!arg(4) || !arg(5))result=e_pointer;
                else{const auto content=read_rect(arg(4));const auto value=margins({std::max(0,content[2]-content[0]),std::max(0,content[3]-content[1])});
                    if(!value)result=e_prop;else{const std::array<std::int32_t,4> extent{content[0]-(*value)[0],content[1]-(*value)[2],content[2]+(*value)[1],content[3]+(*value)[3]};
                        memory.check(GuestAddress{arg(5)},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(5)+i*4},32,static_cast<std::uint32_t>(extent[i]));}}
            }else if(name=="GetThemeBackgroundRegion"){
                // (hTheme, hdc, iPartId, iStateId, pRect, pRegion)：原生控制項背景為不透明矩形。
                if(!arg(4) || !arg(5))result=e_pointer;
                else{const auto rect=read_rect(arg(4));memory.check(GuestAddress{arg(5)},x86_?4:8,Access::Write);
                    const auto region=nested_service(state,memory,"gdi32.dll","CreateRectRgn",{static_cast<std::uint32_t>(rect[0]),static_cast<std::uint32_t>(rect[1]),static_cast<std::uint32_t>(rect[2]),static_cast<std::uint32_t>(rect[3])});
                    if(!region)result=0x8007000eU;else memory.store(GuestAddress{arg(5)},x86_?32:64,region);}
            }else{
                // (hTheme, hdc, iPartId, iStateId, ptm)：主題未定義 TMT_FONT，取 DC 目前字型的 TEXTMETRICW。
                if(!arg(4))result=e_pointer;else if(!arg(1))result=e_handle;
                else if(!(nested_service(state,memory,"gdi32.dll","GetTextMetricsW",{arg(1),arg(4)})&UINT32_MAX))result=0x80004005U;
            }
        }
    }else if(name=="GetThemeInt" || name=="GetThemeBool" || name=="GetThemeEnumValue" || name=="GetThemeMetric" || name=="GetThemePosition" || name=="GetThemeRect" || name=="GetThemeFont"
             || name=="GetThemeString" || name=="GetThemeFilename" || name=="GetThemeIntList" || name=="GetThemeStream" || name=="GetThemeBitmap" || name=="GetThemePropertyOrigin"
             || name=="GetThemeTransitionDuration"){
        // 主題屬性模型：只含原生控制項度量推導出的屬性，型別為 vssym32 的 TMT_* 原始型別（ENUM 200、STRING 201、INT 202、BOOL 203、COLOR 204、MARGINS 205、
        // FILENAME 206、POSITION 208、RECT 209、FONT 210、INTLIST 211、HBITMAP 212、DISKSTREAM 213、STREAM 214），來源依 PROPERTYORIGIN（狀態 0、零件 1、類別 2）。
        // 各取值函式只接受相同原始型別的屬性；主題沒有定義的屬性回 E_PROP_ID_UNSUPPORTED，與主題檔缺少該屬性時相同。
        struct Property {std::uint32_t type{},origin{};std::int32_t number{};std::array<std::int32_t,2> pair{};std::vector<std::int32_t> list{};};
        const auto signed_arg=[&](unsigned i){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(i)));};
        const auto theme=gui_.themes.find(arg(0));
        const auto find=[&](std::int32_t part,std::int32_t status,std::uint32_t id)->std::optional<Property>{
            const auto& data=theme->second;
            const auto info=native_gui_?native_gui_->theme_metrics(data.class_name,part,status,data.dpi,{}):std::nullopt;if(!info)return std::nullopt;
            switch(id){
                case 3802:case 3821:return Property{204,0,static_cast<std::int32_t>(info->background_color&0xffffffU),{},{}}; // TMT_FILLCOLOR／TMT_FILLCOLORHINT
                case 3803:return Property{204,0,static_cast<std::int32_t>(info->text_color&0xffffffU),{},{}};                  // TMT_TEXTCOLOR
                case 2201:return Property{203,0,info->transparent?1:0,{},{}};                                                   // TMT_TRANSPARENT
                case 3403:return Property{208,1,0,info->minimum,{}};                                                             // TMT_MINSIZE
                case 3409:return Property{208,1,0,info->intrinsic,{}};                                                           // TMT_NORMALSIZE
                case 4001:return Property{200,2,0,{},{}};                                                                         // TMT_BGTYPE＝BT_IMAGEFILE（背景為影像）
                case 4004:return Property{200,1,info->fixed_size?0:1,{},{}};                                                     // TMT_SIZINGTYPE：ST_TRUESIZE／ST_STRETCH
                case 3601:case 3602:{                                                                                             // TMT_SIZINGMARGINS／TMT_CONTENTMARGINS（原始尺寸下的左、上）
                    const auto measured=native_gui_->theme_metrics(data.class_name,part,status,data.dpi,info->intrinsic);if(!measured)return std::nullopt;
                    return Property{205,1,std::max(0,measured->content[0]),{std::max(0,measured->content[1]),0},{}};}
                case 2:return Property{212,0,0,info->intrinsic,{}};                                                              // TMT_DIBDATA：背景影像
                default:return std::nullopt;
            }
        };
        if(name=="GetThemeTransitionDuration"){
            // (hTheme, iPartId, iStateIdFrom, iStateIdTo, iPropId, pdwDuration)：TMT_TRANSITIONDURATIONS 為零件層 INTLIST，第一個值 N 為狀態數，其後是 N×N 的毫秒矩陣；
            // 屬性不存在時輸出 0。
            const auto from=signed_arg(2),to=signed_arg(3);
            if(!arg(5) || from<1 || to<1)result=0x80070057U;
            else if(theme==gui_.themes.end())result=e_handle;
            else{
                memory.check(GuestAddress{arg(5)},4,Access::Write);const auto found=find(signed_arg(1),0,static_cast<std::uint32_t>(arg(4)));std::uint32_t duration=0;
                if(!found || found->type!=211)result=e_prop;
                else{const auto& list=found->list;const auto states=list.empty()?0:list[0];
                    if(states<1 || from>states || to>states || list.size()!=1+static_cast<std::size_t>(states)*static_cast<std::size_t>(states))result=0x80070057U;
                    else duration=static_cast<std::uint32_t>(list[1+static_cast<std::size_t>(states)*static_cast<std::size_t>(from-1)+static_cast<std::size_t>(to-1)]);}
                memory.store(GuestAddress{arg(5)},32,duration);
            }
        }else if(theme==gui_.themes.end())result=e_handle;
        else{
            const bool with_dc=name=="GetThemeMetric" || name=="GetThemeFont";const unsigned first=with_dc?2U:1U;
            const auto part=signed_arg(first),status=signed_arg(first+1);auto id=static_cast<std::uint32_t>(arg(first+2));
            const auto output=arg(name=="GetThemeBitmap"?5U:first+3);
            if(with_dc && !valid_dc(arg(1)))result=e_handle;
            else if(!output || (name=="GetThemeStream" && !arg(5)))result=e_pointer;
            else if((name=="GetThemeString" || name=="GetThemeFilename") && signed_arg(5)<=0)result=0x80070057U;
            else if(name=="GetThemePropertyOrigin"){
                const auto found=find(part,status,id);memory.check(GuestAddress{output},4,Access::Write);memory.store(GuestAddress{output},32,found?found->origin:4U); // PO_NOTFOUND
            }else if(name=="GetThemeBitmap"){
                // (hTheme, iPartId, iStateId, iPropId, dwFlags, phBitmap)：iPropId 0 取第一個可用影像，TMT_DIBDATA／TMT_HBITMAP 為背景影像，TMT_GLYPHDIBDATA 為字形影像（此主題沒有）；
                // GBF_DIRECT 回主題擁有的點陣圖（CloseThemeData 時釋放），GBF_COPY 回呼叫者負責 DeleteObject 的複本。
                const auto flags=static_cast<std::uint32_t>(arg(4));if(id==0 || id==212)id=2;
                const auto found=find(part,status,id);auto& data=theme->second;const std::array<std::int32_t,3> key{part,status,static_cast<std::int32_t>(id)};
                if(flags!=1 && flags!=2)result=0x80070057U;
                else if(!found || found->type!=212)result=e_prop;
                else if(const auto cached=data.bitmaps.find(key);flags==1 && cached!=data.bitmaps.end()){memory.check(GuestAddress{output},x86_?4:8,Access::Write);memory.store(GuestAddress{output},x86_?32:64,cached->second);}
                else{
                    const auto image=native_gui_->theme_background(data.class_name,part,status,data.dpi,found->pair);
                    GuestScratch scratch(*this,memory,state);const auto info=image?scratch.allocate(48):0;
                    if(!image)result=0x80004005U;else if(!info)result=0x8007000eU;
                    else{
                        memory.write(GuestAddress{info},std::vector<std::uint8_t>(48));memory.store(GuestAddress{info},32,40);memory.store(GuestAddress{info+4},32,image->width);
                        memory.store(GuestAddress{info+8},32,static_cast<std::uint32_t>(-static_cast<std::int32_t>(image->height)));memory.store(GuestAddress{info+12},16,1);memory.store(GuestAddress{info+14},16,32);
                        const auto bitmap=nested_service(state,memory,"gdi32.dll","CreateDIBSection",{0,info,0,info+40,0,0});const auto bits=memory.load(GuestAddress{info+40},x86_?32:64);
                        if(!bitmap || !bits){if(bitmap)(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{bitmap});result=0x8007000eU;}
                        else{
                            // 原生像素為預乘的 R、G、B、A；32 位元 DIB 的位元組順序為 B、G、R、A。
                            std::vector<std::uint8_t> bytes(image->pixels.size()*4);
                            for(std::size_t i=0;i<image->pixels.size();++i){const auto pixel=image->pixels[i];bytes[i*4]=static_cast<std::uint8_t>(pixel>>16);bytes[i*4+1]=static_cast<std::uint8_t>(pixel>>8);bytes[i*4+2]=static_cast<std::uint8_t>(pixel);bytes[i*4+3]=static_cast<std::uint8_t>(pixel>>24);}
                            memory.write(GuestAddress{bits},bytes);import_dib_bitmap(memory,bitmap);rasters_.erase(bitmap);
                            if(flags==1)data.bitmaps[key]=bitmap;
                            memory.check(GuestAddress{output},x86_?4:8,Access::Write);memory.store(GuestAddress{output},x86_?32:64,bitmap);
                        }
                    }
                }
            }else{
                // 其餘取值函式只接受同一原始型別；GetThemeMetric 另接受列舉、布林、位置（x）、邊界（左）與整數清單（第一個值）。
                // 此主題的屬性模型沒有字串、檔名、矩形、字型與串流屬性，這些取值函式在參數驗證後回 E_PROP_ID_UNSUPPORTED。
                const auto found=find(part,status,id);
                const auto wanted=name=="GetThemeInt"?202U:name=="GetThemeBool"?203U:name=="GetThemeEnumValue"?200U:name=="GetThemePosition"?208U:name=="GetThemeIntList"?211U:0U;
                const bool accepted=found && (name=="GetThemeMetric"?(found->type==202 || found->type==200 || found->type==203 || found->type==208 || found->type==205 || found->type==211):wanted && found->type==wanted);
                if(!accepted)result=e_prop;
                else if(name=="GetThemePosition"){memory.check(GuestAddress{output},8,Access::Write);for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+i*4},32,static_cast<std::uint32_t>(found->pair[i]));}
                else if(name=="GetThemeIntList"){
                    // INTLIST：iValueCount 後接最多 MAX_INTLIST_COUNT（402）個整數。
                    const auto count=std::min<std::size_t>(found->list.size(),402);memory.check(GuestAddress{output},4+count*4,Access::Write);memory.store(GuestAddress{output},32,count);
                    for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{output+4+i*4},32,static_cast<std::uint32_t>(found->list[i]));
                }else{
                    auto value=found->number;if(found->type==208)value=found->pair[0];else if(found->type==211)value=found->list.empty()?0:found->list[0];
                    memory.check(GuestAddress{output},4,Access::Write);memory.store(GuestAddress{output},32,static_cast<std::uint32_t>(value));
                }
            }
        }
    }else if(name=="GetThemeSysString"){
        // (hTheme, iStringId, pszStringBuff, cchMaxStringChars)：TMT_CSSNAME～TMT_ALIAS（1401～1404）為主題的系統字串；此主題沒有定義這些字串。
        const auto id=static_cast<std::uint32_t>(arg(1));
        if(!gui_.themes.contains(arg(0)))result=e_handle;else if(!arg(2))result=e_pointer;else if(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(3)))<=0)result=0x80070057U;
        else{(void)id;result=e_prop;}
    }else if(name=="GetThemeDocumentationProperty"){
        // (pszThemeName, pszPropertyName, pszValueBuff, cchMaxValChars)：主題檔名與 GetCurrentThemeName 相同；[documentation] 只定義 DisplayName、ThemeName 與 Description。
        if(!arg(0) || !arg(1) || !arg(2))result=e_pointer;
        else if(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(3)))<=0)result=0x80070057U;
        else{
            const auto fold=[](std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;};
            const auto file=fold(read_utf16(memory,GuestAddress{arg(0)})),property=fold(read_utf16(memory,GuestAddress{arg(1)}));
            if(!active || file!=fold(u"AppKit/NSAppearanceNameAqua"))result=0x80070002U; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
            else{
                std::u16string_view value;
                if(property==u"displayname" || property==u"themename")value=u"Aqua";else if(property==u"description")value=u"macOS AppKit Aqua (light) appearance";
                if(value.empty())result=e_prop;
                else{const auto capacity=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(3)));const auto count=std::min<std::size_t>(value.size(),static_cast<std::size_t>(capacity-1));
                    memory.check(GuestAddress{arg(2)},(count+1)*2,Access::Write);for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(2)+i*2},16,value[i]);memory.store(GuestAddress{arg(2)+count*2},16,0);}
            }
        }
    }else if(name=="GetThemeTextExtent"){
        // (hTheme, hdc, iPartId, iStateId, pszText, cchCharCount, dwTextFlags, pBoundingRect, pExtentRect)：主題沒有 TMT_FONT，以 DC 目前字型計算 DrawTextW(DT_CALCRECT)。
        const auto count=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(5)));
        if(!gui_.themes.contains(arg(0)) || !arg(1) || !valid_dc(arg(1)))result=e_handle;
        else if((!arg(4) && count) || !arg(8))result=e_pointer;
        else if(count<-1)result=0x80070057U;
        else{
            GuestScratch scratch(*this,memory,state);const auto rect=scratch.allocate(16);
            if(!rect)result=0x8007000eU;
            else{
                std::vector<std::uint8_t> box(16);if(arg(7)){memory.check(GuestAddress{arg(7)},16,Access::Read);memory.read(GuestAddress{arg(7)},box);}memory.write(GuestAddress{rect},box);
                const auto height=nested_service(state,memory,"user32.dll","DrawTextW",{arg(1),arg(4),static_cast<std::uint32_t>(count),rect,static_cast<std::uint32_t>(arg(6))|0x400U});
                if(!(height&UINT32_MAX) && count)result=0x80004005U;
                else{memory.read(GuestAddress{rect},box);memory.check(GuestAddress{arg(8)},16,Access::Write);memory.write(GuestAddress{arg(8)},box);}
            }
        }
    }else if(name=="HitTestThemeBackground"){
        // (hTheme, hdc, iPartId, iStateId, dwOptions, pRect, hrgn, ptTest, pwHitTestCode)：HTTB_FIXEDBORDER 2、HTTB_CAPTION 4、HTTB_RESIZINGBORDER_LEFT／TOP／RIGHT／BOTTOM 0x10～0x80、
        // HTTB_SIZINGTEMPLATE 0x100（此主題沒有 TMT_SIZINGTEMPLATE，改用尺寸邊界）、HTTB_SYSTEMSIZINGMARGINS 0x200（系統調整框寬度）。
        const auto theme=gui_.themes.find(arg(0));const auto options=static_cast<std::uint32_t>(arg(4));
        if(theme==gui_.themes.end() || !valid_dc(arg(1)))result=e_handle;
        else if(!arg(5) || !arg(8))result=e_pointer;
        else if(options&~0x3f6U)result=0x80070057U;
        else{
            const auto& data=theme->second;const auto part=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2))),status=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(3)));
            std::array<std::int32_t,4> rect{};memory.check(GuestAddress{arg(5)},16,Access::Read);for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)+i*4},32)));
            const auto x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(7))),y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(7)>>32));
            const std::array<std::int32_t,2> size{std::max(0,rect[2]-rect[0]),std::max(0,rect[3]-rect[1])};
            const auto info=native_gui_?native_gui_->theme_metrics(data.class_name,part,status,data.dpi,size):std::nullopt;
            if(!info)result=e_prop;
            else{
                const bool inside=arg(6)?(nested_service(state,memory,"gdi32.dll","PtInRegion",{arg(6),static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y)})&UINT32_MAX)!=0:x>=rect[0] && x<rect[2] && y>=rect[1] && y<rect[3];
                std::uint32_t code=0; // HTNOWHERE
                if(inside){
                    std::array<std::int32_t,4> margin{}; // 左、右、上、下
                    if(options&0x200U){const auto cx=static_cast<std::int32_t>(system_metric(32,data.dpi)+system_metric(92,data.dpi)),cy=static_cast<std::int32_t>(system_metric(33,data.dpi)+system_metric(92,data.dpi));margin={cx,cx,cy,cy};}
                    else margin={std::max(0,info->content[0]),std::max(0,size[0]-info->content[2]),std::max(0,info->content[1]),std::max(0,size[1]-info->content[3])};
                    const bool left=x<rect[0]+margin[0],right=x>=rect[2]-margin[1],top=y<rect[1]+margin[2],bottom=y>=rect[3]-margin[3];
                    const bool l=options&0x10U,t=options&0x20U,r=options&0x40U,b=options&0x80U;
                    if(top && left && t && l)code=13;else if(top && right && t && r)code=14;else if(bottom && left && b && l)code=16;else if(bottom && right && b && r)code=17;
                    else if(left && l)code=10;else if(right && r)code=11;else if(top && t)code=12;else if(bottom && b)code=15;
                    else if(top && (options&4U))code=2;                               // HTCAPTION
                    else if((left || right || top || bottom) && (options&2U))code=18;  // HTBORDER
                    else code=1;                                                       // HTCLIENT
                }
                memory.check(GuestAddress{arg(8)},2,Access::Write);memory.store(GuestAddress{arg(8)},16,code);
            }
        }
    }else if(name=="DrawThemeEdge"){
        // (hTheme, hdc, iPartId, iStateId, pDestRect, uEdge, uFlags, pContentRect)：主題沒有 TMT_EDGE*COLOR，與 DrawEdge 相同使用系統色；BF_ADJUST 時回傳內部矩形。
        if(!gui_.themes.contains(arg(0)) || !arg(1) || !valid_dc(arg(1)))result=e_handle;
        else if(!arg(4))result=e_pointer;
        else{
            GuestScratch scratch(*this,memory,state);const auto rect=scratch.allocate(16);
            if(!rect)result=0x8007000eU;
            else{std::vector<std::uint8_t> box(16);memory.check(GuestAddress{arg(4)},16,Access::Read);memory.read(GuestAddress{arg(4)},box);memory.write(GuestAddress{rect},box);
                if(!(nested_service(state,memory,"user32.dll","DrawEdge",{arg(1),rect,static_cast<std::uint32_t>(arg(5)),static_cast<std::uint32_t>(arg(6))})&UINT32_MAX))result=0x80004005U;
                else if(arg(7) && (arg(6)&0x2000U)){memory.read(GuestAddress{rect},box);memory.check(GuestAddress{arg(7)},16,Access::Write);memory.write(GuestAddress{arg(7)},box);}}
        }
    }else if(name=="DrawThemeIcon"){
        // (hTheme, hdc, iPartId, iStateId, pRect, himl, iImageIndex)：主題沒有 TMT_ICONEFFECT（ICE_NONE），在矩形左上角以 ILD_TRANSPARENT 繪製，寬高為矩形大小。
        if(!gui_.themes.contains(arg(0)) || !arg(1) || !valid_dc(arg(1)))result=e_handle;
        else if(!arg(4))result=e_pointer;
        else{
            std::array<std::int32_t,4> rect{};memory.check(GuestAddress{arg(4)},16,Access::Read);for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*4},32)));
            const auto width=std::max(0,rect[2]-rect[0]),height=std::max(0,rect[3]-rect[1]);
            if(!(nested_service(state,memory,"comctl32.dll","ImageList_DrawEx",{arg(5),static_cast<std::uint32_t>(arg(6)),arg(1),static_cast<std::uint32_t>(rect[0]),static_cast<std::uint32_t>(rect[1]),
                static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),0xffffffffU,0xffffffffU,1})&UINT32_MAX))result=0x80004005U;
        }
    }else if(name=="CloseThemeData") {
        if(const auto found=gui_.themes.find(arg(0));found!=gui_.themes.end())release_bitmaps(found->second);
        if(!gui_.themes.erase(arg(0)))result=e_handle;
        else std::erase_if(gui_.window_themes,[&](const auto& item){return item.second==arg(0);});
    }else if(name.starts_with("DrawTheme") || name.starts_with("GetThemeBackground") || name=="GetThemePartSize" || name=="IsThemePartDefined" || name=="IsThemeBackgroundPartiallyTransparent") {
        const bool boolean=name.starts_with("IsTheme");const auto theme=gui_.themes.find(arg(0));
        if(theme==gui_.themes.end()){if(boolean)last_error_=e_handle;else result=e_handle;}
        else {
            const auto& data=theme->second;const unsigned part_index=boolean?1U:2U;
            const auto part=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(part_index))),status=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(part_index+1)));
            const auto info=native_gui_->theme_metrics(data.class_name,part,status,data.dpi,{});
            if(!info){if(boolean)last_error_=e_prop;else result=e_prop;}
            else if(boolean)result=name=="IsThemePartDefined"?1:info->transparent;
            else if(!valid_dc(arg(1)))result=e_handle;
            else if(name=="GetThemePartSize" || name=="GetThemeBackgroundContentRect") {
                const auto output=arg(name=="GetThemePartSize"?6:5);
                if(!output || (name=="GetThemeBackgroundContentRect" && !arg(4)))result=e_pointer;
                else if(name=="GetThemePartSize" && arg(5)>2)result=0x80070057U;
                else {
                    std::array<std::int32_t,4> rect{};if(arg(4)){memory.check(GuestAddress{arg(4)},16,Access::Read);for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*4},32)));}
                    const auto width=static_cast<std::int64_t>(rect[2])-rect[0],height=static_cast<std::int64_t>(rect[3])-rect[1];
                    if(arg(4) && (width<0 || height<0 || width>8192 || height>8192))result=0x80070057U;
                    else if(name=="GetThemePartSize") {
                        auto size=arg(5)==0?info->minimum:info->intrinsic;
                        if(arg(5)==2 && arg(4) && (!info->fixed_size || (data.flags&1)))size={static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)};
                        memory.check(GuestAddress{output},8,Access::Write);for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+i*4},32,static_cast<std::uint32_t>(size[i]));
                    } else {
                        const auto measured=native_gui_->theme_metrics(data.class_name,part,status,data.dpi,{static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)});
                        memory.check(GuestAddress{output},16,Access::Write);for(unsigned i=0;i<4;++i){const auto limit=i%2?height:width;const auto value=static_cast<std::int64_t>(rect[i%2])+std::clamp<std::int64_t>(measured->content[i],0,limit);memory.store(GuestAddress{output+i*4},32,static_cast<std::uint32_t>(value));}
                    }
                }
            }else if(name=="DrawThemeText" || name=="DrawThemeTextEx") {
                const auto context=gui_.contexts.find(arg(1));const bool ex=name=="DrawThemeTextEx";const auto rect=arg(ex?7:8);const auto count=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(5)));
                if(context==gui_.contexts.end())result=e_handle;
                else if(!rect || (!arg(4) && count))result=e_pointer;
                else if(count < -1 || count>65536 || (arg(6)&~0x3fffffU))result=0x80070057U;
                else {
                    memory.check(GuestAddress{rect},16,Access::Read);std::uint32_t color=info->text_color,flags=static_cast<std::uint32_t>(arg(6));
                    if(!ex && arg(7)&~1ULL)result=0x80070057U;
                    else if(!ex && (arg(7)&1))color=0x808080;
                    else if(ex && arg(8)) {
                        const auto p=arg(8);const unsigned size=x86_?64:72;memory.check(GuestAddress{p},size,Access::Read);const auto options=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                        if(memory.load(GuestAddress{p},32)!=size || (options&~0x3fffU))result=0x80070057U;
                        else if(options&~(1U|0x400U|0x2000U))result=e_notimpl;
                        else {if(options&1)color=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8},32));if(options&0x400)flags|=0x400;}
                        if(!result && (options&0x2000U)){const auto bitmap=gui_.bitmaps.find(context->second.bitmap);if(bitmap==gui_.bitmaps.end() || !bitmap->second.dib_address || bitmap->second.bits_per_pixel!=32 || bitmap->second.signed_height>=0)result=0x80070057U;}
                    }
                    if(!result){const auto token=gui_.next_object++;GuiThemeText operation;operation.dc=arg(1);operation.saved=context->second;operation.allow_zero=count==0 || (flags&0x400);
                        const auto owner=arg(1)>=0x10000000 && !gui_.dc_objects.contains(arg(1))?gui_.windows.find(arg(1)-0x10000000):gui_.windows.end();
                        if(owner!=gui_.windows.end())operation.window_text=std::array{owner->second.text_color,owner->second.background_mode};
                        gui_.theme_text_operations.emplace(token,operation);context->second.color=color;context->second.mode=1;
                        if(owner!=gui_.windows.end()){owner->second.text_color=color;owner->second.background_mode=1;}
                        try{start_callbacks(state,memory,{{resolve("user32.dll","DrawTextW").value,{arg(1),arg(4),arg(5),rect,flags}}},Continuation::ThemeTextOperation,token);}catch(...){discard_theme_text(token);throw;}
                        return std::nullopt;
                    }
                }
            }else {
                if(!arg(4))result=e_pointer;
                else if(!arg(1))result=e_handle;
                else {
                    std::array<std::int32_t,4> rect{};memory.check(GuestAddress{arg(4)},16,Access::Read);for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*4},32)));
                    std::uint32_t flags=0;std::optional<std::array<std::int32_t,4>> clip;
                    if(arg(5)) {
                        auto p=arg(5);if(name=="DrawThemeBackgroundEx"){memory.check(GuestAddress{p},24,Access::Read);flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));if(memory.load(GuestAddress{p},32)!=24 || (flags&~0x7fU))result=0x80070057U;else if(flags&0x12U)result=e_notimpl;p+=8;}
                        if(name=="DrawThemeBackground" || (flags&1)){std::array<std::int32_t,4> box{};memory.check(GuestAddress{p},16,Access::Read);for(unsigned i=0;i<4;++i)box[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{p+i*4},32)));clip=box;}
                    }
                    const auto width=static_cast<std::int64_t>(rect[2])-rect[0],height=static_cast<std::int64_t>(rect[3])-rect[1];
                    if(width<0 || height<0 || width>4096 || height>4096 || width*height>4194304)result=0x80070057U;
                    if(!result && width && height) {
                        auto bitmap=native_gui_->theme_background(data.class_name,part,status,data.dpi,{static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)});
                        auto surface=raster_surface(memory,arg(1));
                        if(!bitmap || !surface)result=e_notimpl;
                        else {
                            if(flags&12){const auto measured=native_gui_->theme_metrics(data.class_name,part,status,data.dpi,{static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)});
                                for(std::int32_t y=0;y<height;++y)for(std::int32_t x=0;x<width;++x){const bool inside=x>=measured->content[0] && y>=measured->content[1] && x<measured->content[2] && y<measured->content[3];if((inside && (flags&8)) || (!inside && (flags&4)))bitmap->pixels[static_cast<std::size_t>(y)*bitmap->width+static_cast<std::size_t>(x)]=0;}}
                            if((flags&0x20) && !(flags&0x40))for(unsigned y=0;y<bitmap->height;++y){auto first=bitmap->pixels.begin()+static_cast<std::ptrdiff_t>(y*bitmap->width);std::reverse(first,first+bitmap->width);}
                            auto dc=gui_.contexts.at(arg(1));if(clip){if(dc.clipped){(*clip)[0]=std::max((*clip)[0],dc.clip[0]);(*clip)[1]=std::max((*clip)[1],dc.clip[1]);(*clip)[2]=std::min((*clip)[2],dc.clip[2]);(*clip)[3]=std::min((*clip)[3],dc.clip[3]);}dc.clipped=true;dc.clip=*clip;}
                            auto source=make_hardware_raster(bitmap->width,bitmap->height);source->upload(bitmap->pixels,true);surface->blit(*source,rect,{0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)},dc,0x01ff0000U);
                            if(arg(1)>=0x10000000 && gui_.windows.contains(arg(1)-0x10000000))present_gui(arg(1)-0x10000000);
                        }
                    }
                }
            }
        }
    }
    else return std::nullopt;
    arg.return_to(state,result);return std::nullopt;
}
void WindowsServices::discard_theme_text(std::uint64_t token) {
    const auto found=gui_.theme_text_operations.find(token);if(found==gui_.theme_text_operations.end())return;
    if(const auto dc=gui_.contexts.find(found->second.dc);dc!=gui_.contexts.end())dc->second=found->second.saved;
    if(found->second.window_text){const auto owner=gui_.windows.find(found->second.dc-0x10000000);if(owner!=gui_.windows.end()){owner->second.text_color=(*found->second.window_text)[0];owner->second.background_mode=(*found->second.window_text)[1];}}
    gui_.theme_text_operations.erase(found);
}
std::optional<std::uint32_t> WindowsServices::finish_theme_text(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value) {
    const auto found=gui_.theme_text_operations.find(frame.value);const bool success=value || (found!=gui_.theme_text_operations.end() && found->second.allow_zero);
    const auto error=last_error_;discard_theme_text(frame.value);complete_call(state,memory,success?0:0x80070000U|(error?error:87));return std::nullopt;
}
}
