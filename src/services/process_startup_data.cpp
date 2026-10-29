#include "x64arm/process_startup_data.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace x64arm {
namespace {
constexpr std::array<std::uint8_t,8> signature{'M','W','S','T','A','R','T','1'};
constexpr std::size_t startup_limit=1024*1024;
void append32(std::vector<std::uint8_t>& bytes,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
bool valid_native(const NativeProcessParameters& value){
    if(value.flags&~1U)return false;
    for(unsigned i=0;i<value.strings.size();++i)if(value.strings[i].size()>65533 || (i!=7 && value.strings[i].size()%2))return false;
    return true;
}
}
int create_process_startup_descriptor(const ProcessStartupData& data,int minimum_descriptor){
#if !defined(_WIN32)
    if(data.reserved2.size()>65535 || (data.pointer_bytes!=4 && data.pointer_bytes!=8) || minimum_descriptor<3 || (data.native && !valid_native(*data.native))){errno=EINVAL;return -1;}
    try {
    auto pattern=(std::filesystem::temp_directory_path()/"x64arm-startup-XXXXXX").string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    const auto descriptor=::mkstemp(name.data());if(descriptor<0)return -1;
    struct Close {int fd;~Close(){::close(fd);}} close{descriptor};
    if(::unlink(name.data()) || ::fchmod(descriptor,0600))return -1;
    std::vector<std::uint8_t> bytes(signature.begin(),signature.end());bytes.push_back(static_cast<std::uint8_t>(data.pointer_bytes));
    bytes.push_back(data.native?1:0);bytes.push_back(static_cast<std::uint8_t>(data.reserved2.size()));bytes.push_back(static_cast<std::uint8_t>(data.reserved2.size()>>8));
    bytes.insert(bytes.end(),data.reserved2.begin(),data.reserved2.end());
    if(data.native){
        const auto& native=*data.native;append32(bytes,native.flags);append32(bytes,native.console_flags);
        for(const auto value:native.standard_handles){append32(bytes,static_cast<std::uint32_t>(value));append32(bytes,static_cast<std::uint32_t>(value>>32));}
        for(const auto value:native.window)append32(bytes,value);
        for(const auto& string:native.strings){append32(bytes,static_cast<std::uint32_t>(string.size()));bytes.insert(bytes.end(),string.begin(),string.end());}
    }
    std::size_t offset=0;while(offset<bytes.size()){const auto count=::write(descriptor,bytes.data()+offset,bytes.size()-offset);if(count<0 && errno==EINTR)continue;if(count<=0)return -1;offset+=static_cast<std::size_t>(count);}
    return ::fcntl(descriptor,F_DUPFD_CLOEXEC,minimum_descriptor);
    }catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
    catch(const std::filesystem::filesystem_error& error){errno=error.code().value();return -1;}
#else
    (void)data;(void)minimum_descriptor;return -1;
#endif
}
ProcessStartupData read_process_startup_descriptor(int descriptor){
#if !defined(_WIN32)
    struct stat info{};
    if(descriptor<3 || ::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink || info.st_size<12 || info.st_size>static_cast<off_t>(startup_limit))
        throw std::invalid_argument("程序啟動保留資料的私有描述元無效");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));std::size_t offset=0;
    while(offset<bytes.size()){const auto count=::pread(descriptor,bytes.data()+offset,bytes.size()-offset,static_cast<off_t>(offset));if(count<0 && errno==EINTR)continue;if(count<=0)throw std::invalid_argument("程序啟動保留資料讀取不完整");offset+=static_cast<std::size_t>(count);}
    const auto reserved=std::size_t(bytes[10])+(std::size_t(bytes[11])<<8);
    if(!std::equal(signature.begin(),signature.end(),bytes.begin()) || (bytes[8]!=4 && bytes[8]!=8) || bytes[9]>1 || reserved>bytes.size()-12 || (!bytes[9] && reserved!=bytes.size()-12))
        throw std::invalid_argument("程序啟動保留資料格式無效");
    ProcessStartupData result;result.pointer_bytes=bytes[8];result.reserved2.assign(bytes.data()+12,bytes.data()+12+reserved);
    if(bytes[9]){
        std::size_t at=12+reserved;NativeProcessParameters native;
        const auto read32=[&](){if(at>bytes.size() || bytes.size()-at<4)throw std::invalid_argument("Native 程序參數傳輸截斷");std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at++]}<<(i*8);return value;};
        native.flags=read32();native.console_flags=read32();
        for(auto& value:native.standard_handles){const auto low=read32();value=low|(std::uint64_t{read32()}<<32);}
        for(auto& value:native.window)value=read32();
        for(auto& string:native.strings){const auto size=read32();if(size>bytes.size()-at)throw std::invalid_argument("Native 程序參數字串截斷");string.assign(bytes.data()+at,bytes.data()+at+size);at+=size;}
        if(at!=bytes.size() || !valid_native(native))throw std::invalid_argument("Native 程序參數傳輸無效");result.native=std::move(native);
    }
    return result;
#else
    (void)descriptor;throw std::invalid_argument("此平台沒有程序啟動私有描述元");
#endif
}
}
