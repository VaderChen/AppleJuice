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
        {"uxtheme.dll","EnableTheming",1},{"uxtheme.dll","GetCurrentThemeName",6},
        {"uxtheme.dll","DrawThemeBackground",6},{"uxtheme.dll","DrawThemeBackgroundEx",6},
        {"uxtheme.dll","DrawThemeText",9},{"uxtheme.dll","DrawThemeTextEx",9},
        {"uxtheme.dll","GetThemeBackgroundContentRect",6},{"uxtheme.dll","GetThemePartSize",7},
        {"uxtheme.dll","IsThemePartDefined",3},{"uxtheme.dll","IsThemeBackgroundPartiallyTransparent",3},
        {"uxtheme.dll","GetThemeColor",5},{"uxtheme.dll","GetThemeSysColor",2},
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
    }else if(name=="EnableTheming") {
        if(arg(0) && !native)result=e_notimpl;
        else if(gui_.theme_enabled!=(arg(0)!=0)){
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
    }else if(name=="CloseThemeData") {
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
