#include "x64arm/windows_services.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
constexpr std::uint32_t first=0x400;
struct TooltipAbi : GuiControlAbi {
    explicit TooltipAbi(bool x86):GuiControlAbi(x86){}
    unsigned owner()const{return 8;}unsigned id()const{return 8+unit;}
    unsigned rect()const{return 8+unit*2;}unsigned instance()const{return rect()+16;}
    unsigned text()const{return instance()+unit;}unsigned data()const{return text()+unit;}
    unsigned size()const{return data()+unit*2;}
};
std::int32_t signed32(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
bool mouse_message(std::uint32_t code){return code==0x200 || code==0xa0 || code==0x201 || code==0x202 || code==0x204 || code==0x205 || code==0x207 || code==0x208;}
GuiTooltip& tooltip(GuiWindow& window){if(!window.tooltip)window.tooltip.emplace();return *window.tooltip;}
auto find_tool(GuiTooltip& tip,std::uint64_t identity){return std::find_if(tip.tools.begin(),tip.tools.end(),[&](const auto& tool){return tool.identity==identity;});}
bool tool_info(const GuestMemory& memory,std::uint64_t pointer,const TooltipAbi& abi){
    if(!pointer)return false;const auto size=memory.load(GuestAddress{pointer},32);
    if(size!=abi.text()+abi.unit && size!=abi.data()+abi.unit && size!=abi.size())return false;
    memory.check(GuestAddress{pointer},static_cast<std::size_t>(size),Access::Read);return true;
}
auto identify_tool(GuiTooltip& tip,const GuestMemory& memory,std::uint64_t pointer,const TooltipAbi& abi){
    if(!tool_info(memory,pointer,abi))return tip.tools.end();
    const auto owner=memory.load(GuestAddress{pointer+abi.owner()},abi.bits),id=memory.load(GuestAddress{pointer+abi.id()},abi.bits);
    return std::find_if(tip.tools.begin(),tip.tools.end(),[&](const auto& tool){return tool.owner==owner && tool.id==id;});
}
void copy_text(GuestMemory& memory,std::uint64_t pointer,std::u16string_view value,bool wide,std::uint64_t capacity){
    if(!pointer || !capacity)return;
    if(wide){memory.write(GuestAddress{pointer},utf16_bytes(std::u16string(value.substr(0,static_cast<std::size_t>(capacity-1)))));return;}
    auto text=utf16_to_utf8(std::u16string(value));text.resize(std::min<std::size_t>(text.size(),static_cast<std::size_t>(capacity-1)));
    std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{pointer},bytes);
}
void write_tool(GuestMemory& memory,std::uint64_t pointer,const TooltipAbi& abi,const GuiTooltipTool& tool,bool wide){
    const auto size=memory.load(GuestAddress{pointer},32),output=memory.load(GuestAddress{pointer+abi.text()},abi.bits);
    memory.check(GuestAddress{pointer},static_cast<std::size_t>(size),Access::Write);
    if(tool.callback)memory.store(GuestAddress{pointer+abi.text()},abi.bits,UINT64_MAX);
    else if(tool.text_pointer && tool.text_pointer<=65535)memory.store(GuestAddress{pointer+abi.text()},abi.bits,tool.text_pointer);
    else if(output && output!=(abi.unit==4?UINT32_MAX:UINT64_MAX))copy_text(memory,output,tool.text,wide,wide?tool.text.size()+1:utf16_to_utf8(tool.text).size()+1);
    memory.store(GuestAddress{pointer+4},32,tool.flags);memory.store(GuestAddress{pointer+abi.owner()},abi.bits,tool.owner);memory.store(GuestAddress{pointer+abi.id()},abi.bits,tool.id);
    for(unsigned i=0;i<4;++i)memory.store(GuestAddress{pointer+abi.rect()+i*4},32,static_cast<std::uint32_t>(tool.rectangle[i]));
    memory.store(GuestAddress{pointer+abi.instance()},abi.bits,tool.instance);
    if(size>=abi.data()+abi.unit)memory.store(GuestAddress{pointer+abi.data()},abi.bits,tool.data);
    if(size>=abi.size())memory.store(GuestAddress{pointer+abi.data()+abi.unit},abi.bits,0);
}
GuiMessage read_tooltip_message(const GuestMemory& memory,std::uint64_t pointer,const TooltipAbi& abi){
    memory.check(GuestAddress{pointer},abi.unit==4?28:48,Access::Read);
    return {memory.load(GuestAddress{pointer},abi.bits),memory.load(GuestAddress{pointer+abi.unit*2},abi.bits),memory.load(GuestAddress{pointer+abi.unit*3},abi.bits),static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+abi.unit},32))};
}
bool contains(const std::array<std::int32_t,4>& r,std::int64_t x,std::int64_t y){return x>=r[0] && y>=r[1] && x<r[2] && y<r[3];}
std::uint64_t hit_tool(const GuiState& gui,const GuiTooltip& tip,std::uint64_t source,std::int32_t x,std::int32_t y){
    for(const auto& tool:tip.tools){if(tool.flags&0x20U)continue;
        const auto target=(tool.flags&1U)?tool.id:tool.owner;
        const auto window=gui.windows.find(target);if(window==gui.windows.end() || !window->second.visible)continue;
        if(source!=target && (!(tool.flags&1U) || source!=tool.owner))continue;
        const auto from=gui_client_origin(gui,source),to=gui_client_origin(gui,target);
        const auto r=(tool.flags&1U)?std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(window->second.width),static_cast<std::int32_t>(window->second.height)}:tool.rectangle;
        if(contains(r,from[0]+x-to[0],from[1]+y-to[1]))return tool.identity;
    }return 0;
}
}

