// GetLocaleInfo A／W／Ex 共用資料；語言、地區與完整名稱是不同的 NLS 欄位。
struct LocaleInfo {std::u16string text;std::optional<std::uint32_t> number;};
LocaleInfo locale_info_number(std::uint32_t value,bool hex=false){
    char text[16]{};std::snprintf(text,sizeof text,hex?"%04x":"%u",value);
    return {utf8_to_utf16(text),value};
}
template<class F>std::u16string locale_info_text(F call){
    UErrorCode error=U_ZERO_ERROR;const auto count=call(nullptr,0,&error);
    if(count<0 || (U_FAILURE(error) && error!=U_BUFFER_OVERFLOW_ERROR))throw std::runtime_error("ICU 語系資料查詢失敗");
    error=U_ZERO_ERROR;std::u16string text(static_cast<std::size_t>(count),0);call(text.data(),count,&error);
    if(U_FAILURE(error))throw std::runtime_error("ICU 語系資料讀取失敗");return text;
}
std::u16string windows_info_pattern(std::u16string_view pattern){
    auto tokens=date_tokens(pattern);for(auto& token:tokens){
        if(token.field==u'E' || token.field==u'c')token.text.assign(token.text.size()>=4?4:3,u'd');
        else if(token.field==u'a')token.text=u"tt";
        else if(token.field==u'L')token.text.assign(token.text.size(),u'M');
        else if(token.field==u'G')token.text=u"gg";
    }return render_pattern(tokens);
}
// LOCALE_SABBREVLANGNAME 是 Windows 舊有的三字母縮寫（ISO 639-1 加子語言字母），無法由 ICU 推導。
// 表外地區沿用同語言的預設子語言；連語言都不在表內時回退 ISO 639-2 三字母代碼（新版 Windows 對無舊縮寫的語系同此處理）。
std::u16string abbreviated_language(const Locale& locale,const std::string& id){
    static constexpr std::pair<std::uint16_t,const char*> table[]{
        {0x0401,"ARA"},{0x0402,"BGR"},{0x0403,"CAT"},{0x0404,"CHT"},{0x0405,"CSY"},{0x0406,"DAN"},{0x0407,"DEU"},{0x0408,"ELL"},{0x0409,"ENU"},{0x040a,"ESP"},
        {0x040b,"FIN"},{0x040c,"FRA"},{0x040d,"HEB"},{0x040e,"HUN"},{0x040f,"ISL"},{0x0410,"ITA"},{0x0411,"JPN"},{0x0412,"KOR"},{0x0413,"NLD"},{0x0414,"NOR"},
        {0x0415,"PLK"},{0x0416,"PTB"},{0x0417,"RMC"},{0x0418,"ROM"},{0x0419,"RUS"},{0x041a,"HRV"},{0x041b,"SKY"},{0x041c,"SQI"},{0x041d,"SVE"},{0x041e,"THA"},
        {0x041f,"TRK"},{0x0420,"URD"},{0x0421,"IND"},{0x0422,"UKR"},{0x0423,"BEL"},{0x0424,"SLV"},{0x0425,"ETI"},{0x0426,"LVI"},{0x0427,"LTH"},{0x0429,"FAR"},
        {0x042a,"VIT"},{0x042b,"HYE"},{0x042c,"AZE"},{0x042d,"EUQ"},{0x042f,"MKI"},{0x0436,"AFK"},{0x0437,"KAT"},{0x0438,"FOS"},{0x0439,"HIN"},{0x043e,"MSL"},
        {0x043f,"KKZ"},{0x0440,"KYR"},{0x0441,"SWK"},{0x0443,"UZB"},{0x0444,"TTT"},{0x0445,"BNG"},{0x0446,"PAN"},{0x0447,"GUJ"},{0x0449,"TAI"},{0x044a,"TEL"},
        {0x044b,"KDI"},{0x044e,"MAR"},{0x044f,"SAN"},{0x0450,"MNN"},{0x0452,"CYM"},{0x0456,"GLC"},{0x0457,"KNK"},{0x045a,"SYR"},{0x0464,"FPO"},{0x0465,"DIV"},
        {0x0801,"ARI"},{0x0804,"CHS"},{0x0807,"DES"},{0x0809,"ENG"},{0x080a,"ESM"},{0x080c,"FRB"},{0x0810,"ITS"},{0x0813,"NLB"},{0x0814,"NON"},{0x0816,"PTG"},
        {0x081a,"SRL"},{0x081d,"SVF"},{0x083e,"MSB"},{0x0c01,"ARE"},{0x0c04,"ZHH"},{0x0c07,"DEA"},{0x0c09,"ENA"},{0x0c0a,"ESN"},{0x0c0c,"FRC"},{0x0c1a,"SRB"},
        {0x1001,"ARL"},{0x1004,"ZHI"},{0x1007,"DEL"},{0x1009,"ENC"},{0x100a,"ESG"},{0x100c,"FRS"},{0x1401,"ARG"},{0x1404,"ZHM"},{0x1407,"DEC"},{0x1409,"ENZ"},
        {0x140a,"ESC"},{0x140c,"FRL"},{0x1801,"ARM"},{0x1809,"ENI"},{0x180a,"ESA"},{0x180c,"FRM"},{0x1c01,"ART"},{0x1c09,"ENS"},{0x1c0a,"ESD"},{0x2001,"ARO"},
        {0x2009,"ENJ"},{0x200a,"ESV"},{0x2401,"ARY"},{0x2409,"ENB"},{0x240a,"ESO"},{0x241a,"SRM"},{0x2801,"ARS"},{0x2809,"ENL"},{0x280a,"ESR"},{0x281a,"SRO"},
        {0x2c01,"ARJ"},{0x2c09,"ENT"},{0x2c0a,"ESS"},{0x3001,"ARB"},{0x3009,"ENW"},{0x300a,"ESF"},{0x3401,"ARK"},{0x3409,"ENP"},{0x340a,"ESL"},{0x3801,"ARU"},
        {0x380a,"ESY"},{0x3c01,"ARH"},{0x3c0a,"ESZ"},{0x4001,"ARQ"},{0x4009,"ENN"},{0x400a,"ESB"},{0x4409,"ENM"},{0x440a,"ESE"},{0x4809,"ENE"},{0x480a,"ESH"},
        {0x4c0a,"ESI"},{0x500a,"ESU"},{0x540a,"EST"}};
    const auto find=[&](std::uint32_t lcid)->const char*{for(const auto& [code,name]:table)if(code==lcid)return name;return nullptr;};
    if(locale.name.empty())return u"IVL";
    // 中文中性名稱依字體區分；zh 與 zh-Hans 對應簡體預設，zh-Hant 對應繁體預設。
    if(locale.name==u"zh" || locale.name==u"zh-Hans")return u"CHS";if(locale.name==u"zh-Hant")return u"CHT";
    auto found=locale.id<0x1000?find(locale.id):nullptr;
    if(!found && locale.id && locale.id<0x1000)found=find((locale.id&0x3ffU)|0x400U);
    if(!found){char language[ULOC_LANG_CAPACITY]{};UErrorCode error=U_ZERO_ERROR;uloc_getLanguage(id.c_str(),language,sizeof language,&error);
        if(U_SUCCESS(error))if(const auto lcid=uloc_getLCID(language);lcid && lcid<0x1000)found=find((lcid&0x3ffU)|0x400U);}
    if(found)return utf8_to_utf16(found);
    auto result=utf8_to_utf16(uloc_getISO3Language(id.c_str()));for(auto& c:result)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);return result;
}
// LOCALE_IPOSITIVEPERCENT／INEGATIVEPERCENT 的 Windows 樣式表；ICU 百分比前後綴正規化為 n、%、- 與空白後比對。
constexpr std::u16string_view positive_percent_patterns[]{u"n %",u"n%",u"%n",u"% n"};
constexpr std::u16string_view negative_percent_patterns[]{u"-n %",u"-n%",u"-%n",u"%-n",u"%n-",u"n-%",u"n%-",u"-% n",u"n %-",u"% n-",u"% -n",u"n- %"};
std::optional<LocaleInfo> locale_information(const Locale& locale,std::uint32_t type,bool genitive){
    const auto id=icu_locale(locale);const bool invariant=locale.name.empty();
    const auto text=[](std::u16string value){return LocaleInfo{std::move(value),{}};};
    const auto part=[&](auto function){char value[ULOC_FULLNAME_CAPACITY]{};UErrorCode error=U_ZERO_ERROR;
        function(id.c_str(),value,sizeof value,&error);if(U_FAILURE(error))throw std::runtime_error("ICU 語系名稱拆解失敗");return utf8_to_utf16(value);};
    switch(type){
    case 1:case 9:return locale_info_number(locale.id,true);
    case 0x59:return text(invariant?u"iv":part(uloc_getLanguage));
    case 0x5a:return text(invariant?u"IV":part(uloc_getCountry));
    case 0x67:return text(invariant?u"ivl":utf8_to_utf16(uloc_getISO3Language(id.c_str())));
    case 3:return text(abbreviated_language(locale,id));
    // 新版 Windows 的 LOCALE_SABBREVCTRYNAME 與 LOCALE_SISO3166CTRYNAME2 同為 ISO 3166 三字母代碼。
    case 7:case 0x68:return text(invariant?u"IVC":utf8_to_utf16(uloc_getISO3Country(id.c_str())));
    case 0x5c:case 0x6e:case 0x7b:return text(locale.name);
    case 0x6d:{auto parent=locale.name;const auto at=parent.find_last_of(u'-');parent.resize(at==parent.npos?0:at);return text(std::move(parent));}
    case 0x71:return locale_info_number(locale.neutral?1:0);
    case 0x70:return locale_info_number(uloc_isRightToLeft(id.c_str())?1:0);
    case 0x6c:{UScriptCode scripts[32]{};UErrorCode error=U_ZERO_ERROR;const auto count=uscript_getCode(id.c_str(),scripts,32,&error);
        if(U_FAILURE(error))throw std::runtime_error("ICU 語系字體查詢失敗");std::u16string result;
        for(int i=0;i<count;++i){result+=utf8_to_utf16(uscript_getShortName(scripts[i]));result+=u';';}return text(std::move(result));}
    case 2:case 0x72:case 0x73:case 4:case 0x6f:case 0x1001:case 6:case 8:case 0x1002:{
        const bool native=type==0x73 || type==4 || type==8;const char* display=native?id.c_str():"en_US";
        const auto function=type==2 || type==0x72 || type==0x73?uloc_getDisplayName:
            type==6 || type==8 || type==0x1002?uloc_getDisplayCountry:uloc_getDisplayLanguage;
        return text(locale_info_text([&](UChar* buffer,int count,UErrorCode* error){return function(id.c_str(),display,buffer,count,error);}));}
    // 虛擬 Windows 的 ANSI／OEM 預設為 UTF-8，與 GetACP／GetOEMCP 一致。
    case 0xb:case 0x1004:return locale_info_number(65001);
    default:break;
    }
    UErrorCode error=U_ZERO_ERROR;
    NumberFormat decimal(unum_open(UNUM_DECIMAL,nullptr,0,id.c_str(),nullptr,&error),unum_close);
    NumberFormat currency(unum_open(UNUM_CURRENCY,nullptr,0,id.c_str(),nullptr,&error),unum_close);
    if(U_FAILURE(error) || !decimal || !currency)throw std::runtime_error("ICU 數值語系建立失敗");
    const auto grouping=[&](UNumberFormat* format){
        if(!unum_getAttribute(format,UNUM_GROUPING_USED))return std::u16string(u"0");
        const auto primary=unum_getAttribute(format,UNUM_GROUPING_SIZE),secondary=unum_getAttribute(format,UNUM_SECONDARY_GROUPING_SIZE);
        if(primary<=0)return std::u16string(u"0");auto result=utf8_to_utf16(std::to_string(primary));
        if(secondary>0 && secondary!=primary)result+=u";"+utf8_to_utf16(std::to_string(secondary));return result+u";0";};
    const auto affix=[&](UNumberFormatTextAttribute attr){return locale_info_text([&](UChar* buffer,int count,UErrorCode* status){return unum_getTextAttribute(currency.get(),attr,buffer,count,status);});};
    switch(type){
    case 0xc:return text(number_symbol(decimal.get(),UNUM_DECIMAL_SEPARATOR_SYMBOL)==u","?u";":u",");
    case 0xd:{error=U_ZERO_ERROR;return locale_info_number(ulocdata_getMeasurementSystem(id.c_str(),&error)==UMS_US?1:0);}
    case 0xe:return text(number_symbol(decimal.get(),UNUM_DECIMAL_SEPARATOR_SYMBOL));
    case 0xf:return text(number_symbol(decimal.get(),UNUM_GROUPING_SEPARATOR_SYMBOL));
    case 0x10:return text(grouping(decimal.get()));
    case 0x11:return locale_info_number(2);
    case 0x12:return locale_info_number(static_cast<std::uint32_t>(unum_getAttribute(decimal.get(),UNUM_MIN_INTEGER_DIGITS)>0));
    case 0x13:{std::u16string result;for(unsigned i=0;i<10;++i)result+=number_text(decimal.get(),std::to_string(i));return text(std::move(result));}
    case 0x1014:return locale_info_number((locale.id&0x3ffU)==1 || (locale.id&0x3ffU)==0x29?0U:1U);
    case 0x14:return text(number_symbol(currency.get(),UNUM_CURRENCY_SYMBOL));
    case 0x15:return text(number_symbol(currency.get(),UNUM_INTL_CURRENCY_SYMBOL));
    case 0x16:return text(number_symbol(currency.get(),UNUM_MONETARY_SEPARATOR_SYMBOL));
    case 0x17:return text(number_symbol(currency.get(),UNUM_MONETARY_GROUPING_SEPARATOR_SYMBOL));
    case 0x18:return text(grouping(currency.get()));
    case 0x19:case 0x1a:return locale_info_number(static_cast<std::uint32_t>(unum_getAttribute(currency.get(),UNUM_MAX_FRACTION_DIGITS)));
    case 0x50:return text(u""); // 正數不帶正號，符合 Windows 的預設數值格式。
    case 0x51:return text(number_symbol(decimal.get(),UNUM_MINUS_SIGN_SYMBOL));
    case 0x69:return text(number_symbol(decimal.get(),UNUM_NAN_SYMBOL));
    case 0x6a:return text(number_symbol(decimal.get(),UNUM_INFINITY_SYMBOL));
    case 0x6b:return text(number_symbol(decimal.get(),UNUM_MINUS_SIGN_SYMBOL)+number_symbol(decimal.get(),UNUM_INFINITY_SYMBOL));
    case 0x76:return text(number_symbol(decimal.get(),UNUM_PERCENT_SYMBOL));
    case 0x77:return text(number_symbol(decimal.get(),UNUM_PERMILL_SYMBOL));
    case 0x74:case 0x75:{
        error=U_ZERO_ERROR;NumberFormat percent(unum_open(UNUM_PERCENT,nullptr,0,id.c_str(),nullptr,&error),unum_close);
        if(U_FAILURE(error) || !percent)throw std::runtime_error("ICU 百分比語系建立失敗");
        const bool negative=type==0x74;const auto sign=number_symbol(percent.get(),UNUM_MINUS_SIGN_SYMBOL),mark=number_symbol(percent.get(),UNUM_PERCENT_SYMBOL);
        const auto canonical=[&](UNumberFormatTextAttribute attr){
            const auto value=locale_info_text([&](UChar* buffer,int count,UErrorCode* status){return unum_getTextAttribute(percent.get(),attr,buffer,count,status);});
            std::u16string out;for(std::size_t i=0;i<value.size();){
                if(!sign.empty() && value.compare(i,sign.size(),sign)==0){out+=u'-';i+=sign.size();}
                else if(!mark.empty() && value.compare(i,mark.size(),mark)==0){out+=u'%';i+=mark.size();}
                else{if(u_isUWhiteSpace(value[i]) && (out.empty() || out.back()!=u' '))out+=u' ';++i;}
            }return out;};
        const auto shape=canonical(negative?UNUM_NEGATIVE_PREFIX:UNUM_POSITIVE_PREFIX)+u"n"+canonical(negative?UNUM_NEGATIVE_SUFFIX:UNUM_POSITIVE_SUFFIX);
        const std::span<const std::u16string_view> patterns=negative?std::span<const std::u16string_view>(negative_percent_patterns):std::span<const std::u16string_view>(positive_percent_patterns);
        for(std::size_t i=0;i<patterns.size();++i)if(patterns[i]==shape)return locale_info_number(static_cast<std::uint32_t>(i));
        return locale_info_number(1);}
    case 0x1007:case 0x1008:{
        // 貨幣全名：英文名稱固定以 en 查詢，本地名稱以語系本身查詢。
        error=U_ZERO_ERROR;UChar code[4]{};ucurr_forLocale(id.c_str(),code,4,&error);if(U_FAILURE(error) || !code[0])return {};
        UBool choice=false;int32_t length=0;error=U_ZERO_ERROR;
        const auto* name=ucurr_getName(code,type==0x1007?"en":id.c_str(),UCURR_LONG_NAME,&choice,&length,&error);
        if(U_FAILURE(error) || !name)return {};return text(std::u16string(reinterpret_cast<const char16_t*>(name),static_cast<std::size_t>(length)));}
    case 0x100a:{error=U_ZERO_ERROR;int32_t height=0,width=0;ulocdata_getPaperSize(id.c_str(),&height,&width,&error);
        if(U_FAILURE(error))return {};return locale_info_number(height==279 && width==216?1:height==356 && width==216?5:height==420 && width==297?8:9);}
    default:break;
    }
    if(type>=0x52 && type<=0x57){
        const bool negative=type==0x53 || type>=0x56;
        const auto prefix=affix(negative?UNUM_NEGATIVE_PREFIX:UNUM_POSITIVE_PREFIX),suffix=affix(negative?UNUM_NEGATIVE_SUFFIX:UNUM_POSITIVE_SUFFIX);
        const auto symbol=number_symbol(currency.get(),UNUM_CURRENCY_SYMBOL),sign=number_symbol(currency.get(),UNUM_MINUS_SIGN_SYMBOL);
        const auto before=prefix.find(symbol),after=suffix.find(symbol);
        if(type==0x54 || type==0x56)return locale_info_number(before!=prefix.npos?1:0);
        if(type==0x55 || type==0x57){bool separated=false;for(auto c:prefix+suffix)separated|=u_isUWhiteSpace(c);return locale_info_number(separated?1:0);}
        if(prefix.find(u'(')!=prefix.npos && suffix.find(u')')!=suffix.npos)return locale_info_number(0);
        const auto sign_before=prefix.find(sign),sign_after=suffix.find(sign);
        if(sign_before!=prefix.npos)return locale_info_number(before!=prefix.npos && sign_before>before?4:1);
        if(sign_after!=suffix.npos)return locale_info_number(after!=suffix.npos && sign_after<after?3:2);
        return locale_info_number(1);
    }
    error=U_ZERO_ERROR;DateFormat date(udat_open(UDAT_MEDIUM,UDAT_SHORT,id.c_str(),u"UTC",3,nullptr,0,&error),udat_close);
    if(U_FAILURE(error) || !date)throw std::runtime_error("ICU 日期語系建立失敗");
    UDateFormatSymbolType symbol=UDAT_ERAS;int index=-1;
    if(type>=0x2a && type<=0x30){symbol=UDAT_WEEKDAYS;index=static_cast<int>((type-0x2a+1)%7+1);}
    else if(type>=0x31 && type<=0x37){symbol=UDAT_SHORT_WEEKDAYS;index=static_cast<int>((type-0x31+1)%7+1);}
    else if(type>=0x60 && type<=0x66){symbol=UDAT_NARROW_WEEKDAYS;index=static_cast<int>((type-0x60+1)%7+1);}
    else if(type>=0x38 && type<=0x43){symbol=genitive?UDAT_MONTHS:UDAT_STANDALONE_MONTHS;index=static_cast<int>(type-0x38);}
    else if(type>=0x44 && type<=0x4f){symbol=genitive?UDAT_SHORT_MONTHS:UDAT_STANDALONE_SHORT_MONTHS;index=static_cast<int>(type-0x44);}
    else if(type==0x100e || type==0x100f){symbol=type==0x100e?UDAT_MONTHS:UDAT_SHORT_MONTHS;index=12;}
    else if(type==0x28 || type==0x29){symbol=UDAT_AM_PMS;index=static_cast<int>(type-0x28);}
    if(index>=0){if(index>=udat_countSymbols(date.get(),symbol))return text(u"");
        return text(locale_info_text([&](UChar* buffer,int count,UErrorCode* status){return udat_getSymbols(date.get(),symbol,index,buffer,count,status);}));}
    if(type==0x1009){const auto* calendar=ucal_getType(udat_getCalendar(date.get()),&error);
        if(!calendar || U_FAILURE(error))return {};const std::string_view name(calendar);
        if(name=="gregorian")return locale_info_number(1);if(name=="japanese")return locale_info_number(3);
        if(name=="roc")return locale_info_number(4);if(name=="islamic")return locale_info_number(6);
        if(name=="buddhist")return locale_info_number(7);if(name=="hebrew")return locale_info_number(8);
        if(name=="persian")return locale_info_number(22);if(name=="islamic-umalqura")return locale_info_number(23);return {};
    }
    if(type==0x100c)return locale_info_number(static_cast<std::uint32_t>((ucal_getAttribute(udat_getCalendar(date.get()),UCAL_FIRST_DAY_OF_WEEK)+5)%7));
    if(type==0x100d){const auto days=ucal_getAttribute(udat_getCalendar(date.get()),UCAL_MINIMAL_DAYS_IN_FIRST_WEEK);return locale_info_number(days==1?0:days==7?1:2);}
    if(type==0x100b)return locale_information(locale,0x1009,false);
    if(type==0x1006 || type==0x78){
        error=U_ZERO_ERROR;std::unique_ptr<UDateTimePatternGenerator,decltype(&udatpg_close)> generator(udatpg_open(id.c_str(),&error),udatpg_close);
        if(U_FAILURE(error) || !generator)return {};const auto skeleton=type==0x1006?u"yMMMM":u"MMMMd";
        const auto pattern=locale_info_text([&](UChar* buffer,int count,UErrorCode* status){return udatpg_getBestPattern(generator.get(),skeleton,5,buffer,count,status);});
        // Windows 年月樣式以 yyyy 表示完整年份；ICU 的 y 代表不補零的完整年份。
        auto tokens=date_tokens(windows_info_pattern(pattern));for(auto& token:tokens)if(token.field==u'y' && token.text.size()!=2)token.text=u"yyyy";
        return text(render_pattern(tokens));
    }
    if(type==0x1d || type==0x1e || (type>=0x21 && type<=0x27) || type==0x1005){
        // 舊式分隔字元、順序與補零欄位由目前的短日期、長日期與時間樣式推導，兩者保持一致。
        const bool time=type==0x1e || type==0x23 || type==0x25 || type==0x1005;
        const auto source=locale_information(locale,type==0x22?0x20:time?0x1003:0x1f,false);if(!source)return {};
        const auto tokens=date_tokens(source->text);
        const auto date_field=[](const DateToken& token){return token.field==u'M' || token.field==u'y' || (token.field==u'd' && token.text.size()<=2);};
        const auto hour=[](const DateToken& token){return token.field==u'h' || token.field==u'H';};
        if(type==0x1d || type==0x1e){
            for(std::size_t i=1;i+1<tokens.size();++i)if(!tokens[i].field && (type==0x1d?date_field(tokens[i-1]) && date_field(tokens[i+1]):hour(tokens[i-1]) && tokens[i+1].field==u'm'))return text(tokens[i].text);
            return text(type==0x1d?u"/":u":");
        }
        if(type==0x21 || type==0x22){for(const auto& token:tokens)if(date_field(token))return locale_info_number(token.field==u'M'?0:token.field==u'd'?1:2);return locale_info_number(0);}
        if(type==0x23)return locale_info_number(std::any_of(tokens.begin(),tokens.end(),[](const DateToken& token){return token.field==u'H';})?1:0);
        if(type==0x1005){for(const auto& token:tokens){if(hour(token))return locale_info_number(0);if(token.field==u't')return locale_info_number(1);}return locale_info_number(0);}
        const char16_t field=type==0x24?u'y':type==0x25?0:type==0x26?u'd':u'M';
        for(const auto& token:tokens)if(field?token.field==field && (field!=u'd' || token.text.size()<=2):hour(token))
            return locale_info_number(field==u'y'?token.text.size()>=4:token.text.size()==2);
        return locale_info_number(0);
    }
    if(type==0x1f || type==0x20 || type==0x1003 || type==0x79){
        const bool time=type==0x1003 || type==0x79;error=U_ZERO_ERROR;
        DateFormat format(udat_open(time?(type==0x79?UDAT_SHORT:UDAT_MEDIUM):UDAT_NONE,time?UDAT_NONE:(type==0x20?UDAT_FULL:UDAT_SHORT),id.c_str(),u"UTC",3,nullptr,0,&error),udat_close);
        if(U_FAILURE(error) || !format)return {};return text(windows_info_pattern(date_pattern(format.get())));
    }
    return {};
}
