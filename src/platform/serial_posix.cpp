#include "x64arm/serial.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/serial/ioss.h>
#endif

namespace x64arm {
namespace {
std::uint64_t now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
std::uint32_t get(const std::array<std::uint8_t,28>& b,unsigned at,unsigned size=4){std::uint32_t v=0;for(unsigned i=0;i<size;++i)v|=std::uint32_t{b[at+i]}<<(8*i);return v;}
void put(std::array<std::uint8_t,28>& b,unsigned at,std::uint32_t v,unsigned size=4){for(unsigned i=0;i<size;++i)b[at+i]=static_cast<std::uint8_t>(v>>(8*i));}
#if !defined(_WIN32)
std::uint32_t error(){switch(errno){case ENOENT:case ENXIO:return 2;case EACCES:case EPERM:case EBUSY:return 5;case EINVAL:return 87;case ENOTTY:case ENOTSUP:return 50;case ENODEV:return 1167;default:return 1117;}}
const std::vector<std::pair<std::uint32_t,speed_t>>& rates(){
    static const std::vector<std::pair<std::uint32_t,speed_t>> values{{110,B110},{300,B300},{600,B600},{1200,B1200},{2400,B2400},{4800,B4800},{9600,B9600},{19200,B19200},{38400,B38400},{57600,B57600},{115200,B115200},
#ifdef B14400
        {14400,B14400},
#endif
#ifdef B230400
        {230400,B230400},
#endif
    };return values;
}
std::uint32_t rate(speed_t value){for(const auto& [n,s]:rates())if(s==value)return n;return static_cast<std::uint32_t>(value);}
#if defined(__APPLE__)
constexpr tcflag_t cts_flow=CCTS_OFLOW,dsr_flow=CDSR_OFLOW,dtr_flow=CDTR_IFLOW,rts_flow=CRTS_IFLOW;
#else
constexpr tcflag_t cts_flow=CRTSCTS,dsr_flow=0,dtr_flow=0,rts_flow=CRTSCTS;
#endif
#endif
}
struct NativeSerial::Impl {
    std::array<std::uint8_t,28> dcb{};
    std::array<std::uint32_t,5> timeouts{};
    std::deque<std::uint8_t> input;
    std::uint32_t mask{},events{},errors{},input_capacity{65536},output_capacity{65536},custom_baud{};
    std::uint64_t generation{};
    unsigned marker{};bool xoff_sent{},paused{},transmitting{},readable{};
#if !defined(_WIN32)
    int fd{-1},original_modem{},last_modem{};
    termios original{};bool restore{},exclusive{},modem_available{},modem_changed{},breaking{};
    ~Impl(){if(fd>=0){if(breaking)(void)::ioctl(fd,TIOCCBRK);if(restore)(void)::tcsetattr(fd,TCSANOW,&original);if(modem_changed)(void)::ioctl(fd,TIOCMSET,&original_modem);if(exclusive)(void)::ioctl(fd,TIOCNXCL);::close(fd);}}
    std::uint32_t outq(std::uint32_t& count){int size=0;if(::ioctl(fd,TIOCOUTQ,&size))return error();count=static_cast<std::uint32_t>(std::max(0,size));return 0;}
    void receive(std::uint8_t value){
        events|=1;if(value==dcb[25])events|=2;
        if(value==0 && (get(dcb,8)&0x800))return;
        if(input.size()<input_capacity)input.push_back(value);else {errors|=1;events|=128;}
    }
    std::uint32_t sample(){
        int lines=0;if(modem_available){if(::ioctl(fd,TIOCMGET,&lines))return error();
            const auto changed=lines^last_modem;last_modem=lines;
            if(changed&TIOCM_CTS)events|=8;if(changed&TIOCM_DSR)events|=16;
            if(changed&TIOCM_CAR)events|=32;if(changed&TIOCM_RNG)events|=256;
        }
        const auto flags=get(dcb,8);
        std::array<std::uint8_t,4096> bytes{};
        while(readable && input.size()<input_capacity){
            pollfd descriptor{fd,POLLIN,0};const auto ready=::poll(&descriptor,1,0);
            if(ready<0){if(errno==EINTR)continue;return error();}
            if(!ready || !(descriptor.revents&POLLIN)){
                if(descriptor.revents&(POLLHUP|POLLERR|POLLNVAL))return 1167;break;
            }
            const auto length=::read(fd,bytes.data(),std::min(bytes.size(),input_capacity-input.size()));
            if(length<0){if(errno==EINTR)continue;if(errno==EAGAIN || errno==EWOULDBLOCK)break;errors|=0x400;events|=128;return error();}
            if(!length)return 1167;
            for(ssize_t i=0;i<length;++i){const auto value=bytes[static_cast<std::size_t>(i)];
                if((flags&64) && !(lines&TIOCM_DSR))continue;
                // PARMRK 的 FF FF 是資料，FF 00 xx 是 parity／break 指示。
                if(flags&2){
                    if(marker==1){if(value==0){marker=2;continue;}marker=0;if(value==255){receive(255);continue;}receive(255);}
                    else if(marker==2){marker=0;errors|=value?4U:16U;events|=value?128U|512U:128U|64U;receive((value && (flags&0x400))?dcb[23]:value);continue;}
                    else if(value==255){marker=1;continue;}
                }
                receive(value);
            }
        }
        if(transmitting){std::uint32_t queued=0;if(const auto e=outq(queued))return e;if(!queued){transmitting=false;events|=4;}}
        return flow();
    }
    std::uint32_t flow(){
        const auto flags=get(dcb,8);const auto high=input_capacity-get(dcb,16,2),low=get(dcb,14,2);
        const bool stopping=(flags&0x200) && input.size()>=high;
        const bool starting=xoff_sent && (!(flags&0x200) || input.size()<=low);
        if((!xoff_sent && stopping) || starting){
            if(::tcflow(fd,starting?TCION:TCIOFF))return error();
            if(!(flags&128) && ::tcflow(fd,starting?TCOON:TCOOFF))return error();
            xoff_sent=!starting;
        }
        return 0;
    }
#endif
};
NativeSerial::NativeSerial(std::unique_ptr<Impl> value):impl_(std::move(value)){}
NativeSerial::~NativeSerial()=default;
std::vector<std::string> NativeSerial::devices(){
    std::vector<std::string> result;
#if defined(__APPLE__)
    auto matching=IOServiceMatching(kIOSerialBSDServiceValue);if(!matching)return result;
    CFDictionarySetValue(matching,CFSTR(kIOSerialBSDTypeKey),CFSTR(kIOSerialBSDAllTypes));
    io_iterator_t iterator=0;if(IOServiceGetMatchingServices(MACH_PORT_NULL,matching,&iterator)!=KERN_SUCCESS)return result;
    while(const auto service=IOIteratorNext(iterator)){
        const auto path=IORegistryEntryCreateCFProperty(service,CFSTR(kIOCalloutDeviceKey),kCFAllocatorDefault,0);
        if(path){if(CFGetTypeID(path)==CFStringGetTypeID()){std::array<char,4096> bytes{};if(CFStringGetCString(static_cast<CFStringRef>(path),bytes.data(),static_cast<CFIndex>(bytes.size()),kCFStringEncodingUTF8))result.emplace_back(bytes.data());}CFRelease(path);}
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
#elif !defined(_WIN32)
    std::error_code e;for(const auto& entry:std::filesystem::directory_iterator("/dev",e)){const auto name=entry.path().filename().string();if(name.starts_with("ttyS") || name.starts_with("ttyUSB") || name.starts_with("ttyACM"))result.push_back(entry.path().string());}
#endif
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
bool NativeSerial::is_device(const std::string& path){
#if defined(_WIN32)
    (void)path;return false;
#else
    // 只查詢 termios，不套用模式、流量控制或排他鎖。
    const auto fd=::open(path.c_str(),O_RDONLY|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return false;
    struct stat info{};termios settings{};const bool result=::fstat(fd,&info)==0 && S_ISCHR(info.st_mode) && ::tcgetattr(fd,&settings)==0;
    ::close(fd);return result;
#endif
}
NativeSerial::Opened NativeSerial::open(const std::string& path,std::uint32_t access){
#if defined(_WIN32)
    (void)path;(void)access;return {{},50};
#else
    auto value=std::make_unique<Impl>();value->readable=(access&0x80000000U)!=0;value->fd=::open(path.c_str(),(value->readable?((access&0x40000000U)?O_RDWR:O_RDONLY):O_WRONLY)|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if(value->fd<0)return {{},error()};
    struct stat info{};if(::fstat(value->fd,&info))return {{},error()};if(!S_ISCHR(info.st_mode))return {{},87};
    if(::tcgetattr(value->fd,&value->original))return {{},error()};value->restore=true;
    if(::ioctl(value->fd,TIOCEXCL))return {{},error()};value->exclusive=true;
    auto mode=value->original;::cfmakeraw(&mode);mode.c_cflag|=CLOCAL|CREAD;mode.c_cc[VMIN]=0;mode.c_cc[VTIME]=0;
    if(::tcsetattr(value->fd,TCSANOW,&mode))return {{},error()};
    value->modem_available=::ioctl(value->fd,TIOCMGET,&value->original_modem)==0;value->last_modem=value->original_modem;
    put(value->dcb,0,28);put(value->dcb,8,1U|128U|(value->original_modem&TIOCM_DTR?16U:0U)|(value->original_modem&TIOCM_RTS?4096U:0U));
    put(value->dcb,14,128,2);put(value->dcb,16,128,2);value->dcb[21]=mode.c_cc[VSTART];value->dcb[22]=mode.c_cc[VSTOP];
    auto port=std::shared_ptr<NativeSerial>(new NativeSerial(std::move(value)));std::array<std::uint8_t,28> state{};
    if(const auto e=port->get_state(state))return {{},e};port->impl_->dcb=state;return {std::move(port),0};
#endif
}
std::uint32_t NativeSerial::get_state(std::array<std::uint8_t,28>& value){
#if defined(_WIN32)
    (void)value;return 50;
#else
    auto& p=*impl_;termios mode{};if(::tcgetattr(p.fd,&mode))return error();value=p.dcb;
    put(value,0,28);put(value,4,p.custom_baud?p.custom_baud:rate(::cfgetospeed(&mode)));
    auto flags=get(value,8)&~(2U|4U|8U|256U);
    if(mode.c_iflag&INPCK)flags|=2;if(mode.c_cflag&cts_flow)flags|=4;if(dsr_flow && (mode.c_cflag&dsr_flow))flags|=8;
    if(mode.c_iflag&IXON)flags|=256;put(value,8,flags);
    const auto size=mode.c_cflag&CSIZE;value[18]=size==CS5?5:size==CS6?6:size==CS7?7:8;
    value[19]=(mode.c_cflag&PARENB)?((mode.c_cflag&PARODD)?1:2):0;
    value[20]=(mode.c_cflag&CSTOPB)?(value[18]==5?1:2):0;value[21]=mode.c_cc[VSTART];value[22]=mode.c_cc[VSTOP];return 0;
#endif
}
std::uint32_t NativeSerial::set_state(const std::array<std::uint8_t,28>& value){
#if defined(_WIN32)
    (void)value;return 50;
#else
    auto& p=*impl_;const auto flags=get(value,8),baud=get(value,4),dtr=(flags>>4)&3,rts=(flags>>12)&3;
    if(get(value,0)!=28 || !(flags&1) || (flags&0xffff8000U) || dtr==3 || value[18]<5 || value[18]>8 || value[19]>4 || value[20]>2 || !baud || value[21]==value[22] || (value[20]==1 && value[18]!=5) || (value[20]==2 && value[18]==5))return 87;
    if((flags&0x200) && (get(value,14,2)>=p.input_capacity || get(value,16,2)>=p.input_capacity || get(value,14,2)+get(value,16,2)>=p.input_capacity))return 87;
    if(value[19]>2 || rts==3 || ((flags&8) && !dsr_flow) || (dtr==2 && !dtr_flow))return 50;
    if(!p.modem_available && ((flags&76) || dtr!=((get(p.dcb,8)>>4)&3) || rts!=((get(p.dcb,8)>>12)&3)))return 50;
    termios old{};if(::tcgetattr(p.fd,&old))return error();auto mode=old;::cfmakeraw(&mode);mode.c_cflag|=CLOCAL|CREAD;
    mode.c_cflag&=~static_cast<tcflag_t>(CSIZE|PARENB|PARODD|CSTOPB|cts_flow|dsr_flow|dtr_flow|rts_flow);
    mode.c_cflag|=value[18]==5?CS5:value[18]==6?CS6:value[18]==7?CS7:CS8;
    if(value[19])mode.c_cflag|=PARENB;if(value[19]==1)mode.c_cflag|=PARODD;if(value[20])mode.c_cflag|=CSTOPB;
    if(flags&4)mode.c_cflag|=cts_flow;if(flags&8)mode.c_cflag|=dsr_flow;if(dtr==2)mode.c_cflag|=dtr_flow;if(rts==2)mode.c_cflag|=rts_flow;
    if(flags&2)mode.c_iflag|=INPCK|PARMRK;if(flags&256)mode.c_iflag|=IXON;
    mode.c_cc[VSTART]=value[21];mode.c_cc[VSTOP]=value[22];mode.c_cc[VMIN]=0;mode.c_cc[VTIME]=0;
    const auto speed=std::find_if(rates().begin(),rates().end(),[&](const auto& item){return item.first==baud;});
    const auto native=speed==rates().end()?B9600:speed->second;
    if(::cfsetispeed(&mode,native) || ::cfsetospeed(&mode,native))return error();
    if(::tcsetattr(p.fd,TCSANOW,&mode))return error();
    std::uint32_t failure=0;
    if(speed==rates().end()){
#if defined(__APPLE__)
        speed_t requested=baud;if(::ioctl(p.fd,IOSSIOSPEED,&requested))failure=error();
#else
        failure=50;
#endif
    }
    termios applied{};
    if(!failure && ::tcgetattr(p.fd,&applied))failure=error();
    constexpr auto control=static_cast<tcflag_t>(CSIZE|PARENB|PARODD|CSTOPB|cts_flow|dsr_flow|dtr_flow|rts_flow);
    constexpr auto input=static_cast<tcflag_t>(INPCK|PARMRK|IXON);
    if(!failure && ((applied.c_cflag&control)!=(mode.c_cflag&control) || (applied.c_iflag&input)!=(mode.c_iflag&input) || applied.c_cc[VSTART]!=value[21] || applied.c_cc[VSTOP]!=value[22] ||
        (speed!=rates().end() && ::cfgetospeed(&applied)!=native)))failure=50;
    int old_lines=0;if(!failure && p.modem_available){if(::ioctl(p.fd,TIOCMGET,&old_lines))failure=error();
        else {auto lines=old_lines;if(dtr<2){if(dtr)lines|=TIOCM_DTR;else lines&=~TIOCM_DTR;}if(rts<2){if(rts)lines|=TIOCM_RTS;else lines&=~TIOCM_RTS;}
            if(lines!=old_lines){if(::ioctl(p.fd,TIOCMSET,&lines))failure=error();else p.modem_changed=true;}
        }
    }
    if(failure){(void)::tcsetattr(p.fd,TCSANOW,&old);return failure;}
    p.dcb=value;p.custom_baud=speed==rates().end()?baud:0;p.marker=0;return p.flow();
#endif
}
std::array<std::uint32_t,5> NativeSerial::timeouts()const{return impl_->timeouts;}
std::uint32_t NativeSerial::timeouts(const std::array<std::uint32_t,5>& value){if(value[0]==UINT32_MAX && value[1]==UINT32_MAX && value[2]==UINT32_MAX)return 87;impl_->timeouts=value;return 0;}
std::uint32_t NativeSerial::setup(std::uint32_t in,std::uint32_t out){if(!in || !out || in>16U*1024U*1024U || out>16U*1024U*1024U || in<impl_->input.size())return 87;if((get(impl_->dcb,8)&512) && get(impl_->dcb,14,2)+get(impl_->dcb,16,2)>=in)return 87;impl_->input_capacity=in;impl_->output_capacity=out;return 0;}
std::uint32_t NativeSerial::mask()const{return impl_->mask;}
std::uint32_t NativeSerial::mask(std::uint32_t value){if(value&~0x3ffU)return 50;
#if !defined(_WIN32)
    if((value&3) && !impl_->readable)return 5;
    if((value&0x138U) && !impl_->modem_available)return 50;
    if(const auto e=impl_->sample())return e;
#endif
    impl_->mask=value;impl_->events=0;++impl_->generation;return 0;
}
std::uint32_t NativeSerial::modem(std::uint32_t& value){
#if defined(_WIN32)
    (void)value;return 50;
#else
    int status=0;if(::ioctl(impl_->fd,TIOCMGET,&status))return error();value=(status&TIOCM_CTS?16U:0U)|(status&TIOCM_DSR?32U:0U)|(status&TIOCM_RNG?64U:0U)|(status&TIOCM_CAR?128U:0U);return 0;
#endif
}
std::uint32_t NativeSerial::escape(unsigned command){
#if defined(_WIN32)
    (void)command;return 50;
#else
    auto& p=*impl_;const auto flags=get(p.dcb,8);
    if(command==1 || command==2){if(::tcflow(p.fd,command==1?TCOOFF:TCOON))return error();p.paused=command==1;return 0;}
    if(command==8 || command==9){if(::ioctl(p.fd,command==8?TIOCSBRK:TIOCCBRK))return error();p.breaking=command==8;return 0;}
    if(command==7)return 50;if(command<3 || command>6)return 87;
    if((command<5?((flags>>12)&3):((flags>>4)&3))==2)return 87;
    int bit=command<5?TIOCM_RTS:TIOCM_DTR;if(::ioctl(p.fd,(command&1)?TIOCMBIS:TIOCMBIC,&bit))return error();p.modem_changed=true;return 0;
#endif
}
std::uint32_t NativeSerial::purge(unsigned flags){
    if(flags&~15U)return 87;
#if defined(_WIN32)
    return 50;
#else
    if(flags&12){if(::tcflush(impl_->fd,(flags&12)==12?TCIOFLUSH:(flags&8)?TCIFLUSH:TCOFLUSH))return error();}
    if(flags&8){impl_->input.clear();impl_->marker=0;if(const auto e=impl_->flow())return e;}
    return 0;
#endif
}
std::uint32_t NativeSerial::clear(std::uint32_t& errors,std::array<std::uint32_t,3>& stat){
#if defined(_WIN32)
    (void)errors;(void)stat;return 50;
#else
    auto& p=*impl_;if(const auto e=p.sample())return e;std::uint32_t output=0;if(const auto e=p.outq(output))return e;
    const auto flags=get(p.dcb,8);stat={(flags&4) && !(p.last_modem&TIOCM_CTS)?1U:0U,static_cast<std::uint32_t>(p.input.size()),output};
    if((flags&8) && !(p.last_modem&TIOCM_DSR))stat[0]|=2;if(p.paused)stat[0]|=8;if(p.xoff_sent)stat[0]|=16;
    errors=p.errors;p.errors=0;return 0;
#endif
}
std::shared_ptr<SerialTransfer> NativeSerial::transfer(SerialTransfer::Kind kind,std::vector<std::uint8_t> data){auto io=std::make_shared<SerialTransfer>();io->kind=kind;io->data=std::move(data);io->timeouts=impl_->timeouts;io->started=now();io->generation=impl_->generation;return io;}
PipeResult NativeSerial::poll(SerialTransfer& io){
#if defined(_WIN32)
    (void)io;return {0,50};
#else
    auto& p=*impl_;if(const auto e=p.sample())return {io.position,e};
    const auto elapsed=now()-io.started;const auto requested=static_cast<std::uint32_t>(io.data.size());
    if(io.kind==SerialTransfer::Kind::Event){
        if(io.generation!=p.generation || !p.mask){io.events=0;return {};}
        io.events=p.events&p.mask;if(!io.events)return {0,0,true};p.events&=~io.events;return {};
    }
    if(io.kind==SerialTransfer::Kind::Flush){std::uint32_t queued=0;if(const auto e=p.outq(queued))return {0,e};return {0,0,queued!=0};}
    if(p.errors && (get(p.dcb,8)&0x4000))return {io.position,1117};
    if(io.kind==SerialTransfer::Kind::Write){
        const auto flags=get(p.dcb,8);
        const bool held=p.paused || (p.xoff_sent && !(flags&128)) || ((flags&4) && !(p.last_modem&TIOCM_CTS)) || ((flags&8) && !(p.last_modem&TIOCM_DSR));
        if(io.position<requested && !held){const auto count=::write(p.fd,io.data.data()+io.position,std::min(requested-io.position,p.output_capacity));
            if(count>0){io.position+=static_cast<std::uint32_t>(count);p.transmitting=true;}
            else if(count<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)return {io.position,error()};
        }
        if(io.position==requested)return {io.position,0};
        const auto total=std::uint64_t{io.timeouts[3]}*requested+io.timeouts[4];if(total && elapsed>=total)return {io.position,121};return {io.position,0,true};
    }
    const auto previous=io.position;
    while(io.position<requested && !p.input.empty()){io.data[io.position++]=p.input.front();p.input.pop_front();}
    if(previous!=io.position)io.last_byte=now();if(const auto e=p.flow())return {io.position,e};
    const auto& t=io.timeouts;const bool immediate=t[0]==UINT32_MAX && !t[1] && !t[2];
    const bool first_byte=t[0]==UINT32_MAX && t[1]==UINT32_MAX && t[2] && t[2]<UINT32_MAX;
    const auto total=first_byte?std::uint64_t{t[2]}:std::uint64_t{t[1]}*requested+t[2];
    if(io.position==requested || immediate || (first_byte && io.position) || (total && elapsed>=total) ||
       (io.position && t[0] && t[0]!=UINT32_MAX && now()-io.last_byte>=t[0]))return {io.position,0};
    return {io.position,0,true};
#endif
}
}
