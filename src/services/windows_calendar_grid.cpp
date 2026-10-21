#include "x64arm/windows_calendar.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>

namespace x64arm {
namespace {
using Rect=std::array<std::int32_t,4>;
std::u16string number(int value,unsigned count=1){auto text=utf8_to_utf16_lossy(std::to_string(value));if(text.size()<count)text.insert(0,count-text.size(),u'0');return text;}
GuiSystemTime date(int year,unsigned month,unsigned day){return gui_date_normalize({static_cast<std::uint16_t>(year),static_cast<std::uint16_t>(month),0,static_cast<std::uint16_t>(day),0,0,0,0},false);}
GuiSystemTime month_end(int year,unsigned month){using namespace std::chrono;return date(year,month,unsigned(year_month_day_last{std::chrono::year{year},month_day_last{std::chrono::month{month}}}.day()));}
bool overlaps(const GuiCalendar& c,const GuiSystemTime& first,const GuiSystemTime& last){return gui_date_ordinal(last)>=gui_date_ordinal(date(1601,1,1)) && gui_date_ordinal(first)<=gui_date_ordinal(date(9999,12,31)) && (!(c.range_flags&1U) || gui_date_ordinal(last)>=gui_date_ordinal(c.minimum)) && (!(c.range_flags&2U) || gui_date_ordinal(first)<=gui_date_ordinal(c.maximum));}
bool selected(const GuiCalendar& c,const GuiSystemTime& first,const GuiSystemTime& last){return gui_date_ordinal(last)>=gui_date_ordinal(c.value) && gui_date_ordinal(first)<=gui_date_ordinal(c.last);}
Rect unite(Rect a,const Rect& b){return {std::min(a[0],b[0]),std::min(a[1],b[1]),std::max(a[2],b[2]),std::max(a[3],b[3])};}
std::pair<unsigned,unsigned> count_for(const GuiCalendar& c,std::int64_t width,std::int64_t height){const auto cols=static_cast<unsigned>(std::clamp<std::int64_t>(width/c.minimum_width,1,12));return {cols,static_cast<unsigned>(std::clamp<std::int64_t>(height/c.minimum_height,1,12/cols))};}
}

void gui_calendar_set_view(GuiCalendar& c,std::uint32_t view){
    if(c.view==view || view>3)return;
    GuiDateNotice notice;notice.code=-750;notice.old_view=c.view;notice.new_view=view;c.view=view;c.pressed=-1;c.dragging=false;c.notices.push_back(notice);
}

void gui_calendar_grid(GuiWindow& w,const GuiFontMetrics& metrics,const std::function<std::int32_t(std::u16string_view)>& measure){
    if(!w.calendar || w.calendar->picker)return;auto& c=*w.calendar;c.cells.clear();
    const auto width=[&](std::u16string_view text){return measure?std::max(1,measure(text)):static_cast<int>(text.size())*metrics.average_width;};
    const int row=std::max(18,metrics.height+6),border=static_cast<int>(c.border),week=(w.style&4U)?std::max(20,width(u"52")+8):0;
    int weekday_width=0,month_width=0,short_month_width=0;for(unsigned n=0;n<7;++n)weekday_width=std::max(weekday_width,width(gui_calendar_weekday_name(c,n,(w.style&0x80U)?2U:1U)));for(unsigned n=1;n<=12;++n){month_width=std::max(month_width,width(gui_calendar_month_name(c,n)));short_month_width=std::max(short_month_width,width(gui_calendar_month_name(c,n,true)));}
    c.minimum_width=static_cast<std::uint32_t>(std::max({188,7*std::max(28,weekday_width+8)+week,month_width+width(u"9999")+56,4*(short_month_width+12),width(gui_calendar_today_text(c))})+2*border);
    c.minimum_height=static_cast<std::uint32_t>(row*8+((w.style&0x10U)?0:row)+2*border);
    const auto [cols,rows]=count_for(c,w.width,w.height);c.calendar_count=cols*rows;c.grid_columns=cols;
    const int inner=int(c.minimum_width)-2*border,body_width=inner-week;
    const auto add=[&](GuiSystemTime first,GuiSystemTime last,Rect rect,std::u16string text,int action,int calendar,int grid_row=-1,int column=-1,bool trailing=false){
        GuiDateCell cell;cell.date=first;cell.end=last;cell.rect=rect;cell.text=std::move(text);cell.action=action;cell.calendar=calendar;cell.row=grid_row;cell.column=column;cell.trailing=trailing;
        if(action==1 || action==2){cell.selected=selected(c,first,last);cell.disabled=!overlaps(c,first,last);cell.today=gui_date_ordinal(c.today)>=gui_date_ordinal(first) && gui_date_ordinal(c.today)<=gui_date_ordinal(last);}
        if(action==1 && trailing && (w.style&0x40U)){cell.text.clear();cell.disabled=true;}c.cells.push_back(std::move(cell));
    };
    for(unsigned calendar=0;calendar<c.calendar_count;++calendar){
        const int ox=int(calendar%cols*c.minimum_width),oy=int(calendar/cols*c.minimum_height);GuiSystemTime first{},last{};std::u16string title;
        int year=c.display_year;unsigned month=static_cast<unsigned>(c.display_month);
        if(!c.view){const auto total=std::clamp(year*12+int(month)-1+int(calendar),1601*12,9999*12+11);year=total/12;month=static_cast<unsigned>(total%12+1);first=date(year,month,1);last=month_end(year,month);title=std::u16string(gui_calendar_month_name(c,month))+u" "+number(year);}
        else if(c.view==1){year=std::clamp(year+int(calendar),1601,9999);first=date(year,1,1);last=date(year,12,31);title=number(year);}
        else {const int span=c.view==2?10:100;year=std::clamp((year/span)*span+int(calendar)*span,1600,c.view==2?9990:9900);first=date(year,1,1);last=date(year+span-1,12,31);title=number(year)+u" – "+number(year+span-1);}
        if(!c.view){const auto month_text=std::u16string(gui_calendar_month_name(c,month)),year_text=number(year);const int split=ox+int(c.minimum_width)/2+(width(month_text)-width(year_text))/2;
            add(first,last,{ox+border+24,oy+2,split,oy+row},month_text,-6,int(calendar));add(first,last,{split,oy+2,ox+int(c.minimum_width)-border-24,oy+row},year_text,-7,int(calendar));}
        else add(first,last,{ox+border+24,oy+2,ox+int(c.minimum_width)-border-24,oy+row},std::move(title),-6,int(calendar));
        if(!calendar)add(first,last,{ox+2,oy+2,ox+26,oy+row},u"‹",-1,int(calendar));
        if(calendar+1==c.calendar_count)add(first,last,{ox+int(c.minimum_width)-26,oy+2,ox+int(c.minimum_width)-2,oy+row},u"›",-2,int(calendar));
        if(!c.view){
            const auto start=gui_date_ordinal(first)-static_cast<std::int64_t>((first[2]+7-(c.first_day+1)%7)%7);
            for(int column=0;column<7;++column){const auto wd=(c.first_day+static_cast<unsigned>(column)+1)%7;add(gui_date_from_ordinal(start+column),gui_date_from_ordinal(start+column),{ox+border+week+column*body_width/7,oy+row,ox+border+week+(column+1)*body_width/7,oy+2*row},std::u16string(gui_calendar_weekday_name(c,wd,(w.style&0x80U)?2U:1U)),0,int(calendar),-1,column);}
            for(int index=0;index<42;++index){const auto d=gui_date_from_ordinal(start+index);const int r=index/7,col=index%7;add(d,d,{ox+border+week+col*body_width/7,oy+row*(2+r),ox+border+week+(col+1)*body_width/7,oy+row*(3+r)},number(d[3]),1,int(calendar),r,col,d[1]!=month);}
            if(week)for(int r=0;r<6;++r){const auto begin=start+r*7,end=begin+6;const auto rule=c.locale?c.locale->first_week:2U;const auto week_start=[&](int y){const auto january=date(y,1,1);auto first_week=gui_date_ordinal(january)-static_cast<std::int64_t>((january[2]+7-(c.first_day+1)%7)%7);const auto present=7-(gui_date_ordinal(january)-first_week);const auto minimum=rule==0?1:rule==1?7:4;if(present<minimum)first_week+=7;return first_week;};int y=gui_date_from_ordinal(begin)[0];if(begin<week_start(y))--y;else if(begin>=week_start(y+1))++y;const auto week_number=(begin-week_start(y))/7+1;add(gui_date_from_ordinal(begin),gui_date_from_ordinal(end),{ox+border,oy+row*(2+r),ox+border+week,oy+row*(3+r)},number(int(week_number)),0,int(calendar),r,-1);}
        }else for(int index=0;index<12;++index){
            GuiSystemTime a{},b{};std::u16string text;bool trailing=false;
            if(c.view==1){a=date(year,static_cast<unsigned>(index+1),1);b=month_end(year,static_cast<unsigned>(index+1));text=gui_calendar_month_name(c,static_cast<unsigned>(index+1),true);}
            else {const int span=c.view==2?1:10,begin=year+(index-1)*span;a=date(begin,1,1);b=date(begin+span-1,12,31);text=number(begin);if(span>1)text+=u"\n"+number(begin+span-1);trailing=index==0 || index==11;}
            const int r=index/4,col=index%4;add(a,b,{ox+border+col*inner/4,oy+row+1+r*(row*7-1)/3,ox+border+(col+1)*inner/4,oy+row+1+(r+1)*(row*7-1)/3},std::move(text),2,int(calendar),r,col,trailing);
        }
    }
    if(!(w.style&0x10U)){const int y=int(c.minimum_height*rows)-row-border;add(c.today,c.today,{border,y,int(c.minimum_width*cols)-border,y+row},gui_calendar_today_text(c),-3,0);c.cells.back().disabled=!overlaps(c,c.today,c.today);}
    if((w.style&1U) && !c.view){
        int first_month=INT32_MAX,last_month=0;for(const auto& cell:c.cells)if(cell.action==1){const auto key=int(cell.date[0])*12+cell.date[1]-1;first_month=std::min(first_month,key);last_month=std::max(last_month,key);}
        const auto count=static_cast<std::size_t>(last_month-first_month+1);
        if(c.day_state_month!=first_month || c.day_states.size()!=count){
            c.day_state_month=first_month;c.day_states.assign(count,0);++c.day_state_revision;
            std::erase_if(c.notices,[](const auto& n){return n.code==-743;});GuiDateNotice request;request.code=-743;request.first=date(first_month/12,static_cast<unsigned>(first_month%12+1),1);request.flags=static_cast<std::uint32_t>(count);request.revision=c.day_state_revision;c.notices.push_back(request);
        }
        for(auto& cell:c.cells)if(cell.action==1){const auto index=int(cell.date[0])*12+cell.date[1]-1-c.day_state_month;if(index>=0 && static_cast<std::size_t>(index)<c.day_states.size())cell.bold=(c.day_states[static_cast<std::size_t>(index)]&(1U<<(cell.date[3]-1)))!=0;}
    }else if(c.day_state_month!=-1){c.day_state_month=-1;c.day_states.clear();++c.day_state_revision;std::erase_if(c.notices,[](const auto& n){return n.code==-743;});}

}

std::optional<std::uint64_t> gui_calendar_grid_message(GuestMemory& memory,GuiWindow& w,const GuiMessage& msg,bool x86){
    if(!w.calendar || w.calendar->picker)return std::nullopt;auto& c=*w.calendar;const auto m=msg.message;const auto lp=msg.lparam;
    if(m==0x1016)return c.view;if(m==0x1017)return c.calendar_count;
    if(m==0x1020){if(msg.wparam || lp>3)return 0;gui_calendar_set_view(c,static_cast<std::uint32_t>(lp));return 1;}
    if(m==0x101f)return c.border;
    if(m==0x101e){const auto border=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(lp));c.border_explicit=msg.wparam && border>=0 && border<=(INT32_MAX-100000)/2;c.border=c.border_explicit?static_cast<std::uint32_t>(border):4U;return 0;}
    if(m==0x101d){if(msg.wparam || !lp)return 0;memory.check(GuestAddress{lp},16,Access::Read);memory.check(GuestAddress{lp},16,Access::Write);Rect rect{};for(unsigned n=0;n<4;++n)rect[n]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+n*4},32)));const auto [cols,rows]=count_for(c,std::int64_t(rect[2])-rect[0],std::int64_t(rect[3])-rect[1]);const auto right=std::int64_t(rect[0])+cols*c.minimum_width,bottom=std::int64_t(rect[1])+rows*c.minimum_height;if(right>INT32_MAX || bottom>INT32_MAX)return 0;memory.store(GuestAddress{lp+8},32,static_cast<std::uint32_t>(right));memory.store(GuestAddress{lp+12},32,static_cast<std::uint32_t>(bottom));return 0;}
    if(m==0x1007){if(msg.wparam>1)return 0;GuiSystemTime first{},last{};bool found=false;for(const auto& cell:c.cells){if(!cell.date[0] || (msg.wparam==0?(cell.action!=-6 && cell.action!=-7):(cell.action!=(c.view?2:1))))continue;if(!found){first=cell.date;last=cell.end;found=true;}else {if(gui_date_ordinal(cell.date)<gui_date_ordinal(first))first=cell.date;if(gui_date_ordinal(cell.end)>gui_date_ordinal(last))last=cell.end;}}if(!found)return 0;if(lp){memory.check(GuestAddress{lp},32,Access::Write);gui_date_write(memory,lp,first);gui_date_write(memory,lp+16,last);}return static_cast<std::uint64_t>((int(last[0])-first[0])*12+last[1]-first[1]+1);}
    if(m==0x1008){if(!(w.style&1U) || c.view || !lp || msg.wparam!=c.day_states.size() || c.day_state_month<0)return 0;memory.check(GuestAddress{lp},c.day_states.size()*4,Access::Read);std::vector<std::uint32_t> states;states.reserve(c.day_states.size());for(std::size_t n=0;n<c.day_states.size();++n)states.push_back(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+n*4},32)));c.day_states=std::move(states);++c.day_state_revision;std::erase_if(c.notices,[](const auto& n){return n.code==-743;});return 1;}
    if(m!=0x1018)return std::nullopt;if(msg.wparam || !lp)return 0;
    const unsigned size=x86?84:96,pointer=x86?76:80;const std::uint8_t bits=x86?32:64;
    if(memory.load(GuestAddress{lp},32)!=size)return 0;memory.check(GuestAddress{lp},size,Access::Read);
    const auto part=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+4},32)),flags=static_cast<std::uint32_t>(memory.load(GuestAddress{lp+8},32));
    const auto signed_at=[&](unsigned offset){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+offset},32)));};const int calendar=signed_at(12),row=signed_at(16),col=signed_at(20);
    if(part>8 || flags&~7U || (part>=4 && (calendar<0 || static_cast<unsigned>(calendar)>=c.calendar_count)))return 0;
    const bool name_requested=(flags&4U) && (part==4 || part==5 || part==8);std::uint64_t name=0,capacity=0;
    if(name_requested){name=memory.load(GuestAddress{lp+pointer},bits);capacity=memory.load(GuestAddress{lp+pointer+bits/8},bits);if(!name || !capacity)return 0;}
    GuiSystemTime first{},last{};Rect rect{};std::u16string text;bool found=false,is_selected=false;
    for(const auto& cell:c.cells){bool match=false;
        if(part==0)match=true;else if(part==1)match=cell.action==-2;else if(part==2)match=cell.action==-1;else if(part==3)match=cell.action==-3;
        else if(cell.calendar==calendar){if(part==4)match=cell.action!=-3;else if(part==5)match=cell.action==-6 || cell.action==-7;else if(part==6)match=cell.row>=0;else if(part==7)match=cell.row==row && cell.column>=0;else if(part==8)match=cell.row==row && cell.column==col && cell.action>=0;}
        if(!match)continue;rect=found?unite(rect,cell.rect):cell.rect;found=true;is_selected|=cell.selected;
        if(cell.date[0]){if(!first[0] || gui_date_ordinal(cell.date)<gui_date_ordinal(first))first=cell.date;if(!last[0] || gui_date_ordinal(cell.end)>gui_date_ordinal(last))last=cell.end;}
        if(part==5){if(!text.empty())text+=u" ";text+=cell.text;}else if(part==8)text=cell.text;
    }
    if(!found)return 0;
    if(part==0)rect={0,0,static_cast<int>(w.width),static_cast<int>(w.height)};
    if(part==4)text=gui_date_format_text(c,c.value,c.long_format.empty()?u"dddd, MMMM d, yyyy":std::u16string_view(c.long_format));
    // 先檢查所有輸出區域，避免後面的無效名稱緩衝區造成部分寫入。
    if(flags&1U)memory.check(GuestAddress{lp+28},32,Access::Write);if(flags&2U)memory.check(GuestAddress{lp+60},16,Access::Write);if(part==8)memory.check(GuestAddress{lp+24},4,Access::Write);
    if(name_requested){if(text.size()>=capacity)text.resize(static_cast<std::size_t>(capacity-1));if(!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();memory.check(GuestAddress{name},(text.size()+1)*2,Access::Write);}
    if(flags&1U){gui_date_write(memory,lp+28,first);gui_date_write(memory,lp+44,last);}if(flags&2U)for(unsigned n=0;n<4;++n)memory.store(GuestAddress{lp+60+n*4},32,static_cast<std::uint32_t>(rect[n]));if(part==8)memory.store(GuestAddress{lp+24},32,is_selected?1:0);
    if(name_requested)memory.write(GuestAddress{name},utf16_bytes(text));return 1;
}
}
