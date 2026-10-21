#include "x64arm/data_protection.hpp"
#include "x64arm/crypto.hpp"
#include "x64arm/private_key_store.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <openssl/rand.h>
#include <cerrno>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif
namespace x64arm {
#if !defined(_WIN32)
namespace {
struct Descriptor{
    int value{-1};explicit Descriptor(int fd):value(fd){}~Descriptor(){if(value>=0)::close(value);}
    Descriptor(const Descriptor&)=delete;
};
}
#endif
std::array<std::uint8_t,32> sandbox_data_protection_key(const PrivateKeyStoreContext& store,std::string_view identity,bool machine,bool create){
    struct Secret{std::vector<std::uint8_t> bytes;~Secret(){OPENSSL_cleanse(bytes.data(),bytes.size());}} saved;
    const auto name=std::string(identity);auto status=private_key_store_read(store,0x10001,name,machine,saved.bytes);
    if(status==0x80090016U && create){
        saved.bytes.resize(32);if(RAND_priv_bytes(saved.bytes.data(),32)!=1)throw DataProtectionError(8);
        status=private_key_store_write(store,0x10001,name,machine,saved.bytes,true,PrivateKeyAccess::LogonUser);
        if(status==0x8009000fU){OPENSSL_cleanse(saved.bytes.data(),saved.bytes.size());saved.bytes.clear();status=private_key_store_read(store,0x10001,name,machine,saved.bytes);}
    }
    if(status)throw DataProtectionError(status==0x80090016U?2U:status);
    if(saved.bytes.size()!=32)throw DataProtectionError(13);std::array<std::uint8_t,32> result{};std::copy_n(saved.bytes.begin(),32,result.begin());return result;
}
std::string native_data_protection_boot(){
#if defined(__APPLE__)
    std::array<char,128> value{};std::size_t size=value.size();if(::sysctlbyname("kern.bootsessionuuid",value.data(),&size,nullptr,0) || !size || size>value.size())throw DataProtectionError(50);
    return std::string(value.data(),size-(value[size-1]==0?1U:0U));
#elif !defined(_WIN32)
    Descriptor fd(::open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_NOFOLLOW|O_CLOEXEC));if(fd.value<0)throw DataProtectionError(50);
    std::array<char,128> value{};const auto n=::read(fd.value,value.data(),value.size());if(n<=0 || n==static_cast<ssize_t>(value.size()))throw DataProtectionError(50);return std::string(value.data(),static_cast<std::size_t>(n));
#else
    throw DataProtectionError(50);
#endif
}
}
