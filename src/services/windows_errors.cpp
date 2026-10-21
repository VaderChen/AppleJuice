#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "crt_os_error.hpp"
#include "x64arm/windows_error_support.hpp"
#include <algorithm>
#include <charconv>

namespace x64arm {
std::span<const WindowsServiceDescriptor> error_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","FormatMessageA",7},{"kernel32.dll","FormatMessageW",7},
        {"ntdll.dll","RtlNtStatusToDosError",1},
        {"crt","__doserrno",0},{"crt","_get_errno",1},{"crt","_set_errno",1},
        {"crt","_get_doserrno",1},{"crt","_set_doserrno",1},
        {"crt","strerror",1},{"crt","_wcserror",1},{"crt","perror",1},{"crt","_wperror",1},
        {"crt","strerror_s",3},{"crt","_wcserror_s",3},{"crt","__sys_nerr",0},{"crt","__sys_errlist",0}
    };return table;
}
namespace {
#include "windows_generic_error_data.inc"
constexpr std::uint32_t crt_error_count=43;
std::string_view crt_message(std::uint32_t code) {
    switch(code) {
    case 0:return "Error 0";case 1:return "Operation not permitted";case 2:return "No such file or directory";
    case 3:return "No such process";case 4:return "Interrupted function call";case 5:return "Input/output error";
    case 6:return "No such device or address";case 7:return "Arg list too long";
    case 8:return "Exec format error";case 9:return "Bad file number";case 10:return "No child processes";
    case 11:return "Resource temporarily unavailable";case 12:return "Not enough core";case 13:return "Permission denied";
    case 14:return "Bad address";case 16:return "Device or resource busy";case 17:return "File exists";case 18:return "Cross-device link";
    case 19:return "No such device";case 20:return "Not a directory";case 21:return "Is a directory";case 22:return "Invalid argument";
    case 23:return "Too many open files in system";case 24:return "Too many open files";case 25:return "Inappropriate I/O control operation";
    case 27:return "File too large";case 28:return "No space left on device";case 29:return "Invalid seek";case 30:return "Read-only file system";
    case 31:return "Too many links";case 32:return "Broken pipe";case 33:return "Math argument";case 34:return "Result too large";
    case 36:return "Resource deadlock would occur";case 38:return "Filename too long";case 39:return "No locks available";
    case 40:return "Function not implemented";case 41:return "Directory not empty";case 42:return "Illegal byte sequence";
    default:return "Unknown error";
    }
}
std::string_view system_message(std::uint32_t code) {
    switch (code) {
    case 0:return "The operation completed successfully.";
    case 2:return "The system cannot find the file specified.";
    case 3:return "The system cannot find the path specified.";
    case 4:return "The system cannot open the file.";
    case 5:return "Access is denied.";
    case 6:return "The handle is invalid.";
    case 8:return "Not enough memory resources are available to process this command.";
    case 18:return "There are no more files.";
    case 32:return "The process cannot access the file because it is being used by another process.";
    case 38:return "Reached the end of the file.";
    case 50:return "The request is not supported.";
    case 80:return "The file exists.";
    case 87:return "The parameter is incorrect.";
    case 109:return "The pipe has been ended.";
    case 112:return "There is not enough space on the disk.";
    case 120:return "This function is not supported on this system.";
    case 122:return "The data area passed to a system call is too small.";
    case 123:return "The filename, directory name, or volume label syntax is incorrect.";
    case 126:return "The specified module could not be found.";
    case 127:return "The specified procedure could not be found.";
    case 131:return "An attempt was made to move the file pointer before the beginning of the file.";
    case 183:return "Cannot create a file when that file already exists.";
    case 203:return "The system could not find the environment option that was entered.";
    case 206:return "The filename or extension is too long.";
    case 234:return "More data is available.";
    case 258:return "The wait operation timed out.";
    case 259:return "No more data is available.";
    case 267:return "The directory name is invalid.";
    case 995:return "The I/O operation has been aborted because of either a thread exit or an application request.";
    case 996:return "Overlapped I/O event is not in a signaled state.";
    case 997:return "Overlapped I/O operation is in progress.";
    case 1117:return "The request could not be performed because of an I/O device error.";
    case 10004:return "Socket operation interrupted.";
    case 10013:return "Socket access denied.";
    case 10014:return "Invalid socket buffer or address.";
    case 10022:return "Invalid socket parameter.";
    case 10035:return "Socket operation would block.";
    case 10038:return "Invalid socket handle.";
    case 10047:return "Unsupported address family.";
    case 10048:return "Network address is already in use.";
    case 10049:return "Requested network address is unavailable.";
    case 10050:return "Network is down.";
    case 10051:return "Network is unreachable.";
    case 10053:return "Connection was aborted.";
    case 10054:return "Connection was reset by the peer.";
    case 10055:return "Insufficient socket buffer space.";
    case 10057:return "Socket is not connected.";
    case 10060:return "Connection timed out.";
    case 10061:return "Connection refused.";
    case 10065:return "Remote host is unreachable.";
    case 10093:return "Winsock has not been initialized.";
    case 10109:return "Requested network service was not found.";
    case 11001:return "Host name was not found.";
    case 11002:return "Temporary DNS lookup failure.";
    case 11003:return "DNS lookup failed.";
    case 11004:return "No address records are available for this host.";
    default:return {};
    }
}
std::u16string format_message(const GuestMemory& memory,std::u16string_view text,std::uint64_t arguments,bool array,bool wide,bool ignore) {
    std::u16string result;
    for (std::size_t i=0;i<text.size();++i) {
        if (result.size()>65535) throw std::length_error("訊息過長");
        if (text[i]!='%' || i+1==text.size()) {result+=text[i];continue;}
        const auto c=text[++i];
        if (c=='0') break;
        if (c>='1' && c<='9') {
            const auto begin=i-1;unsigned index=static_cast<unsigned>(c-'0');
            if (i+1<text.size() && text[i+1]>='0' && text[i+1]<='9') index=index*10+static_cast<unsigned>(text[++i]-'0');
            std::u16string pattern=u"s";
            if (i+1<text.size() && text[i+1]=='!') {
                const auto end=text.find(u'!',i+2);if (end==text.npos) throw std::invalid_argument("不完整訊息插入格式");
                pattern=text.substr(i+2,end-i-2);i=end;
            }
            if (ignore) {result+=text.substr(begin,i-begin+1);continue;}
            if (!arguments) throw std::invalid_argument("缺少訊息插入參數");
            const auto base=array?arguments:memory.load(GuestAddress{arguments},64);
            const auto value=memory.load(GuestAddress{base+(index-1)*8U},64);
            if (pattern==u"s" || pattern==u"ls" || pattern==u"ws" || pattern==u"hs") {
                const bool utf16=pattern==u"ls" || pattern==u"ws" || (wide && pattern!=u"hs");
                result+=utf16?read_utf16(memory,GuestAddress{value}):utf8_to_utf16(read_ansi(memory,value));
            } else {
                if (pattern.empty()) throw std::invalid_argument("空的訊息插入格式");
                const auto type=pattern.back();if (type!='d' && type!='i' && type!='u' && type!='x' && type!='X') throw std::invalid_argument("未支援訊息插入格式");
                pattern.pop_back();bool long64=false;
                if (pattern.ends_with(u"I64")) {long64=true;pattern.resize(pattern.size()-3);}
                else if (pattern.ends_with(u"l")) pattern.pop_back();
                const bool zero=!pattern.empty() && pattern[0]=='0';unsigned width=0;
                for (const auto digit:pattern) {if (digit<'0' || digit>'9' || width>6553) throw std::invalid_argument("訊息欄寬無效");width=width*10+static_cast<unsigned>(digit-'0');}
                auto magnitude=long64?value:static_cast<std::uint32_t>(value);bool negative=false;
                if (type=='d' || type=='i') {negative=long64?(value>>63)!=0:(value&0x80000000)!=0;if (negative) magnitude=long64?~value+1:static_cast<std::uint32_t>(~value+1);}
                std::array<char,32> buffer{};const auto [end,error]=std::to_chars(buffer.data(),buffer.data()+buffer.size(),magnitude,type=='x' || type=='X'?16:10);(void)error;
                std::string digits(buffer.data(),end);if (type=='X') for (auto& digit:digits) if (digit>='a' && digit<='f') digit=static_cast<char>(digit-'a'+'A');
                const auto padding=width>digits.size()+negative?width-digits.size()-negative:0;
                if (!zero) result.append(padding,u' ');if (negative) result+=u'-';if (zero) result.append(padding,u'0');result+=utf8_to_utf16(digits);
            }
        } else if (c=='n') result+=u"\r\n";
        else if (c=='r') result+=u'\r';else if (c=='t') result+=u'\t';else result+=c;
    }
    return result;
}
}
std::string_view windows_crt_error_message(std::uint32_t code){return crt_message(code);}
std::optional<std::uint32_t> windows_generic_error_condition(std::uint32_t code) noexcept{
    const auto entry=std::lower_bound(std::begin(windows_error_conditions),std::end(windows_error_conditions),code,
        [](const auto& value,std::uint32_t code){return value.system<code;});
    if(entry==std::end(windows_error_conditions) || entry->system!=code)return std::nullopt;
    return entry->generic;
}
std::string_view windows_generic_error_message(std::uint32_t code) noexcept{
    const auto entry=std::lower_bound(std::begin(windows_generic_messages),std::end(windows_generic_messages),code,
        [](const auto& value,std::uint32_t code){return value.code<code;});
    return entry==std::end(windows_generic_messages) || entry->code!=code?"unknown error":entry->message;
}
std::u16string windows_error_message(std::uint32_t code){
    // 共用 WinSDK winerror.h 的 HRESULT MessageText；不以客體的自訂訊息覆寫系統描述。
    std::string_view message;
    switch(code){
    case 0x80004001U:message="Not implemented";break;
    case 0x80004002U:message="No such interface supported";break;
    case 0x80004003U:message="Invalid pointer";break;
    case 0x80004004U:message="Operation aborted";break;
    case 0x80004005U:message="Unspecified error";break;
    case 0x80070005U:message="General access denied error";break;
    case 0x8007000eU:message="Ran out of memory";break;
    case 0x80070057U:message="One or more arguments are invalid";break;
    case 0x8000000bU:message="The operation attempted to access data outside the valid range";break;
    case 0x8000000cU:message="A concurrent or interleaved operation changed the state of the object, invalidating this operation.";break;
    case 0x80000013U:message="The object has been closed.";break;
    default:message=system_message((code&0xffff0000U)==0x80070000U?code&0xffffU:code);break;
    }
    return message.empty()?std::u16string{}:utf8_to_utf16(message)+u"\r\n";
}
std::optional<std::uint32_t> WindowsServices::invoke_error(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);std::uint64_t result=0;
    if(service.family=="crt") {
        const auto name=service.symbol;
        if(name=="__doserrno" || name=="_get_errno" || name=="_set_errno" || name=="_get_doserrno" || name=="_set_doserrno") {
            const auto cell=name.find("doserrno")!=name.npos?crt_os_error::cell(runtime_.errno_cell):runtime_.errno_cell;
            if(name=="__doserrno")result=cell;
            else if(name.starts_with("_set_"))memory.store(GuestAddress{cell},32,arg(0));
            else if(!arg(0)) {
                memory.store(GuestAddress{runtime_.errno_cell},32,22);
                if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
                return 0xc0000417U;
            } else memory.store(GuestAddress{arg(0)},32,memory.load(GuestAddress{cell},32));
            arg.return_to(state,result);return std::nullopt;
        }
        if(name=="__sys_nerr" || name=="__sys_errlist"){
            if(!runtime_.crt_errlist){
                // 與 strerror 共用 errno 對照表；末項保留越界錯誤的 Unknown error。
                const unsigned unit=x86_?4U:8U;
                const auto table=runtime_allocate(memory,{},(crt_error_count+1)*unit);
                for(std::uint32_t code=0;code<=crt_error_count;++code){
                    const auto message=crt_message(code);
                    std::vector<std::uint8_t> bytes(message.begin(),message.end());bytes.push_back(0);
                    const auto pointer=runtime_allocate(memory,bytes);
                    memory.store(GuestAddress{table+code*unit},static_cast<std::uint8_t>(unit*8),pointer);
                }
                const auto count=runtime_allocate(memory,{},4);
                memory.store(GuestAddress{count},32,crt_error_count);
                runtime_.crt_nerr_cell=count;runtime_.crt_errlist=table;
            }
            arg.return_to(state,name=="__sys_nerr"?runtime_.crt_nerr_cell:runtime_.crt_errlist);return std::nullopt;
        }
        if(service.symbol=="strerror_s" || service.symbol=="_wcserror_s"){
            if(!arg(0) || !arg(1)){
                memory.store(GuestAddress{runtime_.errno_cell},32,22);
                if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
                return 0xc0000417U;
            }
            const auto message=crt_message(static_cast<std::uint32_t>(arg(2))).substr(0,static_cast<std::size_t>(std::min<std::uint64_t>(arg(1)-1,UINT32_MAX)));
            if(service.symbol=="_wcserror_s")memory.write(GuestAddress{arg(0)},utf16_bytes(utf8_to_utf16(message)));
            else {std::vector<std::uint8_t> bytes(message.begin(),message.end());bytes.push_back(0);memory.write(GuestAddress{arg(0)},bytes);}
            arg.return_to(state,0);return std::nullopt;
        }
        const bool print=service.symbol=="perror" || service.symbol=="_wperror",wide=service.symbol=="_wcserror" || service.symbol=="_wperror";
        const auto code=static_cast<std::uint32_t>(print?memory.load(GuestAddress{runtime_.errno_cell},32):arg(0));
        std::string message(crt_message(code));
        if(print) {
            const auto prefix=arg(0)?wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0)):std::string{};
            if(!prefix.empty())message=prefix+": "+message;message+='\n';
            const auto written=write_standard_output(2,std::span(reinterpret_cast<const std::uint8_t*>(message.data()),message.size()));
            if(written.error)memory.store(GuestAddress{runtime_.errno_cell},32,written.error==109?32:9);
        } else {
            auto found=runtime_.crt_error_buffers.find(threads_.current);
            if(found==runtime_.crt_error_buffers.end()) {
                const auto pointer=allocate_heap(memory,512);
                if(pointer)found=runtime_.crt_error_buffers.emplace(threads_.current,pointer).first;
            }
            if(found!=runtime_.crt_error_buffers.end()) {
                result=found->second;
                if(wide)memory.write(GuestAddress{result},utf16_bytes(utf8_to_utf16(message)));
                else {message+='\0';memory.write(GuestAddress{result},std::span(reinterpret_cast<const std::uint8_t*>(message.data()),message.size()));}
            }
        }
    } else if (service.symbol=="RtlNtStatusToDosError") {
        switch (static_cast<std::uint32_t>(arg(0))) {
        case 0:result=0;break;case 0x103:result=997;break;case 0x10c:result=1022;break;case 0x105:result=234;break;case 0x80000005:result=234;break;
        case 0x80000006:result=18;break;case 0xc0000003:case 0xc000000d:result=87;break;case 0xc0000004:result=24;break;
        case 0xc000000f:result=2;break;case 0xc0000010:result=1;break;case 0xc000007f:result=112;break;case 0xc000009a:result=1450;break;case 0xc0000103:result=267;break;
        case 0xc0000008:result=6;break;case 0xc0000011:result=38;break;case 0xc0000017:result=8;break;
        case 0xc0000022:result=5;break;case 0xc0000023:result=122;break;case 0xc0000034:result=2;break;
        case 0xc0000035:result=183;break;case 0xc000003a:result=3;break;case 0xc0000043:result=32;break;
        case 0xc00000bb:result=50;break;case 0xc0000120:result=995;break;default:result=317;break;
        }
    } else {
        const auto flags=static_cast<std::uint32_t>(arg(0));const bool wide=service.symbol.back()=='W';
        const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
        if (flags&~0x3fffU || !(flags&0x1c00) || ((flags&0x400) && (flags&0x1800))) fail(87);
        else if (arg(3) && arg(3)!=0x409 && !(flags&0x400)) fail(1815);
        else {
            std::u16string text;
            if (flags&0x400) text=wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16(read_ansi(memory,arg(1)));
            else if (flags&0x1000) text=windows_error_message(static_cast<std::uint32_t>(arg(2)));
            if (text.empty() && !(flags&0x400)) fail(flags&0x1000?317:1813);
            else try {
                text=format_message(memory,text,arg(6),(flags&0x2000)!=0,wide,(flags&0x200)!=0);
                // 固定寬度折行需要保留 %n 與一般換行的差異，尚未提供時回報錯誤。
                if ((flags&0xff)!=0 && (flags&0xff)!=255) throw std::invalid_argument("未支援固定欄寬折行");
                auto bytes=wide?utf16_bytes(text):std::vector<std::uint8_t>{};
                if (!wide) {const auto narrow=utf16_to_utf8(text);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
                const auto length=bytes.size()/(wide?2:1)-1;
                if (bytes.size()>131072) fail(234);
                else if (flags&0x100) {
                    memory.check(GuestAddress{arg(4)},8,Access::Write);
                    const auto allocation=allocate_heap(memory,std::max<std::uint64_t>(bytes.size(),static_cast<std::uint32_t>(arg(5))*(wide?2ULL:1ULL)));
                    if (!allocation) fail(8);else {memory.write(GuestAddress{allocation},bytes);runtime_.local_allocations.insert(allocation);memory.store(GuestAddress{arg(4)},64,allocation);result=length;}
                } else if (static_cast<std::uint32_t>(arg(5))<=length) fail(122);
                else {memory.write(GuestAddress{arg(4)},bytes);result=length;}
            } catch (const std::invalid_argument&) {fail(87);} catch (const std::length_error&) {fail(234);}
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
