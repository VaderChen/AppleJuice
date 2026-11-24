#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_version.hpp"
#include "service_callback_task.hpp"
#include "crt_mbcs.hpp"
#include "crt_os_error.hpp"
#include "windows_crt_character.hpp"
#include <algorithm>
#include <array>

// CRT 執行階段補充家族（msvcrt／msvcr*／ucrtbase 共用 crt 家族）：
// 標準輸入輸出補充（getwchar／putwchar、gets(_s)、_getws／_putws、_getw／_putw、fread_s、clearerr_s、freopen_s）、
// 清單參數的 _spawnl*／_execl*（收集到 NULL 後交給既有 _spawnv*／_execv*）、作業系統包裝（_beep、_sleep、_seterrormode、
// _getdrives、_getdiskfree、_getdllprocaddr、_loaddll、_unloaddll、_eof、_pipe）、_mktemp 系列、_searchenv 系列、
// __fpecode／__pxcptinfoptrs、__pwctype_func、set_unexpected／_get_unexpected、
// 作業系統版本查詢（_get_osplatform／_get_osver／_get_winver／_get_winmajor／_get_winminor，與 GetVersionEx 同一版本常數）、
// _set_SSE2_enable 與 small-block heap 門檻（_get／_set_sbh_threshold）。
// 會阻塞或回呼客體的操作（串流、程序、DLL 載入、Sleep、Beep）經既有 CRT／kernel32 入口續行；其餘在宿主端直接完成。
namespace x64arm {
namespace {
constexpr std::uint32_t einval=22,erange=34,enoent=2,eexist=17;
constexpr unsigned unexpected_offset=192,wctype_offset=0x400;
// 舊版 CRT 匯出資料格：_pwctype（指向 _wctype 表的指標）、_amblksiz、_winver／_winmajor／_winminor／_osver；_dstbias 與 crt_extras 共用 160。
constexpr unsigned pwctype_offset=0x320,amblksiz_offset=0x328,winver_offset=0x330,winmajor_offset=0x334,winminor_offset=0x338,osver_offset=0x33c,dstbias_offset=160;
}
std::span<const WindowsServiceDescriptor> crt_runtime_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"crt","getwchar",0},{"crt","putwchar",1},{"crt","gets",1},{"crt","gets_s",2},{"crt","_getws",1},{"crt","_putws",1},
        {"crt","_getw",1},{"crt","_putw",2},{"crt","fread_s",5},{"crt","clearerr_s",1},{"crt","freopen_s",4},{"crt","_wfreopen_s",4},
        {"crt","_spawnl",3},{"crt","_spawnle",3},{"crt","_spawnlp",3},{"crt","_spawnlpe",3},
        {"crt","_wspawnl",3},{"crt","_wspawnle",3},{"crt","_wspawnlp",3},{"crt","_wspawnlpe",3},
        {"crt","_execl",2},{"crt","_execle",2},{"crt","_execlp",2},{"crt","_execlpe",2},
        {"crt","_wexecl",2},{"crt","_wexecle",2},{"crt","_wexeclp",2},{"crt","_wexeclpe",2},
        {"crt","_beep",2},{"crt","_sleep",1},{"crt","_loaddll",1},{"crt","_unloaddll",1},
        {"crt","_seterrormode",1},{"crt","_getdrives",0},{"crt","_getdiskfree",2},{"crt","_getdllprocaddr",3},{"crt","_eof",1},{"crt","_pipe",3},
        {"crt","_mktemp",1},{"crt","_wmktemp",1},{"crt","_mktemp_s",2},{"crt","_wmktemp_s",2},
        {"crt","_searchenv",3},{"crt","_wsearchenv",3},{"crt","_searchenv_s",4},{"crt","_wsearchenv_s",4},
        {"crt","__fpecode",0},{"crt","__pxcptinfoptrs",0},{"crt","__pwctype_func",0},
        {"crt","set_unexpected",1},{"crt","_get_unexpected",0},
        {"crt","_get_osplatform",1},{"crt","_get_osver",1},{"crt","_get_winver",1},{"crt","_get_winmajor",1},{"crt","_get_winminor",1},
        {"crt","_set_SSE2_enable",1},{"crt","_get_sbh_threshold",0},{"crt","_set_sbh_threshold",1},
        // 舊版 CRT 的資料指標存取（與資料匯出同一資料格）、va_list 複製、已停用的 _heapadd、未啟用 RTC 時的 _CRT_RTC_INIT 與 __crt* 系統包裝。
        {"crt","__p__iob",0},{"crt","__p__pctype",0},{"crt","__p__pwctype",0},{"crt","__p__dstbias",0},{"crt","__p__amblksiz",0},
        {"crt","__p__winver",0},{"crt","__p__winmajor",0},{"crt","__p__winminor",0},{"crt","__p__osver",0},
        {"crt","_vacopy",2},{"crt","_heapadd",2},{"crt","_CRT_RTC_INIT",5},{"crt","_CRT_RTC_INITW",5},
        {"crt","__crtSleep",1},{"crt","__crtInitializeCriticalSectionEx",3},{"crt","__crtGetLocaleInfoEx",4},
    };return table;
}
void WindowsServices::initialize_crt_wctype(GuestMemory& memory){
    // _wctype／__pwctype_func：U+0000～U+00FF 的寬字元分類表，與 iswctype 的 Unicode 規則一致；水平 Tab 另含 _BLANK。
    for(unsigned c=0;c<256;++c){auto mask=windows_crt_unicode_mask(static_cast<char16_t>(c));if(c=='\t')mask|=0x40;memory.store(GuestAddress{runtime_base_.value+wctype_offset+c*2},16,mask);}
    // 同時建立舊版 CRT 的資料格：_pwctype 指向上述表；_amblksiz 預設 16；版本值與 _get_winver 等同一組常數。
    const auto cell=[&](unsigned offset){return GuestAddress{runtime_base_.value+offset};};
    memory.store(cell(pwctype_offset),x86_?32:64,runtime_base_.value+wctype_offset);memory.store(cell(amblksiz_offset),32,16);
    memory.store(cell(winver_offset),32,(compat_version_.major<<8)|compat_version_.minor);memory.store(cell(winmajor_offset),32,compat_version_.major);
    memory.store(cell(winminor_offset),32,compat_version_.minor);memory.store(cell(osver_offset),32,compat_version_.build);
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_runtime_extra(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
    // ---- 宿主端直接完成 ----
    if(name=="_seterrormode" || name=="_getdrives" || name=="_getdiskfree" || name=="_getdllprocaddr" || name=="_eof" || name=="_pipe" ||
       name.find("mktemp")!=name.npos || name.find("searchenv")!=name.npos || name=="__fpecode" || name=="__pxcptinfoptrs" ||
       name=="__pwctype_func" || name=="set_unexpected" || name=="_get_unexpected" ||
       name.starts_with("_get_os") || name.starts_with("_get_win") || name=="_set_SSE2_enable" || name.ends_with("_sbh_threshold") ||
       name.starts_with("__p__") || name=="_vacopy" || name=="_heapadd" || name.starts_with("_CRT_RTC_INIT")){
        auto& state=entry_state;auto& memory=entry_memory;const WindowsCall arg(state,memory,service);
        const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto set_errno=[&](std::uint32_t code){memory.store(GuestAddress{runtime_.errno_cell},32,code);};
        const auto invalid=[&](std::uint64_t result,std::uint32_t code=einval)->std::optional<std::uint32_t>{
            set_errno(code);
            if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
            return done(result);
        };
        const auto kernel=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"kernel32.dll",symbol,args);};
        const auto crt=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"crt",symbol,args);};
        const auto cell=[&](unsigned offset){return runtime_base_.value+offset;};
        GuestScratch temporary(*this,memory,state);
        try{
            if(name=="_seterrormode"){(void)kernel("SetErrorMode",{arg(0)});return done(0);}
            if(name.starts_with("_get_os") || name.starts_with("_get_win")){
                // errno_t _get_*(unsigned*)：VER_PLATFORM_WIN32_NT、組建號碼、(主版本<<8)|次版本、主版本、次版本。
                if(!arg(0))return invalid(einval);
                const std::uint32_t value=name=="_get_osplatform"?2U:name=="_get_osver"?compat_version_.build:name=="_get_winver"?(compat_version_.major<<8)|compat_version_.minor:
                    name=="_get_winmajor"?compat_version_.major:compat_version_.minor;
                memory.store(GuestAddress{arg(0)},32,value);return done(0);
            }
            if(name=="_set_SSE2_enable"){
                // 回傳設定後是否使用 SSE2 版本的 CRT 數學函式；客體 CPU 一律支援 SSE2。
                crt_sse2_enabled_=static_cast<std::uint32_t>(arg(0))?1U:0U;return done(crt_sse2_enabled_);
            }
            if(name=="_get_sbh_threshold")return done(crt_sbh_threshold_);
            if(name=="_set_sbh_threshold"){
                // 門檻上限 1016 位元組（0 停用 small-block heap）；超過時回傳 0 且維持原值。
                const auto value=arg(0);if(value>1016)return done(0);crt_sbh_threshold_=value;return done(1);
            }
            if(name=="_getdrives")return done(kernel("GetLogicalDrives",{}));
            if(name=="_getdiskfree"){
                // (drive 1＝A:，0＝目前磁碟, _diskfree_t*)：{total_clusters, avail_clusters, sectors_per_cluster, bytes_per_sector}。
                const auto drive=static_cast<std::uint32_t>(arg(0));const auto output=arg(1);if(!output || drive>26)return invalid(einval);
                const auto block=temporary.allocate(32);if(!block)return done(12);
                if(drive){memory.store(GuestAddress{block},8,'A'+drive-1);memory.store(GuestAddress{block+1},8,':');memory.store(GuestAddress{block+2},8,'\\');}
                if(!kernel("GetDiskFreeSpaceA",{drive?block:0,block+8,block+12,block+16,block+20})){const auto error=last_error_;crt_os_error::store(memory,runtime_.errno_cell,error);return done(error);}
                memory.store(GuestAddress{output},32,memory.load(GuestAddress{block+20},32));memory.store(GuestAddress{output+4},32,memory.load(GuestAddress{block+16},32));
                memory.store(GuestAddress{output+8},32,memory.load(GuestAddress{block+8},32));memory.store(GuestAddress{output+12},32,memory.load(GuestAddress{block+12},32));
                return done(0);
            }
            if(name=="_getdllprocaddr"){
                // (模組, 名稱, 序號)：有名稱時序號須為 -1；沒有名稱時以序號查詢。
                const auto module=arg(0),symbol=arg(1);const auto ordinal=arg(2);const auto none=x86_?UINT32_MAX:UINT64_MAX;
                if(symbol)return done(ordinal==none?kernel("GetProcAddress",{module,symbol}):0);
                if(ordinal==none || ordinal>0xffff)return done(0);
                return done(kernel("GetProcAddress",{module,ordinal}));
            }
            if(name=="_eof"){
                // 目前位置已到檔案長度回 1，否則 0；無效描述元回 -1（errno 由位置／長度查詢設定）。
                const auto failed=[&](std::uint64_t value){return x86_?value==UINT32_MAX:value==UINT64_MAX;};
                const auto position=crt("_telli64",{arg(0)});if(failed(position))return done(UINT32_MAX);
                const auto size=crt("_filelengthi64",{arg(0)});if(failed(size))return done(UINT32_MAX);
                return done(position>=size?1U:0U);
            }
            if(name=="_pipe"){
                // (int fds[2], psize, textmode)：CreatePipe 後以 _open_osfhandle 建立讀／寫描述元；_O_NOINHERIT（0x80）時不可繼承。
                const auto fds=arg(0);const auto size=static_cast<std::uint32_t>(arg(1)),mode=static_cast<std::uint32_t>(arg(2));
                // 允許 _O_TEXT／_O_BINARY／_O_WTEXT／_O_U16TEXT／_O_U8TEXT 與 _O_NOINHERIT。
                if(!fds || (mode&~0x7c080U))return invalid(UINT32_MAX);
                const auto block=temporary.allocate(64);if(!block)return done(UINT32_MAX);
                memory.store(GuestAddress{block+16},32,x86_?12:24);memory.store(GuestAddress{block+16+unit*2},32,(mode&0x80U)?0U:1U);
                if(!kernel("CreatePipe",{block,block+8,block+16,size})){crt_os_error::store(memory,runtime_.errno_cell,last_error_);return done(UINT32_MAX);}
                const auto flags=mode&0x7c000U;
                const auto read=crt("_open_osfhandle",{memory.load(GuestAddress{block},bits),flags|0x0U});
                const auto write=crt("_open_osfhandle",{memory.load(GuestAddress{block+8},bits),flags|0x1U});
                if(static_cast<std::int32_t>(read)<0 || static_cast<std::int32_t>(write)<0)return done(UINT32_MAX);
                memory.store(GuestAddress{fds},32,read);memory.store(GuestAddress{fds+4},32,write);return done(0);
            }
            if(name.find("mktemp")!=name.npos){
                // 範本以六個 X 結尾：第一個 X 依序換成 a～z，其餘五個是執行緒識別碼；選第一個不存在的名稱。
                const bool wide=name.starts_with("_w"),secure=name.ends_with("_s");const unsigned width=wide?2U:1U;
                const auto text=arg(0);const auto limit=secure?arg(1):UINT64_MAX;
                if(!text || (secure && !limit))return secure?invalid(einval):invalid(0);
                std::uint64_t length=0;while(length<limit && memory.load(GuestAddress{text+length*width},static_cast<std::uint8_t>(width*8)))++length;
                if(secure && length+1>limit)return invalid(einval);
                bool shaped=length>=6;for(std::uint64_t i=length>=6?length-6:0;shaped && i<length;++i)shaped=memory.load(GuestAddress{text+i*width},static_cast<std::uint8_t>(width*8))=='X';
                if(!shaped)return secure?invalid(einval):invalid(0);
                auto number=thread_identifier(threads_.current)%100000U;
                for(unsigned i=0;i<5;++i){memory.store(GuestAddress{text+(length-1-i)*width},static_cast<std::uint8_t>(width*8),'0'+number%10);number/=10;}
                for(char letter='a';letter<='z';++letter){
                    memory.store(GuestAddress{text+(length-6)*width},static_cast<std::uint8_t>(width*8),static_cast<std::uint64_t>(letter));
                    if(static_cast<std::uint32_t>(kernel(wide?"GetFileAttributesW":"GetFileAttributesA",{text}))==UINT32_MAX)return done(secure?0:text);
                }
                memory.store(GuestAddress{text},static_cast<std::uint8_t>(width*8),0);set_errno(eexist);return done(secure?eexist:0);
            }
            if(name.find("searchenv")!=name.npos){
                // 先找目前目錄，再依環境變數列出的目錄（以 ; 分隔）尋找；找到時寫入完整路徑，否則寫入空字串並設 ENOENT。
                const bool wide=name.starts_with("_w"),secure=name.ends_with("_s");const unsigned width=wide?2U:1U;
                const auto file=arg(0),variable=arg(1),output=arg(2);const auto capacity=secure?arg(3):260;
                if(!output || !capacity)return secure?invalid(einval):invalid(0);
                memory.store(GuestAddress{output},static_cast<std::uint8_t>(width*8),0);
                if(!file || !variable)return secure?invalid(einval):invalid(0);
                const auto read=[&](std::uint64_t at){std::u16string text;for(unsigned i=0;i<32767;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{at+i*width},static_cast<std::uint8_t>(width*8)));if(!c)break;text+=c;}return wide?text:utf8_to_utf16_lossy(std::string(text.begin(),text.end()));};
                const auto name_text=read(file);if(name_text.empty()){set_errno(enoent);return done(secure?enoent:0);}
                const auto value_pointer=crt(wide?"_wgetenv":"getenv",{variable});const auto directories=value_pointer?read(value_pointer):std::u16string{};
                std::vector<std::u16string> candidates{name_text};
                for(std::size_t start=0;start<=directories.size();){const auto end=directories.find(u';',start);auto directory=directories.substr(start,end==directories.npos?directories.npos:end-start);
                    if(!directory.empty()){if(directory.back()!=u'\\' && directory.back()!=u'/')directory+=u'\\';candidates.push_back(directory+name_text);}
                    if(end==directories.npos)break;start=end+1;}
                for(const auto& candidate:candidates){
                    const auto path=temporary.text(candidate);if(!path)return done(secure?12:0);
                    if(static_cast<std::uint32_t>(kernel("GetFileAttributesW",{path}))==UINT32_MAX)continue;
                    const auto full=temporary.allocate(32768*2);if(!full)return done(secure?12:0);
                    const auto length=kernel("GetFullPathNameW",{path,32768,full,0});if(!length)continue;
                    const auto text=read_utf16(memory,GuestAddress{full},32767);
                    std::vector<std::uint8_t> bytes;
                    if(wide){bytes=utf16_bytes(text);}else{const auto narrow=utf16_to_utf8_lossy(text);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
                    if(bytes.size()/width>capacity){if(secure)return invalid(erange,erange);set_errno(erange);return done(0);}
                    memory.write(GuestAddress{output},bytes);return done(0);
                }
                set_errno(enoent);return done(secure?enoent:0);
            }
            if(name=="__fpecode" || name=="__pxcptinfoptrs"){
                // 每執行緒 16 位元組：+0 浮點例外碼（int）、+8 例外資訊指標（void*）。
                auto& thread=threads_.items.at(threads_.current);
                if(!thread.crt_signal_cells){thread.crt_signal_cells=allocate_heap(memory,16);if(!thread.crt_signal_cells)return done(0);memory.write(GuestAddress{thread.crt_signal_cells},std::array<std::uint8_t,16>{});}
                return done(thread.crt_signal_cells+(name=="__fpecode"?0U:8U));
            }
            // 與 _iob、_pctype、_pwctype、_dstbias、_amblksiz、_winver 等資料匯出同一位址。
            if(name=="__p__iob")return done(cell(0x800));
            if(name=="__p__pctype")return done(cell(136));
            if(name=="__p__pwctype")return done(cell(pwctype_offset));
            if(name=="__p__dstbias")return done(cell(dstbias_offset));
            if(name=="__p__amblksiz")return done(cell(amblksiz_offset));
            if(name=="__p__winver")return done(cell(winver_offset));
            if(name=="__p__winmajor")return done(cell(winmajor_offset));
            if(name=="__p__winminor")return done(cell(winminor_offset));
            if(name=="__p__osver")return done(cell(osver_offset));
            if(name=="_vacopy"){if(!arg(0))return invalid(0);memory.store(GuestAddress{arg(0)},bits,arg(1));return done(0);}
            // _heapadd 自 VC2005 起不再支援：一律回 -1 並設 errno 為 ENOSYS。
            if(name=="_heapadd"){set_errno(40);return done(UINT32_MAX);}
            // 未連結 RTC 支援時 _CRT_RTC_INIT 回傳 NULL（沒有錯誤報告函式）。
            if(name.starts_with("_CRT_RTC_INIT"))return done(0);
            if(name=="__pwctype_func")return done(cell(wctype_offset));
            if(name=="set_unexpected"){const auto previous=memory.load(GuestAddress{cell(unexpected_offset)},bits);memory.store(GuestAddress{cell(unexpected_offset)},bits,arg(0));return done(previous);}
            if(name=="_get_unexpected")return done(memory.load(GuestAddress{cell(unexpected_offset)},bits));
        }catch(const MemoryFault&){return invalid(0);}
        throw HostCallFault("未知的 CRT 執行階段補充服務");
    }
    // ---- 經既有入口續行（串流、程序、DLL 與等待）----
    return nested_continuation(entry_state,entry_memory,"CrtRuntimeExtra",[this,service,unit,bits](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall arg(state,memory,service);const auto name=service.symbol;
        const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto set_errno=[&](std::uint32_t code){memory.store(GuestAddress{runtime_.errno_cell},32,code);};
        const auto crt=[this](std::string_view symbol){return resolve("ucrtbase.dll",symbol).value;};
        const auto kernel=[this](std::string_view symbol){return resolve("kernel32.dll",symbol).value;};
        const auto stream=[&](unsigned index){return nested_service(state,memory,"crt","__acrt_iob_func",{index});};
        const auto fail=[&](std::uint64_t result,std::uint32_t code)->service_callback::Task<std::optional<std::uint32_t>>{
            set_errno(code);
            if(const auto handler=current_invalid_parameter_handler())(void)co_await service_callback::call(handler,{0,0,0,0,0});
            co_return done(result);
        };
        GuestScratch temporary(*this,memory,state);
        const auto weof=0xffffU;const auto eof=UINT32_MAX;
        if(name=="getwchar")co_return done(co_await service_callback::call(crt("fgetwc"),{stream(0)}));
        if(name=="putwchar")co_return done(co_await service_callback::call(crt("fputwc"),{arg(0)&0xffffU,stream(1)}));
        if(name=="gets" || name=="_getws"){
            // 讀到換行或檔尾；去掉換行字元。沒有讀到任何字元就到檔尾時回 NULL。以固定大小暫存區分段讀取，只把實際內容寫回呼叫端緩衝區。
            const bool wide=name=="_getws";const unsigned width=wide?2U:1U;const auto buffer=arg(0);if(!buffer)co_return co_await fail(0,einval);
            const auto block=temporary.allocate(4096*2);if(!block)co_return done(0);
            std::vector<std::uint8_t> line;bool any=false;
            for(;;){
                const auto result=co_await service_callback::call_value(crt(wide?"fgetws":"fgets"),{block,4096,stream(0)});
                if(!result)break;any=true;bool newline=false;
                for(std::uint64_t i=0;i<4096;++i){
                    const auto c=memory.load(GuestAddress{block+i*width},static_cast<std::uint8_t>(width*8));if(!c)break;
                    if(c=='\n'){newline=true;break;}
                    for(unsigned b=0;b<width;++b)line.push_back(static_cast<std::uint8_t>(c>>(b*8)));
                }
                if(newline)break;
            }
            if(!any)co_return done(0);
            for(unsigned b=0;b<width;++b)line.push_back(0);memory.write(GuestAddress{buffer},line);
            co_return done(buffer);
        }
        if(name=="gets_s"){
            // 整行放不下時清空緩衝區、丟棄該行其餘字元並回報 ERANGE。
            const auto buffer=arg(0),size=arg(1);if(!buffer || !size)co_return co_await fail(0,einval);
            const auto block=temporary.allocate(static_cast<std::size_t>(size)+2);if(!block)co_return done(0);
            const auto result=co_await service_callback::call_value(crt("fgets"),{block,size+1,stream(0)});
            if(!result){memory.store(GuestAddress{buffer},8,0);co_return done(0);}
            std::vector<std::uint8_t> line;for(std::uint64_t i=0;i<=size;++i){const auto c=static_cast<std::uint8_t>(memory.load(GuestAddress{block+i},8));if(!c)break;line.push_back(c);}
            if(!line.empty() && line.back()=='\n')line.pop_back();
            else if(line.size()>=size){
                for(;;){const auto c=static_cast<std::uint32_t>(co_await service_callback::call(crt("fgetc"),{stream(0)}));if(c=='\n' || c==eof)break;}
                memory.store(GuestAddress{buffer},8,0);co_return co_await fail(0,erange);
            }
            line.push_back(0);memory.write(GuestAddress{buffer},line);co_return done(buffer);
        }
        if(name=="_putws"){
            if(!arg(0))co_return co_await fail(weof,einval);
            if(static_cast<std::int32_t>(co_await service_callback::call(crt("fputws"),{arg(0),stream(1)}))<0)co_return done(weof);
            co_return done((co_await service_callback::call(crt("fputwc"),{'\n',stream(1)}))==weof?weof:0U);
        }
        if(name=="_getw"){
            // 讀一個 int（4 位元組）；不足時回 EOF。
            const auto block=temporary.allocate(8);if(!block || !arg(0))co_return done(eof);
            const auto count=co_await service_callback::call_value(crt("fread"),{block,1,4,arg(0)});
            co_return done(count==4?memory.load(GuestAddress{block},32):eof);
        }
        if(name=="_putw"){
            const auto block=temporary.allocate(8);if(!block || !arg(1))co_return done(eof);
            memory.store(GuestAddress{block},32,arg(0));
            const auto count=co_await service_callback::call_value(crt("fwrite"),{block,4,1,arg(1)});
            co_return done(count==1?static_cast<std::uint32_t>(arg(0)):eof);
        }
        if(name=="fread_s"){
            // (buffer, bufferSize, elementSize, count, stream)：要求超過緩衝區時清空緩衝區並回報 ERANGE。
            const auto buffer=arg(0),capacity=arg(1),element=arg(2),count=arg(3),file=arg(4);
            if(!element || !count)co_return done(0);
            if(!buffer || !file)co_return co_await fail(0,einval);
            if(count>capacity/element){memory.write(GuestAddress{buffer},std::vector<std::uint8_t>(static_cast<std::size_t>(std::min<std::uint64_t>(capacity,1U<<20)),0));co_return co_await fail(0,erange);}
            co_return done(co_await service_callback::call_value(crt("fread"),{buffer,element,count,file}));
        }
        if(name=="clearerr_s"){if(!arg(0))co_return co_await fail(einval,einval);(void)co_await service_callback::call(crt("clearerr"),{arg(0)});co_return done(0);}
        if(name=="freopen_s" || name=="_wfreopen_s"){
            const auto output=arg(0);if(!output)co_return co_await fail(einval,einval);
            const auto result=co_await service_callback::call_value(crt(name=="freopen_s"?"freopen":"_wfreopen"),{arg(1),arg(2),arg(3)});
            memory.store(GuestAddress{output},bits,result);
            co_return done(result?0U:static_cast<std::uint32_t>(memory.load(GuestAddress{runtime_.errno_cell},32)));
        }
        if(name.find("spawnl")!=name.npos || name.find("execl")!=name.npos){
            // 清單參數到 NULL 為止，e 版本的 envp 在 NULL 之後；轉成陣列後交給同名的 v 版本。
            const bool spawn=name.find("spawn")!=name.npos,environment=name.ends_with("e");
            const unsigned first=spawn?2U:1U;std::vector<std::uint64_t> values;
            for(unsigned i=0;i<4096;++i){const auto value=arg(first+i);if(x86_?!(value&UINT32_MAX):!value)break;values.push_back(x86_?value&UINT32_MAX:value);}
            if(values.size()==4096)co_return co_await fail(UINT32_MAX,einval);
            const auto envp=environment?arg(first+static_cast<unsigned>(values.size())+1):0;
            const auto array=temporary.allocate((values.size()+1)*unit);if(!array)co_return done(UINT32_MAX);
            for(std::size_t i=0;i<values.size();++i)memory.store(GuestAddress{array+i*unit},bits,values[i]);
            std::string target(name);target[target.find('l')]='v';
            std::vector<std::uint64_t> args;if(spawn)args.push_back(arg(0));args.push_back(arg(spawn?1:0));args.push_back(array);if(environment)args.push_back(envp);
            co_return done(co_await service_callback::call_value(crt(target),std::move(args)));
        }
        if(name=="_beep"){(void)co_await service_callback::call(kernel("Beep"),{arg(0),arg(1)});co_return done(0);}
        if(name=="_sleep" || name=="__crtSleep"){(void)co_await service_callback::call(kernel("Sleep"),{arg(0)});co_return done(0);}
        // __crt* 包裝：cdecl 轉呼叫同名 Win32 API（x86 的 Win32 API 為 stdcall，由服務入口各自清理）。
        if(name=="__crtInitializeCriticalSectionEx")co_return done(co_await service_callback::call_value(kernel("InitializeCriticalSectionEx"),{arg(0),arg(1),arg(2)}));
        if(name=="__crtGetLocaleInfoEx")co_return done(co_await service_callback::call_value(kernel("GetLocaleInfoEx"),{arg(0),arg(1),arg(2),arg(3)}));
        if(name=="_loaddll"){if(!arg(0))co_return done(0);co_return done(co_await service_callback::call_value(kernel("LoadLibraryA"),{arg(0)}));}
        if(name=="_unloaddll"){
            // 成功回 0，失敗回 GetLastError 的值。
            const auto ok=co_await service_callback::call(kernel("FreeLibrary"),{arg(0)});
            co_return done(ok?0U:last_error_);
        }
        throw HostCallFault("未知的 CRT 執行階段補充服務");
    });
}
}
