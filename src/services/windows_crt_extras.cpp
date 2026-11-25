#include "x64arm/windows_services.hpp"
#include "crt_program_data.hpp"
#include "x64arm/unicode.hpp"
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <locale.h>

// CRT 補充家族（msvcrt／msvcr*／ucrtbase 共用 crt 家族）：字元分類小函式、字串填充、浮點小函式、
// 執行緒識別、無效參數處理器查詢、FMA3 切換、程式路徑、timespec、_atodbl 類解析、寬字元轉換的 _s 版本、
// _strerror／__wcserror、_expand、複數建構。浮點參數依 ABI：Win32 在堆疊（double 占兩槽）、Win64 在 XMM0～3；
// 浮點回傳經 windows_floating_return（ST(0)／XMM0）。
namespace x64arm {
void windows_floating_return(GuestState& state,bool x86,double value,bool single);
namespace {
// CRT 資料區（runtime_base_）中本家族使用的固定位置；0～143 與 0x800 起的串流由既有資料匯出使用。
constexpr unsigned dstbias_offset=160,fma3_offset=168,strerror_offset=176,wcserror_offset=184;
constexpr std::uint32_t einval=22,erange=34,eilseq=42,enomem=12;
}
std::span<const WindowsServiceDescriptor> crt_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"crt","__toascii",1},
        {"crt","_tolower",1},{"crt","_toupper",1},{"crt","iswascii",1},{"crt","wctrans",1},{"crt","towctrans",2},
        {"crt","_chgsign",1,1},{"crt","_chgsignf",1},{"crt","_finitef",1},{"crt","_isnanf",1},
        {"crt","remquo",3,3},{"crt","remquof",3},{"crt","tgammaf",1},
        {"crt","_memccpy",4},{"crt","_strnset",3},{"crt","_strset",2},{"crt","_wcsnset",3},{"crt","_wcsset",2},
        {"crt","_strnset_s",4},{"crt","_wcsnset_s",4},{"crt","_wcsset_s",3},
        {"crt","__threadid",0},{"crt","__threadhandle",0},
        {"crt","_get_invalid_parameter_handler",0},{"crt","_get_thread_local_invalid_parameter_handler",0},
        {"crt","_get_FMA3_enable",0},{"crt","_set_FMA3_enable",1},
        {"crt","__dstbias",0},{"crt","_get_pgmptr",1},{"crt","_get_wpgmptr",1},{"crt","__p__pgmptr",0},{"crt","__p__wpgmptr",0},
        {"crt","_timespec32_get",2},{"crt","_timespec64_get",2},
        {"crt","_atodbl",2},{"crt","_atoflt",2},{"crt","_atoldbl",2},
        {"crt","wctomb_s",4},{"crt","wcrtomb_s",5},
        {"crt","_strerror",1},{"crt","__wcserror",1},
        {"crt","_expand",2},{"crt","_Cbuild",3,6},{"crt","_FCbuild",2},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value,bool wide=false){arg.return_to(state,value,wide);return std::optional<std::uint32_t>{};};
    const auto set_errno=[&](std::uint32_t code){memory.store(GuestAddress{runtime_.errno_cell},32,code);};
    // 參數錯誤：設定 errno，有處理器時先回呼再回傳。
    const auto invalid=[&](std::uint64_t result,std::uint32_t code=einval)->std::optional<std::uint32_t>{
        set_errno(code);
        if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
        return done(result);
    };
    const auto real_bits=[&](unsigned index,bool single){
        std::uint64_t raw=0;
        if(x86_)raw=arg(index);else for(unsigned i=0;i<(single?4U:8U);++i)raw|=std::uint64_t(state.xmm[index][i])<<(8*i);
        return raw;
    };
    const auto real=[&](unsigned index,bool single){
        const auto raw=real_bits(index,single);
        return single?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))):std::bit_cast<double>(raw);
    };
    const auto give=[&](double value,bool single){windows_floating_return(state,x86_,value,single);return done(0);};
    const auto crt=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"crt",symbol,args);};
    const auto cell=[&](unsigned offset){return runtime_base_.value+offset;};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto s32=[&](unsigned index){return static_cast<std::int32_t>(u32(index));};
    try{
        if(name=="__toascii")return done(u32(0)&0x7fU);
        // 不檢查範圍的轉換：與 Windows 相同，只對對應大小寫字母有意義。
        if(name=="_tolower")return done(static_cast<std::uint32_t>(s32(0)-'A'+'a'));
        if(name=="_toupper")return done(static_cast<std::uint32_t>(s32(0)-'a'+'A'));
        if(name=="iswascii")return done((u32(0)&0xffffU)<0x80);
        if(name=="wctrans"){
            // wctrans_t：1＝tolower、2＝toupper，與 towctrans 對應。
            if(!arg(0))return done(0);std::string text;
            for(unsigned i=0;i<16;++i){const auto c=static_cast<char>(memory.load(GuestAddress{arg(0)+i},8));if(!c)break;text+=c;}
            return done(text=="tolower"?1U:text=="toupper"?2U:0U);
        }
        if(name=="towctrans"){
            const auto c=u32(0)&0xffffU,kind=u32(1)&0xffffU;
            return done(kind==1?crt("towlower",{c}):kind==2?crt("towupper",{c}):c);
        }
        // 浮點小函式。
        if(name=="_chgsign")return give(-real(0,false),false);
        if(name=="_chgsignf")return give(-real(0,true),true);
        if(name=="_finitef")return done(std::isfinite(real(0,true))?1U:0U);
        if(name=="_isnanf")return done(std::isnan(real(0,true))?1U:0U);
        if(name=="remquo" || name=="remquof"){
            const bool single=name=="remquof";int quotient=0;
            const auto value=single?static_cast<double>(std::remquo(static_cast<float>(real(0,true)),static_cast<float>(real(1,true)),&quotient)):std::remquo(real(0,false),real(1,false),&quotient);
            if(arg(2))memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(quotient));
            return give(value,single);
        }
        if(name=="tgammaf")return give(static_cast<double>(std::tgamma(static_cast<float>(real(0,true)))),true);
        if(name=="_memccpy"){
            // 複製到包含 c 的位元組為止；回傳目的端中 c 之後的位置，未遇到則回 NULL。
            const auto target=static_cast<std::uint8_t>(u32(2));
            for(std::uint64_t i=0;i<arg(3);++i){const auto b=static_cast<std::uint8_t>(memory.load(GuestAddress{arg(1)+i},8));memory.store(GuestAddress{arg(0)+i},8,b);if(b==target)return done(arg(0)+i+1);}
            return done(0);
        }
        if(name=="_strnset" || name=="_strset" || name=="_wcsnset" || name=="_wcsset"){
            // 填入字元直到 NUL（_n 版本最多 count 個），回傳原字串。寬字元版本以 16 位元碼元處理。
            const bool wide=name.starts_with("_wcs"),limited=name.ends_with("nset");const unsigned width=wide?2U:1U;
            const auto value=u32(1)&(wide?0xffffU:0xffU);const auto limit=limited?arg(2):UINT64_MAX;
            for(std::uint64_t i=0;i<limit;++i){const auto at=arg(0)+i*width;if(!memory.load(GuestAddress{at},static_cast<std::uint8_t>(width*8)))break;memory.store(GuestAddress{at},static_cast<std::uint8_t>(width*8),value);}
            return done(arg(0));
        }
        if(name=="_strnset_s" || name=="_wcsnset_s" || name=="_wcsset_s"){
            // (str, numberOfElements, c[, count])：str 必須在緩衝區內以 NUL 結尾，否則清空並回 EINVAL。
            const bool wide=name.starts_with("_wcs"),limited=name.ends_with("nset_s");const unsigned width=wide?2U:1U;
            const auto text=arg(0),size=arg(1);const auto value=u32(2)&(wide?0xffffU:0xffU);const auto limit=limited?arg(3):UINT64_MAX;
            if(!text){if(!size && limited && !limit)return done(0);return invalid(einval);}
            if(!size)return invalid(einval);
            std::uint64_t length=0;while(length<size && memory.load(GuestAddress{text+length*width},static_cast<std::uint8_t>(width*8)))++length;
            if(length==size){memory.store(GuestAddress{text},static_cast<std::uint8_t>(width*8),0);return invalid(einval);}
            for(std::uint64_t i=0;i<length && i<limit;++i)memory.store(GuestAddress{text+i*width},static_cast<std::uint8_t>(width*8),value);
            return done(0);
        }
        if(name=="__threadid")return done(thread_identifier(threads_.current));
        if(name=="__threadhandle")return done(x86_?0xfffffffeU:UINT64_C(0xfffffffffffffffe));
        if(name=="_get_invalid_parameter_handler")return done(runtime_.invalid_parameter_handler);
        if(name=="_get_thread_local_invalid_parameter_handler"){const auto found=threads_.items.find(threads_.current);return done(found==threads_.items.end()?0:found->second.invalid_parameter_handler);}
        if(name=="_get_FMA3_enable")return done(memory.load(GuestAddress{cell(fma3_offset)},32));
        if(name=="_set_FMA3_enable"){const auto previous=memory.load(GuestAddress{cell(fma3_offset)},32);memory.store(GuestAddress{cell(fma3_offset)},32,u32(0)?1U:0U);return done(previous);}
        if(name=="__dstbias")return done(cell(dstbias_offset));
        if(name=="_get_pgmptr" || name=="_get_wpgmptr" || name=="__p__pgmptr" || name=="__p__wpgmptr"){
            const bool wide=name.find("wpgmptr")!=name.npos;
            const auto slot=cell(wide?detail::crt_wpgmptr_offset:detail::crt_pgmptr_offset);
            if(name.starts_with("__p__"))return done(slot);
            if(!arg(0))return invalid(einval);
            memory.store(GuestAddress{arg(0)},bits,memory.load(GuestAddress{slot},bits));return done(0);
        }
        if(name=="_timespec32_get" || name=="_timespec64_get"){
            // 只支援 TIME_UTC（1）；成功回傳 base。
            const auto base=u32(1);if(!arg(0))return invalid(0);if(base!=1)return done(0);
            const auto since=runtime_.system_filetime()-UINT64_C(116444736000000000);
            const auto seconds=since/10000000U,nanoseconds=(since%10000000U)*100U;
            if(name=="_timespec32_get"){memory.store(GuestAddress{arg(0)},32,seconds);memory.store(GuestAddress{arg(0)+4},32,nanoseconds);}
            else {memory.store(GuestAddress{arg(0)},64,seconds);memory.store(GuestAddress{arg(0)+8},32,nanoseconds);}
            return done(1);
        }
        if(name=="_atodbl" || name=="_atoflt" || name=="_atoldbl"){
            // (結果, 字串)：C locale 解析，溢位回 _OVERFLOW（3）、下溢回 _UNDERFLOW（4）；_LDOUBLE 為 10 位元組 x87 延伸精度。
            if(!arg(0) || !arg(1))return invalid(0);
            std::string text;for(unsigned i=0;i<4096;++i){const auto c=static_cast<char>(memory.load(GuestAddress{arg(1)+i},8));if(!c)break;text+=c;}
            static const auto locale=newlocale(LC_NUMERIC_MASK,"C",nullptr);errno=0;char* end=nullptr;
            const bool single=name=="_atoflt";
            const double value=single?static_cast<double>(strtof_l(text.c_str(),&end,locale)):strtod_l(text.c_str(),&end,locale);
            const bool range=errno==ERANGE;
            if(single)memory.store(GuestAddress{arg(0)},32,std::bit_cast<std::uint32_t>(static_cast<float>(value)));
            else if(name=="_atodbl")memory.store(GuestAddress{arg(0)},64,std::bit_cast<std::uint64_t>(value));
            else {
                // double → 80 位元延伸精度：64 位元有效數字（含明示整數位）與 15 位元指數。
                const auto raw=std::bit_cast<std::uint64_t>(value);const bool negative=raw>>63;const auto exponent=static_cast<int>((raw>>52)&0x7ff);auto fraction=raw&((UINT64_C(1)<<52)-1);
                std::uint64_t significand=0;std::uint32_t biased=0;
                if(exponent==0x7ff){biased=0x7fff;significand=(UINT64_C(1)<<63)|(fraction<<11);}
                else if(exponent){biased=static_cast<std::uint32_t>(exponent-1023+16383);significand=(UINT64_C(1)<<63)|(fraction<<11);}
                else if(fraction){int shift=std::countl_zero(fraction)-11;significand=(fraction<<11)<<shift;biased=static_cast<std::uint32_t>(1-1023+16383-shift);}
                memory.store(GuestAddress{arg(0)},64,significand);memory.store(GuestAddress{arg(0)+8},16,(negative?0x8000U:0U)|biased);
            }
            return done(range?(std::isinf(value) || std::fabs(value)>=1.0?3U:4U):0U);
        }
        if(name=="wctomb_s" || name=="wcrtomb_s"){
            // wctomb_s(int* retval, char* mb, size_t size, wchar_t wc)／wcrtomb_s(size_t* retval, char* mb, size_t size, wchar_t wc, mbstate_t*)：
            // 轉換交給既有 wctomb／wcrtomb（ACP＝UTF-8），在此檢查緩衝區並回報長度。
            const bool restartable=name=="wcrtomb_s";const auto retval=arg(0),output=arg(1),size=arg(2);const auto wc=u32(3)&0xffffU;
            const auto report=[&](std::uint64_t value){if(retval)memory.store(GuestAddress{retval},restartable?bits:std::uint8_t{32},value);};
            if(!output && size)return invalid(einval);
            // 沒有輸出緩衝區：wctomb_s 回報 0（UTF-8 無轉換狀態）；wcrtomb_s 等同轉換 L'\0' 以重設狀態。
            if(!output && !restartable){report(0);return done(0);}
            GuestScratch temporary(*this,memory,state);const auto buffer=temporary.allocate(16);if(!buffer)return done(enomem);
            const auto converted=restartable?crt("wcrtomb",{buffer,output?wc:0U,arg(4)}):crt("wctomb",{buffer,wc});
            const bool failed=restartable?converted==(x86_?UINT32_MAX:UINT64_MAX):static_cast<std::int32_t>(static_cast<std::uint32_t>(converted))<0;
            if(failed){report(restartable?(x86_?UINT32_MAX:UINT64_MAX):UINT32_MAX);set_errno(eilseq);return done(eilseq);}
            const auto length=restartable?converted:static_cast<std::uint32_t>(converted);
            if(!output){report(length);return done(0);}
            if(length>size){report(restartable?(x86_?UINT32_MAX:UINT64_MAX):UINT32_MAX);return invalid(erange,erange);}
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{buffer},bytes);if(length)memory.write(GuestAddress{output},bytes);
            report(length);return done(0);
        }
        if(name=="_strerror" || name=="__wcserror"){
            // 「訊息: errno 說明\n」；訊息為空或 NULL 時只有說明。窄字元版本以 UTF-8 寫出。
            const bool wide=name!="_strerror";const auto message=arg(0);
            const auto code=static_cast<std::uint32_t>(memory.load(GuestAddress{runtime_.errno_cell},32));
            const auto description=read_utf16(memory,GuestAddress{crt("_wcserror",{code})});
            std::u16string prefix;
            if(message){if(wide)prefix=read_utf16(memory,GuestAddress{message});else {std::string narrow;for(unsigned i=0;i<4096;++i){const auto c=static_cast<char>(memory.load(GuestAddress{message+i},8));if(!c)break;narrow+=c;}prefix=utf8_to_utf16_lossy(narrow);}}
            auto text=(prefix.empty()?std::u16string{}:prefix+u": ")+description;
            while(!text.empty() && (text.back()==u'\n' || text.back()==u'\r'))text.pop_back();text+=u'\n';
            const auto slot=cell(wide?wcserror_offset:strerror_offset);
            if(!memory.load(GuestAddress{slot},bits))memory.store(GuestAddress{slot},bits,runtime_allocate(memory,{},1024));
            const auto buffer=memory.load(GuestAddress{slot},bits);
            if(wide){const auto length=std::min<std::size_t>(text.size(),511);for(std::size_t i=0;i<length;++i)memory.store(GuestAddress{buffer+i*2},16,text[i]);memory.store(GuestAddress{buffer+length*2},16,0);}
            else {auto narrow=utf16_to_utf8_lossy(text);if(narrow.size()>1023)narrow.resize(1023);std::vector<std::uint8_t> bytes(narrow.begin(),narrow.end());bytes.push_back(0);memory.write(GuestAddress{buffer},bytes);}
            return done(buffer);
        }
        if(name=="_expand"){
            // 只在原處調整：縮小保留原區塊並回傳原指標，放大無法不移動時回 NULL（errno＝ENOMEM）。
            if(!arg(0))return invalid(0);
            const auto current=crt("_msize",{arg(0)});if(current==(x86_?UINT32_MAX:UINT64_MAX))return done(0);
            if(arg(1)<=current){if(const auto found=allocations_.find(arg(0));found!=allocations_.end())found->second=arg(1);return done(arg(0));}
            set_errno(enomem);return done(0);
        }
        if(name=="_Cbuild"){
            // _Dcomplex 為 16 位元組結構：呼叫端提供的隱含結果指標為第一個參數，實部與虛部隨後。
            const auto output=arg(0);if(!output)return done(0);
            const auto re=real_bits(1,false),im=real_bits(2,false);
            memory.check(GuestAddress{output},16,Access::Write);
            memory.store(GuestAddress{output},64,re);memory.store(GuestAddress{output+8},64,im);
            return done(output);
        }
        if(name=="_FCbuild"){
            // _Fcomplex 為 8 位元組結構：Win64 由 RAX、Win32 由 EDX:EAX 回傳。
            const auto re=static_cast<std::uint32_t>(real_bits(0,true)),im=static_cast<std::uint32_t>(real_bits(1,true));
            return done(std::uint64_t{re}|(std::uint64_t{im}<<32),true);
        }
    }catch(const MemoryFault&){return invalid(0);}
    throw HostCallFault("未知的 CRT 補充服務");
}
}
