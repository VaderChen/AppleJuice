// 使用者地區覆寫（由 windows_locale.cpp 在 invoke_locale 之前引入）：HKCU\Control Panel\International 的值，
// SetLocaleInfo 寫入、GetLocaleInfo 對使用者預設地區（未指定 LOCALE_NOUSEROVERRIDE）讀出。沿用登錄交易、安全性檢查與變更通知。
namespace {
struct LocaleOverride {std::uint32_t type;const char16_t* name;std::uint32_t limit;std::uint32_t minimum,maximum;bool number;};
// limit 為不含結尾 NUL 的最大字元數；number 為十進位整數欄位（minimum／maximum 為允許範圍）。
constexpr LocaleOverride locale_overrides[]{
    {0x1009,u"iCalendarType",2,1,23,true},{0x19,u"iCurrDigits",2,0,99,true},{0x1b,u"iCurrency",1,0,3,true},{0x11,u"iDigits",2,0,99,true},
    {0x1014,u"NumShape",1,0,2,true},{0x100c,u"iFirstDayOfWeek",1,0,6,true},{0x100d,u"iFirstWeekOfYear",1,0,2,true},{0x12,u"iLZero",1,0,1,true},
    {0x0d,u"iMeasure",1,0,1,true},{0x1c,u"iNegCurr",2,0,15,true},{0x1010,u"iNegNumber",1,0,4,true},{0x100a,u"iPaperSize",1,1,9,true},{0x23,u"iTime",1,0,1,true},
    {0x28,u"s1159",14,0,0,false},{0x29,u"s2359",14,0,0,false},{0x14,u"sCurrency",12,0,0,false},{0x0e,u"sDecimal",3,0,0,false},{0x10,u"sGrouping",9,0,0,false},
    {0x0c,u"sList",3,0,0,false},{0x20,u"sLongDate",79,0,0,false},{0x16,u"sMonDecimalSep",3,0,0,false},{0x18,u"sMonGrouping",9,0,0,false},
    {0x17,u"sMonThousandSep",3,0,0,false},{0x13,u"sNativeDigits",10,0,0,false},{0x51,u"sNegativeSign",4,0,0,false},{0x50,u"sPositiveSign",4,0,0,false},
    {0x79,u"sShortTime",79,0,0,false},{0x1f,u"sShortDate",79,0,0,false},{0x0f,u"sThousand",3,0,0,false},{0x1006,u"sYearMonth",79,0,0,false},
    {0x1003,u"sTimeFormat",79,0,0,false},{0x1d,u"sDate",3,0,0,false},{0x1e,u"sTime",3,0,0,false}};
const LocaleOverride* locale_override(std::uint32_t type){
    for(const auto& item:locale_overrides)if(item.type==type)return &item;
    return nullptr;
}
std::u16string override_fold(std::u16string_view text){std::u16string value(text);for(auto& c:value)c=unicode_case_unit(c,false);return value;}
// 要求的是否為使用者預設地區（套用使用者覆寫的對象）：LCID 為 LOCALE_USER_DEFAULT 或使用者地區 0x409；名稱為空（LOCALE_NAME_USER_DEFAULT）或 en-US。
bool locale_user_request(bool named_request,std::u16string_view name,std::uint32_t id){
    if(named_request)return name.empty() || override_fold(name)==u"en-us";
    return id==0x400 || id==0x409;
}
// 日期／時間格式中第一個不在引號內、也不是格式字母的字元（分隔符號）。
std::u16string format_separator(std::u16string_view format,std::u16string_view letters){
    bool quoted=false;std::u16string separator;
    for(const auto c:format){
        if(c==u'\''){quoted=!quoted;if(!separator.empty())break;continue;}
        const bool letter=letters.find(c)!=std::u16string_view::npos;
        if(quoted || letter){if(!separator.empty())break;continue;}
        if(c==u' ' && separator.empty())continue;
        separator.push_back(c);
    }
    while(!separator.empty() && separator.back()==u' ')separator.pop_back();
    return separator;
}
// 把格式中引號外的舊分隔符號換成新分隔符號。
std::u16string replace_separator(std::u16string_view format,std::u16string_view from,std::u16string_view to){
    if(from.empty())return std::u16string(format);std::u16string out;bool quoted=false;
    for(std::size_t i=0;i<format.size();){
        if(format[i]==u'\''){quoted=!quoted;out.push_back(format[i++]);continue;}
        if(!quoted && format.substr(i,from.size())==from){out.append(to);i+=from.size();continue;}
        out.push_back(format[i++]);
    }
    return out;
}
}
std::optional<std::u16string> WindowsServices::user_locale_override(std::uint32_t type){
    const auto* entry=locale_override(type);if(!entry)return std::nullopt;
    if(!registry_file_.empty() && !runtime_.network.external_io)return std::nullopt;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return std::nullopt;
    const auto path=registry_current_user_path(true)+u"\\control panel\\international";
    const auto key=runtime_.registry.keys.find(path);if(key==runtime_.registry.keys.end())return std::nullopt;
    try{(void)registry_security_access(path,1);}catch(const SecurityDescriptorError&){return std::nullopt;}
    const auto value=key->second.find(override_fold(entry->name));if(value==key->second.end() || (value->second.type!=1 && value->second.type!=2))return std::nullopt;
    std::u16string text;const auto& bytes=value->second.bytes;
    for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(unsigned(bytes[i+1])<<8));if(!c)break;text.push_back(c);}
    return text;
}
std::uint32_t WindowsServices::set_user_locale_overrides(const std::vector<std::pair<std::u16string,std::u16string>>& values,GuestMemory& memory){
    if(!registry_file_.empty() && !runtime_.network.external_io)return 50;
    auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,false);if(transaction.error)return transaction.error;
    const auto base=registry_current_user_path();const auto before=registry;const auto now=runtime_.system_filetime();std::vector<std::u16string> created;
    std::u16string path=base;
    try{
        for(const auto& [folded,display]:std::initializer_list<std::pair<std::u16string,std::u16string>>{{u"control panel",u"Control Panel"},{u"international",u"International"}}){
            const auto parent=path;path+=u"\\";path+=folded;if(registry.keys.contains(path))continue;
            registry.keys.try_emplace(parent);(void)registry_security_access(parent,4U);auto descriptor=registry_creation_descriptor(registry_security_descriptor(parent),{});
            registry.keys.try_emplace(path);registry.display_names[path]=display;registry.modified_times[path]=now;registry.security_descriptors[path]=std::move(descriptor);created.push_back(parent);
        }
        (void)registry_security_access(path,2U);auto& key=registry.keys[path];
        for(const auto& [name,text]:values){
            const auto folded=override_fold(name);if(const auto old=key.find(folded);old!=key.end()){registry.bytes-=old->second.bytes.size();--registry.values;key.erase(old);}
            RegistryValue stored{1,utf16_bytes(text),name};registry.bytes+=stored.bytes.size();++registry.values;key[folded]=std::move(stored);
        }
        registry.modified_times[path]=now;
        if(const auto error=commit_registry_transaction(transaction)){registry=before;return error;}
    }catch(const SecurityDescriptorError& error){registry=before;return error.code;}
    for(const auto& parent:created)poll_registry_notifications(false,parent,1);poll_registry_notifications(false,path,4);poll_registry_notifications(false,{},0,&memory);
    return 0;
}
