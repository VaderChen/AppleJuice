// propsys 的 PROPVARIANT／VARIANT 轉換輔助（由 windows_shell.cpp 的 PROPVARIANT 區塊引入，沿用同一組 read_variant／write_variant／clear_variant／string_value）。
// - change：PropVariantChangeType 的型別表。數值與布林互轉（布林為 0／1，數值非零即 TRUE），超出範圍回 HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW)；
//   字串轉數值不受地區影響（預設接受 0x 十六進位，PVCHF_NOHEXSTRING 關閉），字串轉布林接受 True／False；字串與 FILETIME／DATE 使用標準格式
//   「yyyy/mm/dd:hh:mm:ss.fff」；字串與 CLSID 用 {…} 文字；VT_EMPTY 轉成數值 0、FALSE 或空字串；只有一個元素的向量可轉成單一值，向量之間逐元素轉換。
// - PropVariantTo*／VariantTo*：先經 change 再寫出；失敗時輸出歸零並回傳錯誤，WithDefault 版本回傳預設值。
// - compare_values：PropVariantCompareEx（與 VariantCompare 共用）。空值、VT_NULL 與空向量最小（PVCF_TREATEMPTYASGREATERTHAN 時最大）；型別不同時先把右值轉成左值型別，
//   無法轉換才依 VARTYPE 排序；字串依旗標呼叫 shlwapi 的 StrCmpLogicalW／StrCmpW／StrCmpCW／StrCmpIW／StrCmpICW；FILETIME／DATE 依 PROPVAR_COMPARE_UNIT 截斷後比較。
// - PropVariantToVariant：LPSTR／LPWSTR／CLSID→BSTR、FILETIME→DATE、VT_VECTOR|x→VT_ARRAY|y（SafeArrayCreateVector）、BLOB→IStream（SHCreateMemStream）、
//   STREAM 與介面以 AddRef 共用；VariantToPropVariant 與 PropVariantCopy 的 VT_ARRAY 以 SafeArrayCopy 複製。
const bool propvariant_helper=symbol=="PropVariantChangeType" || symbol=="PropVariantCompareEx" || symbol=="PropVariantGetStringElem" || symbol=="PropVariantToVariant" ||
    symbol=="VariantToPropVariant" || symbol=="VariantToGUID" || symbol=="VariantToStringWithDefault" || symbol=="VariantCompare" ||
    (symbol.starts_with("PropVariantTo") && !symbol.starts_with("PropVariantToString")) || symbol=="PropVariantToStringWithDefault" ||
    (symbol.starts_with("InitPropVariantFrom") && symbol!="InitPropVariantFromCLSID") || symbol.starts_with("InitVariantFrom") ||
    (symbol=="PropVariantCopy" && arg(0) && arg(1) && arg(0)!=arg(1) && (memory.load(GuestAddress{arg(1)},16)&0x7000U)==0x2000U);
constexpr std::uint32_t type_mismatch=automation::mismatch,arithmetic_overflow=0x80070216U,bad_type=automation::badtype,buffer_small=0x80028016U,generic_failure=0x80004005U;
using Converted=std::pair<std::uint32_t,GuiStoredVariant>;
const auto little=[](const auto& bytes,unsigned n,std::size_t at=0){std::uint64_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint64_t{bytes[at+i]}<<(8*i);return value;};
const auto scalar_variant=[](std::uint16_t type,std::uint64_t value){GuiStoredVariant v;v.type=type;for(unsigned i=0;i<8;++i)v.scalar[i]=static_cast<std::uint8_t>(value>>(8*i));return v;};
const auto is_text=[](unsigned type){return type==8 || type==30 || type==31;};
const auto is_number=[](unsigned type){switch(type){case 2:case 3:case 4:case 5:case 6:case 14:case 16:case 17:case 18:case 19:case 20:case 21:case 22:case 23:return true;default:return false;}};
const auto text_variant=[&](std::uint16_t type,std::u16string_view text){
    GuiStoredVariant v;v.type=type;
    if(type==30){const auto narrow=utf16_to_utf8_lossy(text);v.payload.assign(narrow.begin(),narrow.end());v.payload.push_back(0);}
    else{v.payload=utf16_bytes(text);if(type==8)v.payload.resize(v.payload.size()-2);}
    return v;};
