#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// std::time_get 的格式解析（C locale）：語義依 MSVC 的 get(格式範圍)、do_get、_Getint 與 _Getloctxt。
// 輸入以「已取走的字元＋一個前瞻」重播：解析器需要尚未取得的前瞻時丟出 Need::Peek，
// 決定接受前瞻字元時丟出 Need::Consume；呼叫端取得或取走字元後從頭重跑。字元數很少（時間字串），重跑成本可忽略。
namespace x64arm::msvcp_time_get {
struct Need {enum class Kind {Peek,Consume} kind;};
inline constexpr int unknown=-2,end_of_input=-1;
struct Fields {std::optional<std::int32_t> sec,min,hour,mday,mon,year,wday,yday;};
struct Result {Fields fields;unsigned state{};bool at_end{};};
class Cursor {
public:
    Cursor(std::u16string_view seen,int look):seen_(seen),look_(look){}
    // 目前位置的字元；nullopt 表示輸入結束。
    std::optional<char16_t> peek() const {
        if(at_<seen_.size())return seen_[at_];
        if(look_==unknown)throw Need{Need::Kind::Peek};
        if(look_==end_of_input)return std::nullopt;
        return static_cast<char16_t>(look_);
    }
    bool end() const {return !peek();}
    void advance(){if(at_<seen_.size()){++at_;return;}(void)peek();throw Need{Need::Kind::Consume};}
private:
    std::u16string_view seen_;int look_;std::size_t at_{};
};
inline bool space(char16_t c){return c==u' ' || (c>=u'\t' && c<=u'\r');}
inline char16_t lower(char16_t c){return c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;}
inline constexpr unsigned eofbit=1,failbit=2;
// _Getint：最多讀 hi 的位數（前導空白也計入），可有正負號；超出範圍或沒有數字時 failbit。
inline unsigned get_int(Cursor& in,int lo,int hi,std::int32_t& value){
    const int limit=hi<=9?1:hi<=99?2:hi<=999?3:4;int seen=0;std::string digits;
    while(!in.end() && seen<limit && space(*in.peek())){in.advance();++seen;}
    if(!in.end() && seen<limit){const auto c=*in.peek();if(c==u'+' || c==u'-'){digits.push_back(static_cast<char>(c));in.advance();}}
    while(!in.end() && seen<limit && *in.peek()==u'0'){++seen;in.advance();}
    if(seen>0)digits.push_back('0');
    while(!in.end() && seen<limit && *in.peek()>=u'0' && *in.peek()<=u'9'){digits.push_back(static_cast<char>(*in.peek()));in.advance();++seen;}
    unsigned state=in.end()?eofbit:0U;
    bool any=false;long long number=0;bool negative=false;
    for(const auto c:seen?digits:std::string{}){if(c=='+')continue;if(c=='-'){negative=true;continue;}any=true;number=number*10+(c-'0');}
    if(negative)number=-number;
    if(!any || number<lo || number>hi)state|=failbit;else value=static_cast<std::int32_t>(number);
    return state;
}
// _Getloctxt：在以 ':' 分隔的欄位中找出與輸入相符最長者（不分大小寫），一邊比對一邊取走字元；回傳欄位索引，失敗為負值。
inline int get_text(Cursor& in,std::u16string_view fields){
    std::vector<std::u16string_view> names;
    for(std::size_t at=1;at<=fields.size();){const auto next=fields.find(u':',at);names.push_back(fields.substr(at,next==fields.npos?fields.npos:next-at));if(next==fields.npos)break;at=next+1;}
    std::vector<bool> closed(names.size());int answer=-2;
    for(std::size_t column=1;;++column){
        bool prefix=false;
        for(std::size_t field=0;field<names.size();++field){
            if(closed[field])continue;
            if(names[field].size()<column){closed[field]=true;answer=static_cast<int>(field);continue;}
            if(in.end() || lower(*in.peek())!=lower(names[field][column-1]))closed[field]=true;else prefix=true;
        }
        if(!prefix || in.end())break;
        // 取走字元後，之前完整相符的較短欄位不再是答案（MSVC 每推進一欄都把 _Ans 設為 -1），只有之後完整相符者才算。
        in.advance();answer=-1;
    }
    return answer;
}
inline unsigned get_spec(Cursor& in,Fields& out,char16_t spec,char16_t modifier);
// get(格式範圍)：每個元素前若已無輸入則 eof|fail；空白跳過任意輸入空白；字面字元須相同；% 規格交給 do_get。
inline unsigned run(Cursor& in,std::u16string_view format,Fields& out){
    unsigned state=0;
    for(std::size_t at=0;at<format.size() && !state;++at){
        if(in.end()){state=eofbit|failbit;break;}
        const auto c=format[at];
        if(c!=u'%'){
            if(space(c)){while(!in.end() && space(*in.peek()))in.advance();}
            else if(*in.peek()!=c)state=failbit;else in.advance();
            continue;
        }
        if(++at==format.size()){if(*in.peek()!=u'%')state=failbit;else in.advance();break;}
        auto spec=format[at];char16_t modifier=0;
        if(spec==u'E' || spec==u'O' || spec==u'Q' || spec==u'#'){
            if(++at==format.size()){if(*in.peek()!=u'%')state=failbit;else in.advance();break;}
            modifier=spec;spec=format[at];
        }
        state|=get_spec(in,out,spec,modifier);
    }
    if(in.end())state|=eofbit;
    return state;
}
inline unsigned get_spec(Cursor& in,Fields& out,char16_t spec,char16_t){
    constexpr std::u16string_view days=u":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
    constexpr std::u16string_view months=u":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";
    std::int32_t value=0;unsigned state=0;
    const auto field=[&](std::optional<std::int32_t>& target,int lo,int hi,std::int32_t offset=0){state|=get_int(in,lo,hi,value);if(!(state&failbit))target=value+offset;};
    switch(spec){
    case u'a':case u'A':{const auto found=get_text(in,days);if(found<0)state|=failbit;else out.wday=found/2;break;}
    case u'b':case u'B':case u'h':{const auto found=get_text(in,months);if(found<0)state|=failbit;else out.mon=found/2;break;}
    case u'c':state|=run(in,u"%b %d %H:%M:%S %Y",out);break;
    case u'C':field(out.year,0,99);if(!(state&failbit))out.year=value*100-1900;break;
    case u'd':case u'e':field(out.mday,1,31);break;
    case u'D':case u'x':state|=run(in,u"%m/%d/%y",out);break;
    case u'F':state|=run(in,u"%Y-%m-%d",out);break;
    case u'H':field(out.hour,0,23);break;
    case u'I':field(out.hour,1,12);if(!(state&failbit))out.hour=value%12;break;
    case u'j':field(out.yday,1,366,-1);break;
    case u'm':field(out.mon,1,12,-1);break;
    case u'M':field(out.min,0,59);break;
    case u'n':case u't':while(!in.end() && space(*in.peek()))in.advance();break;
    case u'p':{const auto found=get_text(in,u":AM:am:PM:pm");if(found<0)state|=failbit;else if(found>1 && out.hour.value_or(0)<12)out.hour=out.hour.value_or(0)+12;break;}
    case u'r':state|=run(in,u"%I:%M:%S %p",out);break;
    case u'R':state|=run(in,u"%H:%M",out);break;
    case u'S':field(out.sec,0,60);break;
    case u'T':case u'X':state|=run(in,u"%H:%M:%S",out);break;
    case u'U':case u'W':{std::optional<std::int32_t> week;field(week,0,53);break;}
    case u'w':field(out.wday,0,6);break;
    case u'y':field(out.year,0,99);if(!(state&failbit))out.year=value<69?value+100:value;break;
    case u'Y':field(out.year,0,9999,-1900);break;
    case u'%':if(*in.peek()!=u'%')state|=failbit;else in.advance();break;
    default:state|=failbit;break;
    }
    return state;
}
inline Result parse(std::u16string_view format,std::u16string_view seen,int look){
    Cursor in(seen,look);Result result;result.state=run(in,format,result.fields);result.at_end=in.end();return result;
}
}
