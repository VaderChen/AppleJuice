#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "x64arm/unicode.hpp"
#include "lz_compression.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace x64arm {
// 舊式檔案與裝置家族：LZ（SZDD／COMPRESS.EXE 格式）解壓、磁帶操作與執行緒 I/O 狀態。
// LZ 與磁帶經 ReadFile／WriteFile／OpenFile／DeviceIoControl 完成，沿用既有檔案、權限與裝置契約；解壓在宿主端原生執行。
std::span<const WindowsServiceDescriptor> legacy_device_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","LZInit",1},{"kernel32.dll","LZOpenFileA",3},{"kernel32.dll","LZOpenFileW",3},{"kernel32.dll","LZSeek",3},
        {"kernel32.dll","LZRead",3},{"kernel32.dll","LZClose",1},{"kernel32.dll","LZCopy",2},{"kernel32.dll","CopyLZFile",2},
        {"kernel32.dll","LZStart",0},{"kernel32.dll","LZDone",0},{"kernel32.dll","GetExpandedNameA",2},{"kernel32.dll","GetExpandedNameW",2},
        {"kernel32.dll","CreateTapePartition",4},{"kernel32.dll","EraseTape",3},{"kernel32.dll","PrepareTape",3},{"kernel32.dll","WriteTapemark",4},
        {"kernel32.dll","GetTapePosition",5},{"kernel32.dll","SetTapePosition",6},{"kernel32.dll","GetTapeParameters",4},
        {"kernel32.dll","SetTapeParameters",3},{"kernel32.dll","GetTapeStatus",1},
        {"kernel32.dll","GetThreadIOPendingFlag",2}
    };return table;
}
namespace {
constexpr std::uint32_t lz_bias=400,lz_states=16;
constexpr std::int32_t lz_bad_input=-1,lz_read_error=-3,lz_write_error=-4,lz_no_memory=-5,lz_bad_value=-7;
}
std::optional<std::uint32_t> WindowsServices::invoke_legacy_device(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="GetThreadIOPendingFlag"){
        // 該執行緒仍有尚未完成的同步或非同步檔案 I/O 時為 TRUE。
        const WindowsCall call(entry_state,entry_memory,service);std::uint64_t result=0;
        const auto id=thread_id(call(0));
        if(!id)last_error_=6;else if(!(thread_access(call(0))&0x840U))last_error_=5;else if(!call(1))last_error_=998;
        else {const bool pending=std::any_of(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.thread==id && !io.done;});
            try{entry_memory.store(GuestAddress{call(1)},32,pending?1:0);result=1;}catch(const MemoryFault&){last_error_=998;}}
        call.return_to(entry_state,result);return std::nullopt;
    }
    if(service.symbol=="LZStart" || service.symbol=="LZDone"){
        // Win32 的 LZ 不需要預先配置的多檔複製緩衝；LZStart 依文件回傳 1。
        const WindowsCall call(entry_state,entry_memory,service);call.return_to(entry_state,service.symbol=="LZStart"?1:0);return std::nullopt;
    }
    return service_callback::invoke<std::optional<std::uint32_t>>(
        this,threads_.current,entry_state,entry_memory,"LegacyDevice",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);const auto symbol=service.symbol;
            const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
            const auto api=[&](std::string_view name){return resolve("kernel32.dll",name).value;};
            const auto d=[&](unsigned i){return static_cast<std::uint32_t>(call(i));};
            GuestScratch scratch(*this,memory,state);
            // 磁帶：組出與 winioctl.h 相同的輸入結構呼叫 DeviceIoControl，回傳 Win32 錯誤碼；一般檔案依既有裝置契約回 ERROR_INVALID_FUNCTION。
            if(symbol.find("Tape")!=symbol.npos){
                const auto control=[&](std::uint32_t code,std::span<const std::uint8_t> input,std::uint64_t output,std::uint32_t output_size)->service_callback::Task<std::uint32_t>{
                    const auto buffer=scratch.allocate(std::max<std::size_t>(input.size(),1)+8);if(!buffer)co_return 8U;
                    if(!input.empty())memory.write(GuestAddress{buffer+8},input);
                    const auto ok=static_cast<std::uint32_t>(co_await service_callback::call(api("DeviceIoControl"),{call(0),code,input.empty()?0:buffer+8,input.size(),output,output_size,buffer,0}));
                    co_return ok?0U:last_error_;};
                const auto pack=[](std::initializer_list<std::uint32_t> words){std::vector<std::uint8_t> bytes;for(const auto w:words)for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(w>>(8*i)));return bytes;};
                if(symbol=="GetTapeStatus")co_return done(co_await control(0x1f4024,{},0,0));
                if(symbol=="EraseTape")co_return done(co_await control(0x1fc000,pack({d(1),d(2)?1U:0U}),0,0));
                if(symbol=="PrepareTape")co_return done(co_await control(0x1f4004,pack({d(1),d(2)?1U:0U}),0,0));
                if(symbol=="WriteTapemark")co_return done(co_await control(0x1fc008,pack({d(1),d(2),d(3)?1U:0U}),0,0));
                if(symbol=="CreateTapePartition")co_return done(co_await control(0x1fc028,pack({d(1),d(2),d(3)}),0,0));
                if(symbol=="SetTapePosition")co_return done(co_await control(0x1f4010,pack({d(1),d(2),d(3),d(4),d(5)?1U:0U,0}),0,0));
                if(symbol=="GetTapePosition"){
                    // TAPE_GET_POSITION：Type、Partition、Offset（LARGE_INTEGER）；成功後依呼叫端提供的指標寫回。
                    const auto output=scratch.allocate(16);if(!output)co_return done(8);memory.write(GuestAddress{output},pack({d(1),0,0,0}));
                    const auto error=co_await control(0x1f400c,pack({d(1),0,0,0}),output,16);
                    if(!error)for(unsigned i=0;i<3;++i)if(call(2+i))memory.store(GuestAddress{call(2+i)},32,memory.load(GuestAddress{output+4+i*4},32));
                    co_return done(error);
                }
                if(symbol=="GetTapeParameters"){
                    // GET_TAPE_MEDIA_INFORMATION（0）／GET_TAPE_DRIVE_INFORMATION（1）的輸出結構都是 32 位元組；不足回 ERROR_MORE_DATA。
                    if(d(1)>1 || !call(2))co_return done(87);
                    const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{call(2)},32));
                    if(size<32){memory.store(GuestAddress{call(2)},32,32);co_return done(234);}
                    co_return done(co_await control(d(1)?0x1f4014U:0x1f401cU,{},call(3),32));
                }
                if(symbol=="SetTapeParameters"){
                    // SET_TAPE_MEDIA_INFORMATION（0）為 4 位元組的區塊大小；SET_TAPE_DRIVE_INFORMATION（1）為 8 位元組的磁碟機參數。
                    if(d(1)>1 || !call(2))co_return done(87);std::vector<std::uint8_t> input(d(1)?8U:4U);memory.read(GuestAddress{call(2)},input);
                    co_return done(co_await control(d(1)?0x1fc018U:0x1f4020U,input,0,0));
                }
                throw HostCallFault("未知的磁帶服務");
            }
            // LZ：讀取 SZDD 檔頭；壓縮檔整份解壓到宿主端狀態，LZRead／LZSeek 由宿主緩衝提供；非壓縮檔沿用原本的 HFILE。
            const auto read_all=[&](std::uint64_t file,std::vector<std::uint8_t>& bytes)->service_callback::Task<bool>{
                constexpr std::uint32_t chunk=65536;const auto buffer=scratch.allocate(chunk+4);if(!buffer)co_return false;
                for(;;){
                    if(!static_cast<std::uint32_t>(co_await service_callback::call(api("ReadFile"),{file,buffer+4,chunk,buffer,0})))co_return false;
                    const auto moved=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer},32));if(!moved)co_return true;
                    const auto at=bytes.size();bytes.resize(at+moved);memory.read(GuestAddress{buffer+4},std::span(bytes).subspan(at));
                    if(bytes.size()>output_limit)co_return false;
                }
            };
            // 回傳 SZDD 檔頭（尚未壓縮時為空）；讀取失敗回 nullopt。檔案位置回到開頭。
            const auto header=[&](std::uint64_t file)->service_callback::Task<std::optional<std::vector<std::uint8_t>>>{
                const auto buffer=scratch.allocate(20);if(!buffer)co_return std::nullopt;
                if(static_cast<std::uint32_t>(co_await service_callback::call(api("SetFilePointer"),{file,0,0,0}))==UINT32_MAX)co_return std::nullopt;
                if(!static_cast<std::uint32_t>(co_await service_callback::call(api("ReadFile"),{file,buffer+4,14,buffer,0})))co_return std::nullopt;
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(memory.load(GuestAddress{buffer},32)));memory.read(GuestAddress{buffer+4},bytes);
                if(bytes.size()<14 || !std::equal(szdd_magic.begin(),szdd_magic.end(),bytes.begin()) || bytes[8]!='A'){
                    (void)co_await service_callback::call(api("SetFilePointer"),{file,0,0,0});co_return std::vector<std::uint8_t>{};}
                co_return bytes;
            };
            const auto initialize=[&](std::uint64_t file)->service_callback::Task<std::int32_t>{
                const auto head=co_await header(file);if(!head)co_return lz_bad_input;if(head->empty())co_return static_cast<std::int32_t>(file);
                std::uint32_t slot=0;while(slot<lz_states && runtime_.lz_files.contains(lz_bias+slot))++slot;if(slot==lz_states)co_return lz_no_memory;
                std::vector<std::uint8_t> compressed;if(!co_await read_all(file,compressed))co_return lz_read_error;
                const auto size=std::uint32_t{(*head)[10]}|(std::uint32_t{(*head)[11]}<<8)|(std::uint32_t{(*head)[12]}<<16)|(std::uint32_t{(*head)[13]}<<24);
                if(size>output_limit)co_return lz_no_memory;auto expanded=lz_expand(compressed,size);if(!expanded)co_return lz_read_error;
                runtime_.lz_files[lz_bias+slot]={file,std::move(*expanded),0};co_return static_cast<std::int32_t>(lz_bias+slot);
            };
            const auto lz=[&](std::uint64_t handle)->RuntimeState::LzFile*{const auto found=runtime_.lz_files.find(static_cast<std::uint32_t>(handle));return found==runtime_.lz_files.end()?nullptr:&found->second;};
            if(symbol=="LZInit")co_return done(static_cast<std::uint32_t>(co_await initialize(call(0))));
            if(symbol=="LZOpenFileA" || symbol=="LZOpenFileW"){
                // OpenFile 只有 ANSI 版本；W 版本以程序 ACP 轉換，名稱維持 ACP 位元組。找不到時改試最後一字元換成底線的壓縮檔名；唯讀開啟才經 LZInit。
                if(!call(0) || !call(1))co_return done(static_cast<std::uint32_t>(lz_bad_input));
                auto name=symbol.ends_with('W')?utf16_to_ansi(read_utf16(memory,GuestAddress{call(0)},260)):read_ansi(memory,call(0),260);
                const auto open=[&](const std::string& path)->service_callback::Task<std::uint64_t>{
                    const auto text=scratch.allocate(path.size()+1);if(!text)co_return UINT32_MAX;
                    std::vector<std::uint8_t> bytes(path.begin(),path.end());bytes.push_back(0);memory.write(GuestAddress{text},bytes);
                    co_return static_cast<std::uint32_t>(co_await service_callback::call(api("OpenFile"),{text,call(1),d(2)}));};
                auto file=co_await open(name);
                if(file==UINT32_MAX && !name.empty() && !(d(2)&0x1000U)){
                    // 壓縮檔名：副檔名不足三字元時補底線、否則最後一字元換成底線；沒有副檔名時加上「._」。
                    const auto base=name.find_last_of("\\/:");const auto dot=name.find('.',base==std::string::npos?0:base+1);auto compressed=name;
                    if(dot==std::string::npos)compressed+="._";else if(name.size()-dot-1<3)compressed+='_';else compressed.back()='_';
                    file=co_await open(compressed);}
                if(file==UINT32_MAX)co_return done(static_cast<std::uint32_t>(lz_bad_input));
                if((d(2)&~0x70U)!=0)co_return done(file); // 只有 OF_READ（共用模式不限）的開啟才解壓
                co_return done(static_cast<std::uint32_t>(co_await initialize(file)));
            }
            if(symbol=="LZRead"){
                const auto count=static_cast<std::int32_t>(d(2));
                if(auto* state_lz=lz(call(0))){
                    if(count<0)co_return done(static_cast<std::uint32_t>(lz_bad_value));
                    const auto available=state_lz->data.size()-std::min<std::size_t>(state_lz->position,state_lz->data.size());
                    const auto moved=std::min<std::size_t>(available,static_cast<std::size_t>(count));
                    if(moved){if(!call(1))co_return done(static_cast<std::uint32_t>(lz_bad_value));memory.write(GuestAddress{call(1)},std::span(state_lz->data).subspan(static_cast<std::size_t>(state_lz->position),moved));}
                    state_lz->position+=moved;co_return done(static_cast<std::uint32_t>(moved));
                }
                const auto moved=static_cast<std::uint32_t>(co_await service_callback::call(api("_lread"),{call(0),call(1),d(2)}));
                co_return done(moved==UINT32_MAX?static_cast<std::uint32_t>(lz_read_error):moved);
            }
            if(symbol=="LZSeek"){
                if(auto* state_lz=lz(call(0))){
                    const auto offset=static_cast<std::int64_t>(static_cast<std::int32_t>(d(1)));const auto origin=d(2);
                    const std::int64_t base=origin==0?0:origin==1?static_cast<std::int64_t>(state_lz->position):origin==2?static_cast<std::int64_t>(state_lz->data.size()):-1;
                    const auto target=base+offset;if(base<0 || target<0 || target>static_cast<std::int64_t>(state_lz->data.size()))co_return done(static_cast<std::uint32_t>(lz_bad_value));
                    state_lz->position=static_cast<std::uint64_t>(target);co_return done(static_cast<std::uint32_t>(target));
                }
                co_return done(static_cast<std::uint32_t>(co_await service_callback::call(api("_llseek"),{call(0),d(1),d(2)})));
            }
            if(symbol=="LZClose"){
                if(auto* state_lz=lz(call(0))){const auto file=state_lz->file;runtime_.lz_files.erase(static_cast<std::uint32_t>(call(0)));(void)co_await service_callback::call(api("_lclose"),{file});co_return done(0);}
                (void)co_await service_callback::call(api("_lclose"),{call(0)});co_return done(0);
            }
            if(symbol=="LZCopy" || symbol=="CopyLZFile"){
                // 來源若不是 LZ handle 先暫時初始化；回傳寫入的位元組數或 LZERROR_*。
                std::uint64_t source=call(0);bool temporary=false;
                if(!lz(source)){const auto initialized=co_await initialize(source);if(initialized<0)co_return done(static_cast<std::uint32_t>(initialized));
                    temporary=initialized!=static_cast<std::int32_t>(source);source=static_cast<std::uint32_t>(initialized);}
                std::vector<std::uint8_t> data;
                if(auto* state_lz=lz(source)){data=std::vector<std::uint8_t>(state_lz->data.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(state_lz->position,state_lz->data.size())),state_lz->data.end());state_lz->position=state_lz->data.size();}
                else if(!co_await read_all(source,data))co_return done(static_cast<std::uint32_t>(lz_read_error));
                if(temporary)runtime_.lz_files.erase(static_cast<std::uint32_t>(source));
                constexpr std::size_t chunk=65536;const auto buffer=scratch.allocate(chunk);if(!buffer)co_return done(static_cast<std::uint32_t>(lz_no_memory));
                for(std::size_t at=0;at<data.size();at+=chunk){
                    const auto part=std::min(chunk,data.size()-at);memory.write(GuestAddress{buffer},std::span(data).subspan(at,part));
                    const auto written=static_cast<std::uint32_t>(co_await service_callback::call(api("_lwrite"),{call(1),buffer,part}));
                    if(written!=part)co_return done(static_cast<std::uint32_t>(lz_write_error));
                }
                co_return done(static_cast<std::uint32_t>(data.size()));
            }
            if(symbol=="GetExpandedNameA" || symbol=="GetExpandedNameW"){
                // 以唯讀開啟來源讀取檔頭：壓縮檔依檔頭補回最後一字元，否則原樣複製；輸出緩衝依文件須能容納 MAX_PATH。
                const bool wide=symbol.ends_with('W');if(!call(0) || !call(1))co_return done(static_cast<std::uint32_t>(lz_bad_input));
                const auto name=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{call(0)},260)):ansi_to_utf8(read_ansi(memory,call(0),260));
                const auto info=scratch.allocate(136+name.size()+1);if(!info)co_return done(static_cast<std::uint32_t>(lz_no_memory));
                std::vector<std::uint8_t> text(name.begin(),name.end());text.push_back(0);memory.write(GuestAddress{info+136},text);
                const auto file=static_cast<std::uint32_t>(co_await service_callback::call(api("OpenFile"),{info+136,info,0}));
                if(file==UINT32_MAX)co_return done(static_cast<std::uint32_t>(lz_bad_input));
                const auto head=co_await header(file);(void)co_await service_callback::call(api("_lclose"),{file});
                if(!head)co_return done(static_cast<std::uint32_t>(lz_bad_input));
                const auto expanded=head->empty()?name:lz_expanded_name(name,static_cast<char>((*head)[9]));
                if(wide)memory.write(GuestAddress{call(1)},utf16_bytes(utf8_to_utf16(expanded)));
                else {std::vector<std::uint8_t> bytes(expanded.begin(),expanded.end());bytes.push_back(0);memory.write(GuestAddress{call(1)},bytes);}
                co_return done(1);
            }
            throw HostCallFault("未知的 LZ 服務");
        });
}
}
