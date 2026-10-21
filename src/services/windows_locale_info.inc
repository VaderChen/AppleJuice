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
    case 0x68:return text(invariant?u"IVC":utf8_to_utf16(uloc_getISO3Country(id.c_str())));
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
    if(type==0x1f || type==0x20 || type==0x1003 || type==0x79){
        const bool time=type==0x1003 || type==0x79;error=U_ZERO_ERROR;
        DateFormat format(udat_open(time?(type==0x79?UDAT_SHORT:UDAT_MEDIUM):UDAT_NONE,time?UDAT_NONE:(type==0x20?UDAT_FULL:UDAT_SHORT),id.c_str(),u"UTC",3,nullptr,0,&error),udat_close);
        if(U_FAILURE(error) || !format)return {};return text(windows_info_pattern(date_pattern(format.get())));
    }
    return {};
}