std::u16string WindowsServices::gui_tooltip_text(GuestMemory& memory,std::uint64_t instance,std::uint64_t pointer,bool wide){
    if(!pointer || pointer==(x86_?UINT32_MAX:UINT64_MAX))return {};
    if(pointer>65535)return wide?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16_lossy(read_ansi(memory,pointer,32768));
    const auto module=instance?instance:runtime_.image.base;const auto resource=gui_resource(memory,module,(pointer>>4)+1,6,true);
    const auto image=resource_image(module);if(!resource || !image)return {};
    auto address=image->base+memory.load(GuestAddress{resource},32);const auto size=memory.load(GuestAddress{resource+4},32);
    if(size>1024*1024)return {};memory.check(GuestAddress{address},static_cast<std::size_t>(size),Access::Read);const auto end=address+size;
    for(unsigned i=0;i<16 && end-address>=2;++i){const auto length=memory.load(GuestAddress{address},16);address+=2;if(length>(end-address)/2)return {};
        if(i==(pointer&15)){std::u16string value;value.reserve(length);for(std::uint64_t j=0;j<length;++j)value.push_back(static_cast<char16_t>(memory.load(GuestAddress{address+j*2},16)));return value;}address+=length*2;
    }return {};
}

void WindowsServices::layout_gui_tooltip(GuiWindow& window){
    auto& tip=tooltip(window);const auto metrics=native_gui_?native_gui_->font_metrics(window.control_font).value_or(fallback_font_metrics(window.control_font)):fallback_font_metrics(window.control_font);
    tip.line_height=static_cast<std::uint32_t>(std::max(1,metrics.height));tip.lines.clear();
    std::u16string text;for(std::size_t i=0;i<window.title.size();++i){const auto c=window.title[i];if(c==u'\r')continue;if(c==u'&' && !(window.style&2U)){if(i+1<window.title.size() && window.title[i+1]==u'&'){text.push_back(c);++i;}}else text.push_back(c);}
    const auto measure=[&](const std::u16string& value){return native_gui_?native_gui_->text_width(window.control_font,value).value_or(static_cast<std::int32_t>(value.size())*metrics.average_width):static_cast<std::int32_t>(value.size())*metrics.average_width;};
    std::int32_t width=0;std::size_t begin=0;
    while(begin<=text.size() && tip.lines.size()<512){const auto found=text.find(u'\n',begin),end=found==text.npos?text.size():found;const auto paragraph=text.substr(begin,end-begin);
        const auto advances=native_gui_?native_gui_->text_advances(window.control_font,paragraph):std::nullopt;
        const auto position=[&](std::size_t at){return at && advances && advances->size()==paragraph.size()?(*advances)[at-1]:static_cast<std::int32_t>(at)*metrics.average_width;};
        if(tip.maximum_width<=0 || paragraph.empty()){tip.lines.push_back(paragraph);width=std::max(width,measure(paragraph));}
        else for(std::size_t at=0;at<paragraph.size() && tip.lines.size()<512;){auto last=at,space=at;
            while(last<paragraph.size() && (last==at || position(last+1)-position(at)<=tip.maximum_width)){if(paragraph[last]==u' ')space=last;++last;}
            if(last<paragraph.size() && space>at)last=space;
            if(last<paragraph.size() && last>at && paragraph[last]>=0xdc00 && paragraph[last]<=0xdfff && paragraph[last-1]>=0xd800 && paragraph[last-1]<=0xdbff){if(last-at==1)++last;else --last;}
            tip.lines.push_back(paragraph.substr(at,last-at));width=std::max(width,measure(tip.lines.back()));at=last;while(at<paragraph.size() && paragraph[at]==u' ')++at;
        }
        if(found==text.npos)break;begin=end+1;
    }
    tip.title_height=tip.title.empty()?0:std::max<std::uint32_t>(tip.line_height,tip.icon>=4 && tip.icon<=6?32U:16U)+4;
    if(!tip.title.empty())width=std::max(width,measure(tip.title)+8+(tip.icon?tip.icon>=4 && tip.icon<=6?36:20:0)+((window.style&0x80U)?20:0));
    const auto horizontal=static_cast<std::int64_t>(tip.margin[0])+tip.margin[2]+8,vertical=static_cast<std::int64_t>(tip.margin[1])+tip.margin[3]+8;
    window.width=static_cast<std::uint32_t>(std::clamp<std::int64_t>(width+horizontal,8,16384));
    window.height=static_cast<std::uint32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(tip.lines.size())*tip.line_height+tip.title_height+vertical,8,16384));
    window.list_icons.clear();if(tip.icon>6)if(const auto icon=gui_.icons.find(tip.icon);icon!=gui_.icons.end())window.list_icons.push_back(icon->second);
}

