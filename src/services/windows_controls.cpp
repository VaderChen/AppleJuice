#include "x64arm/windows_services.hpp"
#include "x64arm/windows_calendar.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_dib_rows.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_userdraw.hpp"
#include <algorithm>
#include <bit>
#include <functional>
#include <charconv>
#include <tuple>

namespace x64arm {
std::span<const WindowsServiceDescriptor> control_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","GetComboBoxInfo",2},
        {"comctl32.dll","ImageList_Create",5},{"comctl32.dll","ImageList_Destroy",1},
        {"comctl32.dll","ImageList_ReplaceIcon",3},{"comctl32.dll","ImageList_AddIcon",2},
        {"comctl32.dll","ImageList_AddMasked",3},{"comctl32.dll","ImageList_Add",3},{"comctl32.dll","ImageList_Replace",4},
        {"comctl32.dll","ImageList_GetImageCount",1},{"comctl32.dll","ImageList_SetImageCount",2},
        {"comctl32.dll","ImageList_GetIcon",3},{"comctl32.dll","ImageList_Remove",2},{"comctl32.dll","ImageList_GetIconSize",3},
        {"comctl32.dll","ImageList_SetBkColor",2},{"comctl32.dll","ImageList_GetBkColor",1},
        {"comctl32.dll","ImageList_Draw",6},{"comctl32.dll","ImageList_DrawEx",10},{"comctl32.dll","ImageList_GetImageInfo",3},
        {"comctl32.dll","CreateToolbarEx",13},{"comctl32.dll","CreateStatusWindowA",4},{"comctl32.dll","CreateStatusWindowW",4}
    };return table;
}
GuiBitmap decode_dib_bitmap(std::span<const std::uint8_t> bytes){
    const auto integer=[&](std::size_t at,unsigned size){
        if(at>bytes.size() || size>bytes.size()-at)throw HostCallFault("DIB bitmap 資料截斷");
        std::uint32_t value=0;for(unsigned n=0;n<size;++n)value|=std::uint32_t{bytes[at+n]}<<(n*8U);return value;
    };
    const auto header=integer(0,4);if(header<40 || header>bytes.size())throw HostCallFault("尚未支援此 DIB bitmap header");
    const auto width=std::bit_cast<std::int32_t>(integer(4,4)),signed_height=std::bit_cast<std::int32_t>(integer(8,4));
    const auto height=static_cast<std::uint32_t>(signed_height<0?-static_cast<std::int64_t>(signed_height):signed_height);
    const auto bpp=integer(14,2),compression=integer(16,4);
    if(width<=0 || width>8192 || !height || height>8192 || static_cast<std::uint64_t>(width)*height>16U*1024U*1024U || integer(12,2)!=1 ||
        (compression!=0 && compression!=3) || (compression==3 && bpp!=16 && bpp!=32) ||
        (bpp!=1 && bpp!=4 && bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32))throw HostCallFault("DIB bitmap 尺寸、壓縮或色深不支援");
    std::array<std::uint32_t,3> masks=bpp==16?std::array<std::uint32_t,3>{0x7c00,0x3e0,0x1f}:std::array<std::uint32_t,3>{0xff0000,0xff00,0xff};
    if(compression==3){
        if(header!=40 && header<52)throw HostCallFault("DIB bitfield header 截斷");
        for(unsigned n=0;n<masks.size();++n)masks[n]=integer(40+n*4,4);
        if(!valid_dib_masks(masks,bpp))throw HostCallFault("DIB bitfield mask 為空、不連續、重疊或超過色深");
    }
    const auto colors=bpp<=8?(integer(32,4)?integer(32,4):(1U<<bpp)):0;
    if(colors>256 || (bpp<=8 && colors>(1U<<bpp)))throw HostCallFault("DIB palette 大小不合法");
    const auto palette_start=header+(compression==3 && header==40?12U:0U);
    const auto pixel_start=static_cast<std::uint64_t>(palette_start)+colors*4;
    const auto stride=(static_cast<std::uint64_t>(width)*bpp+31)/32*4;
    if(pixel_start>bytes.size() || height*stride>bytes.size()-pixel_start)throw HostCallFault("DIB 像素資料截斷");
    GuiBitmap bitmap{static_cast<std::uint32_t>(width),height,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height)};
    const auto color=[&](std::size_t at){return (integer(at,1)<<16)|(integer(at+1,1)<<8)|integer(at+2,1);};
    std::vector<std::uint32_t> palette;for(unsigned n=0;n<colors;++n)palette.push_back(color(palette_start+n*4));
    bitmap.bits_per_pixel=static_cast<std::uint16_t>(bpp);bitmap.dib_masks={masks[0],masks[1],masks[2],0};bitmap.dib_palette=palette;
    const DibRowCodec codec(bpp,{masks[0],masks[1],masks[2],0},palette);
    for(std::uint32_t y=0;y<height;++y){const auto row=pixel_start+(signed_height<0?y:height-y-1)*stride;
        auto target=std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*bitmap.width,bitmap.width);
        if(!codec.decode(bytes.subspan(static_cast<std::size_t>(row),static_cast<std::size_t>(stride)),target))throw HostCallFault("DIB palette index 越界");
        for(auto& pixel:target)pixel&=0xffffffU;
    }
    return bitmap;
}
namespace {
std::uint64_t bitmap_images(GuiImageList& list,const GuiBitmap& bitmap,const GuiBitmap* mask,std::optional<std::uint32_t> transparent,bool replace,std::int32_t index){
        const auto width=static_cast<std::uint32_t>(list.width),height=static_cast<std::uint32_t>(list.height);
        if(!width || !height || bitmap.width<width || bitmap.height<height || (mask && (mask->width<bitmap.width || mask->height<height)))return UINT64_MAX;
        const auto count=replace?1U:bitmap.width/width;
        if((replace && (index<0 || static_cast<std::size_t>(index)>=list.images.size())) || (!replace && count>65536-list.images.size()))return UINT64_MAX;
        std::vector<GuiIcon> icons;icons.reserve(count);
        for(std::uint32_t n=0;n<count;++n){
            GuiIcon icon;icon.width=width;icon.height=height;icon.rgba.resize(static_cast<std::size_t>(width)*height);
            for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
                const auto source=static_cast<std::size_t>(y)*bitmap.width+n*width+x;const auto color=bitmap.pixels.at(source)&0xffffffU;
                // 光柵快照帶 alpha 位元組；遮罩只以 RGB 判斷白色（透明）。
                const bool clear=transparent?color==(*transparent&0xffffffU):mask && (mask->pixels.at(static_cast<std::size_t>(y)*mask->width+n*width+x)&0xffffffU)!=0;
                icon.rgba[static_cast<std::size_t>(y)*width+x]=color|(clear?0U:0xff000000U);
            }
            // 與 CreateIconIndirect 相同補齊 AND 遮罩／色彩平面，ImageList_GetIcon／ImageList_Draw 共用 DrawIconEx 路徑。
            finish_icon(icon);icons.push_back(std::move(icon));
        }
        if(replace){list.images[static_cast<std::size_t>(index)]=std::move(icons.front());return UINT64_C(1);}
        const auto first=list.images.size();for(auto& icon:icons)list.images.push_back(std::move(icon));return static_cast<std::uint64_t>(first);
    }
