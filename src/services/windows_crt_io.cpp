#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "crt_c_locale.hpp"
#include "crt_os_error.hpp"
#include "crt_guest_buffer.hpp"
#include "crt_lowio_abi.hpp"
#include "crt_lowio_text.hpp"
#include "crt_open_mode.hpp"
#include "windows_locale_data.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
namespace {
// 只在失敗路徑讀取開關，不啟用會改變服務快速路徑的完整 API 追蹤。
void trace_crt_file_failure(std::string_view operation,const char* phase,std::string_view path,
                           std::uint32_t code,std::uint32_t native_error=0,
                           std::uint64_t requested=0,std::uint64_t transferred=0,
                           std::uint32_t stdio_limit=0,std::size_t stdio_used=0) {
    if(!std::getenv("X64ARM_TRACE_FILE_ERRORS"))return;
    std::fprintf(stderr,"[crt_file_error] operation=%.*s phase=%s path=%.*s errno=%u native_error=%u requested=%llu transferred=%llu stdio_limit=%u stdio_used=%zu\n",
        static_cast<int>(operation.size()),operation.data(),phase,static_cast<int>(path.size()),path.empty()?"":path.data(),
        code,native_error,static_cast<unsigned long long>(requested),static_cast<unsigned long long>(transferred),stdio_limit,stdio_used);
}
}
FilePositionReference WindowsServices::crt_file_position(CrtFile& file) {
    if(file.os_handle){const auto found=files_.find(file.os_handle);if(found!=files_.end())return FilePositionReference{runtime_.win_file_positions.at(found->second.object)};}
    if(file.standard_handle==0x100)return FilePositionReference{runtime_.standard_input.position};
    return FilePositionReference{file.description?runtime_.file_positions.at(file.description):file.position};
}
std::uint64_t WindowsServices::write_crt_file(GuestMemory& memory,CrtFile& file,std::span<const std::uint8_t> bytes,bool stream) {
    file.lowio_error=0;
    const auto failed=[&](std::uint32_t code){file.lowio_error=code;if(stream)file.error=true;memory.store(GuestAddress{runtime_.errno_cell},32,code);};
    if(!import_crt_lowio(memory,file) || (file.os_handle && !files_.contains(file.os_handle))){failed(9);return 0;}
    if(!file.writable) {failed(9);trace_crt_file_failure("write","not-writable",file.key,9,0,bytes.size());return 0;}
    const bool standard=file.standard_handle==0x101 || file.standard_handle==0x102;
    if(!standard && !file.native){failed(9);trace_crt_file_failure("write","no-native-file",file.key,9,0,bytes.size());return 0;}
    auto mode=crt_lowio::text_mode(file.lowio_extended);
    // 原生 console 的呈現介面接收 UTF-8；重新導向仍保留 descriptor 的磁碟格式。
    if(standard && file.text && mode!=crt_lowio::TextMode::Ansi && standard_file_type(static_cast<unsigned>(file.standard_handle-0x100))==2)mode=crt_lowio::TextMode::Utf8;
    std::vector<std::uint8_t> translated;
    if(file.text)try{translated=crt_lowio::encode_text(bytes,mode);}catch(const std::invalid_argument&){failed(bytes.size()%2?22U:42U);return 0;}
    if(standard){
        const auto written=write_standard_output(static_cast<unsigned>(file.standard_handle-0x100),file.text?std::span<const std::uint8_t>(translated):bytes,true);
        if(written.error){failed(crt_os_error::mapped(written.error));trace_crt_file_failure("write","standard",file.key,crt_os_error::mapped(written.error),written.error,bytes.size(),written.bytes);}
        return !file.text?written.bytes:written.bytes==translated.size()?bytes.size():crt_lowio::written_source(bytes,mode,static_cast<std::size_t>(written.bytes));
    }
    const auto data=file.text?std::span<const std::uint8_t>(translated):bytes;auto position=crt_file_position(file);const auto position_guard=position.lock();
    if(file.append)position=file.native->information().metadata.size;
    const auto written=file.native->write(data,position);position+=written.bytes;
    if(written.error){failed(crt_os_error::mapped(written.error));}
    if(written.error || written.bytes<data.size())trace_crt_file_failure("write","native",file.key,
        written.error?crt_os_error::mapped(written.error):0,written.error,data.size(),written.bytes);
    if(!file.text)return written.bytes;
    return written.bytes==data.size()?bytes.size():crt_lowio::written_source(bytes,mode,static_cast<std::size_t>(written.bytes));
}
std::uint64_t WindowsServices::write_crt_stream(GuestMemory& memory,std::uint64_t stream,std::span<const std::uint8_t> bytes) {
    auto* file=crt_file(stream);
    if(!file || !file->writable){memory.store(GuestAddress{runtime_.errno_cell},32,9);if(file)file->error=true;trace_crt_file_failure("write-stream","invalid-stream",file?file->key:std::string_view{},9,0,bytes.size());return 0;}
    if(!import_crt_lowio(memory,*file)){file->error=true;return 0;}
    import_crt_buffer(memory,*file,x86_);
    auto& buffer=file->buffer;
    if(!buffer.capacity)return write_crt_file(memory,*file,bytes,true);
    if(buffer.direction==CrtFile::Buffer::Direction::Read && !flush_crt_file(memory,*file))return 0;
    buffer.direction=CrtFile::Buffer::Direction::Write;
    std::size_t consumed=0;
    while(consumed<bytes.size()){
        if(buffer.used==buffer.capacity){crt_buffer_fields(memory,*file);if(!flush_crt_file(memory,*file,false))break;}
        buffer.direction=CrtFile::Buffer::Direction::Write;
        const auto count=std::min<std::size_t>(bytes.size()-consumed,buffer.capacity-buffer.used);
        memory.write(GuestAddress{buffer.address+buffer.used},bytes.subspan(consumed,count));buffer.used+=static_cast<std::uint32_t>(count);consumed+=count;
        if(buffer.used==buffer.capacity){crt_buffer_fields(memory,*file);if(!flush_crt_file(memory,*file,false))break;}
    }
    crt_buffer_fields(memory,*file);return consumed;
}

