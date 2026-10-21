#include "x64arm/windows_calendar.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <ctime>

namespace x64arm {
namespace {
using namespace std::chrono;
constexpr std::u16string_view months[]{u"January",u"February",u"March",u"April",u"May",u"June",u"July",u"August",u"September",u"October",u"November",u"December"};
constexpr std::u16string_view weekdays[]{u"Sunday",u"Monday",u"Tuesday",u"Wednesday",u"Thursday",u"Friday",u"Saturday"};
sys_days day(const GuiSystemTime& d){return sys_days{year{d[0]}/month{d[1]}/std::chrono::day{d[3]}};}
std::int64_t stamp(const GuiSystemTime& d,bool time){return static_cast<std::int64_t>(day(d).time_since_epoch().count())*86400000LL+(time?(d[4]*3600LL+d[5]*60LL+d[6])*1000+d[7]:0);}
GuiSystemTime from_day(sys_days date){const year_month_day d{date};GuiSystemTime result{static_cast<std::uint16_t>(int(d.year())),static_cast<std::uint16_t>(unsigned(d.month())),static_cast<std::uint16_t>(weekday{date}.c_encoding()),static_cast<std::uint16_t>(unsigned(d.day())),0,0,0,0};return result;}
GuiSystemTime read_date(GuestMemory& memory,std::uint64_t at){memory.check(GuestAddress{at},16,Access::Read);GuiSystemTime d{};for(unsigned i=0;i<8;++i)d[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{at+i*2},16));return d;}
bool in_range(const GuiCalendar& c,const GuiSystemTime& d){return (!(c.range_flags&1U) || stamp(d,c.picker)>=stamp(c.minimum,c.picker)) && (!(c.range_flags&2U) || stamp(d,c.picker)<=stamp(c.maximum,c.picker));}
std::u16string number(unsigned value,unsigned count=1){auto text=utf8_to_utf16_lossy(std::to_string(value));if(text.size()<count)text.insert(0,count-text.size(),u'0');return text;}
void notice(GuiCalendar& c,std::int32_t code){c.notices.push_back({code,c.none?1U:0U,c.value,c.last});}
void select(GuiCalendar& c,GuiSystemTime value,bool user){const auto old=c.value;const auto none=c.none;if(user && !c.picker)std::copy(old.begin()+4,old.end(),value.begin()+4);c.value=gui_date_normalize(value,true);c.last=c.value;c.none=false;c.display_year=c.value[0];c.display_month=c.value[1];if(c.value!=old || none){if(c.picker){++c.picker_revision;notice(c,-759);}else if(user)notice(c,-749);}}
GuiSystemTime add_month(GuiSystemTime value,int delta){const auto total=std::clamp<int>(int(value[0])*12+value[1]-1+delta,1601*12,9999*12+11);value[0]=static_cast<std::uint16_t>(total/12);value[1]=static_cast<std::uint16_t>(total%12+1);const auto end=year_month_day_last{year{value[0]},month_day_last{month{value[1]}}};value[3]=std::min(value[3],static_cast<std::uint16_t>(unsigned(end.day())));return gui_date_normalize(value);}
void clear_input(GuiCalendar& c){c.input_field=-1;c.input_value=c.input_digits=0;c.name_input.clear();}
bool numeric_day(std::u16string_view format){bool quoted=false;for(std::size_t at=0;at<format.size();){const auto ch=format[at++];if(ch==u'\''){quoted=!quoted;continue;}if(!quoted && ch==u'd'){unsigned n=1;while(at<format.size() && format[at]==ch){++at;++n;}if(n<=2)return true;}}return false;}
std::u16string designator(const GuiCalendar& c,unsigned pm,unsigned count){auto value=c.locale?c.locale->designators[pm]:std::u16string(pm?u"PM":u"AM");if(count==1 && !value.empty()){const auto n=value[0]>=0xd800 && value[0]<=0xdbff && value.size()>1 && value[1]>=0xdc00 && value[1]<=0xdfff?2U:1U;value.resize(n);}return value;}
std::u16string field_text(const GuiCalendar& c,const GuiSystemTime& d,char16_t ch,unsigned n,bool genitive){
    if(ch==u'y')return number(n<=2?d[0]%100:d[0],n<=2?n:4);
    if(ch==u'M')return n>=3?std::u16string(gui_calendar_month_name(c,d[1],n==3,genitive)):number(d[1],n);
    if(ch==u'd')return n>=3?std::u16string(gui_calendar_weekday_name(c,d[2],n==3?1U:0U)):number(d[3],n);
    if(ch==u'h' || ch==u'H')return number(ch==u'h'?(d[4]%12?d[4]%12:12):d[4],n);
    if(ch==u'm' || ch==u's')return number(d[ch==u'm'?5U:6U],n);
    if(ch==u't')return designator(c,d[4]>=12,n);return std::u16string(n,ch);
}
std::u16string picker_format(const GuiWindow& w){
    const auto& c=*w.calendar;if(!c.format.empty())return c.format;
    if((w.style&12U)==8U)return c.time_format.empty()?u"h:mm:ss tt":c.time_format;
    if((w.style&12U)==4U)return c.long_format.empty()?u"dddd, MMMM d, yyyy":c.long_format;
    auto format=c.short_format.empty()?std::u16string(u"M/d/yy"):c.short_format;
    if((w.style&12U)==12U){bool quoted=false;for(std::size_t at=0;at<format.size();){
        if(format[at]==u'\''){quoted=!quoted;++at;continue;}
        if(!quoted && format[at]==u'y'){std::size_t end=at+1;while(end<format.size() && format[end]==u'y')++end;format.replace(at,end-at,u"yyyy");at+=4;}else ++at;
    }}return format;
}
const GuiDateCell* picker_cell(const GuiCalendar& c){const auto at=std::find_if(c.cells.begin(),c.cells.end(),[&](const auto& cell){return cell.action==c.field+1 && cell.action>0;});return at==c.cells.end()?nullptr:&*at;}
std::pair<unsigned,unsigned> picker_limits(const GuiCalendar& c){
    switch(c.field){case 0:return {1601,9999};case 1:return {1,12};case 3:return {1,static_cast<unsigned>(unsigned(year_month_day_last{year{c.value[0]},month_day_last{month{c.value[1]}}}.day()))};
        case 4:{const auto* cell=picker_cell(c);return cell && cell->format_symbol==u'h'?std::pair<unsigned,unsigned>{1,12}:std::pair<unsigned,unsigned>{0,23};}
        case 5:case 6:return {0,59};case 8:return {0,1};default:return {0,0};}
}
bool set_picker_field(GuiCalendar& c,unsigned number){
    const auto [minimum,maximum]=picker_limits(c);if(c.field<0 || number<minimum || number>maximum)return false;
    auto value=c.value;if(c.field==8)value[4]=static_cast<std::uint16_t>(value[4]%12+number*12);
    else if(c.field==4){const auto* cell=picker_cell(c);value[4]=static_cast<std::uint16_t>(cell && cell->format_symbol==u'h'?number%12+(value[4]>=12?12:0):number);}
    else if(c.field==0 || c.field==1 || c.field==3 || c.field==5 || c.field==6)value[static_cast<std::size_t>(c.field)]=static_cast<std::uint16_t>(number);else return false;
    if(c.field==0 || c.field==1)value[3]=std::min(value[3],static_cast<std::uint16_t>(unsigned(year_month_day_last{year{value[0]},month_day_last{month{value[1]}}}.day())));
    if(!gui_date_valid(value) || !in_range(c,value))return false;select(c,value,true);return true;
}
void picker_character(GuiCalendar& c,std::uint32_t character){
    if(c.field>=9)return;
    const auto* current=picker_cell(c);
    if((c.field==8 || (c.field==1 && current && current->format_length>=3)) && (character<u'0' || character>u'9')){
        if(character<32 || character>65535)return;if(c.input_field!=c.field){clear_input(c);c.input_field=c.field;}
        const auto prefix=[](std::u16string_view text,std::u16string_view input){if(input.size()>text.size())return false;for(std::size_t n=0;n<input.size();++n)if(unicode_case_unit(text[n],true)!=unicode_case_unit(input[n],true))return false;return true;};
        auto input=c.name_input;input.push_back(static_cast<char16_t>(character));
        const auto match=[&](std::u16string_view key){std::vector<unsigned> values;for(unsigned n=0;n<(c.field==8?2U:12U);++n){bool accepted=false;if(c.field==8)accepted=prefix(designator(c,n,2),key);else for(auto abbreviated:{false,true})for(auto genitive:{false,true})accepted|=prefix(gui_calendar_month_name(c,n+1,abbreviated,genitive),key);if(accepted)values.push_back(c.field==8?n:n+1);}return values;};
        auto values=match(input);if(values.empty()){input.assign(1,static_cast<char16_t>(character));values=match(input);}
        if(values.size()==1){c.input_value=c.input_digits=0;c.name_input=std::move(input);(void)set_picker_field(c,values.front());}else if(!values.empty())c.name_input=std::move(input);else {clear_input(c);if(c.field==8 && (character==u'a' || character==u'A' || character==u'p' || character==u'P'))(void)set_picker_field(c,character==u'p' || character==u'P');}return;
    }
    if(character<u'0' || character>u'9' || !picker_cell(c))return;
    c.name_input.clear();
    const auto* cell=picker_cell(c);const auto digits=c.field==0 && cell->format_length>2?4U:2U;
    if(c.input_field!=c.field || !c.input_digits || c.input_digits>=digits){c.input_field=c.field;c.input_value=c.input_digits=0;c.input_previous=c.value;}
    c.input_value=c.input_value*10+character-u'0';++c.input_digits;
    auto value=c.input_value;if(c.field==0 && digits==2)value=(c.input_previous[0]/100)*100+value;
    const bool pending=c.field==0 && digits==4 && c.input_digits<4;
    if(!pending && !set_picker_field(c,value) && c.input_digits==digits){const auto previous=c.input_previous;if(in_range(c,previous))select(c,previous,true);}
    if(c.input_digits==digits)clear_input(c);
}
int hit(const GuiCalendar& c,int x,int y){for(std::size_t i=0;i<c.cells.size();++i){const auto& r=c.cells[i].rect;if(x>=r[0] && x<r[2] && y>=r[1] && y<r[3])return static_cast<int>(i);}return -1;}
std::uint64_t signed_value(std::int32_t n){return static_cast<std::uint64_t>(static_cast<std::int64_t>(n));}
}
bool gui_calendar_class(std::u16string_view c){return c==u"sysdatetimepick32" || c==u"sysmonthcal32";}
bool gui_calendar_dispatch(std::uint32_t m){return m==1 || m==5 || m==0x1a || m==0x30 || m==7 || m==8 || m==15 || m==0x87 || m==0x100 || m==0x102 || m==0x200 || m==0x201 || m==0x202 || m==0x2005 || m==0x2006 || (m>=0x1000 && m<=0x1032);}
bool gui_date_valid(const GuiSystemTime& d,bool time){return d[0]>=1601 && d[0]<=9999 && year_month_day{year{d[0]},month{d[1]},std::chrono::day{d[3]}}.ok() && (!time || (d[4]<24 && d[5]<60 && d[6]<60 && d[7]<1000));}
std::int64_t gui_date_ordinal(const GuiSystemTime& value){return day(value).time_since_epoch().count();}
GuiSystemTime gui_date_from_ordinal(std::int64_t value){return from_day(sys_days{days{value}});}
GuiSystemTime gui_date_normalize(GuiSystemTime d,bool time){d[2]=static_cast<std::uint16_t>(weekday{day(d)}.c_encoding());if(!time)std::fill(d.begin()+4,d.end(),0);return d;}
void gui_date_write(GuestMemory& memory,std::uint64_t at,const GuiSystemTime& d){memory.check(GuestAddress{at},16,Access::Write);for(unsigned i=0;i<8;++i)memory.store(GuestAddress{at+i*2},16,d[i]);}
std::array<std::int32_t,3> gui_date_field_spin(const GuiCalendar& c){const auto limits=picker_limits(c);unsigned value=0;if(c.field==8)value=c.value[4]>=12;else if(c.field>=0 && c.field<8)value=c.value[static_cast<std::size_t>(c.field)];if(c.field==4){const auto* cell=picker_cell(c);if(cell && cell->format_symbol==u'h')value=value%12?value%12:12;}return {static_cast<std::int32_t>(limits.first),static_cast<std::int32_t>(limits.second),static_cast<std::int32_t>(value)};}
bool gui_date_field_step(GuiCalendar& c,std::int64_t delta){
    if(!c.picker || !picker_cell(c) || !delta)return false;clear_input(c);const auto old=c.value;const auto none=c.none;auto value=old;
    if(c.field==0 || c.field==1){const auto months_delta=c.field==0?std::clamp<std::int64_t>(delta,-10000,10000)*12:std::clamp<std::int64_t>(delta,-120000,120000);value=add_month(value,static_cast<int>(months_delta));}
    else if(c.field==8){if(delta%2)value[4]=static_cast<std::uint16_t>(value[4]<12?value[4]+12:value[4]-12);}
    else if(c.field>=4 && c.field<=6){const auto unit=c.field==4?3600000LL:c.field==5?60000LL:1000LL;if(delta>INT64_MAX/unit || delta<INT64_MIN/unit)return false;const auto amount=delta*unit;const auto current=stamp(value,true);if((amount>0 && current>INT64_MAX-amount) || (amount<0 && current<INT64_MIN-amount))return false;const auto next=current+amount;
        const auto ordinal=floor<days>(sys_time<milliseconds>{milliseconds{next}});if(ordinal<sys_days{year{1601}/January/1} || ordinal>sys_days{year{9999}/December/31})return false;value=from_day(ordinal);const auto remainder=(next%86400000+86400000)%86400000;value[4]=static_cast<std::uint16_t>(remainder/3600000);value[5]=static_cast<std::uint16_t>(remainder/60000%60);value[6]=static_cast<std::uint16_t>(remainder/1000%60);value[7]=static_cast<std::uint16_t>(remainder%1000);
    }else if(c.field==3){const auto ordinal=gui_date_ordinal(value);if(delta>3660000 || delta<-3660000)return false;const auto next=sys_days{days{ordinal+delta}};if(next<sys_days{year{1601}/January/1} || next>sys_days{year{9999}/December/31})return false;value=from_day(next);std::copy(old.begin()+4,old.end(),value.begin()+4);}else return false;
    if(!gui_date_valid(value) || !in_range(c,value))return false;select(c,value,true);return c.value!=old || none;
}
void gui_calendar_initialize(GuiWindow& w,const GuiSystemTime& value){if(w.calendar)return;if(!gui_date_valid(value))throw HostCallFault("月曆初始時間無效");w.calendar.emplace();auto& c=*w.calendar;c.picker=gui_control_class(w)==u"sysdatetimepick32";c.month_delta=0;c.value=gui_date_normalize(value,true);c.last=c.value;c.today=gui_date_normalize(c.value,true);c.display_year=c.value[0];c.display_month=c.value[1];}