constexpr std::size_t notify_bytes=gui_control_notification_bytes;
std::u16string list_text(const GuiItem& item,std::uint32_t subitem){
    return subitem==0?item.text:subitem<item.subitems.size()?item.subitems[subitem]:std::u16string{};
}
void set_list_text(GuiItem& item,std::uint32_t subitem,std::u16string text){
    if(subitem==0)item.text=std::move(text);
    else {if(item.subitems.size()<=subitem)item.subitems.resize(subitem+1);item.subitems[subitem]=std::move(text);}
    item.resolved_subitems.insert(subitem);
}
bool needs_list_text(const GuiItem& item,std::uint32_t subitem){
    return gui_list_text_callback(item,subitem) && !item.resolved_subitems.contains(subitem);
}
void list_notification(GuiState& gui,GuestMemory& memory,const std::function<std::uint64_t()>& allocate,
    std::uint64_t handle,std::int32_t code,std::int32_t row,std::uint32_t subitem,
    std::uint32_t old_state=0,std::uint32_t new_state=0,const GuiItem* removed=nullptr,const GuiMessage* event=nullptr){
    const auto found=gui.windows.find(handle);if(found==gui.windows.end())return;
    auto& w=found->second;const auto parent=gui.windows.find(w.parent);if(parent==gui.windows.end())return;
    const auto procedure=parent->second.procedure;
    if(!procedure)return;
    const bool display=code==-177 || code==-150;
    const GuiItem* item=removed?removed:row>=0 && static_cast<std::size_t>(row)<w.items.size()?&w.items[static_cast<std::size_t>(row)]:nullptr;
    if(display && (!item || std::any_of(gui.control_notifications.begin(),gui.control_notifications.end(),[&](const auto& entry){
        return entry.second.display && entry.second.window==handle && entry.second.item==item->handle && entry.second.subitem==subitem;
    })))return;
    if(gui.control_notifications.size()>=4096 || gui.sent_messages.size()>=8192)throw HostCallFault("ListView 通知佇列超過上限");
    const GuiControlAbi abi(memory.execution_mode()==ExecutionMode::X86);const auto header=abi.notify_size;
    std::uint64_t data;
    if(gui.control_notification_buffers.empty())data=allocate();
    else {data=gui.control_notification_buffers.back();gui.control_notification_buffers.pop_back();}
    memory.write(GuestAddress{data},std::array<std::uint8_t,notify_bytes>{});
    memory.store(GuestAddress{data},abi.bits,handle);memory.store(GuestAddress{data+abi.unit},abi.bits,w.id);
    memory.store(GuestAddress{data+abi.notify_code},32,static_cast<std::uint32_t>(code));
    if(display){
        memory.store(GuestAddress{data+header},32,1U|(item->image==-1?2U:0U));
        memory.store(GuestAddress{data+header+4},32,static_cast<std::uint32_t>(row));memory.store(GuestAddress{data+header+8},32,subitem);
        memory.store(GuestAddress{data+header+abi.list_text},abi.bits,data+112);memory.store(GuestAddress{data+header+abi.list_capacity},32,2048);
        memory.store(GuestAddress{data+header+abi.list_image},32,static_cast<std::uint32_t>(item->image));memory.store(GuestAddress{data+header+abi.list_data},abi.bits,item->data);
    }else{
        memory.store(GuestAddress{data+header},32,static_cast<std::uint32_t>(row));memory.store(GuestAddress{data+header+4},32,subitem);
        memory.store(GuestAddress{data+header+8},32,new_state);memory.store(GuestAddress{data+header+12},32,old_state);
        memory.store(GuestAddress{data+header+16},32,old_state!=new_state?8U:0U);
        const auto parameter=header+(abi.unit==4?28U:32U);
        if(item)memory.store(GuestAddress{data+parameter},abi.bits,item->data);
        if(event && event->has_point){memory.store(GuestAddress{data+header+20},32,static_cast<std::uint32_t>(event->point[0]));memory.store(GuestAddress{data+header+24},32,static_cast<std::uint32_t>(event->point[1]));}
        if(code==-114 && event)memory.store(GuestAddress{data+parameter+abi.unit},32,((event->modifiers&1)?4U:0U)|(event->modifiers&2)|((event->modifiers&4)?1U:0U));
    }
    gui.control_notifications.emplace(data,GuiControlNotification{handle,item?item->handle:0,subitem,display,w.list_unicode});
    GuiSentMessage request;request.message={w.parent,w.id,data,0x4e};request.receiver=parent->second.thread;request.wide=w.list_unicode;
    gui.sent_messages.emplace(gui.next_object++,std::move(request));
}
}
namespace {
std::optional<GuiImageList> stock_toolbar_images(std::uint64_t resource){
    if(resource!=0 && resource!=1 && resource!=4 && resource!=5 && resource!=8 && resource!=9)return std::nullopt;
    GuiImageList list;list.width=list.height=(resource&1U)?24:16;
    const unsigned group=static_cast<unsigned>(resource)&~1U,count=group==0?15U:group==4?13U:5U;
    for(unsigned i=0;i<count;++i)list.images.push_back(GuiIcon{{},static_cast<std::uint32_t>(list.width),static_cast<std::uint32_t>(list.height),0x20000U+group*256U+i});
    return list;
}
}
void WindowsServices::layout_gui_toolbar(GuiWindow& window){
    const auto font=gui_font(gui_,window.font);window.toolbar_font=font;
    const auto metrics=native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
    const bool list=(window.style&0x1000U)!=0;
    const auto image_list=gui_.image_lists.find(window.list_image_lists[0]);
    if(image_list!=gui_.image_lists.end()){
        window.toolbar_bitmap_width=static_cast<std::uint32_t>(image_list->second.width);
        window.toolbar_bitmap_height=static_cast<std::uint32_t>(image_list->second.height);
    }
    std::vector<std::uint32_t> widths;std::uint32_t common=window.toolbar_requested_width?window.toolbar_requested_width:28;
    std::uint32_t height=window.toolbar_requested_height?window.toolbar_requested_height:26;
    for(const auto& button:window.toolbar_buttons){
        std::u16string text;
        if(window.toolbar_text_rows)for(std::size_t i=0;i<button.text.size();++i){
            if(button.text[i]==u'&'){if(i+1<button.text.size() && button.text[i+1]==u'&'){text.push_back(u'&');++i;}}else text.push_back(button.text[i]);
        }
        const auto fallback=static_cast<std::int32_t>(std::min<std::size_t>(text.size(),4096))*std::max(1,metrics.average_width);
        const auto measured=native_gui_?native_gui_->text_width(font,text).value_or(fallback):fallback;
        const auto tw=static_cast<std::uint32_t>(std::clamp(measured,0,4096)),th=text.empty()?0U:static_cast<std::uint32_t>(std::max(1,metrics.height));
        const auto iw=button.image>=0?window.toolbar_bitmap_width:0U,ih=button.image>=0?window.toolbar_bitmap_height:0U;
        const auto width=std::clamp((list?iw+tw+(iw && tw?4U:0U):std::max(iw,tw))+8U,window.toolbar_min_width,window.toolbar_max_width);
        widths.push_back(width);
        if(!(button.style&1U) && !(button.state&8U)){
            common=std::max(common,width);
            height=std::max(height,(list?std::max(ih,th):ih+th+(ih && th?2U:0U))+6U);
        }
    }
    window.toolbar_button_width=std::clamp(common,window.toolbar_min_width,window.toolbar_max_width);
    window.toolbar_button_height=std::min(height,4096U);
    std::uint32_t x=0,y=0,extent=0;bool any=false;
    for(std::size_t i=0;i<window.toolbar_buttons.size();++i){auto& button=window.toolbar_buttons[i];
        button.rectangle={};if(button.state&8U)continue;
        const auto width=(button.style&1U)?static_cast<std::uint32_t>(button.image>0?button.image:8):button.width?button.width:((button.style&0x10U) || list)?widths[i]:window.toolbar_button_width;
        if((window.style&0x200U) && x && window.width && x+width>window.width){x=0;y+=window.toolbar_button_height;}
        button.rectangle={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),static_cast<std::int32_t>(x+width),static_cast<std::int32_t>(y+window.toolbar_button_height)};
        x+=width;extent=std::max(extent,x);any=true;
        if(button.state&0x20U){x=0;y+=window.toolbar_button_height;}
    }
    window.toolbar_content_width=extent;
    window.toolbar_content_height=any?y+(x?window.toolbar_button_height:0U):window.toolbar_button_height;
}
void WindowsServices::present_gui(std::uint64_t handle) {
    if(const auto found=gui_.windows.find(handle);found!=gui_.windows.end() && gui_control_class(found->second)==u"comboboxex32"){
        auto& window=found->second;
        for(const auto child:{window.combo_control})if(const auto inner=gui_.windows.find(child);inner!=gui_.windows.end()){
            inner->second.title=window.title;inner->second.selection=window.selection;inner->second.width=window.width;inner->second.height=window.height;
            inner->second.combo_drop_height=window.combo_drop_height;
            inner->second.font=window.font;gui_set_window_enabled(inner->second,window.enabled);inner->second.text_limit=window.text_limit;
            present_gui(child);
        }
    }
    if(const auto found=gui_.windows.find(handle);found!=gui_.windows.end() && gui_control_class(found->second)==u"combobox"){
        layout_gui_combo(handle);
        for(const auto child:{found->second.edit_control,found->second.list_control})if(gui_.windows.contains(child) && native_gui_)present_native_window(child,gui_.windows.at(child));
    }
    if(const auto found=gui_.windows.find(handle);found!=gui_.windows.end() && (gui_control_class(found->second)==u"syslistview32" || gui_control_class(found->second)==u"systreeview32" || gui_control_class(found->second)==u"toolbarwindow32")){
        auto& window=found->second;const auto list=gui_.image_lists.find(window.list_image_lists[gui_control_class(window)==u"systreeview32" || gui_control_class(window)==u"toolbarwindow32" || (window.style&3)==0?0:1]);
        window.list_icons=list==gui_.image_lists.end()?std::vector<GuiIcon>{}:list->second.images;
        if(gui_control_class(window)==u"toolbarwindow32")layout_gui_toolbar(window);
    }
    if(gui_.windows.contains(handle)){
        auto& window=gui_.windows.at(handle);window.control_font=gui_font(gui_,window.font);
        if(gui_calendar_class(gui_control_class(window)) && window.calendar){
            gui_calendar_project(window,native_gui_.get());
            if(window.calendar->picker && gui_.focus!=handle)for(auto& cell:window.calendar->cells)if(cell.action>0)cell.selected=false;
            if(window.calendar->picker){auto& date=*window.calendar;
                if(const auto editor=gui_.windows.find(date.edit_window);editor!=gui_.windows.end()){auto& child=editor->second;child.x=(window.style&2U)?24:5;child.y=2;child.width=window.width>static_cast<unsigned>(child.x+21)?window.width-static_cast<unsigned>(child.x+21):1;child.height=window.height>4?window.height-4:1;child.font=window.font;child.control_font=window.control_font;gui_set_window_enabled(child,window.enabled);child.visible=date.editing;child.style=(child.style&~0x10000000U)|(child.visible?0x10000000U:0U);if(native_gui_)present_native_window(editor->first,child);}
                if(const auto stepper=gui_.windows.find(date.updown_window);stepper!=gui_.windows.end()){auto& child=stepper->second;const auto field=gui_date_field_spin(date);child.spin_min=field[0];child.spin_max=field[1];child.spin_position=field[2];child.x=window.width>20?static_cast<int>(window.width-20):0;child.y=0;child.width=20;child.height=window.height;gui_set_window_enabled(child,window.enabled);child.visible=(window.style&1U)!=0;child.style=(child.style&~0x10000000U)|(child.visible?0x10000000U:0U);if(native_gui_)present_native_window(stepper->first,child);}
            }
        }
        if(gui_control_class(window)==u"sysheader32"){
            layout_gui_header(handle);
            // 外觀改變也須重新走客體 paint 契約；正在繪製的回呼不自行排入另一輪。
            const bool painting=std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& entry){return entry.second.message.window==handle && entry.second.header && entry.second.header->kind==GuiControlOperation::Header::Kind::Paint;});
            if(!painting)invalidate_window(handle);
        }
        if(gui_control_class(window)==u"syslistview32")sync_gui_list_header(handle);
        if(gui_control_class(window)==u"systabcontrol32"){
            layout_gui_tab(window);
            const auto target=gui_.windows.find(window.tab->tooltips);
            if(target!=gui_.windows.end() && target->second.tooltip){
                auto& tip=*target->second.tooltip;auto tools=tip.tools;
                std::erase_if(tools,[&](const auto& tool){return tool.owner==handle;});
                for(std::size_t i=0;i<window.tab->items.size();++i){
                    const auto identity=window.tab->items[i].identity;
                    const auto old=std::find_if(tip.tools.begin(),tip.tools.end(),[&](const auto& tool){return tool.owner==handle && tool.identity==identity;});
                    GuiTooltipTool tool;
                    if(old!=tip.tools.end())tool=*old;
                    else {tool.callback=true;tool.text_pointer=x86_?UINT32_MAX:UINT64_MAX;}
                    tool.identity=identity;tool.owner=handle;tool.id=i;tool.flags=0x10U;tool.wide=window.list_unicode;tool.rectangle=window.tab->rectangles[i];tools.push_back(std::move(tool));
                }
                if(tools!=tip.tools){
                    const auto exists=[&](std::uint64_t identity){return std::any_of(tools.begin(),tools.end(),[&](const auto& tool){return tool.identity==identity;});};
                    if(tip.shown && !exists(tip.shown))hide_gui_tooltip(window.tab->tooltips);
                    if(tip.current && !exists(tip.current)){tip.current=0;tip.tracking=false;}
                    tip.tools=std::move(tools);++tip.revision;
                }
            }
        }
        if(gui_control_class(window)==u"tooltips_class32" && window.tooltip)layout_gui_tooltip(window);
        if(gui_control_class(window)==u"syslink"){
            if(!window.links_initialized)parse_gui_syslink(window);
            window.link_layout=layout_gui_syslink(window,window.width);
        }
        if(!window.parent){auto icon=window.window_icon?window.window_icon:window.window_small_icon;
            if(!icon){const auto cls=gui_.classes.find(window.class_name);if(cls!=gui_.classes.end())icon=cls->second.icon?cls->second.icon:cls->second.small_icon;}
            const auto found=gui_.icons.find(icon);window.application_icon=found==gui_.icons.end()?gui_.default_application_icon:found->second;
        }
    }
    if(native_gui_ && gui_.windows.contains(handle) && gui_.windows.at(handle).redraw)present_native_window(handle,gui_.windows.at(handle));
}
std::vector<GuiWindowCreation> WindowsServices::plan_gui_control_children(std::uint64_t handle,bool wide){
    std::vector<GuiWindowCreation> plans;
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return plans;
    auto& outer=found->second;const auto cls=gui_control_class(outer);
    if(cls==u"syslistview32"){
        if(outer.list_header)return plans;
        if(gui_.windows.size()>=4096)throw std::bad_alloc{};
        GuiWindow child;child.dpi_context=outer.dpi_context;child.thread=outer.thread;child.desktop_object=outer.desktop_object;child.instance=outer.instance;
        child.builtin=true;child.parent=handle;child.header_list=handle;child.style=0x50000082U;child.font=outer.font;child.header.emplace();child.header->inherited_font=outer.font;
        child.class_name=child.control_class=u"sysheader32";child.procedure=stock_gui_proc(child.class_name,wide);child.width=outer.width;child.height=24;
        outer.list_header=allocate_gui_window();plans.push_back({outer.list_header,std::move(child),wide});return plans;
    }
    if(cls==u"systabcontrol32" && (outer.style&0x4000U)){
        if(!outer.tab)outer.tab.emplace();if(outer.tab->tooltips)return plans;
        if(gui_.windows.size()>=4096)throw std::bad_alloc{};
        GuiWindow child;child.dpi_context=outer.dpi_context;child.thread=outer.thread;child.desktop_object=outer.desktop_object;child.instance=outer.instance;
        child.builtin=true;child.owner=handle;child.style=0x80000000U;child.ex_style=0x08000088U;child.font=outer.font;
        child.class_name=child.control_class=u"tooltips_class32";child.procedure=stock_gui_proc(child.class_name,wide);
        outer.tab->tooltips=allocate_gui_window();plans.push_back({outer.tab->tooltips,std::move(child),wide});return plans;
    }
    if((cls!=u"comboboxex32" && cls!=u"combobox") || (cls==u"comboboxex32"?outer.combo_control!=0:outer.list_control!=0))return plans;
    if(gui_.windows.size()>4093)throw std::bad_alloc{};
    const auto make=[&](std::u16string name,std::uint32_t style,std::uint64_t id){
        GuiWindow child;child.dpi_context=effective_dpi_context();child.thread=outer.thread;child.desktop_object=outer.desktop_object;child.instance=outer.instance;child.builtin=true;child.parent=handle;child.id=id;child.style=style;
        child.class_name=std::move(name);child.control_class=child.class_name;child.procedure=stock_gui_proc(child.class_name,wide);
        child.width=outer.width;child.height=24;child.title=outer.title;gui_set_window_enabled(child,outer.enabled);child.font=outer.font;
        return child;
    };
    if(cls==u"comboboxex32"){
        if((outer.style&3U)!=1U)outer.height=24;
        auto child=make(u"combobox",(outer.style&0xffffU)|0x50000000U,1000);child.height=outer.height;child.combo_owner=handle;child.combo_drop_height=outer.combo_drop_height;
        outer.combo_control=allocate_gui_window();plans.push_back({outer.combo_control,std::move(child),wide});return plans;
    }
    const auto owner=outer.combo_owner?outer.combo_owner:handle;const bool simple=(outer.style&3U)==1U;
    if((outer.style&3U)!=3U){
        auto edit=make(u"edit",0x50000080U,1001);edit.combo_owner=owner;outer.edit_control=allocate_gui_window();
        plans.push_back({outer.edit_control,std::move(edit),wide});
    }
    auto list=make(u"listbox",simple?0x50800001U:0x80800001U,1000);list.class_name=u"combolbox";list.combo_list_owner=owner;
    list.height=simple?(outer.height>24?outer.height-24:1):outer.combo_drop_height;list.y=24;
    if(!simple){list.parent=0;list.owner=handle;list.ex_style=0x80U;const auto origin=gui_client_origin(gui_,handle);list.x=static_cast<std::int32_t>(origin[0]);list.y=static_cast<std::int32_t>(origin[1]+outer.height);}
    outer.list_control=allocate_gui_window();plans.push_back({outer.list_control,std::move(list),wide});
    if(outer.combo_owner){auto& ex=gui_.windows.at(outer.combo_owner);ex.edit_control=outer.edit_control;ex.list_control=outer.list_control;}
    return plans;
}
void WindowsServices::layout_gui_combo(std::uint64_t handle){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end() || gui_control_class(found->second)!=u"combobox")return;
    const auto& combo=found->second;const auto owner=gui_.windows.find(combo.combo_owner);const auto& data=owner==gui_.windows.end()?combo:owner->second;
    const bool simple=(combo.style&3U)==1U;const auto height=std::min(combo.height,24U);const auto button=simple?0U:std::min(combo.width,16U);
    if(const auto edit=gui_.windows.find(combo.edit_control);edit!=gui_.windows.end()){
        auto& w=edit->second;w.x=2;w.y=2;w.width=combo.width>button+4?combo.width-button-4:0;w.height=height>4?height-4:0;
        w.title=data.title;w.font=data.font;gui_set_window_enabled(w,data.enabled);w.text_limit=data.text_limit;
    }
    if(const auto list=gui_.windows.find(combo.list_control);list!=gui_.windows.end()){
        auto& w=list->second;const auto origin=gui_client_origin(gui_,handle);w.x=simple?0:static_cast<std::int32_t>(origin[0]);w.y=simple?24:static_cast<std::int32_t>(origin[1]+combo.height);
        w.width=combo.width;w.height=simple?(combo.height>24?combo.height-24:1):combo.combo_drop_height;
        w.items=data.items;w.selection=data.selection;w.font=data.font;gui_set_window_enabled(w,data.enabled);
    }
}
std::uint64_t WindowsServices::write_gui_combo_info(GuestMemory& memory,std::uint64_t handle,std::uint64_t output){
    auto found=gui_.windows.find(handle);if(found==gui_.windows.end()){last_error_=1400;return 0;}
    if(found==gui_.windows.end() || gui_control_class(found->second)!=u"combobox"){last_error_=1423;return 0;}
    const GuiControlAbi abi(x86_);const auto size=x86_?52U:64U;
    if(!output || memory.load(GuestAddress{output},32)!=size){last_error_=87;return 0;}
    memory.check(GuestAddress{output},size,Access::Write);layout_gui_combo(handle);
    const auto& combo=found->second;const bool simple=(combo.style&3U)==1U;const auto height=std::min(combo.height,24U),button=simple?0U:std::min(combo.width,16U);
    const auto left=std::min(combo.width,2U),top=std::min(height,2U),right=std::max(left,combo.width-button>2?combo.width-button-2:0U),bottom=std::max(top,height>2?height-2:0U);
    const std::array<std::uint32_t,8> rects{left,top,right,bottom,simple?0U:combo.width-button,0,simple?0U:combo.width,simple?0U:height};
    for(unsigned i=0;i<rects.size();++i)memory.store(GuestAddress{output+4+i*4},32,rects[i]);
    // 展開清單與按鈕被滑鼠按住是不同狀態；CB_SHOWDROPDOWN 不產生 STATE_SYSTEM_PRESSED。
    memory.store(GuestAddress{output+36},32,simple?0x8000U:0U);
    memory.store(GuestAddress{output+40},abi.bits,handle);
    memory.store(GuestAddress{output+40+abi.unit},abi.bits,gui_.windows.contains(combo.edit_control)?combo.edit_control:0);
    memory.store(GuestAddress{output+40+abi.unit*2},abi.bits,gui_.windows.contains(combo.list_control)?combo.list_control:0);return 1;
}
std::uint64_t WindowsServices::gui_child(std::uint64_t parent,std::uint64_t id) const {
    for(const auto& [handle,window]:gui_.windows)if(window.parent==parent && window.id==id)return handle;
    return 0;
}
bool WindowsServices::gui_native_control_message(GuestMemory& memory,const GuiMessage& msg){
    const auto found=gui_.windows.find(msg.window);
    if(found==gui_.windows.end())return false;
    if(gui_control_class(found->second)==u"msctls_trackbar32" && msg.message==0x1000b){
        auto& w=found->second;w.track_position=std::clamp(static_cast<std::int32_t>(msg.wparam),w.track_min,w.track_max);
        present_gui(msg.window);
        const auto code=static_cast<std::uint32_t>(msg.lparam);
        const auto position=(code==4 || code==5)?static_cast<std::uint32_t>(w.track_position)&65535U:0U;
        enqueue_gui_message({w.parent,(position<<16)|code,msg.window,(w.style&2U)?0x115U:0x114U});return true;
    }
    if(gui_control_class(found->second)==u"msctls_updown32" && msg.message==0x1000b){
        auto& w=found->second;
        (void)gui_control_message(memory,{msg.window,0,0,0x472},true);
        list_notification(gui_,memory,[&]{return runtime_allocate(memory,{},notify_bytes);},msg.window,-722,w.spin_position,static_cast<std::uint32_t>(msg.wparam));
        return true;
    }
    if(found->second.combo_list_owner && msg.message==0x10002){
        const auto owner=gui_.windows.find(found->second.combo_list_owner);if(owner==gui_.windows.end())return true;
        auto& w=owner->second;const auto row=static_cast<std::int32_t>(msg.wparam);if(row<0 || static_cast<std::size_t>(row)>=w.items.size())return true;
        w.selection=row;w.title=w.items[static_cast<std::size_t>(row)].text;w.combo_edit_changed=false;
        const auto combo=gui_.windows.find(w.combo_control?w.combo_control:owner->first);
        if(combo!=gui_.windows.end() && (combo->second.style&3U)!=1U){combo->second.combo_dropped=false;found->second.visible=false;found->second.style&=~0x10000000U;if(native_gui_)show_native_window(msg.window,false);}
        present_gui(owner->first);enqueue_gui_message({w.parent,UINT64_C(0x10000)|(w.id&65535),owner->first,0x111});return true;
    }
    if(gui_control_class(found->second)==u"comboboxex32" && (msg.message==0x10001 || msg.message==0x10002)){
        auto& w=found->second;unsigned notification;
        if(msg.message==0x10001){w.title=msg.text.substr(0,w.text_limit);w.combo_edit_changed=true;notification=5;}
        else {const auto row=static_cast<std::int32_t>(msg.wparam);if(row<0 || static_cast<std::size_t>(row)>=w.items.size())return true;w.selection=row;w.title=w.items[static_cast<std::size_t>(row)].text;w.combo_edit_changed=false;notification=1;}
        gui_.focus=w.edit_control?w.edit_control:w.combo_control;present_gui(msg.window);
        enqueue_gui_message({w.parent,(static_cast<std::uint64_t>(notification)<<16)|(w.id&65535),msg.window,0x111});return true;
    }
    if(gui_control_class(found->second)!=u"syslistview32")return false;
    if(msg.message<0x10005 || msg.message>0x10009)return false;
    auto& w=found->second;const auto row=static_cast<std::int32_t>(msg.wparam);
    const auto notify=[&](std::int32_t code,std::int32_t item,std::uint32_t subitem,std::uint32_t before=0,std::uint32_t after=0){
        list_notification(gui_,memory,[&]{return runtime_allocate(memory,{},notify_bytes);},msg.window,code,item,subitem,before,after,nullptr,&msg);
    };
    if((msg.message==0x10005 || msg.message==0x10006 || msg.message==0x10009) && gui_.focus!=msg.window){
        const auto previous=gui_.focus;gui_.focus=msg.window;
        const auto send_focus=[&](std::uint64_t handle,std::uint32_t code,std::uint64_t other){const auto target=gui_.windows.find(handle);if(target==gui_.windows.end())return;if(gui_.sent_messages.size()>=8192)throw HostCallFault("焦點訊息佇列超過上限");GuiSentMessage request;request.message={handle,other,0,code};request.receiver=target->second.thread;request.wide=w.list_unicode;gui_.sent_messages.emplace(gui_.next_object++,std::move(request));};
        if(previous){send_focus(previous,8,msg.window);const auto old=gui_.windows.find(previous);if(old!=gui_.windows.end() && gui_control_class(old->second)==u"syslistview32")list_notification(gui_,memory,[&]{return runtime_allocate(memory,{},notify_bytes);},previous,-8,-1,0);}
        send_focus(msg.window,7,previous);notify(-7,-1,0);
    }
    if(msg.message==0x10005){
        std::set<std::uint32_t> selected(msg.indices.begin(),msg.indices.end());
        if(w.style&4U){selected.clear();if(row>=0)selected.insert(static_cast<std::uint32_t>(row));}
        for(std::size_t i=0;i<w.items.size();++i){
            auto& item=w.items[i];const auto before=item.state;
            item.state=(before&~3U)|(selected.contains(static_cast<std::uint32_t>(i))?2U:0U)|(row==static_cast<std::int32_t>(i)?1U:0U);
            if(before!=item.state)notify(-101,static_cast<std::int32_t>(i),0,before,item.state);
        }
        w.selection=row>=0 && static_cast<std::size_t>(row)<w.items.size()?row:-1;gui_.focus=msg.window;present_gui(msg.window);
    }else if(msg.message==0x10006){
        notify(-16,-1,0);notify(-3,row,static_cast<std::uint32_t>(msg.lparam));
        if(row>=0 && static_cast<std::size_t>(row)<w.items.size()){const auto before=w.items[static_cast<std::size_t>(row)].state;notify(-114,row,static_cast<std::uint32_t>(msg.lparam),before,before|0x20U);}
    }else if(msg.message==0x10009)notify(-2,row,static_cast<std::uint32_t>(msg.lparam));
    else if(msg.message==0x10008)notify(-108,-1,static_cast<std::uint32_t>(msg.lparam));
    else if(row>=0 && static_cast<std::size_t>(row)<w.items.size() && msg.lparam<256 && needs_list_text(w.items[static_cast<std::size_t>(row)],static_cast<std::uint32_t>(msg.lparam)))
        notify(w.list_unicode?-177:-150,row,static_cast<std::uint32_t>(msg.lparam));
    return true;
}
void WindowsServices::finish_gui_control_notify(GuestMemory& memory,const GuiMessage& msg,std::uint64_t result){
    if(finish_power_notification(msg))return;
    const auto pending=gui_.control_notifications.find(msg.lparam);if(msg.message!=0x4e || pending==gui_.control_notifications.end())return;
    const auto request=pending->second;gui_.control_notifications.erase(pending);gui_.control_notification_buffers.push_back(msg.lparam);
    const auto found=gui_.windows.find(request.window);if(found==gui_.windows.end())return;
    const GuiControlAbi abi(x86_);const auto header=abi.notify_size;
    if(gui_control_class(found->second)==u"msctls_updown32"){
        auto& spin=found->second;
        if(spin.calendar_owner){present_gui(request.window);return;}
        if(!result){
            const auto delta=static_cast<std::int32_t>(memory.load(GuestAddress{msg.lparam+header+4},32));
            auto position=static_cast<std::int64_t>(spin.spin_position)+delta;
            const auto low=std::min(spin.spin_min,spin.spin_max),high=std::max(spin.spin_min,spin.spin_max);
            if(spin.style&1U){if(position>high)position=low;else if(position<low)position=high;}
            position=std::clamp<std::int64_t>(position,low,high);
            (void)gui_control_message(memory,{request.window,0,static_cast<std::uint64_t>(position),0x471},true);
            if(const auto buddy=gui_.windows.find(spin.spin_buddy);buddy!=gui_.windows.end() && (spin.style&2U))
                enqueue_gui_message({buddy->second.parent,UINT64_C(0x3000000)|(buddy->second.id&65535),spin.spin_buddy,0x111});
            enqueue_gui_message({spin.parent,(static_cast<std::uint64_t>(position)&65535U)<<16|4U,request.window,(spin.style&0x40U)?0x114U:0x115U});
        }
        present_gui(request.window);return;
    }
    if(!request.display)return;
    auto& w=found->second;const auto item=std::find_if(w.items.begin(),w.items.end(),[&](const auto& value){return value.handle==request.item;});
    if(item==w.items.end())return;
    const auto mask=memory.load(GuestAddress{msg.lparam+header},32),pointer=memory.load(GuestAddress{msg.lparam+header+abi.list_text},abi.bits);
    if((mask&1) && pointer && pointer!=(x86_?UINT32_MAX:UINT64_MAX)){
        const auto text=request.wide?read_utf16(memory,GuestAddress{pointer},2048):utf8_to_utf16_lossy(read_ansi(memory,pointer,2048));
        set_list_text(*item,request.subitem,text);
        if(mask&0x1000){item->callback_subitems.erase(request.subitem);item->stored_subitems.insert(request.subitem);}
    }else if(mask&1)set_list_text(*item,request.subitem,{});
    if(mask&2)item->image=static_cast<std::int32_t>(memory.load(GuestAddress{msg.lparam+header+abi.list_image},32));
    present_gui(request.window);
}
void WindowsServices::discard_gui_control_notify(const GuiMessage& msg){
    if(finish_power_notification(msg))return;
    if(msg.message==0x4e && gui_.control_notifications.erase(msg.lparam))gui_.control_notification_buffers.push_back(msg.lparam);
}
std::optional<std::uint32_t> WindowsServices::invoke_control_api(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const GuiControlAbi abi(x86_);
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto signed_value=[](std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));};
    std::uint64_t result=0;

    if(symbol=="GetComboBoxInfo"){
        // API 直接查詢控制項狀態；明確傳送 CB_GETCOMBOBOXINFO 才進入可 subclass 的 WNDPROC。
        result=write_gui_combo_info(memory,arg(0),arg(1));
    }else if(symbol=="ImageList_Create"){
        if(d(0) && d(1) && d(0)<=4096 && d(1)<=4096 && d(3)<=65536 && gui_.image_lists.size()<4096){
            result=gui_.next_object++;GuiImageList list;list.width=signed_value(arg(0));list.height=signed_value(arg(1));list.flags=d(2);gui_.image_lists.emplace(result,std::move(list));
        }
    }else if(symbol=="ImageList_Destroy"){
        if(const auto found=gui_.image_lists.find(arg(0));found!=gui_.image_lists.end()){gui_.bitmaps.erase(found->second.image_bitmap);gui_.bitmaps.erase(found->second.mask_bitmap);}
        result=gui_.image_lists.erase(arg(0))?1:0;
        for(const auto& [handle,window]:gui_.windows)if((window.tab && window.tab->image_list==arg(0)) || (window.header && std::find(window.header->image_lists.begin(),window.header->image_lists.end(),arg(0))!=window.header->image_lists.end()) || std::find(window.list_image_lists.begin(),window.list_image_lists.end(),arg(0))!=window.list_image_lists.end())present_gui(handle);
    }
    else if(symbol.starts_with("ImageList_")){
        const auto found=gui_.image_lists.find(arg(0));
        if(found==gui_.image_lists.end())result=symbol=="ImageList_ReplaceIcon" || symbol.starts_with("ImageList_Add")?UINT64_MAX:0;
        else {
            auto& list=found->second;
            if(symbol=="ImageList_GetImageCount")result=list.images.size();
            else if(symbol=="ImageList_SetImageCount"){if(d(1)<=65536){list.images.resize(d(1));result=1;}}
            else if(symbol=="ImageList_GetIconSize"){if(arg(1))memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(list.width));if(arg(2))memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(list.height));result=1;}
            else if(symbol=="ImageList_GetBkColor")result=list.background;
            else if(symbol=="ImageList_SetBkColor"){result=list.background;list.background=d(1);}
            else if(symbol=="ImageList_Draw" || symbol=="ImageList_DrawEx"){
                // ILD_MASK／ILD_IMAGE 對應 DI_MASK／DI_IMAGE；非 ILD_TRANSPARENT 時以背景色暫存筆刷先填滿；ILD_BLEND25/50 依前景色混合不透明像素。
                // DrawEx 的 dx／dy 是自左上角裁取的繪製範圍，不是縮放。
                const bool ex=symbol=="ImageList_DrawEx";const auto style=d(ex?9:5);
                if(d(1)<list.images.size()){
                    // ImageList_Add 只保存 rgba；繪製前與 DrawIconEx 共用同一份 AND 遮罩／色彩平面。
                    auto icon=list.images[d(1)];if(icon.mask_pixels.empty() || (!icon.monochrome && icon.color_pixels.empty()))finish_icon(icon);const auto count=static_cast<std::size_t>(icon.width)*icon.height;
                    if(!icon.animation_sequence.empty() || icon.mask_pixels.size()!=count*(icon.monochrome?2U:1U) || (!icon.monochrome && icon.color_pixels.size()!=count))last_error_=50;
                    else {
                        const auto background=!ex || d(7)==0xff000000U?list.background:d(7),foreground=ex?d(8):0xff000000U;
                        const unsigned flags=(style&0x10U)?1U:(style&0x20U)?2U:3U;
                        if((style&6U) && !icon.monochrome){
                            const auto tint=foreground==0xff000000U || foreground==0xffffffffU?gui_system_color(13):foreground;const unsigned weight=(style&4U)?128U:64U;
                            for(std::size_t i=0;i<count;++i){if(icon.mask_pixels[i] && !icon.alpha)continue;auto& pixel=icon.color_pixels[i];const auto alpha=icon.alpha?pixel>>24:255U;std::uint32_t value=pixel&0xff000000U;
                                for(unsigned c=0;c<3;++c){const auto base=(pixel>>(c*8))&255U,target=((tint>>(c*8))&255U)*alpha/255U;value|=((base*(256U-weight)+target*weight)>>8)<<(c*8);}pixel=value;}
                        }
                        const auto width=ex && d(5)?std::min<std::uint32_t>(d(5),icon.width):icon.width,height=ex && d(6)?std::min<std::uint32_t>(d(6),icon.height):icon.height;
                        if(width!=icon.width || height!=icon.height){
                            const auto crop=[&](auto& values,unsigned planes){std::remove_reference_t<decltype(values)> out(static_cast<std::size_t>(width)*height*planes);
                                for(unsigned plane=0;plane<planes;++plane)for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x)out[plane*width*height+y*width+x]=values[plane*count+static_cast<std::size_t>(y)*icon.width+x];values=std::move(out);};
                            crop(icon.mask_pixels,icon.monochrome?2U:1U);if(!icon.monochrome)crop(icon.color_pixels,1);if(icon.rgba.size()==count)crop(icon.rgba,1);icon.width=width;icon.height=height;icon.image.clear();
                        }
                        const auto handle=gui_.next_object++;gui_.icons.emplace(handle,std::move(icon));std::uint64_t brush=0;
                        if(flags==3 && !(style&1U) && background!=0xffffffffU){brush=gui_.next_object++;gui_.brushes.emplace(brush,background&0xffffffU);}
                        try{result=draw_gui_icon(memory,arg(2),handle,signed_value(arg(3)),signed_value(arg(4)),width,height,flags,brush)?1:0;}catch(...){gui_.icons.erase(handle);if(brush)gui_.brushes.erase(brush);throw;}
                        gui_.icons.erase(handle);if(brush)gui_.brushes.erase(brush);
                    }
                }
            }else if(symbol=="ImageList_GetImageInfo"){
                // comctl32 v6 以每列 4 格排列內部條帶；hbmMask 僅在 ILC_MASK 清單存在。
                if(arg(2) && d(1)<list.images.size()){
                    constexpr std::uint32_t columns=4;const auto width=static_cast<std::uint32_t>(list.width),height=static_cast<std::uint32_t>(list.height);
                    const auto rows=static_cast<std::uint32_t>((list.images.size()+columns-1)/columns);
                    GuiBitmap image{width*columns,height*rows,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*columns*height*rows,0)};GuiBitmap mask=image;mask.bits_per_pixel=1;
                    std::fill(mask.pixels.begin(),mask.pixels.end(),0xffffffU);
                    for(std::size_t n=0;n<list.images.size();++n){auto icon=list.images[n];if(icon.mask_pixels.empty() || (!icon.monochrome && icon.color_pixels.empty()))finish_icon(icon);const auto count=static_cast<std::size_t>(icon.width)*icon.height;
                        if(icon.mask_pixels.size()<count || (!icon.monochrome && icon.color_pixels.size()!=count))continue;const auto left=static_cast<std::uint32_t>(n%columns)*width,top=static_cast<std::uint32_t>(n/columns)*height;
                        for(std::uint32_t y=0;y<std::min(height,icon.height);++y)for(std::uint32_t x=0;x<std::min(width,icon.width);++x){const auto i=static_cast<std::size_t>(y)*icon.width+x,at=static_cast<std::size_t>(top+y)*image.width+left+x;
                            image.pixels[at]=icon.monochrome?(icon.mask_pixels[count+i]?0xffffffU:0U):icon.color_pixels[i];mask.pixels[at]=icon.mask_pixels[i]?0xffffffU:0U;}}
                    if(!list.image_bitmap){list.image_bitmap=gui_.next_object++;}gui_.bitmaps.insert_or_assign(list.image_bitmap,std::move(image));
                    if(list.flags&1U){if(!list.mask_bitmap)list.mask_bitmap=gui_.next_object++;gui_.bitmaps.insert_or_assign(list.mask_bitmap,std::move(mask));}
                    const unsigned unit=x86_?4U:8U;const auto info=arg(2);memory.check(GuestAddress{info},2*unit+24,Access::Write);
                    memory.store(GuestAddress{info},static_cast<std::uint8_t>(unit*8),list.image_bitmap);memory.store(GuestAddress{info+unit},static_cast<std::uint8_t>(unit*8),(list.flags&1U)?list.mask_bitmap:0);
                    memory.store(GuestAddress{info+2*unit},32,0);memory.store(GuestAddress{info+2*unit+4},32,0);
                    const std::array<std::uint32_t,4> rect{static_cast<std::uint32_t>(d(1)%columns)*width,static_cast<std::uint32_t>(d(1)/columns)*height,static_cast<std::uint32_t>(d(1)%columns+1)*width,static_cast<std::uint32_t>(d(1)/columns+1)*height};
                    for(unsigned i=0;i<4;++i)memory.store(GuestAddress{info+2*unit+8+i*4},32,rect[i]);result=1;
                }
            }
            else if(symbol=="ImageList_ReplaceIcon" || symbol=="ImageList_AddIcon"){
                const auto index=symbol=="ImageList_AddIcon"?-1:signed_value(arg(1));const auto icon=gui_.icons.find(arg(symbol=="ImageList_AddIcon"?1:2));result=UINT64_MAX;
                if(icon!=gui_.icons.end()){
                    auto resized=icon->second;if(resized.width && resized.height)resize_icon(resized,static_cast<std::uint32_t>(list.width),static_cast<std::uint32_t>(list.height));
                    if(index==-1 && list.images.size()<65536){result=list.images.size();list.images.push_back(std::move(resized));}
                    else if(index>=0 && static_cast<std::size_t>(index)<list.images.size()){list.images[static_cast<std::size_t>(index)]=std::move(resized);result=static_cast<std::uint32_t>(index);}
                }
            }else if(symbol=="ImageList_GetIcon"){
                if(d(1)<list.images.size()){result=gui_.next_object++;gui_.icons.emplace(result,list.images[d(1)]);}
            }else if(symbol=="ImageList_Remove"){
                if(d(1)==UINT32_MAX){list.images.clear();result=1;}else if(d(1)<list.images.size()){list.images.erase(list.images.begin()+d(1));result=1;}
            }else if(symbol=="ImageList_Add" || symbol=="ImageList_AddMasked" || symbol=="ImageList_Replace"){
                const bool replace=symbol=="ImageList_Replace",masked=symbol=="ImageList_AddMasked";
                sync_raster_bitmap(memory,arg(replace?2:1));if(!masked)sync_raster_bitmap(memory,arg(replace?3:2));
                const auto bitmap=gui_.bitmaps.find(arg(replace?2:1)),mask=masked?gui_.bitmaps.end():gui_.bitmaps.find(arg(replace?3:2));
                result=replace?0:UINT64_MAX;
                if(bitmap!=gui_.bitmaps.end() && (masked || !arg(replace?3:2) || mask!=gui_.bitmaps.end())){
                    std::optional<std::uint32_t> color;if(masked)color=d(2)==0xff000000U?bitmap->second.pixels.front():d(2);
                    const auto value=bitmap_images(list,bitmap->second,mask==gui_.bitmaps.end()?nullptr:&mask->second,color,replace,signed_value(arg(1)));
                    result=replace && value==UINT64_MAX?0:value;
                }
            }
        }
        for(const auto& [handle,window]:gui_.windows)if((window.tab && window.tab->image_list==arg(0)) || (window.header && std::find(window.header->image_lists.begin(),window.header->image_lists.end(),arg(0))!=window.header->image_lists.end()) || std::find(window.list_image_lists.begin(),window.list_image_lists.end(),arg(0))!=window.list_image_lists.end())present_gui(handle);
    }else if(symbol=="CreateToolbarEx" || symbol.starts_with("CreateStatusWindow")){
        const bool toolbar=symbol=="CreateToolbarEx";const auto parent=arg(toolbar?0:2);const auto owner=gui_.windows.find(parent);
        if(owner==gui_.windows.end())last_error_=1400;
        else if(gui_.windows.size()<4096){
            GuiWindow window;window.dpi_context=effective_dpi_context();window.thread=threads_.current;window.builtin=true;window.parent=parent;window.id=arg(toolbar?2:3);window.style=d(toolbar?1:0);
            window.class_name=toolbar?u"toolbarwindow32":u"msctls_statusbar32";window.control_class=window.class_name;window.procedure=stock_gui_proc(window.class_name,!toolbar && symbol.ends_with('W'));window.width=owner->second.width;window.height=toolbar?28:22;window.y=toolbar?0:static_cast<std::int32_t>(owner->second.height)-22;window.visible=(window.style&0x10000000U)!=0;
            if(toolbar){
                window.style|=0x40000000U;
                if(d(12)!=abi.toolbar_size || d(7)>4096 || (d(7) && !arg(6))){last_error_=87;arg.return_to(state,0);return std::nullopt;}
                window.toolbar_struct_size=abi.toolbar_size;window.toolbar_requested_width=d(8);window.toolbar_requested_height=d(9);window.toolbar_button_width=d(8)?d(8):28;window.toolbar_button_height=d(9)?d(9):26;
                window.toolbar_bitmap_width=d(10)?d(10):16;window.toolbar_bitmap_height=d(11)?d(11):16;
                window.height=window.toolbar_button_height+4;
                if(arg(6))memory.check(GuestAddress{arg(6)},static_cast<std::size_t>(d(7))*abi.toolbar_size,Access::Read);
                for(std::uint32_t i=0;i<d(7);++i){
                    const auto at=arg(6)+i*abi.toolbar_size;GuiToolbarButton button;button.image=signed_value(memory.load(GuestAddress{at},32));button.command=signed_value(memory.load(GuestAddress{at+4},32));
                    button.state=static_cast<std::uint8_t>(memory.load(GuestAddress{at+8},8));button.style=static_cast<std::uint8_t>(memory.load(GuestAddress{at+9},8));button.data=memory.load(GuestAddress{at+abi.toolbar_data},abi.bits);
                    const auto text=memory.load(GuestAddress{at+abi.toolbar_string},abi.bits);button.string_value=text;if(text>65535 && text!=(x86_?UINT32_MAX:UINT64_MAX))button.text=utf8_to_utf16_lossy(read_ansi(memory,text));window.toolbar_buttons.push_back(std::move(button));
                }
                std::optional<GuiBitmap> bitmap;
                if(arg(5) && arg(4)!=(x86_?UINT32_MAX:UINT64_MAX)){
                    if(!arg(4)){sync_raster_bitmap(memory,arg(5));const auto found_bitmap=gui_.bitmaps.find(arg(5));if(found_bitmap!=gui_.bitmaps.end())bitmap=found_bitmap->second;}
                    else if(const auto entry=gui_resource(memory,arg(4),arg(5),2,true)){
                        const auto* image=module_image(arg(4));if(image){const auto size=memory.load(GuestAddress{entry+4},32);if(size>16U*1024U*1024U)throw HostCallFault("工具列 bitmap 資源超過上限");std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{image->base+memory.load(GuestAddress{entry},32)},bytes);bitmap=decode_dib_bitmap(bytes);}
                    }
                }
                if(arg(4)==(x86_?UINT32_MAX:UINT64_MAX))if(auto list=stock_toolbar_images(arg(5))){
                    const auto images=gui_.next_object++;window.list_image_lists[0]=images;window.list_icons=list->images;gui_.image_lists.emplace(images,std::move(*list));
                }
                layout_gui_toolbar(window);
                if(bitmap){GuiImageList list;list.width=static_cast<std::int32_t>(window.toolbar_bitmap_width);list.height=static_cast<std::int32_t>(window.toolbar_bitmap_height);(void)bitmap_images(list,*bitmap,nullptr,bitmap->pixels.front(),false,-1);const auto images=gui_.next_object++;window.list_image_lists[0]=images;window.list_icons=list.images;gui_.image_lists.emplace(images,std::move(list));}
            }else {window.title=arg(1)?symbol.ends_with('W')?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16_lossy(read_ansi(memory,arg(1))):std::u16string{};window.status_parts.push_back({window.title,-1,0});}
            result=allocate_gui_window();gui_.windows.emplace(result,std::move(window));if(native_gui_)native_gui_->create(result,gui_.windows.at(result));
        }
    }else throw HostCallFault("尚未實作此 common control API");
    arg.return_to(state,result);return std::nullopt;
}
std::optional<GuiMessage> WindowsServices::complete_gui_button_click(std::uint64_t handle){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return std::nullopt;
    auto& button=found->second;if(!button.enabled || !gui_.windows.contains(button.parent))return std::nullopt;
    const auto kind=button.style&15U;if(kind==7)return std::nullopt;
    if(kind==3 || kind==6)button.check=(button.check+1)%(kind==6?3:2);
    if(kind==9){
        // 群組沿目前同父 sibling Z-order；其他父視窗的控制項不截斷這個群組。
        std::vector<std::uint64_t> siblings;for(const auto peer:gui_.z_order){const auto w=gui_.windows.find(peer);if(w!=gui_.windows.end() && w->second.parent==button.parent)siblings.push_back(peer);}
        const auto current=std::find(siblings.begin(),siblings.end(),handle);
        if(current!=siblings.end()){
            auto first=current;while(first!=siblings.begin() && !(gui_.windows.at(*first).style&0x20000U))--first;
            for(auto it=first;it!=siblings.end();++it){auto& peer=gui_.windows.at(*it);if(it!=first && (peer.style&0x20000U))break;
                if(gui_control_class(peer)==u"button" && ((peer.style&15U)==9 || (peer.style&15U)==4)){peer.check=*it==handle?1:0;present_gui(*it);}}
        }
        button.check=1;
    }
    present_gui(handle);return GuiMessage{button.parent,button.id&65535U,handle,0x111};
}
std::optional<std::uint64_t> WindowsServices::gui_control_message(GuestMemory& memory,const GuiMessage& msg,bool wide) {
    if(const auto result=shell_browse_control(memory,msg,wide))return result;
    auto found=gui_.windows.find(msg.window);if(found==gui_.windows.end())return 0;
    auto& w=found->second;const auto m=msg.message,wp=static_cast<std::uint32_t>(msg.wparam);const auto lp=msg.lparam;
    const GuiControlAbi abi(x86_);
    const auto cls=gui_control_class(w);
    if(m==0x55 && (lp==3 || lp==4) && cls!=u"sysheader32")return w.unicode?2:1;
    if(cls==u"ime")if(const auto result=imm_control_message(memory,msg,wide))return result;
    if(cls==u"syslink")if(const auto result=gui_syslink_message(memory,msg,wide))return result;
    if(cls==u"systabcontrol32")if(const auto result=gui_tab_message(memory,msg,wide))return result;
    if(gui_calendar_class(cls)){
        if(m==0x1015 && w.calendar && !w.calendar->picker)return wp || lp?0:gui_calendar_today_width(w,native_gui_.get());
        if(const auto result=gui_calendar_message(memory,w,msg,wide,x86_)){
            // 查詢不改變原生投影；同一份幾何供 API 命中及真正滑鼠操作使用。
            const auto picker=w.calendar && w.calendar->picker;
            constexpr std::array picker_updates{1U,5U,7U,8U,15U,0x100U,0x102U,0x200U,0x201U,0x202U,0x1002U,0x1004U,0x1005U,0x1032U,0x1006U,0x1009U,0x100bU,0x100dU};
            constexpr std::array month_updates{1U,5U,7U,8U,15U,0x100U,0x200U,0x201U,0x202U,0x1002U,0x1004U,0x1006U,0x1008U,0x100aU,0x100cU,0x100fU,0x1012U,0x1014U,0x101eU,0x1020U};
            const bool repaint=picker?std::find(picker_updates.begin(),picker_updates.end(),m)!=picker_updates.end():std::find(month_updates.begin(),month_updates.end(),m)!=month_updates.end();
            if(repaint)present_gui(msg.window);return result;
        }
    }
    if(cls==u"sysheader32")if(const auto result=gui_header_message(memory,msg,wide))return result;
    if(cls==u"tooltips_class32")if(const auto result=gui_tooltip_message(memory,msg,wide))return result;
    if((cls==u"combobox" || cls==u"comboboxex32") && (m==0x164 || m==0x14f || m==0x157)){
        if(m==0x164)return cls==u"combobox"?write_gui_combo_info(memory,msg.window,lp):0;
        const auto combo=gui_.windows.find(cls==u"comboboxex32"?w.combo_control:msg.window);if(combo==gui_.windows.end())return 0;
        if(m==0x157)return combo->second.combo_dropped?1:0;
        if((combo->second.style&3U)!=1U){
            combo->second.combo_dropped=wp!=0;const auto list=gui_.windows.find(combo->second.list_control);
            if(list!=gui_.windows.end()){list->second.visible=wp!=0;list->second.style=(list->second.style&~0x10000000U)|(wp?0x10000000U:0U);present_gui(combo->first);if(native_gui_)show_native_window(list->first,wp!=0);}
        }return 1;
    }
    if(w.combo_list_owner){
        // 清單 HWND 的基本 LB 資料操作與 combo 共用同一筆資料，不複製成另一份可變選取集合。
        for(const auto [list,combo]:std::array<std::pair<std::uint32_t,std::uint32_t>,14>{{{0x184,0x14b},{0x180,0x143},{0x181,0x14a},{0x182,0x144},{0x18b,0x146},{0x188,0x147},{0x186,0x14e},{0x18a,0x149},{0x189,0x148},{0x199,0x150},{0x19a,0x151},{0x18f,0x14c},{0x18c,0x14d},{0x1a2,0x158}}})
            if(m==list){auto forwarded=msg;forwarded.window=w.combo_list_owner;forwarded.message=combo;return gui_control_message(memory,forwarded,wide);}
    }
    // 內部 HWND 保有各自的 WndProc；只有進入 stock handler 後才共用 ComboBoxEx 資料。
    if(w.combo_owner && (m==0xc || m==0xd || m==0xe || m==0x30 || m==0x31 || m==0xc5 || (gui_control_class(w)==u"combobox" && m>=0x140 && m<=0x164))){
        auto forwarded=msg;forwarded.window=w.combo_owner;return gui_control_message(memory,forwarded,wide);
    }
    const auto read=[&](std::uint64_t p){return p?(wide?read_utf16(memory,GuestAddress{p}):utf8_to_utf16_lossy(read_ansi(memory,p))):std::u16string{};};
    const auto write=[&](std::uint64_t p,const std::u16string& text,std::uint64_t capacity){
        if(!capacity)return UINT64_C(0);
        if(wide){auto value=text.substr(0,static_cast<std::size_t>(capacity-1));memory.write(GuestAddress{p},utf16_bytes(value));return static_cast<std::uint64_t>(value.size());}
        auto value=utf16_to_utf8(text);value.resize(std::min<std::size_t>(value.size(),static_cast<std::size_t>(capacity-1)));std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);memory.write(GuestAddress{p},bytes);return static_cast<std::uint64_t>(value.size());
    };
    const auto update=[&](std::uint64_t result)->std::optional<std::uint64_t>{present_gui(msg.window);return result;};
    if(cls==u"msctls_updown32"){
        const auto set_position=[&](std::int32_t value){
            w.spin_position=std::clamp(value,std::min(w.spin_min,w.spin_max),std::max(w.spin_min,w.spin_max));
            if((w.style&2U) && gui_.windows.contains(w.spin_buddy)){
                char buffer[32];const auto converted=std::to_chars(buffer,buffer+sizeof(buffer),w.spin_position,static_cast<int>(w.spin_base));
                gui_.windows.at(w.spin_buddy).title=utf8_to_utf16_lossy(std::string(buffer,converted.ptr));present_gui(w.spin_buddy);
            }
        };
        const auto read_position=[&](){
            if(!(w.style&2U) || !gui_.windows.contains(w.spin_buddy))return false;
            auto value=utf16_to_utf8(gui_.windows.at(w.spin_buddy).title);
            if(!(w.style&0x80U))value.erase(std::remove(value.begin(),value.end(),','),value.end());
            std::int32_t number{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number,static_cast<int>(w.spin_base));
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || number<std::min(w.spin_min,w.spin_max) || number>std::max(w.spin_min,w.spin_max))return true;
            w.spin_position=number;return false;
        };
        if(m==1){
            if(w.style&0x10U){for(const auto handle:gui_.z_order){const auto& peer=gui_.windows.at(handle);if(handle!=msg.window && peer.parent==w.parent){w.spin_buddy=handle;break;}}}
            return update(0);
        }
        if(m==0x465){w.spin_max=static_cast<std::int16_t>(lp);w.spin_min=static_cast<std::int16_t>(lp>>16);set_position(w.spin_position);return update(0);}
        if(m==0x466)return (static_cast<std::uint32_t>(w.spin_min)&65535U)<<16|(static_cast<std::uint32_t>(w.spin_max)&65535U);
        if(m==0x467 || m==0x471){const auto old=w.spin_position;set_position(m==0x467?static_cast<std::int16_t>(lp):static_cast<std::int32_t>(lp));return update(static_cast<std::uint32_t>(old));}
        if(m==0x468 || m==0x472){const bool error=read_position();if(m==0x472){if(lp)memory.store(GuestAddress{lp},32,error);return static_cast<std::uint32_t>(w.spin_position);}return (static_cast<std::uint32_t>(w.spin_position)&65535U)|(error?65536U:0U);}
        if(m==0x469){const auto old=w.spin_buddy;w.spin_buddy=gui_.windows.contains(msg.wparam)?msg.wparam:0;return update(old);}
        if(m==0x46a)return w.spin_buddy;
        if(m==0x46d){if(wp!=10 && wp!=16)return 0;const auto old=w.spin_base;w.spin_base=wp;set_position(w.spin_position);return update(old);}
        if(m==0x46e)return w.spin_base;
        if(m==0x46f){w.spin_min=static_cast<std::int32_t>(wp);w.spin_max=static_cast<std::int32_t>(lp);set_position(w.spin_position);return update(0);}
        if(m==0x470){if(msg.wparam)memory.store(GuestAddress{msg.wparam},32,static_cast<std::uint32_t>(w.spin_min));if(lp)memory.store(GuestAddress{lp},32,static_cast<std::uint32_t>(w.spin_max));return 0;}
    }
    if(m==0x30){w.font=msg.wparam;if(lp && ((gui_control_class(w)==u"button" && (w.style&15U)==11) || cls==u"msctls_statusbar32"))invalidate_window(msg.window);return update(0);} // WM_SETFONT
    if(m==0x31)return w.font;
    if(m==0xb){w.redraw=msg.wparam!=0;return update(0);}
    if(m==0x80){if(wp>1 || (lp && !gui_.icons.contains(lp)))return 0;auto& icon=wp?w.window_icon:w.window_small_icon;const auto previous=icon;icon=lp;return update(previous);}
    if(m==0x7f)return wp==1?w.window_icon:wp==0 || wp==2?w.window_small_icon:0;
    if(w.dialog && m==0x401){w.default_id=msg.wparam;return 1;}
    if(w.dialog && m==0x400)return w.default_id?(0x534b0000|w.default_id):0;
    if(!w.builtin)return std::nullopt;
    if(cls==u"combobox" && (m==5 || m==3)){present_gui(msg.window);return 0;}
    if(cls==u"edit"){if(const auto input=gui_edit_input(memory,msg,wide))return input;}
    if(cls==u"msctls_statusbar32" && (m==0xc || m==0xd || m==0xe)){
        auto forwarded=msg;forwarded.wparam=0;
        if(m==0xc)forwarded.message=wide?0x40bU:0x401U;
        else if(m==0xe)forwarded.message=wide?0x40cU:0x403U;
        else {if(w.status_parts.empty())return 0;const auto& part=w.status_parts[0];if(part.flags&0x1000U)return part.data;return write(lp,part.text,wp);}
        return gui_control_message(memory,forwarded,wide);
    }
    if(m==0xc){w.title=read(lp);if(gui_control_class(w)==u"comboboxex32"){w.combo_edit_changed=false;w.combo_edit_item.text=w.title;}if(gui_control_class(w)==u"button" && (w.style&15U)==11)invalidate_window(msg.window);return update(1);}
    if(m==0xd)return write(lp,w.title,wp);
    if(m==0xe)return wide?w.title.size():utf16_to_utf8(w.title).size();
    if(m==0x87){ // WM_GETDLGCODE；owner-draw 不能被當成普通 default pushbutton 改寫樣式。
        if(cls==u"msctls_trackbar32")return 1;
        if(cls==u"edit")return 0x89U|(((w.calendar_owner || w.header_owner) && (wp==13 || wp==27))?4U:0U);
        if(cls==u"button"){switch(w.style&15U){case 0:case 8:return 0x2020;case 1:return 0x2010;case 4:case 9:return 0x2040;case 7:return 0x100;default:return 0x2000;}}
        return 0;
    }
    if(gui_control_class(w)==u"comboboxex32"){
        if(m==5){if((w.style&3U)!=1U)w.height=24;return update(0);}
        if(m==0x406)return gui_.windows.contains(w.combo_control)?w.combo_control:0;
        if(m==0x407)return gui_.windows.contains(w.edit_control)?w.edit_control:0;
        if(m==0x402){const auto old=w.list_image_lists[0];if(lp && !gui_.image_lists.contains(lp))return 0;w.list_image_lists[0]=lp;return update(old);}
        if(m==0x403)return w.list_image_lists[0];
        if(m==0x408 || m==0x40e){const auto old=w.combo_extended_style;w.combo_extended_style=m==0x408 || !wp?static_cast<std::uint32_t>(lp):(old&~wp)|(static_cast<std::uint32_t>(lp)&wp);return update(old);}
        if(m==0x409)return w.combo_extended_style;
        if(m==0x40a)return w.combo_edit_changed?1:0;
        if(m==0x2005){const auto old=w.list_unicode;w.list_unicode=wp!=0;return old?1:0;}
        if(m==0x2006)return w.list_unicode?1:0;
        if(m==0xc5){w.text_limit=wp?wp:32767;return update(0);}
        if(m==0x401 || m==0x40b || m==0x404 || m==0x40d || m==0x405 || m==0x40c){
            const bool insert=m==0x401 || m==0x40b,get=m==0x404 || m==0x40d,unicode=m==0x40b || m==0x40c || m==0x40d;
            memory.check(GuestAddress{lp},abi.combo_size,Access::Read);if(get)memory.check(GuestAddress{lp},abi.combo_size,Access::Write);
            const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{lp},32));const auto raw_index=memory.load(GuestAddress{lp+abi.combo_index},abi.bits);const auto requested=x86_?static_cast<std::int64_t>(static_cast<std::int32_t>(raw_index)):static_cast<std::int64_t>(raw_index);
            const bool edit_item=!insert && requested==-1;
            const auto index=insert && requested==-1?w.items.size():static_cast<std::size_t>(requested);
            if(!edit_item && (index>(insert?w.items.size():w.items.empty()?0:w.items.size()-1) || (!insert && w.items.empty()) || (insert && w.items.size()>=65536)))return insert?UINT64_MAX:0;
            if(get){
                const auto& item=edit_item?w.combo_edit_item:w.items[index];
                if(mask&1U){if(item.callback_subitems.contains(0))throw HostCallFault("ComboBoxEx 同步文字回呼尚未提供");const auto target=memory.load(GuestAddress{lp+abi.combo_text},abi.bits);const auto capacity=static_cast<std::int32_t>(memory.load(GuestAddress{lp+abi.combo_capacity},32));if(capacity>0){if(unicode)memory.write(GuestAddress{target},utf16_bytes(item.text.substr(0,static_cast<std::size_t>(capacity-1))));else {auto text=utf16_to_utf8(item.text);text.resize(std::min(text.size(),static_cast<std::size_t>(capacity-1)));std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{target},bytes);}}}
                if(mask&2U)memory.store(GuestAddress{lp+abi.combo_image},32,static_cast<std::uint32_t>(item.image));
                if(mask&4U)memory.store(GuestAddress{lp+abi.combo_selected},32,static_cast<std::uint32_t>(item.selected_image));
                if(mask&8U)memory.store(GuestAddress{lp+abi.combo_overlay},32,static_cast<std::uint32_t>(item.overlay));
                if(mask&16U)memory.store(GuestAddress{lp+abi.combo_indent},32,static_cast<std::uint32_t>(item.indent));
                if(mask&32U)memory.store(GuestAddress{lp+abi.combo_data},abi.bits,item.data);return 1;
            }
            GuiItem item=insert?GuiItem{}:edit_item?w.combo_edit_item:w.items[index];if(insert)item.handle=gui_.next_item++;
            if(mask&1U){const auto pointer=memory.load(GuestAddress{lp+abi.combo_text},abi.bits);if(pointer==(x86_?UINT32_MAX:UINT64_MAX)){item.callback_subitems.insert(0);item.text.clear();}else {item.callback_subitems.erase(0);item.text=pointer?(unicode?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer))):std::u16string{};}}
            if(mask&2U)item.image=static_cast<std::int32_t>(memory.load(GuestAddress{lp+abi.combo_image},32));
            if(mask&4U)item.selected_image=static_cast<std::int32_t>(memory.load(GuestAddress{lp+abi.combo_selected},32));
            if(mask&8U)item.overlay=static_cast<std::int32_t>(memory.load(GuestAddress{lp+abi.combo_overlay},32));
            if(mask&16U)item.indent=static_cast<std::int32_t>(memory.load(GuestAddress{lp+abi.combo_indent},32));
            if(mask&32U)item.data=memory.load(GuestAddress{lp+abi.combo_data},abi.bits);
            if(insert){w.items.insert(w.items.begin()+static_cast<std::ptrdiff_t>(index),std::move(item));if(w.selection>=static_cast<std::int32_t>(index))++w.selection;}
            else if(edit_item){w.combo_edit_item=std::move(item);if(mask&1U){w.title=w.combo_edit_item.text;w.combo_edit_changed=false;}}
            else {w.items[index]=std::move(item);if(w.selection==static_cast<std::int32_t>(index))w.title=w.items[index].text;}
            return update(insert?index:1);
        }
    }
    if(gui_control_class(w)==u"toolbarwindow32"){
        if(m==1)return update(0);
        const auto command=[&](std::uint32_t id){return std::find_if(w.toolbar_buttons.begin(),w.toolbar_buttons.end(),[&](const auto& button){return static_cast<std::uint32_t>(button.command)==id;});};
        const auto string=[&](std::uint64_t pointer,bool unicode){return unicode?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16_lossy(read_ansi(memory,pointer,32768));};
        const auto refresh_strings=[&]{for(auto& button:w.toolbar_buttons)if(button.string_value<w.toolbar_strings.size())button.text=w.toolbar_strings[static_cast<std::size_t>(button.string_value)];};
        const auto button_at=[&](std::uint64_t address,bool unicode){
            memory.check(GuestAddress{address},abi.toolbar_size,Access::Read);GuiToolbarButton button;
            button.image=static_cast<std::int32_t>(memory.load(GuestAddress{address},32));button.command=static_cast<std::int32_t>(memory.load(GuestAddress{address+4},32));
            button.state=static_cast<std::uint8_t>(memory.load(GuestAddress{address+8},8));button.style=static_cast<std::uint8_t>(memory.load(GuestAddress{address+9},8));button.data=memory.load(GuestAddress{address+abi.toolbar_data},abi.bits);
            button.string_value=memory.load(GuestAddress{address+abi.toolbar_string},abi.bits);
            if(button.string_value>65535 && button.string_value!=(x86_?UINT32_MAX:UINT64_MAX))button.text=string(button.string_value,unicode);
            else if(button.string_value<w.toolbar_strings.size())button.text=w.toolbar_strings[static_cast<std::size_t>(button.string_value)];
            return button;
        };
        if(m==0x41e){if(wp!=abi.toolbar_size){last_error_=87;return 0;}w.toolbar_struct_size=wp;return 0;}
        if(m==0x418)return w.toolbar_buttons.size();
        if(m==0x419){const auto button=command(wp);return button==w.toolbar_buttons.end()?UINT64_MAX:static_cast<std::uint64_t>(button-w.toolbar_buttons.begin());}
        if(m==0x414 || m==0x444 || m==0x415 || m==0x443){
            const bool insert=m==0x415 || m==0x443;const auto count=insert?1U:wp;
            if(count>4096-w.toolbar_buttons.size())return 0;
            std::vector<GuiToolbarButton> buttons;buttons.reserve(count);for(std::uint32_t n=0;n<count;++n)buttons.push_back(button_at(lp+n*abi.toolbar_size,m==0x444 || m==0x443));
            if(insert)w.toolbar_buttons.insert(w.toolbar_buttons.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(wp,w.toolbar_buttons.size())),buttons.front());
            else w.toolbar_buttons.insert(w.toolbar_buttons.end(),buttons.begin(),buttons.end());return update(1);
        }
        if(m==0x416){if(wp>=w.toolbar_buttons.size())return 0;w.toolbar_buttons.erase(w.toolbar_buttons.begin()+wp);return update(1);}
        if(m==0x417){if(wp>=w.toolbar_buttons.size())return 0;const auto& button=w.toolbar_buttons[wp];memory.check(GuestAddress{lp},abi.toolbar_size,Access::Write);memory.write(GuestAddress{lp},std::vector<std::uint8_t>(abi.toolbar_size));
            memory.store(GuestAddress{lp},32,static_cast<std::uint32_t>(button.image));memory.store(GuestAddress{lp+4},32,static_cast<std::uint32_t>(button.command));memory.store(GuestAddress{lp+8},8,button.state);memory.store(GuestAddress{lp+9},8,button.style);memory.store(GuestAddress{lp+abi.toolbar_data},abi.bits,button.data);memory.store(GuestAddress{lp+abi.toolbar_string},abi.bits,button.string_value);return 1;}
        if((m>=0x401 && m<=0x405) || (m>=0x409 && m<=0x40d) || m==0x411 || m==0x412){
            auto button=command(wp);if(button==w.toolbar_buttons.end())return m==0x412?UINT64_MAX:0;
            if(m==0x412)return button->state;
            const auto state_changed=[&](std::uint8_t value)->std::optional<std::uint64_t>{const auto previous=button->state;if(previous==value)return std::uint64_t{1};button->state=value;
                if((previous^value)&0x28U)return update(1); // HIDDEN／WRAP 會改變幾何。
                if(native_gui_)present_native_window(msg.window,w);return std::uint64_t{1};
            };
            if(m==0x411){if(lp&0xffff0000U)return 0;return state_changed(static_cast<std::uint8_t>(lp));}
            constexpr std::array<std::uint8_t,5> masks{4,1,2,8,16};const bool get=m>=0x409;const auto mask=masks[m-(get?0x409:0x401)];
            if(get)return (button->state&mask)?1:0;return state_changed(static_cast<std::uint8_t>((lp&65535U)?button->state|mask:button->state&~mask));
        }
        if(m==0x43f || m==0x440 || m==0x441 || m==0x442){
            const bool get=m==0x43f || m==0x441,unicode=m==0x43f || m==0x440;
            if(memory.load(GuestAddress{lp},32)!=abi.toolbar_info_size)return get?UINT64_MAX:0;const auto mask=memory.load(GuestAddress{lp+4},32);
            const auto button=(mask&0x80000000U)?(wp<w.toolbar_buttons.size()?w.toolbar_buttons.begin()+wp:w.toolbar_buttons.end()):command(wp);
            if(button==w.toolbar_buttons.end())return get?UINT64_MAX:0;memory.check(GuestAddress{lp},abi.toolbar_info_size,get?Access::Write:Access::Read);
            const auto field=[&](unsigned offset,std::uint8_t bits,std::uint64_t value){if(get){memory.store(GuestAddress{lp+offset},bits,value);return value;}return memory.load(GuestAddress{lp+offset},bits);};
            if(mask&1)button->image=static_cast<std::int32_t>(field(12,32,static_cast<std::uint32_t>(button->image)));
            if(mask&2){const auto pointer=memory.load(GuestAddress{lp+abi.toolbar_info_text},abi.bits);if(get){const bool previous=wide;wide=unicode;write(pointer,button->text,memory.load(GuestAddress{lp+abi.toolbar_info_capacity},32));wide=previous;}else {button->text=pointer?string(pointer,unicode):std::u16string{};button->string_value=pointer;}}
            if(mask&4)button->state=static_cast<std::uint8_t>(field(16,8,button->state));if(mask&8)button->style=static_cast<std::uint8_t>(field(17,8,button->style));
            if(mask&16)button->data=field(abi.toolbar_info_data,abi.bits,button->data);if(mask&32)button->command=static_cast<std::int32_t>(field(8,32,static_cast<std::uint32_t>(button->command)));
            if(mask&64){if(get)memory.store(GuestAddress{lp+18},16,button->width?button->width:w.toolbar_button_width);else {const auto width=memory.load(GuestAddress{lp+18},16);if(width>4096)return 0;button->width=static_cast<std::uint32_t>(width);}}
            return get?static_cast<std::uint64_t>(button-w.toolbar_buttons.begin()):update(1);
        }
        if(m==0x413){
            const auto instance=memory.load(GuestAddress{lp},abi.bits),resource=memory.load(GuestAddress{lp+abi.unit},abi.bits);std::optional<GuiBitmap> bitmap;
            if(!instance){sync_raster_bitmap(memory,resource);const auto source=gui_.bitmaps.find(resource);if(source!=gui_.bitmaps.end())bitmap=source->second;}
            else if(instance!=(x86_?UINT32_MAX:UINT64_MAX)){const auto entry=gui_resource(memory,instance,resource,2,true);const auto* image=module_image(instance);if(entry && image){const auto bytes=memory.load(GuestAddress{entry+4},32);if(bytes>16U*1024U*1024U)throw HostCallFault("工具列 bitmap 超過上限");std::vector<std::uint8_t> contents(static_cast<std::size_t>(bytes));memory.read(GuestAddress{image->base+memory.load(GuestAddress{entry},32)},contents);bitmap=decode_dib_bitmap(contents);}}
            if(instance==(x86_?UINT32_MAX:UINT64_MAX)){
                auto stock=stock_toolbar_images(resource);if(!stock)return UINT64_MAX;
                if(!gui_.image_lists.contains(w.list_image_lists[0])){const auto handle=gui_.next_object++;w.list_image_lists[0]=handle;gui_.image_lists.emplace(handle,GuiImageList{stock->width,stock->height});}
                auto& list=gui_.image_lists.at(w.list_image_lists[0]);if(list.width!=stock->width || list.height!=stock->height)return UINT64_MAX;
                const auto first=list.images.size();list.images.insert(list.images.end(),stock->images.begin(),stock->images.end());return update(first);
            }
            if(!bitmap)return UINT64_MAX;
            if(!gui_.image_lists.contains(w.list_image_lists[0])){GuiImageList list;list.width=static_cast<std::int32_t>(w.toolbar_bitmap_width);list.height=static_cast<std::int32_t>(w.toolbar_bitmap_height);const auto handle=gui_.next_object++;gui_.image_lists.emplace(handle,std::move(list));w.list_image_lists[0]=handle;}
            auto& list=gui_.image_lists.at(w.list_image_lists[0]);const auto first=bitmap_images(list,*bitmap,nullptr,bitmap->pixels.front(),false,-1);return update(first);
        }
        if(m==0x43b){const auto minimum=static_cast<std::uint16_t>(lp),maximum=static_cast<std::uint16_t>(lp>>16);if(maximum<minimum || maximum>4096)return 0;w.toolbar_min_width=minimum;w.toolbar_max_width=maximum;return update(1);}
        if(m==0x43c){if(wp>1)return 0;w.toolbar_text_rows=wp;return update(1);}
        if(m==0x43d)return w.toolbar_text_rows;
        if(m==0x453){memory.check(GuestAddress{lp},8,Access::Write);memory.store(GuestAddress{lp},32,w.toolbar_content_width);memory.store(GuestAddress{lp+4},32,w.toolbar_content_height);return 1;}
        if(m==0x41c || m==0x44d){
            std::vector<std::u16string> strings;
            if(msg.wparam){ // RT_STRING 可包含以分隔字元編排的工具列標籤。
                const auto resource=gui_resource(memory,msg.wparam,(lp>>4)+1,6,true);if(!resource)return UINT64_MAX;const auto* image=module_image(msg.wparam);if(!image)return UINT64_MAX;
                auto address=image->base+memory.load(GuestAddress{resource},32);const auto end=address+memory.load(GuestAddress{resource+4},32);
                for(std::uint64_t i=0;i<=(lp&15);++i){if(address>end || end-address<2)throw HostCallFault("工具列字串資源截斷");const auto count=memory.load(GuestAddress{address},16);address+=2;if(count>(end-address)/2)throw HostCallFault("工具列字串資源截斷");if(i==(lp&15)){std::u16string text;for(std::uint64_t n=0;n<count;++n)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{address+n*2},16)));strings.push_back(std::move(text));}address+=count*2;}
                if(!strings.front().empty()){const auto text=strings.front();const auto delimiter=text.front();strings.clear();std::size_t start=1;for(std::size_t n=1;n<=text.size();++n)if(n==text.size() || text[n]==delimiter){if(n>start)strings.push_back(text.substr(start,n-start));start=n+1;}}
            }else{
                auto address=lp;for(unsigned n=0;n<4096;++n){const auto text=string(address,m==0x44d);if(text.empty())break;strings.push_back(text);address+=(m==0x44d?text.size()*2:utf16_to_utf8(text).size())+(m==0x44d?2:1);if(n==4095)throw HostCallFault("工具列字串清單未終止");}
            }
            if(strings.size()>65536-w.toolbar_strings.size())return UINT64_MAX;const auto first=w.toolbar_strings.size();w.toolbar_strings.insert(w.toolbar_strings.end(),strings.begin(),strings.end());refresh_strings();return update(first);
        }
        if(m==0x42d || m==0x44b){const auto button=command(wp);if(button==w.toolbar_buttons.end())return UINT64_MAX;const bool unicode=m==0x44b;const auto size=unicode?button->text.size():utf16_to_utf8(button->text).size();if(lp){const bool previous=wide;wide=unicode;write(lp,button->text,size+1);wide=previous;}return size;}
        if(m==0x41f || m==0x420){const auto width=static_cast<std::uint16_t>(lp),height=static_cast<std::uint16_t>(lp>>16);if(!width || !height || width>4096 || height>4096)return 0;if(m==0x41f){w.toolbar_requested_width=width;w.toolbar_requested_height=height;}else {w.toolbar_bitmap_width=width;w.toolbar_bitmap_height=height;}return update(1);}
        if(m==0x43a)return w.toolbar_button_width|(static_cast<std::uint64_t>(w.toolbar_button_height)<<16);
        if(m==0x430 || m==0x436 || m==0x434){const auto index=m==0x430?0U:m==0x436?1U:2U;const auto previous=w.list_image_lists[index];if(lp && !gui_.image_lists.contains(lp))return 0;w.list_image_lists[index]=lp;return update(previous);}
        if(m==0x431 || m==0x437 || m==0x435)return w.list_image_lists[m==0x431?0U:m==0x437?1U:2U];
        if(m==0x438){w.style=static_cast<std::uint32_t>(lp);return update(0);}if(m==0x439)return w.style;
        if(m==0x421 || m==5){const auto parent=gui_.windows.find(w.parent);if(parent!=gui_.windows.end() && !(w.style&0x8U)){w.width=parent->second.width;w.x=0;if((w.style&3U)!=2U)w.y=0;}layout_gui_toolbar(w);if(!(w.style&0x4U))w.height=w.toolbar_content_height+4;return update(0);}
        if(m==0x41d || m==0x433){const auto button=m==0x433?command(wp):wp<w.toolbar_buttons.size()?w.toolbar_buttons.begin()+wp:w.toolbar_buttons.end();if(button==w.toolbar_buttons.end())return 0;memory.check(GuestAddress{lp},16,Access::Write);for(unsigned n=0;n<4;++n)memory.store(GuestAddress{lp+n*4},32,static_cast<std::uint32_t>(button->rectangle[n]));return 1;}

    }
    if(gui_control_class(w)==u"rebarwindow32"){
        if(m==0x111){auto message=msg;message.window=w.parent;enqueue_gui_message(std::move(message));return 0;}
        const auto layout=[&]{
            std::uint32_t x=0,y=0,row_height=0;const auto old_height=w.height;
            for(auto& band:w.rebar_bands){
                const auto child=gui_.windows.find(band.child);const bool visible=!(band.style&8U);if(!visible){if(child!=gui_.windows.end()){child->second.visible=false;present_gui(band.child);}continue;}
                auto width=std::max(band.width,band.min_width+band.header),height=std::max(band.min_height,band.height);
                if(!height && child!=gui_.windows.end())height=child->second.height;
                if(x && ((band.style&1U) || band.min_width+band.header>w.width-x)){y+=row_height;x=0;row_height=0;}
                width=std::min(width,w.width-x);band.x=static_cast<std::int32_t>(x);band.y=static_cast<std::int32_t>(y);
                if(child!=gui_.windows.end()){auto& control=child->second;control.parent=msg.window;control.visible=true;control.x=static_cast<std::int32_t>(x+std::min(width,band.header));control.y=static_cast<std::int32_t>(y);control.width=width-std::min(width,band.header);control.height=height;present_gui(band.child);}
                x+=width;row_height=std::max(row_height,height);
            }
            w.height=y+row_height;present_gui(msg.window);
            if(old_height!=w.height)list_notification(gui_,memory,[&]{return runtime_allocate(memory,{},notify_bytes);},msg.window,-831,-1,0);
        };
        if(m==0x40c)return w.rebar_bands.size();if(m==0x41b)return w.height;
        if(m==0x40d){std::set<std::int32_t> rows;for(const auto& band:w.rebar_bands)if(!(band.style&8U))rows.insert(band.y);return rows.size();}
        if(m==0x40e){if(wp>=w.rebar_bands.size())return 0;const auto& band=w.rebar_bands[wp];return std::max(band.height,band.min_height);}
        if(m==0x410){for(std::size_t n=0;n<w.rebar_bands.size();++n)if(w.rebar_bands[n].id==wp)return n;return UINT64_MAX;}
        if(m==0x401 || m==0x40a || m==0x406 || m==0x40b || m==0x41c || m==0x41d){
            const bool insert=m==0x401 || m==0x40a,get=m==0x41c || m==0x41d,unicode=m==0x40a || m==0x40b || m==0x41c;
            if((!insert && wp>=w.rebar_bands.size()) || (insert && w.rebar_bands.size()>=4096))return 0;
            const auto size=memory.load(GuestAddress{lp},32),mask=memory.load(GuestAddress{lp+4},32);if(size<abi.rebar_id+4 || size>(x86_?100U:128U))return 0;
            if(((mask&512) && size<abi.rebar_ideal+4) || ((mask&1024) && size<abi.rebar_data+abi.unit) || ((mask&2048) && size<abi.rebar_header+4))return 0;
            memory.check(GuestAddress{lp},static_cast<std::size_t>(size),get?Access::Write:Access::Read);
            auto band=insert?GuiRebarBand{}:w.rebar_bands[wp];
            const auto field=[&](unsigned at,unsigned bits,std::uint64_t value){if(at+bits/8>size)throw HostCallFault("REBARBANDINFO 欄位超出 cbSize");if(get){memory.store(GuestAddress{lp+at},static_cast<std::uint8_t>(bits),value);return value;}return memory.load(GuestAddress{lp+at},static_cast<std::uint8_t>(bits));};
            if(mask&1)band.style=static_cast<std::uint32_t>(field(8,32,band.style));
            if(mask&2){band.foreground=static_cast<std::uint32_t>(field(12,32,band.foreground));band.background=static_cast<std::uint32_t>(field(16,32,band.background));}
            if(mask&4){const auto pointer=memory.load(GuestAddress{lp+abi.rebar_text},abi.bits);if(get){const auto capacity=memory.load(GuestAddress{lp+abi.rebar_capacity},32);const bool previous=wide;wide=unicode;write(pointer,band.text,capacity);wide=previous;}else band.text=pointer?(unicode?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer))):std::u16string{};}
            if(mask&8)band.image=static_cast<std::int32_t>(field(abi.rebar_image,32,static_cast<std::uint32_t>(band.image)));
            if(mask&16){band.child=field(abi.rebar_child,abi.bits,band.child);if(!get && band.child){if(!gui_.windows.contains(band.child))return 0;auto ancestor=msg.window;for(unsigned depth=0;ancestor && depth<64;++depth){if(ancestor==band.child)return 0;const auto node=gui_.windows.find(ancestor);ancestor=node==gui_.windows.end()?0:node->second.parent;}if(ancestor)return 0;}}
            if(mask&32){band.min_width=static_cast<std::uint32_t>(field(abi.rebar_min_width,32,band.min_width));band.min_height=static_cast<std::uint32_t>(field(abi.rebar_min_height,32,band.min_height));if(size>=abi.rebar_integral+4){band.height=static_cast<std::uint32_t>(field(abi.rebar_height,32,band.height));band.max_height=static_cast<std::uint32_t>(field(abi.rebar_max_height,32,band.max_height));band.integral=static_cast<std::uint32_t>(field(abi.rebar_integral,32,band.integral));}}
            if(mask&64)band.width=static_cast<std::uint32_t>(field(abi.rebar_width,32,band.width));if(mask&128)band.bitmap=field(abi.rebar_bitmap,abi.bits,band.bitmap);if(mask&256)band.id=static_cast<std::uint32_t>(field(abi.rebar_id,32,band.id));
            if(mask&512)band.ideal=static_cast<std::uint32_t>(field(abi.rebar_ideal,32,band.ideal));if(mask&1024)band.data=field(abi.rebar_data,abi.bits,band.data);if(mask&2048)band.header=static_cast<std::uint32_t>(field(abi.rebar_header,32,band.header));
            if(get)return 1;
            if(std::max({band.width,band.height,band.min_width,band.min_height,band.header})>65535)return 0;
            if(insert)w.rebar_bands.insert(w.rebar_bands.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(wp,w.rebar_bands.size())),std::move(band));else w.rebar_bands[wp]=std::move(band);layout();return 1;
        }
        if(m==0x402){if(wp>=w.rebar_bands.size())return 0;const auto child=w.rebar_bands[wp].child;if(gui_.windows.contains(child)){gui_.windows.at(child).visible=false;present_gui(child);}w.rebar_bands.erase(w.rebar_bands.begin()+wp);layout();return 1;}
        if(m==0x423){if(wp>=w.rebar_bands.size())return 0;auto& band=w.rebar_bands[wp];band.style=lp?band.style&~8U:band.style|8U;layout();return 1;}
        if(m==0x427){if(wp>=w.rebar_bands.size() || lp>=w.rebar_bands.size())return 0;auto band=w.rebar_bands[wp];w.rebar_bands.erase(w.rebar_bands.begin()+wp);w.rebar_bands.insert(w.rebar_bands.begin()+static_cast<std::ptrdiff_t>(lp),std::move(band));layout();return 1;}
        if(m==0x409 || m==0x422){if(wp>=w.rebar_bands.size())return 0;const auto& band=w.rebar_bands[wp];memory.check(GuestAddress{lp},16,Access::Write);const std::array<std::uint32_t,4> rect=m==0x422?std::array<std::uint32_t,4>{band.header,0,0,0}:std::array<std::uint32_t,4>{static_cast<std::uint32_t>(band.x),static_cast<std::uint32_t>(band.y),static_cast<std::uint32_t>(band.x)+std::max(band.width,band.min_width+band.header),static_cast<std::uint32_t>(band.y)+std::max(band.height,band.min_height)};for(unsigned n=0;n<4;++n)memory.store(GuestAddress{lp+n*4},32,rect[n]);return 1;}
        if(m==0x403 || m==0x404){if(memory.load(GuestAddress{lp},32)!=8+abi.unit)return 0;const auto mask=memory.load(GuestAddress{lp+4},32);if(mask&1){if(m==0x403)memory.store(GuestAddress{lp+8},abi.bits,w.list_image_lists[0]);else {const auto handle=memory.load(GuestAddress{lp+8},abi.bits);if(handle && !gui_.image_lists.contains(handle))return 0;w.list_image_lists[0]=handle;present_gui(msg.window);}}return 1;}
        if(m==5 || m==0x417){if(m==0x417){const auto left=static_cast<std::int32_t>(memory.load(GuestAddress{lp},32)),right=static_cast<std::int32_t>(memory.load(GuestAddress{lp+8},32));if(right<left || static_cast<std::int64_t>(right)-left>65535)return 0;w.width=static_cast<std::uint32_t>(right-left);}else if(const auto parent=gui_.windows.find(w.parent);parent!=gui_.windows.end())w.width=parent->second.width;layout();return 1;}
        if(m==0x2005){const auto previous=w.list_unicode;w.list_unicode=wp!=0;return previous?1:0;}if(m==0x2006)return w.list_unicode?1:0;
    }
    if(cls==u"msctls_statusbar32"){
        const auto changed=[&](std::uint64_t result){invalidate_window(msg.window);return update(result);};
        const auto part_at=[&](std::uint32_t index)->GuiStatusPart*{return index==255?&w.status_simple_part:index<w.status_parts.size()?&w.status_parts[index]:nullptr;};
        const auto layout=[&]{
            if(w.style&0x4U)return; // CCS_NORESIZE 保留呼叫者指定的尺寸。
            const auto font=gui_font(gui_,w.font);const auto metrics=native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
            w.height=std::min<std::uint32_t>(4096,static_cast<std::uint32_t>(std::max<std::int64_t>(metrics.height+4,static_cast<std::int64_t>(w.status_min_height)+2)));
            if(const auto parent=gui_.windows.find(w.parent);parent!=gui_.windows.end() && !(w.style&0x8U)){
                w.width=parent->second.width;w.x=0;
                if((w.style&3U)!=2U)w.y=(w.style&3U)==1?0:static_cast<std::int32_t>(parent->second.height)-static_cast<std::int32_t>(w.height);
            }
        };
        if(m==1){if(w.status_parts.empty())w.status_parts.push_back({w.title,-1,0});for(auto& part:w.status_parts)if(!part.identity)part.identity=gui_.next_item++;layout();return changed(0);}
        if(m==0x404){
            if(!wp || wp>256 || !lp)return 0;auto parts=w.status_parts;parts.resize(wp);std::int32_t previous=-1;
            for(std::uint32_t n=0;n<wp;++n){const auto edge=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+n*4},32)));
                if(edge<-1 || (edge==-1 && n+1!=wp) || (edge!=-1 && edge<previous))return 0;parts[n].edge=edge;previous=edge;if(!parts[n].identity)parts[n].identity=gui_.next_item++;}
            w.status_parts=std::move(parts);return changed(1);
        }
        if(m==0x406){if(lp)memory.check(GuestAddress{lp},std::min<std::size_t>(wp,w.status_parts.size())*4,Access::Write);for(std::size_t n=0;lp && n<std::min<std::size_t>(wp,w.status_parts.size());++n)memory.store(GuestAddress{lp+n*4},32,static_cast<std::uint32_t>(w.status_parts[n].edge));return w.status_parts.size();}
        if(m==0x401 || m==0x40b){
            const auto index=wp&255U;auto* part=part_at(index);if(!part || (index==255 && (wp&0x1000U)))return 0;
            const auto flags=wp&0xff00U;const auto text=(flags&0x1000U)?std::u16string{}:lp?(m==0x40b?read_utf16(memory,GuestAddress{lp},65535):utf8_to_utf16_lossy(read_ansi(memory,lp,65535))):std::u16string{};
            if(!(flags&0x1000U) && part->flags==flags && part->text==text)return 1; // owner-draw 的資料指標相同仍可能有新內容。
            part->text=text;part->flags=flags;part->data=(flags&0x1000U)?lp:0;return changed(1);
        }
        if(m==0x402 || m==0x403 || m==0x40c || m==0x40d){
            const auto* part=part_at(wp&255U);if(!part)return 0;const bool getting=m==0x402 || m==0x40d;
            if(getting && (part->flags&0x1000U))return part->data;
            const bool unicode=m==0x40c || m==0x40d;const auto size=std::min<std::size_t>(65535,unicode?part->text.size():utf16_to_utf8(part->text).size());
            if(getting && lp){const bool previous=wide;wide=unicode;write(lp,part->text,size+1);wide=previous;}return size|(static_cast<std::uint64_t>(part->flags)<<16);
        }
        if(m==0x409){if(w.status_simple==(wp!=0))return 0;w.status_simple=wp!=0;return changed(0);}if(m==0x40e)return w.status_simple?1:0;
        if(m==0x408){w.status_min_height=std::min(wp,4096U);return 0;}
        if(m==5){layout();return changed(0);}
        if(m==0x407){memory.check(GuestAddress{lp},12,Access::Write);for(unsigned n=0;n<3;++n)memory.store(GuestAddress{lp+n*4},32,1);return 1;}
        if(m==0x40a){if(wp>=w.status_parts.size() || !lp)return 0;const auto rect=gui_status_rectangle(w,wp);memory.check(GuestAddress{lp},16,Access::Write);for(unsigned n=0;n<4;++n)memory.store(GuestAddress{lp+n*4},32,static_cast<std::uint32_t>(rect[n]));return 1;}
        if(m==0x40f || m==0x414){auto* part=part_at(wp);if(!part)return m==0x414?UINT64_MAX:0;if(m==0x414)return part->icon_handle;if(lp && !gui_.icons.contains(lp))return 0;part->icon_handle=lp;part->icon=lp?gui_.icons.at(lp):GuiIcon{};return changed(1);}
        if(m>=0x410 && m<=0x413){
            auto* part=part_at(wp&65535U);if(!part)return 0;const bool unicode=m==0x411 || m==0x413;
            if(m==0x410 || m==0x411){part->tooltip=lp?(unicode?read_utf16(memory,GuestAddress{lp}):utf8_to_utf16_lossy(read_ansi(memory,lp))):std::u16string{};return update(0);}
            const bool previous=wide;wide=unicode;if(lp)write(lp,part->tooltip,wp>>16);wide=previous;return 0;
        }
        if(m==0x2001){const auto previous=w.status_background;w.status_background=static_cast<std::uint32_t>(lp);return changed(previous);}
        if(m==0x2005){const bool previous=w.list_unicode;w.list_unicode=wp!=0;return previous?1:0;}if(m==0x2006)return w.list_unicode?1:0;
    }
    if(cls==u"msctls_trackbar32"){
        const auto signed32=[](std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));};
        const auto ticks=[&]{
            auto values=w.track_ticks;
            if(!w.track_ticks_cleared && (w.style&1U)){
                const auto count=(static_cast<std::int64_t>(w.track_max)-w.track_min-1)/std::max(1,w.track_tick_frequency);
                if(count>1048576)throw HostCallFault("Trackbar 刻度數超過可配置上限");
                for(std::int64_t n=1;n<=count;++n)values.push_back(static_cast<std::int32_t>(w.track_min+n*w.track_tick_frequency));
            }
            std::sort(values.begin(),values.end());return values;
        };
        const auto clamp=[&]{w.track_max=std::max(w.track_min,w.track_max);w.track_position=std::clamp(w.track_position,w.track_min,w.track_max);};
        const auto page=[&]{return w.track_page?w.track_page:static_cast<std::int32_t>(std::max<std::int64_t>(1,(static_cast<std::int64_t>(w.track_max)-w.track_min)/5));};
        if(m==0x400)return static_cast<std::uint32_t>(w.track_position);
        if(m==0x401)return static_cast<std::uint32_t>(w.track_min);
        if(m==0x402)return static_cast<std::uint32_t>(w.track_max);
        if(m==0x405){w.track_position=signed32(lp);clamp();return wp?update(0):0;}
        if(m==0x406 || m==0x407 || m==0x408){
            if(m==0x406){w.track_min=static_cast<std::int16_t>(lp);w.track_max=static_cast<std::int16_t>(lp>>16);}
            else if(m==0x407)w.track_min=signed32(lp);else w.track_max=signed32(lp);
            clamp();return wp?update(0):0;
        }
        if(m==0x403){const auto values=ticks();return wp<values.size()?static_cast<std::uint32_t>(values[wp]):UINT32_MAX;}
        if(m==0x404){const auto value=signed32(lp);if(value<w.track_min || value>w.track_max)return 0;w.track_ticks.push_back(value);std::sort(w.track_ticks.begin(),w.track_ticks.end());w.track_ticks_cleared=false;return update(1);}
        if(m==0x409){w.track_ticks.clear();w.track_ticks_cleared=true;return wp?update(0):0;}
        if(m==0x410){if(w.track_ticks_cleared || (w.style&16U))return 0;return 2+ticks().size();}
        if(m==0x414){w.track_tick_frequency=std::max(1,signed32(msg.wparam));w.track_ticks_cleared=false;return update(0);}
        if(m==0x415 || m==0x417){auto& value=m==0x415?w.track_page:w.track_line;const auto old=m==0x415?page():value;value=std::max(0,signed32(lp));return static_cast<std::uint32_t>(old);}
        if(m==0x416)return static_cast<std::uint32_t>(page());if(m==0x418)return static_cast<std::uint32_t>(w.track_line);
        if(m==0x100 || m==0x101){
            std::int64_t value=w.track_position;unsigned code=8;
            if(m==0x100){switch(wp){case 37:case 38:value-=w.track_line;code=0;break;case 39:case 40:value+=w.track_line;code=1;break;case 33:value-=page();code=2;break;case 34:value+=page();code=3;break;case 36:value=w.track_min;code=6;break;case 35:value=w.track_max;code=7;break;default:return 0;}}
            else if(wp<33 || wp>40)return 0;
            w.track_position=static_cast<std::int32_t>(std::clamp(value,static_cast<std::int64_t>(w.track_min),static_cast<std::int64_t>(w.track_max)));
            enqueue_gui_message({w.parent,code,msg.window,(w.style&2U)?0x115U:0x114U});return update(0);
        }
    }
    if(gui_control_class(w)==u"msctls_progress32"){
        const auto signed32=[](std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));};
        const auto previous=static_cast<std::uint32_t>(w.progress_position);
        if(m==0x401 || m==0x406){
            const auto range=static_cast<std::uint16_t>(w.progress_min)|(static_cast<std::uint32_t>(static_cast<std::uint16_t>(w.progress_max))<<16);
            w.progress_min=m==0x401?static_cast<std::uint16_t>(lp):signed32(msg.wparam);
            w.progress_max=std::max(w.progress_min,m==0x401?static_cast<std::int32_t>(static_cast<std::uint16_t>(lp>>16)):signed32(lp));
            w.progress_position=std::clamp(w.progress_position,w.progress_min,w.progress_max);return update(range);
        }
        if(m==0x402 || m==0x403){const auto value=m==0x402?static_cast<std::int64_t>(signed32(msg.wparam)):static_cast<std::int64_t>(w.progress_position)+signed32(msg.wparam);w.progress_position=static_cast<std::int32_t>(std::clamp(value,static_cast<std::int64_t>(w.progress_min),static_cast<std::int64_t>(w.progress_max)));return update(previous);}
        if(m==0x404){const auto step=w.progress_step;w.progress_step=signed32(msg.wparam);return static_cast<std::uint32_t>(step);}
        if(m==0x405){
            auto value=static_cast<std::int64_t>(w.progress_position)+w.progress_step;const auto extent=static_cast<std::int64_t>(w.progress_max)-w.progress_min;
            if(!extent)value=w.progress_min;
            else if(value>w.progress_max)value=w.progress_min+(value-w.progress_min-1)%extent+1;
            else if(value<w.progress_min)value=w.progress_max-(w.progress_max-value-1)%extent-1;
            w.progress_position=static_cast<std::int32_t>(value);return update(previous);
        }
        if(m==0x407){if(lp){memory.check(GuestAddress{lp},8,Access::Write);memory.store(GuestAddress{lp},32,static_cast<std::uint32_t>(w.progress_min));memory.store(GuestAddress{lp+4},32,static_cast<std::uint32_t>(w.progress_max));}return static_cast<std::uint32_t>(msg.wparam?w.progress_min:w.progress_max);}
        if(m==0x408)return previous;
        if(m==0x409 || m==0x2001){auto& color=m==0x409?w.progress_bar_color:w.progress_background_color;const auto old=color;color=static_cast<std::uint32_t>(lp);return update(old);}
        if(m==0x40a){w.progress_marquee=msg.wparam!=0;return update(1);}
        if(m==0x40d)return static_cast<std::uint32_t>(w.progress_step);
        if(m==0x40e)return w.progress_background_color;
        if(m==0x40f)return w.progress_bar_color;
        if(m==0x410){const auto old=w.progress_state;if(wp>=1 && wp<=3)w.progress_state=wp;return update(old);}
        if(m==0x411)return w.progress_state;
    }
    if(gui_control_class(w)==u"button"){
        if(m==0xf0)return w.check;
        if(m==0xf1){w.check=wp;return update(0);}
        if(m==0xf2)return (w.check&3U)|(w.button_pressed?4U:0U)|(gui_.focus==msg.window?8U:0U);
        if(m==0xf3){w.button_pressed=wp!=0;if((w.style&15U)==11)invalidate_window(msg.window);return update(0);}
        if(m==0xf4){w.style=(w.style&~0xfU)|(wp&0xfU);if(lp)invalidate_window(msg.window);return update(0);}
        if((w.style&15U)!=7){
            const auto repaint=[&]{invalidate_window(msg.window);return update(0);};
            const auto inside=[&]{const auto x=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(lp)),y=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(lp>>16));return x>=0 && y>=0 && static_cast<std::uint32_t>(x)<w.width && static_cast<std::uint32_t>(y)<w.height;};
            if(m==7 || m==8){if(m==7)gui_.focus=msg.window;else {if(gui_.focus==msg.window)gui_.focus=0;w.button_pressed=false;w.button_armed=false;if(gui_.capture==msg.window)gui_.capture=0;}return repaint();}
            if(m==0x1f || m==0x215 || m==0xa){w.button_pressed=false;w.button_armed=false;if(gui_.capture==msg.window)gui_.capture=0;return repaint();}
            if(!w.enabled && (m==0x201 || m==0x202 || m==0x200 || m==0x100 || m==0x101))return 0;
            if(m==0x201 || (m==0x100 && wp==32)){w.button_armed=true;w.button_pressed=true;gui_.capture=msg.window;return repaint();}
            if(m==0x200 && w.button_armed){const auto pressed=inside();if(w.button_pressed!=pressed){w.button_pressed=pressed;return repaint();}return 0;}
            if(m==0x202 || (m==0x101 && wp==32)){
                w.button_armed=false;w.button_pressed=false;if(gui_.capture==msg.window)gui_.capture=0;return repaint();
            }
        }
    }
    if(gui_control_class(w)==u"edit"){
        if(m==0xc5){w.text_limit=wp?wp:32767;return 0;}
        if(m==0xcc){ // EM_SETPASSWORDCHAR：多行 EDIT 不支援密碼模式。
            if(w.style&4U)return 0;
            w.password_character=static_cast<std::uint16_t>(wp);
            if(w.password_character)w.style|=0x20U;else w.style&=~0x20U;
            return update(0);
        }
        if(m==0xd2)return (w.style&0x20U) && !(w.style&4U)?w.password_character:0;
        if(m==0xb1 || m==0xb7 || m==0xcd)return 0; // 選取、捲動與復原緩衝區。
    }
    if(gui_control_class(w)==u"static" && (w.style&31)==3){
        if(m==1 && lp){
            const auto name=memory.load(GuestAddress{lp+(x86_?36:56)},abi.bits);
            if(name){
                const auto icon=load_gui_icon(memory,w.instance,name,wide,0,0,(w.style&0x800U)!=0);
                if(icon){w.resource_icon=icon;(void)gui_control_message(memory,{msg.window,icon,0,0x170},wide);}
            }return 0;
        }
        if(m==0x171)return w.icon;
        if(m==0x170){
            const auto icon=gui_.icons.find(msg.wparam);if(msg.wparam && icon==gui_.icons.end())return 0;
            const auto previous=w.icon;w.icon=msg.wparam;w.drawing.clear();
            if(icon!=gui_.icons.end() && !(w.style&0x200U)){w.width=icon->second.width;w.height=icon->second.height;}

            if(icon!=gui_.icons.end()){GuiDraw draw;draw.kind=GuiDraw::Kind::Icon;draw.right=static_cast<std::int32_t>(icon->second.width);draw.bottom=static_cast<std::int32_t>(icon->second.height);draw.image=icon->second.image;draw.system_icon=icon->second.system;w.drawing.push_back(std::move(draw));}
            return update(previous);
        }
    }
    if(gui_control_class(w)==u"syslistview32"){
        const auto signed32=[](std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));};
        const auto notify=[&](std::int32_t code,std::int32_t row,std::uint32_t subitem=0,std::uint32_t before=0,std::uint32_t after=0,const GuiItem* removed=nullptr){
            list_notification(gui_,memory,[&]{return runtime_allocate(memory,{},notify_bytes);},msg.window,code,row,subitem,before,after,removed);
        };
        const auto text_read=[&](std::uint64_t pointer,bool unicode){return !pointer?std::u16string{}:unicode?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer));};
        const auto text_write=[&](std::uint64_t pointer,std::u16string_view text,std::int32_t capacity,bool unicode){
            if(capacity<=0)return UINT64_C(0);
            if(unicode){auto size=std::min(text.size(),static_cast<std::size_t>(capacity-1));if(size && size<text.size() && text[size-1]>=0xd800 && text[size-1]<=0xdbff)--size;const auto value=text.substr(0,size);memory.write(GuestAddress{pointer},utf16_bytes(value));return static_cast<std::uint64_t>(value.size());}
            auto value=utf16_to_utf8(text);auto size=std::min(value.size(),static_cast<std::size_t>(capacity-1));while(size && size<value.size() && (static_cast<unsigned char>(value[size])&0xc0U)==0x80U)--size;value.resize(size);std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);memory.write(GuestAddress{pointer},bytes);return static_cast<std::uint64_t>(value.size());
        };
        const auto set_state=[&](std::size_t index,std::uint32_t state,std::uint32_t mask){
            auto& item=w.items[index];const auto before=item.state;item.state=(before&~mask)|(state&mask);
            if((mask&1) && (state&1)){for(std::size_t i=0;i<w.items.size();++i)if(i!=index)w.items[i].state&=~1U;w.selection=static_cast<std::int32_t>(index);}
            else if((mask&1) && w.selection==static_cast<std::int32_t>(index))w.selection=-1;
            if((w.style&4) && (mask&2) && (state&2))for(std::size_t i=0;i<w.items.size();++i)if(i!=index){const auto previous=w.items[i].state;w.items[i].state&=~2U;if(previous!=w.items[i].state)notify(-101,static_cast<std::int32_t>(i),0,previous,w.items[i].state);}
            if(before!=item.state)notify(-101,static_cast<std::int32_t>(index),0,before,item.state);
        };
        if(m==0x2005){const auto old=w.list_unicode;w.list_unicode=wp!=0;return old;}
        if(m==0x2006)return w.list_unicode;
        if(m==0x101f)return w.list_header;
        if(m==0x1004)return w.items.size();
        if(m==0x100a)return w.list_callback_mask;
        if(m==0x100b){w.list_callback_mask=wp;return 1;}
        if(m==0x1002)return wp<w.list_image_lists.size()?w.list_image_lists[wp]:0;
        if(m==0x1003){if(wp>=w.list_image_lists.size())return 0;const auto old=w.list_image_lists[wp];w.list_image_lists[wp]=lp;return update(old);}
        if(m==0x1036){const auto old=w.list_extended_style;w.list_extended_style=wp?(old&~wp)|(static_cast<std::uint32_t>(lp)&wp):static_cast<std::uint32_t>(lp);return update(old);}
        if(m==0x1037)return w.list_extended_style;
        if(m==0x1032)return static_cast<std::uint64_t>(std::count_if(w.items.begin(),w.items.end(),[](const auto& item){return (item.state&2)!=0;}));
        if(m==0x1042)return static_cast<std::uint64_t>(static_cast<std::int64_t>(w.selection));
        if(m==0x1043){const auto old=w.selection;w.selection=signed32(lp);if(w.selection<0 || static_cast<std::size_t>(w.selection)>=w.items.size())w.selection=-1;return static_cast<std::uint64_t>(static_cast<std::int64_t>(old));}
        if(m==0x100c){
            const auto first=signed32(wp);const auto mask=static_cast<std::uint32_t>(lp)&0xfU;
            for(std::size_t i=first<0?0:static_cast<std::size_t>(first)+1;i<w.items.size();++i)if((w.items[i].state&mask)==mask)return i;
            return UINT64_MAX;
        }
        if(m==0x102c)return wp<w.items.size()?w.items[wp].state&static_cast<std::uint32_t>(lp):0;
        if(m==0x102b){
            memory.check(GuestAddress{lp},20,Access::Read);const auto state=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+12},32)),mask=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+16},32));
            if(wp!=UINT32_MAX && wp>=w.items.size())return 0;
            for(std::size_t i=0;i<w.items.size();++i)if(wp==UINT32_MAX || wp==i)set_state(i,state,mask);
            return update(1);
        }
        if(m==0x1008){
            if(wp>=w.items.size())return 0;const auto removed=w.items[wp];notify(-103,static_cast<std::int32_t>(wp),0,0,0,&removed);
            w.items.erase(w.items.begin()+wp);if(w.selection==signed32(wp))w.selection=-1;else if(w.selection>signed32(wp))--w.selection;return update(1);
        }
        if(m==0x1009){
            for(std::size_t i=0;i<w.items.size();++i)notify(-103,static_cast<std::int32_t>(i));
            w.items.clear();w.selection=-1;return update(1);
        }
        if(m==0x102f){ // LVS_OWNERDATA 的列數由客體提供，不列舉宿主檔案。
            if(!(w.style&0x1000))return 1;if(wp>65536)return 0;
            const auto old=w.items.size();w.items.resize(wp);
            for(std::size_t i=old;i<w.items.size();++i){w.items[i].handle=gui_.next_item++;w.items[i].callback_subitems.insert(UINT32_MAX);}
            if(w.selection>=signed32(wp))w.selection=-1;return update(1);
        }
        if(m==0x1015 || m==0x102a){
            const auto last=m==0x102a?wp:static_cast<std::uint32_t>(lp);if(wp>=w.items.size() || last<wp)return 0;
            for(std::size_t i=wp;i<w.items.size() && i<=last;++i)w.items[i].resolved_subitems.clear();return update(1);
        }
        if(m==0x1007 || m==0x104d || m==0x1006 || m==0x104c || m==0x1005 || m==0x104b || m==0x102d || m==0x1073 || m==0x102e || m==0x1074){
            const bool insert=m==0x1007 || m==0x104d;
            const bool get=m==0x1005 || m==0x104b || m==0x102d || m==0x1073;
            const bool text_only=m==0x102d || m==0x1073 || m==0x102e || m==0x1074;
            const bool unicode=m==0x104d || m==0x104c || m==0x104b || m==0x1073 || m==0x1074;
            const std::uint8_t bits=x86_?32:64;
            memory.check(GuestAddress{lp},x86_?32:40,Access::Read);
            const auto mask=text_only?1U:static_cast<std::uint32_t>(memory.load(GuestAddress{lp},32));
            const auto requested=text_only?signed32(wp):signed32(memory.load(GuestAddress{lp+4},32));
            const auto subitem=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+8},32));
            if(subitem>=256 || (insert && subitem) || (!insert && (requested<0 || static_cast<std::size_t>(requested)>=w.items.size())))return insert?UINT64_MAX:0;
            std::size_t index;
            if(insert){
                if(w.items.size()>=65536 || (w.style&0x1000))return UINT64_MAX;
                index=requested<0?w.items.size():std::min(static_cast<std::size_t>(requested),w.items.size());
                GuiItem item;item.handle=gui_.next_item++;w.items.insert(w.items.begin()+static_cast<std::ptrdiff_t>(index),std::move(item));
                if(w.selection>=static_cast<std::int32_t>(index))++w.selection;
            }else index=static_cast<std::size_t>(requested);
            auto& item=w.items[index];
            if(mask&1){
                const auto pointer=memory.load(GuestAddress{lp+(x86_?20:24)},bits);
                if(get){
                    if((mask&0x800) && gui_list_text_callback(item,subitem))memory.store(GuestAddress{lp+(x86_?20:24)},bits,UINT64_MAX);
                    else {
                        if(needs_list_text(item,subitem))throw HostCallFault("ListView 同步文字查詢需要 guest LVN_GETDISPINFO callback");
                        const auto length=text_write(pointer,list_text(item,subitem),signed32(memory.load(GuestAddress{lp+(x86_?24:32)},32)),unicode);
                        if(text_only)return length;
                    }
                }else if(pointer==(x86_?UINT32_MAX:UINT64_MAX)){item.callback_subitems.insert(subitem);item.resolved_subitems.erase(subitem);}
                else {set_list_text(item,subitem,text_read(pointer,unicode));item.callback_subitems.erase(subitem);item.stored_subitems.insert(subitem);}
            }
            if(mask&2){if(get)memory.store(GuestAddress{lp+(x86_?28:36)},32,static_cast<std::uint32_t>(item.image));else item.image=signed32(memory.load(GuestAddress{lp+(x86_?28:36)},32));}
            if(mask&4){if(get)memory.store(GuestAddress{lp+(x86_?32:40)},bits,item.data);else item.data=memory.load(GuestAddress{lp+(x86_?32:40)},bits);}
            if(mask&8){const auto state_mask=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+16},32));if(get)memory.store(GuestAddress{lp+12},32,item.state&state_mask);else set_state(index,static_cast<std::uint32_t>(memory.load(GuestAddress{lp+12},32)),state_mask);}
            if(insert)notify(-102,static_cast<std::int32_t>(index));
            return get?1:update(insert?index:1);
        }
        if(m==0x103a || m==0x103b){if(!gui_.windows.contains(w.list_header))return 0;auto forwarded=msg;forwarded.window=w.list_header;forwarded.message=m==0x103a?0x1212:0x1211;return gui_header_message(memory,forwarded,wide);}
        if(m==0x1019 || m==0x105f || m==0x101a || m==0x1060 || m==0x101b || m==0x1061){
            const bool get=m==0x1019 || m==0x105f,insert=m==0x101b || m==0x1061,unicode=m>=0x105f;
            memory.check(GuestAddress{lp},abi.column_size,Access::Read);const auto mask=memory.load(GuestAddress{lp},32);
            if((insert && w.columns.size()>=256) || (!insert && wp>=w.columns.size()))return insert?UINT64_MAX:0;
            const auto index=insert?std::min<std::size_t>(wp,w.columns.size()):wp;
            GuiListColumn column=insert?GuiListColumn{}:w.columns[index];if(insert){column.subitem=static_cast<std::int32_t>(index);column.identity=gui_.next_item++;column.order=static_cast<std::int32_t>(index);}
            const auto image=abi.column_subitem+4,order=image+4;
            if(mask&1){if(get)memory.store(GuestAddress{lp+4},32,static_cast<std::uint32_t>(column.format));else column.format=signed32(memory.load(GuestAddress{lp+4},32))|(column.format&0x4000);}
            if(mask&2){if(get)memory.store(GuestAddress{lp+8},32,static_cast<std::uint32_t>(column.width));else column.width=std::max(0,signed32(memory.load(GuestAddress{lp+8},32)));}
            if(mask&4){const auto ptr=memory.load(GuestAddress{lp+abi.column_text},abi.bits);if(get)(void)text_write(ptr,column.text,signed32(memory.load(GuestAddress{lp+abi.column_capacity},32)),unicode);else {column.text=text_read(ptr,unicode);column.text_callback=false;column.format|=0x4000;}}
            if(mask&8){if(get)memory.store(GuestAddress{lp+abi.column_subitem},32,static_cast<std::uint32_t>(column.subitem));else {const auto sub=signed32(memory.load(GuestAddress{lp+abi.column_subitem},32));if(sub<0 || sub>=256)return insert?UINT64_MAX:0;column.subitem=sub;}}
            if(mask&16){if(get)memory.store(GuestAddress{lp+image},32,static_cast<std::uint32_t>(column.image));else {column.image=signed32(memory.load(GuestAddress{lp+image},32));column.image_callback=column.image==-1;}}
            if(mask&32){if(get)memory.store(GuestAddress{lp+order},32,static_cast<std::uint32_t>(column.order));else {const auto target=signed32(memory.load(GuestAddress{lp+order},32));if(target<0 || static_cast<std::size_t>(target)>=(w.columns.size()+(insert?1:0)))return insert?UINT64_MAX:0;column.order=target;}}
            if(get)return 1;
            const auto previous=insert?-1:w.columns[index].order;
            for(std::size_t i=0;i<w.columns.size();++i)if(insert || i!=index){auto& other=w.columns[i];if(insert){if(other.order>=column.order){++other.order;++other.revision;}}
                else if(previous<column.order && other.order>previous && other.order<=column.order){--other.order;++other.revision;}else if(column.order<previous && other.order>=column.order && other.order<previous){++other.order;++other.revision;}}
            ++column.revision;if(insert)w.columns.insert(w.columns.begin()+static_cast<std::ptrdiff_t>(index),std::move(column));else w.columns[index]=std::move(column);
            return update(insert?index:1);
        }
        if(m==0x101c){if(wp>=w.columns.size())return 0;const auto order=w.columns[wp].order;w.columns.erase(w.columns.begin()+wp);for(auto& column:w.columns)if(column.order>order){--column.order;++column.revision;}return update(1);}
        if(m==0x101d)return wp<w.columns.size()?static_cast<std::uint32_t>(w.columns[wp].width):0;
        const auto text_width=[&](const std::u16string& text){
            if(native_gui_)if(const auto advances=native_gui_->text_advances(w.control_font,text);advances && !advances->empty())return advances->back();
            const auto metrics=fallback_font_metrics(w.control_font);std::int64_t width=0;for(std::size_t i=0;i<text.size();++i){auto c=text[i];if(c>=0xd800 && c<=0xdbff && i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<=0xdfff){++i;width+=metrics.average_width*2;}else width+=metrics.average_width*((c>=0x2e80 && c<=0xa4cf) || (c>=0xac00 && c<=0xd7ff)?2:1);}return static_cast<std::int32_t>(std::min<std::int64_t>(width,INT32_MAX));
        };
        if(m==0x101e){
            if(wp>=w.columns.size())return 0;auto& column=w.columns[wp];auto width=signed32(lp);
            if(width==-1 || width==-2){std::int64_t measured=width==-2?static_cast<std::int64_t>(text_width(column.text))+18+((column.format&0x600)?14:0):0;
                for(const auto& item:w.items)measured=std::max(measured,static_cast<std::int64_t>(text_width(list_text(item,static_cast<std::uint32_t>(column.subitem))))+18+((column.subitem==0 && item.image>=0)?20:0));
                if(width==-2 && wp+1==w.columns.size()){std::int64_t used=0;for(std::size_t i=0;i<w.columns.size();++i)if(i!=wp)used+=w.columns[i].width;measured=std::max(measured,static_cast<std::int64_t>(w.width)-used);}
                width=static_cast<std::int32_t>(std::clamp<std::int64_t>(measured,0,INT32_MAX));}
            if(width<0)return 0;column.width=width;++column.revision;return update(1);
        }
        if(m==0x1011 || m==0x1057)return static_cast<std::uint64_t>(text_width(text_read(lp,m==0x1057)));
        if(m==0x1028)return std::max(1U,w.height/17);
        if(m==0x1027)return 0;
        if(m==0x1013)return wp<w.items.size()?update(1):0;
        if(m==0x1023)return w.text_color;
        if(m==0x1024){w.text_color=static_cast<std::uint32_t>(lp);return update(1);}
        if(m==0x1000 || m==0x1025)return 0xffffff;
        if(m==0x1001 || m==0x1026)return update(1);
        if(m==0x1030 || m==0x1051)throw HostCallFault("ListView 排序需要 guest compare callback");
    }
    if(gui_control_class(w)==u"listbox" || gui_control_class(w)==u"combobox" || gui_control_class(w)==u"comboboxex32"){
        const bool combo=gui_control_class(w)!=u"listbox";
        const bool multiple=!combo && (w.style&(0x8U|0x800U));
        const bool strings=!(w.style&0x30U) || (w.style&(combo?0x200U:0x40U));
        const auto is=[&](unsigned list,unsigned cb){return m==(combo?cb:list);};
        const auto index=static_cast<std::size_t>(wp);const auto count=w.items.size();
        if(is(0x184,0x14b)){w.items.clear();w.selection=-1;if(combo)w.title.clear();return update(0);}
        if(is(0x180,0x143) || is(0x181,0x14a)){
            if(count>=65536)return UINT64_MAX;
            GuiItem item;item.handle=gui_.next_item++;if(strings)item.text=read(lp);else item.data=lp;
            const bool append=is(0x180,0x143);std::size_t at=append?count:std::min(index,count);
            if(append && strings && (w.style&(combo?0x100U:2U)))at=static_cast<std::size_t>(std::lower_bound(w.items.begin(),w.items.end(),item.text,[](const auto& a,const auto& b){return a.text<b;})-w.items.begin());
            w.items.insert(w.items.begin()+static_cast<std::ptrdiff_t>(at),std::move(item));
            if(w.selection>=static_cast<std::int32_t>(at))++w.selection;
            if(multiple && w.selection<0)w.selection=0;
            return update(at);
        }
        if(is(0x182,0x144)){if(index>=count)return UINT64_MAX;w.items.erase(w.items.begin()+static_cast<std::ptrdiff_t>(index));if(w.selection==static_cast<std::int32_t>(wp)){w.selection=multiple && count>1?static_cast<std::int32_t>(std::min(index,count-2)):-1;if(combo)w.title.clear();}else if(w.selection>static_cast<std::int32_t>(wp))--w.selection;return update(count-1);}
        if(is(0x18b,0x146))return count;
        if(is(0x188,0x147))return static_cast<std::uint64_t>(static_cast<std::int64_t>(w.selection));
        if(is(0x186,0x14e)){if(multiple)return UINT64_MAX;w.selection=index<count?static_cast<std::int32_t>(wp):-1;if(combo)w.title=w.selection>=0?w.items[index].text:std::u16string{};return update(static_cast<std::uint64_t>(static_cast<std::int64_t>(w.selection)));}
        if(is(0x18a,0x149))return index<count?(strings?(wide?w.items[index].text.size():utf16_to_utf8(w.items[index].text).size()):abi.unit):UINT64_MAX;
        if(is(0x189,0x148)){if(index>=count)return UINT64_MAX;if(strings)return write(lp,w.items[index].text,1048576);memory.store(GuestAddress{lp},abi.bits,w.items[index].data);return abi.unit;}
        if(is(0x18f,0x14c) || is(0x1a2,0x158) || is(0x18c,0x14d)){
            if(!count || (wp!=UINT32_MAX && index>=count) || (multiple && is(0x18c,0x14d)))return UINT64_MAX;
            const bool exact=is(0x1a2,0x158),select=is(0x18c,0x14d);const auto needle=strings?read(lp):std::u16string{};
            const auto first=wp==UINT32_MAX?0:(index+1)%count;
            for(std::size_t offset=0;offset<count;++offset){const auto at=(first+offset)%count;const auto& item=w.items[at];
                const bool match=strings?(!exact || item.text.size()==needle.size()) && item.text.size()>=needle.size() &&
                    std::equal(needle.begin(),needle.end(),item.text.begin(),[](char16_t a,char16_t b){return unicode_case_unit(a,false)==unicode_case_unit(b,false);}):item.data==lp;
                if(!match)continue;
                if(select){w.selection=static_cast<std::int32_t>(at);if(combo)w.title=item.text;return update(at);}return at;
            }return UINT64_MAX;
        }
        if(is(0x199,0x150))return index<count?w.items[index].data:UINT64_MAX;
        if(is(0x19a,0x151)){if(index>=count)return UINT64_MAX;w.items[index].data=lp;return 0;}
        if(!combo && m==0x187)return index<count?(multiple?(w.items[index].state&1U):(w.selection==static_cast<std::int32_t>(wp)?1:0)):UINT64_MAX;
        if(!combo && m==0x190){ // LB_GETSELCOUNT 僅適用於多選清單。
            if(!multiple)return UINT64_MAX;
            return static_cast<std::uint64_t>(std::count_if(w.items.begin(),w.items.end(),[](const auto& item){return (item.state&1U)!=0;}));
        }
        if(!combo && m==0x191){ // LB_GETSELITEMS 的輸出是 32 位元索引，不是指標陣列。
            if(!multiple || static_cast<std::int32_t>(wp)<0)return UINT64_MAX;
            std::vector<std::uint32_t> selected;
            for(std::size_t i=0;i<count && selected.size()<wp;++i)if(w.items[i].state&1U)selected.push_back(static_cast<std::uint32_t>(i));
            if(!selected.empty())memory.check(GuestAddress{lp},selected.size()*4,Access::Write);
            for(std::size_t i=0;i<selected.size();++i)memory.store(GuestAddress{lp+i*4},32,selected[i]);
            return selected.size();
        }
        if(!combo && m==0x185){ // LB_SETSEL 的 -1 表示全部；caret 與選取集合分開保存。
            if(!multiple)return UINT64_MAX;
            const auto item_index=static_cast<std::uint32_t>(lp);
            if(item_index!=UINT32_MAX && item_index>=count)return UINT64_MAX;
            for(std::size_t i=0;i<count;++i)if(item_index==UINT32_MAX || i==item_index)w.items[i].state=(w.items[i].state&~1U)|(wp?1U:0U);
            return update(0);
        }
        if(!combo && (m==0x19b || m==0x183)){ // LB_SELITEMRANGEEX / LB_SELITEMRANGE。
            if(!multiple)return UINT64_MAX;
            std::uint32_t first=m==0x19b?wp:static_cast<std::uint32_t>(lp&65535),last=m==0x19b?static_cast<std::uint32_t>(lp):static_cast<std::uint32_t>((lp>>16)&65535);
            const bool select=m==0x19b?first<=last:wp!=0;
            if(first>last)std::swap(first,last);
            if(first>=count || last>=count)return UINT64_MAX;
            for(auto i=first;i<=last;++i)w.items[i].state=(w.items[i].state&~1U)|(select?1U:0U);
            return update(0);
        }
        if(!combo && m==0x19f)return static_cast<std::uint64_t>(static_cast<std::int64_t>(w.selection)); // LB_GETCARETINDEX
        if(!combo && m==0x19e){if(index>=count)return UINT64_MAX;w.selection=static_cast<std::int32_t>(wp);return update(0);}
        if(is(0x192,0x153) || is(0x19e,0x160) || is(0x197,0x154))return 1;
        if(is(0x198,0x153))return 16;
        if(!combo && m==0x18e)return update(1);
    }
    if(gui_control_class(w)==u"systreeview32"){
        const auto signed32=[](std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));};
        const auto lookup=[&](std::uint64_t handle){return std::find_if(w.items.begin(),w.items.end(),[&](const auto& item){return item.handle==handle;});};
        const auto selected=[&](){return w.selection>=0 && static_cast<std::size_t>(w.selection)<w.items.size()?w.items[static_cast<std::size_t>(w.selection)].handle:0;};
        const auto restore=[&](std::uint64_t handle){const auto found_item=lookup(handle);w.selection=found_item==w.items.end()?-1:static_cast<std::int32_t>(found_item-w.items.begin());};
        if(m==0x2005){const auto old=w.list_unicode;w.list_unicode=wp!=0;return old?1:0;}
        if(m==0x2006)return w.list_unicode?1:0;
        if(m==0x1108 || m==0x1109){if(wp>2)return 0;const auto old=w.list_image_lists[wp];if(m==0x1108)return old;if(lp && !gui_.image_lists.contains(lp))return 0;w.list_image_lists[wp]=lp;return update(old);}
        if(m==0x1100 || m==0x1132){
            if(!lp || w.items.size()>=65536)return 0;
            const auto entry=lp+abi.unit*2;memory.check(GuestAddress{lp},abi.unit*2+abi.tree_size,Access::Read);
            const auto mask=memory.load(GuestAddress{entry},32);GuiItem item;item.tree_children=-2;
            item.parent=memory.load(GuestAddress{lp},abi.bits);if(static_cast<std::uint32_t>(item.parent)==0xffff0000U)item.parent=0;
            if(item.parent && lookup(item.parent)==w.items.end())return 0;
            const auto after=memory.load(GuestAddress{lp+abi.unit},abi.bits);const auto special=static_cast<std::uint32_t>(after);
            const auto previous=selected();std::size_t at=w.items.size();
            if(mask&4U)item.data=memory.load(GuestAddress{entry+abi.tree_data},abi.bits);
            if(mask&8U)item.state=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+abi.tree_state},32)&memory.load(GuestAddress{entry+abi.tree_mask},32));
            if(mask&2U)item.image=signed32(memory.load(GuestAddress{entry+abi.tree_image},32));
            if(mask&32U)item.selected_image=signed32(memory.load(GuestAddress{entry+abi.tree_selected},32));
            if(mask&64U)item.tree_children=signed32(memory.load(GuestAddress{entry+abi.tree_children},32));
            if(mask&1U){const auto pointer=memory.load(GuestAddress{entry+abi.tree_text},abi.bits);if(pointer==(x86_?UINT32_MAX:UINT64_MAX))item.callback_subitems.insert(0);else if(pointer)item.text=m==0x1132?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer));}
            if(special==0xffff0001U || special==0xffff0003U){
                for(std::size_t i=0;i<w.items.size();++i)if(w.items[i].parent==item.parent && (special==0xffff0001U || item.text<w.items[i].text)){at=i;break;}
            }else if(after && special!=0xffff0002U && special!=0xffff0000U){const auto found_item=lookup(after);if(found_item==w.items.end() || found_item->parent!=item.parent)return 0;at=static_cast<std::size_t>(found_item-w.items.begin())+1;}
            item.handle=gui_.next_item++;const auto token=item.handle;w.items.insert(w.items.begin()+static_cast<std::ptrdiff_t>(at),std::move(item));restore(previous);invalidate_window(msg.window);return update(token);
        }
        if(m==0x1105)return w.items.size();
        if(m==0x110c || m==0x113e || m==0x110d || m==0x113f){
            if(!lp)return 0;memory.check(GuestAddress{lp},abi.tree_size,Access::Read);
            const auto token=memory.load(GuestAddress{lp+abi.tree_handle},abi.bits),mask=memory.load(GuestAddress{lp},32);const auto item=lookup(token);if(item==w.items.end())return 0;
            const bool set=m==0x110d || m==0x113f,unicode=m==0x113e || m==0x113f;
            if(mask&4U){if(set)item->data=memory.load(GuestAddress{lp+abi.tree_data},abi.bits);else memory.store(GuestAddress{lp+abi.tree_data},abi.bits,item->data);}
            if(mask&8U){const auto sm=memory.load(GuestAddress{lp+abi.tree_mask},32);if(set)item->state=static_cast<std::uint32_t>((item->state&~sm)|(memory.load(GuestAddress{lp+abi.tree_state},32)&sm));else memory.store(GuestAddress{lp+abi.tree_state},32,item->state&sm);}
            for(const auto [bit,offset,value,slot]:std::array<std::tuple<unsigned,unsigned,std::int32_t*,unsigned>,3>{{{2,abi.tree_image,&item->image,0},{32,abi.tree_selected,&item->selected_image,1},{64,abi.tree_children,&item->tree_children,2}}})if(mask&bit){if(set){*value=signed32(memory.load(GuestAddress{lp+offset},32));item->resolved_subitems.erase(slot+1);item->tree_display[slot]=slot==2?0:-2;}else {const auto output=bit==64 && *value==-2?(std::any_of(w.items.begin(),w.items.end(),[&](const auto& child){return child.parent==item->handle;})?1:0):*value==-1?item->tree_display[slot]:*value;memory.store(GuestAddress{lp+offset},32,static_cast<std::uint32_t>(output));}}
            if(mask&1U){const auto pointer=memory.load(GuestAddress{lp+abi.tree_text},abi.bits);
                if(set){item->resolved_subitems.erase(0);if(pointer==(x86_?UINT32_MAX:UINT64_MAX)){item->callback_subitems.insert(0);item->text.clear();}else {item->callback_subitems.erase(0);item->text=pointer?(unicode?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer))):std::u16string{};}}
                else {const auto capacity=signed32(memory.load(GuestAddress{lp+abi.tree_capacity},32));if(pointer && capacity>0){
                    if(unicode){auto count=std::min(item->text.size(),static_cast<std::size_t>(capacity-1));if(count && count<item->text.size() && item->text[count-1]>=0xd800 && item->text[count-1]<=0xdbff)--count;memory.write(GuestAddress{pointer},utf16_bytes(item->text.substr(0,count)));}
                    else {auto text=utf16_to_utf8(item->text);auto count=std::min(text.size(),static_cast<std::size_t>(capacity-1));while(count && count<text.size() && (static_cast<unsigned char>(text[count])&0xc0U)==0x80U)--count;std::vector<std::uint8_t> bytes(text.begin(),text.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);memory.write(GuestAddress{pointer},bytes);}
                }}
            }
            if(set){++item->revision;invalidate_window(msg.window);return update(1);}return 1;
        }
        if(m==0x110b){
            if(wp!=9)return 0;const auto item=lookup(lp);if(lp && item==w.items.end())return 0;
            for(auto& entry:w.items)entry.state&=~2U;
            if(selected()!=lp)++w.tree_revision;
            restore(lp);if(item!=w.items.end()){item->state|=2U;auto parent=item->parent;for(std::size_t depth=0;parent && depth<w.items.size();++depth){const auto ancestor=lookup(parent);if(ancestor==w.items.end())break;ancestor->state|=0x60U;parent=ancestor->parent;}}invalidate_window(msg.window);return update(1);
        }
        if(m==0x110a){
            if(wp==9)return selected();
            if(wp==0){const auto item=std::find_if(w.items.begin(),w.items.end(),[](const auto& entry){return !entry.parent;});return item==w.items.end()?0:item->handle;}
            const auto item=lookup(lp);if(item==w.items.end())return 0;
            if(wp==3)return item->parent;
            if(wp==2){auto previous=item;while(previous!=w.items.begin()){--previous;if(previous->parent==item->parent)return previous->handle;}return 0;}
            if(wp==1 || wp==4)for(auto next=wp==4?w.items.begin():item+1;next!=w.items.end();++next)if(next->parent==(wp==4?lp:item->parent))return next->handle;
            return 0;
        }
        if(m==0x1102){const auto item=lookup(lp);if(item==w.items.end())return 0;const auto action=wp&3U;if(action<1 || action>3)return 0;
            const bool children=(item->tree_children==-1?item->tree_display[2]:item->tree_children)>0 || std::any_of(w.items.begin(),w.items.end(),[&](const auto& entry){return entry.parent==item->handle;});
            const bool expanded=action==3?!(item->state&0x20U):action==2;if(!children || (!expanded && !(item->state&0x20U) && !(wp&0x8000U)))return 0;
            item->state=(item->state&~0xa0U)|(expanded?0x20U|((wp&0x4000U)?0x80U:0U):0U);if(wp&0x8000U)item->state&=~0x60U;++item->revision;invalidate_window(msg.window);return update(1);
        }
        if(m==0x1101){
            if(!lp || static_cast<std::uint32_t>(lp)==0xffff0000U){if(selected())++w.tree_revision;w.items.clear();w.selection=-1;return update(1);}
            if(lookup(lp)==w.items.end())return 0;const auto previous=selected();
            // 先建父子索引，再遍歷整棵子樹；不留下孫節點或使其他選取索引漂移。
            std::map<std::uint64_t,std::vector<std::uint64_t>> children;for(const auto& item:w.items)children[item.parent].push_back(item.handle);
            std::set<std::uint64_t> removed;std::vector<std::uint64_t> pending{lp};
            while(!pending.empty()){const auto handle=pending.back();pending.pop_back();if(!removed.insert(handle).second)continue;if(const auto descendants=children.find(handle);descendants!=children.end())pending.insert(pending.end(),descendants->second.begin(),descendants->second.end());}
            std::erase_if(w.items,[&](const auto& item){return removed.contains(item.handle);});restore(previous);if(previous!=selected())++w.tree_revision;return update(1);
        }
    }
    // 內建控制項對未知訊息依 DefWindowProc 慣例回傳零。
    return 0;
}
}