std::uint64_t WindowsServices::write_crt_wide_stream(GuestState& state,GuestMemory& memory,std::uint64_t stream,std::u16string_view text) {
    stream=crt_stream(stream);auto* file=crt_file(stream);
    if(!file || !file->writable){memory.store(GuestAddress{runtime_.errno_cell},32,9);if(file)file->error=true;return 0;}
    if(!import_crt_lowio(memory,*file)){file->error=true;return 0;}
    const bool translated=file->text && crt_lowio::text_mode(file->lowio_extended)==crt_lowio::TextMode::Ansi;
    std::vector<std::uint8_t> bytes;
    const auto page=translated?current_crt_locale(state,memory).categories[2].codepage:0U;
    if(translated){
        const auto encoded=windows_crt_encode(text,page);
        if(!encoded){file->error=true;memory.store(GuestAddress{runtime_.errno_cell},32,42);return 0;}
        bytes=*encoded;
    }else{bytes.reserve(text.size()*2);for(auto character:text)crt_lowio::append_unit(bytes,character);}
    const auto written=bytes.empty()?0:write_crt_stream(memory,stream,bytes);
    if(written==bytes.size())return text.size();
    if(!translated)return written/2;
    std::size_t at=0,characters=0;
    while(at<written){const auto decoded=windows_crt_decode(std::span(bytes).subspan(at,static_cast<std::size_t>(written)-at),page);if(decoded.status!=CrtDecodedCharacter::Status::Complete)break;at+=decoded.bytes;characters+=decoded.character>65535?2U:1U;}
    return characters;
}
std::vector<std::uint8_t> WindowsServices::read_crt_file(GuestMemory& memory,CrtFile& file,std::size_t requested,bool stream) {
    file.lowio_error=0;
    const auto failed=[&](std::uint32_t code){file.lowio_error=code;if(stream)file.error=true;memory.store(GuestAddress{runtime_.errno_cell},32,code);};
    if(!import_crt_lowio(memory,file) || (file.os_handle && !files_.contains(file.os_handle))){failed(9);return {};}
    std::vector<std::uint8_t> output;
    if(!file.readable){failed(9);return output;}
    if(stream)import_crt_buffer(memory,file,x86_);
    if(stream && file.buffer.direction==CrtFile::Buffer::Direction::Write && !flush_crt_file(memory,file))return output;
    auto position=crt_file_position(file);const auto position_guard=position.lock();
    if(stream)while(output.size()<requested && !file.pushback.empty()){output.push_back(file.pushback.back());file.pushback.pop_back();}
    if(file.lowio_eof || (stream && file.eof)){if(stream && output.empty())file.eof=true;return output;}
    const auto& snapshot=file.standard_handle==0x100?runtime_.standard_input.bytes:file.bytes;
    const auto read=[&](std::span<std::uint8_t> bytes,std::uint64_t offset) {
        if(file.native)return file.native->read(bytes,offset);
        const auto size=offset<snapshot.size()?std::min<std::uint64_t>(bytes.size(),snapshot.size()-offset):0;
        if(size)std::copy_n(snapshot.begin()+static_cast<std::ptrdiff_t>(offset),static_cast<std::size_t>(size),bytes.begin());process_io::record(process_io::Kind::Read,size);return FileIoResult{size,0};
    };
    auto text_mode=crt_lowio::text_mode(file.lowio_extended);file.lowio_needed_input=0;
    if(file.standard_handle==0x100 && file.text && text_mode!=crt_lowio::TextMode::Ansi && standard_file_type(0)==2)text_mode=crt_lowio::TextMode::Utf8;
    if((file.text && text_mode!=crt_lowio::TextMode::Ansi) || file.lowio_lookahead[0]!=10){
        if(!stream && file.text && text_mode!=crt_lowio::TextMode::Ansi && requested%2){failed(22);return {};}
        if(file.lowio_low_surrogate && output.size()<requested){crt_lowio::append_unit(output,*file.lowio_low_surrogate);file.lowio_low_surrogate.reset();}
        const bool buffered=stream && file.buffer.capacity;
        bool first=true;
        while(output.size()<requested && !file.lowio_eof && (!stream || !file.eof)){
            std::vector<std::uint8_t> input;std::size_t prefix=0,available=0;
            while(prefix<3 && file.lowio_lookahead[prefix]!=10){input.push_back(file.lowio_lookahead[prefix]);++prefix;}
            bool final=false;
            if(buffered){
                auto& buffer=file.buffer;
                if(buffer.direction!=CrtFile::Buffer::Direction::Read || static_cast<std::uint64_t>(position)!=buffer.end || buffer.cursor==buffer.used){
                    buffer.used=buffer.cursor=0;buffer.direction=CrtFile::Buffer::Direction::Read;
                    std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(buffer.capacity,65536));const auto got=read(bytes,position);
                    if(got.error){failed(crt_os_error::mapped(got.error));break;}
                    if(!got.bytes){if(file.standard_handle!=0x100 || live_stdio_[0]<0 || live_input_eof_)final=true;}
                    else{memory.write(GuestAddress{buffer.address},std::span(bytes).first(static_cast<std::size_t>(got.bytes)));buffer.used=static_cast<std::uint32_t>(got.bytes);position+=got.bytes;buffer.end=position;}
                }
                available=buffer.used-buffer.cursor;input.resize(prefix+available);
                if(available)memory.read(GuestAddress{buffer.address+buffer.cursor},std::span(input).subspan(prefix));
            }else{
                const auto capacity=std::min<std::size_t>(std::max<std::size_t>(8,requested-output.size()+4),65536);
                input.resize(prefix+capacity);const auto got=read(std::span(input).subspan(prefix),position);
                if(got.error){failed(crt_os_error::mapped(got.error));break;}
                available=static_cast<std::size_t>(got.bytes);input.resize(prefix+available);
                final=available<capacity && (file.standard_handle!=0x100 || live_stdio_[0]<0 || live_input_eof_);
            }
            crt_lowio::ReadText decoded;
            const auto remaining=requested-output.size();
            const auto capacity=stream && file.text && text_mode!=crt_lowio::TextMode::Ansi?(remaining+1)&~std::size_t{1}:remaining;
            if(file.text)decoded=crt_lowio::decode_text(input,text_mode,capacity,final);
            else{decoded.consumed=std::min(input.size(),requested-output.size());decoded.bytes.assign(input.begin(),input.begin()+static_cast<std::ptrdiff_t>(decoded.consumed));}
            if(buffered && decoded.needed>input.size() && !decoded.error){
                std::array<std::uint8_t,4> extra{};const auto wanted=std::min<std::size_t>(extra.size(),decoded.needed-input.size());const auto got=read(std::span(extra).first(wanted),position);
                if(got.error){failed(crt_os_error::mapped(got.error));break;}
                input.insert(input.end(),extra.begin(),extra.begin()+static_cast<std::ptrdiff_t>(got.bytes));
                final=got.bytes<wanted && (file.standard_handle!=0x100 || live_stdio_[0]<0 || live_input_eof_);
                decoded=crt_lowio::decode_text(input,text_mode,capacity,final);
            }
            const auto consumed=decoded.eof?input.size():decoded.consumed;
            const auto prefix_used=std::min(consumed,prefix);
            if(prefix_used){std::move(file.lowio_lookahead.begin()+static_cast<std::ptrdiff_t>(prefix_used),file.lowio_lookahead.end(),file.lowio_lookahead.begin());std::fill(file.lowio_lookahead.end()-static_cast<std::ptrdiff_t>(prefix_used),file.lowio_lookahead.end(),10);}
            const auto raw_used=consumed-prefix_used;
            if(buffered){auto& buffer=file.buffer;buffer.cursor+=static_cast<std::uint32_t>(std::min(raw_used,available));if(raw_used>available){position+=raw_used-available;buffer.used=buffer.cursor=0;buffer.end=position;}}
            else position+=raw_used;
            if(first){file.lowio_read_flags=decoded.read_newline?crt_lowio::read_newline:0;first=false;}
            output.insert(output.end(),decoded.bytes.begin(),decoded.bytes.end());file.lowio_low_surrogate=decoded.low_surrogate;
            if(decoded.error){failed(decoded.error);break;}
            if(decoded.eof){file.lowio_eof=true;if(stream && output.size()<requested)file.eof=true;}
            if(final && input.empty() && stream)file.eof=true;
            if(decoded.needed>input.size()){file.lowio_needed_input=buffered?decoded.needed-input.size():decoded.needed-decoded.consumed-(prefix-prefix_used);break;}
            if(!decoded.consumed)break;
        }
        // fread 以 byte 計數；Unicode lowio 仍以完整 UTF-16 單位讀取。
        // 半個單位留在 FILE pushback，使下一次 fread 與 fgetwc 共用串流位置。
        if(stream){while(output.size()>requested){file.pushback.push_back(output.back());output.pop_back();}if(!output.empty())file.has_read=true;}
        publish_crt_lowio(memory,file);if(stream)crt_buffer_fields(memory,file);return output;
    }
    if(stream && file.buffer.capacity){
        auto& buffer=file.buffer;
        const auto available=[&](){
            if(buffer.direction==CrtFile::Buffer::Direction::Read && static_cast<std::uint64_t>(position)==buffer.end && buffer.cursor<buffer.used)return true;
            buffer.used=buffer.cursor=0;buffer.direction=CrtFile::Buffer::Direction::Read;
            std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(buffer.capacity,65536));const auto got=read(bytes,position);
            if(got.error){failed(crt_os_error::mapped(got.error));return false;}
            if(!got.bytes){if(file.standard_handle!=0x100 || live_stdio_[0]<0 || live_input_eof_)file.eof=true;return false;}
            memory.write(GuestAddress{buffer.address},std::span(bytes).first(static_cast<std::size_t>(got.bytes)));
            buffer.used=static_cast<std::uint32_t>(got.bytes);position+=got.bytes;buffer.end=position;return true;
        };
        while(output.size()<requested && !file.eof && available()){
            // 緩衝區內的資料整段複製，不逐 byte 讀客體記憶體；文字模式只複製到下一個 CR／Ctrl-Z 之前，
            // CR 與 Ctrl-Z 仍由下方逐 byte 路徑判定 CRLF、EOF 與互動式 stdin 的段尾情形。
            {
                const auto span_size=std::min<std::size_t>(requested-output.size(),buffer.used-buffer.cursor);
                std::size_t count=span_size;
                if(file.text){
                    std::vector<std::uint8_t> chunk(span_size);memory.read(GuestAddress{buffer.address+buffer.cursor},chunk);
                    count=0;while(count<chunk.size() && chunk[count]!=13 && chunk[count]!=26)++count;
                    if(count){output.insert(output.end(),chunk.begin(),chunk.begin()+static_cast<std::ptrdiff_t>(count));buffer.cursor+=static_cast<std::uint32_t>(count);continue;}
                }else{
                    const auto at=output.size();output.resize(at+count);
                    memory.read(GuestAddress{buffer.address+buffer.cursor},std::span(output).subspan(at,count));
                    buffer.cursor+=static_cast<std::uint32_t>(count);continue;
                }
            }
            auto byte=static_cast<std::uint8_t>(memory.load(GuestAddress{buffer.address+buffer.cursor++},8));
            if(file.text && byte==26){file.lowio_eof=true;file.eof=true;buffer.cursor=buffer.used;break;}
            if(file.text && byte==13){
                if(file.standard_handle==0x100 && live_stdio_[0]>=0 && !live_input_eof_ && buffer.cursor==buffer.used && static_cast<std::uint64_t>(position)==snapshot.size()){
                    --buffer.cursor;break;
                }
                if(available() && memory.load(GuestAddress{buffer.address+buffer.cursor},8)==10){++buffer.cursor;byte=10;}
            }
            output.push_back(byte);
        }
        if(!output.empty())file.has_read=true;crt_buffer_fields(memory,file);return output;
    }
    while(output.size()<requested && !file.lowio_eof && (!stream || !file.eof)) {
        std::vector<std::uint8_t> input(std::min<std::size_t>(requested-output.size()+(file.text?1:0),65536));
        const auto got=read(input,position);
        if(got.error){failed(crt_os_error::mapped(got.error));break;}
        if(!got.bytes){if(stream && (file.standard_handle!=0x100 || live_stdio_[0]<0 || live_input_eof_))file.eof=true;break;}input.resize(static_cast<std::size_t>(got.bytes));
        // 游標先在本地推進，再一次寫回共用位置：共用游標每次存取都要取跨程序鎖，不能逐 byte 更新。
        // 讀取 CR 後的下一 byte 與提早返回前先寫回，與原逐 byte 更新觀察到的位置相同。
        std::uint64_t cursor=position;
        if(!file.text){
            const auto take=std::min(input.size(),requested-output.size());
            output.insert(output.end(),input.begin(),input.begin()+static_cast<std::ptrdiff_t>(take));
            position=cursor+take;continue;
        }
        for(std::size_t i=0;i<input.size() && output.size()<requested;++i) {
            const auto byte=input[i];++cursor;
            if(byte==26){file.lowio_eof=true;if(stream)file.eof=true;cursor+=input.size()-i-1;break;}
            if(byte==13) {
                if(file.standard_handle==0x100 && live_stdio_[0]>=0 && !live_input_eof_ && cursor==snapshot.size()){position=cursor-1;if(stream && !output.empty())file.has_read=true;return output;}
                std::uint8_t next=0;
                if(i+1<input.size())next=input[i+1];else {position=cursor;std::array<std::uint8_t,1> peek{};if(read(peek,cursor).bytes)next=peek[0];}
                if(next==10){++i;++cursor;output.push_back(10);continue;}
            }
            output.push_back(byte);
        }
        position=cursor;
    }
    if(stream && !output.empty())file.has_read=true;
    file.lowio_read_flags=static_cast<std::uint8_t>(!output.empty() && output.front()==10?4:0);publish_crt_lowio(memory,file);
    return output;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_io(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint64_t result=0;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    if(symbol=="_umask" || symbol=="_umask_s"){
        const bool secure=symbol=="_umask_s";const auto requested=u32(0),previous=runtime_.crt_umask;
        if(secure && ((requested&~0x180U) || !arg(1))){
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
            arg.return_to(state,22);return std::nullopt;
        }
        if(secure)memory.store(GuestAddress{arg(1)},32,previous);
        runtime_.crt_umask=requested&0x180U;arg.return_to(state,secure?0:previous);return std::nullopt;
    }
    const auto error=[&](unsigned code,std::uint64_t value=UINT32_MAX){memory.store(GuestAddress{runtime_.errno_cell},32,code);result=value;trace_crt_file_failure(symbol,"arguments-or-state",{},code);};
    if((symbol=="fread" || symbol=="fwrite") && (!arg(1) || !arg(2))){arg.return_to(state,0);return std::nullopt;}
    constexpr std::array descriptor_calls{"_read","_write","_get_osfhandle","_close","_commit","_setmode","_isatty","_lseek","_lseeki64","_tell","_telli64","_filelength","_filelengthi64","_chsize","_chsize_s","_dup","_dup2","_locking"};
    const bool descriptor_call=std::find(descriptor_calls.begin(),descriptor_calls.end(),symbol)!=descriptor_calls.end();
    constexpr std::array stream_first_calls{"fclose","fflush","fseek","_fseeki64","ftell","_ftelli64","feof","ferror","clearerr","rewind","fgetpos","fsetpos","fgetwc","fgetc","getc","setvbuf","setbuf"};
    constexpr std::array stream_second_calls{"fputs","fputc","putc","ungetc","ungetwc","fputwc"};
    std::uint64_t tracked_stream=0;
    if(!descriptor_call){
        if(std::find(stream_first_calls.begin(),stream_first_calls.end(),symbol)!=stream_first_calls.end())tracked_stream=crt_stream(arg(0));
        else if(std::find(stream_second_calls.begin(),stream_second_calls.end(),symbol)!=stream_second_calls.end())tracked_stream=crt_stream(arg(1));
        else if(symbol=="fread" || symbol=="fwrite")tracked_stream=crt_stream(arg(3));
        else if(symbol=="fgets")tracked_stream=crt_stream(arg(2));
        else if(symbol=="getchar")tracked_stream=crt_stream(runtime_base_.value+0x800);
        else if(symbol=="freopen" || symbol=="_wfreopen")tracked_stream=crt_stream(arg(2));
    }
    std::vector<CrtIoLock> io_locks;
    const auto lock_row=[&](std::uint32_t fd){if(const auto record=crt_lowio_record(memory,fd,false))io_locks.push_back({record+crt_lowio::layout(x86_).critical,true});};
    const auto lock_stream=[&](std::uint64_t stream){if(const auto* file=crt_file(stream)){io_locks.push_back({stream,false});lock_row(file->descriptor);}};
    if(descriptor_call && symbol!="_get_osfhandle" && symbol!="_isatty")lock_row(u32(0));
    if(symbol=="_dup2")lock_row(u32(1));
    if(tracked_stream)lock_stream(tracked_stream);
    if(symbol=="fflush" && !arg(0)){
        for(unsigned fd=1;fd<3;++fd)lock_stream(crt_stream(runtime_base_.value+0x800+fd*128));
        for(const auto& [stream,file]:runtime_.streams)if(file.stdio && file.writable)lock_stream(stream);
    }
    const CrtIoScope io_scope(*this,state,memory,io_locks);
    if(!io_scope)return std::nullopt;
    if(descriptor_call)if(auto* file=crt_file(crt_descriptor_stream(u32(0)));file && !import_crt_lowio(memory,*file)){
        arg.return_to(state,symbol=="_isatty"?0:symbol=="_get_osfhandle" || symbol=="_lseeki64" || symbol=="_telli64" || symbol=="_filelengthi64"?UINT64_MAX:UINT32_MAX);return std::nullopt;}
    if(descriptor_call && symbol!="_close" && symbol!="_get_osfhandle" && symbol!="_isatty" && symbol!="_setmode")if(const auto* file=crt_file(crt_descriptor_stream(u32(0)));file && file->os_handle && !files_.contains(file->os_handle)){
        error(9);if(x86_ && (symbol=="_lseeki64" || symbol=="_telli64" || symbol=="_filelengthi64"))state.reg(Gpr::Rdx)=UINT32_MAX;
        arg.return_to(state,symbol=="_chsize_s"?9U:symbol=="_lseeki64" || symbol=="_telli64" || symbol=="_filelengthi64"?UINT64_MAX:UINT32_MAX);return std::nullopt;
    }
    if(tracked_stream)if(auto* file=crt_file(tracked_stream);file && !import_crt_lowio(memory,*file)){
        arg.return_to(state,symbol=="fread" || symbol=="fgets"?0:symbol=="fgetwc" || symbol=="fputwc" || symbol=="ungetwc"?65535U:UINT32_MAX);return std::nullopt;}
    if(symbol=="fread" || symbol=="fwrite"){
        const auto* file=crt_file(tracked_stream);
        if(!arg(0) || !arg(3) || arg(2)>(x86_?UINT32_MAX:UINT64_MAX)/arg(1) ||
           (symbol=="fwrite" && file && file->text && crt_lowio::text_mode(file->lowio_extended)!=crt_lowio::TextMode::Ansi && ((arg(1)*arg(2))&1U))){
            error(22,0);if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,0);return std::nullopt;}return 0xc0000417U;
        }
    }
    constexpr std::array narrow_stream_calls{"fgetc","getc","getchar","fgets","fputs","fputc","putc","ungetc"};
    if(tracked_stream && std::find(narrow_stream_calls.begin(),narrow_stream_calls.end(),symbol)!=narrow_stream_calls.end())if(const auto* file=crt_file(tracked_stream);file && file->text && crt_lowio::text_mode(file->lowio_extended)!=crt_lowio::TextMode::Ansi){
        error(22,symbol=="fread" || symbol=="fwrite" || symbol=="fgets"?0:UINT32_MAX);
        if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
        return 0xc0000417U;
    }
    if((symbol=="fgetwc" || symbol=="fputwc" || symbol=="ungetc" || symbol=="ungetwc") && !arg(symbol=="fgetwc"?0:1)){
        memory.store(GuestAddress{runtime_.errno_cell},32,22);const auto failure=symbol=="ungetc"?UINT32_MAX:65535U;
        if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,failure);return std::nullopt;}
        return 0xc0000417U;
    }
    if((symbol=="setvbuf" || symbol=="setbuf") && !arg(0)){
        error(22,symbol=="setbuf"?0:UINT32_MAX);
        if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
        arg.return_to(state,result);return std::nullopt;
    }
    const auto fd_stream=[&](std::uint32_t fd){return crt_descriptor_stream(fd);};
    const auto install=[&](CrtFile file){result=crt_install_file(state,memory,std::move(file));};
    const auto from_stream_mode=[&](const crt_lowio::StreamMode& mode){
        CrtFile file;file.stdio=true;file.readable=mode.operation=='r' || mode.update;file.writable=mode.operation!='r' || mode.update;
        file.append=mode.operation=='a';file.noinherit=mode.noinherit;
        file.commit_on_close=mode.commit<0?memory.load(GuestAddress{runtime_.commode_cell},32)!=0:mode.commit!=0;
        const auto translation=mode.unicode_flags?mode.unicode_flags:mode.explicit_translation?(mode.translated?0x4000U:0x8000U):crt_lowio::open_translation(0,static_cast<std::uint32_t>(memory.load(GuestAddress{runtime_.fmode_cell},32)));
        file.text=(translation&0x8000U)==0;file.lowio_extended=crt_lowio::with_mode(file.lowio_extended,translation&0x70000U);
        return std::pair{file,translation&0x70000U};
    };
    const auto open=[&](std::string path,CrtFile file,std::uint32_t disposition,std::uint32_t creation_attributes=0x80,std::uint32_t unicode_flags=0) {
        const auto failed=[&](const char* phase,std::uint32_t native_error=0){trace_crt_file_failure(symbol,phase,path,
            static_cast<std::uint32_t>(memory.load(GuestAddress{runtime_.errno_cell},32)),native_error,0,0,
            runtime_.max_stdio,runtime_.stdio_slots.size());};
        if(file.stdio && runtime_.stdio_slots.size()>=runtime_.max_stdio-3U){error(24,0);failed("stdio-limit");return;}
        const auto info=filesystem_.information(path);
        const auto conflict=[&](const auto& entry){const auto& held=entry.second;return !held.native && !info.key.empty() && held.key==info.key && (((file.readable?1U:0U)|(file.writable?2U:0U))&~held.sharing || ((held.readable?1U:0U)|(held.writable?2U:0U))&~file.sharing);};
        if(std::any_of(runtime_.streams.begin(),runtime_.streams.end(),conflict)){error(13,0);failed("sharing");return;}
        bool probe_read=false;
        if(filesystem_.writable()) {
            const auto access=(file.readable?0x80000000U:0U)|(file.writable?0x40000000U:0U);
            probe_read=unicode_flags && file.append && !file.readable;
            auto opened=filesystem_.open_host(path,access|(probe_read?0x80000000U:0U),disposition,creation_attributes,file.sharing);
            if(opened.error && probe_read){probe_read=false;opened=filesystem_.open_host(path,access,disposition,creation_attributes,file.sharing);}
            if(opened.error){crt_os_error::store(memory,runtime_.errno_cell,opened.error);result=0;failed("open-host",opened.error);return;}
            file.native=opened.file;file.key=opened.key;file.metadata=opened.metadata;if(file.append)file.position=opened.metadata.size;
        } else {
            if(file.writable || disposition!=3){error(13,0);failed("read-only-filesystem");return;}
            const auto snapshot=filesystem_.open(path);if(snapshot.error){crt_os_error::store(memory,runtime_.errno_cell,snapshot.error);result=0;failed("snapshot",snapshot.error);return;}
            file.key=snapshot.key;file.bytes=*snapshot.bytes;file.metadata=info.metadata;
        }
        if(file.text && file.readable && file.writable && !unicode_flags && file.metadata.size && file.native){
            std::array<std::uint8_t,1> last{};const auto got=file.native->read(last,file.metadata.size-1);
            if(got.error){crt_os_error::store(memory,runtime_.errno_cell,got.error);result=0;failed("text-eof",got.error);return;}
            if(got.bytes && last[0]==26){if(const auto code=file.native->truncate(file.metadata.size-1)){crt_os_error::store(memory,runtime_.errno_cell,code);result=0;failed("text-eof-truncate",code);return;}file.metadata=file.native->information().metadata;}
        }
        if(file.text && unicode_flags){
            auto mode=unicode_flags==0x40000?crt_lowio::TextMode::Utf8:unicode_flags==0x20000 || file.writable || file.stdio?crt_lowio::TextMode::Utf16:crt_lowio::TextMode::Ansi;
            std::size_t start=0;
            if(file.readable || probe_read){
                std::array<std::uint8_t,3> prefix{};std::size_t count=0;
                if(file.native){const auto got=file.native->read(prefix,0);if(got.error){crt_os_error::store(memory,runtime_.errno_cell,got.error);result=0;failed("bom-read",got.error);return;}count=static_cast<std::size_t>(got.bytes);}
                else{count=std::min(prefix.size(),file.bytes.size());std::copy_n(file.bytes.begin(),count,prefix.begin());}
                const auto bom=crt_lowio::byte_order_mark(std::span(prefix).first(count));
                if(bom.error){error(bom.error,0);failed("bom-format");return;}
                if(bom.bytes){mode=bom.mode;start=bom.bytes;}
            }
            if(file.writable && !file.metadata.size){
                const auto bom=crt_lowio::byte_order_mark(mode);
                while(start<bom.size()){
                    const auto written=file.native->write(bom.subspan(start),start);
                    if(written.error || !written.bytes){if(written.error)crt_os_error::store(memory,runtime_.errno_cell,written.error);else error(28,0);result=0;failed("bom-write",written.error);return;}start+=static_cast<std::size_t>(written.bytes);
                }
                file.metadata=file.native->information().metadata;
            }
            file.lowio_extended=crt_lowio::with_mode(file.lowio_extended,mode==crt_lowio::TextMode::Utf8?0x40000U:mode==crt_lowio::TextMode::Utf16?0x20000U:0x4000U)|(unicode_flags==0x10000?crt_lowio::unicode:0U);
            file.position=file.append?file.metadata.size:start;
            if(probe_read){
                const auto identity=file.metadata;file.native.reset();
                const auto reopened=filesystem_.open_host(path,0x40000000U,3,creation_attributes,file.sharing);
                if(reopened.error){crt_os_error::store(memory,runtime_.errno_cell,reopened.error);result=0;failed("bom-reopen",reopened.error);return;}
                if(reopened.metadata.id!=identity.id || reopened.metadata.device!=identity.device){error(13,0);failed("bom-identity",1237);return;}
                file.native=reopened.file;file.key=reopened.key;file.metadata=reopened.metadata;
            }
        }else if(file.append)file.position=file.metadata.size;
        install(std::move(file));
        if(!result)failed("install-stream");
    };
    const auto close=[&](std::uint64_t stream,bool keep_allocation=false){if(!crt_close_file(state,memory,stream,true,keep_allocation))result=UINT32_MAX;};
    if(symbol=="_getmaxstdio")result=runtime_.max_stdio;
    else if(symbol=="_setmaxstdio"){
        const auto limit=u32(0);
        if(limit<20 || limit>8192){error(22);if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}}
        else if(!runtime_.stdio_slots.empty() && runtime_.stdio_slots.rbegin()->first>=limit)error(16);
        else result=runtime_.max_stdio=limit;
    }else if(symbol=="_locking"){
        const auto stream=fd_stream(u32(0));auto* file=crt_file(stream);const auto mode=u32(1);
        const auto length=std::bit_cast<std::int32_t>(u32(2));auto& thread=threads_.items.at(threads_.current);
        auto& wait=thread.wait;const bool retry=wait.kind==ThreadWaitKind::CrtFileRetry;
        if(!file || !file->native || (retry && (wait.handles.size()!=6 || file->os_handle!=wait.handles[5]))){wait={};error(9);}
        else if(mode>4 || length<=0){wait={};error(22);}
        else {
            const auto offset=retry?wait.handles[4]:[&](){auto position=crt_file_position(*file);const auto guard=position.lock();return static_cast<std::uint64_t>(position);}();
            const auto attempt=retry?wait.handles[3]+1:UINT64_C(1);
            std::uint32_t code=0;
            if(runtime_.network.external_io){(void)mapping_session();code=filesystem_.publish_shared_file(*file->native);}
            if(!code)code=file->native->lock_range(offset,static_cast<std::uint64_t>(length),true,mode==0);
            if(code==33 && (mode==1 || mode==3) && attempt<10){
                wait={};wait.kind=ThreadWaitKind::CrtFileRetry;wait.handles={u32(0),mode,u32(2),attempt,offset,file->os_handle};
                wait.deadline=runtime_.clock_ticks>UINT64_MAX-10000000?UINT64_MAX:runtime_.clock_ticks+10000000;
                thread.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
            }
            wait={};if(code)error(code==33?(mode==1 || mode==3?36U:13U):code==158?13U:crt_os_error::mapped(code));
        }
        if((memory.load(GuestAddress{runtime_.errno_cell},32)==9 || memory.load(GuestAddress{runtime_.errno_cell},32)==22) && result==UINT32_MAX && current_invalid_parameter_handler()){
            start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;
        }
    }else if(symbol=="_creat" || symbol=="_wcreat") {
        if(!arg(0) || (u32(1)&~0x180U))error(22);
        else {CrtFile file;file.readable=file.writable=true;file.text=true;const auto path=symbol=="_wcreat"?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));open(path,std::move(file),2,((u32(1)&~runtime_.crt_umask)&0x80)?0x80U:1U);result=result?runtime_.streams.at(result).descriptor:UINT32_MAX;}
    }else if(symbol=="freopen" || symbol=="_wfreopen") {
        const auto base=runtime_base_.value+0x800;const auto original=arg(2)==base+48?base+128:arg(2)==base+96?base+256:arg(2);const auto stream=crt_stream(original);
        const bool standard=original>=base && original<=base+256 && (original-base)%128==0;
        if(!arg(0) || !arg(1) || (!standard && !runtime_.streams.contains(stream)))error(22,0);
        else {
            const auto path=symbol=="_wfreopen"?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
            const auto mode=symbol=="_wfreopen"?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)},32)):read_ansi(memory,arg(1),32);
            close(stream,!standard);runtime_.crt_redirects.erase(original);
            const auto parsed=crt_lowio::stream_mode(mode);
            if(!parsed)error(22,0);
            else {auto [file,unicode_flags]=from_stream_mode(*parsed);open(path,std::move(file),parsed->disposition(),0x80U|parsed->attributes|(parsed->temporary?0x04000000U:0U),unicode_flags);
                if(result){if(standard){runtime_.crt_redirects[original]=result;const auto fd=static_cast<std::uint32_t>((original-base)/128);standard_open_[fd]=true;auto& reopened=runtime_.streams.at(result);close_crt_lowio(memory,reopened.descriptor);reopened.lowio_published=false;runtime_.descriptors.erase(reopened.descriptor);runtime_.next_descriptor=std::min(runtime_.next_descriptor,reopened.descriptor);runtime_.stdio_slots.erase(reopened.stdio_slot);reopened.stdio_slot=fd;reopened.descriptor=fd;runtime_.descriptors[fd]=result;memory.store(GuestAddress{result+28},32,fd);publish_crt_lowio(memory,reopened);}else {const auto fresh=result;auto reopened=std::move(runtime_.streams.at(fresh));runtime_.streams.erase(fresh);runtime_.descriptors[reopened.descriptor]=original;memory.store(GuestAddress{original+24},32,(reopened.readable?1U:0U)|(reopened.writable?2U:0U));memory.store(GuestAddress{original+28},32,reopened.descriptor);reopened.stream_address=original;runtime_.stdio_slots[reopened.stdio_slot]=original;runtime_.streams.emplace(original,std::move(reopened));(void)free_heap(memory,fresh,state);}result=original;}}
        }
    }else if(symbol=="_open_osfhandle") {
        const auto flags=u32(1);const auto handle=arg(0);
        const auto unicode=flags&0x70000U;
        if((flags&~0x7c08bU) || (flags&3U)==3U || (flags&0xc000U)==0xc000U || (unicode && ((unicode&(unicode-1)) || (flags&0x8000U))))error(22);
        else if(handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100]){
            CrtFile file;file.standard_handle=handle;file.readable=handle==0x100;file.writable=handle!=0x100;file.text=!(flags&0x8000);file.lowio_extended=crt_lowio::with_mode(file.lowio_extended,unicode);file.noinherit=(flags&0x80)!=0;install(std::move(file));result=result?runtime_.streams.at(result).descriptor:UINT32_MAX;
        }else if(const auto found=files_.find(handle);found!=files_.end()){
            const auto& source=found->second;CrtFile file;file.native=source.native;if(source.bytes)file.bytes=*source.bytes;
            file.key=source.key;file.metadata=source.metadata;file.os_handle=handle;file.readable=(source.access&(0x80000000U|1U))!=0;file.writable=(source.access&(0x40000000U|2U|4U))!=0;file.text=!(flags&0x8000);file.append=(flags&8U)!=0;
            file.noinherit=(flags&0x80)!=0;
            file.lowio_extended=crt_lowio::with_mode(file.lowio_extended,unicode);
            install(std::move(file));result=result?runtime_.streams.at(result).descriptor:UINT32_MAX;
        }else error(9);
    }else if(symbol=="_get_osfhandle") {
        const auto stream=fd_stream(u32(0));
        if(!stream)error(9,UINT64_MAX);
        else if(u32(0)<3 && stream==runtime_base_.value+0x800+u32(0)*128)result=0x100+u32(0);
        else {
            auto& file=runtime_.streams.at(stream);result=crt_lowio_handle(file);
            if(file.lowio_handle_override){arg.return_to(state,result);return std::nullopt;}
            if(!result){FileHandle handle;handle.key=file.key;handle.path=file.key;handle.native=file.native;handle.metadata=file.metadata;handle.bytes=std::make_shared<const std::vector<std::uint8_t>>(file.bytes);handle.access=(file.readable?0x80000000U:0U)|(file.writable?0x40000000U:0U);handle.sharing=file.sharing;
                result=runtime_.allocate_wait_handle();handle.object=result;runtime_.win_file_positions[result]=crt_file_position(file);files_.emplace(result,std::move(handle));file.os_handle=result;
                // CRT 的重複描述元共用檔案位置，但各自擁有可關閉的 Win32 handle。
                if(file.description)for(auto& [pointer,sibling]:runtime_.streams){
                    if(pointer!=stream && sibling.description==file.description && !sibling.os_handle){
                        const auto duplicate=runtime_.allocate_wait_handle();files_.emplace(duplicate,files_.at(result));sibling.os_handle=duplicate;
                    }
                }
            }
        }
    }else if(symbol=="_chsize" || symbol=="_chsize_s") {
        const bool secure=symbol=="_chsize_s";
        const auto length=secure?std::bit_cast<std::int64_t>(arg(1)):static_cast<std::int64_t>(std::bit_cast<std::int32_t>(u32(1)));
        const auto stream=fd_stream(u32(0));const auto found=runtime_.streams.find(stream);
        unsigned code=0;bool invalid=false;
        if(!stream){code=9;invalid=true;}
        else if(length<0){code=22;invalid=true;}
        else if(found==runtime_.streams.end() || found->second.standard_handle)code=9;
        else if(!found->second.writable || !found->second.native)code=secure?9:13;
        else {
            auto& file=found->second;
            const auto native_error=file.native->truncate(static_cast<std::uint64_t>(length));
            if(native_error){crt_os_error::store(memory,runtime_.errno_cell,native_error);code=crt_os_error::mapped(native_error);}
            else file.metadata=file.native->information().metadata;
            // 截斷共用宿主檔案，不改動任何描述元的目前位置。
        }
        if(code) {
            error(code,secure?code:UINT32_MAX);
            if(invalid && current_invalid_parameter_handler()) {
                start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;
            }
        }
    }else if(symbol=="_filelength" || symbol=="_filelengthi64"){
        const auto stream=fd_stream(u32(0));const auto found=runtime_.streams.find(stream);
        if(found==runtime_.streams.end() || found->second.standard_handle)error(9,UINT64_MAX);
        else {const auto& file=found->second;result=file.native?file.native->information().metadata.size:file.bytes.size();if(symbol=="_filelength" && result>INT32_MAX)error(22);}
        if(x86_ && symbol=="_filelengthi64")state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result>>32);
    }else if(symbol=="_dup" || symbol=="_dup2") {
        const auto stream=fd_stream(u32(0));const auto target=symbol=="_dup2"?u32(1):0U;auto* source=crt_file(stream);
        if(!stream || !source || (symbol=="_dup2" && target>=8192)){
            error(9);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
        }else if(symbol=="_dup2" && u32(0)==target)result=0;
        else {
            auto file=*source;file.buffer={};file.stream_address=0;file.stdio=false;file.stdio_slot=UINT32_MAX;
            file.eof=file.error=file.has_read=false;file.pushback.clear();
            const auto pointer=crt_install_file(state,memory,std::move(file),symbol=="_dup2"?std::optional<std::uint32_t>{target}:std::nullopt,true);
            result=pointer?(symbol=="_dup2"?0:runtime_.streams.at(pointer).descriptor):UINT32_MAX;
        }
    } else if(symbol=="_fdopen" || symbol=="_wfdopen") {
        result=fd_stream(u32(0));
        const auto mode=!arg(1)?std::string{}:symbol=="_wfdopen"?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)},32)):read_ansi(memory,arg(1),32);
        const auto parsed=crt_lowio::stream_mode(mode);
        if(!result)error(9,0);
        else if(!parsed)error(22,0);
        else {auto* file=crt_file(result);if(!file)error(9,0);
            else if(!crt_attach_stream(memory,*file))result=0;
            else {file->commit_on_close=parsed->commit<0?memory.load(GuestAddress{runtime_.commode_cell},32)!=0:parsed->commit!=0;}}

    } else if(symbol=="_open" || symbol=="_wopen" || symbol=="_sopen" || symbol=="_wsopen" || symbol=="_sopen_s" || symbol=="_wsopen_s") {
        const bool secure=symbol.ends_with("_s"),shared=symbol.find("sopen")!=symbol.npos;
        const unsigned first=secure?1U:0U;
        if(secure && arg(0))memory.store(GuestAddress{arg(0)},32,UINT32_MAX);
        const auto flags_value=u32(first+1),share=shared?u32(first+2):0x40U;
        const auto translation=crt_lowio::open_translation(flags_value,static_cast<std::uint32_t>(memory.load(GuestAddress{runtime_.fmode_cell},32)));
        const auto mode=(secure || (flags_value&0x100))?u32(first+(shared?3:2)):0x180U;
        // 一般 open／sopen 保留舊 CRT 契約，只採用 owner read/write bits；_s 版本才驗證完整 pmode。
        const bool bad=(secure && !arg(0)) || !arg(first) || (flags_value&~0x7c7ffU) || (flags_value&3)==3 || !crt_lowio::valid_open_translation(translation) || (share!=0x10 && share!=0x20 && share!=0x30 && share!=0x40 && share!=0x80) || (secure && (mode&~0x180U));
        if(bad){
            error(22,secure?22:UINT32_MAX);
            if(secure){
                if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
                return 0xc0000417U;
            }
        }else {
            const auto path=symbol.starts_with("_w")?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(first)})):read_ansi(memory,arg(first));
            CrtFile file;file.readable=(flags_value&3)!=1;file.writable=(flags_value&3)!=0;file.append=(flags_value&8)!=0;file.text=!(translation&0x8000);
            file.lowio_extended=crt_lowio::with_mode(file.lowio_extended,translation&0x70000U);
            file.noinherit=(flags_value&0x80)!=0;
            file.sharing=share==0x10?0:share==0x20?1:share==0x30?2:share==0x80?(file.writable?0:1):3;
            const auto disposition=(flags_value&0x100)?(flags_value&0x400)?1U:(flags_value&0x200)?2U:4U:(flags_value&0x200)?5U:3U;
            open(path,std::move(file),disposition,(((mode&~runtime_.crt_umask)&0x80)?0x80U:1U)|((flags_value&0x1000U)?0x100U:0U)|((flags_value&0x40U)?0x04000000U:0U),translation&0x70000U);
            if(secure){if(result){memory.store(GuestAddress{arg(0)},32,runtime_.streams.at(result).descriptor);result=0;}else result=memory.load(GuestAddress{runtime_.errno_cell},32);}
            else result=result?runtime_.streams.at(result).descriptor:UINT32_MAX;
        }
    } else if(symbol.starts_with("?_Fiopen@std@@") || symbol=="fopen" || symbol=="_wfopen" || symbol=="_fsopen" || symbol=="_wfsopen" || symbol=="fopen_s" || symbol=="_wfopen_s") {
        const bool cpp=symbol.starts_with("?_Fiopen@std@@"),secure=symbol=="fopen_s" || symbol=="_wfopen_s";
        const bool wide=cpp?symbol.find("BDHH")==symbol.npos:symbol.starts_with("_w");
        std::string cpp_mode;
        if(cpp){
            auto flags=u32(1);if(flags&0x40U)flags|=1U;if(flags&8U)flags|=2U;
            const auto operation=flags&~0xe4U;
            if(operation==1)cpp_mode="r";
            else if(operation==3)cpp_mode="r+";
            else if(operation==2 || operation==18)cpp_mode="w";
            else if(operation==19)cpp_mode="w+";
            else if(operation==10)cpp_mode="a";
            else if(operation==11)cpp_mode="a+";
            if(cpp_mode.empty() || ((flags&0x80U) && cpp_mode.front()!='w')){trace_crt_file_failure(symbol,"cpp-open-mode",{},0,0,flags);arg.return_to(state,0);return std::nullopt;}
            if(flags&0x20U)cpp_mode+='b';if(flags&0x80U)cpp_mode+='x';
        }
        const unsigned first=secure?1:0;
        if(secure){
            if(arg(0))memory.store(GuestAddress{arg(0)},x86_?32:64,0);
            if(!arg(0) || !arg(1) || !arg(2)){
                error(22,22);
                if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
                arg.return_to(state,result);return {};
            }
        }
        if(!arg(first) || (!cpp && !arg(first+1))){error(22,secure?22:0);if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}arg.return_to(state,result);return {};}
        const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(first)})):read_ansi(memory,arg(first));
        const auto mode=cpp?cpp_mode:wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(first+1)},32)):read_ansi(memory,arg(first+1),32);
        const auto share=secure?0x10U:(cpp || symbol.ends_with("fsopen"))?u32(2):0x40U;
        const auto parsed=crt_lowio::stream_mode(mode);
        if(!parsed || (share!=0x10 && share!=0x20 && share!=0x30 && share!=0x40))error(22,0);
        else {
            auto [file,unicode_flags]=from_stream_mode(*parsed);file.sharing=share==0x10?0:share==0x20?1:share==0x30?2:3;
            open(path,std::move(file),parsed->disposition(),0x80U|parsed->attributes|(parsed->temporary?0x04000000U:0U),unicode_flags);
        }
        if(cpp && result && (u32(1)&4U)){
            auto& file=runtime_.streams.at(result);std::uint64_t size=file.bytes.size();std::uint32_t failure=0;
            if(file.native){const auto info=file.native->information();failure=info.error;size=info.metadata.size;}
            if(failure || size>INT64_MAX){const auto pointer=result;close(pointer);if(failure)crt_os_error::store(memory,runtime_.errno_cell,failure);else memory.store(GuestAddress{runtime_.errno_cell},32,22);result=0;}
            else {auto position=crt_file_position(file);const auto guard=position.lock();position=size;}
        }
        if(secure){
            if(result){memory.store(GuestAddress{arg(0)},x86_?32:64,result);result=0;}
            else {
                result=memory.load(GuestAddress{runtime_.errno_cell},32);
                if(result==22 && current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
            }
        }
    } else if(symbol=="_read" || symbol=="_write") {
        const auto stream=fd_stream(u32(0));const auto count=u32(2);
        const auto* descriptor_file=crt_file(stream);
        const bool bad_unicode_count=descriptor_file && descriptor_file->text && crt_lowio::text_mode(descriptor_file->lowio_extended)!=crt_lowio::TextMode::Ansi && count%2;
        if(!stream)error(9);
        else if(count>output_limit || bad_unicode_count || (count && !arg(1))){error(22);if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}}
        else if(symbol=="_write") {std::vector<std::uint8_t> bytes(count);memory.read(GuestAddress{arg(1)},bytes);auto* file=crt_file(stream);result=file?write_crt_file(memory,*file,bytes):0;if(!result && count)result=UINT32_MAX;}
        else if((u32(0)==1 || u32(0)==2) && stream==runtime_base_.value+0x800+u32(0)*128)error(9);
        else {auto& file=stream==runtime_base_.value+0x800?runtime_.standard_input:runtime_.streams.at(stream);memory.check(GuestAddress{arg(1)},count,Access::Write);if(file.standard_handle==0x100 && !file.lowio_eof && !file.lowio_low_surrogate && count && !await_standard_input(state,memory,1,file.text))return std::nullopt;const auto bytes=read_crt_file(memory,file,count);if(bytes.empty() && file.standard_handle==0x100 && file.lowio_needed_input && !await_standard_input(state,memory,file.lowio_needed_input))return std::nullopt;memory.write(GuestAddress{arg(1)},bytes);result=file.lowio_error && bytes.empty()?UINT32_MAX:bytes.size();}
    } else if(symbol=="fputwc") {
        const std::u16string text(1,static_cast<char16_t>(arg(0)));result=write_crt_wide_stream(state,memory,arg(1),text)==1?arg(0)&65535U:65535U;
    } else if(symbol=="fwrite" || symbol=="fputs" || symbol=="fputc" || symbol=="putc") {
        const bool character=symbol=="fputc" || symbol=="putc",string=symbol=="fputs";
        const auto stream=crt_stream(arg(character || string?1:3));const auto size=character || string?1:arg(1),count=character?1:string?read_ansi(memory,arg(0)).size():arg(2);
        if(size && count) {
            if(count>output_limit/size)throw HostCallFault("CRT 單次寫入超過上限");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size*count));if(character)bytes[0]=static_cast<std::uint8_t>(arg(0));else memory.read(GuestAddress{arg(0)},bytes);
            const auto written=write_crt_stream(memory,stream,bytes);result=character?(written?arg(0)&255:UINT32_MAX):string?(written==count?0:UINT32_MAX):written/size;
        }
    } else {
        const bool descriptor=symbol=="_commit" || symbol=="_close" || symbol=="_isatty" || symbol=="_setmode" || symbol=="_lseeki64" || symbol=="_lseek" || symbol=="_telli64" || symbol=="_tell";
        const auto stream=symbol=="getchar"?fd_stream(0):descriptor?fd_stream(u32(0)):crt_stream(arg(symbol=="fread"?3:symbol=="fgets"?2:(symbol=="ungetc" || symbol=="ungetwc")?1:0));
        const auto base=runtime_base_.value+0x800;const bool standard=stream>=base && stream<=base+256 && (stream-base)%128==0;
        const auto found=runtime_.streams.find(stream);const bool valid=standard?standard_open_[(stream-base)/128]:found!=runtime_.streams.end();
        if(symbol=="fflush" && !arg(0))result=flush_crt_streams(memory)?0:UINT32_MAX;
        else if(!valid)error(9,symbol=="fread" || symbol=="fgets" || symbol=="_isatty"?0:symbol=="_telli64" || symbol=="_lseeki64"?UINT64_MAX:UINT32_MAX);
        else if(symbol=="fclose" || symbol=="_close")result=crt_close_file(state,memory,stream,symbol=="fclose")?0:UINT32_MAX;
        else if(symbol=="_isatty") {const auto* file=crt_file(stream);const auto record=file?crt_lowio_record(memory,file->descriptor,false):0;result=record?(memory.load(GuestAddress{record+crt_lowio::layout(x86_).flags},8)&crt_lowio::tty):0;}
        else if(symbol=="_setmode") {
            if(!crt_lowio::valid_mode(u32(1))){error(22);if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}}
            else {auto& file=*crt_file(stream);result=crt_lowio::mode_value(file.text,crt_lowio::text_mode(file.lowio_extended));file.text=u32(1)!=0x8000;file.lowio_extended=crt_lowio::with_mode(file.lowio_extended,u32(1));file.lowio_low_surrogate.reset();file.lowio_needed_input=0;file.lowio_multibyte_size=0;if(file.descriptor<3)runtime_.descriptor_modes[file.descriptor]=u32(1);}
        } else if(symbol=="setvbuf" || symbol=="setbuf") {
            auto* file=crt_file(stream);const auto mode=symbol=="setbuf"?(arg(1)?0U:4U):u32(2);
            const auto capacity=symbol=="setbuf"?UINT64_C(512):arg(3);
            if(!arg(0) || (mode!=0 && mode!=0x40 && mode!=4) || (mode!=4 && (capacity<2 || capacity>INT32_MAX))){
                error(22,symbol=="setbuf"?0:UINT32_MAX);
                if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
            }else {
                const auto size=mode==4?0U:static_cast<std::uint32_t>(capacity&~UINT64_C(1));
                const bool owned=size && !arg(1);const auto address=size?(owned?allocate_heap(memory,size):arg(1)):0;
                if(size && !address)error(12,symbol=="setbuf"?0:UINT32_MAX);
                else {
                    if(size)memory.check(GuestAddress{address},size,Access::Write);
                    if(!flush_crt_file(memory,*file)){if(owned)(void)free_heap(memory,address,state);result=UINT32_MAX;}
                    else {if(file->buffer.owned)(void)free_heap(memory,file->buffer.address,state);file->buffer={};file->buffer.address=address;file->buffer.capacity=size;file->buffer.owned=owned;crt_buffer_fields(memory,*file);}
                }
            }
        }
        else if(symbol=="fflush")result=flush_crt_file(memory,*crt_file(stream))?0:UINT32_MAX;
        else if(symbol=="_commit") {if(!standard && found->second.native) {const auto code=found->second.native->flush();if(code){crt_os_error::store(memory,runtime_.errno_cell,code);result=UINT32_MAX;}}}
        else if(standard && stream!=base) {
            if(symbol=="feof" || symbol=="ferror" || symbol=="clearerr")result=0;else error(9,symbol=="fread" || symbol=="fgets"?0:UINT32_MAX);
        } else {
            auto& file=standard?runtime_.standard_input:found->second;auto position=crt_file_position(file);const auto position_guard=position.lock();
            const auto physical=static_cast<std::uint64_t>(position),logical=crt_stream_position(memory,file);
            if(symbol=="ungetc" || symbol=="ungetwc"){
                const bool wide=symbol=="ungetwc";const auto character=wide?u32(0)&65535U:u32(0);const auto failure=wide?65535U:UINT32_MAX;
                result=failure;
                if(file.readable && file.has_read && file.pushback.empty() && character!=failure){
                    if(wide && (!file.text || crt_lowio::text_mode(file.lowio_extended)!=crt_lowio::TextMode::Ansi)){file.pushback.push_back(static_cast<std::uint8_t>(character>>8));file.pushback.push_back(static_cast<std::uint8_t>(character));result=character;}
                    else if(!wide){file.pushback.push_back(static_cast<std::uint8_t>(character));result=character&255U;}
                    else if(const auto encoded=windows_crt_encode(std::u16string(1,static_cast<char16_t>(character)),current_crt_locale(state,memory).categories[2].codepage)){
                        file.pushback.insert(file.pushback.end(),encoded->rbegin(),encoded->rend());result=character;file.lowio_multibyte_size=0;
                    }else memory.store(GuestAddress{runtime_.errno_cell},32,42);
                    if(result!=failure)file.eof=false;
                }
            }
            else if(symbol=="feof")result=file.eof;else if(symbol=="ferror")result=file.error;
            else if(symbol=="clearerr"){file.eof=file.lowio_eof=false;file.error=false;}
            else if(symbol=="fread" || symbol=="getchar" || symbol=="fgetc" || symbol=="getc" || symbol=="fgets" || symbol=="fgetwc") {
                if(symbol=="fread") {const auto size=arg(1),count=arg(2);if(size && count){if(count>output_limit/size)throw HostCallFault("CRT 單次讀取超過上限");memory.check(GuestAddress{arg(0)},static_cast<std::size_t>(size*count),Access::Write);if(file.standard_handle==0x100 && !file.eof && !file.lowio_eof && size*count>file.pushback.size() && !await_standard_input(state,memory,size*count-file.pushback.size(),file.text))return std::nullopt;const auto bytes=read_crt_file(memory,file,static_cast<std::size_t>(size*count),true);memory.write(GuestAddress{arg(0)},bytes);result=bytes.size()/size;}}
                else if(symbol=="fgets") {
                    const auto capacity=std::bit_cast<std::int32_t>(u32(1));if(capacity>1048576)throw HostCallFault("fgets 請求超過上限");
                    if(capacity>0){memory.check(GuestAddress{arg(0)},static_cast<std::size_t>(capacity),Access::Write);if(file.standard_handle==0x100 && !file.eof && !file.lowio_eof && file.pushback.empty() && !await_standard_input(state,memory,static_cast<std::uint64_t>(capacity-1),file.text,true))return std::nullopt;std::vector<std::uint8_t> bytes;
                        while(bytes.size()+1<static_cast<std::size_t>(capacity)){const auto part=read_crt_file(memory,file,1,true);if(part.empty())break;bytes.push_back(part[0]);if(part[0]==10)break;}
                        if(!bytes.empty() || capacity==1){bytes.push_back(0);memory.write(GuestAddress{arg(0)},bytes);result=arg(0);}}
                } else if(symbol=="fgetwc" && file.text && crt_lowio::text_mode(file.lowio_extended)==crt_lowio::TextMode::Ansi){
                    result=65535U;
                    if(!file.lowio_multibyte_size){const auto& category=current_crt_locale(state,memory).categories[2];file.lowio_multibyte_page=category.codepage;file.lowio_multibyte_maximum=static_cast<std::uint8_t>(std::min<std::size_t>(std::max<std::size_t>(1,category.maximum_character_bytes),file.lowio_multibyte.size()));}
                    const auto maximum=file.lowio_multibyte_maximum;
                    while(file.lowio_multibyte_size<maximum){
                        if(file.standard_handle==0x100 && !file.eof && !file.lowio_eof && file.pushback.empty() && !await_standard_input(state,memory,1,file.text))return std::nullopt;
                        const auto bytes=read_crt_file(memory,file,1,true);
                        if(bytes.empty()){if(file.lowio_multibyte_size){file.error=true;file.lowio_multibyte_size=0;memory.store(GuestAddress{runtime_.errno_cell},32,42);}break;}
                        file.lowio_multibyte[file.lowio_multibyte_size++]=bytes[0];
                        const auto decoded=windows_crt_decode(std::span(file.lowio_multibyte).first(file.lowio_multibyte_size),file.lowio_multibyte_page);
                        if(decoded.status==CrtDecodedCharacter::Status::Complete && decoded.character<=65535){result=decoded.character;file.lowio_multibyte_size=0;break;}
                        if(decoded.status==CrtDecodedCharacter::Status::Invalid || decoded.character>65535 || file.lowio_multibyte_size==maximum){file.error=true;file.lowio_multibyte_size=0;memory.store(GuestAddress{runtime_.errno_cell},32,42);break;}
                    }
                } else {const bool wide=symbol=="fgetwc";const unsigned count=wide && (!file.text || crt_lowio::text_mode(file.lowio_extended)!=crt_lowio::TextMode::Ansi)?2:1;
                    if(file.standard_handle==0x100 && !file.eof && !file.lowio_eof && !file.lowio_low_surrogate && file.pushback.size()<count && !await_standard_input(state,memory,count-file.pushback.size(),file.text))return std::nullopt;
                    const auto bytes=read_crt_file(memory,file,count,true);if(bytes.empty() && file.standard_handle==0x100 && file.lowio_needed_input && !await_standard_input(state,memory,file.lowio_needed_input))return std::nullopt;result=bytes.size()!=count?(wide?65535U:UINT32_MAX):bytes[0]|(count==2?unsigned{bytes[1]}<<8:0U);
                }
            } else if(standard || file.standard_handle)error(29,(symbol=="_ftelli64" || symbol=="_telli64" || symbol=="_lseeki64")?UINT64_MAX:UINT32_MAX);
            else if(symbol=="rewind"){if(flush_crt_file(memory,file)){position=0;file.eof=file.lowio_eof=false;file.error=false;file.lowio_low_surrogate.reset();file.lowio_lookahead={10,10,10};file.lowio_needed_input=0;file.lowio_multibyte_size=0;}}
            else if(symbol=="fgetpos" || symbol=="fsetpos") {
                if(!arg(1))error(22);
                else if(symbol=="fgetpos"){
                    if(logical>INT64_MAX)error(34);else memory.store(GuestAddress{arg(1)},64,logical);
                } else {
                    const auto offset=memory.load(GuestAddress{arg(1)},64);
                    if(offset>INT64_MAX)error(22);else if(!flush_crt_file(memory,file))result=UINT32_MAX;else {position=offset;file.eof=file.lowio_eof=false;file.lowio_low_surrogate.reset();file.lowio_lookahead={10,10,10};file.lowio_needed_input=0;file.lowio_multibyte_size=0;}
                }
            }
            else if(symbol=="ftell" || symbol=="_ftelli64" || symbol=="_tell" || symbol=="_telli64") {
                const auto current=descriptor?physical:logical;
                if((symbol=="ftell" || symbol=="_tell") && current>INT32_MAX)error(34);else result=current;
            } else if(symbol=="fseek" || symbol=="_fseeki64" || symbol=="_lseek" || symbol=="_lseeki64") {
                const auto offset=(symbol=="fseek" || symbol=="_lseek")?static_cast<std::int64_t>(std::bit_cast<std::int32_t>(u32(1))):std::bit_cast<std::int64_t>(arg(1));
                const __int128 next=static_cast<__int128>(offset)+(u32(2)==1?(descriptor?physical:logical):u32(2)==2?(file.native?file.native->information().metadata.size:file.bytes.size()):0);
                if(u32(2)>2 || next<0 || next>INT64_MAX || (symbol=="_lseek" && next>INT32_MAX))error(22,symbol=="_lseeki64"?UINT64_MAX:UINT32_MAX);
                else if(!descriptor && !flush_crt_file(memory,file))result=UINT32_MAX;else {position=static_cast<std::uint64_t>(next);file.eof=file.lowio_eof=false;file.pushback.clear();file.lowio_low_surrogate.reset();file.lowio_lookahead={10,10,10};file.lowio_needed_input=0;file.lowio_multibyte_size=0;if(descriptor){file.buffer.used=file.buffer.cursor=0;file.buffer.direction=CrtFile::Buffer::Direction::None;crt_buffer_fields(memory,file);result=position;}}
            } else throw HostCallFault("CRT I/O 未處理的服務");
        }
    }
    if(descriptor_call)if(auto* file=crt_file(crt_descriptor_stream(u32(0))))publish_crt_lowio(memory,*file);
    if(tracked_stream)if(auto* file=crt_file(tracked_stream))publish_crt_lowio(memory,*file);
    if(x86_ && (symbol=="_lseeki64" || symbol=="_telli64" || symbol=="_ftelli64"))state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result>>32);
    arg.return_to(state,result);return std::nullopt;
}
}