void WindowsServices::hide_gui_tooltip(std::uint64_t handle){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end() || !found->second.tooltip)return;auto& window=found->second;auto& tip=*window.tooltip;
    gui_.timers.erase({handle,tip.timer});tip.shown=0;window.visible=false;window.style&=~0x10000000U;
    present_gui(handle);if(native_gui_)show_native_window(handle,false);
}
void WindowsServices::relay_gui_tooltip(std::uint64_t handle,const GuiMessage& message){
    if(!mouse_message(message.message))return;auto& window=gui_.windows.at(handle);auto& tip=tooltip(window);if(tip.tracking || !tip.active)return;
    if(message.message!=0x200 && message.message!=0xa0){tip.suppressed=true;++tip.revision;hide_gui_tooltip(handle);return;}
    auto x=static_cast<std::int32_t>(static_cast<std::int16_t>(message.lparam)),y=static_cast<std::int32_t>(static_cast<std::int16_t>(message.lparam>>16));
    const auto origin=gui_client_origin(gui_,message.window);if(message.message==0xa0){x-=static_cast<std::int32_t>(origin[0]);y-=static_cast<std::int32_t>(origin[1]);}
    auto current=hit_tool(gui_,tip,message.window,x,y);
    if(!(window.style&1U)){auto root=message.window;for(unsigned depth=0;depth<64 && gui_.windows.contains(root) && gui_.windows.at(root).parent;++depth)root=gui_.windows.at(root).parent;if(root!=gui_.active)current=0;}
    const std::array<std::int32_t,2> point{static_cast<std::int32_t>(std::clamp<std::int64_t>(origin[0]+x,INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<std::int64_t>(origin[1]+y,INT32_MIN,INT32_MAX))};
    const bool changed=current!=tip.current,moved=point!=tip.point,was_visible=window.visible;tip.point=point;
    if(changed){tip.reshow=was_visible;tip.suppressed=false;tip.current=current;++tip.revision;hide_gui_tooltip(handle);}
    if(!current || tip.suppressed || window.visible)return;
    if(changed || moved || !gui_.timers.contains({handle,tip.timer})){
        const auto interval=std::max<std::uint64_t>(1,tip.delays[tip.reshow?1:3])*10000;
        gui_.timers[{handle,tip.timer}]={runtime_.clock_ticks+interval,interval,0,window.thread,0};
    }
}

std::optional<std::uint64_t> WindowsServices::gui_tooltip_message(GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& window=gui_.windows.at(message.window);auto& tip=tooltip(window);const TooltipAbi abi(x86_);const auto code=message.message;const auto wp=message.wparam,lp=message.lparam;
    const bool unicode=code>=first+50 && code<=first+59;
    const auto refresh=[&](std::uint64_t result){present_gui(message.window);return result;};
    if(code==1){window.style=(window.style&0xffffU)|0x80000000U;window.ex_style|=0x08000088U;window.visible=false;tip.timer=gui_.next_object++;return refresh(0);}
    if(code==first+1){tip.active=wp!=0;++tip.revision;if(!tip.active)hide_gui_tooltip(message.window);return 0;}
    if(code==0x2005){const auto old=window.list_unicode;window.list_unicode=wp!=0;return old?1:0;}if(code==0x2006)return window.list_unicode?1:0;
    if(code==first+3){if(wp>3)return 0;const auto value=signed32(lp)<0?500U:static_cast<std::uint32_t>(lp)&65535U;
        if(!wp)tip.delays={0,value/5,value*10,value};else tip.delays[wp]=signed32(lp)<0?(wp==1?100U:wp==2?5000U:500U):value;return 0;}
    if(code==first+21)return wp>=1 && wp<=3?tip.delays[wp]:0;
    if(code==first+13)return tip.tools.size();
    const bool add=code==first+4 || code==first+50,set=code==first+9 || code==first+54,update=code==first+12 || code==first+57;
    if(add || set || update){
        if(!tool_info(memory,lp,abi))return 0;auto found=identify_tool(tip,memory,lp,abi);if((add && found!=tip.tools.end()) || (!add && found==tip.tools.end()) || (add && tip.tools.size()>=65536))return 0;
        GuiTooltipTool tool=add?GuiTooltipTool{}:*found;tool.wide=unicode;tool.instance=memory.load(GuestAddress{lp+abi.instance()},abi.bits);tool.text_pointer=memory.load(GuestAddress{lp+abi.text()},abi.bits);
        tool.callback=tool.text_pointer==(x86_?UINT32_MAX:UINT64_MAX);tool.text=tool.callback?std::u16string{}:gui_tooltip_text(memory,tool.instance,tool.text_pointer,unicode);
        if(!update){tool.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+4},32));tool.owner=memory.load(GuestAddress{lp+abi.owner()},abi.bits);tool.id=memory.load(GuestAddress{lp+abi.id()},abi.bits);
            if(!gui_.windows.contains(tool.owner) || ((tool.flags&1U) && !gui_.windows.contains(tool.id)))return 0;
            for(unsigned i=0;i<4;++i)tool.rectangle[i]=signed32(memory.load(GuestAddress{lp+abi.rect()+i*4},32));
            if(memory.load(GuestAddress{lp},32)>=abi.data()+abi.unit)tool.data=memory.load(GuestAddress{lp+abi.data()},abi.bits);
        }
        ++tip.revision;if(add){tool.identity=gui_.next_object++;tip.tools.push_back(std::move(tool));}else *found=std::move(tool);return add?1:0;
    }
    if(code==first+5 || code==first+51){const auto found=identify_tool(tip,memory,lp,abi);if(found==tip.tools.end())return 0;const auto identity=found->identity;
        if(tip.shown==identity)hide_gui_tooltip(message.window);if(tip.current==identity){tip.current=0;tip.tracking=false;gui_.timers.erase({message.window,tip.timer});}tip.tools.erase(found);++tip.revision;return 0;}
    if(code==first+6 || code==first+52){const auto found=identify_tool(tip,memory,lp,abi);if(found!=tip.tools.end()){for(unsigned i=0;i<4;++i)found->rectangle[i]=signed32(memory.load(GuestAddress{lp+abi.rect()+i*4},32));++tip.revision;}return 0;}
    if(code==first+8 || code==first+53 || code==first+14 || code==first+58 || code==first+15 || code==first+59){
        const bool enumeration=code==first+14 || code==first+58,current=code==first+15 || code==first+59;
        if(current && !lp)return tip.current?1:0;if(!tool_info(memory,lp,abi))return 0;
        const auto found=enumeration?(wp<tip.tools.size()?tip.tools.begin()+static_cast<std::ptrdiff_t>(wp):tip.tools.end()):current?find_tool(tip,tip.current):identify_tool(tip,memory,lp,abi);
        if(found==tip.tools.end())return 0;write_tool(memory,lp,abi,*found,unicode);return 1;
    }
    if(code==first+10 || code==first+55){if(!lp)return 0;const auto info=lp+abi.unit+8;if(!tool_info(memory,info,abi))return 0;
        const auto owner=memory.load(GuestAddress{lp},abi.bits);const auto x=signed32(memory.load(GuestAddress{lp+abi.unit},32)),y=signed32(memory.load(GuestAddress{lp+abi.unit+4},32));
        const auto found=find_tool(tip,hit_tool(gui_,tip,owner,x,y));if(found==tip.tools.end())return 0;write_tool(memory,info,abi,*found,unicode);return 1;}
    if(code==first+7){if(lp)relay_gui_tooltip(message.window,read_tooltip_message(memory,lp,abi));return 0;}
    if(code==first+16){if(!lp)return 0;return gui_mouse_hit(gui_,signed32(memory.load(GuestAddress{lp},32)),signed32(memory.load(GuestAddress{lp+4},32)),gui_.input_desktop).first;}
    if(code==first+17){const auto found=identify_tool(tip,memory,lp,abi);if(found==tip.tools.end())return 0;
        if(!wp){if(tip.current==found->identity){hide_gui_tooltip(message.window);tip.tracking=false;++tip.revision;}}else {if(tip.shown && tip.shown!=found->identity)hide_gui_tooltip(message.window);tip.current=found->identity;tip.tracking=true;tip.suppressed=false;++tip.revision;}return 0;}
    if(code==first+18){const std::array<std::int32_t,2> point{static_cast<std::int16_t>(lp),static_cast<std::int16_t>(lp>>16)};if(tip.point!=point){tip.point=point;++tip.revision;}return 0;}
    if(code==first+19 || code==first+20){(code==first+19?tip.background:tip.color)=static_cast<std::uint32_t>(wp)&0xffffffU;return refresh(0);}
    if(code==first+22)return tip.background;if(code==first+23)return tip.color;
    if(code==first+24){const auto old=tip.maximum_width;tip.maximum_width=signed32(lp);return refresh(static_cast<std::uint64_t>(static_cast<std::int64_t>(old)));}
    if(code==first+25)return static_cast<std::uint64_t>(static_cast<std::int64_t>(tip.maximum_width));
    if(code==first+26 || code==first+27){if(!lp)return 0;memory.check(GuestAddress{lp},16,code==first+26?Access::Read:Access::Write);
        for(unsigned i=0;i<4;++i){if(code==first+26)tip.margin[i]=std::clamp(signed32(memory.load(GuestAddress{lp+i*4},32)),0,4096);else memory.store(GuestAddress{lp+i*4},32,static_cast<std::uint32_t>(tip.margin[i]));}return code==first+26?refresh(0):0;}
    if(code==first+28){tip.suppressed=true;++tip.revision;hide_gui_tooltip(message.window);return 0;}
    if(code==0x202 && (window.style&0x80U) && !tip.title.empty()){
        const auto x=static_cast<std::int16_t>(lp),y=static_cast<std::int16_t>(lp>>16);
        if(x>=static_cast<std::int64_t>(window.width)-22 && static_cast<std::int64_t>(x)<window.width && y>=0 && y<24){tip.suppressed=true;++tip.revision;hide_gui_tooltip(message.window);}return 0;
    }
    if(code==first+31){if(!lp)return 0;memory.check(GuestAddress{lp},16,Access::Read);memory.check(GuestAddress{lp},16,Access::Write);
        for(unsigned i=0;i<4;++i){const auto delta=static_cast<std::int64_t>(tip.margin[i])+4;const auto result=static_cast<std::int64_t>(signed32(memory.load(GuestAddress{lp+i*4},32)))+((i<2) == (wp!=0)?-delta:delta);memory.store(GuestAddress{lp+i*4},32,static_cast<std::uint32_t>(std::clamp<std::int64_t>(result,INT32_MIN,INT32_MAX)));}return 1;}
    if(code==first+32 || code==first+33){if(wp>6 && !gui_.icons.contains(wp))return 0;tip.title=lp?(code==first+33?read_utf16(memory,GuestAddress{lp},100):utf8_to_utf16_lossy(read_ansi(memory,lp,400))):std::u16string{};tip.icon=tip.title.empty()?0:wp;return refresh(1);}
    if(code==first+35){if(!lp)return 0;const auto offset=x86_?12U:16U;if(memory.load(GuestAddress{lp},32)!=offset+abi.unit)return 0;
        const auto capacity=memory.load(GuestAddress{lp+8},32),pointer=memory.load(GuestAddress{lp+offset},abi.bits);copy_text(memory,pointer,tip.title,true,capacity);memory.store(GuestAddress{lp+4},32,tip.icon);return 0;}
    if(code==0x113 && wp==tip.timer){gui_.timers.erase({message.window,tip.timer});if(window.visible){tip.suppressed=true;++tip.revision;hide_gui_tooltip(message.window);}return 0;}
    if(code==first+29 || code==first+30 || code==first+34 || code==first+11 || code==first+56)return 0;
    (void)wide;return std::nullopt;
}