std::shared_ptr<const GuiCalendarLocale> gui_calendar_read_locale(const GuestMemory& memory,std::uint64_t buffer){
    auto locale=std::make_shared<GuiCalendarLocale>();const auto text=[&](unsigned index){return read_utf16(memory,GuestAddress{buffer+index*gui_calendar_locale_chars*2},gui_calendar_locale_chars);};
    locale->short_date=text(0);locale->long_date=text(1);locale->time=text(2);const auto day=text(3),week=text(4);if(day.size()==1 && day[0]>=u'0' && day[0]<=u'6')locale->first_day=day[0]-u'0';if(week.size()==1 && week[0]>=u'0' && week[0]<=u'2')locale->first_week=week[0]-u'0';
    for(unsigned n=0;n<12;++n){locale->months[n]=text(5+n);locale->short_months[n]=text(17+n);locale->genitive_months[n]=text(29+n);locale->short_genitive_months[n]=text(41+n);}
    for(unsigned n=0;n<7;++n){const auto slot=(n+1)%7;locale->weekdays[slot]=text(53+n);locale->short_weekdays[slot]=text(60+n);locale->narrow_weekdays[slot]=text(67+n);}locale->designators[0]=text(74);locale->designators[1]=text(75);return locale;
}
void gui_calendar_apply_locale(GuiCalendar& c,std::shared_ptr<const GuiCalendarLocale> locale){c.locale=std::move(locale);if(!c.locale)return;c.short_format=c.locale->short_date;c.long_format=c.locale->long_date;c.time_format=c.locale->time;if(!c.first_day_explicit)c.first_day=c.locale->first_day;clear_input(c);c.callback_schema.clear();c.callback_query_dirty=c.callback_text_dirty=true;++c.callback_revision;}
std::u16string_view gui_calendar_month_name(const GuiCalendar& c,unsigned month,bool abbreviated,bool genitive){if(!month || month>12)return {};if(c.locale){const auto& values=genitive?(abbreviated?c.locale->short_genitive_months:c.locale->genitive_months):(abbreviated?c.locale->short_months:c.locale->months);return values[month-1];}return months[month-1].substr(0,abbreviated?3:months[month-1].size());}
std::u16string_view gui_calendar_weekday_name(const GuiCalendar& c,unsigned weekday,unsigned style){if(weekday>6)return {};if(c.locale)return (style==2?c.locale->narrow_weekdays:style==1?c.locale->short_weekdays:c.locale->weekdays)[weekday];return weekdays[weekday].substr(0,style==2?1:style==1?3:weekdays[weekday].size());}
std::u16string gui_date_format_text(const GuiCalendar& c,const GuiSystemTime& date,std::u16string_view format){std::u16string text;const bool genitive=numeric_day(format);for(std::size_t at=0;at<format.size();){const auto ch=format[at++];if(ch==u'\''){while(at<format.size()){if(format[at]==u'\''){++at;if(at<format.size() && format[at]==u'\''){text.push_back(u'\'');++at;continue;}break;}text.push_back(format[at++]);}}else {unsigned n=1;while(at<format.size() && format[at]==ch){++at;++n;}text+=field_text(c,date,ch,n,genitive);}}return text;}

