#include "x64arm/windows_services.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_calendar.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_control_images.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/windows_userdraw.hpp"
#include "x64arm/raster_surface.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <numeric>

namespace x64arm {
namespace {
std::int32_t integer(std::uint64_t n){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(n));}
std::uint64_t signed_result(std::int32_t n){return static_cast<std::uint64_t>(static_cast<std::int64_t>(n));}
GuiHeaderControl& header(GuiWindow& w){if(!w.header)w.header.emplace();return *w.header;}
std::uint64_t drag_image(GuiState& gui,GuiBitmap image){
    if(!image.width || !image.height || image.pixels.empty())return 0;
    GuiImageList list;list.width=static_cast<std::int32_t>(image.width);list.height=static_cast<std::int32_t>(image.height);list.flags=0x21;
    GuiIcon icon;icon.width=image.width;icon.height=image.height;icon.rgba=std::move(image.pixels);
    for(auto& pixel:icon.rgba)pixel=(pixel&0x00ffffffU)|((((pixel>>24)+1U)/2U)<<24);
    finish_icon(icon);list.images.push_back(std::move(icon));const auto handle=gui.next_object++;gui.image_lists.emplace(handle,std::move(list));return handle;
}
std::vector<GuiListColumn>& columns(GuiState& gui,GuiWindow& w){
    const auto owner=gui.windows.find(w.header_list);return owner==gui.windows.end()?w.columns:owner->second.columns;
}
std::vector<std::uint32_t> display_order(const std::vector<GuiListColumn>& items){
    std::vector<std::uint32_t> order(items.size());std::iota(order.begin(),order.end(),0U);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){const auto x=items[a].order<0?static_cast<std::int32_t>(a):items[a].order,y=items[b].order<0?static_cast<std::int32_t>(b):items[b].order;return x<y;});return order;
}
void move_order(std::vector<GuiListColumn>& items,std::size_t index,std::int32_t target){
    const auto old=items[index].order;target=std::clamp(target,0,static_cast<std::int32_t>(items.size()-1));
    for(std::size_t i=0;i<items.size();++i)if(i!=index){auto& c=items[i];if(old<target && c.order>old && c.order<=target){--c.order;++c.revision;}else if(target<old && c.order>=target && c.order<old){++c.order;++c.revision;}}
    items[index].order=target;
}
struct ItemAbi {
    unsigned unit,text{8},bitmap,count,format,data,image,order,type,filter,state,size;std::uint8_t bits;
    explicit ItemAbi(bool small):unit(small?4U:8U),bitmap(small?12U:16U),count(small?16U:24U),format(small?20U:28U),data(small?24U:32U),image(small?28U:40U),order(small?32U:44U),type(small?36U:48U),filter(small?40U:56U),state(small?44U:64U),size(small?48U:72U),bits(small?32:64){}
};
std::vector<std::uint8_t> text_bytes(const std::u16string& text,std::size_t capacity,bool wide){
    if(!capacity)return {};
    if(wide){auto n=std::min(text.size(),capacity-1);if(n && n<text.size() && text[n-1]>=0xd800 && text[n-1]<=0xdbff)--n;return utf16_bytes(text.substr(0,n));}
    auto value=utf16_to_utf8(text);auto n=std::min(value.size(),capacity-1);while(n && n<value.size() && (static_cast<unsigned char>(value[n])&0xc0U)==0x80U)--n;
    std::vector<std::uint8_t> result(value.begin(),value.begin()+static_cast<std::ptrdiff_t>(n));result.push_back(0);return result;
}
template<class ReadText>
bool read_item(GuestMemory& memory,std::uint64_t address,const ItemAbi& abi,bool wide,GuiListColumn& item,ReadText read){
    if(!address)return false;const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{address},32));
    if(mask&~0x3ffU)return false;
    const auto load=[&](unsigned offset,std::uint8_t bits=32){return memory.load(GuestAddress{address+offset},bits);};
    if(mask&1U)item.width=std::max(0,integer(load(4)));
    if(mask&2U){const auto p=load(abi.text,abi.bits);item.text_callback=p==(abi.bits==32?UINT32_MAX:UINT64_MAX);item.text=item.text_callback?std::u16string{}:read(p,wide);}
    if(mask&4U)item.format=integer(load(abi.format));
    if(mask&8U)item.data=load(abi.data,abi.bits);
    if(mask&16U)item.bitmap=load(abi.bitmap,abi.bits);
    if(mask&32U){item.image=integer(load(abi.image));item.image_callback=item.image==-1;}
    if(mask&128U)item.order=integer(load(abi.order));
    if(mask&512U)item.state=static_cast<std::uint32_t>(load(abi.state))&1U;
    if(mask&256U){
        GuiHeaderFilter value;value.type=static_cast<std::uint32_t>(load(abi.type));
        if((value.type&~0x8003U) || (value.type&3U)>2U)return false;
        if(!(value.type&0x8000U)){
            const auto p=load(abi.filter,abi.bits);if(!p)return false;
            if((value.type&3U)==0){const auto text=memory.load(GuestAddress{p},abi.bits);value.text=read(text,wide);}
            else if((value.type&3U)==1)value.number=integer(memory.load(GuestAddress{p},32));
            else {memory.check(GuestAddress{p},16,Access::Read);for(unsigned n=0;n<8;++n)value.date[n]=static_cast<std::uint16_t>(memory.load(GuestAddress{p+n*2},16));if(!gui_date_valid(value.date))return false;}
        }
        item.filter=std::move(value);
    }
    return true;
}
std::u16string filter_text(const GuiHeaderFilter& value){
    if(value.type&0x8000U)return {};
    if((value.type&3U)==0)return value.text;
    if((value.type&3U)==1)return utf8_to_utf16_lossy(std::to_string(value.number));
    const auto digits=[](unsigned n,unsigned length){auto text=std::to_string(n);if(text.size()<length)text.insert(0,length-text.size(),'0');return utf8_to_utf16_lossy(text);};
    return digits(value.date[0],4)+u"-"+digits(value.date[1],2)+u"-"+digits(value.date[3],2);
}
bool edit_filter(GuiHeaderFilter& value,std::u16string_view text){
    auto next=value;next.type&=~0x8000U;
    if(text.empty()){next.type|=0x8000U;next.text.clear();value=std::move(next);return true;}
    if((value.type&3U)==0)next.text=text;
    else {
        const auto input=utf16_to_utf8(text);const auto number=[](std::string_view part,std::int32_t& output){const auto parsed=std::from_chars(part.data(),part.data()+part.size(),output);return parsed.ec==std::errc{} && parsed.ptr==part.data()+part.size();};
        if((value.type&3U)==1){if(!number(input,next.number))return false;}
        else {std::array<std::int32_t,3> parts{};std::size_t begin=0;for(unsigned n=0;n<3;++n){const auto end=n==2?input.size():input.find('-',begin);if(end==std::string::npos || !number(std::string_view(input).substr(begin,end-begin),parts[n]) || parts[n]<0 || parts[n]>65535)return false;begin=end+1;}next.date[0]=static_cast<std::uint16_t>(parts[0]);next.date[1]=static_cast<std::uint16_t>(parts[1]);next.date[3]=static_cast<std::uint16_t>(parts[2]);if(!gui_date_valid(next.date))return false;next.date=gui_date_normalize(next.date);}
    }
    value=std::move(next);return true;
}
std::array<std::int32_t,4> filter_rectangle(const GuiWindow& w,std::size_t index,bool button=false){
    auto rect=w.header->rectangles[index];rect[1]=(rect[1]+rect[3])/2;
    const auto edge=std::min(18,std::max(0,rect[2]-rect[0]));
    if(w.ex_style&0x400000U){if(button)rect[2]=rect[0]+edge;else rect[0]+=edge;}
    else {if(button)rect[0]=rect[2]-edge;else rect[2]-=edge;}
    return rect;
}
std::array<std::int32_t,4> dropdown_rectangle(const GuiWindow& w,std::size_t index){
    auto rect=w.header->rectangles[index];if(w.style&0x100U)rect[3]=(rect[1]+rect[3])/2;const auto width=std::min(18,std::max(0,rect[2]-rect[0]));
    if(w.ex_style&0x400000U)rect[2]=rect[0]+width;else rect[0]=rect[2]-width;return rect;
}
std::pair<std::int32_t,std::uint32_t> hit(const GuiWindow& w,std::int32_t x,std::int32_t y){
    std::uint32_t flags=0;if(x<0)flags|=0x800;if(y<0)flags|=0x100;if(static_cast<std::uint32_t>(std::max(0,x))>=w.width)flags|=0x400;if(static_cast<std::uint32_t>(std::max(0,y))>=w.height)flags|=0x200;
    if(flags)return {-1,flags};if(!w.header)return {-1,1};
    const auto& overflow=w.header->overflow_rectangle;if(w.header->overflow_item>=0 && x>=overflow[0] && x<overflow[2] && y>=overflow[1] && y<overflow[3])return {-1,0x4000};
    for(std::size_t i=0;i<w.header->rectangles.size();++i){const auto& r=w.header->rectangles[i];if(x>=r[0] && x<r[2] && y>=r[1] && y<r[3]){
        if((w.style&0x100U) && y>=(r[1]+r[3])/2){const auto button=filter_rectangle(w,i,true);return {static_cast<std::int32_t>(i),x>=button[0] && x<button[2]?0x20U:0x10U};}
        if(!(w.style&0x800U) && !(w.columns[i].format&0x100) && x>=r[2]-std::min(4,std::max(0,r[2]-r[0])))return {static_cast<std::int32_t>(i),4};
        const auto middle=(w.style&0x100U)?r[1]+(r[3]-r[1])/4:(r[1]+r[3])/2;
        if((w.style&0x400U) && (w.columns[i].format&0x40) && x>=r[0]+6 && x<r[0]+18 && y>=middle-6 && y<middle+6)return {static_cast<std::int32_t>(i),0x1000};
        if(w.columns[i].format&0x1000000){const auto dropdown=dropdown_rectangle(w,i);if(x>=dropdown[0] && x<dropdown[2] && y>=dropdown[1] && y<dropdown[3])return {static_cast<std::int32_t>(i),0x2000};}
        return {static_cast<std::int32_t>(i),2};
    }}return {-1,1};
}
}

