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
}
int create_process_startup_descriptor(const ProcessStartupData& data,int minimum_descriptor){
#if !defined(_WIN32)
    if(data.reserved2.size()>65535 || (data.pointer_bytes!=4 && data.pointer_bytes!=8) || minimum_descriptor<3){errno=EINVAL;return -1;}
    try {
    auto pattern=(std::filesystem::temp_directory_path()/"x64arm-startup-XXXXXX").string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    const auto descriptor=::mkstemp(name.data());if(descriptor<0)return -1;
    struct Close {int fd;~Close(){::close(fd);}} close{descriptor};
    if(::unlink(name.data()) || ::fchmod(descriptor,0600))return -1;
    std::vector<std::uint8_t> bytes(signature.begin(),signature.end());bytes.push_back(static_cast<std::uint8_t>(data.pointer_bytes));
    bytes.push_back(0);bytes.push_back(static_cast<std::uint8_t>(data.reserved2.size()));bytes.push_back(static_cast<std::uint8_t>(data.reserved2.size()>>8));
    bytes.insert(bytes.end(),data.reserved2.begin(),data.reserved2.end());
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
    if(descriptor<3 || ::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink || info.st_size<12 || info.st_size>65547)
        throw std::invalid_argument("程序啟動保留資料的私有描述元無效");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));std::size_t offset=0;
    while(offset<bytes.size()){const auto count=::pread(descriptor,bytes.data()+offset,bytes.size()-offset,static_cast<off_t>(offset));if(count<0 && errno==EINTR)continue;if(count<=0)throw std::invalid_argument("程序啟動保留資料讀取不完整");offset+=static_cast<std::size_t>(count);}
    if(!std::equal(signature.begin(),signature.end(),bytes.begin()) || (bytes[8]!=4 && bytes[8]!=8) || bytes[9] || (std::size_t(bytes[10])+(std::size_t(bytes[11])<<8))!=bytes.size()-12)
        throw std::invalid_argument("程序啟動保留資料格式無效");
    return {std::vector<std::uint8_t>(bytes.begin()+12,bytes.end()),bytes[8]};
#else
    (void)descriptor;throw std::invalid_argument("此平台沒有程序啟動私有描述元");
#endif
}
}
