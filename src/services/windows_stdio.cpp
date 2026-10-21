#include "x64arm/windows_services.hpp"
#include "x64arm/native_console.hpp"
#include <algorithm>
#include <cerrno>
#include "x64arm/unicode.hpp"
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#endif

namespace x64arm {
void WindowsServices::import_standard_handle(const InheritedHandle& item) {
#if !defined(_WIN32)
    if(item.handle<0x100 || item.handle>0x102 || item.auxiliary!=item.handle-0x100 || item.descriptor<0)
        throw HostCallFault("繼承標準串流內容無效");
    const auto stream=static_cast<unsigned>(item.auxiliary);
    (void)::fcntl(item.descriptor,F_SETFD,FD_CLOEXEC);
    inherited_stdio_[stream]=std::shared_ptr<int>(new int(item.descriptor),[](int* fd){if(*fd>=0)::close(*fd);delete fd;});
    auto descriptors=live_stdio_;
    for(unsigned i=0;i<3;++i)if(descriptors[i]<0)descriptors[i]=static_cast<int>(i);
    descriptors[stream]=item.descriptor;
    set_live_standard_io(descriptors[0],descriptors[1],descriptors[2]);
    runtime_.handle_flags[item.handle]=item.flags;standard_open_[stream]=true;
#else
    (void)item;throw HostCallFault("此平台不支援標準串流傳輸");
#endif
}
std::uint32_t WindowsServices::standard_file_type(unsigned stream) const {
#if !defined(_WIN32)
    if(live_stdio_[stream]>=0){struct stat info{};if(::fstat(live_stdio_[stream],&info))return 0;
        if(S_ISCHR(info.st_mode))return 2;
        if(S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode))return 3;
        if(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode))return 1;
        return 0;
    }
