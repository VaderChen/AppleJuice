#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/raster_surface.hpp"
#include <algorithm>
#include <bit>
#include <numeric>

namespace x64arm {
namespace {
constexpr std::uint32_t first=0x1300,buttons=0x100,multiline=0x200,vertical=0x80,bottom=2;
std::int32_t integer(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
std::uint64_t index_result(std::int32_t value){return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));}
GuiTabControl& tabs(GuiWindow& window){if(!window.tab)window.tab.emplace();return *window.tab;}
bool valid(const GuiTabControl& tab,std::int32_t index){return index>=0 && static_cast<std::size_t>(index)<tab.items.size();}
std::int32_t hit(const GuiTabControl& tab,std::int32_t x,std::int32_t y){
    for(std::size_t i=0;i<tab.rectangles.size();++i){const auto& r=tab.rectangles[i];if(x>=r[0] && y>=r[1] && x<r[2] && y<r[3])return static_cast<std::int32_t>(i);}return -1;
}
std::u16string display_text(const GuiTabItem& item){
    std::u16string text;for(std::size_t i=0;i<item.text.size();++i){if(item.text[i]!=u'&')text.push_back(item.text[i]);else if(i+1<item.text.size() && item.text[i+1]==u'&'){text.push_back(u'&');++i;}}return text;
}
}

