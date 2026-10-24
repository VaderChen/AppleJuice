// NLS 日曆查詢與列舉共用 ICU 資料；格式、月份格位及 Windows CALID 保持一致。
struct CalendarKind {std::uint32_t id;const char* keyword;const char* language;};
std::optional<Locale> calendar_numbered(std::uint32_t id){
    // 此家族接受三個 custom LCID 別名；本虛擬系統尚無另外安裝的 custom locale。
    return numbered(id==0xc00 || id==0x1000 || id==0x1400?0x400:id);
}
constexpr CalendarKind calendar_kinds[]{
    {1,"gregorian",nullptr},{2,"gregorian","en_US"},{3,"japanese",nullptr},
    {4,"roc",nullptr},{5,"gregorian",nullptr},{6,"islamic-civil",nullptr},
    {7,"buddhist",nullptr},{8,"hebrew",nullptr},{9,"gregorian","fr_FR"},
    {10,"gregorian","ar_SA"},{11,"gregorian","ar_SA"},{12,"gregorian","ar_SA"},
    {22,"persian",nullptr},{23,"islamic-umalqura",nullptr}
};
const CalendarKind* calendar_kind(std::uint32_t id){
    for(const auto& kind:calendar_kinds)if(kind.id==id)return &kind;return nullptr;
}
std::string calendar_locale(const Locale& locale,const CalendarKind& kind,bool english=false){
    return std::string(english?"en_US":kind.language?kind.language:icu_locale(locale))+"@calendar="+kind.keyword;
}
std::vector<std::uint32_t> locale_calendars(const Locale& locale){
    const auto id=icu_locale(locale);UErrorCode error=U_ZERO_ERROR;
    std::unique_ptr<UEnumeration,decltype(&uenum_close)> values(ucal_getKeywordValuesForLocale("calendar",id.c_str(),true,&error),uenum_close);
    if(U_FAILURE(error) || !values)throw std::runtime_error("無法取得語系日曆清單");
    std::vector<std::uint32_t> result;
    const auto add=[&](std::uint32_t value){if(std::find(result.begin(),result.end(),value)==result.end())result.push_back(value);};
    while(const auto* value=uenum_next(values.get(),nullptr,&error)){
        const std::string_view name(value);if(name=="dangi"){add(5);continue;}
        if(name.starts_with("islamic") && name!="islamic-umalqura"){add(6);continue;}
        for(const auto& kind:calendar_kinds)if(!kind.language && kind.id!=5 && name==kind.keyword){add(kind.id);break;}
    }
    if(U_FAILURE(error))throw std::runtime_error("語系日曆列舉失敗");
    add(1); // Gregorian 在 Windows NLS 中可用於所有語系。
    return result;
}
using Calendar=std::unique_ptr<UCalendar,decltype(&ucal_close)>;
Calendar calendar_open(const std::string& id){
    UErrorCode error=U_ZERO_ERROR;Calendar result(ucal_open(u"UTC",3,id.c_str(),UCAL_DEFAULT,&error),ucal_close);
    if(U_FAILURE(error) || !result)throw std::runtime_error("無法建立 ICU 日曆");return result;
}
std::uint32_t calendar_year_max(const CalendarKind& kind){
    if(kind.id==3 || kind.id==4)return 99;
    if(kind.id==5)return 2029+2333;
    // 與本相容層既有 1930～2029 年的日期解析區間一致；使用者覆寫另存於客體登錄。
    auto value=calendar_open(std::string("en_US@calendar=")+kind.keyword);UErrorCode error=U_ZERO_ERROR;
    ucal_setMillis(value.get(),1893455999000.0,&error);const auto year=ucal_get(value.get(),UCAL_YEAR,&error);
    if(U_FAILURE(error) || year<99)throw std::runtime_error("無法計算日曆年份上限");return static_cast<std::uint32_t>(year);
}
std::optional<std::uint32_t> calendar_decimal(std::u16string_view text){
    if(text.empty())return {};std::uint32_t value=0;
    for(const auto c:text){if(c<u'0' || c>u'9')return {};const auto digit=static_cast<unsigned>(c-u'0');if(value>(9999-digit)/10)return {};value=value*10+digit;}
    if(value<99 || value>9999)return {};return value;
}
std::optional<LocaleInfo> calendar_information(const Locale& locale,const CalendarKind& kind,
                                              std::uint32_t type,bool genitive){
    if(type==1)return locale_info_number(kind.id);
    if(type==0x30)return locale_info_number(calendar_year_max(kind));
    const auto id=calendar_locale(locale,kind,type==0x3b || type==0x3c);UErrorCode error=U_ZERO_ERROR;
    DateFormat date(udat_open(UDAT_NONE,UDAT_SHORT,id.c_str(),u"UTC",3,nullptr,0,&error),udat_close);
    if(U_FAILURE(error) || !date)throw std::runtime_error("無法取得日曆格式");
    const auto text=[](std::u16string value){return LocaleInfo{std::move(value),{}};};
    const auto symbol=[&](UDateFormatSymbolType group,int index){
        if(index<0 || index>=udat_countSymbols(date.get(),group))return std::u16string{};
        return locale_info_text([&](UChar* buffer,int count,UErrorCode* status){return udat_getSymbols(date.get(),group,index,buffer,count,status);});
    };
    if(type==2){
        if(kind.id==5)return text(u"단군기원");
        std::unique_ptr<ULocaleDisplayNames,decltype(&uldn_close)> names(uldn_open(icu_locale(locale).c_str(),ULDN_STANDARD_NAMES,&error),uldn_close);
        if(U_FAILURE(error) || !names)throw std::runtime_error("無法取得日曆名稱");
        return text(locale_info_text([&](UChar* buffer,int count,UErrorCode* status){return uldn_keyValueDisplayName(names.get(),"calendar",kind.keyword,buffer,count,status);}));
    }
    if(type==3){
        if(kind.id==5)return locale_info_number(2333);if(kind.id==7)return locale_info_number(543);
        if(kind.id!=3 && kind.id!=4)return locale_info_number(0);
        auto native=calendar_open(id);const auto era=udat_countSymbols(date.get(),UDAT_ERAS)-1;
        ucal_clear(native.get());ucal_set(native.get(),UCAL_ERA,era);ucal_setDate(native.get(),1,0,1,&error);
        const auto stamp=ucal_getMillis(native.get(),&error);auto gregorian=calendar_open("en_US@calendar=gregorian");
        ucal_setMillis(gregorian.get(),stamp,&error);const auto year=ucal_get(gregorian.get(),UCAL_YEAR,&error);
        if(U_FAILURE(error) || year<1)throw std::runtime_error("日曆年代轉換失敗");return locale_info_number(static_cast<std::uint32_t>(year));
    }
    if(type==4 || type==0x39 || type==0x3b || type==0x3c){
        if(kind.id==5)return text(type==0x3b || type==0x3c?u"Dangun":u"단기");
        const auto group=type==4 || type==0x3b?UDAT_ERA_NAMES:UDAT_ERAS;
        return text(symbol(group,udat_countSymbols(date.get(),group)-1));
    }
    if(type>=7 && type<=0xd)return text(symbol(UDAT_WEEKDAYS,static_cast<int>((type-7+1)%7+1)));
    if(type>=0xe && type<=0x14)return text(symbol(UDAT_SHORT_WEEKDAYS,static_cast<int>((type-0xe +1)%7+1)));
    if(type>=0x31 && type<=0x37)return text(symbol(UDAT_NARROW_WEEKDAYS,static_cast<int>((type-0x31+1)%7+1)));
    if(type>=0x15 && type<=0x2e){
        const bool abbreviated=type>=0x22;int index=static_cast<int>(type-(abbreviated?0x22:0x15));
        // ICU 希伯來曆固定保留 Adar I 的格位；Windows 使用平年月序，閏月另置第十三格。
        if(kind.id==8){if(index==12)index=5;else if(index>=5)++index;}
        return text(symbol(abbreviated?(genitive?UDAT_SHORT_MONTHS:UDAT_STANDALONE_SHORT_MONTHS):
                                      (genitive?UDAT_MONTHS:UDAT_STANDALONE_MONTHS),index));
    }
    if(type==5)return text(windows_info_pattern(date_pattern(date.get())));
    if(type==6){
        error=U_ZERO_ERROR;DateFormat full(udat_open(UDAT_NONE,UDAT_FULL,id.c_str(),u"UTC",3,nullptr,0,&error),udat_close);
        if(U_FAILURE(error) || !full)throw std::runtime_error("無法取得完整日曆日期格式");return text(windows_info_pattern(date_pattern(full.get())));
    }
    if(type==0x2f || type==0x38 || type==0x3a){
        error=U_ZERO_ERROR;std::unique_ptr<UDateTimePatternGenerator,decltype(&udatpg_close)> patterns(udatpg_open(id.c_str(),&error),udatpg_close);
        if(U_FAILURE(error) || !patterns)throw std::runtime_error("無法取得日曆樣式");
        const std::u16string_view skeleton=type==0x2f?u"yMMMM":type==0x38?u"MMMMd":u"MMMMEEEEd";
        auto value=windows_info_pattern(locale_info_text([&](UChar* buffer,int count,UErrorCode* status){
            return udatpg_getBestPattern(patterns.get(),skeleton.data(),static_cast<int>(skeleton.size()),buffer,count,status);}));
        if(type==0x2f){auto tokens=date_tokens(value);for(auto& token:tokens)if(token.field==u'y' && token.text.size()!=2)token.text=u"yyyy";value=render_pattern(tokens);}
        return text(std::move(value));
    }
    if(type==0x3d)return text(kind.id==3?u"元":u"1");
    return {};
}