const auto vector_items=[&](const GuiStoredVariant& v){
    std::vector<GuiStoredVariant> items;const auto element=static_cast<std::uint16_t>(v.type&0xfffU);
    if(is_text(element)){
        for(std::size_t at=0;at+4<=v.payload.size();){
            const auto length=static_cast<std::size_t>(little(v.payload,4,at));at+=4;if(at+length>v.payload.size())throw std::invalid_argument("PROPVARIANT 向量資料不合法");
            GuiStoredVariant item;item.type=element;item.payload.assign(v.payload.begin()+static_cast<std::ptrdiff_t>(at),v.payload.begin()+static_cast<std::ptrdiff_t>(at+length));at+=length;items.push_back(std::move(item));
        }
    }else{
        const auto stride=vector_stride(v.type);if(!stride)throw std::invalid_argument("不支援的向量型別");
        for(std::size_t at=0;at+stride<=v.payload.size();at+=stride){
            GuiStoredVariant item;item.type=element;
            if(element==72)item.payload.assign(v.payload.begin()+static_cast<std::ptrdiff_t>(at),v.payload.begin()+static_cast<std::ptrdiff_t>(at+16));
            else std::copy_n(v.payload.begin()+static_cast<std::ptrdiff_t>(at),stride,item.scalar.begin());
            items.push_back(std::move(item));
        }
    }
    return items;};
const auto vector_of=[&](std::uint16_t element,const std::vector<GuiStoredVariant>& items){
    GuiStoredVariant v;v.type=static_cast<std::uint16_t>(0x1000U|element);const auto stride=vector_stride(v.type);
    for(const auto& item:items){
        if(is_text(element)){for(unsigned k=0;k<4;++k)v.payload.push_back(static_cast<std::uint8_t>(item.payload.size()>>(8*k)));v.payload.insert(v.payload.end(),item.payload.begin(),item.payload.end());}
        else if(element==72)v.payload.insert(v.payload.end(),item.payload.begin(),item.payload.end());
        else v.payload.insert(v.payload.end(),item.scalar.begin(),item.scalar.begin()+static_cast<std::ptrdiff_t>(stride));
    }
    return v;};
// FILETIME（1601 年起 100 ns）與 DATE（1899-12-30 起的日數，負值的小數部分仍向後計時）互轉，精度到毫秒。
constexpr std::int64_t day_ms=86400000,date_epoch_days=109205;
const auto filetime_date=[&](std::uint64_t ticks)->std::optional<double>{
    if(ticks>>63)return std::nullopt;const auto ms=static_cast<std::int64_t>(ticks/10000);const auto days=ms/day_ms-date_epoch_days;
    const auto fraction=static_cast<double>(ms%day_ms)/static_cast<double>(day_ms);const auto value=days<0?static_cast<double>(days)-fraction:static_cast<double>(days)+fraction;
    if(value>=2958466.0)return std::nullopt;return value;};
const auto date_filetime=[&](double value)->std::optional<std::uint64_t>{
    if(!std::isfinite(value) || value<=-657435.0 || value>=2958466.0)return std::nullopt;const auto integral=std::trunc(value);
    const auto ms=static_cast<std::int64_t>(std::llround(std::abs(value-integral)*static_cast<double>(day_ms)));const auto total=(static_cast<std::int64_t>(integral)+date_epoch_days)*day_ms+ms;
    if(total<0)return std::nullopt;return static_cast<std::uint64_t>(total)*10000;};
const auto canonical_time=[&](std::uint64_t ticks){
    using namespace std::chrono;const auto ms=static_cast<std::int64_t>(ticks/10000);const year_month_day ymd{sys_days{year{1601}/1/1}+days{ms/day_ms}};const auto in_day=ms%day_ms;
    char text[48];std::snprintf(text,sizeof text,"%04d/%02u/%02u:%02lld:%02lld:%02lld.%03lld",int(ymd.year()),unsigned(ymd.month()),unsigned(ymd.day()),
        static_cast<long long>(in_day/3600000),static_cast<long long>(in_day/60000%60),static_cast<long long>(in_day/1000%60),static_cast<long long>(in_day%1000));
    return utf8_to_utf16(text);};
const auto parse_canonical_time=[&](std::u16string_view text)->std::optional<std::uint64_t>{
    std::array<unsigned,7> field{};std::size_t at=0;constexpr std::u16string_view separators=u"//:::.";
    for(unsigned i=0;i<7;++i){
        if(i){if(at>=text.size() && i==6)break;if(at>=text.size() || text[at]!=separators[i-1])return std::nullopt;++at;}
        unsigned digits=0,value=0;while(at<text.size() && text[at]>=u'0' && text[at]<=u'9' && digits<(i?3U:4U)){value=value*10+static_cast<unsigned>(text[at]-u'0');++at;++digits;}
        if(!digits)return std::nullopt;field[i]=value;
    }
    if(at!=text.size() || field[3]>23 || field[4]>59 || field[5]>59 || field[6]>999)return std::nullopt;
    using namespace std::chrono;const year_month_day ymd{year{static_cast<int>(field[0])},month{field[1]},day{field[2]}};if(!ymd.ok())return std::nullopt;
    const auto days_since=(sys_days{ymd}-sys_days{year{1601}/1/1}).count();if(days_since<0)return std::nullopt;
    const auto ms=static_cast<std::int64_t>(days_since)*day_ms+field[3]*3600000LL+field[4]*60000LL+field[5]*1000LL+field[6];return static_cast<std::uint64_t>(ms)*10000;};