#endif
    return console_handle(0x100+stream)?2U:3U;
}
FileInformation WindowsServices::file_handle_information(std::uint64_t handle) {
    if(handle>=0x100 && handle<=0x102){
        const auto stream=static_cast<unsigned>(handle-0x100);
        if(!standard_open_[stream])return {{},{},6};
        // 標準 HANDLE 的型別由實際重新導向決定；磁碟檔案與一般開啟檔案
        // 共用 metadata 契約，管線／終端不捏造磁碟資訊。
        if(standard_file_type(stream)!=1)return {{},{},1};
        return HostFile::descriptor_information(live_stdio_[stream]);
    }
    const auto found=files_.find(handle);
    if(found==files_.end())return {{},{},6};
    auto& file=found->second;
    if(file.pipe || file.serial || file.hid)return {{},{},1};
    if(file.native){
        auto info=file.native->information();
        if(!info.error)file.metadata=info.metadata;
        return info;
    }
    return {file.key,file.metadata,0};
}
bool WindowsServices::console_handle(std::uint64_t handle) const {
    return handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100] && consoles_[handle-0x100]!=nullptr;
}
void WindowsServices::set_live_standard_io(int input,int output,int error) {
    runtime_.lowio_standard_dirty=true;
    if(input<0 || output<0 || error<0)throw std::invalid_argument("即時標準串流需要有效的宿主描述元");
    if(!runtime_.network.external_io)throw std::invalid_argument("compare 不接受即時標準串流");
    for(const auto& endpoint:consoles_)if(endpoint && endpoint->buffer->window_handle){const auto handle=endpoint->buffer->window_handle;endpoint->buffer->window_handle=0;erase_window(handle);}
    consoles_={};parent_stdio_={input,output,error};live_stdio_=parent_stdio_;live_input_eof_=false;
#if !defined(_WIN32)
    for(unsigned i=0;i<3;++i)if(!detached_console_ && ::isatty(live_stdio_[i])){
        auto console=std::make_shared<NativeConsole>();console->descriptor=::dup(live_stdio_[i]);
        if(console->descriptor>=0 && !::tcgetattr(console->descriptor,&console->original)){
            struct stat current{};if(::fstat(console->descriptor,&current))throw std::invalid_argument("無法識別原生終端");
            for(unsigned j=0;j<i;++j)if(consoles_[j]){struct stat previous{};if(!::fstat(consoles_[j]->descriptor,&previous) && previous.st_rdev==current.st_rdev && previous.st_ino==current.st_ino)console->buffer=consoles_[j]->buffer;}
            if(!console->buffer){console->buffer=std::make_shared<ConsoleBuffer>();const auto& mode=console->original;
                console->buffer->input_mode=((mode.c_lflag&ISIG)?1U:0U)|((mode.c_lflag&ICANON)?2U:0U)|((mode.c_lflag&ECHO)?4U:0U);
                console->buffer->output_mode=((mode.c_oflag&OPOST)?1U:0U)|6U;
                winsize size{};if(!::ioctl(console->descriptor,TIOCGWINSZ,&size) && size.ws_col && size.ws_row)console->buffer->resize(size.ws_col,size.ws_row);
            }
            live_stdio_[i]=console->descriptor;consoles_[i]=std::move(console);
        }
    }
#endif
    runtime_.standard_input.standard_handle=0x100;
    runtime_.standard_input.text=runtime_.descriptor_modes[0]==0x4000;
    for(unsigned i=0;i<3;++i)runtime_.handle_flags.try_emplace(0x100+i,1U);
    publish_console_identity(!detached_console_ && (console_identity_ || std::any_of(consoles_.begin(),consoles_.end(),[](const auto& endpoint){return endpoint!=nullptr;})));
}
bool WindowsServices::standard_input_ready(std::uint64_t count,bool text,bool line,int timeout) {
    if(console_handle(0x100)){(void)text;return !count || console_text_ready(line,timeout);}
    if(live_stdio_[0]<0 || !count)return true;
    if(!runtime_.network.external_io)throw HostCallFault("compare 不允許讀取即時標準輸入");
    auto& input=runtime_.standard_input;
    const auto ready=[&] {
        if(live_input_eof_)return true;
        std::uint64_t size=0;
        const auto buffered=input.buffer.direction==CrtFile::Buffer::Direction::Read && input.position==input.buffer.end?input.buffer.used-input.buffer.cursor:0U;
        for(auto offset=static_cast<std::size_t>(input.position-buffered);offset<input.bytes.size();++offset) {
            auto value=input.bytes[offset];
            if(text && value==26)return true;
            if(text && value==13) {
                if(offset+1==input.bytes.size())break;
                if(input.bytes[offset+1]==10){++offset;value=10;}
            }
            if(++size>=count || (line && value==10))return true;
        }
        return false;
    };
    if(ready())return true;
#if !defined(_WIN32)
    pollfd descriptor{live_stdio_[0],POLLIN,0};int result;
    do{result=::poll(&descriptor,1,timeout);}while(result<0 && errno==EINTR);
    if(result<0 || descriptor.revents&POLLNVAL)throw HostCallFault("無法輪詢宿主標準輸入");
    if(!result)return false;
    // 消耗後才壓縮，固定上限只限制單次等待，不限制整個管線的總流量。
    if(input.buffer.direction==CrtFile::Buffer::Direction::Read && input.position==input.buffer.end){
        input.position-=input.buffer.used-input.buffer.cursor;input.buffer.used=input.buffer.cursor=0;input.buffer.direction=CrtFile::Buffer::Direction::None;
    }
    if(input.position){input.bytes.erase(input.bytes.begin(),input.bytes.begin()+static_cast<std::ptrdiff_t>(input.position));input.position=0;}
    if(input.bytes.size()>=2*output_limit+2)throw HostCallFault("即時標準輸入的單次等待超過上限");
    std::array<std::uint8_t,65536> bytes{};ssize_t received;
    do{received=::read(live_stdio_[0],bytes.data(),std::min(bytes.size(),2*output_limit+2-input.bytes.size()));}while(received<0 && errno==EINTR);
    if(received<0){if(errno==EAGAIN || errno==EWOULDBLOCK)return false;throw HostCallFault("讀取宿主標準輸入失敗");}
    if(!received)live_input_eof_=true;
    else input.bytes.insert(input.bytes.end(),bytes.begin(),bytes.begin()+received);
    return ready();
#else
    (void)timeout;throw HostCallFault("此宿主尚未實作即時標準輸入");
#endif
}
bool WindowsServices::await_standard_input(GuestState& state,GuestMemory& memory,std::uint64_t count,bool text,bool line) {
    if(standard_input_ready(count,text,line))return true;
    if(threads_.items.empty()) {
        while(!standard_input_ready(count,text,line,50)){}
        return true;
    }
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
    thread.wait.kind=ThreadWaitKind::StandardInput;thread.wait.handles={count,text?1U:0U,line?1U:0U};
    schedule(state,memory);return false;
}
FileIoResult WindowsServices::write_standard_output(unsigned stream,std::span<const std::uint8_t> bytes,bool retained) {
    const auto completed=[](FileIoResult io){process_io::record(process_io::Kind::Write,io.bytes);return io;};
    if(stream<1 || stream>2 || (!retained && !standard_open_[stream]))return {0,6};
    if(console_handle(0x100+stream) && consoles_[stream]->buffer->window){
        auto& buffer=*consoles_[stream]->buffer;if(buffer.window->closed())return {0,109};
        buffer.write(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
        const auto error=console_sync(stream);return completed(error?FileIoResult{0,error}:FileIoResult{bytes.size(),0});
    }
    if(live_stdio_[stream]<0) {
        if(bytes.size()>output_limit || stdout_.size()+stderr_.size()>output_limit-bytes.size())return {0,8};
        (stream==1?stdout_:stderr_).append(reinterpret_cast<const char*>(bytes.data()),bytes.size());return completed({bytes.size(),0});
    }
    if(!runtime_.network.external_io)throw HostCallFault("compare 不允許寫入即時標準輸出");
#if !defined(_WIN32)
    std::size_t position=0;
    while(position<bytes.size()) {
        const auto written=::write(live_stdio_[stream],bytes.data()+position,bytes.size()-position);
        if(written<0){if(errno==EINTR)continue;return completed({position,errno==EPIPE?109U:5U});}
        if(!written)return completed({position,109});position+=static_cast<std::size_t>(written);
    }
    if(console_handle(0x100+stream))consoles_[stream]->buffer->write(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    return completed({position,0});
#else
    throw HostCallFault("此宿主尚未實作即時標準輸出");
#endif
}
std::optional<std::uint32_t> WindowsServices::invoke_console(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    if(symbol=="SetStdHandle"){
        const auto index=static_cast<std::uint32_t>(arg(0));
        if(index<UINT32_MAX-11 || index>UINT32_MAX-9)fail(87);
        else {runtime_.standard_handles[UINT32_MAX-9-index]=arg(1);result=1;}
    }else if(!console_handle(arg(0)))fail(6);
    else {
#if defined(_WIN32)
        fail(50);
#else
        const auto stream=static_cast<unsigned>(arg(0)-0x100);auto& console=*consoles_[stream];auto& buffer=*console.buffer;termios mode{};
        const bool native=console.descriptor>=0;
        if(native && ::tcgetattr(console.descriptor,&mode))fail(6);
        else if(symbol=="GetConsoleMode"){
            if(!arg(1))fail(87);
            else {const auto flags=stream?buffer.output_mode:buffer.input_mode;
                memory.store(GuestAddress{arg(1)},32,flags);result=1;}
        }else if(symbol=="SetConsoleMode"){
            const auto flags=static_cast<std::uint32_t>(arg(1));
            // 原生視窗提供 keyboard／mouse／resize；TTY 保留其控制訊號政策。
            if((flags&~(stream?7U:0x1ffU)) || (native && stream && (flags&6U)!=6U) ||
               (native && !stream && ((flags&1U)!=((mode.c_lflag&ISIG)?1U:0U))) ||
               (native && !stream && (flags&16U)))fail(50);
            else if(!stream && (flags&4U) && !(flags&2U))fail(87);
            else {
                if(stream){if(flags&1U)mode.c_oflag|=OPOST;else mode.c_oflag&=~static_cast<tcflag_t>(OPOST);}
                else {mode.c_lflag&=~static_cast<tcflag_t>(ICANON|ECHO);if(flags&2U)mode.c_lflag|=ICANON;if(flags&4U)mode.c_lflag|=ECHO;mode.c_cc[VMIN]=1;mode.c_cc[VTIME]=0;}
                if(native && ::tcsetattr(console.descriptor,TCSANOW,&mode))fail(87);
                else {if(native)console.changed=true;if(stream)buffer.output_mode=flags;else buffer.input_mode=flags;result=1;}
            }
        }else {
            const bool writing=symbol.starts_with("Write"),wide=symbol.ends_with('W');const auto count=static_cast<std::uint32_t>(arg(2));
            if((writing && !stream) || (!writing && stream))fail(6);
            else if(arg(4))fail(writing?87:50);
            else if((count && !arg(1)) || (!writing && !arg(3)))fail(87);
            else if(count>output_limit/(wide?2U:1U))fail(8);
            else {
                if(arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
                memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*(wide?2U:1U),writing?Access::Read:Access::Write);
                if(writing){
                    std::string text;
                    if(wide){std::u16string units;units.reserve(count);for(std::uint32_t i=0;i<count;++i)units.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(1)+2ULL*i},16)));
                        try{text=utf16_to_utf8(units);}catch(const std::invalid_argument&){fail(1113);arg.return_to(state,result);return std::nullopt;}}
                    else {text.resize(count);memory.read(GuestAddress{arg(1)},std::span(reinterpret_cast<std::uint8_t*>(text.data()),text.size()));}
                    const auto written=write_standard_output(stream,std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
                    if(written.error)fail(written.error);else {if(arg(3))memory.store(GuestAddress{arg(3)},32,count);result=1;}
                }else {
                    auto& pending=runtime_.console_input;auto& input=runtime_.standard_input;
                    if(count && pending.empty()){
                        if(!await_standard_input(state,memory,1))return std::nullopt;
                        while(input.position<input.bytes.size()){
                            const auto begin=static_cast<std::size_t>(input.position);const auto lead=input.bytes[begin];unsigned extent=lead<0x80?1U:lead>=0xc2 && lead<=0xdf?2U:lead>=0xe0 && lead<=0xef?3U:lead>=0xf0 && lead<=0xf4?4U:1U;
                            if(begin+extent>input.bytes.size() && !live_input_eof_){if(!pending.empty())break;if(!await_standard_input(state,memory,extent))return std::nullopt;continue;}
                            extent=static_cast<unsigned>(std::min<std::size_t>(extent,input.bytes.size()-begin));
                            const auto value=utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(input.bytes.data()+begin),extent));input.position+=extent;
                            if((buffer.input_mode&2) && value==u"\n"){if(pending.empty() || pending.back()!=u'\r')pending+=u'\r';pending+=u'\n';break;}
                            pending+=value;if(pending.size()>=count)break;
                        }
                    }
                    std::uint32_t copied=0;
                    if(wide){copied=static_cast<std::uint32_t>(std::min<std::size_t>(count,pending.size()));for(std::uint32_t i=0;i<copied;++i)memory.store(GuestAddress{arg(1)+2ULL*i},16,pending[i]);pending.erase(0,copied);result=1;}
                    else {
                        std::string text;std::size_t consumed=0;
                        while(consumed<pending.size()){
                            const auto first=pending[consumed];const std::size_t units=first>=0xd800 && first<=0xdbff && consumed+1<pending.size() && pending[consumed+1]>=0xdc00 && pending[consumed+1]<=0xdfff?2:1;
                            const auto encoded=units==1 && first>=0xd800 && first<=0xdfff?std::string("\xef\xbf\xbd"):utf16_to_utf8(std::u16string_view(pending).substr(consumed,units));if(text.size()+encoded.size()>count)break;text+=encoded;consumed+=units;
                        }
                        if(count && !pending.empty() && text.empty())fail(122);
                        else {memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));copied=static_cast<std::uint32_t>(text.size());pending.erase(0,consumed);result=1;}
                    }
                    if(result)process_io::record(process_io::Kind::Read,static_cast<std::uint64_t>(copied)*(wide?2U:1U));
                    memory.store(GuestAddress{arg(3)},32,copied);
                }
            }
        }
#endif
    }
    arg.return_to(state,result);return std::nullopt;
}

}