void WindowsServices::layout_gui_tab(GuiWindow& window){
    auto& tab=tabs(window);const bool sideways=(window.style&vertical)!=0,multi=(window.style&multiline)!=0;
    const auto metrics=native_gui_?native_gui_->font_metrics(window.control_font).value_or(fallback_font_metrics(window.control_font)):fallback_font_metrics(window.control_font);
    window.list_icons.clear();
    const auto image=gui_.image_lists.find(tab.image_list);
    if(image!=gui_.image_lists.end())window.list_icons=image->second.images;
    const auto icon_width=image==gui_.image_lists.end()?0:std::max(0,image->second.width),icon_height=image==gui_.image_lists.end()?0:std::max(0,image->second.height);
    const auto height=tab.item_height?tab.item_height:std::clamp(std::max(metrics.height,icon_height)+tab.padding_y*2+4,1,65535);
    const std::int64_t extent=sideways?window.height:window.width;
    const std::int64_t cross=sideways?window.width:window.height;
    std::vector<std::int32_t> widths; widths.reserve(tab.items.size());
    for(const auto& item:tab.items){
        const auto text=display_text(item);std::int64_t length=static_cast<std::int64_t>(text.size())*metrics.average_width;
        if(native_gui_)if(const auto measured=native_gui_->text_advances(window.control_font,text);measured && !measured->empty())length=measured->back();
        const bool has_icon=item.image>=0 && static_cast<std::size_t>(item.image)<window.list_icons.size();
        const auto minimum=tab.minimum_width<0?54:tab.minimum_width;
        // TCS_FIXEDWIDTH 一律固定寬度；TCS_OWNERDRAWFIXED 只在 TCM_SETITEMSIZE 或已處理的 WM_MEASUREITEM 給定寬度時固定，否則與一般分頁同樣依文字。
        widths.push_back(((window.style&0x400U) || ((window.style&0x2000U) && tab.item_width))?static_cast<std::int32_t>(std::clamp<std::int64_t>(std::max<std::int64_t>(tab.item_width?tab.item_width:96,static_cast<std::int64_t>(icon_width)+1),1,65535)):static_cast<std::int32_t>(std::clamp<std::int64_t>(std::max<std::int64_t>(minimum,length+2*tab.padding_x+(has_icon?icon_width+4:0)),1,65535)));
    }
    std::vector<std::array<std::int64_t,4>> rectangles;rectangles.reserve(widths.size());std::vector<std::size_t> row_starts{0};
    std::int64_t x=2,y=2;
    for(std::size_t i=0;i<widths.size();++i){
        if(multi && x>2 && static_cast<std::int64_t>(x)+widths[i]>std::max<std::int64_t>(4,extent-2)){x=2;y+=height;row_starts.push_back(i);}
        rectangles.push_back({x,y,x+widths[i],y+height});x+=widths[i]+((window.style&buttons)?3:0);
    }
    tab.rows=tab.items.empty()?0:static_cast<std::uint32_t>(row_starts.size());
    const auto band=static_cast<std::int64_t>(tab.rows)*height+2;
    if(multi && !(window.style&0x800U) && !(window.style&buttons)){
        // TCS_RIGHTJUSTIFY 將每一列填滿；查詢、命中及原生繪圖採相同矩形。
        row_starts.push_back(tab.items.size());
        for(std::size_t row=0;row+1<row_starts.size();++row){const auto begin=row_starts[row],end=row_starts[row+1];if(begin==end)continue;
            const auto remaining=std::max<std::int64_t>(0,extent-2-rectangles[end-1][2]);std::int64_t offset=0;
            for(auto i=begin;i<end;++i){const auto extra=remaining/static_cast<std::int32_t>(end-begin)+(i-begin<static_cast<std::size_t>(remaining%static_cast<std::int32_t>(end-begin))?1:0);auto& r=rectangles[i];r[0]+=offset;offset+=extra;r[2]+=offset;}
        }
    }
    if(multi && !(window.style&buttons) && valid(tab,window.selection)){
        const auto selected_row=(rectangles[static_cast<std::size_t>(window.selection)][1]-2)/height;
        for(auto& r:rectangles){const auto row=(r[1]-2)/height;const auto target=row==selected_row?static_cast<std::int64_t>(tab.rows)-1:row>selected_row?row-1:row;const auto delta=(target-row)*height;r[1]+=delta;r[3]+=delta;}
    }
    if(!multi){
        if(valid(tab,window.selection)){const auto& r=rectangles[static_cast<std::size_t>(window.selection)];if(r[0]<tab.scroll+2)tab.scroll=r[0]-2;else if(r[2]>tab.scroll+extent-2)tab.scroll=r[2]-extent+2;}
        tab.scroll=std::clamp<std::int64_t>(tab.scroll,0,std::max<std::int64_t>(0,x-extent+2));for(auto& r:rectangles){r[0]-=tab.scroll;r[2]-=tab.scroll;}
    }else tab.scroll=0;
    for(auto& r:rectangles){
        if(window.style&bottom){const auto a=cross-r[3];r[3]=cross-r[1];r[1]=a;}
        if(sideways)r={r[1],r[0],r[3],r[2]};
        if(window.ex_style&0x400000U){const auto a=static_cast<std::int64_t>(window.width)-r[2];r[2]=static_cast<std::int64_t>(window.width)-r[0];r[0]=a;}
    }
    std::array<std::int64_t,4> display{2,2,static_cast<std::int64_t>(window.width)-2,static_cast<std::int64_t>(window.height)-2};
    if(!(window.style&buttons)){
        if(sideways){if(window.style&bottom)display[2]-=band;else display[0]+=band;}
        else {if(window.style&bottom)display[3]-=band;else display[1]+=band;}
    }
    const auto coordinate=[](std::int64_t value){return static_cast<std::int32_t>(std::clamp<std::int64_t>(value,INT32_MIN,INT32_MAX));};
    tab.rectangles.clear();tab.rectangles.reserve(rectangles.size());
    for(const auto& rect:rectangles)tab.rectangles.push_back({coordinate(rect[0]),coordinate(rect[1]),coordinate(rect[2]),coordinate(rect[3])});
    for(unsigned i=0;i<4;++i)tab.display[i]=coordinate(display[i]);
}