const auto failed=[](std::uint32_t status){return Converted{status,GuiStoredVariant{}};};
std::function<Converted(const GuiStoredVariant&,std::uint16_t,std::uint32_t)> change;
change=[&](const GuiStoredVariant& source,std::uint16_t target,std::uint32_t flags)->Converted{
    if(source.type==target)return {0,source};
    if(!target)return {0,GuiStoredVariant{}};
    const bool source_vector=(source.type&0x1000U)!=0,target_vector=(target&0x1000U)!=0;
    if(source_vector || target_vector){
        if(source_vector && !target_vector){const auto items=vector_items(source);if(items.size()!=1)return failed(type_mismatch);return change(items[0],target,flags);}
        if(!source_vector)return failed(type_mismatch);
        if(!vector_stride(target))return failed(bad_type);
        std::vector<GuiStoredVariant> items;
        for(const auto& item:vector_items(source)){auto converted=change(item,static_cast<std::uint16_t>(target&0xfffU),flags);if(converted.first)return failed(converted.first);items.push_back(std::move(converted.second));}
        return {0,vector_of(static_cast<std::uint16_t>(target&0xfffU),items)};
    }
    if(source.type==1 || target==1)return failed(type_mismatch);
    if(!source.type){
        if(is_number(target) || target==11)return {0,scalar_variant(target,0)};
        if(is_text(target))return {0,text_variant(target,u"")};
        return failed(type_mismatch);
    }
    if(is_text(target)){
        std::u16string text;
        if(source.type==64)text=canonical_time(little(source.scalar,8));
        else if(source.type==7){const auto ticks=date_filetime(std::bit_cast<double>(little(source.scalar,8)));if(!ticks)return failed(type_mismatch);text=canonical_time(*ticks);}
        else if(is_text(source.type) || is_number(source.type) || source.type==11 || source.type==72)text=string_value(source);
        else return failed(type_mismatch);
        return {0,text_variant(target,text)};
    }
    if(target==64 || target==7){
        std::optional<std::uint64_t> ticks;
        if(source.type==64)ticks=little(source.scalar,8);
        else if(source.type==7)ticks=date_filetime(std::bit_cast<double>(little(source.scalar,8)));
        else if(is_text(source.type))ticks=parse_canonical_time(string_value(source));
        else return failed(type_mismatch);
        if(!ticks)return failed(type_mismatch);
        if(target==64)return {0,scalar_variant(64,*ticks)};
        const auto date=filetime_date(*ticks);if(!date)return failed(type_mismatch);return {0,scalar_variant(7,std::bit_cast<std::uint64_t>(*date))};
    }
    if(target==72){
        if(!is_text(source.type))return failed(type_mismatch);
        const auto guid=parse_windows_guid(string_value(source),true);if(!guid)return failed(type_mismatch);
        GuiStoredVariant v;v.type=72;v.payload.assign(guid->begin(),guid->end());return {0,v};
    }
    if(target!=11 && !is_number(target))return failed(bad_type);
    const auto status_of=[&](std::uint32_t status){return status==automation::overflow?arithmetic_overflow:type_mismatch;};
    automation::Value value{};std::string decimal;
    if(source.type==11){automation::put(value,0,2,3);automation::put(value,8,4,little(source.scalar,2)?1:0);}
    else if(is_number(source.type)){std::copy_n(source.scalar.begin(),scalar_size(source.type),value.begin()+(source.type==14?0:8));automation::put(value,0,2,source.type);}
    else if(is_text(source.type)){
        auto text=string_value(source);
        if(target==11)if(const auto boolean=automation::parse_bool(text,0x409,(flags&8U)!=0))return {0,scalar_variant(11,*boolean?0xffffU:0)};
        if(!(flags&0x10U) && text.size()>2 && text[0]==u'0' && (text[1]==u'x' || text[1]==u'X')){
            std::uint64_t hex=0;
            for(std::size_t i=2;i<text.size();++i){
                const auto c=text[i];const unsigned digit=c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:16;
                if(digit>15)return failed(type_mismatch);if(hex>>60)return failed(arithmetic_overflow);hex=hex<<4|digit;
            }
            automation::put(value,0,2,21);automation::put(value,8,8,hex);
        }else{
            automation::put(value,0,2,8);std::uint32_t status=0;const auto parsed=automation::parse_number(std::move(text),0x409,status);
            if(!parsed)return failed(status_of(status));decimal=*parsed;
        }
    }
    else return failed(type_mismatch);
    if(target==11){
        automation::Value number{};const auto status=automation::number_convert(value,decimal,5,number);if(status)return failed(status_of(status));
        return {0,scalar_variant(11,std::bit_cast<double>(automation::get(number,8,8))!=0.0?0xffffU:0)};
    }
    automation::Value out{};if(const auto status=automation::number_convert(value,decimal,target,out))return failed(status_of(status));
    GuiStoredVariant result;result.type=target;std::copy_n(out.begin()+(target==14?0:8),scalar_size(target),result.scalar.begin());return {0,result};
};
// 比較：同型別後依類別比較；向量逐元素比較，再比元素數。
const auto time_key=[&](std::uint64_t ticks,unsigned unit_kind)->std::uint64_t{
    switch(unit_kind){
    case 1:return ticks/10000000;case 2:return ticks/600000000;case 3:return ticks/36000000000ULL;case 4:return ticks/864000000000ULL;
    case 5:case 6:{using namespace std::chrono;const year_month_day ymd{sys_days{year{1601}/1/1}+days{static_cast<std::int64_t>(ticks/864000000000ULL)}};
        return unit_kind==6?static_cast<std::uint64_t>(int(ymd.year())):static_cast<std::uint64_t>(int(ymd.year()))*12+unsigned(ymd.month());}
    default:return ticks;
    }};