void WindowsServices::layout_gui_header(std::uint64_t handle){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return;auto& w=found->second;auto& control=header(w);auto& items=columns(gui_,w);
    const auto order=display_order(items);for(std::size_t i=0;i<order.size();++i){auto& item=items[order[i]];item.order=static_cast<std::int32_t>(i);if(!item.identity)item.identity=gui_.next_item++;}
    for(auto& item:items)item.filter_display=filter_text(item.filter);
    w.columns=items;control.rectangles.resize(items.size());std::int64_t x=0;
    const auto coordinate=[](std::int64_t n){return static_cast<std::int32_t>(std::clamp<std::int64_t>(n,INT32_MIN,INT32_MAX));};
    for(const auto index:order){const auto width=std::max(0,items[index].width);auto left=x,right=x+width;x=right;
        if(w.ex_style&0x400000U){left=static_cast<std::int64_t>(w.width)-right;right=static_cast<std::int64_t>(w.width)-(x-width);}
        control.rectangles[index]={coordinate(left),0,coordinate(right),coordinate(w.height)};
    }
    control.overflow_item=-1;control.overflow_rectangle={};
    if((w.style&0x1000U) && w.width && x>w.width){
        const auto edge=std::min<std::uint32_t>(18,w.width),available=w.width-edge;std::int64_t end=0;
        for(const auto index:order){end+=std::max(0,items[index].width);if(items[index].width>0 && end>available){control.overflow_item=static_cast<std::int32_t>(index);break;}}
        const auto height=coordinate((w.style&0x100U)?w.height/2:w.height);
        control.overflow_rectangle=(w.ex_style&0x400000U)?std::array<std::int32_t,4>{0,0,coordinate(edge),height}:std::array<std::int32_t,4>{coordinate(available),0,coordinate(w.width),height};
    }
    const auto list=gui_.image_lists.find(control.image_lists[0]);w.list_icons=list==gui_.image_lists.end()?std::vector<GuiIcon>{}:list->second.images;
    control.bitmap_images.clear();control.bitmap_images.resize(items.size());
    for(std::size_t i=0;i<items.size();++i)if(const auto bitmap=gui_.bitmaps.find(items[i].bitmap);bitmap!=gui_.bitmaps.end()){
        auto& image=control.bitmap_images[i];image.width=bitmap->second.width;image.height=bitmap->second.height;image.rgba=bitmap->second.pixels;
        if(!bitmap->second.raw_alpha)for(auto& pixel:image.rgba)pixel|=0xff000000U;
    }
    if(const auto editor=gui_.windows.find(control.filter_editor);editor!=gui_.windows.end()){
        const auto item=std::find_if(items.begin(),items.end(),[&](const auto& value){return value.identity==control.filter_item;});auto& child=editor->second;
        if((control.filter_editing || control.filter_item) && (!(w.style&0x100U) || item==items.end())){control.filter_editing=false;control.filter_deadline=0;control.filter_item=0;++control.filter_generation;}
        child.visible=control.filter_editing && w.visible;child.style=(child.style&~0x10000000U)|(child.visible?0x10000000U:0U);gui_set_window_enabled(child,w.enabled);child.font=w.font;child.control_font=w.control_font;
        if(item!=items.end()){const auto r=filter_rectangle(w,static_cast<std::size_t>(item-items.begin()));child.x=r[0]+2;child.y=r[1]+2;child.width=static_cast<std::uint32_t>(std::max(0,r[2]-r[0]-4));child.height=static_cast<std::uint32_t>(std::max(0,r[3]-r[1]-4));}
        if(native_gui_ && child.redraw)present_native_window(editor->first,child);
    }
}

void WindowsServices::sync_gui_list_header(std::uint64_t handle){
    auto& list=gui_.windows.at(handle);const auto found=gui_.windows.find(list.list_header);if(found==gui_.windows.end())return;
    auto& w=found->second;auto& control=header(w);w.width=list.width;w.x=w.y=0;gui_set_window_enabled(w,list.enabled);
    if(!control.inherited_font || w.font==control.inherited_font)w.font=list.font;control.inherited_font=list.font;w.control_font=gui_font(gui_,w.font);
    const auto metrics=native_gui_?native_gui_->font_metrics(w.control_font).value_or(fallback_font_metrics(w.control_font)):fallback_font_metrics(w.control_font);
    w.height=static_cast<std::uint32_t>(std::clamp((metrics.height+8)*((w.style&0x100U)?2:1),1,65535));w.visible=(list.style&3U)==1U && !(list.style&0x4000U);
    w.style=(w.style&~0x10000000U)|(w.visible?0x10000000U:0U);layout_gui_header(list.list_header);
    if(native_gui_ && w.redraw)present_native_window(list.list_header,w);
}

