// CRT 主控台讀行 _cgets／_cgets_s／_cgetws／_cgetws_s（msvcrt／msvcr*／ucrtbase 共用 crt 家族）：
// 開啟 CONIN$（不是主控台時用標準輸入的主控台 handle），暫時加上 ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT，以 ReadConsoleA／W 讀一行（沿用主控台的
// 行編輯與回顯），去掉結尾 CR LF 後還原模式。容量為 size（含結尾 NUL），最多保存 size-1 個字元，超出的部分捨棄。
// _cgets／_cgetws 的 buffer[0] 為容量、buffer[1] 回報讀到的字元數、字串自 buffer+2 起，回傳 buffer+2。沒有主控台時失敗（_cgets 回 NULL）。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> crt_console_read_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"crt","_cgets",1},{"crt","_cgets_s",3},{"crt","_cgetws",1},{"crt","_cgetws_s",3}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_console_read(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // 協程恢復時會重新進入本入口，所有驗證與副作用都放在協程內，只在第一次執行。
    return nested_continuation(state,memory,"CRT 主控台讀行",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto name=service.symbol;const bool wide=name.starts_with("_cgetws"),secure=name.ends_with("_s");const unsigned unit=wide?2U:1U;
        // 無效參數：errno＝EINVAL，有處理器時呼叫後回傳失敗值，沒有處理器時與其他 CRT 函式相同以 STATUS_INVALID_CRUNTIME_PARAMETER 結束。
        const auto invalid=[&](std::uint64_t failure)->service_callback::Task<std::optional<std::uint32_t>>{
            m.store(GuestAddress{runtime_.errno_cell},32,22);
            if(const auto handler=current_invalid_parameter_handler()){(void)co_await service_callback::call_value(handler,{0,0,0,0,0});co_return finish(failure);}
            co_return std::optional<std::uint32_t>{0xc0000417U};
        };
        // 參數驗證與 _RESET_STRING 依 _s 版本順序：buffer、容量，清空字串後再驗證 sizeRead。
        std::uint64_t target=0,capacity=0,size_read=0;bool bad=false;
        try{
            if(secure){
                target=call(0);capacity=call(1);size_read=call(2);
                if(!target || !capacity)bad=true;
                else {m.store(GuestAddress{target},static_cast<std::uint8_t>(unit*8),0);if(!size_read)bad=true;}
            }else if(!call(0))bad=true;
            else {
                capacity=m.load(GuestAddress{call(0)},static_cast<std::uint8_t>(unit*8));target=call(0)+2*unit;
                if(!capacity){m.store(GuestAddress{call(0)+unit},static_cast<std::uint8_t>(unit*8),0);m.store(GuestAddress{target},static_cast<std::uint8_t>(unit*8),0);co_return finish(target);}
            }
        }catch(const MemoryFault&){bad=true;}
        if(bad)co_return co_await invalid(secure?22U:0U);
        const unsigned pointer=x86_?4U:8U;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));};
        const auto fail=[&](std::uint32_t error){m.store(GuestAddress{runtime_.errno_cell},32,error);return finish(secure?error:0);};
        const auto limit=std::min<std::uint64_t>(capacity-1,32766);
        constexpr std::u16string_view conin=u"CONIN$";const auto device=scratch(16);for(std::size_t i=0;i<conin.size();++i)m.store(GuestAddress{device+i*2},16,conin[i]);
        // CONIN$ 不是主控台時退回標準輸入的主控台 handle；兩者都不是主控台就失敗（EBADF）。
        const auto invalid_handle=x86_?UINT64_C(0xffffffff):UINT64_MAX;const auto mode=scratch(4);
        auto console=co_await api("kernel32.dll","CreateFileW",{device,0xc0000000U,3,0,3,0,0});bool opened=console && console!=invalid_handle;
        if(!opened || !static_cast<std::uint32_t>(co_await api("kernel32.dll","GetConsoleMode",{console,mode}))){
            if(opened)(void)co_await api("kernel32.dll","CloseHandle",{console});opened=false;
            console=co_await api("kernel32.dll","GetStdHandle",{static_cast<std::uint64_t>(x86_?UINT64_C(0xfffffff6):static_cast<std::uint64_t>(-10))});
            if(!console || console==invalid_handle || !static_cast<std::uint32_t>(co_await api("kernel32.dll","GetConsoleMode",{console,mode})))co_return fail(9);
        }
        const bool restore=true;const auto previous=m.load(GuestAddress{mode},32);
        // 行輸入＋回顯；ENABLE_PROCESSED_INPUT 沿用目前設定（本機終端機以 ISIG 對應，不能任意切換）。
        (void)co_await api("kernel32.dll","SetConsoleMode",{console,(previous&1U)|6U});
        // 多讀兩個字元的空間以收到結尾 CR LF。
        const auto room=limit+2;const auto buffer=scratch(static_cast<std::size_t>(room*unit)),count=scratch(4);
        const bool read=static_cast<std::uint32_t>(co_await api("kernel32.dll",wide?"ReadConsoleW":"ReadConsoleA",{console,buffer,room,count,0}))!=0;
        if(restore)(void)co_await api("kernel32.dll","SetConsoleMode",{console,previous});
        if(opened)(void)co_await api("kernel32.dll","CloseHandle",{console});
        if(!read)co_return fail(9);
        auto got=std::min<std::uint64_t>(m.load(GuestAddress{count},32),room);
        const auto at=[&](std::uint64_t i){return m.load(GuestAddress{buffer+i*unit},static_cast<std::uint8_t>(unit*8));};
        if(got && at(got-1)==u'\n')--got;if(got && at(got-1)==u'\r')--got;
        got=std::min(got,limit);
        for(std::uint64_t i=0;i<got;++i)m.store(GuestAddress{target+i*unit},static_cast<std::uint8_t>(unit*8),at(i));
        m.store(GuestAddress{target+got*unit},static_cast<std::uint8_t>(unit*8),0);
        if(secure){m.store(GuestAddress{size_read},static_cast<std::uint8_t>(pointer*8),got);co_return finish(0);}
        m.store(GuestAddress{target-unit},static_cast<std::uint8_t>(unit*8),got);co_return finish(target);
    });
}
}