std::function<std::int32_t(const GuiStoredVariant&,const GuiStoredVariant&,unsigned,std::uint32_t)> compare_same;
compare_same=[&](const GuiStoredVariant& left,const GuiStoredVariant& right,unsigned unit_kind,std::uint32_t flags)->std::int32_t{
    const auto order=[](auto a,auto b)->std::int32_t{return a<b?-1:b<a?1:0;};
    if(left.type&0x1000U){
        const auto a=vector_items(left),b=vector_items(right);
        for(std::size_t i=0;i<std::min(a.size(),b.size());++i)if(const auto c=compare_same(a[i],b[i],unit_kind,flags))return c;
        return order(a.size(),b.size());
    }
    const auto raw=[&](const GuiStoredVariant& v,unsigned n){return little(v.scalar,n);};
    switch(left.type){
    case 16:return order(static_cast<std::int8_t>(raw(left,1)),static_cast<std::int8_t>(raw(right,1)));
    case 2:return order(static_cast<std::int16_t>(raw(left,2)),static_cast<std::int16_t>(raw(right,2)));
    case 3:case 22:return order(static_cast<std::int32_t>(raw(left,4)),static_cast<std::int32_t>(raw(right,4)));
    case 20:case 6:return order(static_cast<std::int64_t>(raw(left,8)),static_cast<std::int64_t>(raw(right,8)));
    case 17:return order(raw(left,1),raw(right,1));case 18:return order(raw(left,2),raw(right,2));case 19:case 23:return order(raw(left,4),raw(right,4));case 21:return order(raw(left,8),raw(right,8));
    case 4:return order(std::bit_cast<float>(static_cast<std::uint32_t>(raw(left,4))),std::bit_cast<float>(static_cast<std::uint32_t>(raw(right,4))));
    case 5:return order(std::bit_cast<double>(raw(left,8)),std::bit_cast<double>(raw(right,8)));
    case 11:return order(raw(left,2)!=0,raw(right,2)!=0);
    case 64:return order(time_key(raw(left,8),unit_kind),time_key(raw(right,8),unit_kind));
    case 7:{const auto a=date_filetime(std::bit_cast<double>(raw(left,8))),b=date_filetime(std::bit_cast<double>(raw(right,8)));
        if(a && b)return order(time_key(*a,unit_kind),time_key(*b,unit_kind));return order(std::bit_cast<double>(raw(left,8)),std::bit_cast<double>(raw(right,8)));}
    case 14:{automation::Value a{},b{},x{},y{};std::copy_n(left.scalar.begin(),16,a.begin());std::copy_n(right.scalar.begin(),16,b.begin());automation::put(a,0,2,14);automation::put(b,0,2,14);
        if(automation::number_convert(a,{},5,x) || automation::number_convert(b,{},5,y))return order(left.scalar,right.scalar);
        return order(std::bit_cast<double>(automation::get(x,8,8)),std::bit_cast<double>(automation::get(y,8,8)));}
    case 8:case 30:case 31:{
        // 與 shlwapi 的字串比較共用實作（StrCmpLogicalW 為預設）。
        GuestScratch scratch(*this,memory,state);const auto a=scratch.text(string_value(left)),b=scratch.text(string_value(right));if(!a || !b)throw std::bad_alloc{};
        const auto function=(flags&2U)?"StrCmpW":(flags&4U)?"StrCmpCW":(flags&8U)?"StrCmpIW":(flags&0x10U)?"StrCmpICW":"StrCmpLogicalW";
        const auto result=static_cast<std::int32_t>(static_cast<std::uint32_t>(nested_service(state,memory,"shlwapi.dll",function,{a,b})));return result<0?-1:result>0?1:0;}
    default:return left.payload!=right.payload?order(left.payload,right.payload):order(left.scalar,right.scalar);
    }};