void gui_date_capture_callbacks(GuiWindow& w,GuiControlOperation::Calendar& request,std::uint32_t message,std::uint64_t previous_value_revision){
    auto& c=*w.calendar;if(!c.picker)return;
    const bool query=message==0x1005 || message==0x1032 || message==0x30 || message==0x2005;
    if(query)c.callback_query_dirty=c.callback_text_dirty=true;
    const bool mutation=query || c.picker_revision!=previous_value_revision || c.callback_key;
    const bool idle=!c.callback_depth && (message==1 || message==5 || message==15 || message==0x100f);
    if(c.callback_fields.empty() || (!mutation && !idle))return;
    if(!mutation && message!=15 && !c.callback_query_dirty && !c.callback_text_dirty)return;
    request.callbacks=request.callback_active=true;request.callback_wide=w.list_unicode;request.callback_query=c.callback_query_dirty;
    request.callback_key=c.callback_key;c.callback_key=0;request.callback_field=c.field>=9?static_cast<std::uint32_t>(c.field-9):0;
    request.callback_revision=++c.callback_revision;request.callback_value_revision=c.picker_revision;++c.callback_depth;
}

void gui_calendar_layout(GuiWindow& w,const GuiFontMetrics& metrics,const std::function<std::int32_t(std::u16string_view)>& measured){
    if(!w.calendar)return;auto& c=*w.calendar;c.cells.clear();const auto width=[&](std::u16string_view s){if(measured)return std::max(1,measured(s));int n=0;for(auto ch:s)n+=ch>=0x2e80?2:1;return std::max(1,n*metrics.average_width);};const int row=std::max(18,metrics.height+6);
    c.minimum_width=static_cast<std::uint32_t>(std::max(196,7*std::max(28,width(u"Wed")+8)+8));c.minimum_height=static_cast<std::uint32_t>(row*8+((w.style&0x10U)?0:row)+8);
    if(c.picker){
        const auto format=picker_format(w);const bool genitive=numeric_day(format);std::int64_t x=(w.style&2U)?24:5;std::u16string title;std::size_t callback=0;
        if(c.callback_schema!=format){c.callback_schema=format;c.callback_fields.clear();bool quoted=false;for(std::size_t at=0;at<format.size();){if(format[at]==u'\''){quoted=!quoted;++at;continue;}if(!quoted && format[at]==u'X'){const auto start=at;while(at<format.size() && format[at]==u'X')++at;c.callback_fields.push_back({format.substr(start,at-start),{}, {}});}else ++at;}++c.callback_revision;c.callback_query_dirty=c.callback_text_dirty=true;}
        for(std::size_t i=0;i<format.size();){std::u16string text;int field=-1;const auto ch=format[i];std::size_t n=1;if(ch==u'\''){++i;while(i<format.size()){if(format[i]==u'\''){++i;if(i<format.size() && format[i]==u'\''){text.push_back(u'\'');++i;continue;}break;}text.push_back(format[i++]);}}
            else {while(i+n<format.size() && format[i+n]==ch)++n;i+=n;const auto& d=c.value;
                if(ch==u'y'){text=number(n<=2?d[0]%100:d[0],n<=2?static_cast<unsigned>(n):4);field=0;}
                else if(ch==u'M'){text=field_text(c,d,ch,static_cast<unsigned>(n),genitive);field=1;}
                else if(ch==u'd'){text=field_text(c,d,ch,static_cast<unsigned>(n),genitive);field=n<3?3:-1;}
                else if(ch==u'h' || ch==u'H'){text=number(ch==u'h'?(d[4]%12?d[4]%12:12):d[4],static_cast<unsigned>(n));field=4;}
                else if(ch==u'm' || ch==u's'){field=ch==u'm'?5:6;text=number(d[static_cast<std::size_t>(field)],static_cast<unsigned>(n));}
                else if(ch==u't'){text=designator(c,d[4]>=12,static_cast<unsigned>(n));field=8;}
                else if(ch==u'X'){field=9+static_cast<int>(callback);if(callback<c.callback_fields.size())text=c.callback_fields[callback].text;++callback;}
                else text.assign(n,ch);
            }
            if(field==0 && n>2 && c.input_field==0 && c.input_digits)text=number(c.input_value,c.input_digits);
            const auto extent=field>=9 && static_cast<std::size_t>(field-9)<c.callback_fields.size() && c.callback_fields[static_cast<std::size_t>(field-9)].maximum[0]>0?c.callback_fields[static_cast<std::size_t>(field-9)].maximum[0]:width(text);const auto end=std::min<std::int64_t>(INT32_MAX,x+extent);c.cells.push_back({c.value,{static_cast<int>(x),2,static_cast<int>(end),static_cast<int>(w.height)-2},text,field+1,c.field==field,false,false,c.none});c.cells.back().format_symbol=ch;c.cells.back().format_length=static_cast<std::uint32_t>(n);x=end;title+=text;
        }w.title=std::move(title);if(!picker_cell(c)){const auto first=std::find_if(c.cells.begin(),c.cells.end(),[](const auto& cell){return cell.action>0;});c.field=first==c.cells.end()?-1:first->action-1;for(auto& cell:c.cells)cell.selected=cell.action>0 && cell.action==c.field+1;}
        if(w.style&2U)c.cells.push_back({c.value,{4,std::max(0,int(w.height)/2-7),18,int(w.height)/2+7},u"",-4,!c.none});
        c.cells.push_back({c.value,{std::max(0,int(w.width)-20),0,int(w.width),int(w.height)},u"",-5});return;
    }
    gui_calendar_grid(w,metrics,measured);
}

