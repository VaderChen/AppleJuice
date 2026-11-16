// STL 的 C 層輔助匯出（xmath／xlocinfo／xstod 系列），msvcp80～msvcp140 共用：
// <complex> 的 _Cosh／_Sinh／_Exp／_Dtest／_Dscale、locale 向量 _Getctype／_Getcvt／_Getcoll、_Getdateorder、
// 寬字元分類 _Getwctype(s)、大小寫 _Tolower／_Toupper／_Towlower／_Towupper、_Wcrtomb、數值轉換 _Sto*，以及 msvcp140 的 __crt* 包裝與 _To_wide／_To_byte。
// VC8～VC10 的 _Ctypevec／_Cvtvec／_Collvec 以 LCID 識別 locale，VC11 起改以 locale 名稱；兩者共用同一套語義。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_locale_data.hpp"
#include "windows_math_environment.hpp"
#include "msvcp_math.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

namespace x64arm {
namespace {
// 名稱、引數槽數、Win32 堆疊上 64 位元引數遮罩、提供的版本（bit 0：VC9／VC10，bit 1：VC11／VC12，bit 2：msvcp140）。
// msvcp140 的 _Getctype／_Getcvt 與 shared_ptr 自旋鎖仍在 msvcp100 流程，這裡只登錄其他版本。
struct RuntimeEntry {std::string_view name;std::uint8_t arguments,wide,versions;};
constexpr RuntimeEntry runtime_entries[]{
    {"_Cosh",2,3,7},{"_LCosh",2,3,7},{"_FCosh",2,0,7},
    {"_Sinh",2,3,7},{"_LSinh",2,3,7},{"_FSinh",2,0,7},
    {"_Exp",3,2,7},{"_LExp",3,2,4},{"_FExp",3,0,7},
    {"_Dtest",1,0,7},{"_LDtest",1,0,7},{"_FDtest",1,0,7},
    {"_Dscale",2,0,3},{"_LDscale",2,0,3},{"_FDscale",2,0,3},
    {"_Getctype",1,0,3},{"_Getcvt",0,0,3},{"_Getcoll",1,0,7},{"_Getdateorder",0,0,7},
    {"_Getwctype",2,0,7},{"_Getwctypes",4,0,7},
    {"_Tolower",2,0,7},{"_Toupper",2,0,7},{"_Towlower",2,0,7},{"_Towupper",2,0,7},
    {"_Wcrtomb",4,0,7},
    {"_Stod",3,0,7},{"_Stodx",4,0,7},{"_Stof",3,0,7},{"_Stofx",4,0,7},
    {"_Stold",3,0,7},{"_Stoldx",4,0,7},{"_Stolx",4,0,7},{"_Stoul",3,0,7},{"_Stoulx",4,0,7},
    {"_Stoll",3,0,7},{"_Stollx",4,0,7},{"_Stoull",3,0,7},{"_Stoullx",4,0,7},
};
// msvcp80 由 msvcp90 家族提供（builtin_provider_family），不另行登錄。
constexpr std::string_view runtime_families[]{"msvcp90.dll","msvcp100.dll","msvcp110.dll","msvcp120.dll","msvcp140.dll"};
unsigned runtime_version(std::string_view family){return family=="msvcp140.dll"?4U:family=="msvcp110.dll" || family=="msvcp120.dll"?2U:1U;}
// msvcp140 的 __crt* 是 cdecl 包裝，Vista 起直接呼叫同名 Win32 API（kernel32 為 stdcall，x86 不能直接設為別名）；
// 名稱、引數數與目標 API。NLS 包裝另補上 CompareStringEx／LCMapStringEx 的保留引數。
struct CrtWrapper {std::string_view name;std::uint8_t arguments;std::string_view target;};
constexpr CrtWrapper crt_wrappers[]{
    {"__crtCloseThreadpoolTimer",1,"CloseThreadpoolTimer"},{"__crtCloseThreadpoolWait",1,"CloseThreadpoolWait"},
    {"__crtCreateEventExW",4,"CreateEventExW"},{"__crtCreateSemaphoreExW",6,"CreateSemaphoreExW"},
    {"__crtCreateThreadpoolTimer",3,"CreateThreadpoolTimer"},{"__crtCreateThreadpoolWait",3,"CreateThreadpoolWait"},
    {"__crtFlushProcessWriteBuffers",0,"FlushProcessWriteBuffers"},{"__crtFreeLibraryWhenCallbackReturns",2,"FreeLibraryWhenCallbackReturns"},
    {"__crtGetCurrentProcessorNumber",0,"GetCurrentProcessorNumber"},{"__crtGetTickCount64",0,"GetTickCount64"},
    {"__crtInitializeCriticalSectionEx",3,"InitializeCriticalSectionEx"},{"__crtSetThreadpoolTimer",4,"SetThreadpoolTimer"},
    {"__crtSetThreadpoolWait",3,"SetThreadpoolWait"},{"__crtWaitForThreadpoolTimerCallbacks",2,"WaitForThreadpoolTimerCallbacks"},
    {"__crtCompareStringW",6,"CompareStringEx"},{"__crtLCMapStringW",6,"LCMapStringEx"},
    {"__crtCompareStringA",7,""},{"__crtLCMapStringA",8,""},
    // <filesystem> 的路徑轉換：與 _MAX_FILESYS_NAME（260）同上限，以系統碼頁轉換。
    {"_To_wide",2,""},{"_To_byte",2,""},
};
bool c_space(char c){return c==' ' || (c>='\t' && c<='\r');}
// _Stoulx：略過空白、可選正負號、base 0 依前綴決定、16 進位可帶 0x；溢位回 ULONG_MAX 並回報錯誤。
struct ParsedInteger {std::uint64_t value{};std::size_t end{};bool negative{},overflow{},valid{};};
ParsedInteger parse_unsigned(std::string_view text,int base,std::uint64_t maximum){
    ParsedInteger result;std::size_t at=0;
    while(at<text.size() && c_space(text[at]))++at;
    if(at<text.size() && (text[at]=='+' || text[at]=='-'))result.negative=text[at++]=='-';
    if(base<0 || base==1 || base>36)return result;
    const auto digit=[](char c)->int{return c>='0' && c<='9'?c-'0':c>='a' && c<='z'?c-'a'+10:c>='A' && c<='Z'?c-'A'+10:99;};
    if((base==0 || base==16) && at+1<text.size() && text[at]=='0' && (text[at+1]=='x' || text[at+1]=='X') && at+2<text.size() && digit(text[at+2])<16){at+=2;base=16;}
    else if(base==0)base=at<text.size() && text[at]=='0'?8:10;
    const auto start=at;
    for(;at<text.size() && digit(text[at])<base;++at){
        const auto value=static_cast<std::uint64_t>(digit(text[at]));
        if(result.value>(maximum-value)/static_cast<std::uint64_t>(base))result.overflow=true;else result.value=result.value*static_cast<std::uint64_t>(base)+value;
    }
    if(at==start)return result;
    result.valid=true;result.end=at;return result;
}
// 小數格式的十進位尺度 pten 併入指數再轉換，避免先轉換再相乘的雙重捨入。
std::string scaled_decimal(std::string number,long pten){
    if(!pten)return number;
    std::size_t first=0;while(first<number.size() && (number[first]=='+' || number[first]=='-'))++first;
    if(number.size()>first+1 && number[first]=='0' && (number[first+1]=='x' || number[first+1]=='X'))return number;
    const auto exponent=number.find_first_of("eE");
    if(exponent==number.npos)return number+"e"+std::to_string(pten);
    const auto value=std::strtoll(number.c_str()+exponent+1,nullptr,10);
    return number.substr(0,exponent)+"e"+std::to_string(static_cast<long long>(value)+pten);
}
}
std::span<const WindowsServiceDescriptor> msvcp_runtime_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> rows;
        for(const auto family:runtime_families)for(const auto& entry:runtime_entries){
            if(!(entry.versions&runtime_version(family)))continue;
            rows.push_back({family,entry.name,entry.arguments,entry.wide,0,0,true});
        }
        for(const auto& wrapper:crt_wrappers)rows.push_back({"msvcp140.dll",wrapper.name,wrapper.arguments,0,0,0,true});
        // <random> 的 _XLgamma（float／double／long double）：x86 的 double 占兩個堆疊槽。
        rows.push_back({"msvcp140.dll","?_XLgamma@std@@YAMM@Z",1,0,0,0,true});rows.push_back({"msvcp140.dll","?_XLgamma@std@@YANN@Z",1,1,0,0,true});
        rows.push_back({"msvcp140.dll","?_XLgamma@std@@YAOO@Z",1,1,0,0,true});
        return rows;
    }();
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_msvcp_runtime(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const bool legacy=runtime_version(service.family)==1;const unsigned p=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(p*8);
    const auto done=[&](std::uint64_t value,bool wide=false)->std::optional<std::uint32_t>{arg.return_to(state,value,wide);return std::nullopt;};
    const auto set_errno=[&](std::uint32_t value){memory.store(GuestAddress{runtime_.errno_cell},32,value);};
    auto& locale=current_crt_locale(state,memory);
    // 值參數：Win32 在堆疊（double 占兩槽），Win64 在與參數序相同的 XMM。
    const auto floating_argument=[&](unsigned index,unsigned width)->std::uint64_t{
        if(x86_)return arg(index);
        std::uint64_t raw=0;for(unsigned b=0;b<width;++b)raw|=std::uint64_t{state.xmm[index][b]}<<(b*8);return raw;
    };
    // locale 向量（_Ctypevec／_Cvtvec 共用開頭）：VC8～VC10 為｛LCID, 碼頁, …｝，msvcp140 為｛碼頁, …, locale 名稱｝；NULL 時取目前 LC_CTYPE。
    const auto vector_category=[&](std::uint64_t vector,unsigned name_offset)->CrtLocaleCategory{
        if(!vector)return locale.categories[2];
        std::optional<CrtLocaleCategory> category;std::uint32_t page=0;
        if(legacy){const auto lcid=static_cast<std::uint32_t>(memory.load(GuestAddress{vector},32));page=static_cast<std::uint32_t>(memory.load(GuestAddress{vector+4},32));category=windows_crt_locale_id(lcid);}
        else {page=static_cast<std::uint32_t>(memory.load(GuestAddress{vector},32));const auto text=name_offset?memory.load(GuestAddress{vector+name_offset},bits):0;
            category=text?windows_crt_locale(read_utf16(memory,GuestAddress{text},255)):CrtLocaleCategory{};}
        if(!category)category=CrtLocaleCategory{};
        // 向量記錄的碼頁優先（例如 ".932"）；C locale 沒有碼頁。
        if(category->codepage && page)if(const auto paged=windows_crt_codepage(page)){category->codepage=page;category->maximum_character_bytes=paged->maximum_character_bytes;category->lead=paged->lead;category->trail=paged->trail;}
        return *category;
    };
    const auto evaluate=[&](auto zero)->std::optional<std::uint32_t>{
        using Value=decltype(zero);using Raw=std::conditional_t<sizeof(Value)==4,std::uint32_t,std::uint64_t>;constexpr bool single=sizeof(Value)==4;
        const auto width=static_cast<std::uint8_t>(sizeof(Value)*8);const auto operation=name.substr(name.starts_with("_F") || name.starts_with("_L")?2:1);
        const auto decode=[](std::uint64_t raw){return std::bit_cast<Value>(static_cast<Raw>(raw));};
        const auto code_result=[&](std::int16_t code){return done(static_cast<std::uint64_t>(static_cast<std::int64_t>(code)));};
        if(operation=="Dtest")return code_result(msvcp_detail::float_class(decode(memory.load(GuestAddress{arg(0)},width))));
        if(operation=="Dscale"){
            // *px×2^lexp：零、無限大與 NaN 原樣回報；結果上溢為無限大、下溢為零。
            auto x=decode(memory.load(GuestAddress{arg(0)},width));const auto code=msvcp_detail::float_class(x);if(code>=0)return code_result(code);
            const auto exponent=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1)));
            x=std::scalbn(x,exponent);memory.store(GuestAddress{arg(0)},width,std::bit_cast<Raw>(x));
            return code_result(std::isinf(x)?1:x==Value{}?0:-1);
        }
        if(operation=="Exp"){
            auto x=decode(memory.load(GuestAddress{arg(0)},width));
            const auto code=msvcp_detail::scaled_exp(x,decode(floating_argument(1,sizeof(Value))),static_cast<int>(static_cast<std::int16_t>(arg(2))));
            memory.store(GuestAddress{arg(0)},width,std::bit_cast<Raw>(x));return code_result(code);
        }
        const auto x=decode(floating_argument(0,sizeof(Value))),y=decode(floating_argument(1,sizeof(Value)));bool range_error=false;
        const auto result=operation=="Cosh"?msvcp_detail::scaled_cosh(x,y,range_error):msvcp_detail::scaled_sinh(x,y,range_error);
        if(range_error)set_errno(34);
        windows_floating_return(state,x86_,static_cast<double>(result),single);return done(0);
    };
    if(name.starts_with("?_XLgamma@std@@")){
        // ln|Γ(x)|：與 C99 lgamma 相同的特殊值（非正整數為 +∞ 並設 ERANGE）。
        const bool single=name.ends_with("MM@Z");const auto raw=floating_argument(0,single?4U:8U);
        const double x=single?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))):std::bit_cast<double>(raw);
        const double result=std::lgamma(x);if(std::isinf(result) && std::isfinite(x))set_errno(34);
        windows_floating_return(state,x86_,result,single);return done(0);
    }
    if(name.ends_with("Cosh") || name.ends_with("Sinh") || name.ends_with("Exp") || name.ends_with("Dtest") || name.ends_with("Dscale"))
        return name.starts_with("_F")?evaluate(float{}):evaluate(double{});
    if(name=="_Getctype" && !legacy){(void)msvcp_ctype_vector(state,memory,arg(0));return done(arg(0));}
    if(name=="_Getcvt" && !legacy){msvcp_cvt_vector(state,memory,arg(0));return done(arg(0));}
    if(name=="_Getctype"){
        // VC8～VC10 _Ctypevec｛LCID, 碼頁, 遮罩表, 釋放旗標｝：表以 CRT heap 配置，由 ctype::_Tidy 以 free 回收。
        const auto output=arg(0);memory.check(GuestAddress{output},x86_?16U:24U,Access::Write);const auto& category=locale.categories[2];
        const auto& values=windows_crt_character_table(category);const auto table=allocate_heap(memory,512);
        if(table)for(unsigned c=0;c<256;++c)memory.store(GuestAddress{table+c*2},16,values.masks[c+1]);
        memory.fill(GuestAddress{output},0,x86_?16U:24U);memory.store(GuestAddress{output},32,category.lcid);memory.store(GuestAddress{output+4},32,category.codepage);
        // 配置失敗時與 MSVC 相同不擁有表（釋放旗標 0）；呼叫端取得空表即改用 classic 分類。
        memory.store(GuestAddress{output+8},bits,table);memory.store(GuestAddress{output+8+p},32,table?1U:0U);return done(output);
    }
    if(name=="_Getcvt"){
        // VC8～VC10 _Cvtvec｛LCID, 碼頁｝為 8 位元組，Win32 以 EDX:EAX、Win64 以 RAX 回傳。
        const auto& category=locale.categories[2];return done(std::uint64_t{category.lcid}|(std::uint64_t{category.codepage}<<32),true);
    }
    if(name=="_Getcoll"){
        const auto& category=locale.categories[1];
        if(legacy)return done(std::uint64_t{category.lcid}|(std::uint64_t{category.codepage}<<32),true);
        // msvcp140 _Collvec｛碼頁, locale 名稱｝：Win32 8 位元組走 EDX:EAX，Win64 16 位元組以隱藏指標回傳；名稱由 collate 以 free 回收。
        std::uint64_t text=0;
        if(category.codepage){const auto bytes=utf16_bytes(category.name);text=allocate_heap(memory,bytes.size());if(text)memory.write(GuestAddress{text},bytes);}
        if(x86_)return done(std::uint64_t{category.codepage}|(text<<32),true);
        const auto output=arg(0);memory.check(GuestAddress{output},16,Access::Write);memory.fill(GuestAddress{output},0,16);
        memory.store(GuestAddress{output},32,category.codepage);memory.store(GuestAddress{output+8},64,text);return done(output);
    }
    if(name=="_Getdateorder"){
        // time_base::dateorder（no_order 0、dmy 1、mdy 2、ymd 3）：依 LC_TIME 長日期格式中日、月、年第一次出現的順序；dddd 為星期名稱。
        const auto data=windows_crt_time_data(locale.categories[5]);const auto& format=data.fields[41];std::u16string order;bool quoted=false;
        for(std::size_t i=0;i<format.size();){
            const auto c=format[i];if(c==u'\''){quoted=!quoted;++i;continue;}
            std::size_t run=1;while(i+run<format.size() && format[i+run]==c)++run;
            if(!quoted && (c==u'M' || c==u'y' || (c==u'd' && run<=2)) && order.find(c)==order.npos)order+=c;
            i+=run;
        }
        return done(order==u"dMy"?1U:order==u"Mdy"?2U:order==u"yMd"?3U:0U);
    }
    if(name=="_Getwctype" || name=="_Getwctypes"){
        // GetStringTypeW(CT_CTYPE1) 的分類旗標；_Getwctypes 填滿 [first,last) 對應的陣列並回傳 last。
        if(name=="_Getwctype")return done(static_cast<std::uint16_t>(windows_nls_character_type(static_cast<char16_t>(arg(0)),1)));
        const auto first=arg(0),last=arg(1),output=arg(2);if(last<first || (last-first)%2)return done(last);
        const auto count=(last-first)/2;memory.check(GuestAddress{output},count*2,Access::Write);
        for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{output+i*2},16,windows_nls_character_type(static_cast<char16_t>(memory.load(GuestAddress{first+i*2},16)),1));
        return done(last);
    }
    if(name=="_Tolower" || name=="_Toupper"){
        // C locale 只轉換 ASCII；其他 locale 以向量的遮罩表判斷，單一位元組或前導位元組＋尾隨位元組依 locale 轉換大小寫。
        const bool upper=name=="_Toupper";const auto c=static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0)));const auto category=vector_category(arg(1),3*p);
        if(!category.codepage){if(upper?(c>='a' && c<='z'):(c>='A' && c<='Z'))return done(static_cast<std::uint32_t>(c+(upper?-32:32)));return done(static_cast<std::uint32_t>(c));}
        if(c>=0 && c<256){
            const auto table=arg(1)?memory.load(GuestAddress{arg(1)+(legacy?8U:p)},bits):0;
            const auto mask=table?static_cast<std::uint16_t>(memory.load(GuestAddress{table+static_cast<std::uint64_t>(c)*2},16)):windows_crt_character_table(category).masks[static_cast<std::size_t>(c)+1];
            if(!(mask&(upper?2U:1U)))return done(static_cast<std::uint32_t>(c));
        }
        std::vector<std::uint8_t> input;if(category.lead[static_cast<std::uint8_t>(c>>8)])input={static_cast<std::uint8_t>(c>>8),static_cast<std::uint8_t>(c)};else input={static_cast<std::uint8_t>(c)};
        const auto decoded=windows_crt_decode(input,category.codepage);
        if(decoded.status!=CrtDecodedCharacter::Status::Complete || decoded.character>0xffff)return done(static_cast<std::uint32_t>(c));
        const auto mapped=windows_crt_map_character(static_cast<char16_t>(decoded.character),category,upper);
        const auto encoded=windows_crt_encode(std::u16string_view(&mapped,1),category.codepage);
        if(!encoded || encoded->empty() || encoded->size()>2)return done(static_cast<std::uint32_t>(c));
        return done(encoded->size()==1?(*encoded)[0]:(std::uint32_t{(*encoded)[0]}<<8)|(*encoded)[1]);
    }
    if(name=="_Towlower" || name=="_Towupper"){
        // WEOF 不變；C locale 的 Latin-1 範圍只轉 ASCII，其餘依向量 locale（C locale 時用 Unicode 預設規則）轉換。
        const bool upper=name=="_Towupper";const auto c=static_cast<char16_t>(arg(0));if(c==0xffff)return done(c);
        auto category=vector_category(arg(1),3*p);
        if(!category.codepage){
            if(c<256){if(upper?(c>=u'a' && c<=u'z'):(c>=u'A' && c<=u'Z'))return done(static_cast<char16_t>(upper?c-32:c+32));return done(c);}
            category.name=u"";category.codepage=65001;
        }
        return done(windows_crt_map_character(c,category,upper));
    }
    if(name=="_Wcrtomb"){
        // C locale 只接受 0～255；其他碼頁與 WideCharToMultiByte(旗標 0) 相同允許 best-fit，用到預設字元時回 -1 並設 EILSEQ。
        const auto output=arg(0);const auto c=static_cast<char16_t>(arg(1));const auto vector=arg(3);
        std::uint32_t page=0;
        if(vector){if(legacy)page=memory.load(GuestAddress{vector},32)?static_cast<std::uint32_t>(memory.load(GuestAddress{vector+4},32)):0U;
            else page=memory.load(GuestAddress{vector+8},32)?0U:static_cast<std::uint32_t>(memory.load(GuestAddress{vector},32));}
        else page=locale.categories[2].codepage;
        std::vector<std::uint8_t> bytes;
        if(!page){if(c>255){set_errno(42);return done(UINT32_MAX);}bytes.push_back(static_cast<std::uint8_t>(c));}
        else {const auto encoded=windows_nls_encode(std::u16string_view(&c,1),page,false,"?");
            if(!encoded || encoded->used_default || encoded->bytes.empty()){set_errno(42);return done(UINT32_MAX);}bytes=encoded->bytes;}
        if(output)memory.write(GuestAddress{output},bytes);return done(bytes.size());
    }
    if(name.starts_with("_Sto")){
        // _Sto*：C locale 數值語法；x 版本另回報錯誤旗標（成功 0、範圍錯誤 1），範圍錯誤一律設 errno ERANGE。
        const auto source=arg(0),end_pointer=arg(1);const bool checked=name.ends_with('x');
        const auto error_pointer=checked?arg(3):0;
        if(error_pointer)memory.store(GuestAddress{error_pointer},32,0);
        if(end_pointer)memory.check(GuestAddress{end_pointer},p,Access::Write);
        const auto text=source?read_ansi(memory,source):std::string{};
        const auto finish_end=[&](std::size_t consumed){if(end_pointer)memory.store(GuestAddress{end_pointer},bits,source+consumed);};
        const auto range=[&]{set_errno(34);if(error_pointer)memory.store(GuestAddress{error_pointer},32,1);};
        const auto floating_name=name.substr(4,name.size()-4-(checked?1:0));
        if(floating_name=="d" || floating_name=="f" || floating_name=="ld"){
            const bool single=floating_name=="f";const auto pten=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
            char* end=nullptr;errno=0;(void)std::strtod(text.c_str(),&end);const auto consumed=static_cast<std::size_t>(end-text.c_str());
            double value=0;
            if(consumed){
                const auto number=scaled_decimal(text.substr(0,consumed),pten);errno=0;
                value=single?static_cast<double>(std::strtof(number.c_str(),nullptr)):std::strtod(number.c_str(),nullptr);
                if(errno==ERANGE)range();
            }
            finish_end(consumed);windows_floating_return(state,x86_,value,single);return done(0);
        }
        const bool wide=name.starts_with("_Stoll") || name.starts_with("_Stoull"),is_signed=name.starts_with("_Stol");
        const auto base=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
        const std::uint64_t maximum=wide?UINT64_MAX:UINT32_MAX;
        const auto parsed=parse_unsigned(text,base,maximum);
        if(!parsed.valid){finish_end(0);return done(0,wide);}
        finish_end(parsed.end);
        if(!is_signed){
            if(parsed.overflow){range();return done(maximum,wide);}
            return done((parsed.negative?0-parsed.value:parsed.value)&maximum,wide);
        }
        // _Stolx／_Stollx：以無號轉換後檢查有號範圍，溢位回 LONG_MIN／LONG_MAX。
        const std::uint64_t positive=wide?static_cast<std::uint64_t>(INT64_MAX):static_cast<std::uint64_t>(INT32_MAX);
        if(parsed.overflow || parsed.value>positive+(parsed.negative?1U:0U)){range();return done((parsed.negative?positive+1:positive)&maximum,wide);}
        return done((parsed.negative?0-parsed.value:parsed.value)&maximum,wide);
    }
    if(const auto wrapper=std::find_if(std::begin(crt_wrappers),std::end(crt_wrappers),[&](const CrtWrapper& item){return item.name==name;});wrapper!=std::end(crt_wrappers)){
        // 64 位元回傳值（GetTickCount64）在 Win32 需 EDX:EAX，直接取值；其餘經客體呼叫原 API，等待與回呼語義完全沿用。
        if(name=="__crtGetTickCount64")return done(nested_service(state,memory,"kernel32.dll","GetTickCount64",{}),true);
        if(!wrapper->target.empty()){
            std::vector<std::uint64_t> arguments;for(unsigned i=0;i<wrapper->arguments;++i)arguments.push_back(arg(i));
            if(name=="__crtCompareStringW" || name=="__crtLCMapStringW")arguments.insert(arguments.end(),{0,0,0});
            start_callbacks(state,memory,{{resolve("kernel32.dll",wrapper->target).value,std::move(arguments)}},Continuation::ReturnCallback);return std::nullopt;
        }
        const auto page_of=[&](std::uint32_t page){return page?page:locale.categories[2].codepage?locale.categories[2].codepage:65001U;};
        if(name=="_To_wide" || name=="_To_byte"){
            // 失敗或超過 260 個單位時與 MultiByteToWideChar／WideCharToMultiByte 相同不寫入完整結果；回傳目的指標。
            const auto output=arg(1);
            if(name=="_To_wide"){
                const auto text=windows_nls_decode(read_ansi(memory,arg(0),65536),65001,false);
                if(text && text->size()<260){memory.write(GuestAddress{output},utf16_bytes(*text));}
            }else {
                const auto text=read_utf16(memory,GuestAddress{arg(0)},65536);const auto encoded=windows_nls_encode(text,65001,false,"?");
                if(encoded && encoded->bytes.size()<260){auto bytes=encoded->bytes;bytes.push_back(0);memory.write(GuestAddress{output},bytes);}
            }
            return done(output);
        }
        // __crtCompareStringA(locale, flags, s1, n1, s2, n2, 碼頁)、__crtLCMapStringA(locale, flags, src, n, dst, ndst, 碼頁, 嚴格)：
        // 依碼頁（0 為目前 LC_CTYPE）轉成 UTF-16 後交給 CompareStringEx／LCMapStringEx；LCMAP_SORTKEY 直接輸出位元組，其他映射結果轉回碼頁。
        const bool compare=name=="__crtCompareStringA";const auto page=page_of(static_cast<std::uint32_t>(arg(6)));
        const auto narrow=[&](std::uint64_t address,std::int32_t count)->std::optional<std::u16string>{
            if(!address)return std::nullopt;std::string bytes;
            if(count<0)bytes=read_ansi(memory,address,1U<<20);else{bytes.resize(static_cast<std::size_t>(count));memory.read(GuestAddress{address},std::span(reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()));}
            return windows_nls_decode(bytes,page,!compare && static_cast<std::uint32_t>(arg(7))!=0);
        };
        GuestScratch scratch(*this,memory,state);
        if(compare){
            const auto left=narrow(arg(2),static_cast<std::int32_t>(arg(3))),right=narrow(arg(4),static_cast<std::int32_t>(arg(5)));
            if(!left || !right){last_error_=87;return done(0);}
            const auto first=scratch.text(*left),second=scratch.text(*right);if(!first || !second){last_error_=8;return done(0);}
            return done(nested_service(state,memory,"kernel32.dll","CompareStringEx",{arg(0),arg(1),first,left->size(),second,right->size(),0,0,0}));
        }
        const auto source=narrow(arg(2),static_cast<std::int32_t>(arg(3)));if(!source){last_error_=1113;return done(0);}
        const auto flags=static_cast<std::uint32_t>(arg(1));const auto input=scratch.text(*source);if(!input){last_error_=8;return done(0);}
        const auto required=static_cast<std::int32_t>(nested_service(state,memory,"kernel32.dll","LCMapStringEx",{arg(0),flags,input,source->size(),0,0,0,0,0}));
        if(required<=0)return done(0);
        const auto buffer=scratch.allocate(static_cast<std::size_t>(required)*2+2);if(!buffer){last_error_=8;return done(0);}
        const auto produced=static_cast<std::int32_t>(nested_service(state,memory,"kernel32.dll","LCMapStringEx",{arg(0),flags,input,source->size(),buffer,static_cast<std::uint64_t>(required),0,0,0}));
        if(produced<=0)return done(0);
        std::vector<std::uint8_t> bytes;
        if(flags&0x400U){bytes.resize(static_cast<std::size_t>(produced));memory.read(GuestAddress{buffer},bytes);}
        else {const auto mapped=read_utf16_counted(memory,GuestAddress{buffer},static_cast<std::size_t>(produced));const auto encoded=windows_nls_encode(mapped,page,false,"?");if(!encoded){last_error_=1113;return done(0);}bytes=encoded->bytes;}
        const auto capacity=static_cast<std::int32_t>(arg(5));
        if(!capacity)return done(bytes.size());
        if(capacity<static_cast<std::int32_t>(bytes.size())){last_error_=122;return done(0);}
        memory.write(GuestAddress{arg(4)},bytes);return done(bytes.size());
    }
    throw HostCallFault("未知的 STL C 輔助匯出");
}
// VC11 起的 _Ctypevec｛碼頁, 遮罩表, 釋放旗標, locale 名稱｝：表與名稱以 CRT heap 配置，由 ctype::_Tidy 以 free 回收；
// 回傳配置的表，0 表示配置失敗（呼叫端可改用常駐 classic 表）。msvcp140 的 _Locinfo::_Getctype 共用此處。
std::uint64_t WindowsServices::msvcp_ctype_vector(GuestState& state,GuestMemory& memory,std::uint64_t output){
    const unsigned p=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(p*8);
    memory.check(GuestAddress{output},4*p,Access::Write);const auto& category=current_crt_locale(state,memory).categories[2];
    const auto& values=windows_crt_character_table(category);const auto table=allocate_heap(memory,512);
    if(table)for(unsigned c=0;c<256;++c)memory.store(GuestAddress{table+c*2},16,values.masks[c+1]);
    std::uint64_t locale_name=0;
    if(category.codepage){const auto bytes=utf16_bytes(category.name);locale_name=allocate_heap(memory,bytes.size());if(locale_name)memory.write(GuestAddress{locale_name},bytes);}
    memory.fill(GuestAddress{output},0,4*p);memory.store(GuestAddress{output},32,category.codepage);memory.store(GuestAddress{output+p},bits,table);
    memory.store(GuestAddress{output+2*p},32,table?1U:0U);memory.store(GuestAddress{output+3*p},bits,locale_name);
    return table;
}
// VC11 起的 _Cvtvec｛代碼頁, 最大位元組數, 是否 C locale, 前導位元組位圖 32 位元組｝，與 codecvt facet 保存的欄位相同。
void WindowsServices::msvcp_cvt_vector(GuestState& state,GuestMemory& memory,std::uint64_t output){
    memory.check(GuestAddress{output},44,Access::Write);const auto& category=current_crt_locale(state,memory).categories[2];
    std::vector<std::uint8_t> bytes(44);for(unsigned b=0;b<4;++b){bytes[b]=static_cast<std::uint8_t>(category.codepage>>(8*b));bytes[4+b]=static_cast<std::uint8_t>(category.maximum_character_bytes>>(8*b));}
    bytes[8]=category.codepage==0;for(unsigned c=0;c<256;++c)if(category.lead[c])bytes[12+c/8]=static_cast<std::uint8_t>(bytes[12+c/8]|(1U<<(c%8)));
    memory.write(GuestAddress{output},bytes);
}
}
