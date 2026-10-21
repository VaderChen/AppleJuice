#include "x64arm/windows_services.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/unicode.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>

namespace x64arm {
struct DataProtectionState{
    std::array<std::uint8_t,32> process_key{};
    DataProtectionState(){if(RAND_priv_bytes(process_key.data(),static_cast<int>(process_key.size()))!=1)throw DataProtectionError(8);}
    ~DataProtectionState(){OPENSSL_cleanse(process_key.data(),process_key.size());}
};
std::span<const WindowsServiceDescriptor> data_protection_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"crypt32.dll","CryptProtectMemory",3},{"crypt32.dll","CryptUnprotectMemory",3},
        {"advapi32.dll","SystemFunction040",3},{"advapi32.dll","SystemFunction041",3},
        {"cryptbase.dll","SystemFunction040",3},{"cryptbase.dll","SystemFunction041",3},
        {"ntdll.dll","RtlEncryptMemory",3},{"ntdll.dll","RtlDecryptMemory",3}
    };return table;
}
namespace {
struct Secret{
    std::array<std::uint8_t,32> bytes{};
    ~Secret(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
using Cipher=std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)>;
void require(bool value,std::uint32_t error=13){if(!value)throw DataProtectionError(error);}
std::vector<std::uint8_t> derive(std::span<const std::uint8_t> key,std::span<const std::uint8_t> salt,std::span<const std::uint8_t> entropy){
    CryptoHash hash("SHA256",true,key);hash.update(salt);
    std::array<std::uint8_t,4> size{};for(unsigned i=0;i<4;++i)size[i]=static_cast<std::uint8_t>(entropy.size()>>(8*i));hash.update(size);hash.update(entropy);return hash.digest();
}
std::vector<std::uint8_t> gcm(std::span<const std::uint8_t> data,std::span<const std::uint8_t> key,std::span<const std::uint8_t> nonce,std::span<const std::uint8_t> header,std::span<std::uint8_t> tag,bool encrypt){
    Cipher cipher(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);require(bool(cipher),8);
    require(EVP_CipherInit_ex(cipher.get(),EVP_aes_256_gcm(),nullptr,key.data(),nonce.data(),encrypt?1:0)==1);
    int written=0;require(EVP_CipherUpdate(cipher.get(),nullptr,&written,header.data(),static_cast<int>(header.size()))==1);
    std::vector<std::uint8_t> output(data.size()+16);
    require(EVP_CipherUpdate(cipher.get(),output.data(),&written,data.data(),static_cast<int>(data.size()))==1);int tail=0;
    if(!encrypt)require(EVP_CIPHER_CTX_ctrl(cipher.get(),EVP_CTRL_GCM_SET_TAG,static_cast<int>(tag.size()),tag.data())==1);
    require(EVP_CipherFinal_ex(cipher.get(),output.data()+written,&tail)==1);
    if(encrypt)require(EVP_CIPHER_CTX_ctrl(cipher.get(),EVP_CTRL_GCM_GET_TAG,static_cast<int>(tag.size()),tag.data())==1);
    output.resize(static_cast<std::size_t>(written+tail));return output;
}
constexpr std::array<std::uint8_t,8> magic{'X','6','4','D','P','A','P','1'};
std::uint32_t dword(std::span<const std::uint8_t> bytes,std::size_t at){
    std::uint32_t result=0;for(unsigned i=0;i<4;++i)result|=std::uint32_t{bytes[at+i]}<<(8*i);return result;
}
void put(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
}
std::optional<std::uint32_t> WindowsServices::invoke_data_protection(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const bool data=name.ends_with("Data"),encrypt=name=="CryptProtectData" || name=="CryptProtectMemory" || name=="SystemFunction040" || name=="RtlEncryptMemory";
    const bool native=service.family!="crypt32.dll";const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    const auto finish=[&](std::uint32_t error){
        if(native)arg.return_to(state,!error?0:error==87?0xc000000dU:error==8?0xc0000017U:error==50?0xc00000bbU:error==5?0xc0000022U:error==998?0xc0000005U:0xc000003eU);
        else{if(error)last_error_=error;arg.return_to(state,error?0:1);}return std::optional<std::uint32_t>{};
    };
    std::vector<std::uint64_t> allocations;
    const auto allocate=[&](std::span<const std::uint8_t> bytes){
        if(gui_.global_memory.size()>=65536)throw DataProtectionError(8);
        const auto size=std::max<std::size_t>(1,bytes.size());const auto buffer=allocate_heap(memory,size);if(!buffer)throw DataProtectionError(8);
        allocations.push_back(buffer);gui_.global_memory.emplace(buffer,GuiGlobalMemory{buffer,size,0,0,false});
        memory.write(GuestAddress{buffer},bytes);return buffer;
    };
    const auto cleanup=[&]{for(const auto p:allocations){gui_.global_memory.erase(p);(void)free_heap(memory,p,state);}};
    try{
        initialize_tokens();
        const auto flags=static_cast<std::uint32_t>(arg(data?5:2));
        const auto read_blob=[&](std::uint64_t pointer){
            require(pointer!=0,87);memory.check(GuestAddress{pointer},unit*2,Access::Read);
            const auto count=memory.load(GuestAddress{pointer},32),buffer=memory.load(GuestAddress{pointer+unit},bits);
            require(count<=output_limit,8);require(!count || buffer,87);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));memory.read(GuestAddress{buffer},bytes);return bytes;
        };
        if(!data){
            const auto count=static_cast<std::uint32_t>(arg(1));require(arg(0) && count && !(count%16) && flags<=2,87);require(count<=output_limit,8);
            memory.check(GuestAddress{arg(0)},count,Access::Write);std::vector<std::uint8_t> input(count);memory.read(GuestAddress{arg(0)},input);
            if(!runtime_.network.external_io)return finish(50);
            Secret key;
            if(!flags){if(!data_protection_)data_protection_=std::make_shared<DataProtectionState>();key.bytes=data_protection_->process_key;}
            else{
                const auto identity=flags==1?"machine-memory":"logon-memory:"+token_user_sid_string(effective_token_id())+":"+std::to_string(runtime_.token_objects.at(effective_token_id()).authentication_id);
                Secret master;master.bytes=sandbox_data_protection_key(private_key_context(),identity,flags==1,encrypt);
                const auto boot=native_data_protection_boot();const auto derived=derive(master.bytes,std::span(reinterpret_cast<const std::uint8_t*>(boot.data()),boot.size()),{});std::copy_n(derived.begin(),32,key.bytes.begin());
            }
            Cipher cipher(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);require(bool(cipher),8);const std::array<std::uint8_t,16> iv{};
            require(EVP_CipherInit_ex(cipher.get(),EVP_aes_256_cbc(),nullptr,key.bytes.data(),iv.data(),encrypt?1:0)==1);
            require(EVP_CIPHER_CTX_set_padding(cipher.get(),0)==1);std::vector<std::uint8_t> output(count+16);int length=0,tail=0;
            require(EVP_CipherUpdate(cipher.get(),output.data(),&length,input.data(),static_cast<int>(count))==1 && EVP_CipherFinal_ex(cipher.get(),output.data()+length,&tail)==1);
            require(static_cast<std::uint64_t>(length+tail)==count);memory.write(GuestAddress{arg(0)},std::span(output).first(count));return finish(0);
        }
        require(!arg(3) && arg(6),87);require(!(flags&~(encrypt?0x15U:0x41U)),87);
        memory.check(GuestAddress{arg(6)},unit*2,Access::Write);
        if(!encrypt && arg(1))memory.check(GuestAddress{arg(1)},unit,Access::Write);
        if(arg(4)){
            memory.check(GuestAddress{arg(4)},unit*2+8,Access::Read);require(memory.load(GuestAddress{arg(4)},32)==unit*2+8,87);
            if(memory.load(GuestAddress{arg(4)+4},32))return finish(flags&1U?1325U:50U);
        }
        const auto input=read_blob(arg(0)),entropy=arg(2)?read_blob(arg(2)):std::vector<std::uint8_t>{};
        if(!runtime_.network.external_io)return finish(50);
        std::vector<std::uint8_t> output,description;Secret master,key;bool machine=false;
        if(encrypt){
            description=utf16_bytes(arg(1)?read_utf16(memory,GuestAddress{arg(1)},32767):std::u16string{});
            if((flags&16U) && description.size()>2)return finish(50); // 尚未提供宿主稽核事件紀錄。
            require(input.size()+description.size()+64<=output_limit,8);machine=(flags&4U)!=0;
            master.bytes=sandbox_data_protection_key(private_key_context(),machine?"machine-data":"user-data:"+token_user_sid_string(effective_token_id()),machine,true);
            output.resize(48+description.size());std::copy(magic.begin(),magic.end(),output.begin());put(output,8,machine?4U:0U);put(output,12,static_cast<std::uint32_t>(input.size()));put(output,16,static_cast<std::uint32_t>(description.size()));
            require(RAND_bytes(output.data()+20,28)==1,8);std::copy(description.begin(),description.end(),output.begin()+48);
            const auto derived=derive(master.bytes,std::span(output).subspan(20,16),entropy);std::copy_n(derived.begin(),32,key.bytes.begin());
            std::array<std::uint8_t,16> tag{};const auto ciphertext=gcm(input,key.bytes,std::span(output).subspan(36,12),output,tag,true);output.insert(output.end(),ciphertext.begin(),ciphertext.end());output.insert(output.end(),tag.begin(),tag.end());
        }else{
            require(input.size()>=66 && std::equal(magic.begin(),magic.end(),input.begin()));
            const auto stored_flags=dword(input,8),count=dword(input,12),desc=dword(input,16);
            require((stored_flags==0 || stored_flags==4) && desc>=2 && !(desc%2) && desc<=65536 && std::uint64_t{48}+desc+count+16==input.size());
            machine=stored_flags==4;description.assign(input.begin()+48,input.begin()+48+desc);require(description[desc-1]==0 && description[desc-2]==0);
            master.bytes=sandbox_data_protection_key(private_key_context(),machine?"machine-data":"user-data:"+token_user_sid_string(effective_token_id()),machine,false);
            const auto derived=derive(master.bytes,std::span(input).subspan(20,16),entropy);std::copy_n(derived.begin(),32,key.bytes.begin());
            std::array<std::uint8_t,16> tag{};std::copy_n(input.end()-16,16,tag.begin());output=gcm(std::span(input).subspan(48+desc,count),key.bytes,std::span(input).subspan(36,12),std::span(input).first(48+desc),tag,false);
        }
        const auto buffer=allocate(output),text=!encrypt && arg(1)?allocate(description):0;
        memory.store(GuestAddress{arg(6)},32,output.size());memory.store(GuestAddress{arg(6)+unit},bits,buffer);
        if(text)memory.store(GuestAddress{arg(1)},bits,text);allocations.clear();return finish(0);
    }catch(const MemoryFault&){cleanup();return finish(998);}
    catch(const DataProtectionError& e){cleanup();return finish(e.code);}
    catch(const std::bad_alloc&){cleanup();return finish(8);}
}
}
