#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
constexpr unsigned id_chars=48,url_chars=2084;
char16_t lower(char16_t c){return c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;}
bool equal(std::u16string_view a,std::u16string_view b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y){return lower(x)==lower(y);});}
bool space(char16_t c){return c==u' ' || c==u'\t' || c==u'\r' || c==u'\n';}
std::optional<std::u16string> bounded_text(const GuestMemory& memory,std::uint64_t address,unsigned capacity){
    std::u16string text;for(unsigned i=0;i<capacity;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{address+i*2},16));if(!c)return text;text.push_back(c);}return std::nullopt;
}
void write_text(GuestMemory& memory,std::uint64_t address,std::u16string_view text,unsigned capacity){
    std::vector<std::uint8_t> bytes(capacity*2);for(std::size_t i=0;i<std::min(text.size(),static_cast<std::size_t>(capacity-1));++i){bytes[i*2]=static_cast<std::uint8_t>(text[i]);bytes[i*2+1]=static_cast<std::uint8_t>(text[i]>>8);}memory.write(GuestAddress{address},bytes);
}
std::int32_t hit(const GuiWindow& window,std::int32_t x,std::int32_t y){
    if(x<0 || y<0 || static_cast<std::uint32_t>(x)>=window.width || static_cast<std::uint32_t>(y)>=window.height)return -1;
    for(const auto& run:window.link_layout.runs){const auto& r=run.rectangle;if(run.link>=0 && x>=r[0] && y>=r[1] && x<r[2] && y<r[3])return run.link;}return -1;
}
}
void WindowsServices::parse_gui_syslink(GuiWindow& window){
    window.link_text.clear();window.links.clear();window.links_initialized=true;window.link_pressed=-1;
    const std::u16string_view input{window.title};
    for(std::size_t i=0;i<input.size();){
        if(input[i]==u'<' && i+2<input.size() && lower(input[i+1])==u'a' && (space(input[i+2]) || input[i+2]==u'>')){
            const auto end=input.find(u'>',i+2);auto close=end==input.npos?input.npos:input.find(u'<',end+1);
            while(close!=input.npos && !(close+4<=input.size() && equal(input.substr(close,4),u"</a>")))close=input.find(u'<',close+1);
            if(end!=input.npos && close!=input.npos){
                GuiLinkItem link;link.start=static_cast<std::uint32_t>(window.link_text.size());link.state=window.enabled?2U:0U;
                auto p=i+2;while(p<end){
                    while(p<end && space(input[p]))++p;const auto name=p;while(p<end && !space(input[p]) && input[p]!=u'=')++p;
                    const auto attribute=input.substr(name,p-name);while(p<end && space(input[p]))++p;
                    if(p==end || input[p]!=u'=')break;++p;while(p<end && space(input[p]))++p;
                    const auto quote=p<end?input[p]:0;if(quote!=u'\'' && quote!=u'"')break;const auto value=++p;while(p<end && input[p]!=quote)++p;
                    if(p==end)break;
                    if(equal(attribute,u"id"))link.id=input.substr(value,std::min<std::size_t>(p-value,id_chars-1));
                    else if(equal(attribute,u"href"))link.url=input.substr(value,std::min<std::size_t>(p-value,url_chars-1));++p;
                }
                window.link_text.append(input.substr(end+1,close-end-1));link.length=static_cast<std::uint32_t>(window.link_text.size())-link.start;
                window.links.push_back(std::move(link));i=close+4;continue;
            }
        }
        // SysLink 只解析 anchor；其他標記保持可見，不當成完整 HTML 引擎。
        window.link_text.push_back(input[i++]);
    }
    if(!(window.style&4U)){
        std::u16string display;std::vector<std::uint32_t> offsets(window.link_text.size()+1);
        for(std::size_t i=0;i<window.link_text.size();++i){offsets[i]=static_cast<std::uint32_t>(display.size());if(window.link_text[i]!=u'&')display.push_back(window.link_text[i]);else if(i+1<window.link_text.size() && window.link_text[i+1]==u'&'){offsets[++i]=static_cast<std::uint32_t>(display.size());display.push_back(u'&');}}
        offsets.back()=static_cast<std::uint32_t>(display.size());for(auto& link:window.links){const auto end=offsets[link.start+link.length];link.start=offsets[link.start];link.length=end-link.start;}window.link_text=std::move(display);
    }
}
GuiLinkLayout WindowsServices::layout_gui_syslink(const GuiWindow& window,std::uint32_t width){
    GuiLinkLayout layout;if(window.link_text.empty())return layout;
    const auto metrics=native_gui_?native_gui_->font_metrics(window.control_font).value_or(fallback_font_metrics(window.control_font)):fallback_font_metrics(window.control_font);
    const auto height=std::max(1,metrics.height+metrics.external_leading);const auto limit=static_cast<std::int32_t>(std::min(width?width:window.width,static_cast<std::uint32_t>(INT32_MAX)));
    auto measured=native_gui_?native_gui_->text_advances(window.control_font,window.link_text):std::nullopt;
    std::vector<std::int32_t> advances(window.link_text.size());
    for(std::size_t i=0;i<advances.size();++i){
        const auto c=window.link_text[i];advances[i]=std::max(1,metrics.average_width)*(c>=0x2e80?2:1);
        if(measured && measured->size()==advances.size())advances[i]=std::max(0,(*measured)[i]-(i?(*measured)[i-1]:0));
        if(c>=0xdc00 && c<=0xdfff && i && window.link_text[i-1]>=0xd800 && window.link_text[i-1]<=0xdbff){advances[i-1]+=advances[i];advances[i]=0;}
    }
    std::size_t first=0;std::int32_t y=0;
    while(first<advances.size()){
        auto end=first,last_space=first;std::int32_t x=0;
        while(end<advances.size() && window.link_text[end]!=u'\n' && window.link_text[end]!=u'\r'){
            if(limit>0 && x && static_cast<std::int64_t>(x)+advances[end]>limit){if(last_space>first)end=last_space;break;}
            x+=advances[end];++end;if(space(window.link_text[end-1]))last_space=end;
            if(end<advances.size() && window.link_text[end]>=0xdc00 && window.link_text[end]<=0xdfff)++end;
        }
        x=0;const auto begin_run=layout.runs.size();
        for(auto at=first;at<end;){
            std::int32_t link=-1;for(std::size_t n=0;n<window.links.size();++n)if(at>=window.links[n].start && at<window.links[n].start+window.links[n].length){link=static_cast<std::int32_t>(n);break;}
            const auto start=at;const auto left=x;do{x+=advances[at++];}while(at<end && (link>=0?at<window.links[static_cast<std::size_t>(link)].start+window.links[static_cast<std::size_t>(link)].length:std::none_of(window.links.begin(),window.links.end(),[&](const auto& item){return at>=item.start && at<item.start+item.length;})));
            layout.runs.push_back({static_cast<std::uint32_t>(start),static_cast<std::uint32_t>(at-start),link,{left,y,x,y+height}});
        }
        layout.width=std::max(layout.width,static_cast<std::uint32_t>(x));
        if((window.style&0x20U) && limit>x)for(auto n=begin_run;n<layout.runs.size();++n){layout.runs[n].rectangle[0]+=limit-x;layout.runs[n].rectangle[2]+=limit-x;}
        y+=height;first=end;if(first<advances.size() && window.link_text[first]==u'\r')++first;if(first<advances.size() && window.link_text[first]==u'\n')++first;
    }
    layout.height=static_cast<std::uint32_t>(y);return layout;
}
void WindowsServices::notify_gui_syslink(GuestMemory& memory,std::uint64_t handle,std::uint32_t index,std::int32_t code){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return;const auto& window=found->second;
    const auto parent=gui_.windows.find(window.parent);if(parent==gui_.windows.end() || !parent->second.procedure || !window.enabled || index>=window.links.size() || !(window.links[index].state&2U))return;
    if(gui_.control_notifications.size()>=4096 || gui_.sent_messages.size()>=8192)throw HostCallFault("控制項通知佇列超過上限");
    std::uint64_t data;if(gui_.control_notification_buffers.empty())data=runtime_allocate(memory,{},gui_control_notification_bytes);else {data=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
    memory.write(GuestAddress{data},std::array<std::uint8_t,gui_control_notification_bytes>{});
    const std::uint8_t bits=x86_?32:64;const auto offset=x86_?12U:24U;memory.store(GuestAddress{data},bits,handle);memory.store(GuestAddress{data+(x86_?4:8)},bits,window.id);memory.store(GuestAddress{data+(x86_?8:16)},32,static_cast<std::uint32_t>(code));
    const auto& link=window.links[index];memory.store(GuestAddress{data+offset},32,15);memory.store(GuestAddress{data+offset+4},32,index);memory.store(GuestAddress{data+offset+8},32,link.state);memory.store(GuestAddress{data+offset+12},32,31);
    write_text(memory,data+offset+16,link.id,id_chars);write_text(memory,data+offset+112,link.url,url_chars);
    gui_.control_notifications.emplace(data,GuiControlNotification{handle,0,0,false,true});GuiSentMessage request;request.message={window.parent,window.id,data,0x4e};request.receiver=parent->second.thread;request.wide=true;gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
}
std::optional<std::uint64_t> WindowsServices::gui_syslink_message(GuestMemory& memory,const GuiMessage& msg,bool wide){
    auto& window=gui_.windows.at(msg.window);if(!window.links_initialized)parse_gui_syslink(window);
    const auto m=msg.message,wp=static_cast<std::uint32_t>(msg.wparam);const auto lp=msg.lparam;
    const auto update=[&](std::uint64_t result)->std::optional<std::uint64_t>{present_gui(msg.window);return result;};
    const auto focus=[&](std::int32_t index){for(std::size_t n=0;n<window.links.size();++n)window.links[n].state=(window.links[n].state&~1U)|(static_cast<std::int32_t>(n)==index?1U:0U);window.selection=index;};
    if(m==0xc){window.title=lp?(wide?read_utf16(memory,GuestAddress{lp}):utf8_to_utf16_lossy(read_ansi(memory,lp))):std::u16string{};parse_gui_syslink(window);return update(1);}
    if(m==1)return update(0);
    if(m==0x701){const auto layout=layout_gui_syslink(window,wp);if(lp){memory.check(GuestAddress{lp},8,Access::Write);memory.store(GuestAddress{lp},32,layout.width);memory.store(GuestAddress{lp+4},32,layout.height);}return layout.height;}
    if(m==0x700){
        if(wp || !lp)return 0;const auto x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp},32))),y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+4},32)));
        const auto index=hit(window,x,y);if(index<0)return 0;memory.check(GuestAddress{lp+12},4,Access::Write);memory.check(GuestAddress{lp+24},id_chars*2,Access::Write);memory.store(GuestAddress{lp+12},32,static_cast<std::uint32_t>(index));write_text(memory,lp+24,window.links[static_cast<std::size_t>(index)].id,id_chars);return 1;
    }
    if(m==0x702 || m==0x703){
        if(wp || !lp)return 0;const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{lp},32)),index=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+4},32));
        if(!(mask&1U) || (mask&~15U) || index>=window.links.size())return 0;
        const auto state_mask=mask&2U?static_cast<std::uint32_t>(memory.load(GuestAddress{lp+12},32)):0U;if(state_mask&~31U)return 0;
        auto link=window.links[index];
        if(m==0x703){
            if(mask&2U)memory.check(GuestAddress{lp+8},4,Access::Write);if(mask&4U)memory.check(GuestAddress{lp+16},id_chars*2,Access::Write);if(mask&8U)memory.check(GuestAddress{lp+112},url_chars*2,Access::Write);
            if(mask&2U)memory.store(GuestAddress{lp+8},32,link.state&state_mask);if(mask&4U)write_text(memory,lp+16,link.id,id_chars);if(mask&8U)write_text(memory,lp+112,link.url,url_chars);return 1;
        }
        if(mask&2U){const auto state=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+8},32));link.state=(link.state&~state_mask)|(state&state_mask);if(!(link.state&2U))link.state&=~1U;}
        if(mask&4U){const auto id=bounded_text(memory,lp+16,id_chars);if(!id)return 0;link.id=*id;}
        if(mask&8U){const auto url=bounded_text(memory,lp+112,url_chars);if(!url)return 0;if(link.url!=*url)link.state&=~4U;link.url=*url;}
        window.links[index]=std::move(link);if(window.links[index].state&1U)focus(static_cast<std::int32_t>(index));return update(1);
    }
    if(m==7){gui_.focus=msg.window;auto index=window.selection;if(index<0 || static_cast<std::size_t>(index)>=window.links.size() || !(window.links[static_cast<std::size_t>(index)].state&2U)){index=-1;for(std::size_t n=0;n<window.links.size();++n)if(window.links[n].state&2U){index=static_cast<std::int32_t>(n);break;}}focus(index);return update(0);}
    if(m==8){for(auto& link:window.links)link.state&=~1U;window.link_pressed=-1;if(gui_.focus==msg.window)gui_.focus=0;return update(0);}
    if(m==0x87){if(wp==13 && !(window.style&2U))return 4;return 0;}
    if(m==0x100 && wp==13){if(!(window.style&2U) && window.selection>=0)notify_gui_syslink(memory,msg.window,static_cast<std::uint32_t>(window.selection),-4);return 0;}
    if(m==0x200 || m==0x201 || m==0x202){
        const auto index=hit(window,static_cast<std::int16_t>(lp),static_cast<std::int16_t>(lp>>16));
        if(m==0x200){for(std::size_t n=0;n<window.links.size();++n)window.links[n].state=(window.links[n].state&~8U)|(static_cast<std::int32_t>(n)==index && (window.links[n].state&2U)?8U:0U);return update(0);}
        if(m==0x201){window.link_pressed=window.enabled && index>=0 && (window.links[static_cast<std::size_t>(index)].state&2U)?index:-1;if(window.link_pressed>=0){focus(index);gui_.focus=msg.window;}return update(0);}
        const auto pressed=window.link_pressed;window.link_pressed=-1;if(index>=0 && index==pressed)notify_gui_syslink(memory,msg.window,static_cast<std::uint32_t>(index),-2);return update(0);
    }
    return std::nullopt;
}
}