std::optional<std::uint64_t> WindowsServices::gui_tab_message(GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& window=gui_.windows.at(message.window);auto& tab=tabs(window);const auto m=message.message;const auto wp=message.wparam,lp=message.lparam;const auto index=integer(wp);
    const auto update=[&](std::uint64_t result){present_gui(message.window);if(window.style&0x2000U)invalidate_window(message.window);return result;};
    const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto extra=tab.extra_bytes==UINT32_MAX?unit:tab.extra_bytes;
    if(m==first+4)return tab.items.size();
    if(m==first+2)return tab.image_list;
    if(m==first+3){if(lp && !gui_.image_lists.contains(lp))return 0;const auto previous=tab.image_list;tab.image_list=lp;return update(previous);}
    if(m==first+11)return index_result(window.selection);
    if(m==first+12){if(index!=-1 && !valid(tab,index))return index_result(-1);const auto old=window.selection;window.selection=index;tab.focus=index;++tab.revision;return update(index_result(old));}
    if(m==first+47)return index_result(tab.focus);
    if(m==first+14){if(!tab.items.empty() || wp>4096)return 0;tab.extra_bytes=static_cast<std::uint32_t>(wp);return 1;}
    const bool insert=m==first+7 || m==first+62,get=m==first+5 || m==first+60,set=m==first+6 || m==first+61;
    if(insert || get || set){
        if(!lp || index<0 || (!insert && !valid(tab,index)))return insert?index_result(-1):0;
        const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{lp},32));
        if(mask&~31U)return insert?index_result(-1):0;
        const unsigned text_offset=x86_?12U:16U,count_offset=text_offset+unit,image_offset=count_offset+4,data_offset=x86_?24U:32U;
        const bool unicode=m>=first+60;
        const auto read=[&](unsigned at,std::uint8_t n=32){return memory.load(GuestAddress{lp+at},n);};
        if(get){
            const auto& item=tab.items[static_cast<std::size_t>(index)];std::vector<std::uint8_t> text;
            std::uint64_t pointer=0;
            // 先驗證所有輸出，避免部份欄位寫入後才遇到壞指標。
            if(mask&1U){pointer=read(text_offset,bits);const auto count=integer(read(count_offset));if(pointer && count>0){
                if(unicode)text=utf16_bytes(item.text.substr(0,static_cast<std::size_t>(count-1)));
                else {auto ansi=utf16_to_utf8(item.text);ansi.resize(std::min(ansi.size(),static_cast<std::size_t>(count-1)));text.assign(ansi.begin(),ansi.end());text.push_back(0);}
                memory.check(GuestAddress{pointer},text.size(),Access::Write);
            }}
            if(mask&2U)memory.check(GuestAddress{lp+image_offset},4,Access::Write);
            if(mask&8U)memory.check(GuestAddress{lp+data_offset},extra,Access::Write);
            const auto state_mask=mask&16U?read(8):0;if(mask&16U)memory.check(GuestAddress{lp+4},4,Access::Write);
            if(!text.empty())memory.write(GuestAddress{pointer},text);
            if(mask&2U)memory.store(GuestAddress{lp+image_offset},32,static_cast<std::uint32_t>(item.image));
            if(mask&8U)memory.write(GuestAddress{lp+data_offset},item.data);
            if(mask&16U)memory.store(GuestAddress{lp+4},32,item.state&state_mask);return 1;
        }
        if(insert && tab.items.size()>=65536){last_error_=8;return index_result(-1);}
        GuiTabItem item=insert?GuiTabItem{}:tab.items[static_cast<std::size_t>(index)];if(insert){item.identity=gui_.next_item++;item.data.resize(extra);}
        if(mask&1U){const auto pointer=read(text_offset,bits);if(pointer==(x86_?UINT32_MAX:UINT64_MAX)){last_error_=87;return insert?index_result(-1):0;}item.text=pointer?(unicode?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer))):std::u16string{};}
        if(mask&2U)item.image=integer(read(image_offset));
        if(mask&8U)memory.read(GuestAddress{lp+data_offset},item.data);
        if(mask&4U)item.rtl=true;
        if((mask&16U) && !insert){const auto states=static_cast<std::uint32_t>(read(8));item.state=(item.state&~states)|(static_cast<std::uint32_t>(read(4))&states);}
        if(insert){const auto at=std::min(static_cast<std::size_t>(index),tab.items.size());tab.items.insert(tab.items.begin()+static_cast<std::ptrdiff_t>(at),std::move(item));
            if(tab.items.size()==1){window.selection=tab.focus=0;++tab.revision;}else {if(window.selection>=static_cast<std::int32_t>(at))++window.selection;if(tab.focus>=static_cast<std::int32_t>(at))++tab.focus;}
            return update(at);
        }
        tab.items[static_cast<std::size_t>(index)]=std::move(item);return update(1);
    }
    if(m==first+8){if(!valid(tab,index))return 0;tab.items.erase(tab.items.begin()+index);
        if(window.selection==index){window.selection=-1;++tab.revision;}else if(window.selection>index)--window.selection;
        if(tab.focus==index)tab.focus=-1;else if(tab.focus>index)--tab.focus;return update(1);
    }
    if(m==first+9){tab.items.clear();window.selection=tab.focus=-1;++tab.revision;return update(1);}
    if(m==first+10){if(!lp || !valid(tab,index))return 0;layout_gui_tab(window);memory.check(GuestAddress{lp},16,Access::Write);const auto& r=tab.rectangles[static_cast<std::size_t>(index)];for(unsigned i=0;i<4;++i)memory.store(GuestAddress{lp+i*4},32,static_cast<std::uint32_t>(r[i]));return 1;}
    if(m==first+13){
        if(!lp)return index_result(-1);memory.check(GuestAddress{lp},12,Access::Read);memory.check(GuestAddress{lp+8},4,Access::Write);layout_gui_tab(window);
        const auto x=integer(memory.load(GuestAddress{lp},32)),y=integer(memory.load(GuestAddress{lp+4},32));
        const auto found=x<0 || y<0 || static_cast<std::uint32_t>(x)>=window.width || static_cast<std::uint32_t>(y)>=window.height?-1:hit(tab,x,y);
        std::uint32_t flags=1;
        if(found>=0){const auto& item=tab.items[static_cast<std::size_t>(found)];const auto& r=tab.rectangles[static_cast<std::size_t>(found)];flags=4;
            const auto image=gui_.image_lists.find(tab.image_list);if(image!=gui_.image_lists.end() && item.image>=0 && static_cast<std::size_t>(item.image)<image->second.images.size() && x<r[0]+tab.padding_x+image->second.width)flags=2;}
        memory.store(GuestAddress{lp+8},32,flags);return index_result(found);
    }
    if(m==first+40){ // TCM_ADJUSTRECT：RECT 以呼叫者的原點計算，而非替換成控制項絕對座標。
        if(!lp)return 0;layout_gui_tab(window);memory.check(GuestAddress{lp},16,Access::Read);memory.check(GuestAddress{lp},16,Access::Write);
        const std::array<std::int64_t,4> insets{tab.display[0],tab.display[1],static_cast<std::int64_t>(window.width)-tab.display[2],static_cast<std::int64_t>(window.height)-tab.display[3]};
        for(unsigned i=0;i<4;++i){const auto old=integer(memory.load(GuestAddress{lp+i*4},32));const auto delta=(i<2?1LL:-1LL)*(wp?-1LL:1LL)*insets[i];memory.store(GuestAddress{lp+i*4},32,static_cast<std::uint32_t>(std::clamp<std::int64_t>(old+delta,INT32_MIN,INT32_MAX)));}return 0;
    }
    if(m==first+41){
        const auto metrics=native_gui_?native_gui_->font_metrics(window.control_font).value_or(fallback_font_metrics(window.control_font)):fallback_font_metrics(window.control_font);
        const auto image=gui_.image_lists.find(tab.image_list);const auto icon_height=image==gui_.image_lists.end()?0:image->second.height,icon_width=image==gui_.image_lists.end()?0:image->second.width;
        const auto old_width=std::clamp(std::max(tab.item_width?tab.item_width:96,icon_width+1),1,65535);
        const auto old_height=tab.item_height?tab.item_height:std::clamp(std::max(metrics.height,icon_height)+tab.padding_y*2+4,1,65535);
        tab.item_width=std::max(1,static_cast<std::int32_t>(lp&65535U));tab.item_height=std::max(1,static_cast<std::int32_t>((lp>>16)&65535U));return update(static_cast<std::uint32_t>(old_width)|(static_cast<std::uint32_t>(old_height)<<16));
    }
    if(m==first+42){const auto image=gui_.image_lists.find(tab.image_list);if(index<0 || image==gui_.image_lists.end() || static_cast<std::size_t>(index)>=image->second.images.size())return 0;
        image->second.images.erase(image->second.images.begin()+index);for(auto& item:tab.items){if(item.image==index)item.image=-1;else if(item.image>index)--item.image;}
        for(const auto& [handle,other]:gui_.windows)if(handle!=message.window && ((other.tab && other.tab->image_list==tab.image_list) || std::find(other.list_image_lists.begin(),other.list_image_lists.end(),tab.image_list)!=other.list_image_lists.end()))present_gui(handle);
        return update(0);
    }
    if(m==first+43){tab.padding_x=static_cast<std::int32_t>(lp&65535U);tab.padding_y=static_cast<std::int32_t>((lp>>16)&65535U);return update(0);}
    if(m==first+44){layout_gui_tab(window);return tab.rows;}
    if(m==first+45)return gui_.windows.contains(tab.tooltips)?tab.tooltips:0;
    if(m==first+46){
        if(wp==tab.tooltips)return 0;
        if(const auto previous=gui_.windows.find(tab.tooltips);previous!=gui_.windows.end() && previous->second.tooltip){
            auto& tip=*previous->second.tooltip;
            if(std::any_of(tip.tools.begin(),tip.tools.end(),[&](const auto& tool){return tool.owner==message.window && (tool.identity==tip.current || tool.identity==tip.shown);})){hide_gui_tooltip(tab.tooltips);tip.current=0;tip.tracking=false;}
            std::erase_if(tip.tools,[&](const auto& tool){return tool.owner==message.window;});++tip.revision;
        }
        tab.tooltips=wp;return update(0);
    }
    if(m==first+49){const auto previous=tab.minimum_width<0?54:tab.minimum_width;tab.minimum_width=std::max(-1,integer(lp));return update(index_result(previous));}
    if(m==first+50){for(std::size_t i=0;i<tab.items.size();++i)if(!wp || static_cast<std::int32_t>(i)!=window.selection)tab.items[i].state&=~1U;if(!wp){window.selection=-1;++tab.revision;}return update(0);}
    if(m==first+51){if(!valid(tab,index))return 0;auto& item=tab.items[static_cast<std::size_t>(index)];item.state=(item.state&~2U)|((lp&65535U)?2U:0U);return update(1);}
    if(m==first+52){const auto previous=tab.extended_style,mask=wp?static_cast<std::uint32_t>(wp):UINT32_MAX;tab.extended_style=(previous&~mask)|(static_cast<std::uint32_t>(lp)&mask);return update(previous);}
    if(m==first+53)return tab.extended_style;
    if(m==0x2005){const bool old=window.list_unicode;window.list_unicode=wp!=0;return old?1:0;}
    if(m==0x2006)return window.list_unicode?1:0;
    if(m==0x87)return 1; // DLGC_WANTARROWS；Tab 留給對話框導航。
    if(m==7){tab.focused=true;if(!valid(tab,tab.focus))tab.focus=window.selection;return update(0);}
    if(m==8){tab.focused=false;return update(0);}
    (void)wide;return std::nullopt;
}