std::u16string gui_calendar_today_text(const GuiCalendar& c){return u"Today: "+gui_date_format_text(c,c.today,c.short_format.empty()?u"M/d/yy":std::u16string_view(c.short_format));}
std::uint32_t gui_calendar_today_width(const GuiWindow& w,NativeGui* native){if(!w.calendar)return 0;const auto text=gui_calendar_today_text(*w.calendar);if(native)if(const auto widths=native->text_advances(w.control_font,text);widths && !widths->empty())return static_cast<std::uint32_t>(std::max(0,widths->back()));const auto metrics=fallback_font_metrics(w.control_font);return static_cast<std::uint32_t>(text.size()*static_cast<std::size_t>(std::max(1,metrics.average_width)));}

void gui_calendar_project(GuiWindow& window,NativeGui* native){
    const auto metrics=native?native->font_metrics(window.control_font).value_or(fallback_font_metrics(window.control_font)):fallback_font_metrics(window.control_font);
    if(!native){gui_calendar_layout(window,metrics);return;}
    gui_calendar_layout(window,metrics,[&](std::u16string_view text){if(const auto advances=native->text_advances(window.control_font,std::u16string(text));advances && !advances->empty())return advances->back();int count=0;for(std::size_t i=0;i<text.size();++i){const auto ch=text[i];if(ch>=0xd800 && ch<=0xdbff && i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<=0xdfff){count+=2;++i;}else count+=ch>=0x2e80?2:1;}return count*metrics.average_width;});
}