bool WindowsServices::begin_gui_tooltip_relay(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide,bool fixed,std::uint64_t value){
    // 0x10012 只轉送已由原生控制項處理的滑鼠事件，不再次觸發按鈕 click。
    const bool native=message.message==0x10012;
    if(!mouse_message(native?static_cast<std::uint32_t>(message.wparam):message.message))return false;
    for(auto& [token,operation]:gui_.control_operations){(void)token;
        if(operation.thread==threads_.current && operation.tooltip && operation.tooltip->kind==GuiTooltipOperation::Kind::Relay && operation.phase==2 && operation.message.window==message.window && operation.message.message==message.message && operation.message.wparam==message.wparam && operation.message.lparam==message.lparam){operation.phase=3;return false;}
    }
    GuiTooltipOperation relay;relay.kind=GuiTooltipOperation::Kind::Relay;relay.return_fixed=fixed;relay.return_value=value;
    for(const auto& [handle,window]:gui_.windows)if(window.tooltip && window.thread==threads_.current && window.tooltip->active && !window.tooltip->tracking &&
        (window.tooltip->current || std::any_of(window.tooltip->tools.begin(),window.tooltip->tools.end(),[&](const auto& tool){return (tool.flags&0x10U) && !(tool.flags&0x20U) && ((tool.flags&1U)?tool.id:tool.owner)==message.window;})))relay.targets.push_back(handle);
    if(relay.targets.empty()){if(native)complete_call(state,memory,0);return native;}if(gui_.control_operations.size()>=128)throw HostCallFault("Tooltip callback 操作超過上限");
    GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.tooltip=std::move(relay);
    const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
    try{(void)resume_gui_tooltip(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
}

bool WindowsServices::begin_gui_tooltip(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide){
    auto& window=gui_.windows.at(message.window);auto& tip=tooltip(window);const TooltipAbi abi(x86_);const auto code=message.message;
    const bool was_visible=window.visible;const auto old=find_tool(tip,tip.shown);const auto previous=old==tip.tools.end()?std::optional<GuiTooltipTool>{}:*old;
    const bool timer=code==0x113 && message.wparam==tip.timer,text=code==first+11 || code==first+56 || code==first+30;
    const auto value=gui_tooltip_message(memory,message,wide);if(!value)return false;
    const bool refreshing=(code==first+9 || code==first+54 || code==first+12 || code==first+57) && window.visible;
    const bool show=(code==first+17 && message.wparam) || code==first+34 || (code==first+29 && window.visible) || (code==first+18 && tip.tracking && window.visible) || (timer && !was_visible && !tip.suppressed) || refreshing;
    const auto item=text?identify_tool(tip,memory,message.lparam,abi):find_tool(tip,tip.current);
    GuiTooltipOperation details;details.return_value=*value;details.previous=previous;details.was_visible=was_visible && window.visible && tip.shown==tip.current;details.revision=tip.revision;
    if((text || show) && item!=tip.tools.end() && (text || tip.active)){
        details.kind=text?GuiTooltipOperation::Kind::Text:GuiTooltipOperation::Kind::Show;details.tool=*item;
    }else if(previous && was_visible && !window.visible){details.kind=GuiTooltipOperation::Kind::Pop;details.tool=*previous;}
    else {complete_call(state,memory,*value);return true;}
    if(gui_.control_operations.size()>=128)throw HostCallFault("Tooltip callback 操作超過上限");
    GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=window.list_unicode;operation.tooltip=std::move(details);
    const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
    try{(void)resume_gui_tooltip(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
}

std::optional<std::uint32_t> WindowsServices::resume_gui_tooltip(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& operation=gui_.control_operations.at(token);auto& details=*operation.tooltip;const TooltipAbi abi(x86_);
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{discard_gui_control(token);complete_call(state,memory,value);return {};};
    const auto allocate=[&]{if(operation.buffer)return;if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}};
    const auto send=resolve("user32.dll","SendMessageW").value;
    if(details.kind==GuiTooltipOperation::Kind::Relay){
        if(!operation.phase){allocate();const auto buffer=operation.buffer;memory.write(GuestAddress{buffer},std::array<std::uint8_t,48>{});
            const bool native=operation.message.message==0x10012;
            memory.store(GuestAddress{buffer},abi.bits,operation.message.window);memory.store(GuestAddress{buffer+abi.unit},32,native?operation.message.wparam:operation.message.message);memory.store(GuestAddress{buffer+abi.unit*2},abi.bits,native?0:operation.message.wparam);memory.store(GuestAddress{buffer+abi.unit*3},abi.bits,operation.message.lparam);
            std::vector<GuestCallback> callbacks;for(const auto target:details.targets)callbacks.push_back({send,{target,first+7,0,buffer}});operation.phase=1;start_callbacks(state,memory,std::move(callbacks),Continuation::GuiControlOperation,token);return {};
        }
        if(operation.phase==1){if(operation.message.message==0x10012 || !gui_.windows.contains(operation.message.window))return finish(details.return_fixed?details.return_value:0);operation.phase=2;
            start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{operation.message.window,operation.message.message,operation.message.wparam,operation.message.lparam}}},Continuation::GuiControlOperation,token);return {};
        }return finish(details.return_fixed?details.return_value:result);
    }
    const auto found=gui_.windows.find(operation.message.window);if(found==gui_.windows.end() || !found->second.tooltip)return finish(0);
    auto& window=found->second;auto& tip=*window.tooltip;
    const auto notify=[&](const GuiTooltipTool& tool,std::int32_t code){
        if(!gui_.windows.contains(tool.owner))return false;allocate();memory.write(GuestAddress{operation.buffer},std::array<std::uint8_t,256>{});
        memory.store(GuestAddress{operation.buffer},abi.bits,operation.message.window);memory.store(GuestAddress{operation.buffer+abi.unit},abi.bits,tool.id);memory.store(GuestAddress{operation.buffer+abi.notify_code},32,static_cast<std::uint32_t>(code));
        if(code==-520 || code==-530){const auto inline_text=abi.notify_size+abi.unit,instance=inline_text+(operation.wide?160U:80U),flags=instance+abi.unit,data=(flags+4+abi.unit-1)/abi.unit*abi.unit;
            memory.store(GuestAddress{operation.buffer+abi.notify_size},abi.bits,operation.buffer+inline_text);memory.store(GuestAddress{operation.buffer+instance},abi.bits,tool.instance);memory.store(GuestAddress{operation.buffer+flags},32,tool.flags);memory.store(GuestAddress{operation.buffer+data},abi.bits,tool.data);
        }
        start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{tool.owner,0x4e,tool.id,operation.buffer}}},Continuation::GuiControlOperation,token);return true;
    };
    if(details.kind==GuiTooltipOperation::Kind::Pop){if(!operation.phase){operation.phase=1;if(notify(details.tool,-522))return {};}return finish(details.return_value);}
    if(tip.revision!=details.revision || find_tool(tip,details.tool.identity)==tip.tools.end())return finish(0);
    if(!operation.phase){operation.phase=1;if(details.kind==GuiTooltipOperation::Kind::Show && details.previous && details.previous->identity!=details.tool.identity){hide_gui_tooltip(operation.message.window);if(notify(*details.previous,-522))return {};}}
    if(operation.phase==1){operation.phase=2;operation.text=details.tool.text;if(details.tool.callback && notify(details.tool,operation.wide?-530:-520))return {};}
    else if(operation.phase==2 && details.tool.callback){
        const auto inline_text=abi.notify_size+abi.unit,instance=inline_text+(operation.wide?160U:80U),flags=instance+abi.unit;
        const auto pointer=memory.load(GuestAddress{operation.buffer+abi.notify_size},abi.bits),module=memory.load(GuestAddress{operation.buffer+instance},abi.bits);
        operation.text=gui_tooltip_text(memory,module,pointer,operation.wide);
        if(memory.load(GuestAddress{operation.buffer+flags},32)&0x8000U){auto actual=find_tool(tip,details.tool.identity);actual->callback=false;actual->text_pointer=0;actual->instance=0;actual->text=operation.text;}
    }
    if(operation.phase<=2){
        if(details.kind==GuiTooltipOperation::Kind::Text){
            if(operation.message.message==first+30){auto model=window;model.title=operation.text;layout_gui_tooltip(model);return finish((model.width&65535U)|((model.height&65535U)<<16));}
            if(tool_info(memory,operation.message.lparam,abi)){const auto pointer=memory.load(GuestAddress{operation.message.lparam+abi.text()},abi.bits);copy_text(memory,pointer,operation.text,operation.message.message==first+56,operation.message.wparam);}
            return finish(0);
        }
        if(operation.text.empty() || !tip.active || tip.current!=details.tool.identity){hide_gui_tooltip(operation.message.window);return finish(0);}
        window.title=operation.text;window.control_font=gui_font(gui_,window.font);tip.rtl=(details.tool.flags&4U)!=0;layout_gui_tooltip(window);
        std::int64_t x=tip.point[0],y=tip.point[1];
        if(!(tip.tracking && (details.tool.flags&0x80U))){y+=20;if(details.tool.flags&2U){const auto origin=gui_client_origin(gui_,(details.tool.flags&1U)?details.tool.id:details.tool.owner);
                const auto target=gui_.windows.find(details.tool.id);const auto rect=(details.tool.flags&1U) && target!=gui_.windows.end()?std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(target->second.width),static_cast<std::int32_t>(target->second.height)}:details.tool.rectangle;
                x=origin[0]+(static_cast<std::int64_t>(rect[0])+rect[2]-window.width)/2;y=origin[1]+rect[3]+2;}}
        const auto area=gui_work_area();x=std::clamp<std::int64_t>(x,area[0],std::max<std::int64_t>(area[0],static_cast<std::int64_t>(area[2])-window.width));y=std::clamp<std::int64_t>(y,area[1],std::max<std::int64_t>(area[1],static_cast<std::int64_t>(area[3])-window.height));
        details.position={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y)};operation.phase=3;
        if(!details.was_visible && notify(details.tool,-521))return {};result=0;
    }
    if(!result || details.was_visible){window.x=details.position[0];window.y=details.position[1];}
    tip.shown=details.tool.identity;window.visible=true;window.style|=0x10000000U;present_gui(operation.message.window);if(native_gui_)show_native_window(operation.message.window,true);
    if(!tip.tracking){const auto interval=std::max<std::uint64_t>(1,tip.delays[2])*10000;gui_.timers[{operation.message.window,tip.timer}]={runtime_.clock_ticks+interval,interval,0,window.thread,0};}
    return finish(details.return_value);
}
}