const auto compare_values=[&](const GuiStoredVariant& left,const GuiStoredVariant& right,unsigned unit_kind,std::uint32_t flags)->std::int32_t{
    const auto empty=[](const GuiStoredVariant& v){return v.type==0 || v.type==1 || ((v.type&0x1000U) && v.payload.empty());};
    if(empty(left) || empty(right)){if(empty(left)==empty(right))return 0;const std::int32_t result=empty(left)?-1:1;return (flags&1U)?-result:result;}
    if(left.type==right.type)return compare_same(left,right,unit_kind,flags);
    auto converted=change(right,left.type,0);
    if(converted.first)return left.type<right.type?-1:1;
    return compare_same(left,converted.second,unit_kind,flags);};
// SAFEARRAY 一維向量的資料與元素數：{cDims, fFeatures, cbElements, cLocks, pvData, {cElements, lLbound}}。
const auto safe_vector=[&](std::uint64_t array,unsigned element_size)->std::pair<std::uint64_t,std::uint64_t>{
    if(!array || memory.load(GuestAddress{array},16)!=1 || memory.load(GuestAddress{array+4},32)!=element_size)throw std::invalid_argument("SAFEARRAY 不是一維向量");
    const auto data=memory.load(GuestAddress{array+(x86_?12U:16U)},bits),count=memory.load(GuestAddress{array+(x86_?16U:24U)},32);
    if(count>16*1024*1024 || (count && !data))throw std::invalid_argument("SAFEARRAY 資料不合法");return {data,count};};
const auto task_string=[&](std::u16string_view text){
    const auto bytes=utf16_bytes(text);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)throw std::bad_alloc{};
    memory.write(GuestAddress{pointer},bytes);gui_.task_allocations.insert(pointer);return pointer;};
// 介面型別共用同一物件：複製指標後呼叫 AddRef。
const auto share_interface=[&](std::uint64_t output,std::uint16_t type,std::uint64_t object)->std::optional<std::uint32_t>{
    std::vector<std::uint8_t> data(size);data[0]=type&255;data[1]=type>>8;for(unsigned i=0;i<unit;++i)data[8+i]=static_cast<std::uint8_t>(object>>(8*i));memory.write(GuestAddress{output},data);
    if(!object)return finish(0);const auto table=memory.load(GuestAddress{object},bits);
    start_callbacks(state,memory,{{memory.load(GuestAddress{table+unit},bits),{object}}},Continuation::Return,0);return std::nullopt;};
const auto copy_array=[&](std::uint64_t output,std::uint16_t type,std::uint64_t array)->std::optional<std::uint32_t>{
    std::uint64_t copy=0;
    if(array){GuestScratch scratch(*this,memory,state);const auto slot=scratch.allocate(8);if(!slot)return finish(0x8007000eU);
        if(const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"oleaut32.dll","SafeArrayCopy",{array,slot})))return finish(status);copy=memory.load(GuestAddress{slot},bits);}
    std::vector<std::uint8_t> data(size);data[0]=type&255;data[1]=type>>8;for(unsigned i=0;i<unit;++i)data[8+i]=static_cast<std::uint8_t>(copy>>(8*i));memory.write(GuestAddress{output},data);return finish(0);};
