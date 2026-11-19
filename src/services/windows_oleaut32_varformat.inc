// oleaut32 的 VB 格式字串（VarFormat／VarTokenizeFormatString／VarFormatFromTokens）：由 windows_oleaut32_conversions.cpp 引入。
// 以 ';' 分段：數值格式為 正數;負數;零;Null，字串格式為 有內容;空字串或 Null。各段依記號判斷種類：
// - 數值：0／# 位數、. 小數點、, 千分位（位於位數之間）或除以 1000（緊接小數點或整數部分尾端）、% 乘 100、E+／E-／e+／e- 指數；
//   數值先轉成精確的 DECIMAL 十進位字串，捨入為遠離零的四捨五入（與 VB Format 相同）。
// - 日期：c、d～dddddd、w、ww、m～mmmm（緊接 h 之後或後接 s 時為分鐘）、q、y、yy、yyyy、h、hh、n、nn、s、ss、ttttt、
//   AM/PM、am/pm、A/P、a/p、AMPM；/ 與 : 為地區的日期與時間分隔符號。
// - 字串：@（字元或空白）、&（字元或不顯示）、< 全小寫、> 全大寫、! 由左向右填入（預設由右向左）。
// 引號內文字與 \ 之後的字元原樣輸出。具名格式（General Number、Currency、Fixed、Standard、Percent、Scientific、Yes/No、
// True/False、On/Off、General／Long／Medium／Short Date、Long／Medium／Short Time）先展開。
// 記號緩衝區為本實作自己的版面：標頭（簽章、iFirstDay、iFirstWeek、字元數、LCID）後接 UTF-16 格式字串；應用程式只把它當成不透明資料。
namespace {
struct FormatLocale {
    std::u16string decimal{u"."},thousands{u","},date_separator{u"/"},time_separator{u":"},am{u"AM"},pm{u"PM"};
    std::array<std::u16string,7> days,short_days; // 0＝星期日
    std::array<std::u16string,12> months,short_months;
    unsigned first_day{1},first_week{1}; // VB 常數：1＝星期日；1＝含 1 月 1 日的週、2＝至少四天、3＝第一個完整週
    // 值本身以地區格式化的日期／時間（c、ddddd、dddddd、ttttt 與具名日期格式）。
    std::u16string general_date,short_date,long_date,long_time;
};
struct FormatValue {
    enum class Kind {Null,Empty,Number,Text} kind{Kind::Empty};
    bool negative{},zero{true},has_date{};
    std::string magnitude{"0"}; // 精確十進位絕對值：只含數字與小數點
    std::array<unsigned,8> date{}; // SYSTEMTIME 欄位順序：年、月、星期、日、時、分、秒、毫秒
    std::u16string text;
};
bool format_letter(char16_t c,char16_t lower){return c==lower || c==static_cast<char16_t>(lower-32);}
// 依引號與反斜線切成段落（保留原文，供各段再解析）。
std::vector<std::u16string> format_sections(std::u16string_view format){
    std::vector<std::u16string> sections(1);bool quoted=false;
    for(std::size_t i=0;i<format.size();++i){
        const auto c=format[i];
        if(c==u'"')quoted=!quoted;
        else if(!quoted && c==u'\\' && i+1<format.size()){sections.back()+=c;sections.back()+=format[++i];continue;}
        else if(!quoted && c==u';'){sections.emplace_back();continue;}
        sections.back()+=c;
    }
    return sections;
}
enum class SectionKind {Literal,Number,Date,Text};
SectionKind section_kind(std::u16string_view s){
    bool number=false,date=false,text=false,quoted=false;
    for(std::size_t i=0;i<s.size();++i){
        const auto c=s[i];
        if(c==u'"'){quoted=!quoted;continue;}if(quoted)continue;
        if(c==u'\\'){++i;continue;}
        if(c==u'0' || c==u'#')number=true;
        else if(c==u'@' || c==u'&' || c==u'<' || c==u'>' || c==u'!')text=true;
        else for(const auto letter:{u'd',u'm',u'y',u'h',u'n',u's',u'w',u'q',u'c'})if(format_letter(c,letter))date=true;
        if(s.substr(i).size()>=5 && (s.substr(i,5)==u"AM/PM" || s.substr(i,5)==u"am/pm"))date=true;
        if(s.substr(i).size()>=3 && (s.substr(i,3)==u"A/P" || s.substr(i,3)==u"a/p"))date=true;
    }
    return number?SectionKind::Number:text?SectionKind::Text:date?SectionKind::Date:SectionKind::Literal;
}
// 十進位字串（magnitude）乘以 10^shift。
std::string shifted(const std::string& magnitude,long shift){return plain_decimal(magnitude,shift);}
// 四捨五入到小數 places 位（遠離零）；回傳 {整數位數字（無前導零，零為 ""）, 小數位數字（長度 places）}。
std::pair<std::string,std::string> rounded(const std::string& magnitude,unsigned places){
    const auto point=magnitude.find('.');std::string integral=point==std::string::npos?magnitude:magnitude.substr(0,point);
    std::string fraction=point==std::string::npos?std::string{}:magnitude.substr(point+1);
    const bool up=fraction.size()>places && fraction[places]>='5';
    fraction.resize(places,'0');
    std::string digits=integral+fraction;
    if(up){std::size_t i=digits.size();while(i>0){--i;if(digits[i]=='9'){digits[i]='0';if(!i){digits.insert(digits.begin(),'1');break;}}else{++digits[i];break;}}}
    integral=digits.substr(0,digits.size()-places);fraction=digits.substr(digits.size()-places);
    const auto first=integral.find_first_not_of('0');integral=first==std::string::npos?std::string{}:integral.substr(first);
    return {integral,fraction};
}
std::u16string widen(const std::string& text){return std::u16string(text.begin(),text.end());}
// 數值段落。prefix_sign 為使用正數段格式化負數時是否在最前面加負號。
std::u16string format_number_section(std::u16string_view s,const FormatValue& value,const FormatLocale& locale,bool prefix_sign){
    struct Token {enum Type {Literal,Zero,Hash,Point,Comma,Percent,Exponent} type;std::u16string text;bool plus{};};
    std::vector<Token> tokens;bool quoted=false;
    for(std::size_t i=0;i<s.size();++i){
        const auto c=s[i];
        if(c==u'"'){quoted=!quoted;continue;}
        if(quoted){tokens.push_back({Token::Literal,std::u16string(1,c)});continue;}
        if(c==u'\\'){if(i+1<s.size())tokens.push_back({Token::Literal,std::u16string(1,s[++i])});continue;}
        if(c==u'0')tokens.push_back({Token::Zero,{}});
        else if(c==u'#')tokens.push_back({Token::Hash,{}});
        else if(c==u'.')tokens.push_back({Token::Point,{}});
        else if(c==u',')tokens.push_back({Token::Comma,{}});
        else if(c==u'%')tokens.push_back({Token::Percent,{}});
        else if((c==u'E' || c==u'e') && i+1<s.size() && (s[i+1]==u'+' || s[i+1]==u'-')){tokens.push_back({Token::Exponent,std::u16string(1,c),s[i+1]==u'+'});++i;}
        else tokens.push_back({Token::Literal,std::u16string(1,c)});
    }
    // 位數分區：整數、小數、指數。
    std::size_t point=tokens.size(),exponent=tokens.size();
    for(std::size_t i=0;i<tokens.size();++i){if(tokens[i].type==Token::Exponent && exponent==tokens.size())exponent=i;if(tokens[i].type==Token::Point && point==tokens.size() && i<exponent)point=i;}
    const auto digit=[&](std::size_t i){return tokens[i].type==Token::Zero || tokens[i].type==Token::Hash;};
    std::vector<std::size_t> integer,fraction,exponent_digits;
    for(std::size_t i=0;i<tokens.size();++i){if(!digit(i))continue;if(i<point && i<exponent)integer.push_back(i);else if(i<exponent)fraction.push_back(i);else exponent_digits.push_back(i);}
    // 逗號：位於整數位數之間為千分位；緊接小數點（或整數部分最後一個位數之後）的連續逗號各除以 1000。
    bool grouping=false;long scale=0;
    const auto integer_end=std::min(point,exponent);
    if(!integer.empty()){
        std::size_t i=integer_end;while(i>0 && tokens[i-1].type!=Token::Zero && tokens[i-1].type!=Token::Hash){if(tokens[i-1].type==Token::Comma)++scale;--i;}
        for(std::size_t j=integer.front();j<integer.back();++j)if(tokens[j].type==Token::Comma)grouping=true;
    }
    long percents=0;for(const auto& token:tokens)if(token.type==Token::Percent)++percents;
    auto magnitude=shifted(value.magnitude,2*percents-3*scale);
    long power=0;
    if(exponent<tokens.size() && magnitude!="0"){
        // 指數：尾數的整數位數等於格式的整數位數（至少一位）。
        const auto dot=magnitude.find('.');const std::string integral=dot==std::string::npos?magnitude:magnitude.substr(0,dot);
        long magnitude_power=0;
        if(integral!="0")magnitude_power=static_cast<long>(integral.size())-1;
        else{const auto fraction_text=magnitude.substr(dot+1);magnitude_power=-static_cast<long>(fraction_text.find_first_not_of('0'))-1;}
        const auto width=static_cast<long>(std::max<std::size_t>(integer.size(),1));
        power=magnitude_power-(width-1);magnitude=shifted(magnitude,-power);
        auto [whole,part]=rounded(magnitude,static_cast<unsigned>(fraction.size()));
        if(static_cast<long>(whole.size())>width){++power;magnitude=shifted(value.magnitude,2*percents-3*scale-power);}
    }
    auto [whole,part]=rounded(magnitude,static_cast<unsigned>(fraction.size()));
    // 整數位數：自最左邊的 0 起都要顯示。
    std::size_t required=0;for(std::size_t k=0;k<integer.size();++k)if(tokens[integer[k]].type==Token::Zero){required=integer.size()-k;break;}
    if(whole.size()<required)whole.insert(0,required-whole.size(),'0');
    // 小數尾端的 # 位數不顯示 0。
    std::size_t shown=part.size();while(shown>0 && tokens[fraction[shown-1]].type==Token::Hash && part[shown-1]=='0')--shown;
    std::u16string grouped;
    if(grouping){for(std::size_t i=0;i<whole.size();++i){if(i && (whole.size()-i)%3==0)grouped+=locale.thousands;grouped+=static_cast<char16_t>(whole[i]);}}
    std::u16string out;
    if(prefix_sign)out+=u'-';
    std::size_t integer_index=0,fraction_index=0,exponent_index=0;std::u16string exponent_text;
    if(exponent<tokens.size()){
        const auto magnitude_text=std::to_string(power<0?-power:power);std::size_t minimum=0;
        for(std::size_t k=0;k<exponent_digits.size();++k)if(tokens[exponent_digits[k]].type==Token::Zero){minimum=exponent_digits.size()-k;break;}
        exponent_text=widen(std::string(minimum>magnitude_text.size()?minimum-magnitude_text.size():0,'0')+magnitude_text);
    }
    for(std::size_t i=0;i<tokens.size();++i){
        const auto& token=tokens[i];
        if(digit(i) && i<integer_end){
            if(grouping){if(!integer_index)out+=grouped;}
            else{
                const auto count=integer.size();const auto have=whole.size();
                if(!integer_index && have>count)out+=widen(whole.substr(0,have-count));
                const auto slot=static_cast<long>(integer_index)-static_cast<long>(count)+static_cast<long>(have);
                if(slot>=0 && static_cast<std::size_t>(slot)<have)out+=static_cast<char16_t>(whole[static_cast<std::size_t>(slot)]);
            }
            ++integer_index;continue;
        }
        if(digit(i) && i<exponent){if(fraction_index<shown)out+=static_cast<char16_t>(part[fraction_index]);++fraction_index;continue;}
        if(digit(i)){if(!exponent_index)out+=exponent_text;++exponent_index;continue;}
        switch(token.type){
        case Token::Point:if(i==point)out+=locale.decimal;break;
        case Token::Comma:break;
        case Token::Percent:out+=u'%';break;
        case Token::Exponent:out+=token.text;if(power<0)out+=u'-';else if(token.plus)out+=u'+';
            if(exponent_digits.empty())out+=exponent_text;break;
        default:out+=token.text;
        }
    }
    return out;
}
// 星期在週內的位置（0 起），first_day 為 VB 常數（1＝星期日）。
unsigned weekday_offset(unsigned dow,unsigned first_day){return (dow+7-(first_day-1))%7;}
bool leap_year(unsigned year){return (year%4==0 && year%100!=0) || year%400==0;}
unsigned day_of_year(unsigned year,unsigned month,unsigned day){
    static constexpr std::array<unsigned,12> before{0,31,59,90,120,151,181,212,243,273,304,334};
    return before[month-1]+day+(month>2 && leap_year(year)?1U:0U);
}
// DatePart("ww")：依每週第一天與第一週規則計算。
unsigned week_of_year(unsigned year,unsigned month,unsigned day,unsigned dow,unsigned first_day,unsigned first_week){
    const auto ordinal=static_cast<long>(day_of_year(year,month,day));
    const auto january_offset=weekday_offset((dow+7*60-static_cast<unsigned>((ordinal-1)%7))%7,first_day);
    long start=1-static_cast<long>(january_offset); // 第一週第一天的年內序號（可能落在前一年）
    if(first_week==3 && january_offset)start+=7;
    else if(first_week==2 && 7-january_offset<4)start+=7;
    if(ordinal<start){
        // 落在前一年最後一週：以 12 月 31 日計算。
        const auto previous_dow=(dow+7*60-static_cast<unsigned>(ordinal%7))%7;
        return week_of_year(year-1,12,31,previous_dow,first_day,first_week);
    }
    return static_cast<unsigned>((ordinal-start)/7+1);
}
std::u16string two_digits(unsigned value){std::u16string out;if(value<10)out+=u'0';out+=widen(std::to_string(value));return out;}
std::u16string format_date_section(std::u16string_view s,const FormatValue& value,const FormatLocale& locale,unsigned first_day,unsigned first_week){
    const auto& d=value.date;const unsigned year=d[0],month=d[1],dow=d[2],day=d[3],hour=d[4],minute=d[5],second=d[6];
    bool twelve=false;
    for(std::size_t i=0;i<s.size();++i){const auto rest=s.substr(i);if(rest.starts_with(u"AM/PM") || rest.starts_with(u"am/pm") || rest.starts_with(u"A/P") || rest.starts_with(u"a/p") ||
        (rest.size()>=4 && format_letter(rest[0],u'a') && format_letter(rest[1],u'm') && format_letter(rest[2],u'p') && format_letter(rest[3],u'm')))twelve=true;}
    const auto display_hour=twelve?(hour%12?hour%12:12U):hour;
    std::u16string out;bool quoted=false,after_hour=false;
    // m 之後（略過字面字元）是否接 s：用於判斷分鐘。
    const auto next_is_second=[&](std::size_t from){
        for(std::size_t j=from;j<s.size();++j){const auto c=s[j];if(c==u'"' || c==u'\\')return false;
            if(format_letter(c,u's'))return true;for(const auto letter:{u'd',u'm',u'y',u'h',u'n',u'w',u'q',u'c',u't'})if(format_letter(c,letter))return false;}
        return false;};
    for(std::size_t i=0;i<s.size();){
        const auto c=s[i];
        if(c==u'"'){quoted=!quoted;++i;continue;}
        if(quoted){out+=c;++i;continue;}
        if(c==u'\\'){if(i+1<s.size())out+=s[i+1];i+=2;continue;}
        const auto rest=s.substr(i);
        if(rest.starts_with(u"AM/PM") || rest.starts_with(u"am/pm")){out+=hour<12?(c==u'A'?u"AM":u"am"):(c==u'A'?u"PM":u"pm");i+=5;continue;}
        if(rest.starts_with(u"A/P") || rest.starts_with(u"a/p")){out+=hour<12?(c==u'A'?u"A":u"a"):(c==u'A'?u"P":u"p");i+=3;continue;}
        if(rest.size()>=4 && format_letter(rest[0],u'a') && format_letter(rest[1],u'm') && format_letter(rest[2],u'p') && format_letter(rest[3],u'm')){out+=hour<12?locale.am:locale.pm;i+=4;continue;}
        std::size_t run=1;const auto lower=static_cast<char16_t>(c>=u'A' && c<=u'Z'?c+32:c);
        while(i+run<s.size() && (s[i+run]==lower || s[i+run]==static_cast<char16_t>(lower-32)))++run;
        switch(lower){
        case u'c':out+=locale.general_date;after_hour=false;break;
        case u'd':
            if(run==1)out+=widen(std::to_string(day));else if(run==2)out+=two_digits(day);else if(run==3)out+=locale.short_days[dow];
            else if(run==4)out+=locale.days[dow];else if(run==5)out+=locale.short_date;else out+=locale.long_date;
            after_hour=false;break;
        case u'w':
            if(run==1)out+=widen(std::to_string(weekday_offset(dow,first_day)+1));
            else out+=widen(std::to_string(week_of_year(year,month,day,dow,first_day,first_week)));
            after_hour=false;break;
        case u'm':
            if(run<=2 && (after_hour || next_is_second(i+run)))out+=run==1?widen(std::to_string(minute)):two_digits(minute);
            else if(run==1)out+=widen(std::to_string(month));else if(run==2)out+=two_digits(month);
            else if(run==3)out+=locale.short_months[month-1];else out+=locale.months[month-1];
            after_hour=false;break;
        case u'q':out+=widen(std::to_string((month-1)/3+1));after_hour=false;break;
        case u'y':
            if(run==1)out+=widen(std::to_string(day_of_year(year,month,day)));
            else if(run<4){out+=two_digits(year%100);if(run==3)out+=widen(std::to_string(day_of_year(year,month,day)));}
            else out+=widen(std::to_string(year));
            after_hour=false;break;
        case u'h':out+=run==1?widen(std::to_string(display_hour)):two_digits(display_hour);after_hour=true;break;
        case u'n':out+=run==1?widen(std::to_string(minute)):two_digits(minute);after_hour=false;break;
        case u's':out+=run==1?widen(std::to_string(second)):two_digits(second);after_hour=false;break;
        case u't':if(run>=5){out+=locale.long_time;run=5;}else out+=std::u16string(rest.substr(0,run));after_hour=false;break;
        default:
            run=1;
            if(c==u'/')out+=locale.date_separator;else if(c==u':')out+=locale.time_separator;else out+=c;
        }
        i+=run;
    }
    return out;
}
std::u16string format_text_section(std::u16string_view s,std::u16string_view text){
    struct Token {bool placeholder{},space{};std::u16string literal;};
    std::vector<Token> tokens;bool lower=false,upper=false,left=false,quoted=false;
    for(std::size_t i=0;i<s.size();++i){
        const auto c=s[i];
        if(c==u'"'){quoted=!quoted;continue;}
        if(quoted){tokens.push_back({false,false,std::u16string(1,c)});continue;}
        if(c==u'\\'){if(i+1<s.size())tokens.push_back({false,false,std::u16string(1,s[++i])});continue;}
        if(c==u'@' || c==u'&')tokens.push_back({true,c==u'@',{}});
        else if(c==u'<'){lower=true;upper=false;}else if(c==u'>'){upper=true;lower=false;}else if(c==u'!')left=true;
        else tokens.push_back({false,false,std::u16string(1,c)});
    }
    std::u16string data(text);
    for(auto& c:data){if(lower)c=unicode_case_unit(c,false);else if(upper)c=unicode_case_unit(c,true);}
    std::size_t placeholders=0;for(const auto& token:tokens)if(token.placeholder)++placeholders;
    if(!placeholders){std::u16string out;for(const auto& token:tokens)out+=token.literal;return out+data;}
    std::u16string out;std::size_t seen=0;
    if(left){
        for(const auto& token:tokens){
            if(!token.placeholder){out+=token.literal;continue;}
            if(seen<data.size())out+=data[seen];else if(token.space)out+=u' ';++seen;
        }
        if(data.size()>placeholders)out+=data.substr(placeholders);
    }else{
        // 由右向左：第 k 個位置對應資料的 (k - 位置數 + 資料長度)；多出的字元放在第一個位置之前。
        for(const auto& token:tokens){
            if(!token.placeholder){out+=token.literal;continue;}
            if(!seen && data.size()>placeholders)out+=data.substr(0,data.size()-placeholders);
            const auto slot=static_cast<long>(seen)-static_cast<long>(placeholders)+static_cast<long>(data.size());
            if(slot>=0)out+=data[static_cast<std::size_t>(slot)];else if(token.space)out+=u' ';++seen;
        }
    }
    return out;
}
// 具名格式展開：數值與日期格式回傳等效的格式字串；Currency 與地區日期格式另以 named 傳回種類。
enum class NamedFormat {None,GeneralNumber,Currency,YesNo,TrueFalse,OnOff,GeneralDate,LongDate,ShortDate,LongTime,ShortTime};
NamedFormat named_format(std::u16string_view format,std::u16string& expanded){
    std::u16string lower(format);for(auto& c:lower)c=unicode_case_unit(c,false);
    if(lower==u"general number")return NamedFormat::GeneralNumber;
    if(lower==u"currency")return NamedFormat::Currency;
    if(lower==u"fixed"){expanded=u"0.00";return NamedFormat::None;}
    if(lower==u"standard"){expanded=u"#,##0.00";return NamedFormat::None;}
    if(lower==u"percent"){expanded=u"0.00%";return NamedFormat::None;}
    if(lower==u"scientific"){expanded=u"0.00E+00";return NamedFormat::None;}
    if(lower==u"yes/no")return NamedFormat::YesNo;
    if(lower==u"true/false")return NamedFormat::TrueFalse;
    if(lower==u"on/off")return NamedFormat::OnOff;
    if(lower==u"general date")return NamedFormat::GeneralDate;
    if(lower==u"long date")return NamedFormat::LongDate;
    if(lower==u"medium date"){expanded=u"dd-mmm-yy";return NamedFormat::None;}
    if(lower==u"short date")return NamedFormat::ShortDate;
    if(lower==u"long time")return NamedFormat::LongTime;
    if(lower==u"medium time"){expanded=u"hh:nn AMPM";return NamedFormat::None;}
    if(lower==u"short time")return NamedFormat::ShortTime;
    expanded=std::u16string(format);return NamedFormat::None;
}
// 依值與段落選擇格式化；回傳 nullopt 表示結果為 NULL BSTR（Null 值且沒有 Null 段）。fallback 為沒有可用段落時的一般字串。
std::optional<std::u16string> apply_format(std::u16string_view format,const FormatValue& value,const FormatLocale& locale,unsigned first_day,unsigned first_week,const std::u16string& fallback){
    const auto sections=format_sections(format);const auto kind=section_kind(sections.front());
    const auto section=[&](std::size_t index)->const std::u16string*{return index<sections.size()?&sections[index]:nullptr;};
    if(kind==SectionKind::Text){
        const bool empty=value.kind==FormatValue::Kind::Null || value.kind==FormatValue::Kind::Empty || value.text.empty();
        const auto* chosen=empty && section(1)?section(1):section(0);
        if(value.kind==FormatValue::Kind::Null && !section(1))return std::nullopt;
        return format_text_section(*chosen,value.text);
    }
    if(value.kind==FormatValue::Kind::Null){if(const auto* null_section=section(3))return format_number_section(*null_section,value,locale,false);return std::nullopt;}
    if(kind==SectionKind::Date){
        if(!value.has_date)return fallback;
        return format_date_section(sections.front(),value,locale,first_day,first_week);
    }
    if(value.kind==FormatValue::Kind::Text)return value.text; // 無法轉成數值的字串原樣顯示
    const std::u16string* chosen=&sections.front();bool sign=value.negative;
    if(value.zero && section(2) && !section(2)->empty()){chosen=section(2);sign=false;}
    else if(value.negative && section(1) && !section(1)->empty()){chosen=section(1);sign=false;}
    if(section_kind(*chosen)==SectionKind::Date)return value.has_date?format_date_section(*chosen,value,locale,first_day,first_week):fallback;
    return format_number_section(*chosen,value,locale,sign && !value.zero);
}
constexpr std::uint32_t format_token_signature=0x46544b54U; // 'TKTF'
}
