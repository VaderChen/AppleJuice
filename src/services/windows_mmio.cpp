// WinMM 多媒體檔案 I/O（mmio）。內建 DOS（宿主檔案）與 MEM（記憶體）I/O 程序在宿主端執行；其餘 FOURCC 由 mmioInstallIOProc
// 安裝或 MMIOINFO.pIOProc 指定的客體 I/O 程序處理，讀寫、搜尋、chunk 與緩衝層都經同一組 read_at／write_at／length 送出
// MMIOM_READ／WRITE／SEEK，開啟與關閉送 MMIOM_OPEN／CLOSE，mmioRename 送 MMIOM_RENAME，mmioSendMessage 轉交自訂訊息。
// 交給客體程序的 MMIOINFO 由引擎配置並在開啟期間保留（程序在其中保存 lDiskOffset 與 adwInfo 狀態）。
// 檔名含「+」時以「+」之前副檔名的 FOURCC（轉大寫）選取已安裝的程序，完整檔名交給程序。
#include "x64arm/windows_services.hpp"
#include "x64arm/riff_chunk.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <bit>

namespace x64arm {
namespace {
constexpr std::uint32_t memory_proc=0x204d454d, disk_proc=0x20534f44, dirty=0x10000000;
constexpr std::uint32_t install_proc=0x10000, remove_proc=0x20000, find_proc=0x40000, unicode_proc=0x01000000;
constexpr std::uint32_t message_read=0, message_write=1, message_seek=2, message_open=3, message_close=4, message_rename=6;
constexpr std::uint64_t memory_limit=64U*1024U*1024U;
struct MmioError {std::uint32_t code;};
std::uint32_t open_error(std::uint32_t error){
    switch(error){case 2:return 257;case 3:return 267;case 5:return 268;case 8:return 258;case 32:return 269;default:return 272;}
}
void put(std::span<std::uint8_t> out,unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)out[offset+i]=static_cast<std::uint8_t>(value>>(i*8));}
std::uint64_t host_length(MultimediaFile& file){
    if(!file.native)return file.size;
    const auto info=file.native->information();if(info.error)throw MmioError{261};file.size=info.metadata.size;return file.size;
}
std::size_t host_read_at(MultimediaFile& file,GuestMemory& memory,std::uint64_t offset,std::span<std::uint8_t> bytes){
    if(file.mode==1)throw MmioError{261};
    const auto size=host_length(file);const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),offset<size?size-offset:0));
    if(!count)return 0;bytes=bytes.first(count);
    if(file.native){const auto result=file.native->read(bytes,offset);if(result.error)throw MmioError{261};return static_cast<std::size_t>(result.bytes);}
    if(file.buffer)memory.read(GuestAddress{file.buffer+offset},bytes);
    else std::copy_n(file.bytes.begin()+static_cast<std::ptrdiff_t>(offset),count,bytes.begin());return count;
}
std::size_t host_write_at(MultimediaFile& file,GuestMemory& memory,std::uint64_t offset,std::span<const std::uint8_t> bytes){
    if(file.mode==0 || offset>INT32_MAX || bytes.size()>INT32_MAX-offset)throw MmioError{262};
    if(bytes.empty())return 0;
    if(file.native){const auto result=file.native->write(bytes,offset);if(result.error)throw MmioError{262};file.size=std::max(file.size,offset+result.bytes);return static_cast<std::size_t>(result.bytes);}
    const auto end=offset+bytes.size();
    if(end>file.capacity){
        if(file.buffer || !file.growth || end>memory_limit)throw MmioError{264};
        const auto capacity=std::min(memory_limit,((end+file.growth-1)/file.growth)*file.growth);
        file.bytes.resize(static_cast<std::size_t>(capacity));file.capacity=static_cast<std::uint32_t>(capacity);
    }
    if(file.buffer)memory.write(GuestAddress{file.buffer+offset},bytes);
    else std::copy(bytes.begin(),bytes.end(),file.bytes.begin()+static_cast<std::ptrdiff_t>(offset));
    file.size=std::max(file.size,end);return bytes.size();
}
// mmioStringToFOURCC：前四個字元，不足以空白補齊；MMIO_TOUPPER 轉大寫。
std::uint32_t string_fourcc(std::u16string_view text,bool upper){
    std::uint32_t value=0;
    for(unsigned i=0;i<4;++i){
        auto c=i<text.size()?text[i]:u' ';if(c>=0x80)c=u'?';
        if(upper && c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
        value|=std::uint32_t{static_cast<std::uint8_t>(c)}<<(8*i);
    }
    return value;
}
std::int64_t signed_result(std::uint64_t value){return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
}
std::optional<std::uint32_t> WindowsServices::invoke_mmio(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name.starts_with("mmioStringToFOURCC")){
        const WindowsCall call(state,memory,service);std::u16string text;
        try{if(call(0))text=name.ends_with('W')?read_utf16(memory,GuestAddress{call(0)},64):utf8_to_utf16_lossy(read_ansi(memory,call(0),64));}catch(const MemoryFault&){}
        call.return_to(state,string_fourcc(text,(static_cast<std::uint32_t>(call(1))&0x10U)!=0));return std::nullopt;
    }
    if(name.starts_with("mmioInstallIOProc")){
        const WindowsCall call(state,memory,service);const auto fourcc=static_cast<std::uint32_t>(call(0)),flags=static_cast<std::uint32_t>(call(2));const auto procedure=call(1);
        auto& table=runtime_.multimedia_procedures;std::uint64_t result=0;
        const auto action=flags&(install_proc|remove_proc|find_proc);
        if(action==install_proc){if(procedure){table.insert(table.begin(),MultimediaProcedure{fourcc,procedure,name.ends_with('W') || (flags&unicode_proc)});result=procedure;}}
        else if(action==remove_proc){const auto found=std::find_if(table.begin(),table.end(),[&](const auto& item){return item.fourcc==fourcc && item.procedure==procedure;});if(found!=table.end()){table.erase(found);result=procedure;}}
        else if(action==find_proc){const auto found=std::find_if(table.begin(),table.end(),[&](const auto& item){return item.fourcc==fourcc;});if(found!=table.end())result=found->procedure;}
        call.return_to(state,result);return std::nullopt;
    }
    return nested_continuation(state,memory,"MMIO",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
    const WindowsCall call(s,m,service);const auto name=service.symbol;
    const bool opening=name=="mmioOpenA" || name=="mmioOpenW";
    const bool counted=name=="mmioRead" || name=="mmioWrite" || name=="mmioSeek";
    const unsigned info_size=x86_?72U:100U,pointer_bytes=x86_?4U:8U,base_offset=x86_?24U:32U;const std::uint8_t pointer_bits=x86_?32:64;
    const unsigned disk_offset=base_offset+4*pointer_bytes+4,adw_offset=base_offset+4*pointer_bytes+8,handle_offset=x86_?68U:92U;
    const auto finish=[&](std::uint64_t result){call.return_to(s,result);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){
        if(opening){if(call(1))m.store(GuestAddress{call(1)+(x86_?12U:16U)},32,error);return finish(0);}
        return finish(counted?UINT32_MAX:error);
    };
    std::vector<std::uint64_t> blocks;
    struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
    const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw MmioError{258};blocks.push_back(block);return block;};
    std::uint64_t transfer=0;
    const auto transfer_block=[&]{if(!transfer)transfer=scratch(65536);return transfer;};
    const auto file_name=[&](std::u16string_view text,bool unicode)->std::uint64_t{
        std::vector<std::uint8_t> bytes;if(unicode)bytes=utf16_bytes(text);else {const auto narrow=utf16_to_utf8(text);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
        const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;
    };
    // 客體 I/O 程序呼叫：LRESULT IOProc(LPSTR lpmmioinfo, UINT uMsg, LPARAM lParam1, LPARAM lParam2)。
    const auto procedure_call=[&](const MultimediaFile& file,std::uint32_t message,std::uint64_t first,std::uint64_t second)->service_callback::Task<std::uint64_t>{
        co_return co_await service_callback::call_value(file.procedure,{file.procedure_info,message,first,second});
    };
    const auto seek_to=[&](MultimediaFile& file,std::uint64_t offset)->service_callback::Task<bool>{
        if(m.load(GuestAddress{file.procedure_info+disk_offset},32)==offset)co_return true;
        if(offset>INT32_MAX || signed_result(co_await procedure_call(file,message_seek,offset,0))!=static_cast<std::int64_t>(offset))throw MmioError{263};
        co_return true;
    };
    const auto read_at=[&](MultimediaFile& file,std::uint64_t offset,std::span<std::uint8_t> bytes)->service_callback::Task<std::size_t>{
        if(!file.procedure)co_return host_read_at(file,m,offset,bytes);
        if(file.mode==1)throw MmioError{261};
        std::size_t done=0;
        while(done<bytes.size()){
            (void)co_await seek_to(file,offset+done);
            const auto part=std::min<std::size_t>(65536,bytes.size()-done);const auto block=transfer_block();
            const auto got=signed_result(co_await procedure_call(file,message_read,block,part));if(got<0)throw MmioError{261};
            const auto count=std::min<std::size_t>(static_cast<std::size_t>(got),part);if(count)m.read(GuestAddress{block},bytes.subspan(done,count));
            done+=count;if(count<part)break;
        }
        co_return done;
    };
    const auto write_at=[&](MultimediaFile& file,std::uint64_t offset,std::span<const std::uint8_t> bytes)->service_callback::Task<std::size_t>{
        if(!file.procedure)co_return host_write_at(file,m,offset,bytes);
        if(file.mode==0 || offset>INT32_MAX || bytes.size()>INT32_MAX-offset)throw MmioError{262};
        std::size_t done=0;
        while(done<bytes.size()){
            (void)co_await seek_to(file,offset+done);
            const auto part=std::min<std::size_t>(65536,bytes.size()-done);const auto block=transfer_block();
            m.write(GuestAddress{block},std::vector<std::uint8_t>(bytes.begin()+static_cast<std::ptrdiff_t>(done),bytes.begin()+static_cast<std::ptrdiff_t>(done+part)));
            const auto wrote=signed_result(co_await procedure_call(file,message_write,block,part));if(wrote<0)throw MmioError{262};
            done+=std::min<std::size_t>(static_cast<std::size_t>(wrote),part);if(static_cast<std::size_t>(wrote)<part)break;
        }
        co_return done;
    };
    const auto length=[&](MultimediaFile& file)->service_callback::Task<std::uint64_t>{
        if(!file.procedure)co_return host_length(file);
        const auto saved=m.load(GuestAddress{file.procedure_info+disk_offset},32);
        const auto end=signed_result(co_await procedure_call(file,message_seek,0,2));if(end<0)throw MmioError{263};
        if(signed_result(co_await procedure_call(file,message_seek,saved,0))<0)throw MmioError{263};
        co_return static_cast<std::uint64_t>(end);
    };
    // I/O 程序：明確的 pIOProc、fccIOProc（已安裝或內建 DOS／MEM），或檔名「+」之前副檔名的 FOURCC。
    struct Selection {std::uint32_t fourcc{};std::uint64_t procedure{};bool unicode{};bool found{};};
    const auto select=[&](std::uint64_t info,std::u16string_view path)->Selection{
        Selection result;
        if(info){result.fourcc=static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32));result.procedure=m.load(GuestAddress{info+8},pointer_bits);}
        if(result.procedure){result.found=true;return result;}
        if(!result.fourcc){
            const auto plus=path.find(u'+');if(plus==std::u16string_view::npos){result.fourcc=disk_proc;result.found=true;return result;}
            const auto dot=path.substr(0,plus).find_last_of(u'.');if(dot==std::u16string_view::npos)return result;
            result.fourcc=string_fourcc(path.substr(dot+1,plus-dot-1),true);
        }
        if(result.fourcc==disk_proc || result.fourcc==memory_proc){result.found=true;return result;}
        const auto& table=runtime_.multimedia_procedures;
        const auto found=std::find_if(table.begin(),table.end(),[&](const auto& item){return item.fourcc==result.fourcc;});
        if(found!=table.end()){result.procedure=found->procedure;result.unicode=found->unicode;result.found=true;}
        return result;
    };
    try{
        if(opening){
            const auto flags=static_cast<std::uint32_t>(call(2)),mode=flags&3U,sharing=(flags>>4)&7U;
            const auto info=call(1);if(info)m.check(GuestAddress{info},info_size,Access::Write);
            if(mode==3 || sharing>4 || (flags&~0x11073U))co_return fail(11);
            if(runtime_.multimedia_files.size()>=4096)co_return fail(271);
            const bool wide=name.ends_with('W');
            const std::u16string path=call(0)?(wide?read_utf16(m,GuestAddress{call(0)}):utf8_to_utf16_lossy(read_ansi(m,call(0)))):std::u16string{};
            const auto selection=select(info,path);if(!selection.found)co_return fail(259);
            MultimediaFile file;file.mode=mode;file.open_flags=flags;file.memory=!selection.procedure && selection.fourcc==memory_proc;
            const auto handle=runtime_.allocate_wait_handle();
            if(selection.procedure){
                file.procedure=selection.procedure;file.procedure_fourcc=selection.fourcc;file.unicode_procedure=selection.unicode;
                file.procedure_info=allocate_heap(m,info_size);if(!file.procedure_info)co_return fail(258);
                std::vector<std::uint8_t> internal(info_size);put(internal,0,flags);put(internal,4,selection.fourcc);
                for(unsigned i=0;i<pointer_bytes;++i)internal[8+i]=static_cast<std::uint8_t>(file.procedure>>(8*i));
                if(info)for(unsigned i=0;i<12;++i)internal[adw_offset+i]=static_cast<std::uint8_t>(m.load(GuestAddress{info+adw_offset+i},8));
                for(unsigned i=0;i<pointer_bytes;++i)internal[handle_offset+i]=static_cast<std::uint8_t>(handle>>(8*i));
                m.write(GuestAddress{file.procedure_info},internal);
                const auto error=static_cast<std::uint32_t>(co_await procedure_call(file,message_open,call(0)?file_name(path,file.unicode_procedure):0,0));
                if(error){(void)free_heap(m,file.procedure_info,s);co_return fail(error);}
            }else if(file.memory){
                if(call(0))co_return fail(11);
                const auto count=m.load(GuestAddress{info+(x86_?20U:28U)},32);
                file.buffer=m.load(GuestAddress{info+(x86_?24U:32U)},pointer_bits);
                file.growth=static_cast<std::uint32_t>(m.load(GuestAddress{info+(x86_?48U:72U)},32));
                if(count>memory_limit || (file.buffer && file.growth))co_return fail(11);
                file.capacity=static_cast<std::uint32_t>(count?count:8192);
                if(file.buffer){if(!count)co_return fail(11);m.check(GuestAddress{file.buffer},file.capacity,mode?Access::Write:Access::Read);}
                else {file.bytes.resize(file.capacity);if(!file.growth)file.growth=8192;}
                file.size=flags&0x1000U?0:file.capacity;
            }else{
                if(!call(0))co_return fail(259);
                const auto host_path=utf16_to_utf8(path);
                if(!runtime_.network.external_io && (mode || (flags&0x1000U)))throw HostCallFault("比較模式不允許 MMIO 修改宿主檔案");
                const auto share=sharing==1?0U:sharing==2?1U:sharing==3?2U:3U;
                const auto opened=!filesystem_.writable() && !mode && !(flags&0x1000U)?filesystem_.open_reader(host_path,{}, {},share):
                    filesystem_.open_host(host_path,(mode!=1?0x80000000U:0U)|(mode?0x40000000U:0U),(flags&0x1000U)?2U:3U,0x80,share);
                if(opened.error)co_return fail(open_error(opened.error));
                if(opened.metadata.attributes&16U)co_return fail(272);
                file.native=opened.file;file.key=opened.key;file.size=opened.metadata.size;
            }
            // 直接存取緩衝區與記憶體檔案的底層資料分開，避免移動視窗覆蓋來源。
            const auto abandon=[&]()->service_callback::Task<bool>{if(file.procedure){(void)co_await procedure_call(file,message_close,0,0);(void)free_heap(m,file.procedure_info,s);}co_return true;};
            if(file.memory || (flags&0x10000U)){
                file.io_capacity=file.memory?std::min(file.capacity,8192U):8192U;
                file.io_buffer=allocate_heap(m,file.io_capacity);if(!file.io_buffer){(void)co_await abandon();co_return fail(258);}
                file.io_owned=true;
            }else if(info){
                file.io_capacity=static_cast<std::uint32_t>(m.load(GuestAddress{info+(x86_?20U:28U)},32));
                file.io_buffer=m.load(GuestAddress{info+(x86_?24U:32U)},pointer_bits);
                if(file.io_capacity>memory_limit || (file.io_capacity && !file.io_buffer)){(void)co_await abandon();co_return fail(11);}
                if(file.io_capacity)m.check(GuestAddress{file.io_buffer},file.io_capacity,Access::Write);
            }
            runtime_.multimedia_files.emplace(handle,std::move(file));
            if(info)m.store(GuestAddress{info+(x86_?12U:16U)},32,0);co_return finish(handle);
        }
        if(name.starts_with("mmioRename")){
            // MMIOM_RENAME 交給選定的 I/O 程序；內建 DOS 程序以 MoveFileW 改名，MEM 檔案沒有名稱。
            if(!call(0) || !call(1) || static_cast<std::uint32_t>(call(3)))co_return finish(11);
            const bool wide=name.ends_with('W');
            const auto text=[&](std::uint64_t address){return wide?read_utf16(m,GuestAddress{address}):utf8_to_utf16_lossy(read_ansi(m,address));};
            const auto from=text(call(0)),to=text(call(1));const auto info=call(2);
            if(info)m.check(GuestAddress{info},info_size,Access::Read);
            const auto selection=select(info,from);if(!selection.found)co_return finish(259);
            if(selection.procedure){
                MultimediaFile file;file.procedure=selection.procedure;file.procedure_info=scratch(info_size);
                std::vector<std::uint8_t> internal(info_size);put(internal,4,selection.fourcc);
                for(unsigned i=0;i<pointer_bytes;++i)internal[8+i]=static_cast<std::uint8_t>(selection.procedure>>(8*i));
                if(info)for(unsigned i=0;i<12;++i)internal[adw_offset+i]=static_cast<std::uint8_t>(m.load(GuestAddress{info+adw_offset+i},8));
                m.write(GuestAddress{file.procedure_info},internal);
                co_return finish(static_cast<std::uint32_t>(co_await procedure_call(file,message_rename,file_name(from,selection.unicode),file_name(to,selection.unicode))));
            }
            if(selection.fourcc==memory_proc)co_return finish(8);
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 MMIO 修改宿主檔案");
            if(co_await service_callback::call_value(resolve("kernel32.dll","MoveFileW").value,{file_name(from,true),file_name(to,true)}))co_return finish(0);
            co_return finish(open_error(static_cast<std::uint32_t>(co_await service_callback::call_value(resolve("kernel32.dll","GetLastError").value,{}))));
        }
        const auto found=runtime_.multimedia_files.find(call(0));if(found==runtime_.multimedia_files.end())co_return fail(5);
        const auto handle=call(0);
        // 客體程序可在回呼中開關其他 MMIO 檔案；每次使用前重新取得本檔案。
        const auto current=[&]()->MultimediaFile&{const auto again=runtime_.multimedia_files.find(handle);if(again==runtime_.multimedia_files.end())throw MmioError{5};return again->second;};
        if(name=="mmioClose"){
            if(call(1))co_return fail(11);
            auto& file=current();
            if(file.native && file.mode && file.native->flush())co_return fail(262);
            std::uint32_t error=0;
            if(file.procedure){error=static_cast<std::uint32_t>(co_await procedure_call(file,message_close,call(1),0));(void)free_heap(m,current().procedure_info,s);}
            if(current().io_owned)(void)free_heap(m,current().io_buffer,s);
            runtime_.multimedia_files.erase(handle);co_return finish(error);
        }
        if(name=="mmioSendMessage"){
            // 自訂訊息交給客體 I/O 程序；內建 DOS／MEM 程序不認得任何 mmioSendMessage 訊息，回 0。
            auto& file=current();if(!file.procedure)co_return finish(0);
            co_return finish(co_await procedure_call(file,static_cast<std::uint32_t>(call(1)),call(2),call(3)));
        }
        if(name=="mmioFlush"){
            // 緩衝區的變更在 mmioSetInfo／mmioAdvance 時即寫出，這裡只把宿主檔案寫入磁碟；MMIO_EMPTYBUF 丟棄緩衝內容。
            const auto flags=static_cast<std::uint32_t>(call(1));if(flags&~0x10U)co_return fail(11);
            auto& file=current();
            if(file.native && file.mode && file.native->flush())co_return fail(262);
            if(flags&0x10U){file.io_ready=false;file.io_valid=0;file.io_next=0;}
            co_return finish(0);
        }
        if(name=="mmioSetBuffer"){
            // 換成呼叫者的緩衝區（pchBuffer）、由 MMIO 配置（pchBuffer 為 NULL）或取消緩衝（cchBuffer 為 0）；同時更新 MMIO_ALLOCBUF。
            const auto buffer=call(1);const auto capacity=static_cast<std::uint32_t>(call(2));
            if(call(3) || capacity>memory_limit)co_return fail(11);
            if(buffer && capacity)m.check(GuestAddress{buffer},capacity,Access::Write);
            std::uint64_t replacement=buffer;
            if(!buffer && capacity){replacement=allocate_heap(m,capacity);if(!replacement)co_return fail(258);}
            auto& file=current();
            if(file.io_owned)(void)free_heap(m,file.io_buffer,s);
            file.io_buffer=capacity?replacement:0;file.io_capacity=capacity;file.io_owned=!buffer && capacity;
            file.io_ready=false;file.io_valid=0;file.io_next=0;file.io_offset=file.position;
            file.open_flags=file.io_owned?(file.open_flags|0x10000U):(file.open_flags&~0x10000U);
            co_return finish(0);
        }
        if(name=="mmioGetInfo" || name=="mmioSetInfo" || name=="mmioAdvance"){
            auto& file=current();
            const auto info=call(1),flags=call(2);const bool advancing=name=="mmioAdvance",getting=name=="mmioGetInfo";
            if((advancing?flags>1:flags!=0) || (!advancing && !info))co_return fail(11);
            if(info)m.check(GuestAddress{info},info_size,getting||advancing?Access::Write:Access::Read);
            if(advancing && !file.io_capacity)co_return fail(266);
            if(!getting && info){
                const auto buffer=m.load(GuestAddress{info+base_offset},pointer_bits);
                const auto next=m.load(GuestAddress{info+base_offset+pointer_bytes},pointer_bits);
                const auto info_flags=m.load(GuestAddress{info},32);
                if(buffer!=file.io_buffer || next<buffer || next-buffer>file.io_capacity ||
                   m.load(GuestAddress{info+(x86_?20U:28U)},32)!=file.io_capacity ||
                   (info_flags&~dirty)!=file.open_flags)co_return fail(11);
                if(!file.io_ready && file.io_capacity)co_return fail(11);
                const auto consumed=next-buffer;
                if(!file.mode && consumed>file.io_valid)co_return fail(11);
                if(info_flags&dirty){
                    if(file.native && !runtime_.network.external_io)throw HostCallFault("比較模式不允許 MMIO 寫入宿主檔案");
                    std::vector<std::uint8_t> data(static_cast<std::size_t>(consumed));m.read(GuestAddress{buffer},data);
                    if(co_await write_at(current(),current().io_offset,data)!=data.size())co_return fail(262);
                    current().io_valid=std::max(current().io_valid,static_cast<std::uint32_t>(consumed));
                }
                current().io_next=consumed;current().position=current().io_offset+consumed;
            }else if(advancing && file.io_ready){file.position=file.io_offset+(flags?file.io_next:file.io_valid);}
            if(getting || advancing){
                {auto& state_file=current();state_file.io_offset=state_file.position;state_file.io_next=0;state_file.io_valid=0;}
                if(current().io_capacity){
                    if((getting && current().mode!=1) || (advancing && flags==0)){
                        std::vector<std::uint8_t> data(current().io_capacity);
                        const auto valid=static_cast<std::uint32_t>(co_await read_at(current(),current().io_offset,data));
                        current().io_valid=valid;m.write(GuestAddress{current().io_buffer},std::span(data).first(valid));
                    }
                    current().io_ready=true;
                }
                if(info){
                    const auto& value=current();
                    m.write(GuestAddress{info},std::vector<std::uint8_t>(info_size));
                    m.store(GuestAddress{info},32,value.open_flags);
                    m.store(GuestAddress{info+4},32,value.procedure?value.procedure_fourcc:value.memory?memory_proc:disk_proc);
                    if(value.procedure){
                        m.store(GuestAddress{info+8},pointer_bits,value.procedure);
                        for(unsigned i=0;i<12;i+=4)m.store(GuestAddress{info+adw_offset+i},32,m.load(GuestAddress{value.procedure_info+adw_offset+i},32));
                    }
                    m.store(GuestAddress{info+(x86_?20U:28U)},32,value.io_capacity);
                    m.store(GuestAddress{info+base_offset},pointer_bits,value.io_buffer);
                    m.store(GuestAddress{info+base_offset+pointer_bytes},pointer_bits,value.io_buffer);
                    m.store(GuestAddress{info+base_offset+2*pointer_bytes},pointer_bits,value.io_buffer+value.io_valid);
                    m.store(GuestAddress{info+base_offset+3*pointer_bytes},pointer_bits,value.io_buffer+value.io_capacity);
                    m.store(GuestAddress{info+base_offset+4*pointer_bytes},32,value.io_offset);
                    m.store(GuestAddress{info+disk_offset},32,value.io_offset+value.io_valid);
                    m.store(GuestAddress{info+handle_offset},pointer_bits,handle);
                }
            }
            co_return finish(0);
        }
        current().io_ready=false;
        if(name=="mmioRead" || name=="mmioWrite"){
            const auto count=static_cast<std::uint32_t>(call(2));if(count>INT32_MAX)co_return fail(11);
            const bool writing=name=="mmioWrite";
            {const auto& file=current();if((writing && !file.mode) || (!writing && file.mode==1))co_return fail(writing?262:261);
                if(writing && !runtime_.network.external_io && file.native)throw HostCallFault("比較模式不允許 MMIO 寫入宿主檔案");}
            // 內建程序依檔案長度截短讀取；客體程序自行回報實際讀到的位元組數。
            std::uint64_t requested=count;
            if(!writing && !current().procedure){const auto size=host_length(current());requested=std::min<std::uint64_t>(count,current().position<size?size-current().position:0);}
            m.check(GuestAddress{call(1)},static_cast<std::size_t>(requested),writing?Access::Read:Access::Write);
            std::vector<std::uint8_t> buffer(65536);std::uint64_t done=0;
            while(done<requested){
                auto part=std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),requested-done)));
                if(writing)m.read(GuestAddress{call(1)+done},part);
                const auto position=current().position;
                const auto actual=writing?co_await write_at(current(),position,part):co_await read_at(current(),position,part);
                if(!writing && actual)m.write(GuestAddress{call(1)+done},part.first(actual));
                current().position+=actual;done+=actual;if(actual<part.size())break;
            }
            co_return finish(done);
        }
        if(name=="mmioSeek"){
            const auto origin=static_cast<std::uint32_t>(call(2));if(origin>2)co_return fail(11);
            const auto offset=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(call(1)));
            if(current().procedure){
                // 客體程序直接處理搜尋並回報新位置。
                const auto target=origin==1?static_cast<std::int64_t>(current().position)+offset:static_cast<std::int64_t>(offset);
                const auto moved=signed_result(co_await procedure_call(current(),message_seek,static_cast<std::uint32_t>(origin==1?static_cast<std::int32_t>(target):offset),origin==1?0:origin));
                if(moved<0)co_return fail(263);current().position=static_cast<std::uint64_t>(moved);co_return finish(current().position);
            }
            const auto position=static_cast<std::int64_t>(origin==1?current().position:origin==2?host_length(current()):0)+offset;
            if(position<0 || position>INT32_MAX)co_return fail(263);current().position=static_cast<std::uint64_t>(position);co_return finish(current().position);
        }
        if(!call(1))co_return fail(11);m.check(GuestAddress{call(1)},20,Access::Write);
        const auto field=[&](unsigned offset){return static_cast<std::uint32_t>(m.load(GuestAddress{call(1)+offset},32));};
        const auto save=[&](unsigned offset,std::uint64_t value){m.store(GuestAddress{call(1)+offset},32,value);};
        if(name=="mmioDescend"){
            const auto flags=static_cast<std::uint32_t>(call(3));if(flags && flags!=0x10U && flags!=0x20U && flags!=0x40U)co_return fail(11);
            auto limit=co_await length(current());if(call(2)){
                const auto parent=call(2),begin=m.load(GuestAddress{parent+12},32),size=m.load(GuestAddress{parent+4},32);
                if(current().position<begin)co_return fail(265);limit=std::min(limit,begin+size);
            }
            const auto id=flags==0x20U?riff::riff_id:flags==0x40U?riff::list_id:field(0),type=field(8);
            while(current().position<limit){
                std::array<std::uint8_t,8> header{};if(co_await read_at(current(),current().position,header)!=8)co_return fail(265);
                const auto chunk=riff::chunk(header,current().position,limit);const bool container=chunk.id==riff::riff_id || chunk.id==riff::list_id;
                std::uint32_t form=0;if(container){if(chunk.size<4)co_return fail(265);std::array<std::uint8_t,4> bytes{};if(co_await read_at(current(),chunk.data,bytes)!=4)co_return fail(265);form=riff::word(bytes);}
                if(!flags || (chunk.id==id && (flags==0x10U || form==type))){
                    if(chunk.data>UINT32_MAX)co_return fail(263);
                    save(0,chunk.id);save(4,chunk.size);save(8,form);save(12,chunk.data);save(16,0);current().position=chunk.data+(container?4:0);co_return finish(0);
                }
                current().position=chunk.next;
            }
            co_return fail(265);
        }
        if(current().native && !runtime_.network.external_io && (name=="mmioCreateChunk" || (field(16)&dirty)))throw HostCallFault("比較模式不允許 MMIO 更新宿主 chunk");
        if(name=="mmioCreateChunk"){
            const auto flags=static_cast<std::uint32_t>(call(2));if(flags && flags!=0x20U && flags!=0x40U)co_return fail(11);
            const auto id=flags==0x20U?riff::riff_id:flags==0x40U?riff::list_id:field(0);
            const auto count=id==riff::riff_id || id==riff::list_id?12U:8U;
            std::array<std::uint8_t,12> bytes{};put(bytes,0,id);put(bytes,4,field(4));put(bytes,8,field(8));
            if(co_await write_at(current(),current().position,std::span(bytes).first(count))!=count)co_return fail(262);
            save(0,id);save(12,current().position+8);save(16,dirty);current().position+=count;co_return finish(0);
        }
        if(name=="mmioAscend"){
            if(call(2) || (field(16)&~dirty))co_return fail(11);
            const auto begin=std::uint64_t{field(12)};auto size=std::uint64_t{field(4)};
            if(field(16)&dirty){
                if(begin<8 || current().position<begin || current().position-begin>UINT32_MAX)co_return fail(263);
                size=current().position-begin;std::array<std::uint8_t,4> bytes{};put(bytes,0,static_cast<std::uint32_t>(size));
                if(co_await write_at(current(),begin-4,bytes)!=4)co_return fail(262);
                if(size&1U){const std::array<std::uint8_t,1> pad{};if(co_await write_at(current(),begin+size,pad)!=1)co_return fail(262);}
                save(4,size);save(16,0);
            }
            const auto next=begin+size+(size&1U);if(next>INT32_MAX)co_return fail(263);current().position=next;co_return finish(0);
        }
        co_return fail(8);
    }catch(const MmioError& error){co_return fail(error.code);}catch(const std::invalid_argument&){co_return fail(name=="mmioDescend"?265:11);}
    catch(const std::bad_alloc&){co_return fail(258);}catch(const MemoryFault&){if(opening){last_error_=998;co_return finish(0);}co_return fail(11);}
    });
}
}
