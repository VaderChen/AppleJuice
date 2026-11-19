// oleaut32 的 VB 數值／日期格式化與 NUMPARSE 家族（由 windows_oleaut32_conversions.cpp 引入）。
// VarFormatNumber／Currency／Percent／DateTime 先經既有 VariantChangeTypeEx 取得 DECIMAL（超出範圍改 R8）或 DATE，
// BYREF、BSTR 依使用者地區解析、IDispatch 取 DISPID_VALUE 都沿用同一轉型；再以使用者預設地區（含 SetLocaleInfo 覆寫）的
// LOCALE_* 值組出 NUMBERFMT／CURRENCYFMT，交給既有 GetNumberFormatW／GetCurrencyFormatW／GetDateFormatW／GetTimeFormatW。
// VarParseNumFromStr 依地區分隔符號與貨幣符號把字串拆成 NUMPARSE 與數字陣列；VarNumFromParseNum 依 VTBIT 由小到大選型別，
// 十進位值的捨入（ties-to-even）、範圍與 DECIMAL 規格化沿用 number_convert。
namespace {
constexpr std::uint32_t numprs_leading_white=0x1,numprs_trailing_white=0x2,numprs_leading_plus=0x4,numprs_trailing_plus=0x8,
    numprs_leading_minus=0x10,numprs_trailing_minus=0x20,numprs_hex_oct=0x40,numprs_parens=0x80,numprs_decimal=0x100,
    numprs_thousands=0x200,numprs_currency=0x400,numprs_exponent=0x800,numprs_use_all=0x1000,numprs_neg=0x10000,numprs_inexact=0x20000;
// 十進位字串（可含指數）→ 只含數字、小數點與前導負號的固定格式；shift 為額外的十進位位移（百分比為 2）。
std::string plain_decimal(std::string_view text,long shift){
    bool negative=false;if(!text.empty() && (text.front()=='-' || text.front()=='+')){negative=text.front()=='-';text.remove_prefix(1);}
    long exponent=shift;const auto marker=text.find_first_of("eE");
    if(marker!=std::string_view::npos){exponent+=std::stol(std::string(text.substr(marker+1)));text=text.substr(0,marker);}
    std::string digits;long point=-1;
    for(const char c:text){if(c=='.'){point=static_cast<long>(digits.size());continue;}if(c<'0' || c>'9')throw std::invalid_argument("數值格式不合法");digits.push_back(c);}
    if(digits.empty())throw std::invalid_argument("數值為空");
    if(point<0)point=static_cast<long>(digits.size());point+=exponent;
    if(point>4096 || point<-4096)throw std::range_error("數值位數超過範圍");
    std::string integral,fraction;
    if(point<=0){integral="0";fraction=std::string(static_cast<std::size_t>(-point),'0')+digits;}
    else if(static_cast<std::size_t>(point)>=digits.size())integral=digits+std::string(static_cast<std::size_t>(point)-digits.size(),'0');
    else {integral=digits.substr(0,static_cast<std::size_t>(point));fraction=digits.substr(static_cast<std::size_t>(point));}
    const auto first=integral.find_first_not_of('0');integral=first==std::string::npos?"0":integral.substr(first);
    while(!fraction.empty() && fraction.back()=='0')fraction.pop_back();
    const bool zero=integral=="0" && fraction.empty();
    return std::string(negative && !zero?"-":"")+integral+(fraction.empty()?"":"."+fraction);
}
// LOCALE_SGROUPING「3;2;0」→ 32、「3;0」→ 3、「3」→ 30，與 GetNumberFormat 的 NUMBERFMT.Grouping 相同。
unsigned grouping_value(std::u16string_view value){
    std::string digits;for(const auto c:value)if(c>=u'0' && c<=u'9')digits.push_back(static_cast<char>(c));
    if(digits.empty())return 0;if(value.size()>=2 && value.ends_with(u";0"))digits.pop_back();else digits.push_back('0');
    if(digits.empty() || digits.size()>9)return 0;return static_cast<unsigned>(std::stoul(digits));
}
struct NumberSymbols {std::u16string decimal,thousands,currency,money_decimal,money_thousands;};
struct ParsedNumber {std::uint32_t status=0,flags=0;std::int32_t used=0,shift=0,power=0;std::vector<std::uint8_t> digits;};
bool number_white(char16_t c){return c==u' ' || (c>=u'\t' && c<=u'\r') || c==0xa0 || c==0x3000;}
unsigned digit_value(char16_t c){
    if(c>=u'0' && c<=u'9')return static_cast<unsigned>(c-u'0');
    if(c>=u'a' && c<=u'f')return static_cast<unsigned>(c-u'a')+10;
    if(c>=u'A' && c<=u'F')return static_cast<unsigned>(c-u'A')+10;
    return 99;
}
// NUMPARSE.dwInFlags 允許的語法：前後空白、前後正負號、括號負數、貨幣符號、&H／&O、小數點、千分位、指數（e／d）。
ParsedNumber parse_numprs(std::u16string_view s,std::uint32_t allowed,const NumberSymbols& symbols,std::size_t capacity){
    ParsedNumber out;std::size_t i=0;bool sign=false,parens=false,currency=false,closed=false;
    const auto at=[&](std::u16string_view token){return !token.empty() && s.substr(i,token.size())==token;};
    const auto money=(allowed&numprs_currency)!=0;
    const auto mismatch=[&]{out.status=0x80020005U;return out;};
    for(;i<s.size();){
        if(number_white(s[i]) && (allowed&numprs_leading_white)){out.flags|=numprs_leading_white;++i;continue;}
        if(!sign && !parens && s[i]==u'+' && (allowed&numprs_leading_plus)){sign=true;out.flags|=numprs_leading_plus;++i;continue;}
        if(!sign && !parens && s[i]==u'-' && (allowed&numprs_leading_minus)){sign=true;out.flags|=numprs_leading_minus|numprs_neg;++i;continue;}
        if(!sign && !parens && s[i]==u'(' && (allowed&numprs_parens)){parens=true;out.flags|=numprs_parens;++i;continue;}
        if(!currency && money && at(symbols.currency)){currency=true;out.flags|=numprs_currency;i+=symbols.currency.size();continue;}
        break;
    }
    std::vector<std::uint8_t> digits;long power=0;bool any=false;
    if((allowed&numprs_hex_oct) && i<s.size() && s[i]==u'&'){
        unsigned base=8;std::size_t j=i+1;
        if(j<s.size() && (s[j]==u'h' || s[j]==u'H')){base=16;++j;}else if(j<s.size() && (s[j]==u'o' || s[j]==u'O'))++j;
        for(;j<s.size();++j){const auto v=digit_value(s[j]);if(v>=base)break;any=true;if(!digits.empty() || v)digits.push_back(static_cast<std::uint8_t>(v));}
        if(!any)return mismatch();
        i=j;out.flags|=numprs_hex_oct;out.shift=base==16?4:3;if(digits.empty())digits.push_back(0);
        if(digits.size()>capacity){digits.resize(capacity);out.flags|=numprs_inexact;}
    }else{
        bool point=false;
        while(i<s.size()){
            const auto c=s[i];
            if(c>=u'0' && c<=u'9'){any=true;const auto v=static_cast<std::uint8_t>(c-u'0');if(!digits.empty() || v)digits.push_back(v);if(point)--power;++i;continue;}
            if(!point && (allowed&numprs_decimal)){
                const auto& separator=at(symbols.decimal)?symbols.decimal:money && at(symbols.money_decimal)?symbols.money_decimal:std::u16string{};
                if(!separator.empty()){point=true;out.flags|=numprs_decimal;i+=separator.size();continue;}
            }
            if(!point && any && (allowed&numprs_thousands)){
                const auto& separator=at(symbols.thousands)?symbols.thousands:money && at(symbols.money_thousands)?symbols.money_thousands:std::u16string{};
                // 千分位符號後面必須接數字，否則數字在此結束（例如列表中的「1,」）。
                if(!separator.empty() && i+separator.size()<s.size() && s[i+separator.size()]>=u'0' && s[i+separator.size()]<=u'9'){out.flags|=numprs_thousands;i+=separator.size();continue;}
            }
            break;
        }
        if(!any)return mismatch();
        if((allowed&numprs_exponent) && i<s.size() && (s[i]==u'e' || s[i]==u'E' || s[i]==u'd' || s[i]==u'D')){
            std::size_t j=i+1;bool minus=false;if(j<s.size() && (s[j]==u'+' || s[j]==u'-')){minus=s[j]==u'-';++j;}
            if(j<s.size() && s[j]>=u'0' && s[j]<=u'9'){
                long exponent=0;for(;j<s.size() && s[j]>=u'0' && s[j]<=u'9';++j)exponent=std::min(exponent*10+static_cast<long>(s[j]-u'0'),1000000L);
                power+=minus?-exponent:exponent;i=j;out.flags|=numprs_exponent;
            }
        }
        // 前導零不記錄；尾端零併入 nPwr10；零值為單一數字 0。
        if(digits.empty()){digits.push_back(0);power=0;}
        while(digits.size()>1 && digits.back()==0){digits.pop_back();++power;}
        if(digits.size()>capacity){
            power+=static_cast<long>(digits.size()-capacity);digits.resize(capacity);out.flags|=numprs_inexact;
            while(digits.size()>1 && digits.back()==0){digits.pop_back();++power;}
        }
    }
    while(i<s.size()){
        if(number_white(s[i]) && (allowed&numprs_trailing_white)){out.flags|=numprs_trailing_white;++i;continue;}
        if(!sign && !parens && s[i]==u'+' && (allowed&numprs_trailing_plus)){sign=true;out.flags|=numprs_trailing_plus;++i;continue;}
        if(!sign && !parens && s[i]==u'-' && (allowed&numprs_trailing_minus)){sign=true;out.flags|=numprs_trailing_minus|numprs_neg;++i;continue;}
        if(parens && !closed && s[i]==u')'){closed=true;out.flags|=numprs_neg;++i;continue;}
        if(!currency && money && at(symbols.currency)){currency=true;out.flags|=numprs_currency;i+=symbols.currency.size();continue;}
        break;
    }
    if(parens && !closed)return mismatch();
    if((allowed&numprs_use_all) && i<s.size())return mismatch();
    out.used=static_cast<std::int32_t>(i);out.power=static_cast<std::int32_t>(std::clamp(power,-1000000L,1000000L));out.digits=std::move(digits);return out;
}
std::string unsigned_text(unsigned __int128 value){
    std::string out;do{out.push_back(static_cast<char>('0'+static_cast<unsigned>(value%10)));value/=10;}while(value);
    std::reverse(out.begin(),out.end());return out;
}
}
#include "windows_oleaut32_varformat.inc"
std::optional<std::uint32_t> WindowsServices::invoke_oleaut32_format(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace automation;const auto name=service.symbol;const unsigned unit=x86_?4U:8U,variant_size=x86_?16U:24U;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(name=="VarTokenizeFormatString"){
        // (pstrFormat, rgbTok, cbTok, iFirstDay, iFirstWeek, lcid, pcbActual)：記號緩衝區＝簽章、iFirstDay、iFirstWeek、字元數、LCID 與 UTF-16 格式字串。
        const WindowsCall arg(state,memory,service);const auto done=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
        const auto first_day=static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(3))),first_week=static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(4)));
        if(!arg(0) || !arg(1) || first_day<0 || first_day>7 || first_week<0 || first_week>3)return done(invalid);
        try{
            const auto format=read_utf16(memory,GuestAddress{arg(0)},32767);const auto size=12U+static_cast<std::uint32_t>(format.size())*2U;
            if(arg(6))memory.store(GuestAddress{arg(6)},32,size);
            if(static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)))<static_cast<std::int32_t>(size))return done(0x80020013U); // DISP_E_BUFFERTOOSMALL
            std::vector<std::uint8_t> bytes(size);
            for(unsigned i=0;i<4;++i){bytes[i]=static_cast<std::uint8_t>(format_token_signature>>(8*i));bytes[8+i]=static_cast<std::uint8_t>(arg(5)>>(8*i));}
            bytes[4]=static_cast<std::uint8_t>(first_day);bytes[5]=static_cast<std::uint8_t>(first_week);
            bytes[6]=static_cast<std::uint8_t>(format.size());bytes[7]=static_cast<std::uint8_t>(format.size()>>8);
            for(std::size_t i=0;i<format.size();++i){bytes[12+i*2]=static_cast<std::uint8_t>(format[i]);bytes[13+i*2]=static_cast<std::uint8_t>(format[i]>>8);}
            memory.write(GuestAddress{arg(1)},bytes);return done(0);
        }catch(const MemoryFault&){return done(invalid);}
    }
    if(name=="GetAltMonthNames"){
        // (lcid, LPOLESTR** prgp)：阿拉伯文（回曆月名）、波蘭文與俄文（屬格月名）有替代月名，其餘語言為 NULL。
        // 陣列有 13 個指標（第 13 個為 NULL），存放在程序內常駐的客體記憶體。
        const WindowsCall arg(state,memory,service);const auto done=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
        if(!arg(1))return done(invalid);
        static constexpr std::array<std::array<std::u16string_view,12>,3> names{{
            {u"محرم",u"صفر",u"ربيع الاول",u"ربيع الثاني",u"جمادى الاولى",u"جمادى الثانية",u"رجب",u"شعبان",u"رمضان",u"شوال",u"ذو القعدة",u"ذو الحجة"},
            {u"stycznia",u"lutego",u"marca",u"kwietnia",u"maja",u"czerwca",u"lipca",u"sierpnia",u"września",u"października",u"listopada",u"grudnia"},
            {u"января",u"февраля",u"марта",u"апреля",u"мая",u"июня",u"июля",u"августа",u"сентября",u"октября",u"ноября",u"декабря"}}};
        const auto language=static_cast<std::uint32_t>(arg(0))&0x3ffU;const int which=language==0x01?0:language==0x15?1:language==0x19?2:-1;
        try{
            memory.check(GuestAddress{arg(1)},unit,Access::Write);
            if(which<0){memory.store(GuestAddress{arg(1)},bits,0);return done(0);}
            static std::map<const WindowsServices*,std::array<std::uint64_t,3>> tables;auto& table=tables[this][static_cast<std::size_t>(which)];
            if(!table){
                std::vector<std::uint8_t> strings;std::vector<std::size_t> offsets;
                for(const auto month:names[static_cast<std::size_t>(which)]){offsets.push_back(strings.size());const auto bytes=utf16_bytes(month);strings.insert(strings.end(),bytes.begin(),bytes.end());}
                const auto size=13*unit+strings.size();const auto block=allocate_heap(memory,size);if(!block)return done(nomemory);
                std::vector<std::uint8_t> bytes(size);
                for(std::size_t i=0;i<12;++i){const auto pointer=block+13*unit+offsets[i];for(unsigned b=0;b<unit;++b)bytes[i*unit+b]=static_cast<std::uint8_t>(pointer>>(8*b));}
                std::copy(strings.begin(),strings.end(),bytes.begin()+static_cast<std::ptrdiff_t>(13*unit));memory.write(GuestAddress{block},bytes);table=block;
            }
            memory.store(GuestAddress{arg(1)},bits,table);return done(0);
        }catch(const MemoryFault&){return done(invalid);}
    }
    if(name=="VarFormat" || name=="VarFormatFromTokens"){
        return nested_continuation(state,memory,"VarFormat",[this,service,variant_size,bits](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool tokens=service.symbol=="VarFormatFromTokens";const auto saved=last_error_;
            const auto finish=[&](std::uint32_t status){last_error_=saved;call.return_to(s,status);return std::optional<std::uint32_t>{};};
            // VarFormat(pVarIn, lpszFormat, iFirstDay, iFirstWeek, dwFlags, pbstrOut)；VarFormatFromTokens(pVarIn, pstrFormat, pbTokCur, dwFlags, pbstrOut, lcid)。
            const auto output=call(tokens?4:5);const auto flags=static_cast<std::uint32_t>(call(tokens?3:4));
            if(!call(0) || !output)co_return finish(invalid);
            std::u16string format;std::uint32_t lcid=0x400;int first_day=0,first_week=0;
            try{
                m.store(GuestAddress{output},bits,0);
                if(tokens){
                    if(!call(2) || m.load(GuestAddress{call(2)},32)!=format_token_signature)co_return finish(invalid);
                    first_day=static_cast<int>(m.load(GuestAddress{call(2)+4},8));first_week=static_cast<int>(m.load(GuestAddress{call(2)+5},8));
                    format=read_utf16_counted(m,GuestAddress{call(2)+12},static_cast<std::size_t>(m.load(GuestAddress{call(2)+6},16)));lcid=static_cast<std::uint32_t>(call(5));
                }else{
                    if(call(1))format=read_utf16(m,GuestAddress{call(1)},32767);
                    first_day=static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(call(2))));first_week=static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(call(3))));
                }
            }catch(const MemoryFault&){co_return finish(invalid);}
            if(first_day<0 || first_day>7 || first_week<0 || first_week>3)co_return finish(invalid);
            GuestScratch scratch(*this,m,s);constexpr unsigned capacity=512;
            const auto block=scratch.allocate(variant_size),buffer=scratch.allocate(capacity*2);if(!block || !buffer)co_return finish(nomemory);
            const auto convert=resolve("oleaut32.dll","VariantChangeTypeEx").value,release=resolve("oleaut32.dll","SysFreeString").value;
            const auto bstr_text=[&](std::uint64_t bstr){return bstr?read_utf16_counted(m,GuestAddress{bstr},static_cast<std::size_t>(m.load(GuestAddress{bstr-4},32)/2)):std::u16string{};};
            // 文字形式（字串格式與無法格式化時使用）：布林值顯示 True／False。
            FormatValue value;std::u16string general;bool have_text=false;
            auto vt=m.load(GuestAddress{call(0)},16);std::uint64_t variant=call(0);
            if(vt==(0x4000U|12U)){variant=m.load(GuestAddress{call(0)+8},bits);vt=variant?m.load(GuestAddress{variant},16):0;}
            const auto base=vt&0xfffU;
            if(base==1)value.kind=FormatValue::Kind::Null;
            else{
                if(!co_await service_callback::call(convert,{block,variant,lcid,2,8})){general=bstr_text(m.load(GuestAddress{block+8},bits));have_text=true;(void)co_await service_callback::call(release,{m.load(GuestAddress{block+8},bits)});m.store(GuestAddress{block},16,0);}
                value.text=general;
                if(base==0)value.kind=FormatValue::Kind::Empty;
                else{
                    auto status=co_await service_callback::call(convert,{block,variant,lcid,0,14});
                    if(status==overflow)status=co_await service_callback::call(convert,{block,variant,lcid,0,5});
                    if(!status){
                        Value number{};m.read(GuestAddress{block},std::span{number}.first(variant_size));
                        try{const auto decimal=plain_decimal(number_string(number),0);value.negative=decimal.starts_with('-');value.magnitude=value.negative?decimal.substr(1):decimal;value.kind=FormatValue::Kind::Number;}
                        catch(const std::exception&){value.kind=FormatValue::Kind::Text;}
                    }else value.kind=FormatValue::Kind::Text;
                }
                value.zero=value.magnitude=="0";
                if(base!=0 && !co_await service_callback::call(convert,{block,variant,lcid,0,7})){
                    const auto when=std::bit_cast<double>(m.load(GuestAddress{block+8},64));if(const auto fields=time_fields(when)){value.date=*fields;value.has_date=true;}
                }
            }
            if(format.empty()){
                // 沒有格式：與轉成字串相同；Null 為 NULL BSTR。
                if(value.kind==FormatValue::Kind::Null)co_return finish(0);
                if(!have_text)co_return finish(mismatch);
            }
            FormatLocale locale;std::u16string expanded;NamedFormat named=NamedFormat::None;
            try{
                const auto nested=[&](std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return static_cast<std::uint32_t>(nested_service(s,m,"kernel32.dll",symbol,arguments));};
                const auto info=[&](std::uint32_t type){const auto length=nested("GetLocaleInfoW",{lcid,type|(flags&0x80000000U),buffer,capacity});return length?read_utf16(m,GuestAddress{buffer},capacity):std::u16string{};};
                const auto number=[&](std::uint32_t type){unsigned v=0;for(const auto c:info(type)){if(c<u'0' || c>u'9')break;v=v*10+static_cast<unsigned>(c-u'0');}return v;};
                if(!format.empty())named=named_format(format,expanded);
                for(const auto [field,type]:{std::pair{&locale.decimal,0x0eU},{&locale.thousands,0x0fU},{&locale.date_separator,0x1dU},{&locale.time_separator,0x1eU},{&locale.am,0x28U},{&locale.pm,0x29U}})
                    if(auto text=info(type);!text.empty() || type==0x0fU)*field=std::move(text);
                // LOCALE_SDAYNAME1（星期一）起；陣列以星期日為 0。
                for(unsigned i=0;i<7;++i){locale.days[(i+1)%7]=info(0x2aU+i);locale.short_days[(i+1)%7]=info(0x31U+i);}
                for(unsigned i=0;i<12;++i){locale.months[i]=info(0x38U+i);locale.short_months[i]=info(0x44U+i);}
                locale.first_day=(number(0x100cU)+1)%7+1;const auto week=number(0x100dU);locale.first_week=week==1?3U:week==2?2U:1U;
                if(value.has_date){
                    const auto systemtime=scratch.allocate(16);if(!systemtime)co_return finish(nomemory);
                    for(unsigned i=0;i<8;++i)m.store(GuestAddress{systemtime+i*2},16,value.date[i]);
                    const auto date_text=[&](std::uint32_t style){const auto length=nested("GetDateFormatW",{lcid,style|(flags&0x80000000U),systemtime,0,buffer,capacity});return length?read_utf16(m,GuestAddress{buffer},capacity):std::u16string{};};
                    const auto time_text=[&]{const auto length=nested("GetTimeFormatW",{lcid,flags&0x80000000U,systemtime,0,buffer,capacity});return length?read_utf16(m,GuestAddress{buffer},capacity):std::u16string{};};
                    locale.short_date=date_text(1);locale.long_date=date_text(2);locale.long_time=time_text();
                    const bool whole_day=!value.date[4] && !value.date[5] && !value.date[6],no_date=value.date[0]==1899 && value.date[1]==12 && value.date[3]==30;
                    locale.general_date=no_date?locale.long_time:whole_day?locale.short_date:locale.short_date+u" "+locale.long_time;
                }
            }catch(const std::invalid_argument&){co_return finish(invalid);}catch(const std::bad_alloc&){co_return finish(nomemory);}
            const unsigned day=first_day?static_cast<unsigned>(first_day):locale.first_day,week=first_week?static_cast<unsigned>(first_week):locale.first_week;
            std::optional<std::u16string> text;
            if(format.empty())text=general;
            else switch(named){
            case NamedFormat::GeneralNumber:
                if(value.kind!=FormatValue::Kind::Null)text=general;
                break;
            case NamedFormat::Currency:
                if(value.kind==FormatValue::Kind::Text)text=value.text;
                else if(value.kind!=FormatValue::Kind::Null){
                    const auto status=co_await service_callback::call(resolve("oleaut32.dll","VarFormatCurrency").value,{variant,UINT32_MAX,UINT32_MAX-1,UINT32_MAX-1,UINT32_MAX-1,flags&0x80000000U,block});
                    if(status)co_return finish(status);
                    const auto bstr=m.load(GuestAddress{block},bits);text=bstr_text(bstr);(void)co_await service_callback::call(release,{bstr});
                }
                break;
            case NamedFormat::YesNo:case NamedFormat::TrueFalse:case NamedFormat::OnOff:
                if(value.kind==FormatValue::Kind::Text)text=value.text;
                else if(value.kind!=FormatValue::Kind::Null)
                    text=named==NamedFormat::YesNo?(value.zero?u"No":u"Yes"):named==NamedFormat::TrueFalse?(value.zero?u"False":u"True"):(value.zero?u"Off":u"On");
                break;
            case NamedFormat::GeneralDate:case NamedFormat::LongDate:case NamedFormat::ShortDate:case NamedFormat::LongTime:case NamedFormat::ShortTime:
                if(value.kind==FormatValue::Kind::Null)break;
                if(!value.has_date)text=general;
                else text=named==NamedFormat::GeneralDate?locale.general_date:named==NamedFormat::LongDate?locale.long_date:named==NamedFormat::ShortDate?locale.short_date:
                    named==NamedFormat::LongTime?locale.long_time:format_date_section(u"hh:nn",value,locale,day,week);
                break;
            default:text=apply_format(expanded,value,locale,day,week,general);
            }
            if(!text)co_return finish(0);
            auto data=utf16_bytes(*text);data.resize(data.size()-2);
            const auto string=allocate_bstr(m,data.size(),data);if(!string)co_return finish(nomemory);
            m.store(GuestAddress{output},bits,string);co_return finish(0);
        });
    }
    if(name.starts_with("VarFormat")){
        return nested_continuation(state,memory,"VarFormat",[this,service,unit,variant_size,bits](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto name=service.symbol;const bool date=name=="VarFormatDateTime";
            // 格式化經巢狀地區服務完成，失敗時不留下呼叫端看得到的 GetLastError 變化。
            const auto saved=last_error_;
            const auto finish=[&](std::uint32_t status){last_error_=saved;call.return_to(s,status);return std::optional<std::uint32_t>{};};
            const auto argument=[&](unsigned index){return static_cast<std::int32_t>(static_cast<std::uint32_t>(call(index)));};
            const auto output=call(date?3:6);
            if(!call(0) || !output)co_return finish(invalid);
            if(date?(argument(1)<0 || argument(1)>4):(argument(1)<-1 || argument(1)>9))co_return finish(invalid);
            m.store(GuestAddress{output},bits,0);
            GuestScratch scratch(*this,m,s);constexpr unsigned capacity=1024;
            const auto block=scratch.allocate(variant_size),buffer=scratch.allocate(capacity*2);if(!block || !buffer)co_return finish(nomemory);
            const auto convert=resolve("oleaut32.dll","VariantChangeTypeEx").value;
            auto status=co_await service_callback::call(convert,{block,call(0),0x400,0,date?7U:14U});
            // DECIMAL 放不下的值（例如 1E+300）改用 R8，有效位數與 VarBstrFromR8 相同。
            if(status==overflow && !date)status=co_await service_callback::call(convert,{block,call(0),0x400,0,5});
            if(status)co_return finish(status);
            Value value{};m.read(GuestAddress{block},std::span{value}.first(variant_size));
            const auto override_flag=static_cast<std::uint32_t>(call(date?2:5))&0x80000000U;
            std::u16string text;
            try{
                const auto nested=[&](std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return static_cast<std::uint32_t>(nested_service(s,m,"kernel32.dll",symbol,arguments));};
                const auto pointer=[&](std::u16string_view value){const auto address=scratch.text(value);if(!address)throw std::bad_alloc();return address;};
                const auto result=[&](std::uint32_t length){if(!length)throw std::invalid_argument("地區格式化失敗");return read_utf16(m,GuestAddress{buffer},capacity);};
                if(date){
                    const auto when=std::bit_cast<double>(get(value,8,8));const auto fields=time_fields(when);if(!fields)co_return finish(invalid);
                    const auto systemtime=scratch.allocate(16);if(!systemtime)co_return finish(nomemory);
                    for(unsigned i=0;i<8;++i)m.store(GuestAddress{systemtime+i*2},16,(*fields)[i]);
                    const auto date_text=[&](std::uint32_t style){return result(nested("GetDateFormatW",{0x400,style|override_flag,systemtime,0,buffer,capacity}));};
                    const auto time_text=[&](std::uint64_t format){return result(nested("GetTimeFormatW",{0x400,override_flag,systemtime,format,buffer,capacity}));};
                    // vbGeneralDate：沒有日期部分只顯示時間、沒有時間部分只顯示日期；vbLongDate、vbShortDate、vbLongTime、vbShortTime（24 小時 hh:mm）。
                    switch(argument(1)){
                    case 1:text=date_text(2);break;
                    case 2:text=date_text(1);break;
                    case 3:text=time_text(0);break;
                    case 4:text=time_text(pointer(u"HH:mm"));break;
                    default:{
                        const auto whole=std::trunc(when);
                        if(whole==0)text=time_text(0);
                        else {text=date_text(1);if(when!=whole)text+=u" "+time_text(0);}
                    }
                    }
                }else{
                    const bool currency=name=="VarFormatCurrency",percent=name=="VarFormatPercent";
                    const auto decimal=plain_decimal(number_string(value),percent?2:0);
                    const auto info=[&](std::uint32_t type){return result(nested("GetLocaleInfoW",{0x400,type|override_flag,buffer,capacity}));};
                    const auto number=[&](std::uint32_t type){unsigned v=0;for(const auto c:info(type)){if(c<u'0' || c>u'9')break;v=v*10+static_cast<unsigned>(c-u'0');}return v;};
                    const unsigned locale_digits=number(currency?0x19:0x11),locale_leading=number(0x12),locale_negative=number(currency?0x1c:0x1010);
                    unsigned groups=grouping_value(info(currency?0x18:0x10));
                    const auto separator=info(currency?0x16:0x0e),thousands=info(currency?0x17:0x0f);
                    // 三態參數：-2 使用地區設定、0 關閉、其他（-1）開啟。
                    const auto tristate=[&](unsigned index,unsigned fallback){const auto v=argument(index);return v==-2?fallback:v?1U:0U;};
                    const unsigned digits=argument(1)<0?locale_digits:static_cast<unsigned>(argument(1)),leading=tristate(2,locale_leading);
                    if(argument(4)==0)groups=0;else if(argument(4)!=-2 && !groups)groups=3;
                    // 括號負數：開啟時改用同符號位置的括號格式，關閉時改用同位置的負號格式；未指定沿用地區 INEGNUMBER／INEGCURR。
                    unsigned negative=locale_negative;
                    if(argument(3)!=-2){
                        static constexpr std::array<unsigned,16> with{0,0,0,0,4,4,4,4,15,14,15,14,14,15,14,15},without{1,1,2,3,5,5,6,7,8,9,10,11,12,13,9,8};
                        if(currency)negative=(argument(3)?with:without)[std::min(negative,15U)];
                        else negative=argument(3)?0U:negative?negative:1U;
                    }
                    const unsigned offset=x86_?12U:16U,order=offset+unit*2;
                    const auto format=scratch.allocate(currency?(x86_?32U:48U):(x86_?24U:40U));if(!format)throw std::bad_alloc();
                    m.store(GuestAddress{format},32,digits);m.store(GuestAddress{format+4},32,leading);m.store(GuestAddress{format+8},32,groups);
                    m.store(GuestAddress{format+offset},bits,pointer(separator));m.store(GuestAddress{format+offset+unit},bits,pointer(thousands));
                    m.store(GuestAddress{format+order},32,negative);
                    if(currency){m.store(GuestAddress{format+order+4},32,number(0x1b));m.store(GuestAddress{format+order+8},bits,pointer(info(0x14)));}
                    const auto input=pointer(std::u16string(decimal.begin(),decimal.end()));
                    text=result(nested(currency?"GetCurrencyFormatW":"GetNumberFormatW",{0x400,0,input,format,buffer,capacity}));
                    // 百分比符號緊接在最後一個數字之後，括號與尾端負號保留在外側：(50.00%)、50.00%-。
                    if(percent){const auto last=text.find_last_of(u"0123456789");text.insert(last==std::u16string::npos?text.size():last+1,u"%");}
                }
            }catch(const std::invalid_argument&){co_return finish(invalid);}catch(const std::range_error&){co_return finish(overflow);}catch(const std::bad_alloc&){co_return finish(nomemory);}
            auto data=utf16_bytes(text);data.resize(data.size()-2);
            const auto string=allocate_bstr(m,data.size(),data);if(!string)co_return finish(nomemory);
            m.store(GuestAddress{output},bits,string);co_return finish(0);
        });
    }
    const WindowsCall arg(state,memory,service);
    const auto done=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    try{
        if(name=="VarParseNumFromStr"){
            const auto numprs=arg(3),output=arg(4);if(!numprs || !output)return done(invalid);
            const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{numprs},32));const auto allowed=static_cast<std::uint32_t>(memory.load(GuestAddress{numprs+4},32));
            if(count<=0)return done(invalid);
            const auto lcid=static_cast<std::uint32_t>(arg(1)),override_flag=static_cast<std::uint32_t>(arg(2))&0x80000000U;
            GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(64*2);if(!buffer)return done(nomemory);
            const auto saved=last_error_;std::optional<NumberSymbols> symbols;
            {
                const auto info=[&](std::uint32_t type)->std::optional<std::u16string>{
                    if(!nested_service(state,memory,"kernel32.dll","GetLocaleInfoW",{lcid,type|override_flag,buffer,64}))return std::nullopt;
                    return read_utf16(memory,GuestAddress{buffer},64);};
                const auto decimal=info(0x0e),thousands=info(0x0f),currency=info(0x14),money_decimal=info(0x16),money_thousands=info(0x17);
                if(decimal && thousands && currency && money_decimal && money_thousands)symbols=NumberSymbols{*decimal,*thousands,*currency,*money_decimal,*money_thousands};
            }
            last_error_=saved;if(!symbols)return done(invalid);
            // NULL 字串等同空 BSTR：沒有數字。
            const auto text=arg(0)?read_utf16(memory,GuestAddress{arg(0)}):std::u16string{};
            const auto parsed=parse_numprs(text,allowed,*symbols,static_cast<std::size_t>(count));
            if(parsed.status)return done(parsed.status);
            memory.write(GuestAddress{output},parsed.digits);
            memory.store(GuestAddress{numprs},32,static_cast<std::uint32_t>(parsed.digits.size()));memory.store(GuestAddress{numprs+8},32,parsed.flags);
            memory.store(GuestAddress{numprs+12},32,static_cast<std::uint32_t>(parsed.used));memory.store(GuestAddress{numprs+16},32,static_cast<std::uint32_t>(parsed.shift));
            memory.store(GuestAddress{numprs+20},32,static_cast<std::uint32_t>(parsed.power));
            return done(0);
        }
        if(name=="VarNumFromParseNum"){
            const auto numprs=arg(0),input=arg(1),variant=arg(3);const auto allowed=static_cast<std::uint32_t>(arg(2));
            if(!numprs || !input || !variant)return done(invalid);
            const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{numprs},32));const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{numprs+8},32));
            const auto shift=static_cast<std::int32_t>(memory.load(GuestAddress{numprs+16},32)),power=static_cast<std::int32_t>(memory.load(GuestAddress{numprs+20},32));
            // VTBIT_*＝1 << VT_*：整數由小到大（I1、UI1、I2、UI2、I4、UI4、I8、UI8），再來 R4、R8、CY、DECIMAL。
            static constexpr std::array<std::pair<std::uint16_t,unsigned>,8> integers{{{16,8},{17,8},{2,16},{18,16},{3,32},{19,32},{20,64},{21,64}}};
            static constexpr std::array<std::uint16_t,4> reals{4,5,6,14};
            std::uint32_t known=0;for(const auto& [type,width]:integers)known|=1U<<type;for(const auto type:reals)known|=1U<<type;
            if(count<0 || count>65536 || shift<0 || shift>4 || !(allowed&known))return done(invalid);
            std::vector<std::uint8_t> digits(static_cast<std::size_t>(count));if(count)memory.read(GuestAddress{input},digits);
            const unsigned base=shift?1U<<shift:10U;
            if(std::any_of(digits.begin(),digits.end(),[&](std::uint8_t d){return d>=base;}))return done(invalid);
            const bool negative=(flags&numprs_neg)!=0;
            const auto publish=[&](const Value& v){
                std::vector<std::uint8_t> bytes(variant_size,0);std::copy_n(v.begin(),std::min<std::size_t>(variant_size,v.size()),bytes.begin());
                memory.write(GuestAddress{variant},bytes);return done(0);};
            const auto try_convert=[&](std::uint16_t type,const std::string& text)->bool{
                Value source{};put(source,0,2,8);Value v{};if(number_convert(source,text,type,v))return false;(void)publish(v);return true;};
            std::string text;std::size_t significant=0;
            if(shift){
                // &H／&O：位元樣式依可容納的最小整數型別解讀（&HFFFF 為 I2 的 -1），再套用正負號。
                if(flags&numprs_inexact)return done(overflow);
                unsigned __int128 value=0;
                for(const auto d:digits){if(value>>(128-static_cast<unsigned>(shift)))return done(overflow);value=(value<<static_cast<unsigned>(shift))|d;}
                for(const auto& [type,width]:integers){
                    if(!(allowed&(1U<<type)) || (value>>width))continue;
                    const bool is_signed=type==16 || type==2 || type==3 || type==20;
                    auto result=static_cast<__int128>(value);
                    if(is_signed && ((value>>(width-1))&1U))result-=static_cast<__int128>(1)<<width;
                    if(negative)result=-result;
                    const auto low=is_signed?-(static_cast<__int128>(1)<<(width-1)):static_cast<__int128>(0);
                    const auto high=is_signed?(static_cast<__int128>(1)<<(width-1))-1:(static_cast<__int128>(1)<<width)-1;
                    if(result<low || result>high)continue;
                    Value v{};put(v,0,2,type);put(v,8,width/8,static_cast<std::uint64_t>(result));return publish(v);
                }
                text=unsigned_text(value);significant=text.size();if(negative && value)text.insert(0,1,'-');
            }else{
                std::size_t leading=0,trailing=0;
                while(leading<digits.size() && !digits[leading])++leading;
                while(trailing<digits.size()-leading && !digits[digits.size()-1-trailing])++trailing;
                const bool zero=leading==digits.size();significant=zero?1:digits.size()-leading-trailing;
                if(zero)text="0";
                else {
                    if(negative)text.push_back('-');
                    for(std::size_t i=leading;i<digits.size();++i)text.push_back(static_cast<char>('0'+digits[i]));
                    if(power>200000 || power<-200000)return done(overflow);
                    text+="e"+std::to_string(power);
                }
                // 整數值（或只允許整數型別）先試整數，小數依 ties-to-even 捨入。
                const bool integral=zero || static_cast<long>(power)+static_cast<long>(trailing)>=0;
                const bool real=(allowed&((1U<<4)|(1U<<5)|(1U<<6)|(1U<<14)))!=0;
                if(integral || !real)for(const auto& [type,width]:integers)if((allowed&(1U<<type)) && try_convert(type,text))return std::nullopt;
            }
            for(const auto type:reals){
                if(!(allowed&(1U<<type)))continue;
                // 同時允許 R4 與 R8 時，超過單精度有效位數（7 位）改用 R8。
                if(type==4 && (allowed&(1U<<5)) && significant>7)continue;
                if(try_convert(type,text))return std::nullopt;
            }
            return done(overflow);
        }
    }catch(const std::bad_alloc&){return done(nomemory);}
    return done(invalid);
}