if(propvariant_helper)try{
    const auto zero=[&](std::uint64_t at){memory.check(GuestAddress{at},size,Access::Write);memory.write(GuestAddress{at},std::vector<std::uint8_t>(size));};
    const auto type_at=[&](std::uint64_t at){return static_cast<std::uint16_t>(memory.load(GuestAddress{at},16));};
    struct ScalarTarget {std::string_view name;std::uint16_t type;unsigned bytes;};
    static constexpr std::array<ScalarTarget,8> scalar_targets{{{"PropVariantToInt16",2,2},{"PropVariantToUInt16",18,2},{"PropVariantToInt32",3,4},{"PropVariantToUInt32",19,4},
        {"PropVariantToInt64",20,8},{"PropVariantToUInt64",21,8},{"PropVariantToDouble",5,8},{"PropVariantToBoolean",11,4}}};
    for(const auto& target:scalar_targets)if(symbol==target.name){
        if(!arg(1))return finish(invalid_argument);memory.check(GuestAddress{arg(1)},target.bytes,Access::Write);memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(target.bytes*8),0);
        if(!arg(0))return finish(invalid_argument);const auto converted=change(read_variant(arg(0)),target.type,0);if(converted.first)return finish(converted.first);
        const auto value=target.type==11?(little(converted.second.scalar,2)?1U:0U):little(converted.second.scalar,target.bytes);
        memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(target.bytes*8),value);return finish(0);
    }
    if(symbol=="PropVariantToUInt32WithDefault"){
        const auto fallback=static_cast<std::uint32_t>(arg(1));if(!arg(0) || !type_at(arg(0)))return finish(fallback);
        try{const auto converted=change(read_variant(arg(0)),19,0);return finish(converted.first?fallback:static_cast<std::uint32_t>(little(converted.second.scalar,4)));}
        catch(const std::invalid_argument&){return finish(fallback);}
    }
    if(symbol=="PropVariantChangeType"){
        // (ppropvarDest, propvarSrc, flags, vt)：目的與來源不可相同；旗標 PVCHF_* 只接受 0x1f 內的位元。
        if(!arg(0) || !arg(1) || arg(0)==arg(1) || (static_cast<std::uint32_t>(arg(2))&~0x1fU))return finish(invalid_argument);
        const auto target=static_cast<std::uint16_t>(arg(3));zero(arg(0));const auto type=type_at(arg(1));
        if(type==target && (type==9 || type==13))return share_interface(arg(0),type,memory.load(GuestAddress{arg(1)+8},bits));
        if(type==target && (type&0x7000U)==0x2000U)return copy_array(arg(0),type,memory.load(GuestAddress{arg(1)+8},bits));
        const auto converted=change(read_variant(arg(1)),target,static_cast<std::uint32_t>(arg(2)));if(converted.first)return finish(converted.first);
        write_variant(arg(0),converted.second);return finish(0);
    }
    if(symbol=="PropVariantToFileTime"){
        // (propvar, PSTIME_FLAGS, FILETIME*)：只接受 VT_FILETIME／VT_DATE（UTC）；PSTF_LOCAL 經 FileTimeToLocalFileTime。
        if(!arg(2))return finish(invalid_argument);memory.check(GuestAddress{arg(2)},8,Access::Write);memory.store(GuestAddress{arg(2)},64,0);
        const auto flags=static_cast<std::uint32_t>(arg(1));if(flags>1 || !arg(0))return finish(invalid_argument);
        const auto source=read_variant(arg(0));if(source.type!=64 && source.type!=7)return finish(type_mismatch);
        const auto converted=change(source,64,0);if(converted.first)return finish(converted.first);auto ticks=little(converted.second.scalar,8);
        if(flags){GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(16);if(!buffer)return finish(0x8007000eU);memory.store(GuestAddress{buffer},64,ticks);
            if(!(nested_service(state,memory,"kernel32.dll","FileTimeToLocalFileTime",{buffer,buffer+8})&UINT32_MAX))return finish(invalid_argument);ticks=memory.load(GuestAddress{buffer+8},64);}
        memory.store(GuestAddress{arg(2)},64,ticks);return finish(0);
    }
    if(symbol=="PropVariantToGUID" || symbol=="VariantToGUID"){
        if(!arg(1))return finish(invalid_argument);memory.check(GuestAddress{arg(1)},16,Access::Write);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(16));
        if(!arg(0))return finish(invalid_argument);const auto converted=change(read_variant(arg(0)),72,0);if(converted.first)return finish(converted.first);
        memory.write(GuestAddress{arg(1)},converted.second.payload);return finish(0);
    }
    if(symbol=="PropVariantToBSTR"){
        if(!arg(1))return finish(invalid_argument);memory.store(GuestAddress{arg(1)},bits,0);if(!arg(0))return finish(invalid_argument);
        const auto converted=change(read_variant(arg(0)),8,0);if(converted.first)return finish(converted.first);
        memory.store(GuestAddress{arg(1)},bits,make_bstr(converted.second.payload));return finish(0);
    }
    if(symbol=="PropVariantToStringWithDefault" || symbol=="VariantToStringWithDefault"){
        // 回傳來源內的 LPWSTR／BSTR 指標（不複製）；其他型別、VT_EMPTY 與空指標回傳預設值。
        if(!arg(0))return finish(arg(1));const auto type=type_at(arg(0));
        if(type==31 || type==8)if(const auto pointer=memory.load(GuestAddress{arg(0)+8},bits))return finish(pointer);
        return finish(arg(1));
    }
    if(symbol=="PropVariantGetStringElem"){
        // VT_LPWSTR／VT_BSTR（索引須為 0）、VT_VECTOR|VT_LPWSTR、VT_VECTOR|VT_BSTR、VT_ARRAY|VT_BSTR；結果以 CoTaskMemAlloc 配置，空 BSTR 傳回空字串。
        if(!arg(2))return finish(invalid_argument);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(0))return finish(invalid_argument);
        const auto type=type_at(arg(0));const auto index=static_cast<std::uint32_t>(arg(1));std::u16string text;
        if(type==31 || type==8){if(index)return finish(invalid_argument);text=string_value(read_variant(arg(0)));}
        else if(type==0x101f || type==0x1008){const auto items=vector_items(read_variant(arg(0)));if(index>=items.size())return finish(invalid_argument);text=string_value(items[index]);}
        else if(type==0x2008){
            const auto [data,count]=safe_vector(memory.load(GuestAddress{arg(0)+8},bits),unit);if(index>=count)return finish(invalid_argument);
            if(const auto item=memory.load(GuestAddress{data+index*unit},bits)){std::vector<std::uint8_t> bytes(memory.load(GuestAddress{item-4},32));if(!bytes.empty())memory.read(GuestAddress{item},bytes);
                GuiStoredVariant v;v.type=8;v.payload=std::move(bytes);text=string_value(v);}
        }
        else return finish(invalid_argument);
        memory.store(GuestAddress{arg(2)},bits,task_string(text));return finish(0);
    }
    if(symbol=="PropVariantToBuffer"){
        // VT_VECTOR|VT_UI1 或 VT_ARRAY|VT_UI1：取前 cb 位元組；資料不足時 E_FAIL 且不修改緩衝區。
        if(!arg(0))return finish(invalid_argument);const auto type=type_at(arg(0));const auto length=static_cast<std::uint32_t>(arg(2));std::vector<std::uint8_t> bytes;
        if(type==0x1011)bytes=read_variant(arg(0)).payload;
        else if(type==0x2011){const auto [data,count]=safe_vector(memory.load(GuestAddress{arg(0)+8},bits),1);bytes.resize(count);if(count)memory.read(GuestAddress{data},bytes);}
        else return finish(invalid_argument);
        if(bytes.size()<length)return finish(generic_failure);if(length){if(!arg(1))return finish(invalid_argument);memory.write(GuestAddress{arg(1)},std::span(bytes).first(length));}
        return finish(0);
    }
    if(symbol=="PropVariantToUInt32Vector"){
        // VT_VECTOR|VT_UI4 或 VT_ARRAY|VT_UI4；元素多於 crgn 時 TYPE_E_BUFFERTOOSMALL，*pcElem 為 0。
        const auto capacity=static_cast<std::uint32_t>(arg(2));if(!arg(3))return finish(invalid_argument);memory.store(GuestAddress{arg(3)},32,0);if(!arg(0))return finish(invalid_argument);
        const auto type=type_at(arg(0));std::vector<std::uint8_t> bytes;
        if(type==0x1013)bytes=read_variant(arg(0)).payload;
        else if(type==0x2013){const auto [data,count]=safe_vector(memory.load(GuestAddress{arg(0)+8},bits),4);bytes.resize(count*4);if(count)memory.read(GuestAddress{data},bytes);}
        else return finish(invalid_argument);
        const auto count=bytes.size()/4;if(count>capacity)return finish(buffer_small);if(count){if(!arg(1))return finish(invalid_argument);memory.write(GuestAddress{arg(1)},bytes);}
        memory.store(GuestAddress{arg(3)},32,count);return finish(0);
    }
    if(symbol=="PropVariantCompareEx" || symbol=="VariantCompare"){
        const bool extended=symbol=="PropVariantCompareEx";const auto unit_kind=extended?static_cast<unsigned>(arg(2)):0U;const auto flags=extended?static_cast<std::uint32_t>(arg(3)):0U;
        std::optional<GuiStoredVariant> left,right;try{left=read_variant(arg(0));}catch(const std::invalid_argument&){}try{right=read_variant(arg(1));}catch(const std::invalid_argument&){}
        if(!left || !right){const auto a=type_at(arg(0)),b=type_at(arg(1));return finish(static_cast<std::uint32_t>(a<b?-1:a>b?1:0));}
        return finish(static_cast<std::uint32_t>(compare_values(*left,*right,unit_kind,flags)));
    }
    if(symbol=="PropVariantToVariant" || symbol=="VariantToPropVariant" || symbol=="PropVariantCopy"){
        const bool to_variant=symbol=="PropVariantToVariant";const auto source=symbol=="PropVariantCopy"?arg(1):arg(0),output=symbol=="PropVariantCopy"?arg(0):arg(1);
        if(!source || !output)return finish(invalid_argument);if(symbol=="PropVariantCopy"){const auto error=clear_variant(output);if(error)return finish(error);}
        const auto type=type_at(source);zero(output);const auto pointer=memory.load(GuestAddress{source+8},bits);
        if(type&0x4000U)return finish(bad_type);
        if(type==9 || type==13 || (to_variant && type==66))return share_interface(output,type==66?13:type,pointer);
        if((type&0x7000U)==0x2000U)return copy_array(output,type,pointer);
        if(!to_variant){write_variant(output,read_variant(source));return finish(0);}
        if((type>=67 && type<=71) || type==0x1047)return finish(bad_type);
        if(type==65){
            const auto length=memory.load(GuestAddress{source+8},32),data=memory.load(GuestAddress{source+8+unit},bits);
            const auto stream=nested_service(state,memory,"shlwapi.dll","SHCreateMemStream",{data,length});if(!stream)return finish(0x8007000eU);
            std::vector<std::uint8_t> bytes(size);bytes[0]=13;for(unsigned i=0;i<unit;++i)bytes[8+i]=static_cast<std::uint8_t>(stream>>(8*i));memory.write(GuestAddress{output},bytes);return finish(0);
        }
        const auto value=read_variant(source);
        if(type&0x1000U){
            // 向量轉 SAFEARRAY：字串與 CLSID 元素轉成 BSTR、FILETIME 轉成 DATE，其餘沿用元素型別。
            const auto element=static_cast<std::uint16_t>(type&0xfffU);const std::uint16_t target=is_text(element) || element==72?8:element==64?7:element;
            std::vector<GuiStoredVariant> items;for(const auto& item:vector_items(value)){auto converted=change(item,target,0);if(converted.first)return finish(converted.first);items.push_back(std::move(converted.second));}
            const auto array=nested_service(state,memory,"oleaut32.dll","SafeArrayCreateVector",{target,0,items.size()});if(!array)return finish(0x8007000eU);
            try{
                const auto data=memory.load(GuestAddress{array+(x86_?12U:16U)},bits);const auto stride=target==8?unit:scalar_size(target);
                for(std::size_t i=0;i<items.size();++i){
                    if(target==8)memory.store(GuestAddress{data+i*stride},bits,make_bstr(items[i].payload));
                    else memory.write(GuestAddress{data+i*stride},std::span(items[i].scalar).first(stride));
                }
            }catch(...){(void)nested_service(state,memory,"oleaut32.dll","SafeArrayDestroy",{array});throw;}
            std::vector<std::uint8_t> bytes(size);const auto array_type=static_cast<std::uint16_t>(0x2000U|target);bytes[0]=array_type&255;bytes[1]=array_type>>8;
            for(unsigned i=0;i<unit;++i)bytes[8+i]=static_cast<std::uint8_t>(array>>(8*i));memory.write(GuestAddress{output},bytes);return finish(0);
        }
        if(is_text(type) || type==72){const auto converted=change(value,8,0);if(converted.first)return finish(converted.first);write_variant(output,converted.second);return finish(0);}
        if(type==64){const auto converted=change(value,7,0);if(converted.first)return finish(converted.first);write_variant(output,converted.second);return finish(0);}
        write_variant(output,value);return finish(0);
    }
    if(symbol=="InitPropVariantFromBuffer" || symbol=="InitVariantFromBuffer"){
        // (pv, cb, ppropvar／pvar)：PROPVARIANT 為 VT_VECTOR|VT_UI1，VARIANT 為 VT_ARRAY|VT_UI1。
        const auto length=static_cast<std::uint32_t>(arg(1));if(!arg(2))return finish(invalid_argument);zero(arg(2));if(length && !arg(0))return finish(invalid_argument);
        std::vector<std::uint8_t> bytes(length);if(length)memory.read(GuestAddress{arg(0)},bytes);
        if(symbol=="InitPropVariantFromBuffer"){GuiStoredVariant v;v.type=0x1011;v.payload=std::move(bytes);write_variant(arg(2),v);return finish(0);}
        const auto array=nested_service(state,memory,"oleaut32.dll","SafeArrayCreateVector",{17,0,length});if(!array)return finish(0x8007000eU);
        if(length)memory.write(GuestAddress{memory.load(GuestAddress{array+(x86_?12U:16U)},bits)},bytes);
        std::vector<std::uint8_t> data(size);data[0]=0x11;data[1]=0x20;for(unsigned i=0;i<unit;++i)data[8+i]=static_cast<std::uint8_t>(array>>(8*i));memory.write(GuestAddress{arg(2)},data);return finish(0);
    }
    if(symbol=="InitPropVariantFromGUIDAsString" || symbol=="InitVariantFromGUIDAsString"){
        if(!arg(0) || !arg(1))return finish(invalid_argument);zero(arg(1));WindowsGuid guid{};memory.read(GuestAddress{arg(0)},guid);
        write_variant(arg(1),text_variant(symbol=="InitVariantFromGUIDAsString"?8:31,windows_guid_text(guid)));return finish(0);
    }
    if(symbol=="InitPropVariantFromStringVector"){
        // (prgsz, cElems, ppropvar)：VT_VECTOR|VT_LPWSTR，各字串另行複製；任一元素為 NULL 時失敗。
        const auto count=static_cast<std::uint32_t>(arg(1));if(!arg(2))return finish(invalid_argument);zero(arg(2));if(count && !arg(0))return finish(invalid_argument);
        if(count>1048576)return finish(0x8007000eU);std::vector<GuiStoredVariant> items;
        for(std::uint32_t i=0;i<count;++i){const auto item=memory.load(GuestAddress{arg(0)+i*unit},bits);if(!item)return finish(invalid_argument);items.push_back(text_variant(31,read_utf16(memory,GuestAddress{item})));}
        write_variant(arg(2),vector_of(31,items));return finish(0);
    }
    if(symbol=="InitVariantFromFileTime"){
        if(!arg(0) || !arg(1))return finish(invalid_argument);zero(arg(1));const auto date=filetime_date(memory.load(GuestAddress{arg(0)},64));if(!date)return finish(invalid_argument);
        write_variant(arg(1),scalar_variant(7,std::bit_cast<std::uint64_t>(*date)));return finish(0);
    }
}catch(const std::invalid_argument&){return finish(type_mismatch);}
