#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "guest_counted_string.hpp"
#include "message_digest.hpp"
#include <algorithm>
#include <array>
#include <bit>

// ntdll 的純運算 Rtl 家族：記憶體、計數字串（STRING／UNICODE_STRING）、整數轉換、GUID、位元圖、LUID、TIME_FIELDS、
// 亂數、CRC32、MD4／MD5／SHA-1 訊息摘要、自訂字碼頁表（CPTABLEINFO）與 PE 映像輔助。全部在宿主端直接讀寫客體記憶體，不經客體回呼；ANSI／OEM 與引擎其他轉碼相同使用 UTF-8。
namespace x64arm {
namespace {
constexpr std::uint32_t status_invalid_parameter=0xc000000dU,status_buffer_too_small=0xc0000023U,status_buffer_overflow=0x80000005U,
    status_no_memory=0xc0000017U,status_not_found=0xc0000225U,status_access_violation=0xc0000005U;
constexpr std::uint64_t ticks_per_second=10000000ULL,ticks_per_day=86400ULL*ticks_per_second;
constexpr std::uint64_t seconds_1601_to_1970=11644473600ULL,seconds_1601_to_1980=11960006400ULL;
bool leap(std::uint64_t year){return (year%4==0 && year%100!=0) || year%400==0;}
unsigned month_days(std::uint64_t year,unsigned month){
    constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};return month==2 && leap(year)?29U:days[month-1];
}
// 自 1601-01-01 起的日數 → 年月日。
void civil_from_days(std::uint64_t days,std::uint64_t& year,unsigned& month,unsigned& day){
    year=1601;
    // 400 年週期 146097 日。
    year+=400*(days/146097);days%=146097;
    for(;;){const auto length=leap(year)?366U:365U;if(days<length)break;days-=length;++year;}
    month=1;for(;;){const auto length=month_days(year,month);if(days<length)break;days-=length;++month;}
    day=static_cast<unsigned>(days)+1;
}
std::optional<std::uint64_t> days_from_civil(std::uint64_t year,unsigned month,unsigned day){
    if(year<1601 || year>30827 || month<1 || month>12 || day<1 || day>month_days(year,month))return std::nullopt;
    std::uint64_t days=146097*((year-1601)/400);for(auto y=year-(year-1601)%400;y<year;++y)days+=leap(y)?366:365;
    for(unsigned m=1;m<month;++m)days+=month_days(year,m);return days+day-1;
}
std::uint32_t crc32_table_value(std::uint32_t index){std::uint32_t c=index;for(int k=0;k<8;++k)c=c&1U?0xedb88320U^(c>>1):c>>1;return c;}
const std::array<std::uint32_t,256>& crc32_table(){static const auto table=[]{std::array<std::uint32_t,256> t{};for(std::uint32_t i=0;i<256;++i)t[i]=crc32_table_value(i);return t;}();return table;}
// RtlUniform：Microsoft 文件記載的線性同餘產生器，結果在 [0, MAXLONG-1]。
std::uint32_t uniform_next(std::uint32_t& seed){
    const auto value=static_cast<std::uint32_t>((static_cast<std::uint64_t>(seed)*0x7fffffedULL+0x7fffffc3ULL)%0x7fffffffULL);seed=value;return value;
}
}
std::span<const WindowsServiceDescriptor> ntdll_pure_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        // 記憶體
        {"ntdll.dll","RtlCompareMemory",3},{"ntdll.dll","RtlCompareMemoryUlong",3,0,0,0x4},{"ntdll.dll","RtlFillMemory",3,0,0,0x4},
        {"ntdll.dll","RtlFillMemoryUlong",3,0,0,0x4},{"ntdll.dll","RtlZeroMemory",2},
        // 計數字串
        {"ntdll.dll","RtlRunEncodeUnicodeString",2},{"ntdll.dll","RtlRunDecodeUnicodeString",2},
        {"ntdll.dll","RtlCopyUnicodeString",2},{"ntdll.dll","RtlCopyString",2},{"ntdll.dll","RtlAppendUnicodeStringToString",2},
        {"ntdll.dll","RtlAppendUnicodeToString",2},{"ntdll.dll","RtlAppendStringToString",2},{"ntdll.dll","RtlAppendAsciizToString",2},
        {"ntdll.dll","RtlDuplicateUnicodeString",3,0,0,0x1},{"ntdll.dll","RtlUpcaseUnicodeString",3},{"ntdll.dll","RtlDowncaseUnicodeString",3},
        {"ntdll.dll","RtlUpcaseUnicodeChar",1,0,0,0x1},{"ntdll.dll","RtlDowncaseUnicodeChar",1,0,0,0x1},{"ntdll.dll","RtlUpperChar",1,0,0,0x1},
        {"ntdll.dll","RtlUpperString",2},{"ntdll.dll","RtlCompareString",3},{"ntdll.dll","RtlEqualString",3},{"ntdll.dll","RtlPrefixString",3},
        {"ntdll.dll","RtlPrefixUnicodeString",3},{"ntdll.dll","RtlCompareUnicodeStrings",5},{"ntdll.dll","RtlHashUnicodeString",4,0,0,0x4},
        {"ntdll.dll","RtlFindCharInUnicodeString",4,0,0,0x1},{"ntdll.dll","RtlEraseUnicodeString",1},
        {"ntdll.dll","RtlEqualComputerName",2},{"ntdll.dll","RtlEqualDomainName",2},{"ntdll.dll","RtlAnsiCharToUnicodeChar",1},
        {"ntdll.dll","RtlUnicodeStringToAnsiSize",1},{"ntdll.dll","RtlxUnicodeStringToAnsiSize",1},{"ntdll.dll","RtlUnicodeStringToOemSize",1},
        {"ntdll.dll","RtlxUnicodeStringToOemSize",1},{"ntdll.dll","RtlAnsiStringToUnicodeSize",1},{"ntdll.dll","RtlxAnsiStringToUnicodeSize",1},
        {"ntdll.dll","RtlOemStringToUnicodeSize",1},{"ntdll.dll","RtlxOemStringToUnicodeSize",1},
        {"ntdll.dll","RtlUpcaseUnicodeStringToAnsiString",3},{"ntdll.dll","RtlUpcaseUnicodeStringToOemString",3},
        {"ntdll.dll","RtlUnicodeStringToCountedOemString",3},{"ntoskrnl.exe","RtlOemStringToCountedUnicodeString",3},
        {"ntdll.dll","RtlUpcaseUnicodeStringToCountedOemString",3},{"ntdll.dll","RtlUpcaseUnicodeToMultiByteN",5,0,0,0x12},
        {"ntdll.dll","RtlUpcaseUnicodeToOemN",5,0,0,0x12},{"ntdll.dll","RtlIsNameLegalDOS8Dot3",3},
        // 整數轉換
        {"ntdll.dll","RtlIntegerToChar",4,0,0,0x7},{"ntdll.dll","RtlIntegerToUnicodeString",3,0,0,0x3},
        {"ntdll.dll","RtlInt64ToUnicodeString",3,0x1,0,0x2},{"ntdll.dll","RtlLargeIntegerToChar",4,0,0,0x6},
        {"ntdll.dll","RtlCharToInteger",3,0,0,0x2},{"ntdll.dll","RtlUnicodeStringToInteger",3,0,0,0x2},
        // GUID
        {"ntdll.dll","RtlGUIDFromString",2},{"ntdll.dll","RtlStringFromGUID",2},
        // 位元圖
        {"ntdll.dll","RtlInitializeBitMap",3,0,0,0x4},{"ntdll.dll","RtlSetBits",3,0,0,0x6},{"ntdll.dll","RtlClearBits",3,0,0,0x6},
        {"ntdll.dll","RtlSetAllBits",1},{"ntdll.dll","RtlClearAllBits",1},{"ntdll.dll","RtlAreBitsSet",3,0,0,0x6},{"ntdll.dll","RtlAreBitsClear",3,0,0,0x6},
        {"ntdll.dll","RtlFindSetBits",3,0,0,0x6},{"ntdll.dll","RtlFindClearBits",3,0,0,0x6},{"ntdll.dll","RtlFindSetBitsAndClear",3,0,0,0x6},
        {"ntdll.dll","RtlFindClearBitsAndSet",3,0,0,0x6},{"ntdll.dll","RtlNumberOfSetBits",1},{"ntdll.dll","RtlNumberOfClearBits",1},
        {"ntdll.dll","RtlFindLongestRunSet",2},{"ntdll.dll","RtlFindLongestRunClear",2},
        {"ntdll.dll","RtlFindNextForwardRunSet",3,0,0,0x2},{"ntdll.dll","RtlFindNextForwardRunClear",3,0,0,0x2},
        {"ntdll.dll","RtlFindLastBackwardRunSet",3,0,0,0x2},{"ntdll.dll","RtlFindLastBackwardRunClear",3,0,0,0x2},
        {"ntdll.dll","RtlFindSetRuns",4,0,0,0xc},{"ntdll.dll","RtlFindClearRuns",4,0,0,0xc},
        {"ntdll.dll","RtlFindMostSignificantBit",1,0x1},{"ntdll.dll","RtlFindLeastSignificantBit",1,0x1},
        // LUID
        {"ntdll.dll","RtlCopyLuid",2},{"ntdll.dll","RtlEqualLuid",2},{"ntdll.dll","RtlCopyLuidAndAttributesArray",3,0,0,0x1},
        // TIME_FIELDS 與秒數換算
        {"ntdll.dll","RtlTimeToTimeFields",2},{"ntdll.dll","RtlTimeFieldsToTime",2},{"ntdll.dll","RtlTimeToElapsedTimeFields",2},
        {"ntdll.dll","RtlTimeToSecondsSince1970",2},{"ntdll.dll","RtlSecondsSince1970ToTime",2,0,0,0x1},
        {"ntdll.dll","RtlTimeToSecondsSince1980",2},{"ntdll.dll","RtlSecondsSince1980ToTime",2,0,0,0x1},
        // 亂數與 CRC
        {"ntdll.dll","RtlUniform",1},{"ntdll.dll","RtlRandom",1},{"ntdll.dll","RtlRandomEx",1},{"ntdll.dll","RtlComputeCrc32",3,0,0,0x5},
        // 訊息摘要（advapi32 同名匯出共用此實作）
        {"ntdll.dll","MD4Init",1},{"ntdll.dll","MD4Update",3,0,0,0x4},{"ntdll.dll","MD4Final",1},{"ntdll.dll","MD5Init",1},{"ntdll.dll","MD5Update",3,0,0,0x4},{"ntdll.dll","MD5Final",1},
        {"ntdll.dll","A_SHAInit",1},{"ntdll.dll","A_SHAUpdate",3,0,0,0x4},{"ntdll.dll","A_SHAFinal",2},
        // 自訂字碼頁表
        {"ntdll.dll","RtlInitCodePageTable",2},{"ntdll.dll","RtlCustomCPToUnicodeN",6,0,0,0x24},{"ntdll.dll","RtlUnicodeToCustomCPN",6,0,0,0x24},{"ntdll.dll","RtlUpcaseUnicodeToCustomCPN",6,0,0,0x24},
        // PE 映像
        {"ntdll.dll","RtlImageNtHeader",1},{"ntdll.dll","RtlImageDirectoryEntryToData",4,0,0,0x6},{"ntdll.dll","RtlImageRvaToSection",3,0,0,0x4},
        {"ntdll.dll","RtlImageRvaToVa",4,0,0,0x4},{"ntdll.dll","RtlAddressInSectionTable",3,0,0,0x4},
        // 程序與執行緒狀態
        {"ntdll.dll","RtlGetNtProductType",1},{"ntdll.dll","RtlGetNtGlobalFlags",0},{"ntdll.dll","RtlIsCurrentProcess",1},{"ntdll.dll","RtlIsCurrentThread",1},
        {"ntdll.dll","RtlSetLastWin32ErrorAndNtStatusFromNtStatus",1,0,0,0x1},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_pure(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto byte=[&](unsigned index){return static_cast<std::uint8_t>(arg(index));};
    // STRING／UNICODE_STRING 的讀寫與載入器、Rtl 轉接共用 guest_counted_string.hpp。
    const auto header=[&](std::uint64_t address){return read_counted_header(memory,address,x86_);};
    const auto set_length=[&](std::uint64_t address,std::uint64_t length){memory.store(GuestAddress{address},16,length);};
    const auto wide_text=[&](const GuestCountedString& value){return read_counted_wide(memory,value);};
    const auto narrow_text=[&](const GuestCountedString& value){return read_counted_narrow(memory,value);};
    const auto upper=[](char16_t c){return unicode_case_unit(c,true);};
    const auto ascii_upper=[](char c){return c>='a' && c<='z'?static_cast<char>(c-32):c;};
    const auto bytes_of=[](std::u16string_view text){return utf16_payload(text);};
    const auto store_counted=[&](std::uint64_t address,std::vector<std::uint8_t> data,unsigned terminator,bool allocate)->std::uint32_t{
        return store_counted_string(memory,address,x86_,std::move(data),terminator,allocate,status_buffer_overflow,[&](std::size_t size){return allocate_heap(memory,size);});};
    if(name=="RtlRunEncodeUnicodeString" || name=="RtlRunDecodeUnicodeString"){
        const bool encode=name=="RtlRunEncodeUnicodeString";
        const auto value=header(arg(1));
        const auto seed_arg=arg(0);
        auto seed=static_cast<std::uint8_t>(encode?memory.load(GuestAddress{seed_arg},8):seed_arg);
        if(encode && !seed){
            auto time=runtime_.system_filetime()>>8;
            while(time && !seed){seed=static_cast<std::uint8_t>(time);time>>=8;}
            if(!seed)seed=1;
            memory.store(GuestAddress{seed_arg},8,seed);
        }
        // 解碼從末端往前，以保留尚未使用的編碼位元組；長度可為奇數。
        std::uint8_t previous=0x43U;
        for(unsigned i=0;i<value.length;++i){
            const auto index=encode?i:value.length-1U-i;
            const auto address=GuestAddress{value.buffer+index};
            if(encode)seed=static_cast<std::uint8_t>(memory.load(GuestAddress{seed_arg},8));
            else if(index)previous=static_cast<std::uint8_t>(memory.load(GuestAddress{address.value-1},8));
            const auto mask=index?(seed^previous):(seed|0x43U);
            previous=static_cast<std::uint8_t>(memory.load(address,8)^mask);
            memory.store(address,8,previous);
        }
        return done(0);
    }
    // 位元圖：RTL_BITMAP {ULONG SizeOfBitMap; PULONG Buffer}。
    struct Bitmap {std::uint32_t size;std::uint64_t buffer;};
    const auto bitmap=[&](std::uint64_t address)->Bitmap{return {static_cast<std::uint32_t>(memory.load(GuestAddress{address},32)),memory.load(GuestAddress{address+unit},bits)};};
    const auto test_bit=[&](const Bitmap& map,std::uint32_t index){return (memory.load(GuestAddress{map.buffer+index/8},8)>>(index%8))&1U;};
    const auto put_bits=[&](const Bitmap& map,std::uint32_t start,std::uint32_t count,bool value){
        for(std::uint32_t i=0;i<count;){
            const auto index=start+i;const auto at=GuestAddress{map.buffer+index/8};
            if(index%8==0 && count-i>=8){memory.store(at,8,value?0xffU:0U);i+=8;continue;}
            auto current=static_cast<std::uint8_t>(memory.load(at,8));const auto mask=static_cast<std::uint8_t>(1U<<(index%8));
            current=value?static_cast<std::uint8_t>(current|mask):static_cast<std::uint8_t>(current&~mask);memory.store(at,8,current);++i;
        }
    };
    const auto range_is=[&](const Bitmap& map,std::uint32_t start,std::uint32_t count,bool value){
        for(std::uint32_t i=0;i<count;++i)if(static_cast<bool>(test_bit(map,start+i))!=value)return false;return true;};
    // 自 hint 起找 count 個連續 value 位元（不足時從頭繞回），回傳起點或 0xFFFFFFFF。
    const auto find_bits=[&](const Bitmap& map,std::uint32_t count,std::uint32_t hint,bool value)->std::uint32_t{
        if(!count)return hint<map.size?hint:0;if(count>map.size)return UINT32_MAX;
        if(hint>=map.size)hint=0;
        const auto scan=[&](std::uint32_t from,std::uint32_t limit)->std::uint32_t{
            std::uint32_t run=0;for(std::uint32_t i=from;i<limit;++i){if(static_cast<bool>(test_bit(map,i))==value){if(++run==count)return i+1-count;}else run=0;}return UINT32_MAX;};
        auto found=scan(hint,map.size);if(found==UINT32_MAX && hint)found=scan(0,std::min(map.size,hint+count-1));return found;
    };
    struct Run {std::uint32_t start,length;};
    const auto runs=[&](const Bitmap& map,bool value){
        std::vector<Run> list;std::uint32_t i=0;
        while(i<map.size){if(static_cast<bool>(test_bit(map,i))!=value){++i;continue;}const auto start=i;while(i<map.size && static_cast<bool>(test_bit(map,i))==value)++i;list.push_back({start,i-start});}
        return list;
    };
    try{
        if(name=="RtlCompareMemory"){
            const auto length=arg(2);std::uint64_t equal=0;
            for(;equal<length;++equal)if(memory.load(GuestAddress{arg(0)+equal},8)!=memory.load(GuestAddress{arg(1)+equal},8))break;
            return done(equal);
        }
        if(name=="RtlCompareMemoryUlong"){
            // 長度不是 4 的倍數時只比較完整的 ULONG；回傳相符的位元組數。
            const auto length=arg(1)&~std::uint64_t{3};std::uint64_t equal=0;
            for(;equal<length;equal+=4)if(memory.load(GuestAddress{arg(0)+equal},32)!=u32(2))break;
            return done(equal);
        }
        if(name=="RtlFillMemory" || name=="RtlZeroMemory"){
            const auto length=arg(1);const auto value=name=="RtlZeroMemory"?0U:byte(2);
            if(length>output_limit)throw MemoryFault(GuestAddress{arg(0)},Access::Write,1,"RtlFillMemory 長度過大");
            memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(static_cast<std::size_t>(length),static_cast<std::uint8_t>(value)));return done(0);
        }
        if(name=="RtlFillMemoryUlong"){
            const auto count=arg(1)/4;for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{arg(0)+i*4},32,u32(2));return done(0);
        }
        if(name=="RtlCopyUnicodeString" || name=="RtlCopyString"){
            // 目的為 NULL 時不做事；來源為 NULL 時長度設 0。複製量受目的 MaximumLength 限制，有空間時附加 NUL。
            const bool wide=name=="RtlCopyUnicodeString";
            if(!arg(0))return done(0);
            if(!arg(1)){set_length(arg(0),0);return done(0);}
            const auto target=header(arg(0)),source=header(arg(1));
            std::uint32_t count=std::min<std::uint32_t>(source.length,target.capacity);if(wide)count&=~1U;
            std::vector<std::uint8_t> data(count);if(count)memory.read(GuestAddress{source.buffer},data);
            if(count)memory.write(GuestAddress{target.buffer},data);set_length(arg(0),count);
            const unsigned terminator=wide?2U:1U;if(count+terminator<=target.capacity)memory.store(GuestAddress{target.buffer+count},static_cast<std::uint8_t>(terminator*8U),0);
            return done(0);
        }
        if(name=="RtlAppendUnicodeStringToString" || name=="RtlAppendUnicodeToString" || name=="RtlAppendStringToString" || name=="RtlAppendAsciizToString"){
            const bool wide=name.starts_with("RtlAppendUnicode");const auto target=header(arg(0));
            std::vector<std::uint8_t> data;
            if(name=="RtlAppendUnicodeToString"){if(arg(1))data=bytes_of(read_utf16(memory,GuestAddress{arg(1)},32767));}
            else if(name=="RtlAppendAsciizToString"){if(arg(1)){const auto text=read_ansi(memory,arg(1),65535);data.assign(text.begin(),text.end());}}
            else {const auto source=header(arg(1));data.resize(source.length);if(source.length)memory.read(GuestAddress{source.buffer},data);}
            if(target.length+data.size()>target.capacity)return done(status_buffer_too_small);
            if(!data.empty())memory.write(GuestAddress{target.buffer+target.length},data);
            const auto length=target.length+data.size();set_length(arg(0),length);
            const unsigned terminator=wide?2U:1U;
            // RtlAppendUnicodeToString／RtlAppendUnicodeStringToString 在有空間時附加 NUL。
            if((wide || name=="RtlAppendAsciizToString") && length+terminator<=target.capacity)memory.store(GuestAddress{target.buffer+length},static_cast<std::uint8_t>(terminator*8U),0);
            return done(0);
        }
        if(name=="RtlDuplicateUnicodeString"){
            // flags：1＝結果附加 NUL，2＝來源為空時仍配置空字串。
            const auto flags=u32(0);if(flags&~3U || !arg(1) || !arg(2))return done(status_invalid_parameter);
            const auto source=header(arg(1));if(source.length%2)return done(status_invalid_parameter);
            if(!source.length && !(flags&2U)){memory.store(GuestAddress{arg(2)},32,0);memory.store(GuestAddress{arg(2)+unit},bits,0);return done(0);}
            std::vector<std::uint8_t> data(source.length);if(source.length)memory.read(GuestAddress{source.buffer},data);
            return done(store_counted(arg(2),std::move(data),flags&1U?2U:0U,true));
        }
        if(name=="RtlUpcaseUnicodeString" || name=="RtlDowncaseUnicodeString"){
            const bool to_upper=name=="RtlUpcaseUnicodeString";auto text=wide_text(header(arg(1)));
            for(auto& c:text)c=unicode_case_unit(c,to_upper);
            return done(store_counted(arg(0),bytes_of(text),0,(arg(2)&0xffU)!=0));
        }
        if(name=="RtlUpcaseUnicodeChar" || name=="RtlDowncaseUnicodeChar")return done(unicode_case_unit(static_cast<char16_t>(arg(0)),name=="RtlUpcaseUnicodeChar"));
        if(name=="RtlUpperChar")return done(static_cast<std::uint8_t>(ascii_upper(static_cast<char>(byte(0)))));
        if(name=="RtlUpperString"){
            // 依目的 MaximumLength 截斷，不附加 NUL。
            const auto target=header(arg(0));auto text=narrow_text(header(arg(1)));for(auto& c:text)c=ascii_upper(c);
            text.resize(std::min<std::size_t>(text.size(),target.capacity));
            if(!text.empty())memory.write(GuestAddress{target.buffer},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
            set_length(arg(0),text.size());return done(0);
        }
        if(name=="RtlCompareString" || name=="RtlEqualString" || name=="RtlPrefixString"){
            const bool fold=(arg(2)&0xffU)!=0;auto first=narrow_text(header(arg(0))),second=narrow_text(header(arg(1)));
            if(fold){for(auto& c:first)c=ascii_upper(c);for(auto& c:second)c=ascii_upper(c);}
            if(name=="RtlPrefixString")return done(second.size()>=first.size() && std::equal(first.begin(),first.end(),second.begin())?1U:0U);
            if(name=="RtlEqualString")return done(first==second?1U:0U);
            const auto count=std::min(first.size(),second.size());
            for(std::size_t i=0;i<count;++i)if(first[i]!=second[i])return done(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<unsigned char>(first[i]))-static_cast<std::int32_t>(static_cast<unsigned char>(second[i]))));
            return done(static_cast<std::uint32_t>(static_cast<std::int32_t>(first.size())-static_cast<std::int32_t>(second.size())));
        }
        if(name=="RtlPrefixUnicodeString"){
            const bool fold=(arg(2)&0xffU)!=0;auto prefix=wide_text(header(arg(0))),text=wide_text(header(arg(1)));
            if(prefix.size()>text.size())return done(0);
            for(std::size_t i=0;i<prefix.size();++i){const auto a=fold?upper(prefix[i]):prefix[i],b=fold?upper(text[i]):text[i];if(a!=b)return done(0);}
            return done(1);
        }
        if(name=="RtlCompareUnicodeStrings"){
            // (PCWCH s1, SIZE_T len1, PCWCH s2, SIZE_T len2, BOOLEAN CaseInsensitive)：長度以字元計。
            const auto first=read_utf16_counted(memory,GuestAddress{arg(0)},static_cast<std::size_t>(std::min<std::uint64_t>(arg(1),32767)));
            const auto second=read_utf16_counted(memory,GuestAddress{arg(2)},static_cast<std::size_t>(std::min<std::uint64_t>(arg(3),32767)));
            const bool fold=(arg(4)&0xffU)!=0;const auto count=std::min(first.size(),second.size());
            for(std::size_t i=0;i<count;++i){const auto a=fold?upper(first[i]):first[i],b=fold?upper(second[i]):second[i];
                if(a!=b)return done(static_cast<std::uint32_t>(static_cast<std::int32_t>(a)-static_cast<std::int32_t>(b)));}
            return done(static_cast<std::uint32_t>(static_cast<std::int32_t>(first.size())-static_cast<std::int32_t>(second.size())));
        }
        if(name=="RtlHashUnicodeString"){
            // (String, CaseInSensitive, HashAlgorithm 0 預設／1 X65599, Hash*)：hash = hash × 65599 + 字元。
            if(!arg(0) || !arg(3))return done(status_invalid_parameter);
            if(u32(2)>1)return done(status_invalid_parameter);
            const bool fold=(arg(1)&0xffU)!=0;std::uint32_t hash=0;
            for(const auto c:wide_text(header(arg(0))))hash=hash*65599U+(fold?upper(c):c);
            memory.store(GuestAddress{arg(3)},32,hash);return done(0);
        }
        if(name=="RtlFindCharInUnicodeString"){
            // flags：1＝自尾端往前、2＝找不在集合內的字元、4＝不分大小寫。往前找到時位置為該字元之後的位元組偏移，往回找到時為該字元的位元組偏移。
            const auto flags=u32(0);if(flags&~7U)return done(status_invalid_parameter);
            const auto text=wide_text(header(arg(1)));auto set=wide_text(header(arg(2)));
            const bool fold=flags&4U,complement=flags&2U;if(fold)for(auto& c:set)c=upper(c);
            const auto member=[&](char16_t c){if(fold)c=upper(c);return (set.find(c)!=set.npos)!=complement;};
            memory.store(GuestAddress{arg(3)},16,0);
            if(flags&1U){for(std::size_t i=text.size();i-->0;)if(member(text[i])){memory.store(GuestAddress{arg(3)},16,i*2);return done(0);}}
            else for(std::size_t i=0;i<text.size();++i)if(member(text[i])){memory.store(GuestAddress{arg(3)},16,(i+1)*2);return done(0);}
            return done(status_not_found);
        }
        if(name=="RtlEraseUnicodeString"){
            const auto target=header(arg(0));if(target.buffer && target.capacity)memory.write(GuestAddress{target.buffer},std::vector<std::uint8_t>(target.capacity));
            set_length(arg(0),0);return done(0);
        }
        if(name=="RtlEqualComputerName" || name=="RtlEqualDomainName"){
            auto first=wide_text(header(arg(0))),second=wide_text(header(arg(1)));
            for(auto& c:first)c=upper(c);for(auto& c:second)c=upper(c);return done(first==second?1U:0U);
        }
        if(name=="RtlAnsiCharToUnicodeChar"){
            // (PUCHAR* Source)：解碼一個 UTF-8 字元並前移指標；無效位元組視為單一位元組並回 U+FFFD。
            const auto pointer=memory.load(GuestAddress{arg(0)},bits);const auto lead=static_cast<std::uint8_t>(memory.load(GuestAddress{pointer},8));
            const unsigned length=lead<0x80?1U:(lead>>5)==6?2U:(lead>>4)==14?3U:(lead>>3)==30?4U:1U;
            std::string bytes(length,'\0');for(unsigned i=0;i<length;++i)bytes[i]=static_cast<char>(memory.load(GuestAddress{pointer+i},8));
            const auto text=utf8_to_utf16_lossy(bytes);const auto consumed=text.size()==1 || lead<0x80?length:1U;
            memory.store(GuestAddress{arg(0)},bits,pointer+consumed);return done(text.empty()?0xfffdU:text[0]);
        }
        if(name.ends_with("StringToAnsiSize") || name.ends_with("StringToOemSize")){
            // UNICODE_STRING → 系統碼頁所需位元組數（含 NUL）。
            return done(utf16_to_utf8_lossy(wide_text(header(arg(0)))).size()+1);
        }
        if(name.ends_with("StringToUnicodeSize")){
            return done((utf8_to_utf16_lossy(narrow_text(header(arg(0)))).size()+1)*2);
        }
        if(name=="RtlOemStringToCountedUnicodeString" || name=="RtlUnicodeStringToCountedOemString" ||
           name=="RtlUpcaseUnicodeStringToCountedOemString"){
            const bool upcase=name=="RtlUpcaseUnicodeStringToCountedOemString";
            const auto source=header(arg(1));
            std::vector<std::uint8_t> data;
            if(name=="RtlOemStringToCountedUnicodeString"){
                data=bytes_of(utf8_to_utf16_lossy(narrow_text(source)));
            }else{
                auto text=wide_text(source);
                if(upcase)for(auto& c:text)c=upper(c);
                const auto encoded=utf16_to_utf8_lossy(text);
                data.assign(encoded.begin(),encoded.end());
            }
            const auto destination=arg(0);
            if(data.size()>UINT16_MAX)return done(0xc00000f0U); // STATUS_INVALID_PARAMETER_2
            memory.check(GuestAddress{destination},2*unit,Access::Write);
            if(data.empty() && !upcase){
                memory.write(GuestAddress{destination},std::vector<std::uint8_t>(2*unit));
                return done(0);
            }
            const bool allocate=byte(2)!=0;
            set_length(destination,data.size());
            if(allocate){
                memory.store(GuestAddress{destination+2},16,data.size());
                memory.store(GuestAddress{destination+unit},bits,0);
            }else{
                const auto target=header(destination);
                if(data.size()>target.capacity){
                    // 大寫版本會複製可容納的完整字元；一般版本只回報所需長度。
                    if(upcase){
                        std::size_t used=target.capacity;
                        while(used && (data[used]&0xc0U)==0x80U)--used;
                        if(used)memory.write(GuestAddress{target.buffer},std::span(data).first(used));
                        set_length(destination,target.capacity);
                    }
                    return done(status_buffer_overflow);
                }
            }
            return done(store_counted(destination,std::move(data),0,allocate));
        }
        if(name=="RtlUpcaseUnicodeStringToAnsiString" || name=="RtlUpcaseUnicodeStringToOemString"){
            auto text=wide_text(header(arg(1)));for(auto& c:text)c=upper(c);const auto converted=utf16_to_utf8_lossy(text);
            return done(store_counted(arg(0),std::vector<std::uint8_t>(converted.begin(),converted.end()),1,byte(2)!=0));
        }
        if(name=="RtlUpcaseUnicodeToMultiByteN" || name=="RtlUpcaseUnicodeToOemN"){
            // (Dest, DestBytes, BytesWritten*, Source, SourceBytes)：不附加 NUL，截斷在完整字元邊界。
            auto text=read_utf16_counted(memory,GuestAddress{arg(3)},u32(4)/2);for(auto& c:text)c=upper(c);
            auto converted=utf16_to_utf8_lossy(text);std::size_t written=std::min<std::size_t>(converted.size(),u32(1));
            if(written<converted.size())while(written && (static_cast<unsigned char>(converted[written])&0xc0U)==0x80U)--written;
            if(written)memory.write(GuestAddress{arg(0)},std::span(reinterpret_cast<const std::uint8_t*>(converted.data()),written));
            if(arg(2))memory.store(GuestAddress{arg(2)},32,written);return done(0);
        }
        if(name=="RtlIsNameLegalDOS8Dot3"){
            // (UNICODE_STRING* Name, OEM_STRING* OemName 可為 NULL, BOOLEAN* NameContainsSpaces)：檢查 8.3 名稱規則。
            const auto text=wide_text(header(arg(0)));const auto converted=[&]{std::u16string up=text;for(auto& c:up)c=upper(c);return utf16_to_utf8_lossy(up);}();
            if(arg(1)){const auto oem=header(arg(1));if(converted.size()<=oem.capacity){memory.write(GuestAddress{oem.buffer},std::span(reinterpret_cast<const std::uint8_t*>(converted.data()),converted.size()));set_length(arg(1),converted.size());}}
            bool spaces=false,legal=!converted.empty() && converted.size()<=12;
            if(converted=="." || converted==".."){if(arg(2))memory.store(GuestAddress{arg(2)},8,0);return done(1);}
            const auto dot=converted.find('.');
            if(legal){
                const auto base=converted.substr(0,dot),extension=dot==std::string::npos?std::string{}:converted.substr(dot+1);
                legal=!base.empty() && base.size()<=8 && extension.size()<=3 && (dot==std::string::npos || (!extension.empty() && extension.find('.')==std::string::npos));
                for(const auto c:base+extension){
                    if(static_cast<unsigned char>(c)<0x20 || std::string_view("\"*+,/:;<=>?[\\]|").find(c)!=std::string_view::npos)legal=false;
                    if(c==' ')spaces=true;
                }
                if(legal && (base.back()==' ' || (!extension.empty() && extension.back()==' ')))legal=false;
            }
            if(arg(2))memory.store(GuestAddress{arg(2)},8,spaces?1U:0U);return done(legal?1U:0U);
        }
        if(name=="RtlIntegerToChar" || name=="RtlIntegerToUnicodeString" || name=="RtlInt64ToUnicodeString" || name=="RtlLargeIntegerToChar"){
            const bool large=name=="RtlLargeIntegerToChar",wide64=name=="RtlInt64ToUnicodeString";
            const std::uint64_t value=large?memory.load(GuestAddress{arg(0)},64):wide64?arg(0):u32(0);
            auto base=u32(1);if(!base)base=10;if(base!=2 && base!=8 && base!=10 && base!=16)return done(status_invalid_parameter);
            std::string digits;auto v=value;do{digits.insert(digits.begin(),"0123456789ABCDEF"[v%base]);v/=base;}while(v);
            if(name.ends_with("Char")){
                // (Value, Base, Length, PCHAR)：Length 為輸出容量；位數超過時回 STATUS_BUFFER_OVERFLOW，有空間時附加 NUL。
                const auto capacity=static_cast<std::int32_t>(u32(2));
                if(capacity>=0 && digits.size()>static_cast<std::uint32_t>(capacity))return done(status_buffer_overflow);
                memory.write(GuestAddress{arg(3)},std::span(reinterpret_cast<const std::uint8_t*>(digits.data()),digits.size()));
                if(capacity<0 || digits.size()<static_cast<std::uint32_t>(capacity))memory.store(GuestAddress{arg(3)+digits.size()},8,0);
                return done(0);
            }
            const auto target=header(arg(2));std::u16string text(digits.begin(),digits.end());
            if(text.size()*2>target.capacity)return done(status_buffer_overflow);
            auto data=bytes_of(text);if(data.size()+2<=target.capacity){data.push_back(0);data.push_back(0);}
            memory.write(GuestAddress{target.buffer},data);set_length(arg(2),text.size()*2);return done(0);
        }
        if(name=="RtlCharToInteger" || name=="RtlUnicodeStringToInteger"){
            // 略過前導空白、接受 +／-；Base 為 0 時依 0x／0o／0b 前綴決定，預設十進位；遇到第一個無效字元即停止。
            std::u16string text;
            if(name=="RtlCharToInteger"){if(!arg(0))return done(status_access_violation);const auto source=read_ansi(memory,arg(0),65535);text.assign(source.begin(),source.end());}
            else text=wide_text(header(arg(0)));
            auto base=u32(1);if(base!=0 && base!=2 && base!=8 && base!=10 && base!=16)return done(status_invalid_parameter);
            if(!arg(2))return done(status_access_violation);
            std::size_t at=0;while(at<text.size() && text[at]<=u' ')++at;
            bool negative=false;if(at<text.size() && (text[at]==u'+' || text[at]==u'-')){negative=text[at]==u'-';++at;}
            if(!base){
                base=10;
                if(at+1<text.size() && text[at]==u'0'){const auto p=text[at+1];if(p==u'x' || p==u'X'){base=16;at+=2;}else if(p==u'o' || p==u'O'){base=8;at+=2;}else if(p==u'b' || p==u'B'){base=2;at+=2;}}
            }
            std::uint32_t value=0;
            for(;at<text.size();++at){const auto c=text[at];unsigned digit=99;
                if(c>=u'0' && c<=u'9')digit=c-u'0';else if(c>=u'a' && c<=u'f')digit=c-u'a'+10;else if(c>=u'A' && c<=u'F')digit=c-u'A'+10;
                if(digit>=base)break;value=value*base+digit;}
            memory.store(GuestAddress{arg(2)},32,negative?0U-value:value);return done(0);
        }
        if(name=="RtlGUIDFromString"){
            // 只接受 {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}。
            const auto text=wide_text(header(arg(0)));
            if(text.size()!=38 || text[0]!=u'{' || text[37]!=u'}' || text[9]!=u'-' || text[14]!=u'-' || text[19]!=u'-' || text[24]!=u'-')return done(status_invalid_parameter);
            const auto hex=[&](std::size_t at,unsigned count,std::uint64_t& value){value=0;for(unsigned i=0;i<count;++i){const auto c=text[at+i];unsigned d;
                if(c>=u'0' && c<=u'9')d=c-u'0';else if(c>=u'a' && c<=u'f')d=c-u'a'+10;else if(c>=u'A' && c<=u'F')d=c-u'A'+10;else return false;value=value*16+d;}return true;};
            std::uint64_t data1{},data2{},data3{};std::array<std::uint64_t,8> data4{};
            bool ok=hex(1,8,data1) && hex(10,4,data2) && hex(15,4,data3) && hex(20,2,data4[0]) && hex(22,2,data4[1]);
            for(unsigned i=0;i<6 && ok;++i)ok=hex(25+i*2,2,data4[2+i]);
            if(!ok)return done(status_invalid_parameter);
            memory.store(GuestAddress{arg(1)},32,data1);memory.store(GuestAddress{arg(1)+4},16,data2);memory.store(GuestAddress{arg(1)+6},16,data3);
            for(unsigned i=0;i<8;++i)memory.store(GuestAddress{arg(1)+8+i},8,data4[i]);return done(0);
        }
        if(name=="RtlStringFromGUID"){
            // 配置 {…} 文字（含 NUL），由 RtlFreeUnicodeString 釋放。
            std::string text="{";const auto put=[&](std::uint64_t value,unsigned digits){for(unsigned i=digits;i-->0;)text.push_back("0123456789ABCDEF"[(value>>(i*4))&15U]);};
            put(memory.load(GuestAddress{arg(0)},32),8);text+='-';put(memory.load(GuestAddress{arg(0)+4},16),4);text+='-';put(memory.load(GuestAddress{arg(0)+6},16),4);text+='-';
            for(unsigned i=0;i<8;++i){if(i==2)text+='-';put(memory.load(GuestAddress{arg(0)+8+i},8),2);}text+='}';
            return done(store_counted(arg(1),bytes_of(std::u16string(text.begin(),text.end())),2,true));
        }
        if(name=="RtlInitializeBitMap"){memory.store(GuestAddress{arg(0)},32,u32(2));memory.store(GuestAddress{arg(0)+unit},bits,arg(1));return done(0);}
        if(name=="RtlSetBits" || name=="RtlClearBits"){
            const auto map=bitmap(arg(0));if(static_cast<std::uint64_t>(u32(1))+u32(2)>map.size)return done(0);
            put_bits(map,u32(1),u32(2),name=="RtlSetBits");return done(0);
        }
        if(name=="RtlSetAllBits" || name=="RtlClearAllBits"){
            const auto map=bitmap(arg(0));const auto words=(map.size+31)/32;
            memory.write(GuestAddress{map.buffer},std::vector<std::uint8_t>(words*4,name=="RtlSetAllBits"?0xffU:0U));return done(0);
        }
        if(name=="RtlAreBitsSet" || name=="RtlAreBitsClear"){
            const auto map=bitmap(arg(0));const auto start=u32(1),count=u32(2);
            if(!count || static_cast<std::uint64_t>(start)+count>map.size)return done(0);
            return done(range_is(map,start,count,name=="RtlAreBitsSet")?1U:0U);
        }
        if(name=="RtlFindSetBits" || name=="RtlFindClearBits" || name=="RtlFindSetBitsAndClear" || name=="RtlFindClearBitsAndSet"){
            const auto map=bitmap(arg(0));const bool want_set=name.starts_with("RtlFindSet");
            const auto found=find_bits(map,u32(1),u32(2),want_set);
            if(found!=UINT32_MAX && name.find("And")!=name.npos)put_bits(map,found,u32(1),!want_set); // 找到後一併翻轉
            return done(found);
        }
        if(name=="RtlNumberOfSetBits" || name=="RtlNumberOfClearBits"){
            const auto map=bitmap(arg(0));std::uint32_t count=0;
            for(std::uint32_t i=0;i<map.size;){
                if(i%8==0 && map.size-i>=8){count+=static_cast<std::uint32_t>(std::popcount(static_cast<std::uint8_t>(memory.load(GuestAddress{map.buffer+i/8},8))));i+=8;}
                else {count+=test_bit(map,i)?1U:0U;++i;}
            }
            return done(name=="RtlNumberOfSetBits"?count:map.size-count);
        }
        if(name=="RtlFindLongestRunSet" || name=="RtlFindLongestRunClear"){
            const auto list=runs(bitmap(arg(0)),name.ends_with("Set"));Run best{0,0};
            for(const auto& run:list)if(run.length>best.length)best=run;
            memory.store(GuestAddress{arg(1)},32,best.start);return done(best.length);
        }
        if(name=="RtlFindNextForwardRunSet" || name=="RtlFindNextForwardRunClear"){
            const auto map=bitmap(arg(0));const bool value=name.ends_with("Set");auto i=u32(1);
            while(i<map.size && static_cast<bool>(test_bit(map,i))!=value)++i;
            const auto start=i;while(i<map.size && static_cast<bool>(test_bit(map,i))==value)++i;
            memory.store(GuestAddress{arg(2)},32,start<map.size?start:0);return done(start<map.size?i-start:0);
        }
        if(name=="RtlFindLastBackwardRunSet" || name=="RtlFindLastBackwardRunClear"){
            const auto map=bitmap(arg(0));const bool value=name.ends_with("Set");
            std::int64_t i=std::min<std::int64_t>(u32(1),static_cast<std::int64_t>(map.size)-1);
            while(i>=0 && static_cast<bool>(test_bit(map,static_cast<std::uint32_t>(i)))!=value)--i;
            if(i<0){memory.store(GuestAddress{arg(2)},32,0);return done(0);}
            const auto end=i;while(i>=0 && static_cast<bool>(test_bit(map,static_cast<std::uint32_t>(i)))==value)--i;
            memory.store(GuestAddress{arg(2)},32,static_cast<std::uint64_t>(i+1));return done(static_cast<std::uint64_t>(end-i));
        }
        if(name=="RtlFindSetRuns" || name=="RtlFindClearRuns"){
            // (BitMap, RTL_BITMAP_RUN* Runs, ULONG Count, BOOLEAN LocateLongest)：LocateLongest 時依長度遞減。
            auto list=runs(bitmap(arg(0)),name=="RtlFindSetRuns");
            if(arg(3)&0xffU)std::stable_sort(list.begin(),list.end(),[](const Run& a,const Run& b){return a.length>b.length;});
            const auto count=std::min<std::size_t>(list.size(),u32(2));
            for(std::size_t i=0;i<count;++i){memory.store(GuestAddress{arg(1)+i*8},32,list[i].start);memory.store(GuestAddress{arg(1)+i*8+4},32,list[i].length);}
            return done(count);
        }
        if(name=="RtlFindMostSignificantBit" || name=="RtlFindLeastSignificantBit"){
            const auto value=arg(0);if(!value)return done(0xffU);
            return done(static_cast<std::uint8_t>(name=="RtlFindMostSignificantBit"?63-std::countl_zero(value):std::countr_zero(value)));
        }
        if(name=="RtlCopyLuid"){memory.store(GuestAddress{arg(0)},64,memory.load(GuestAddress{arg(1)},64));return done(0);}
        if(name=="RtlEqualLuid")return done(memory.load(GuestAddress{arg(0)},64)==memory.load(GuestAddress{arg(1)},64)?1U:0U);
        if(name=="RtlCopyLuidAndAttributesArray"){
            const auto count=u32(0);std::vector<std::uint8_t> data(static_cast<std::size_t>(count)*12);
            if(count){memory.read(GuestAddress{arg(1)},data);memory.write(GuestAddress{arg(2)},data);}return done(0);
        }
        if(name=="RtlTimeToTimeFields" || name=="RtlTimeToElapsedTimeFields"){
            // TIME_FIELDS {Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday}（各 CSHORT）。
            const auto time=memory.load(GuestAddress{arg(0)},64);const auto days=time/ticks_per_day,rest=time%ticks_per_day;
            std::array<std::uint16_t,8> fields{};
            fields[3]=static_cast<std::uint16_t>(rest/(3600*ticks_per_second));fields[4]=static_cast<std::uint16_t>(rest/(60*ticks_per_second)%60);
            fields[5]=static_cast<std::uint16_t>(rest/ticks_per_second%60);fields[6]=static_cast<std::uint16_t>(rest%ticks_per_second/10000);
            if(name=="RtlTimeToTimeFields"){
                std::uint64_t year=0;unsigned month=0,day=0;civil_from_days(days,year,month,day);
                fields[0]=static_cast<std::uint16_t>(year);fields[1]=static_cast<std::uint16_t>(month);fields[2]=static_cast<std::uint16_t>(day);
                fields[7]=static_cast<std::uint16_t>((days+1)%7); // 1601-01-01 是星期一
            }else {fields[2]=static_cast<std::uint16_t>(days);}
            for(unsigned i=0;i<8;++i)memory.store(GuestAddress{arg(1)+i*2},16,fields[i]);return done(0);
        }
        if(name=="RtlTimeFieldsToTime"){
            std::array<std::int16_t,8> fields{};for(unsigned i=0;i<8;++i)fields[i]=static_cast<std::int16_t>(memory.load(GuestAddress{arg(0)+i*2},16));
            if(fields[3]<0 || fields[3]>23 || fields[4]<0 || fields[4]>59 || fields[5]<0 || fields[5]>59 || fields[6]<0 || fields[6]>999 || fields[0]<0 || fields[1]<0 || fields[2]<0)return done(0);
            const auto days=days_from_civil(static_cast<std::uint64_t>(fields[0]),static_cast<unsigned>(fields[1]),static_cast<unsigned>(fields[2]));if(!days)return done(0);
            const auto time=*days*ticks_per_day+(static_cast<std::uint64_t>(fields[3])*3600+static_cast<std::uint64_t>(fields[4])*60+static_cast<std::uint64_t>(fields[5]))*ticks_per_second+static_cast<std::uint64_t>(fields[6])*10000;
            memory.store(GuestAddress{arg(1)},64,time);return done(1);
        }
        if(name=="RtlTimeToSecondsSince1970" || name=="RtlTimeToSecondsSince1980"){
            const auto offset=name.ends_with("1970")?seconds_1601_to_1970:seconds_1601_to_1980;const auto seconds=memory.load(GuestAddress{arg(0)},64)/ticks_per_second;
            if(seconds<offset || seconds-offset>UINT32_MAX)return done(0);
            memory.store(GuestAddress{arg(1)},32,seconds-offset);return done(1);
        }
        if(name=="RtlSecondsSince1970ToTime" || name=="RtlSecondsSince1980ToTime"){
            const auto offset=name.ends_with("1970ToTime")?seconds_1601_to_1970:seconds_1601_to_1980;
            memory.store(GuestAddress{arg(1)},64,(u32(0)+offset)*ticks_per_second);return done(0);
        }
        if(name=="RtlUniform"){auto seed=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));const auto value=uniform_next(seed);memory.store(GuestAddress{arg(0)},32,seed);return done(value);}
        if(name=="RtlRandom" || name=="RtlRandomEx"){
            // 以 RtlUniform 為底、128 格洗牌表打散序列；洗牌表為本程序共用狀態。
            auto& shuffle=rtl_random_state_;
            if(!shuffle.ready){std::uint32_t init=0x5ee8u;for(auto& cell:shuffle.table)cell=uniform_next(init);shuffle.last=uniform_next(init);shuffle.ready=true;}
            auto seed=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));
            const auto index=shuffle.last%shuffle.table.size();const auto result=shuffle.table[index];
            shuffle.table[index]=uniform_next(seed);shuffle.last=result;memory.store(GuestAddress{arg(0)},32,seed);return done(result);
        }
        if(name.starts_with("MD4") || name.starts_with("MD5") || name.starts_with("A_SHA")){
            // 客體結構：MD4_CTX {ULONG buf[4]; ULONG i[2]; UCHAR in[64]; UCHAR digest[16]}、MD5_CTX {ULONG i[2]; ULONG buf[4]; UCHAR in[64]; UCHAR digest[16]}，
            // i 為以位元計的長度（低、高）；SHA_CTX {ULONG Unknown[6]; ULONG State[5]; ULONG Count[2]; UCHAR Buffer[64]}，Count 為位元組數（低、高）。
            // 狀態全部存在客體結構，複製結構後可各自繼續；MD4Final／MD5Final 把摘要寫入 digest，A_SHAFinal 寫入第二個參數並重新初始化。
            const bool sha=name.starts_with("A_SHA"),md4=name.starts_with("MD4");const auto context=arg(0);
            const std::uint64_t state_at=sha?24:md4?0:8,count_at=sha?44:md4?16:0,buffer_at=sha?52:24;const unsigned words=sha?5:4;
            std::array<std::uint32_t,5> chain{};for(unsigned i=0;i<words;++i)chain[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{context+state_at+i*4},32));
            auto count=memory.load(GuestAddress{context+count_at},32)|(memory.load(GuestAddress{context+count_at+4},32)<<32);
            std::array<std::uint8_t,64> buffer{};memory.read(GuestAddress{context+buffer_at},buffer);
            const auto bytes=[&]{return sha?count:count/8;};
            const auto compress=[&](const std::uint8_t* block){
                if(sha){std::array<std::uint32_t,5> s=chain;digest::sha1_block(s,block);chain=s;}
                else{std::array<std::uint32_t,4> s{chain[0],chain[1],chain[2],chain[3]};if(md4)digest::md4_block(s,block);else digest::md5_block(s,block);std::copy(s.begin(),s.end(),chain.begin());}};
            const auto absorb=[&](std::span<const std::uint8_t> data){
                for(const auto value:data){const auto index=static_cast<std::size_t>(bytes()%64);buffer[index]=value;count+=sha?1:8;if(index==63)compress(buffer.data());}};
            const auto store_state=[&]{
                for(unsigned i=0;i<words;++i)memory.store(GuestAddress{context+state_at+i*4},32,chain[i]);
                memory.store(GuestAddress{context+count_at},32,count&0xffffffffU);memory.store(GuestAddress{context+count_at+4},32,count>>32);memory.write(GuestAddress{context+buffer_at},buffer);};
            if(name.ends_with("Init")){
                if(sha)std::copy(digest::sha1_initial.begin(),digest::sha1_initial.end(),chain.begin());else std::copy(digest::md_initial.begin(),digest::md_initial.end(),chain.begin());
                count=0;buffer.fill(0);store_state();return done(0);
            }
            if(name.ends_with("Update")){
                // 長輸入分段讀取，避免一次配置整段客體資料。
                auto source=arg(1);auto remaining=static_cast<std::uint64_t>(u32(2));std::vector<std::uint8_t> chunk;
                while(remaining){const auto size=static_cast<std::size_t>(std::min<std::uint64_t>(remaining,65536));chunk.resize(size);memory.read(GuestAddress{source},chunk);absorb(chunk);source+=size;remaining-=size;}
                store_state();return done(0);
            }
            // Final：補 0x80、填 0 到 56 位元組，再附上位元長度（MD4／MD5 為小端序，SHA-1 為大端序）。
            const auto length_bits=sha?count*8:count;
            std::array<std::uint8_t,72> tail{};tail[0]=0x80;const auto used=static_cast<std::size_t>(bytes()%64);const auto pad=(used<56?56:120)-used;
            for(unsigned i=0;i<8;++i)tail[pad+i]=static_cast<std::uint8_t>(sha?length_bits>>(56-8*i):length_bits>>(8*i));
            absorb(std::span<const std::uint8_t>(tail).first(pad+8));
            std::array<std::uint8_t,20> result{};
            for(unsigned i=0;i<words;++i)for(unsigned k=0;k<4;++k)result[i*4+k]=static_cast<std::uint8_t>(sha?chain[i]>>(24-8*k):chain[i]>>(8*k));
            if(sha){memory.write(GuestAddress{arg(1)},std::span<const std::uint8_t>(result).first(20));
                std::copy(digest::sha1_initial.begin(),digest::sha1_initial.end(),chain.begin());count=0;buffer.fill(0);}
            else memory.write(GuestAddress{context+88},std::span<const std::uint8_t>(result).first(16));
            store_state();return done(0);
        }
        if(name=="RtlInitCodePageTable" || name=="RtlCustomCPToUnicodeN" || name=="RtlUnicodeToCustomCPN" || name=="RtlUpcaseUnicodeToCustomCPN"){
            // CPTABLEINFO {USHORT CodePage, MaximumCharacterSize, DefaultChar, UniDefaultChar, TransDefaultChar, TransUniDefaultChar, DBCSCodePage; UCHAR LeadByte[12];
            //              PUSHORT MultiByteTable; PVOID WideCharTable; PUSHORT DBCSRanges; PUSHORT DBCSOffsets}；指標從 28（x86）或 32（x64）位元組開始。
            const std::uint64_t pointers=x86_?28:32;const auto info=arg(0);
            const auto word=[&](std::uint64_t address){return static_cast<std::uint16_t>(memory.load(GuestAddress{address},16));};
            const auto pointer=[&](unsigned index){return memory.load(GuestAddress{info+pointers+index*unit},bits);};
            if(name=="RtlInitCodePageTable"){
                // NLS 碼頁檔：表頭（字數、碼頁、最大字元長度、四個預設字元、12 個前導位元組），接著 [到 WideCharTable 的字數] MultiByteTable[256]、
                // [字形表旗標（非 0 時接 256 項）]、[DBCS 範圍數] 與各前導位元組的位移表；UTF-8（65001）沒有表格。
                const auto base=arg(0),out=arg(1);
                for(std::uint64_t offset=0;offset<pointers+4*unit;offset+=2)memory.store(GuestAddress{out+offset},16,0);
                if(word(base+2)==65001){
                    const std::array<std::uint16_t,6> fields{65001,4,'?',0xfffd,'?','?'};for(unsigned i=0;i<fields.size();++i)memory.store(GuestAddress{out+i*2},16,fields[i]);return done(0);
                }
                for(unsigned i=1;i<=6;++i)memory.store(GuestAddress{out+(i-1)*2},16,word(base+i*2));
                std::array<std::uint8_t,12> lead{};memory.read(GuestAddress{base+14},lead);memory.write(GuestAddress{out+14},lead);
                auto at=base+std::uint64_t{word(base)}*2;const auto wide=at+(std::uint64_t{word(at)}+1)*2;const auto multi=at+2;at=multi+512;
                // 字形表旗標之後（有字形表時再跳過 256 項）為 DBCS 範圍數。
                const auto glyph=at;at=glyph+2;if(word(glyph))at+=512;const auto ranges=at;const bool dbcs=word(ranges)!=0;
                memory.store(GuestAddress{out+12},16,dbcs?1:0);
                memory.store(GuestAddress{out+pointers},bits,multi);memory.store(GuestAddress{out+pointers+unit},bits,wide);
                memory.store(GuestAddress{out+pointers+2*unit},bits,ranges);memory.store(GuestAddress{out+pointers+3*unit},bits,dbcs?ranges+2:0);
                return done(0);
            }
            // 轉換：(info, 目的, 目的位元組上限, 寫入位元組數（可為 NULL）, 來源, 來源位元組數)；輸出不足時截斷，與 RtlMultiByteToUnicodeN 相同回傳 STATUS_SUCCESS。
            const auto destination=arg(1),capacity=static_cast<std::uint64_t>(u32(2)),count_pointer=arg(3),source=arg(4),length=static_cast<std::uint64_t>(u32(5));
            if(length>output_limit)return done(status_no_memory);
            const bool utf8=word(info)==65001,dbcs=word(info+12)!=0;const auto multi=pointer(0),wide=pointer(1),offsets=pointer(3);
            std::vector<std::uint8_t> out;
            if(name=="RtlCustomCPToUnicodeN"){
                std::vector<std::uint8_t> input(static_cast<std::size_t>(length));memory.read(GuestAddress{source},input);
                if(utf8){auto text=utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(input.data()),input.size()));out=utf16_payload(text);
                    out.resize(std::min<std::size_t>(out.size(),static_cast<std::size_t>(capacity&~std::uint64_t{1})));
                    if(out.size()>=2 && out.size()<text.size()*2){const auto last=static_cast<unsigned>(out[out.size()-2])|(static_cast<unsigned>(out[out.size()-1])<<8);if(last>=0xd800 && last<=0xdbff)out.resize(out.size()-2);}}
                else for(std::size_t i=0;i<input.size() && out.size()+2<=capacity;++i){
                    std::uint16_t value=word(multi+std::uint64_t{input[i]}*2);
                    if(dbcs){const auto lead=word(offsets+std::uint64_t{input[i]}*2);if(lead && i+1<input.size())value=word(offsets+(std::uint64_t{lead}+input[++i])*2);}
                    out.push_back(static_cast<std::uint8_t>(value));out.push_back(static_cast<std::uint8_t>(value>>8));
                }
            }else{
                auto text=read_utf16_counted(memory,GuestAddress{source},static_cast<std::size_t>(length/2));
                if(name.starts_with("RtlUpcase"))for(auto& c:text)c=upper(c);
                if(utf8){const auto narrow=utf16_to_utf8_lossy(text);out.assign(narrow.begin(),narrow.end());
                    if(out.size()>capacity){std::size_t keep=static_cast<std::size_t>(capacity);while(keep && (out[keep]&0xc0U)==0x80U)--keep;out.resize(keep);}}
                else for(const auto c:text){
                    if(!dbcs){if(out.size()+1>capacity)break;out.push_back(static_cast<std::uint8_t>(memory.load(GuestAddress{wide+c},8)));continue;}
                    const auto value=word(wide+std::uint64_t{c}*2);
                    if(value&0xff00U){if(out.size()+2>capacity)break;out.push_back(static_cast<std::uint8_t>(value>>8));out.push_back(static_cast<std::uint8_t>(value));}
                    else{if(out.size()+1>capacity)break;out.push_back(static_cast<std::uint8_t>(value));}
                }
            }
            if(!out.empty())memory.write(GuestAddress{destination},out);
            if(count_pointer)memory.store(GuestAddress{count_pointer},32,out.size());
            return done(0);
        }
        if(name=="RtlComputeCrc32"){
            // (ULONG InitialCrc, PUCHAR Buffer, INT Length)：IEEE 802.3 CRC-32（反射多項式 0xEDB88320），可串接計算。
            const auto length=static_cast<std::int32_t>(u32(2));std::uint32_t crc=~u32(0);
            if(length>0){std::vector<std::uint8_t> data(static_cast<std::size_t>(length));memory.read(GuestAddress{arg(1)},data);
                const auto& table=crc32_table();for(const auto b:data)crc=table[(crc^b)&0xffU]^(crc>>8);}
            return done(~crc);
        }
        // PE 映像：只讀客體記憶體，驗證 MZ／PE 簽章。
        const auto nt_header=[&](std::uint64_t base)->std::uint64_t{
            if(!base || memory.load(GuestAddress{base},16)!=0x5a4dU)return 0;
            const auto offset=memory.load(GuestAddress{base+0x3c},32);if(offset>0x10000000U)return 0;
            return memory.load(GuestAddress{base+offset},32)==0x4550U?base+offset:0;
        };
        const auto sections=[&](std::uint64_t nt){
            struct Section {std::uint64_t header;std::uint32_t address,size,raw,raw_size;};std::vector<Section> list;
            const auto count=memory.load(GuestAddress{nt+6},16),optional=memory.load(GuestAddress{nt+20},16);
            const auto first=nt+24+optional;
            for(std::uint64_t i=0;i<count;++i){const auto at=first+i*40;
                list.push_back({at,static_cast<std::uint32_t>(memory.load(GuestAddress{at+12},32)),static_cast<std::uint32_t>(std::max(memory.load(GuestAddress{at+8},32),memory.load(GuestAddress{at+16},32))),
                    static_cast<std::uint32_t>(memory.load(GuestAddress{at+20},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{at+16},32))});}
            return list;
        };
        const auto section_for=[&](std::uint64_t nt,std::uint32_t rva)->std::uint64_t{
            for(const auto& section:sections(nt))if(rva>=section.address && rva-section.address<section.size)return section.header;return 0;};
        if(name=="RtlImageNtHeader")return done(nt_header(arg(0)));
        if(name=="RtlImageRvaToSection")return done(arg(0)?section_for(arg(0),u32(2)):0);
        if(name=="RtlImageRvaToVa" || name=="RtlAddressInSectionTable"){
            // (NtHeaders, Base, Rva[, LastRvaSection*])：檔案佈局下的位址＝Base＋Rva－區段 VA＋PointerToRawData。
            if(!arg(0))return done(0);
            std::uint64_t section=0;
            if(name=="RtlImageRvaToVa" && arg(3)){section=memory.load(GuestAddress{arg(3)},bits);
                if(section){const auto va=memory.load(GuestAddress{section+12},32),extent=memory.load(GuestAddress{section+8},32);if(u32(2)<va || u32(2)-va>=std::max<std::uint64_t>(extent,memory.load(GuestAddress{section+16},32)))section=0;}}
            if(!section)section=section_for(arg(0),u32(2));if(!section)return done(0);
            if(name=="RtlImageRvaToVa" && arg(3))memory.store(GuestAddress{arg(3)},bits,section);
            return done(arg(1)+u32(2)-memory.load(GuestAddress{section+12},32)+memory.load(GuestAddress{section+20},32));
        }
        if(name=="RtlImageDirectoryEntryToData"){
            // (Base, BOOLEAN MappedAsImage, USHORT Directory, PULONG Size)：未映射（檔案佈局）時換算成檔案偏移。
            const auto base=arg(0)&~std::uint64_t{3};const auto nt=nt_header(base);if(!nt)return done(0);
            const auto magic=memory.load(GuestAddress{nt+24},16);const auto pe32=magic==0x10b;if(!pe32 && magic!=0x20b)return done(0);
            const auto directories=nt+24+(pe32?96U:112U);const auto count=memory.load(GuestAddress{nt+24+(pe32?92U:108U)},32);
            const auto index=static_cast<std::uint16_t>(arg(2));if(index>=count)return done(0);
            const auto rva=static_cast<std::uint32_t>(memory.load(GuestAddress{directories+index*8},32));const auto size=memory.load(GuestAddress{directories+index*8+4},32);
            if(!rva)return done(0);
            if(arg(3))memory.store(GuestAddress{arg(3)},32,size);
            if((arg(1)&0xffU) && !(arg(0)&1U))return done(base+rva);
            const auto section=section_for(nt,rva);if(!section)return done(0);
            return done(base+rva-memory.load(GuestAddress{section+12},32)+memory.load(GuestAddress{section+20},32));
        }
        if(name=="RtlGetNtProductType"){
            // 與 GetVersionEx 的 wProductType（VER_NT_WORKSTATION）一致：NtProductWinNt＝1。
            if(arg(0))memory.store(GuestAddress{arg(0)},32,1);return done(1);
        }
        if(name=="RtlGetNtGlobalFlags")return done(memory.load(GuestAddress{peb_base_.value+(x86_?0x68U:0xbcU)},32));
        if(name=="RtlIsCurrentProcess"){
            const auto handle=arg(0);if(handle==(x86_?UINT32_MAX:UINT64_MAX))return done(1);
            return done(process_handle_pid(handle,0)==process_id()?1U:0U);
        }
        if(name=="RtlIsCurrentThread"){
            const auto handle=arg(0);if(handle==(x86_?UINT32_MAX-1U:UINT64_MAX-1U))return done(1);
            return done(thread_id(handle)==threads_.current?1U:0U);
        }
        if(name=="RtlSetLastWin32ErrorAndNtStatusFromNtStatus"){
            const auto error=nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{u32(0)});
            threads_.items.at(threads_.current).last_native_status=u32(0);last_error_=static_cast<std::uint32_t>(error);return done(0);
        }
    }catch(const std::bad_alloc&){return done(status_no_memory);}
    throw HostCallFault("未知的 ntdll 純運算服務");
}
}