std::optional<std::uint64_t> gui_calendar_message(GuestMemory& memory,GuiWindow& w,const GuiMessage& msg,bool wide,bool x86){
    if(!w.calendar)return std::nullopt;auto& c=*w.calendar;const auto m=msg.message,wp=static_cast<std::uint32_t>(msg.wparam);const auto lp=msg.lparam;const bool multi=!c.picker && (w.style&2U);
    if(m==1 || m==5)return 0;
    if(m==0x87){const bool consume=(c.picker && wp==113 && (w.style&0x10U)) || (c.editing && (wp==13 || wp==27)) || (c.picker && wp==115 && !(w.style&1U)) || (c.picker && wp==32 && (w.style&2U)) || (!c.picker && wp==13) || (c.picker_owner && wp==27);return 1U|128U|(consume?4U:0U);}
    if(m==7 || m==8){clear_input(c);notice(c,m==7?-7:-8);return 0;}if(m==15)return 0;
    if(m==0x102 && c.picker){if(w.enabled)picker_character(c,wp);return 0;}
    if(m==0x2005){const auto old=w.list_unicode;w.list_unicode=wp!=0;return old?1:0;}if(m==0x2006)return w.list_unicode?1:0;
    if(c.picker && m==0x1001){if(c.none)return 1;if(!lp)return signed_value(-1);gui_date_write(memory,lp,c.value);return 0;}
    if(c.picker && m==0x1002){if(wp==1){if(!(w.style&2U))return 0;clear_input(c);++c.picker_revision;if(!c.none){c.none=true;notice(c,-759);}return 1;}if(wp || !lp)return 0;auto d=read_date(memory,lp);if(!gui_date_valid(d) || !in_range(c,d))return 0;clear_input(c);++c.picker_revision;select(c,d,false);return 1;}
    if((c.picker && (m==0x1003 || m==0x1004)) || (!c.picker && (m==0x1011 || m==0x1012))){const bool get=m==(c.picker?0x1003U:0x1011U);if(get){if(lp){if(c.range_flags&1U)gui_date_write(memory,lp,c.minimum);if(c.range_flags&2U)gui_date_write(memory,lp+16,c.maximum);}return c.range_flags;}
        if(wp&~3U || (wp && !lp))return 0;GuiSystemTime minimum{},maximum{};if(wp&1U){minimum=read_date(memory,lp);if(!gui_date_valid(minimum,c.picker))return 0;if(!c.picker && !gui_date_valid(minimum,true))std::copy(c.today.begin()+4,c.today.end(),minimum.begin()+4);minimum=gui_date_normalize(minimum,true);}if(wp&2U){maximum=read_date(memory,lp+16);if(!gui_date_valid(maximum,c.picker))return 0;if(!c.picker && !gui_date_valid(maximum,true))std::copy(c.today.begin()+4,c.today.end(),maximum.begin()+4);maximum=gui_date_normalize(maximum,true);}if(wp==3 && stamp(minimum,c.picker)>stamp(maximum,c.picker))return 0;clear_input(c);if(c.picker)++c.picker_revision;c.range_flags=wp;c.minimum=minimum;c.maximum=maximum;if(!in_range(c,c.value)){auto d=(wp&1U) && stamp(c.value,c.picker)<stamp(minimum,c.picker)?minimum:maximum;select(c,d,false);}if(!in_range(c,c.last))c.last=c.value;return 1;
    }
    if(c.picker && (m==0x1005 || m==0x1032)){auto format=lp?(m==0x1032?read_utf16(memory,GuestAddress{lp},65535):utf8_to_utf16_lossy(read_ansi(memory,lp,65535))):std::u16string{};clear_input(c);c.format=std::move(format);return 1;}
    if((c.picker && (m==0x1006 || m==0x1007)) || (!c.picker && (m==0x100a || m==0x100b))){if(wp>=c.colors.size())return UINT32_MAX;const auto old=c.colors[wp];if(m==0x1006 || m==0x100a)c.colors[wp]=static_cast<std::uint32_t>(lp)&0xffffffU;return old;}
    if(c.picker && m==0x1008)return c.open?c.month_window:0;
    if(c.picker && m==0x1009){c.month_font=msg.wparam;return 0;}if(c.picker && m==0x100a)return c.month_font?c.month_font:w.font;
    if(c.picker && m==0x100b){const auto old=c.month_style;c.month_style=static_cast<std::uint32_t>(lp);return old;}if(c.picker && m==0x100c)return c.month_style;
    if(c.picker && m==0x100d){c.close_request=c.open;return 0;}
    if(c.picker && m==0x100e){const auto bytes=x86?56U:72U,handles=x86?44U:48U,unit=x86?4U:8U;const auto bits=static_cast<std::uint8_t>(x86?32:64);if(!lp || memory.load(GuestAddress{lp},32)!=bytes)return 0;memory.check(GuestAddress{lp},bytes,Access::Write);memory.write(GuestAddress{lp+4},std::vector<std::uint8_t>(bytes-4));
        const auto rectangle=[&](std::uint32_t at,int action){for(const auto& cell:c.cells)if(cell.action==action){for(unsigned n=0;n<4;++n)memory.store(GuestAddress{lp+at+n*4},32,static_cast<std::uint32_t>(cell.rect[n]));break;}};
        rectangle(4,-4);rectangle(24,-5);memory.store(GuestAddress{lp+20},32,(w.style&2U)?(w.enabled?0U:1U)|(c.none?0U:0x10U):0x8000U);memory.store(GuestAddress{lp+40},32,(w.enabled?0U:1U)|((w.style&1U)?0x8000U:0U)|((c.pressed>=0 && static_cast<std::size_t>(c.pressed)<c.cells.size() && c.cells[static_cast<std::size_t>(c.pressed)].action==-5)?8U:0U));
        memory.store(GuestAddress{lp+handles},bits,c.edit_window);memory.store(GuestAddress{lp+handles+unit},bits,c.updown_window);memory.store(GuestAddress{lp+handles+unit*2},bits,c.open?c.month_window:0);return 0;}
    if(c.picker && m==0x100f){if(!lp)return 0;std::int64_t extent=30,height=std::max(22U,(c.minimum_height-8)/((w.style&0x10U)?8U:9U));for(const auto& cell:c.cells)if(cell.action>=0)extent=std::max(extent,std::int64_t{cell.rect[2]}+25);for(const auto& field:c.callback_fields)height=std::max(height,std::int64_t{field.maximum[1]}+4);memory.check(GuestAddress{lp},8,Access::Write);memory.store(GuestAddress{lp},32,static_cast<std::uint32_t>(std::min<std::int64_t>(INT32_MAX,extent)));memory.store(GuestAddress{lp+4},32,static_cast<std::uint32_t>(std::min<std::int64_t>(INT32_MAX,height)));return 1;}
    if(!c.picker && (m==0x1001 || m==0x1002)){if(multi || !lp)return 0;if(m==0x1001){gui_date_write(memory,lp,c.value);return 1;}const auto raw=read_date(memory,lp);if(!gui_date_valid(raw,false))return 0;const auto d=gui_date_normalize(raw,true);if(!in_range(c,d))return 0;select(c,d,false);return 1;}
    if(!c.picker && m==0x1003)return multi?c.maximum_selection:1;if(!c.picker && m==0x1004){if(!multi || !wp)return 0;c.maximum_selection=wp;if(day(c.last)-day(c.value)>=days{wp})c.last=from_day(day(c.value)+days{wp-1});return 1;}
    if(!c.picker && (m==0x1005 || m==0x1006)){if(!multi || !lp)return 0;if(m==0x1005){memory.check(GuestAddress{lp},32,Access::Write);gui_date_write(memory,lp,c.value);gui_date_write(memory,lp+16,c.last);return 1;}const auto raw=read_date(memory,lp),end=read_date(memory,lp+16);if(!gui_date_valid(raw,false) || !gui_date_valid(end,false))return 0;auto first=gui_date_normalize(raw,true),last=gui_date_normalize(end,true);if(!gui_date_valid(raw,true))std::copy(c.value.begin()+4,c.value.end(),first.begin()+4);if(!gui_date_valid(end,true))std::copy(c.last.begin()+4,c.last.end(),last.begin()+4);if(!in_range(c,first) || !in_range(c,last) || day(last)<day(first) || day(last)-day(first)>=days{c.maximum_selection})return 0;select(c,first,false);c.last=last;return 1;}
    if(!c.picker && m==0x1009){if(!lp)return 0;memory.check(GuestAddress{lp},16,Access::Write);memory.store(GuestAddress{lp},32,0);memory.store(GuestAddress{lp+4},32,0);memory.store(GuestAddress{lp+8},32,c.minimum_width);memory.store(GuestAddress{lp+12},32,c.minimum_height);return 1;}
    if(!c.picker && (m==0x100c || m==0x100d)){if(m==0x100d){if(!lp)return 0;gui_date_write(memory,lp,c.today);return 1;}if(lp){auto raw=read_date(memory,lp);if(gui_date_valid(raw,false))c.today=gui_date_normalize(raw,true);}return 0;}
    if(!c.picker && (m==0x100f || m==0x1010)){const auto old=c.first_day|(c.first_day_explicit?0x10000U:0U);if(m==0x100f){if(wp)return old;const auto default_day=c.locale?c.locale->first_day:6U;const auto d=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(lp));if(d>=0 && d<7){c.first_day=static_cast<std::uint32_t>(d);c.first_day_explicit=c.first_day!=default_day;}else if(d==-1){c.first_day=default_day;c.first_day_explicit=false;}}return old;}
    if(!c.picker && (m==0x1013 || m==0x1014)){const auto old=c.month_delta;if(m==0x1014){c.month_delta=wp;return old;}return old?old:c.calendar_count;}
    if(!c.picker)if(const auto result=gui_calendar_grid_message(memory,w,msg,x86))return result;
    if(!c.picker && m==0x100e){
        if(!lp)return 0;const auto size=memory.load(GuestAddress{lp},32);if(size<32)return 0;memory.check(GuestAddress{lp},32,Access::Read);
        const auto x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+4},32))),y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{lp+8},32)));const auto at=hit(c,x,y);std::uint32_t flags=0;
        if(at>=0){const auto& cell=c.cells[static_cast<std::size_t>(at)];flags=cell.action==1 || cell.action==2?0x20001U:cell.action==-1?0x2010003U:cell.action==-2?0x1010003U:cell.action==-3?0x30000U:cell.action==-7?0x10002U:cell.action==-6?(c.view?0x10002U:0x10001U):cell.row>=0 && cell.column<0?0x20003U:0x20002U;
            if(cell.action==1 && cell.trailing && (w.style&0x40U))flags=0;
            if(cell.action==1 && flags){if(c.range_flags&1U && day(cell.date)<day(c.minimum))flags=0x20004U;else if(c.range_flags&2U && day(cell.date)>day(c.maximum))flags=0x20005U;}
            if((cell.action==1 || cell.action==2) && flags){if(!c.view && cell.trailing){const auto tile=cell.calendar;flags|=int(cell.date[0])*12+cell.date[1]-1<c.display_year*12+c.display_month-1+tile?0x2000000U:0x1000000U;}gui_date_write(memory,lp+16,cell.date);}
            if(cell.action==0 && (cell.row>=0 || cell.column>=0))gui_date_write(memory,lp+16,cell.date);
            if(size>=60){memory.check(GuestAddress{lp+32},28,Access::Write);for(unsigned n=0;n<4;++n)memory.store(GuestAddress{lp+32+n*4},32,static_cast<std::uint32_t>(cell.rect[n]));memory.store(GuestAddress{lp+48},32,static_cast<std::uint32_t>(cell.calendar));memory.store(GuestAddress{lp+52},32,static_cast<std::uint32_t>(cell.row));memory.store(GuestAddress{lp+56},32,static_cast<std::uint32_t>(cell.column));}
        }memory.store(GuestAddress{lp+12},32,flags);return flags;
    }
    if(m==0x201 || m==0x200 || m==0x202){if(!w.enabled)return 0;const auto x=static_cast<std::int16_t>(lp),y=static_cast<std::int16_t>(lp>>16);const auto at=hit(c,x,y);if(m==0x201){c.pressed=at;c.dragging=false;c.anchor=c.value;if(at>=0 && !c.picker && c.cells[static_cast<std::size_t>(at)].action==1)c.anchor=c.cells[static_cast<std::size_t>(at)].date;return 0;}if(at<0){if(m==0x202){c.pressed=-1;c.dragging=false;}return 0;}const auto cell=c.cells[static_cast<std::size_t>(at)];if(cell.disabled)return 0;
        if(m==0x200){if(c.pressed<0 || !multi || cell.action!=1 || !(wp&1U))return 0;const auto first=std::min(day(c.anchor),day(cell.date)),last=std::max(day(c.anchor),day(cell.date));if(last-first>=days{c.maximum_selection})return 0;c.dragging=true;auto a=from_day(first),b=from_day(last);std::copy(c.value.begin()+4,c.value.end(),a.begin()+4);std::copy(c.last.begin()+4,c.last.end(),b.begin()+4);c.value=a;c.last=b;notice(c,-749);return 0;}if(c.pressed<0)return 0;c.pressed=-1;
        if(c.picker){clear_input(c);if(cell.action==-5){if(w.style&1U){auto key=msg;key.message=0x100;key.wparam=y<int(w.height)/2?38:40;return gui_calendar_message(memory,w,key,wide,x86);}else {c.open_request=!c.open;c.close_request=c.open;}}else if(cell.action==-4){c.none=!c.none;notice(c,-759);}else if(cell.action>0)c.field=cell.action-1;return 0;}
        if(cell.action==-6 || cell.action==-7){gui_calendar_set_view(c,std::min(3U,c.view+1));return 0;}
        if(cell.action==2){c.display_year=std::clamp<int>(cell.date[0],1601,9999);c.display_month=c.view==1?cell.date[1]:1;gui_calendar_set_view(c,c.view-1);return 0;}
        if(cell.action==-1 || cell.action==-2){auto value=c.value;value[0]=static_cast<std::uint16_t>(c.display_year);value[1]=static_cast<std::uint16_t>(c.display_month);value[3]=1;const auto delta=c.view?(c.view==1?12:c.view==2?120:1200):int(c.month_delta?std::min(c.month_delta,120000U):c.calendar_count);value=add_month(value,(cell.action==-1?-1:1)*delta);c.display_year=value[0];c.display_month=value[1];return 0;}
        if(cell.action==1 || cell.action==-3){if(cell.action==-3)gui_calendar_set_view(c,0);if(!c.dragging)select(c,cell.date,true);notice(c,-746);c.dragging=false;}return 0;
    }
    if(m==0x100){if(!w.enabled)return 0;if(c.picker && c.editing && (wp==13 || wp==27)){c.commit_request=wp==13;c.cancel_request=wp==27;return 0;}if(c.picker && wp==113 && (w.style&0x10U)){if(!c.editing){clear_input(c);c.edit_request=true;}return 0;}if(wp==27 && c.picker_owner){c.close_request=true;return 0;}if(c.picker && wp==115 && !(w.style&1U)){clear_input(c);c.open_request=!c.open;c.close_request=c.open;return 0;}if(c.picker && wp==32 && (w.style&2U)){clear_input(c);c.none=!c.none;++c.picker_revision;notice(c,-759);return 0;}
        if(c.picker && c.field>=9 && picker_cell(c) && wp!=37 && wp!=39 && wp!=9 && wp!=16 && wp!=17 && wp!=18){clear_input(c);c.callback_key=wp;return 0;}
        if(c.picker && (wp==0x6b || wp==0x6d)){auto arrow=msg;arrow.wparam=wp==0x6b?38:40;return gui_calendar_message(memory,w,arrow,wide,x86);}
        if(c.picker && (wp==35 || wp==36)){clear_input(c);const auto limits=picker_limits(c);(void)set_picker_field(c,wp==36?limits.first:limits.second);return 0;}
        if(!c.picker && c.view){
            if(wp==13 || wp==32){gui_calendar_set_view(c,c.view-1);return 0;}
            const int unit=c.view==1?1:c.view==2?12:120;
            if(wp==37 || wp==39 || wp==38 || wp==40 || wp==33 || wp==34){const int delta=(wp==37 || wp==38 || wp==33)?-1:1;const int steps=(wp==38 || wp==40)?4:(wp==33 || wp==34)?12:1;auto value=c.value;value[0]=static_cast<std::uint16_t>(c.display_year);value[1]=static_cast<std::uint16_t>(c.display_month);value[3]=1;value=add_month(value,delta*steps*unit);c.display_year=value[0];c.display_month=value[1];return 0;}
            if(wp==35 || wp==36){if(c.view==1)c.display_month=wp==36?1:12;else {const int span=c.view==2?10:100;c.display_year=(c.display_year/span)*span+(wp==36?0:span-(c.view==2?1:10));}return 0;}
        }
        if(c.picker && (wp==37 || wp==39)){clear_input(c);std::vector<int> fields;for(const auto& cell:c.cells)if(cell.action>0 && std::find(fields.begin(),fields.end(),cell.action-1)==fields.end())fields.push_back(cell.action-1);if(!fields.empty()){const auto found=std::find(fields.begin(),fields.end(),c.field);const auto i=found==fields.end()?0:static_cast<std::size_t>(found-fields.begin());c.field=fields[(i+(wp==37?fields.size()-1:1))%fields.size()];}return 0;}
        if(!c.picker && (wp==35 || wp==36) && !(msg.modifiers&2U)){auto value=c.value;value[3]=wp==36?1:static_cast<std::uint16_t>(unsigned(year_month_day_last{year{value[0]},month_day_last{month{value[1]}}}.day()));if(c.range_flags&1U && day(value)<day(c.minimum))value=c.minimum;if(c.range_flags&2U && day(value)>day(c.maximum))value=c.maximum;select(c,value,true);return 0;}
        if(!c.picker && wp==13){notice(c,-746);return 0;}
        if(wp==37 || wp==39 || wp==38 || wp==40 || wp==33 || wp==34){clear_input(c);if(c.picker && !picker_cell(c))return 0;if(c.picker && (wp==38 || wp==40)){(void)gui_date_field_step(c,wp==38?1:-1);return 0;}
            const int delta=c.picker && (wp==38 || wp==40)?(wp==38?1:-1):(wp==37 || wp==38 || wp==33)?-1:1;auto value=c.value;if(wp==33 || wp==34 || (c.picker && c.field==1))value=add_month(value,delta*((!c.picker && (msg.modifiers&2U) && (wp==33 || wp==34))?static_cast<int>(c.calendar_count):1));else if(c.picker && c.field==0)value=add_month(value,delta*12);else if(c.picker && c.field>=4){auto s=stamp(value,true)+delta*(c.field==4?3600000LL:c.field==5?60000LL:1000LL);const auto date=from_day(floor<days>(sys_time<milliseconds>{milliseconds{s}}));value=date;const auto remainder=(s%86400000+86400000)%86400000;value[4]=static_cast<std::uint16_t>(remainder/3600000);value[5]=static_cast<std::uint16_t>(remainder/60000%60);value[6]=static_cast<std::uint16_t>(remainder/1000%60);value[7]=static_cast<std::uint16_t>(remainder%1000);}else {const auto old=value;value=from_day(day(value)+days{delta*(!c.picker && (wp==38 || wp==40)?7:1)});if(c.picker)std::copy(old.begin()+4,old.end(),value.begin()+4);}if(gui_date_valid(value,c.picker) && in_range(c,value))select(c,value,true);return 0;}
    }
    (void)wide;return std::nullopt;
}
}