std::optional<std::uint64_t> WindowsServices::gui_header_message(GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& w=gui_.windows.at(message.window);auto& control=header(w);layout_gui_header(message.window);auto& items=columns(gui_,w);const auto m=message.message;const auto index=integer(message.wparam);const auto lp=message.lparam;
    const ItemAbi abi(x86_);const auto valid=[&]{return index>=0 && static_cast<std::size_t>(index)<items.size();};
    const auto update=[&](std::uint64_t result){invalidate_window(message.window);if(w.header_list && gui_.windows.contains(w.header_list))present_gui(w.header_list);else present_gui(message.window);return result;};
    const auto read=[&](std::uint64_t p,bool unicode){return p?(unicode?read_utf16(memory,GuestAddress{p},65535):utf8_to_utf16_lossy(read_ansi(memory,p,65535))):std::u16string{};};
    if(m==0x55 && lp==3)return w.unicode?2:1;
    if(m==0x1200)return items.size();
    if(m==0x1201 || m==0x120a || m==0x1203 || m==0x120b || m==0x1204 || m==0x120c){
        const bool insert=m==0x1201 || m==0x120a,get=m==0x1203 || m==0x120b,unicode=m>=0x120a;
        if(!lp || index<0 || (!insert && !valid()) || (insert && items.size()>=256))return insert?signed_result(-1):0;
        const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{lp},32));if(mask&~0x3ffU){last_error_=50;return insert?signed_result(-1):0;}
        if(get){const auto& item=items[static_cast<std::size_t>(index)];std::vector<std::pair<unsigned,std::pair<std::uint8_t,std::uint64_t>>> fields;
            if(mask&1U)fields.push_back({4,{32,static_cast<std::uint32_t>(item.width)}});
            if(mask&4U)fields.push_back({abi.format,{32,static_cast<std::uint32_t>(item.format)}});
            if(mask&8U)fields.push_back({abi.data,{abi.bits,item.data}});
            if(mask&16U)fields.push_back({abi.bitmap,{abi.bits,item.bitmap}});
            if(mask&32U)fields.push_back({abi.image,{32,static_cast<std::uint32_t>(item.image)}});
            if(mask&128U)fields.push_back({abi.order,{32,static_cast<std::uint32_t>(item.order)}});
            if(mask&512U)fields.push_back({abi.state,{32,item.state}});
            std::uint64_t pointer=0,filter_pointer=0;std::vector<std::uint8_t> text,filter_data;
            if(mask&2U){pointer=memory.load(GuestAddress{lp+abi.text},abi.bits);const auto capacity=integer(memory.load(GuestAddress{lp+abi.count},32));if(pointer && capacity>0){text=text_bytes(item.text,static_cast<std::size_t>(capacity),unicode);memory.check(GuestAddress{pointer},text.size(),Access::Write);}}
            if(mask&256U){fields.push_back({abi.type,{32,item.filter.type}});
                if(!(item.filter.type&0x8000U)){filter_pointer=memory.load(GuestAddress{lp+abi.filter},abi.bits);if(!filter_pointer)return 0;
                    if((item.filter.type&3U)==0){const auto p=memory.load(GuestAddress{filter_pointer},abi.bits);const auto capacity=integer(memory.load(GuestAddress{filter_pointer+abi.unit},32));filter_pointer=p;if(p && capacity>0)filter_data=text_bytes(item.filter.text,static_cast<std::size_t>(capacity),unicode);}
                    else if((item.filter.type&3U)==1){filter_data.resize(4);for(unsigned n=0;n<4;++n)filter_data[n]=static_cast<std::uint8_t>(static_cast<std::uint32_t>(item.filter.number)>>(n*8));}
                    else {filter_data.resize(16);for(unsigned n=0;n<8;++n){filter_data[n*2]=static_cast<std::uint8_t>(item.filter.date[n]);filter_data[n*2+1]=static_cast<std::uint8_t>(item.filter.date[n]>>8);}}
                    if(!filter_data.empty())memory.check(GuestAddress{filter_pointer},filter_data.size(),Access::Write);
                }
            }
            for(const auto& field:fields)memory.check(GuestAddress{lp+field.first},field.second.first/8,Access::Write);
            if(!text.empty())memory.write(GuestAddress{pointer},text);if(!filter_data.empty())memory.write(GuestAddress{filter_pointer},filter_data);for(const auto& field:fields)memory.store(GuestAddress{lp+field.first},field.second.first,field.second.second);return 1;
        }
        GuiListColumn item=insert?GuiListColumn{}:items[static_cast<std::size_t>(index)];if(!read_item(memory,lp,abi,unicode,item,read))return insert?signed_result(-1):0;
        if(insert){const auto at=std::min(static_cast<std::size_t>(index),items.size());const auto position=(mask&128U)?std::clamp(item.order,0,static_cast<std::int32_t>(items.size())):static_cast<std::int32_t>(at);
            for(auto& other:items)if(other.order>=position){++other.order;++other.revision;}item.identity=gui_.next_item++;item.order=position;item.subitem=static_cast<std::int32_t>(at);items.insert(items.begin()+static_cast<std::ptrdiff_t>(at),std::move(item));if(control.focus>=static_cast<std::int32_t>(at))++control.focus;return update(at);}
        const auto old=items[static_cast<std::size_t>(index)].order;if(mask&128U){item.order=old;move_order(items,static_cast<std::size_t>(index),integer(memory.load(GuestAddress{lp+abi.order},32)));item.order=items[static_cast<std::size_t>(index)].order;}
        ++item.revision;items[static_cast<std::size_t>(index)]=std::move(item);return update(1);
    }
    if(m==0x1202){if(!valid())return 0;const auto order=items[static_cast<std::size_t>(index)].order;items.erase(items.begin()+index);for(auto& other:items)if(other.order>order){--other.order;++other.revision;}if(control.focus==index)control.focus=-1;else if(control.focus>index)--control.focus;return update(1);}
    if(m==0x1205){if(!lp)return 0;const auto rect=memory.load(GuestAddress{lp},abi.bits),position=memory.load(GuestAddress{lp+abi.unit},abi.bits);if(!rect || !position)return 0;
        memory.check(GuestAddress{rect},16,Access::Read);memory.check(GuestAddress{rect+4},4,Access::Write);memory.check(GuestAddress{position},x86_?28:40,Access::Write);
        const auto left=integer(memory.load(GuestAddress{rect},32)),top=integer(memory.load(GuestAddress{rect+4},32)),right=integer(memory.load(GuestAddress{rect+8},32)),bottom=integer(memory.load(GuestAddress{rect+12},32));
        const auto metrics=native_gui_?native_gui_->font_metrics(w.control_font).value_or(fallback_font_metrics(w.control_font)):fallback_font_metrics(w.control_font);
        const auto height=(w.style&8U)?0:std::min(std::max(0,bottom-top),std::clamp((metrics.height+8)*((w.style&0x100U)?2:1),1,65535));
        memory.store(GuestAddress{position},abi.bits,message.window);memory.store(GuestAddress{position+abi.unit},abi.bits,0);const std::array<std::int32_t,4> values{left,top,std::max(0,right-left),height};
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{position+abi.unit*2+i*4},32,static_cast<std::uint32_t>(values[i]));memory.store(GuestAddress{position+abi.unit*2+16},32,0x14);memory.store(GuestAddress{rect+4},32,static_cast<std::uint32_t>(top+height));return 1;
    }
    if(m==0x1206){if(!lp)return signed_result(-1);memory.check(GuestAddress{lp},16,Access::Read);memory.check(GuestAddress{lp+8},8,Access::Write);const auto [item,flags]=hit(w,integer(memory.load(GuestAddress{lp},32)),integer(memory.load(GuestAddress{lp+4},32)));memory.store(GuestAddress{lp+8},32,flags);memory.store(GuestAddress{lp+12},32,static_cast<std::uint32_t>(item));return signed_result(item);}
    if(m==0x1207 || m==0x1219){if(!valid() || !lp || (m==0x1219 && !(items[static_cast<std::size_t>(index)].format&0x1000000)))return 0;auto rect=control.rectangles[static_cast<std::size_t>(index)];if(m==0x1219){rect=dropdown_rectangle(w,static_cast<std::size_t>(index));const auto inset=gui_frame_insets(w);for(unsigned n=0;n<4;++n){const auto offset=(n&1U)?static_cast<std::int64_t>(w.y)+inset.top:static_cast<std::int64_t>(w.x)+inset.left;rect[n]=static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(rect[n])+offset,INT32_MIN,INT32_MAX));}}memory.check(GuestAddress{lp},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{lp+i*4},32,static_cast<std::uint32_t>(rect[i]));return 1;}
    if(m==0x1208){if(message.wparam>1 || (lp && !gui_.image_lists.contains(lp)))return 0;const auto old=control.image_lists[message.wparam];control.image_lists[message.wparam]=lp;return update(old);}
    if(m==0x1209)return message.wparam<2?control.image_lists[message.wparam]:0;
    if(m==0x1210){
        if(!valid() || lp)return 0;
        const auto image=render_gui_header_image(w,static_cast<std::size_t>(index));
        if(!image || !image->width || !image->height || image->pixels.empty()){last_error_=8;return 0;}
        return drag_image(gui_,*image);
    }
    if(m==0x120f){const auto order=display_order(items);return valid()?order[static_cast<std::size_t>(index)]:signed_result(-1);}
    if(m==0x1211 || m==0x1212){if(message.wparam!=items.size() || (!lp && !items.empty()))return 0;const auto order=display_order(items);if(m==0x1211){memory.check(GuestAddress{lp},items.size()*4,Access::Write);for(std::size_t i=0;i<order.size();++i)memory.store(GuestAddress{lp+i*4},32,order[i]);return 1;}
        memory.check(GuestAddress{lp},items.size()*4,Access::Read);std::vector<bool> seen(items.size());std::vector<std::uint32_t> requested;requested.reserve(items.size());for(std::size_t i=0;i<items.size();++i){const auto item=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+i*4},32));if(item>=items.size() || seen[item])return 0;seen[item]=true;requested.push_back(item);}for(std::size_t i=0;i<requested.size();++i){auto& item=items[requested[i]];item.order=static_cast<std::int32_t>(i);++item.revision;}return update(1);
    }
    if(m==0x1213){if(message.wparam){const auto [item,flags]=hit(w,static_cast<std::int16_t>(lp),static_cast<std::int16_t>(lp>>16));(void)flags;control.hot_divider=item<0?static_cast<std::int32_t>(items.size()):items[static_cast<std::size_t>(item)].order;}else control.hot_divider=integer(lp);return update(signed_result(control.hot_divider));}
    if(m==0x1214){const auto old=control.bitmap_margin;control.bitmap_margin=std::max(0,integer(message.wparam));return update(signed_result(old));}
    if(m==0x1215)return signed_result(control.bitmap_margin);
    if(m==0x121a){if(message.wparam || !lp || control.overflow_item<0)return 0;const auto origin=gui_client_origin(gui_,message.window);memory.check(GuestAddress{lp},16,Access::Write);for(unsigned n=0;n<4;++n){const auto position=static_cast<std::int64_t>(control.overflow_rectangle[n])+origin[n&1U];memory.store(GuestAddress{lp+n*4},32,static_cast<std::uint32_t>(std::clamp<std::int64_t>(position,INT32_MIN,INT32_MAX)));}return 1;}
    if(m==0x1216){if(message.wparam)return signed_result(-1);control.filter_timeout=static_cast<std::uint32_t>(lp);++control.filter_generation;if(control.filter_deadline)control.filter_deadline=runtime_.clock_ticks+std::uint64_t{control.filter_timeout}*10000;const auto item=std::find_if(items.begin(),items.end(),[&](const auto& value){return value.identity==control.filter_item;});return !control.filter_editing || item==items.end()?signed_result(-1):static_cast<std::uint64_t>(item-items.begin());}
    if(m==0x121b)return signed_result(control.focus);
    if(m==0x121c){const auto target=integer(lp);if(target<-1 || (target>=0 && static_cast<std::size_t>(target)>=items.size()))return 0;control.focus=target;for(std::size_t i=0;i<items.size();++i)items[i].state=i==static_cast<std::size_t>(target)?1U:0U;return update(1);}
    if(m==0x2005){const auto old=w.list_unicode;w.list_unicode=message.wparam!=0;return old?1:0;}
    if(m==0x2006)return w.list_unicode?1:0;
    if(m==0x87)return 3;
    (void)wide;return std::nullopt;
}