bool WindowsServices::begin_gui_tab(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& initial=gui_.windows.at(message.window);
    if((initial.style&0x2000U) && (message.message==1 || message.message==15)){
        auto& tab=tabs(initial);
        if(message.message==15){
            std::erase_if(gui_.queue,[&](const auto& queued){return queued.window==message.window && queued.message==15;});
            if(!initial.redraw || std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& op){return op.second.owner_draw && op.second.message.window==message.window && op.second.message.message==15;})){complete_call(state,memory,0);return true;}
            initial.invalid=false;initial.drawing.clear();layout_gui_tab(initial);
            if(native_gui_){
                // 先繪製控制項框架，再讓客體畫各頁籤；舊 Raster 不應蓋住新框架。
                const auto dc=0x10000000+message.window;rasters_.erase(dc);native_gui_->graphics_surface(message.window,{});present_gui(message.window);
                const auto background=native_gui_->capture_client(message.window);gui_.contexts.try_emplace(dc);
                if(background)if(const auto surface=raster_surface(memory,dc))surface->upload(background->pixels);
            }
        }
        if(gui_.control_operations.size()>=128)throw HostCallFault("自繪控制項回呼超過上限");
        GuiControlOperation op;op.message=message;op.thread=threads_.current;op.wide=wide;op.owner_draw=true;
        if(message.message==15){const auto dc=0x10000000+message.window;auto& saved=gui_.saved_contexts[dc];saved.push_back(gui_.contexts[dc]);op.parameter=saved.size();}
        for(const auto& item:tab.items)op.identities.push_back(item.identity);
        if(gui_.control_notification_buffers.empty())op.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {op.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(op));
        try{start_callbacks(state,memory,{},Continuation::GuiControlOperation,token);}catch(...){discard_gui_control(token);throw;}return true;
    }
    if(message.message!=first+48 && message.message!=0x201 && message.message!=0x100)return false;
    if(initial.style&0x2000U)invalidate_window(message.window);
    auto& window=gui_.windows.at(message.window);auto& tab=tabs(window);layout_gui_tab(window);std::int32_t target=-1;
    const auto finish=[&]{complete_call(state,memory,0);return true;};
    if(message.message==first+48){target=integer(message.wparam);if(target<0){tab.focus=-1;present_gui(message.window);return finish();}}
    else {
        if(!window.enabled || (tab.items.empty() && message.message!=0x100))return finish();
        if(message.message==0x201){const auto x=static_cast<std::int16_t>(message.lparam),y=static_cast<std::int16_t>(message.lparam>>16);if(x>=0 && y>=0 && static_cast<std::uint32_t>(x)<window.width && static_cast<std::uint32_t>(y)<window.height)target=hit(tab,x,y);}
        else {const auto key=message.wparam;target=valid(tab,tab.focus)?tab.focus:window.selection;if(target<0)target=0;
            if(key==36)target=0;else if(key==35)target=static_cast<std::int32_t>(tab.items.size()-1);
            else if(key>=37 && key<=40){const bool previous=(key==37 && !(window.ex_style&0x400000U)) || key==38 || (key==39 && (window.ex_style&0x400000U));target=previous?std::max(0,target-1):std::min(static_cast<std::int32_t>(tab.items.size()-1),target+1);}
            else if(key!=32 || !(window.style&buttons))target=-1;
        }
    }
    if(!valid(tab,target) && message.message!=0x100)return finish();
    if((window.style&buttons) && message.message==first+48){tab.focus=target;present_gui(message.window);return finish();}
    if(gui_.control_operations.size()>=128)throw HostCallFault("分頁控制項回呼超過上限");
    GuiControlOperation op;op.message=message;op.thread=threads_.current;op.wide=wide;op.tab_select=true;op.item=valid(tab,target)?tab.items[static_cast<std::size_t>(target)].identity:0;op.parameter=tab.revision;
    const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(op));
    try{(void)resume_gui_tab(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
}

std::optional<std::uint32_t> WindowsServices::resume_gui_tab(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& op=gui_.control_operations.at(token);const auto found=gui_.windows.find(op.message.window);
    const auto finish=[&]()->std::optional<std::uint32_t>{discard_gui_control(token);complete_call(state,memory,0);return {};};
    if(found==gui_.windows.end() || !found->second.tab || op.phase==3)return finish();
    auto& window=found->second;auto& tab=*window.tab;
    const auto notify=[&](std::int32_t code){
        if(!op.buffer){if(gui_.control_notification_buffers.empty())op.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {op.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}}
        const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);memory.write(GuestAddress{op.buffer},std::array<std::uint8_t,32>{});
        memory.store(GuestAddress{op.buffer},bits,op.message.window);memory.store(GuestAddress{op.buffer+unit},bits,window.id);memory.store(GuestAddress{op.buffer+unit*2},32,static_cast<std::uint32_t>(code));
        if(code==-550)memory.store(GuestAddress{op.buffer+(x86_?12U:24U)},16,op.message.wparam); // NMTCKEYDOWN 使用 packed 結構；flags 保留為零。
        start_callbacks(state,memory,{{resolve("user32.dll",window.list_unicode?"SendMessageW":"SendMessageA").value,{window.parent,0x4e,window.id,op.buffer}}},Continuation::GuiControlOperation,token);
    };
    if(!op.phase && op.message.message==0x100){op.phase=4;if(gui_.windows.contains(window.parent)){notify(-550);return {};}}
    if(op.phase==4){op.phase=1;result=0;}
    const auto item=std::find_if(tab.items.begin(),tab.items.end(),[&](const auto& value){return value.identity==op.item;});
    if(item==tab.items.end() || tab.revision!=op.parameter)return finish();
    const auto target=static_cast<std::int32_t>(item-tab.items.begin());
    if(op.message.message==0x100 && (window.style&buttons) && op.message.wparam!=32){tab.focus=target;present_gui(op.message.window);return finish();}
    if(!op.phase){op.phase=1;
        if(op.message.message==0x201 && !(window.style&0x8000U) && gui_.focus!=op.message.window){start_callbacks(state,memory,{{resolve("user32.dll","SetFocus").value,{op.message.window}}},Continuation::GuiControlOperation,token);return {};}
    }
    if(op.phase==1){
        if(window.selection==target){tab.focus=target;present_gui(op.message.window);return finish();}
        op.phase=2;if(gui_.windows.contains(window.parent)){notify(-552);return {};}
        result=0;
    }
    if(result)return finish(); // TCN_SELCHANGING 可否決，而且通知中仍可讀取舊選取。
    window.selection=tab.focus=target;++tab.revision;
    if(window.style&buttons){for(auto& value:tab.items)value.state&=~1U;item->state|=1U;}
    const auto handle=op.message.window;present_gui(handle);op.phase=3;
    if(gui_.windows.contains(window.parent)){notify(-551);return {};}
    return finish();
}
}
