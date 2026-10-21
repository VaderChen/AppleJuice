#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "legacy_crypto_catalog.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/file_security.hpp"
#include <cstring>
namespace x64arm {
std::uint32_t WindowsServices::legacy_crypto_provider(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    using Bytes=std::vector<std::uint8_t>;using namespace legacy_crypto;
    const auto fail=[&](std::uint32_t error){last_error_=error;return 0U;};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto text=[](std::string_view value){Bytes bytes(value.begin(),value.end());bytes.push_back(0);return bytes;};
    const auto put=[](Bytes& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto word=[&](std::uint32_t value){Bytes bytes(4);put(bytes,0,value);return bytes;};
    const auto write=[&](const Bytes& bytes,std::uint64_t out,std::uint64_t length){
        if(!length)return fail(87);const auto capacity=memory.load(GuestAddress{length},32);memory.check(GuestAddress{length},4,Access::Write);
        if(out && capacity>=bytes.size())memory.check(GuestAddress{out},bytes.size(),Access::Write);
        memory.store(GuestAddress{length},32,bytes.size());if(!out)return 1U;if(capacity<bytes.size())return fail(234);memory.write(GuestAddress{out},bytes);return 1U;
    };
    try {
    if(symbol=="CryptAcquireContextA" || symbol=="CryptAcquireContextW"){
        if(!arg(0))return fail(87);
        const auto string=[&](unsigned n){return symbol.back()=='W'?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(n)},1024)):read_ansi(memory,arg(n),4096);};
        memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);
        const auto handle=acquire_crypto_provider(arg(1)?string(1):std::string{},arg(2)?string(2):std::string{},u32(3),u32(4));
        if(!handle)return 0;memory.store(GuestAddress{arg(0)},x86_?32:64,handle==UINT64_MAX?0:handle);return 1;
    }
    if(symbol.starts_with("CryptEnumProvider")){
        if(arg(1) || !arg(3) || !arg(5))return fail(87);if(u32(2))return fail(0x80090009);
        if(u32(0)>=std::size(providers))return fail(259);
        const auto& provider=providers[u32(0)];const auto* name=symbol.starts_with("CryptEnumProviderTypes")?provider.description:provider.name;
        const auto bytes=symbol.back()=='W'?utf16_bytes(utf8_to_utf16(name)):text(name);
        memory.check(GuestAddress{arg(3)},4,Access::Write);const auto result=write(bytes,arg(4),arg(5));memory.store(GuestAddress{arg(3)},32,provider.type);return result;
    }
    if(symbol=="CryptSetProvParam" && u32(1)==1){
        if(u32(3))return fail(0x80090009);if(!arg(2))return fail(87);
        const auto window=memory.load(GuestAddress{arg(2)},x86_?32:64);if(window && !gui_.windows.contains(window))return fail(1400);
        if(!arg(0)){runtime_.crypto_client_window=window;return 1;}
        const auto found=runtime_.crypt_providers.find(arg(0));if(found==runtime_.crypt_providers.end())return fail(6);found->second.client_window=window;return 1;
    }
    const auto found=runtime_.crypt_providers.find(arg(0));if(found==runtime_.crypt_providers.end())return fail(6);auto& provider=found->second;
    if((symbol=="CryptSetProvParam" || symbol=="CryptGetProvParam") && u32(1)==8){
        const bool setting=symbol=="CryptSetProvParam";const auto flags=u32(setting?3:4);
        if(!flags || (flags&~0xa000001fU) || ((flags&0xa0000000U)==0xa0000000U))return fail(87);
        if(flags&8U)return fail(1314);if(provider.flags&0xf0000000U)return fail(0x8009000a);
        if(setting && !arg(2))return fail(87);
        refresh_file_security_context();const auto& primary=runtime_.token_objects.at(1);
        const auto access=setting?((flags&0x13U)?0x80000U:0U)|((flags&0xa0000004U)?0x40000U:0U):0x20000U;
        const auto descriptor=setting?std::optional{security::read_descriptor(memory,arg(2),x86_,true)}:std::nullopt;
        if(descriptor && (flags&4U) && !(descriptor->control&4U))return fail(1338);
        const auto status=private_key_store_security(private_key_context(provider.user_scope),provider.type,provider.container,(provider.flags&32U)!=0,access,[&](const HostFile& file){
            if(setting){security::write_native_file_security(file,*descriptor,flags,primary.user.sid,primary.primary_group);return 0U;}
            const auto bytes=security::relative_descriptor(security::file_descriptor(security::read_native_file_security(file,true),flags));
            return write(bytes,arg(2),arg(3))?0U:last_error_;
        });return status?fail(status):1;
    }
    if(symbol=="CryptSetProvParam"){
        if(u32(3))return fail(0x80090009);
        // 本 CSP 為軟體提供者，硬體 RNG／智慧卡 PIN 等參數沒有對應裝置。
        if(u32(1)==38 && arg(2))return fail(87);
        return fail(0x8009000a);
    }
    const auto parameter=u32(1),flags=u32(4);
    if(parameter==1 || parameter==22){
        if(flags!=1 && flags!=2 && flags!=0)return fail(0x80090009);
        std::vector<const Algorithm*> catalog;for(const auto& a:algorithms)if(supported(a,provider.type))catalog.push_back(&a);
        auto& cursor=parameter==1?provider.algorithm_cursor:provider.extended_cursor;const auto index=flags==1?0:cursor;
        if(index>=catalog.size())return fail(259);const auto& a=*catalog[index];Bytes bytes(parameter==1?32:88);
        put(bytes,0,a.id);put(bytes,4,a.default_bits);
        if(parameter==1){put(bytes,8,static_cast<std::uint32_t>(std::strlen(a.name)+1));std::copy_n(a.name,std::strlen(a.name),bytes.begin()+12);}
        else {put(bytes,8,a.min_bits);put(bytes,12,a.max_bits);put(bytes,20,static_cast<std::uint32_t>(std::strlen(a.name)+1));std::copy_n(a.name,std::strlen(a.name),bytes.begin()+24);
            put(bytes,44,static_cast<std::uint32_t>(std::strlen(a.description)+1));std::copy_n(a.description,std::strlen(a.description),bytes.begin()+48);}
        const auto result=write(bytes,arg(2),arg(3));if(result && arg(2))cursor=index+1;else if(flags==1)cursor=0;return result;
    }
    if(parameter==2){
        if(flags!=1 && flags!=2 && flags!=0)return fail(0x80090009);
        if(flags==1){const auto status=private_key_store_list(private_key_context(provider.user_scope),provider.type,(provider.flags&32U)!=0,provider.containers);if(status)return fail(status);provider.container_cursor=0;}
        if(provider.containers.empty() && !arg(2) && flags==1)return write(Bytes(1),0,arg(3));
        if(provider.container_cursor>=provider.containers.size())return fail(259);const auto result=write(text(provider.containers[provider.container_cursor]),arg(2),arg(3));if(result && arg(2))++provider.container_cursor;return result;
    }
    if(flags)return fail(0x80090009);
    switch(parameter){
    case 3:return write(word(2),arg(2),arg(3)); // 實際的軟體後端。
    case 4:return write(text(provider.name),arg(2),arg(3));
    case 5:return write(word(0x200),arg(2),arg(3));
    case 6:case 36:return write(text(provider.container),arg(2),arg(3));
    case 16:return write(word(provider.type),arg(2),arg(3));
    case 19:case 20:return write(word(provider.session_bits),arg(2),arg(3));
    case 27:return write(word(provider.flags&32U),arg(2),arg(3));
    case 34:case 35:return write(word(16),arg(2),arg(3));
    case 39:return write(word(3),arg(2),arg(3));
    default:return fail(0x8009000a);
    }
    }catch(const SecurityDescriptorError& error){return fail(error.code);}
    catch(const std::bad_alloc&){return fail(8);}
}
}