bool WindowsServices::begin_gui_header(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& w=gui_.windows.at(message.window);layout_gui_header(message.window);auto& control=header(w);auto& items=columns(gui_,w);const ItemAbi abi(x86_);const auto m=message.message;
    GuiControlOperation op;op.message=message;op.thread=threads_.current;op.wide=wide;op.header.emplace();auto& request=*op.header;
    const auto read=[&](std::uint64_t p,bool unicode){return p?(unicode?read_utf16(memory,GuestAddress{p},65535):utf8_to_utf16_lossy(read_ansi(memory,p,65535))):std::u16string{};};
    const auto done=[&](std::uint64_t value=0){complete_call(state,memory,value);return true;};
    const auto cancel=[&]{control.pressed=0;control.overflow_pressed=false;control.tracking=control.dragging=false;control.drag_order=-1;if(gui_.capture==message.window)gui_.capture=0;present_gui(message.window);};
    const auto filter_event=m==0x1217 || m==0x1218 || m==0x10018 || m==0x10019 || (m==0x111 && message.lparam==control.filter_editor);
    if(filter_event){
        request.kind=GuiControlOperation::Header::Kind::Filter;
        if(!(w.style&0x100U))return done();
        if(m==0x1217){request.index=integer(message.wparam);if(!w.enabled || request.index<0 || static_cast<std::size_t>(request.index)>=items.size())return done();request.edit=true;request.cancel=message.lparam!=0;op.item=items[static_cast<std::size_t>(request.index)].identity;request.return_value=1;}
        else if(m==0x1218){request.index=integer(message.wparam);if(message.lparam || request.index<-1 || (request.index>=0 && static_cast<std::size_t>(request.index)>=items.size()))return done();if(request.index>=0)op.item=items[static_cast<std::size_t>(request.index)].identity;request.cancel=true;request.return_value=1;}
        else if(m==0x10019){if(message.wparam!=control.filter_item || message.lparam!=control.filter_generation || !control.filter_editing)return done();op.item=control.filter_item;request.code=-312;control.filter_deadline=0;}
        else {
            if(!control.filter_editing)return done();op.item=control.filter_item;
            if(m==0x111){const auto notification=static_cast<std::uint32_t>(message.wparam>>16);if(notification==0x300U){
                    const auto item=std::find_if(items.begin(),items.end(),[&](const auto& value){return value.identity==control.filter_item;});const auto editor=gui_.windows.find(control.filter_editor);
                    if(item!=items.end() && editor!=gui_.windows.end()){const auto previous=item->filter;if(edit_filter(item->filter,editor->second.title) && item->filter!=previous){++item->revision;++control.filter_generation;control.filter_deadline=runtime_.clock_ticks+std::uint64_t{control.filter_timeout}*10000;present_gui(message.window);}}return done();
                }if(notification!=0x200U)return done();
            }else {if(message.lparam!=control.filter_generation)return done();request.cancel=message.wparam==27;}
            request.close=true;
        }
    }else if(m==0x1202 && control.filter_editing && integer(message.wparam)>=0 && static_cast<std::size_t>(integer(message.wparam))<items.size() && items[static_cast<std::size_t>(integer(message.wparam))].identity==control.filter_item){
        request.kind=GuiControlOperation::Header::Kind::Filter;request.index=integer(message.wparam);request.close=true;request.notices.push_back({-315,request.index,0});request.return_value=gui_header_message(memory,message,wide).value_or(0);request.generation=control.filter_generation;request.stage=1;
    }else if(m==0x1f && control.filter_editing){request.kind=GuiControlOperation::Header::Kind::Filter;request.cancel=request.close=true;op.item=control.filter_item;cancel();}
    else if(m==0x1f || m==0x215){cancel();return done();}
    else if(m==1 || (m==0x55 && message.lparam==4)){
        request.kind=GuiControlOperation::Header::Kind::Format;if(m==1)w.list_unicode=w.unicode;
    }else if(m==0x1204 || m==0x120c){
        request.kind=GuiControlOperation::Header::Kind::Set;request.index=integer(message.wparam);if(request.index<0 || static_cast<std::size_t>(request.index)>=items.size() || !message.lparam)return done();
        request.candidate=items[static_cast<std::size_t>(request.index)];request.revision=request.candidate.revision;request.mask=static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam},32));
        if(!read_item(memory,message.lparam,abi,m==0x120c,request.candidate,read)){last_error_=50;return done();}op.item=request.candidate.identity;
    }else if(m==0x1210){
        const auto index=integer(message.wparam);if(index<0 || static_cast<std::size_t>(index)>=items.size() || message.lparam)return done();
        request.kind=GuiControlOperation::Header::Kind::Paint;request.drag_image=true;op.owner_draw=true;op.item=items[static_cast<std::size_t>(index)].identity;
    }else if(m==15){
        if(!w.redraw || std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& entry){return entry.second.message.window==message.window && entry.second.message.message==15 && entry.second.header && entry.second.header->kind==GuiControlOperation::Header::Kind::Paint;}))return done();
        request.kind=GuiControlOperation::Header::Kind::Paint;op.owner_draw=true;
        std::erase_if(gui_.queue,[&](const auto& queued){return queued.window==message.window && queued.message==15;});w.invalid=false;
    }else if(m==0x1203 || m==0x120b){
        request.kind=GuiControlOperation::Header::Kind::Display;
        const auto index=integer(message.wparam);if(index<0 || static_cast<std::size_t>(index)>=items.size() || !message.lparam)return false;const auto mask=memory.load(GuestAddress{message.lparam},32);const auto& item=items[static_cast<std::size_t>(index)];if(((mask&2U) && item.text_callback) || ((mask&32U) && item.image_callback))op.identities.push_back(item.identity);
        if(op.identities.empty())return false;
    }else if(m==0x201 || m==0x202 || m==0x200 || m==0x203){
        if(!w.enabled)return done();const auto x=static_cast<std::int16_t>(message.lparam),y=static_cast<std::int16_t>(message.lparam>>16);const auto [index,flags]=hit(w,x,y);
        if((flags&0x4000U) || control.overflow_pressed){
            if(m==0x201 && (flags&0x4000U)){control.overflow_pressed=true;gui_.capture=message.window;present_gui(message.window);return done();}
            if(m==0x200 && (!(message.wparam&1U) || gui_.capture!=message.window)){cancel();return done();}
            if(m!=0x202)return done();const bool clicked=control.overflow_pressed && (flags&0x4000U);control.overflow_pressed=false;if(gui_.capture==message.window)gui_.capture=0;present_gui(message.window);
            if(!clicked || control.overflow_item<0 || static_cast<std::size_t>(control.overflow_item)>=items.size())return done();
            request.kind=GuiControlOperation::Header::Kind::Click;request.code=-319;request.index=control.overflow_item;request.candidate=items[static_cast<std::size_t>(request.index)];request.revision=request.candidate.revision;op.item=request.candidate.identity;
        }else if(index>=0 && (flags&0x30U)){
            if(m==0x201 && (flags&0x10U)){request.kind=GuiControlOperation::Header::Kind::Filter;request.index=index;request.edit=true;op.item=items[static_cast<std::size_t>(index)].identity;}
            else if(m==0x201){control.pressed=items[static_cast<std::size_t>(index)].identity;gui_.capture=message.window;present_gui(message.window);return done();}
            else if(m==0x202 && (flags&0x20U) && control.pressed==items[static_cast<std::size_t>(index)].identity){control.pressed=0;if(gui_.capture==message.window)gui_.capture=0;request.kind=GuiControlOperation::Header::Kind::Filter;request.index=index;request.code=-313;op.item=items[static_cast<std::size_t>(index)].identity;present_gui(message.window);}
            else return done();
        }else {
        // hover 或遺失 capture 不能延續舊按下狀態，否則一般移動就把欄位拖到最尾端。
        if(m==0x200 && control.pressed && (!(message.wparam&1U) || gui_.capture!=message.window)){cancel();return done();}
        if(m==0x201){
            if(index<0)return done();control.pressed=items[static_cast<std::size_t>(index)].identity;control.origin=x;control.original_width=control.tracking_width=items[static_cast<std::size_t>(index)].width;control.tracking=(flags&4U)!=0;control.dragging=false;control.drag_order=-1;control.focus=index;for(std::size_t n=0;n<items.size();++n)items[n].state=n==static_cast<std::size_t>(index)?1U:0U;gui_.capture=message.window;
            if(!control.tracking){present_gui(message.window);return done();}request.kind=GuiControlOperation::Header::Kind::Track;request.code=-306;
        }else if(m==0x203){if(index<0)return done();request.kind=GuiControlOperation::Header::Kind::Click;request.code=(flags&4U)?-305:-303;request.index=index;op.item=items[static_cast<std::size_t>(index)].identity;}
        else {
            const auto item=std::find_if(items.begin(),items.end(),[&](const auto& i){return i.identity==control.pressed;});if(item==items.end())return done();request.index=static_cast<std::int32_t>(item-items.begin());op.item=item->identity;
            if(control.tracking){request.kind=GuiControlOperation::Header::Kind::Track;request.code=m==0x202?-307:-308;control.tracking_width=static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(control.original_width)+(w.ex_style&0x400000U?control.origin-x:x-control.origin),0,INT32_MAX));}
            else if(m==0x200){
                if(!(w.style&0x40U) || std::abs(static_cast<int>(x)-control.origin)<4)return done();request.kind=GuiControlOperation::Header::Kind::Drag;request.code=control.dragging?0:-310;request.order=index<0?static_cast<std::int32_t>(items.size()-1):items[static_cast<std::size_t>(index)].order;
                if(!request.code){control.drag_order=request.order;return done();}
            }else if(control.dragging){request.kind=GuiControlOperation::Header::Kind::Drag;request.code=-311;request.order=control.drag_order;}
            else {if(gui_.capture==message.window)gui_.capture=0;control.pressed=0;present_gui(message.window);if(index!=request.index || (!(w.style&2U) && !(flags&0x1000U)))return done();request.kind=GuiControlOperation::Header::Kind::Click;request.code=(flags&0x1000U)?-316:(flags&0x2000U)?-318:-302;}
        }
        if(!op.item){request.index=index;op.item=items[static_cast<std::size_t>(index)].identity;}
        request.candidate=items[static_cast<std::size_t>(request.index)];request.revision=request.candidate.revision;
        if(request.kind==GuiControlOperation::Header::Kind::Track){request.mask=1;request.candidate.width=control.tracking_width;}
        if(request.kind==GuiControlOperation::Header::Kind::Drag){request.mask=128;request.candidate.order=request.order;}
        }
    }else return false;
    if(gui_.control_operations.size()>=128)throw HostCallFault("Header 回呼超過上限");const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(op));
    try{(void)resume_gui_header(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
}

std::optional<std::uint32_t> WindowsServices::resume_gui_header(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& op=gui_.control_operations.at(token);auto& request=*op.header;const auto found=gui_.windows.find(op.message.window);
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{discard_gui_control(token);complete_call(state,memory,value);return {};};
    if(found==gui_.windows.end())return finish(request.committed?1:0);auto& w=found->second;
    using Kind=GuiControlOperation::Header::Kind;
    if(request.kind==Kind::Format){
        if(!op.phase){op.phase=1;const auto parent=w.parent?w.parent:w.owner;if(gui_.windows.contains(parent)){start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{parent,0x55,op.message.window,3}}},Continuation::GuiControlOperation,token);return {};}}
        if(result==1 || result==2)w.list_unicode=result==2;
        if(op.message.message!=1)return finish(result==1 || result==2?result:0);
        // 通知格式協商完成後仍執行控制項既有的 WM_CREATE 契約，例如 owner-draw Tab 測量。
        const auto message=op.message;const auto wide=op.wide;const auto cls=gui_control_class(w);discard_gui_control(token);
        if(cls==u"systabcontrol32" && begin_gui_tab(state,memory,message,wide))return {};
        if(cls==u"tooltips_class32" && begin_gui_tooltip(state,memory,message,wide))return {};
        const auto value=gui_control_message(memory,message,wide).value_or(0);complete_call(state,memory,value);return {};
    }
    auto& control=header(w);auto& items=columns(gui_,w);const ItemAbi abi(x86_);const GuiControlAbi common(x86_);
    const auto refresh=[&]{if(w.header_list && gui_.windows.contains(w.header_list))present_gui(w.header_list);else present_gui(op.message.window);};
    const auto buffer=[&]{if(!op.buffer){if(gui_.control_notification_buffers.empty())op.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {op.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}}memory.write(GuestAddress{op.buffer},std::vector<std::uint8_t>(gui_control_notification_bytes));};
    if(request.kind==Kind::Paint){
        const auto again=[&](std::vector<GuestCallback> callbacks){start_callbacks(state,memory,std::move(callbacks),Continuation::GuiControlOperation,token);};
        const auto locate=[&]{return std::find_if(items.begin(),items.end(),[&](const auto& item){return item.identity==op.item;});};
        const auto bounds=[&]{return std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(w.width),static_cast<std::int32_t>(w.height)};};
        const auto canvas=[&]{return request.drag_image?std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(request.paint_surface->width()),static_cast<std::int32_t>(request.paint_surface->height())}:bounds();};
        const auto paint_items=[&]{op.identities.clear();if(request.drag_image){if(locate()!=items.end())op.identities.push_back(op.item);}else for(const auto index:display_order(items))op.identities.push_back(items[index].identity);};
        const auto prepare_dc=[&](const std::array<std::int32_t,4>& rectangle){
            auto& dc=gui_.contexts.at(request.paint_dc);dc=GuiDc{};dc.font=w.font?w.font:0xb011;dc.color=gui_system_color(w.enabled?18U:17U);dc.background=gui_system_color(15);dc.mode=1;
            dc.clipped=true;if(request.drag_image){dc.bitmap=request.paint_bitmap;dc.viewport_origin={-request.paint_rectangle[0],-request.paint_rectangle[1]};dc.clip=canvas();}
            else dc.clip={std::max(0,rectangle[0]),std::max(0,rectangle[1]),std::min(static_cast<std::int32_t>(w.width),rectangle[2]),std::min(static_cast<std::int32_t>(w.height),rectangle[3])};
            gui_.dc_objects[request.paint_dc]=dc.font;if(!request.drag_image){w.text_color=dc.color;w.background_mode=dc.mode;}
        };
        const auto custom=[&](std::uint32_t stage){
            const auto parent=w.parent?w.parent:w.owner;if(!gui_.windows.contains(parent))return false;
            const bool item_stage=(stage&0x10000U)!=0;const auto item=locate();if(item_stage && item==items.end())return false;
            buffer();memory.store(GuestAddress{op.buffer},abi.bits,op.message.window);memory.store(GuestAddress{op.buffer+abi.unit},abi.bits,w.id);memory.store(GuestAddress{op.buffer+abi.unit*2},32,static_cast<std::uint32_t>(-12));
            memory.store(GuestAddress{op.buffer+common.notify_size},32,stage);memory.store(GuestAddress{op.buffer+(x86_?16U:32U)},abi.bits,request.paint_dc);
            const auto rectangle=item_stage?control.rectangles[static_cast<std::size_t>(item-items.begin())]:bounds();const auto offset=x86_?20U:40U;
            for(unsigned n=0;n<4;++n)memory.store(GuestAddress{op.buffer+offset+n*4},32,static_cast<std::uint32_t>(rectangle[n]));
            if(item_stage){const auto index=static_cast<std::size_t>(item-items.begin());memory.store(GuestAddress{op.buffer+(x86_?36U:56U)},abi.bits,index);
                const auto flags=(control.pressed==op.item?1U:0U)|(!w.enabled?4U:0U)|((item->state&1U)?16U:0U);
                memory.store(GuestAddress{op.buffer+(x86_?40U:64U)},32,flags);memory.store(GuestAddress{op.buffer+(x86_?44U:72U)},abi.bits,item->data);
            }
            again({{resolve("user32.dll",gui_.windows.at(parent).unicode?"SendMessageW":"SendMessageA").value,{parent,0x4e,w.id,op.buffer}}});return true;
        };
        const auto render_item=[&](bool frame_only){
            const auto item=locate();if(item==items.end() || !request.paint_surface)return;
            const auto index=static_cast<std::size_t>(item-items.begin());GuiWindow model=w;const auto& dc=gui_.contexts.at(request.paint_dc);
            if(request.item_flags&2U)model.control_font=gui_font(gui_,dc.font);
            const GuiHeaderRenderStyle style{dc.color,dc.background,frame_only};const auto image=render_gui_header_image(model,index,style);if(!image)return;
            auto source=make_hardware_raster(image->width,image->height,image->pixels);GuiDc copy;copy.clipped=true;copy.clip=canvas();if(request.drag_image)copy.viewport_origin={-request.paint_rectangle[0],-request.paint_rectangle[1]};
            request.paint_surface->blit(*source,control.rectangles[index],{0,0,static_cast<std::int32_t>(image->width),static_cast<std::int32_t>(image->height)},copy,UINT32_MAX);
        };
        const auto owner=[&]{
            const auto parent=w.parent?w.parent:w.owner;const auto item=locate();if(item==items.end() || !gui_.windows.contains(parent))return false;
            buffer();memory.store(GuestAddress{op.buffer},32,100);memory.store(GuestAddress{op.buffer+4},32,w.id);memory.store(GuestAddress{op.buffer+8},32,static_cast<std::uint32_t>(item-items.begin()));memory.store(GuestAddress{op.buffer+12},32,1);
            memory.store(GuestAddress{op.buffer+16},32,(control.pressed==op.item?1U:0U)|(!w.enabled?4U:0U)|((item->state&1U)?16U:0U));memory.store(GuestAddress{op.buffer+(x86_?20U:24U)},abi.bits,op.message.window);memory.store(GuestAddress{op.buffer+(x86_?24U:32U)},abi.bits,request.paint_dc);
            const auto& rectangle=control.rectangles[static_cast<std::size_t>(item-items.begin())];const auto offset=x86_?28U:40U;for(unsigned n=0;n<4;++n)memory.store(GuestAddress{op.buffer+offset+n*4},32,static_cast<std::uint32_t>(rectangle[n]));memory.store(GuestAddress{op.buffer+(x86_?44U:56U)},abi.bits,item->data);
            again({{resolve("user32.dll",gui_.windows.at(parent).unicode?"SendMessageW":"SendMessageA").value,{parent,0x2b,w.id,op.buffer}}});return true;
        };
        if(!request.stage){
            GuiWindow background=w;background.columns.clear();background.header->overflow_item=-1;
            if(request.drag_image){const auto item=locate();if(item==items.end() || item->width<=0)return finish(0);request.paint_rectangle=control.rectangles[static_cast<std::size_t>(item-items.begin())];background.width=static_cast<std::uint32_t>(item->width);}
            else {
                request.paint_dc=0x10000000+op.message.window;auto& context=gui_.contexts[request.paint_dc];auto& saved=gui_.saved_contexts[request.paint_dc];saved.push_back(context);op.parameter=saved.size();
                request.previous_text_color=w.text_color;request.previous_background_mode=w.background_mode;
                if(const auto selected=gui_.dc_objects.find(request.paint_dc);selected!=gui_.dc_objects.end()){request.previous_dc_object_present=true;request.previous_dc_object=selected->second;}
            }
            const auto image=render_gui_header_image(background);if(!image)return finish(0);
            request.paint_surface=make_hardware_raster(image->width,image->height,image->pixels);
            if(request.drag_image){request.paint_dc=gui_.next_object++;request.paint_bitmap=gui_.next_object++;gui_.contexts[request.paint_dc]={};gui_.bitmaps.emplace(request.paint_bitmap,*image);rasters_[request.paint_bitmap]=request.paint_surface;}
            else {rasters_[request.paint_dc]=request.paint_surface;raster_shared_.erase(request.paint_dc);}
            prepare_dc(bounds());
            request.stage=1;if(custom(1))return {};result=0;
        }
        if(request.stage==1){
            request.paint_flags=static_cast<std::uint32_t>(result);request.custom_paint=request.paint_flags!=0;request.stage=(request.paint_flags&64U)?11U:(request.paint_flags&4U)?5U:2U;
            paint_items();
        }
        for(;;){
            layout_gui_header(op.message.window);
            if(request.drag_image){const auto item=locate();if(item==items.end())return finish(0);const auto rectangle=control.rectangles[static_cast<std::size_t>(item-items.begin())];auto& dc=gui_.contexts.at(request.paint_dc);
                for(unsigned n=0;n<2;++n)dc.viewport_origin[n]=static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(dc.viewport_origin[n])+request.paint_rectangle[n]-rectangle[n],INT32_MIN,INT32_MAX));request.paint_rectangle=rectangle;
            }
            if(request.stage==11){request.stage=12;if(custom(4))return {};}
            if(request.stage==12){request.stage=(request.paint_flags&4U)?5U:2U;paint_items();}
            if(request.stage==2){
                while(op.start<op.identities.size()){
                    op.item=op.identities[op.start];const auto item=locate();if(item==items.end()){++op.start;continue;}
                    const auto& rectangle=control.rectangles[static_cast<std::size_t>(item-items.begin())];if(rectangle[2]<=rectangle[0] || (!request.drag_image && (rectangle[2]<=0 || rectangle[0]>=static_cast<std::int32_t>(w.width)))){++op.start;continue;}
                    prepare_dc(rectangle);request.item_flags=0;
                    if(item->format&0x8000){request.custom_paint=true;render_item(true);request.stage=4;if(owner())return {};result=0;}
                    else if(request.paint_flags&0x20U){request.stage=3;if(custom(0x10001))return {};result=0;}
                    else request.stage=7;
                    break;
                }
                if(op.start>=op.identities.size())request.stage=5;
            }
            if(request.stage==3){request.item_flags=static_cast<std::uint32_t>(result);request.stage=(request.item_flags&4U)?6U:7U;}
            if(request.stage==4)request.stage=result?6U:7U;
            if(request.stage==7){
                const auto item=locate();request.stage=8;
                // 只在預設內容確實需要時取得 callback 文字／圖示；自繪與 SKIPDEFAULT 不觸發提供者。
                if(item!=items.end() && (item->text_callback || item->image_callback)){
                    buffer();memory.store(GuestAddress{op.buffer+128},32,34);again({{resolve("user32.dll","SendMessageW").value,{op.message.window,0x120b,static_cast<std::uint64_t>(item-items.begin()),op.buffer+128}}});return {};
                }
            }
            if(request.stage==8){render_item(false);request.stage=6;}
            if(request.stage==6){request.stage=9;if((request.item_flags&16U) && custom(0x10002))return {};}
            if(request.stage==9){++op.start;request.stage=2;continue;}
            if(request.stage==5){prepare_dc(bounds());request.stage=10;if((request.paint_flags&16U) && custom(2))return {};}
            if(request.stage==10){
                if(request.drag_image)return finish(drag_image(gui_,request.paint_surface->snapshot()));
                if(native_gui_){
                    if(request.custom_paint){
                        auto displayed=make_hardware_raster(request.paint_surface->width(),request.paint_surface->height(),{},native_gui_->graphics_view(op.message.window));GuiDc copy;
                        const std::array<std::int32_t,4> rectangle{0,0,static_cast<std::int32_t>(displayed->width()),static_cast<std::int32_t>(displayed->height())};displayed->blit(*request.paint_surface,rectangle,rectangle,copy,UINT32_MAX);
                        rasters_[request.paint_dc]=displayed;native_gui_->graphics_surface(op.message.window,displayed);displayed->flush();
                    }else native_gui_->graphics_surface(op.message.window,{});
                    present_gui(op.message.window);
                }
                return finish(0);
            }
        }
    }
    const auto store_filter=[&](std::uint64_t p,const GuiHeaderFilter& value){
        memory.store(GuestAddress{p+abi.type},32,value.type);memory.store(GuestAddress{p+abi.filter},abi.bits,op.buffer+160);
        if(value.type&0x8000U)return;
        if((value.type&3U)==0){if(!request.filter_buffer){if(gui_.control_notification_buffers.empty())request.filter_buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {request.filter_buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}}memory.store(GuestAddress{op.buffer+160},abi.bits,request.filter_buffer);memory.store(GuestAddress{op.buffer+160+abi.unit},32,w.list_unicode?2048:4096);memory.write(GuestAddress{request.filter_buffer},text_bytes(value.text,w.list_unicode?2048:4096,w.list_unicode));}
        else if((value.type&3U)==1)memory.store(GuestAddress{op.buffer+160},32,static_cast<std::uint32_t>(value.number));
        else for(unsigned n=0;n<8;++n)memory.store(GuestAddress{op.buffer+160+n*2},16,value.date[n]);
    };
    const auto send=[&](std::int32_t code,bool display=false){
        buffer();memory.store(GuestAddress{op.buffer},abi.bits,op.message.window);memory.store(GuestAddress{op.buffer+abi.unit},abi.bits,w.id);memory.store(GuestAddress{op.buffer+abi.unit*2},32,static_cast<std::uint32_t>(code));
        if(display){const auto ptr=x86_?20U:32U,cap=x86_?24U:40U,image=x86_?28U:44U,data=x86_?32U:48U;
            memory.store(GuestAddress{op.buffer+common.notify_size},32,static_cast<std::uint32_t>(request.index));memory.store(GuestAddress{op.buffer+common.notify_size+4},32,request.mask);
            memory.store(GuestAddress{op.buffer+ptr},abi.bits,op.buffer+256);memory.store(GuestAddress{op.buffer+cap},32,w.list_unicode?2048:4096);memory.store(GuestAddress{op.buffer+image},32,static_cast<std::uint32_t>(request.candidate.image));memory.store(GuestAddress{op.buffer+data},abi.bits,request.candidate.data);
        }else {
            memory.store(GuestAddress{op.buffer+common.notify_size},32,static_cast<std::uint32_t>(request.index));memory.store(GuestAddress{op.buffer+common.notify_size+4},32,static_cast<std::uint32_t>(request.button));memory.store(GuestAddress{op.buffer+common.notify_size+8},abi.bits,request.mask?op.buffer+64:0);
            const auto p=op.buffer+64;memory.store(GuestAddress{p},32,request.mask);memory.store(GuestAddress{p+4},32,static_cast<std::uint32_t>(request.candidate.width));memory.store(GuestAddress{p+abi.format},32,static_cast<std::uint32_t>(request.candidate.format));memory.store(GuestAddress{p+abi.data},abi.bits,request.candidate.data);memory.store(GuestAddress{p+abi.bitmap},abi.bits,request.candidate.bitmap);memory.store(GuestAddress{p+abi.image},32,static_cast<std::uint32_t>(request.candidate.image));memory.store(GuestAddress{p+abi.order},32,static_cast<std::uint32_t>(request.candidate.order));memory.store(GuestAddress{p+abi.state},32,request.candidate.state);
            const auto bytes=text_bytes(request.candidate.text,w.list_unicode?2048:4096,w.list_unicode);memory.write(GuestAddress{op.buffer+256},bytes);memory.store(GuestAddress{p+abi.text},abi.bits,op.buffer+256);memory.store(GuestAddress{p+abi.count},32,w.list_unicode?2048:4096);
            if(request.mask&256U)store_filter(p,request.candidate.filter);
            if(code==-313){const auto rect=filter_rectangle(w,static_cast<std::size_t>(request.index),true);for(unsigned n=0;n<4;++n)memory.store(GuestAddress{op.buffer+common.notify_size+4+n*4},32,static_cast<std::uint32_t>(rect[n]));}
        }
        start_callbacks(state,memory,{{resolve("user32.dll",w.list_unicode?"SendMessageW":"SendMessageA").value,{w.parent,0x4e,w.id,op.buffer}}},Continuation::GuiControlOperation,token);
    };
    if(request.kind==Kind::Filter){
        const auto again=[&](std::vector<GuestCallback> callbacks){start_callbacks(state,memory,std::move(callbacks),Continuation::GuiControlOperation,token);};
        const auto locate=[&](std::uint64_t identity){return std::find_if(items.begin(),items.end(),[&](const auto& item){return item.identity==identity;});};
        const auto notice=[&](std::int32_t code,std::int32_t index){request.notices.push_back({code,index,index>=0 && static_cast<std::size_t>(index)<items.size()?items[static_cast<std::size_t>(index)].identity:0});};
        if(!request.stage){
            const auto active=locate(control.filter_item);const auto old_index=active==items.end()?-1:static_cast<std::int32_t>(active-items.begin());
            if(request.edit || request.close || (op.message.message==0x1218 && (request.index<0 || op.item==control.filter_item))){
                if(control.filter_editing && active!=items.end()){
                    const auto previous=active->filter;const auto pending=control.filter_deadline!=0;
                    if(request.cancel)active->filter=control.filter_original;
                    else if(const auto editor=gui_.windows.find(control.filter_editor);editor!=gui_.windows.end())(void)edit_filter(active->filter,editor->second.title);
                    if(active->filter!=previous){++active->revision;notice(-312,old_index);}
                    else if(pending && !request.cancel)notice(-312,old_index);
                    notice(-315,old_index);request.close=true;
                }
                control.filter_editing=false;control.filter_deadline=0;
            }
            if(op.message.message==0x1218){
                request.notices.clear();if(request.close)notice(-315,old_index);
                for(std::size_t n=0;n<items.size();++n)if(request.index<0 || n==static_cast<std::size_t>(request.index)){auto& item=items[n];item.filter=GuiHeaderFilter{item.filter.type|0x8000U,{},{},{}};++item.revision;}
                notice(-312,request.index);
            }
            if(request.code)notice(request.code,request.index<0?old_index:request.index);
            request.generation=++control.filter_generation;request.stage=1;
        }else if(control.filter_generation!=request.generation)return finish(request.return_value);
        if(request.stage==1){request.stage=2;
            if(request.close && gui_.windows.contains(control.filter_editor)){
                std::vector<GuestCallback> callbacks;if(gui_.focus==control.filter_editor)callbacks.push_back({resolve("user32.dll","SetFocus").value,{op.message.window}});callbacks.push_back({resolve("user32.dll","ShowWindow").value,{control.filter_editor,0}});again(std::move(callbacks));return {};
            }
        }
        if(request.stage==2){
            const auto target=locate(op.item);if(request.edit && (target==items.end() || !(w.style&0x100U) || !w.enabled))return finish(0);
            if(request.edit){control.filter_item=op.item;control.filter_original=target->filter;control.filter_editing=true;op.text=filter_text(target->filter);request.index=static_cast<std::int32_t>(target-items.begin());notice(-314,request.index);
                if(!gui_.windows.contains(control.filter_editor)){buffer();memory.write(GuestAddress{op.buffer+128},utf16_bytes(u"Edit"));const auto rect=filter_rectangle(w,static_cast<std::size_t>(request.index));request.stage=3;
                    again({{resolve("user32.dll","CreateWindowExW").value,{0,op.buffer+128,0,0x40000080U,static_cast<std::uint32_t>(rect[0]+2),static_cast<std::uint32_t>(rect[1]+2),static_cast<std::uint32_t>(std::max(0,rect[2]-rect[0]-4)),static_cast<std::uint32_t>(std::max(0,rect[3]-rect[1]-4)),op.message.window,0,w.instance,0}}});return {};
                }
                request.stage=4;
            }else request.stage=5;
        }
        if(request.stage==3){if(!result || !gui_.windows.contains(result)){control.filter_editing=false;return finish(0);}control.filter_editor=result;request.stage=4;}
        if(request.stage==4){const auto editor=gui_.windows.find(control.filter_editor);if(editor==gui_.windows.end())return finish(0);editor->second.header_owner=op.message.window;editor->second.font=w.font;editor->second.title=op.text;editor->second.edit_start=0;editor->second.edit_end=static_cast<std::uint32_t>(op.text.size());present_gui(op.message.window);request.stage=5;
            again({{resolve("user32.dll","SendMessageW").value,{control.filter_editor,0x30,w.font,1}},{resolve("user32.dll","ShowWindow").value,{control.filter_editor,5}},{resolve("user32.dll","SetFocus").value,{control.filter_editor}},{resolve("user32.dll","SendMessageW").value,{control.filter_editor,0xb1,0,UINT64_MAX}}});return {};
        }
        if(request.stage==6){request.stage=5;if(result)notice(-312,request.index);}
        while(request.next<request.notices.size()){
            const auto sent=request.notices[request.next++];const auto item=locate(sent.identity);if(sent.identity && item==items.end())continue;
            const auto index=sent.identity?static_cast<std::int32_t>(item-items.begin()):sent.index;const auto code=sent.code;request.index=index;request.mask=sent.identity?256U:0U;
            if(item!=items.end())request.candidate=*item;
            if(gui_.windows.contains(w.parent)){request.stage=code==-313?6U:5U;send(code);return {};}
        }
        refresh();return finish(request.return_value);
    }
    if(request.kind==Kind::Display){
        if(op.phase==1){const auto item=std::find_if(items.begin(),items.end(),[&](const auto& i){return i.identity==op.item;});if(item!=items.end() && item->revision==request.revision){
            const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{op.buffer+common.notify_size+4},32));if(mask&2U){const auto pointer=memory.load(GuestAddress{op.buffer+(x86_?20U:32U)},abi.bits);item->text=pointer?(w.list_unicode?read_utf16(memory,GuestAddress{pointer},2048):utf8_to_utf16_lossy(read_ansi(memory,pointer,4096))):std::u16string{};if((mask|request.mask)&64U)item->text_callback=false;}
            if(mask&32U){item->image=integer(memory.load(GuestAddress{op.buffer+(x86_?28U:44U)},32));if((mask|request.mask)&64U)item->image_callback=false;}
        }++op.start;op.phase=0;}
        while(op.start<op.identities.size()){
            const auto item=std::find_if(items.begin(),items.end(),[&](const auto& i){return i.identity==op.identities[op.start];});if(item==items.end()){++op.start;continue;}
            request.index=static_cast<std::int32_t>(item-items.begin());request.mask=(item->text_callback?2U:0U)|(item->image_callback?32U:0U);if(op.message.message!=15 && op.message.message!=0x1210)request.mask&=static_cast<std::uint32_t>(memory.load(GuestAddress{op.message.lparam},32));
            if(op.message.message!=15 && op.message.message!=0x1210 && (memory.load(GuestAddress{op.message.lparam},32)&64U))request.mask|=64U;
            op.item=item->identity;request.candidate=*item;request.revision=item->revision;op.phase=1;
            if(request.mask && gui_.windows.contains(w.parent)){send(w.list_unicode?-329:-309,true);return {};}++op.start;op.phase=0;
        }
        refresh();auto message=op.message;const auto wide=op.wide;if(message.message==15)return finish(0);
        const auto current=std::find_if(items.begin(),items.end(),[&](const auto& i){return i.identity==op.item;});if(current==items.end())return finish(0);
        message.wparam=static_cast<std::uint64_t>(current-items.begin());
        const auto value=gui_header_message(memory,message,wide).value_or(0);return finish(value);
    }
    const auto item=std::find_if(items.begin(),items.end(),[&](const auto& i){return i.identity==op.item;});
    if(item==items.end() || (!request.committed && item->revision!=request.revision))return finish(0);request.index=static_cast<std::int32_t>(item-items.begin());
    if(request.kind==Kind::Click){if(!op.phase){op.phase=1;if(gui_.windows.contains(w.parent)){send(request.code<=-310?request.code:request.code-(w.list_unicode?20:0));return {};}}return finish(0);}
    if(request.kind==Kind::Track || request.kind==Kind::Drag){
        if(!op.phase){op.phase=1;if(gui_.windows.contains(w.parent)){send(request.kind==Kind::Track?request.code-(w.list_unicode?20:0):request.code);return {};}}
        if(op.phase==1){
            if(result && !(request.kind==Kind::Track && request.code==-307)){control.tracking=control.dragging=false;control.pressed=0;if(gui_.capture==op.message.window)gui_.capture=0;refresh();return finish(0);}
            if(request.kind==Kind::Track){
                if(op.buffer)request.candidate.width=std::max(0,integer(memory.load(GuestAddress{op.buffer+64+4},32)));control.tracking_width=request.candidate.width;
                if(request.code==-306 || (request.code==-308 && !(w.style&0x80U)))return finish(0);
                if(request.code==-307){control.tracking=false;control.pressed=0;if(gui_.capture==op.message.window)gui_.capture=0;}
            }else {
                if(request.code==-310){control.dragging=true;control.drag_order=request.order;return finish(0);}
                if(op.buffer)request.candidate.order=integer(memory.load(GuestAddress{op.buffer+64+abi.order},32));control.dragging=false;control.pressed=0;if(gui_.capture==op.message.window)gui_.capture=0;
            }
            op.phase=0;request.kind=Kind::Set;
        }
    }
    if(!op.phase){op.phase=1;if(gui_.windows.contains(w.parent)){send(w.list_unicode?-320:-300);return {};}result=0;}
    if(op.phase==1){if(result)return finish(0);const auto order=item->order;
        if(request.mask&128U)move_order(items,static_cast<std::size_t>(request.index),request.candidate.order);
        request.candidate.order=(request.mask&128U)?item->order:order;request.candidate.revision=item->revision+1;*item=request.candidate;
        if((request.mask&256U) && control.filter_editing && control.filter_item==item->identity){control.filter_original=item->filter;control.filter_deadline=0;++control.filter_generation;if(const auto editor=gui_.windows.find(control.filter_editor);editor!=gui_.windows.end()){editor->second.title=filter_text(item->filter);present_gui(editor->first);}}
        request.committed=true;refresh();op.phase=2;
        if(gui_.windows.contains(w.parent)){send(w.list_unicode?-321:-301);return {};}
    }
    return finish(op.message.message==0x1204 || op.message.message==0x120c?1:0);
}
}
