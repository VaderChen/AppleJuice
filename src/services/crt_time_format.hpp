#pragma once
#include "windows_locale_data.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace x64arm::crt_time {
enum class Error { None, Invalid, Overflow };
struct Result {std::u16string text;Error error{};};
class Formatter {
public:
    Formatter(const CrtTimeData& data,const std::tm& value,std::u16string zone,std::int64_t offset,std::size_t capacity)
        :data_(data),value_(value),zone_(std::move(zone)),offset_(offset),capacity_(capacity){}
    Result run(std::u16string_view format){expand(format,0);return {std::move(output_),error_};}
private:
    const CrtTimeData& data_;const std::tm& value_;std::u16string zone_,output_;std::int64_t offset_{};std::size_t capacity_{};Error error_{};
    bool valid(int value,int low,int high){if(value<low || value>high){error_=Error::Invalid;return false;}return true;}
    void append(std::u16string_view text){if(error_!=Error::None)return;if(output_.size()>=capacity_ || text.size()>=capacity_-output_.size()){error_=Error::Overflow;return;}output_+=text;}
    void number(std::int64_t value,unsigned width,bool alternate=false,char16_t padding=u'0'){
        const auto raw=std::to_string(value);std::u16string text(raw.begin(),raw.end());if(!alternate && text.size()<width)text.insert(text.begin(),width-text.size(),padding);append(text);
    }
    bool date_fields(){return valid(value_.tm_year,-1900,8099) && valid(value_.tm_mon,0,11) && valid(value_.tm_mday,1,31);}
    bool week_fields(){return valid(value_.tm_year,-1900,8099) && valid(value_.tm_wday,0,6) && valid(value_.tm_yday,0,365);}
    std::pair<int,int> iso_week(){
        const auto weekday=value_.tm_wday?value_.tm_wday:7;int year=value_.tm_year+1900,week=(value_.tm_yday+11-weekday)/7;
        const auto january=((weekday-1-value_.tm_yday%7+14)%7)+1;
        const auto leap=[](int y){return y%4==0 && (y%100!=0 || y%400==0);};
        if(!week){const auto previous=((january-1-(leap(year-1)?2:1)+7)%7)+1;--year;week=previous==4 || (previous==3 && leap(year))?53:52;}
        else if(week==53 && january!=4 && !(january==3 && leap(year))){week=1;++year;}return {year,week};
    }
    void winword(unsigned field,unsigned depth){
        if(depth>8){error_=Error::Invalid;return;}
        if(data_.calendar!=1){const auto result=windows_crt_time_calendar(data_,value_,field);if(result)append(*result);else error_=Error::Invalid;return;}
        const auto& pattern=data_.fields[field];
        for(std::size_t at=0;at<pattern.size() && error_==Error::None;){
            const auto c=pattern[at];std::size_t end=at+1;while(end<pattern.size() && pattern[end]==c)++end;const auto repeat=end-at;
            if(c==u'\''){
                if(!(repeat%2)){at=end;continue;}at=end;const auto close=pattern.find(u'\'',at);append(std::u16string_view(pattern).substr(at,close==pattern.npos?pattern.size()-at:close-at));at=close==pattern.npos?pattern.size():close+1;continue;
            }
            char16_t spec=0;bool alternate=false;
            if(c==u'M'){spec=repeat==1 || repeat==2?u'm':repeat==3?u'b':repeat==4?u'B':0;alternate=repeat==1;}
            else if(c==u'd'){spec=repeat==1 || repeat==2?u'd':repeat==3?u'a':repeat==4?u'A':0;alternate=repeat==1;}
            else if(c==u'y')spec=repeat==2?u'y':repeat==4?u'Y':0;
            else if(c==u'h' || c==u'H' || c==u'm' || c==u's'){spec=c==u'h'?u'I':c==u'H'?u'H':c==u'm'?u'M':u'S';alternate=repeat==1;}
            else if(c==u't'){
                if(valid(value_.tm_hour,0,23)){const auto& marker=data_.fields[value_.tm_hour<12?38:39];append(repeat==1?std::u16string_view(marker).substr(0,1):std::u16string_view(marker));}at=end;continue;
            }else if((c==u'a' || c==u'A') && at+5<=pattern.size()){
                auto marker=pattern.substr(at,5);for(auto& v:marker)if(v>=u'A' && v<=u'Z')v+=32;
                if(marker==u"am/pm"){specifier(u'p',false,depth+1);at+=5;continue;}
            }
            if(spec)specifier(spec,alternate,depth+1);else append(std::u16string_view(pattern).substr(at,repeat));at=end;
        }
    }
    void specifier(char16_t c,bool alternate,unsigned depth){
        if(depth>8){error_=Error::Invalid;return;}
        const auto clock=[&](std::u16string_view format){expand(format,depth+1);};
        switch(c){
        case u'%':append(u"%");break;
        case u'a':case u'A':if(valid(value_.tm_wday,0,6))append(data_.fields[(c==u'a'?0U:7U)+static_cast<unsigned>(value_.tm_wday)]);break;
        case u'b':case u'h':case u'B':if(valid(value_.tm_mon,0,11))append(data_.fields[(c==u'B'?26U:14U)+static_cast<unsigned>(value_.tm_mon)]);break;
        case u'c':if(data_.classic && !alternate)clock(u"%a %b %e %T %Y");else{winword(alternate?41:40,depth+1);append(u" ");winword(42,depth+1);}break;
        case u'C':case u'y':case u'Y':if(valid(value_.tm_year,-1900,8099)){const auto year=value_.tm_year+1900;number(c==u'C'?year/100:c==u'y'?year%100:year,c==u'Y'?4:2,alternate);}break;
        case u'd':case u'e':if(valid(value_.tm_mday,1,31))number(value_.tm_mday,2,alternate,c==u'e'?u' ':u'0');break;
        case u'D':clock(alternate?u"%#m/%#d/%#y":u"%m/%d/%y");break;
        case u'F':clock(alternate?u"%#Y-%#m-%#d":u"%Y-%m-%d");break;
        case u'g':case u'G':case u'V':if(week_fields()){const auto [year,week]=iso_week();number(c==u'V'?week:c==u'g'?year%100:year,c==u'G'?4:2,c==u'V'?alternate:false);}break;
        case u'H':case u'I':if(valid(value_.tm_hour,0,23))number(c==u'H'?value_.tm_hour:value_.tm_hour%12?value_.tm_hour%12:12,2,alternate);break;
        case u'j':if(valid(value_.tm_yday,0,365))number(value_.tm_yday+1,3,alternate);break;
        case u'm':if(valid(value_.tm_mon,0,11))number(value_.tm_mon+1,2,alternate);break;
        case u'M':if(valid(value_.tm_min,0,59))number(value_.tm_min,2,alternate);break;
        case u'n':append(u"\n");break;
        case u'p':if(valid(value_.tm_hour,0,23))append(data_.fields[value_.tm_hour<12?38:39]);break;
        case u'r':if(data_.classic)clock(alternate?u"%#I:%#M:%#S %p":u"%I:%M:%S %p");else winword(42,depth+1);break;
        case u'R':clock(alternate?u"%#H:%#M":u"%H:%M");break;
        case u'S':if(valid(value_.tm_sec,0,60))number(value_.tm_sec,2,alternate);break;
        case u't':append(u"\t");break;
        case u'T':clock(alternate?u"%#H:%#M:%#S":u"%H:%M:%S");break;
        case u'u':case u'w':if(valid(value_.tm_wday,0,6))number(c==u'u' && !value_.tm_wday?7:value_.tm_wday,1,alternate);break;
        case u'U':case u'W':if(valid(value_.tm_wday,0,6) && valid(value_.tm_yday,0,365)){const auto weekday=c==u'U'?value_.tm_wday:(value_.tm_wday+6)%7;number((value_.tm_yday+7-weekday)/7,2,alternate);}break;
        case u'x':if(date_fields())winword(alternate?41:40,depth+1);break;
        case u'X':winword(42,depth+1);break;
        case u'z':if(value_.tm_isdst>=0){append(offset_<0?u"-":u"+");const auto magnitude=offset_<0?-offset_:offset_;number(magnitude/3600,2);number((magnitude/60)%60,2);}break;
        case u'Z':append(zone_);break;
        default:error_=Error::Invalid;break;
        }
    }
    void expand(std::u16string_view format,unsigned depth){
        if(depth>8){error_=Error::Invalid;return;}
        for(std::size_t at=0;at<format.size() && error_==Error::None;){
            const auto percent=format.find(u'%',at);if(percent==format.npos){append(format.substr(at));break;}append(format.substr(at,percent-at));at=percent+1;
            if(at==format.size()){error_=Error::Invalid;break;}const bool alternate=format[at]==u'#';if(alternate && ++at==format.size()){error_=Error::Invalid;break;}
            specifier(format[at++],alternate,depth+1);
        }
    }
};
}
