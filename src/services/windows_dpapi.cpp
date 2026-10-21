#include "x64arm/windows_services.hpp"
#include "x64arm/crypto_aead.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/data_protection.hpp"
#include "x64arm/unicode.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/err.h>
namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
struct ProtectionError {std::uint32_t code;};
struct Sensitive {Bytes bytes;~Sensitive(){if(!bytes.empty())OPENSSL_cleanse(bytes.data(),bytes.size());}};
struct Password {std::u16string text;~Password(){if(!text.empty())OPENSSL_cleanse(text.data(),text.size()*2);}};
constexpr std::uint32_t master_type=0x10000,limit=16*1024*1024;
constexpr std::array<std::uint8_t,8> magic{'M','W','D','P','A','P','I',0},master_magic{'M','W','D','P','K','E','Y',0};
void word(Bytes& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));}
std::uint32_t word(std::span<const std::uint8_t> bytes,std::size_t at){if(at>bytes.size() || bytes.size()-at<4)throw ProtectionError{13};std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(bytes[at+i])<<(8*i);return value;}
void master_key(const PrivateKeyStoreContext& store,const std::string& name,bool machine,bool create,bool external,Sensitive& record){
    auto status=private_key_store_read(store,master_type,name,machine,record.bytes);
    if(status==0x80090016U && create){
        if(!external)throw ProtectionError{50};record.bytes.resize(60);std::copy(master_magic.begin(),master_magic.end(),record.bytes.begin());word(record.bytes,8,1);
        if(RAND_priv_bytes(record.bytes.data()+12,48)!=1){ERR_clear_error();throw ProtectionError{0x80090020};}
        status=private_key_store_write(store,master_type,name,machine,record.bytes,true,PrivateKeyAccess::LogonUser);
        if(status==0x8009000fU){OPENSSL_cleanse(record.bytes.data(),record.bytes.size());record.bytes.clear();status=private_key_store_read(store,master_type,name,machine,record.bytes);}
    }
    if(status==0x80090016U)throw ProtectionError{13};if(status)throw ProtectionError{status==0x80090022U?1325U:status};
    if(record.bytes.size()!=60 || !std::equal(master_magic.begin(),master_magic.end(),record.bytes.begin()) || word(record.bytes,8)!=1)throw ProtectionError{13};
}
void derive_key(std::span<const std::uint8_t> secret,std::span<const std::uint8_t> salt,std::span<const std::uint8_t> entropy,std::u16string_view password,Sensitive& output){
    CryptoHash extract("SHA256",true,salt);extract.update(secret);Sensitive prk{extract.digest()};CryptoHash expand("SHA256",true,prk.bytes);
    constexpr std::array<std::uint8_t,26> label{'M','a','c','W','i','n',' ','D','P','A','P','I',' ','A','E','S','-','2','5','6','-','G','C','M',1,0};expand.update(label);
    Bytes size(4);word(size,0,static_cast<std::uint32_t>(entropy.size()));expand.update(size);expand.update(entropy);Sensitive encoded;encoded.bytes.resize((password.size()+1)*2);for(std::size_t i=0;i<password.size();++i){encoded.bytes[2*i]=static_cast<std::uint8_t>(password[i]);encoded.bytes[2*i+1]=static_cast<std::uint8_t>(password[i]>>8);}
    // 空密碼與未要求密碼相同；是否需要提示另由經認證的封包政策決定。
    if(password.empty())encoded.bytes.clear();word(size,0,static_cast<std::uint32_t>(encoded.bytes.size()));expand.update(size);expand.update(encoded.bytes);const std::array<std::uint8_t,1> counter{1};expand.update(counter);output.bytes=expand.digest();
}
std::u16string text(std::span<const std::uint8_t> bytes){if(bytes.size()<2 || (bytes.size()&1) || bytes[bytes.size()-2] || bytes.back())throw ProtectionError{13};std::u16string out;
    for(std::size_t i=0;i+2<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(unsigned(bytes[i+1])<<8));if(!c)throw ProtectionError{13};out.push_back(c);}return out;
}
}
std::span<const WindowsServiceDescriptor> dpapi_service_descriptors(){static constexpr WindowsServiceDescriptor table[]{{"crypt32.dll","CryptProtectData",7},{"crypt32.dll","CryptUnprotectData",7}};return table;}
std::optional<std::uint32_t> WindowsServices::invoke_dpapi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool protect=service.symbol=="CryptProtectData";const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);std::uint32_t result=0;std::uint64_t allocated=0,description=0;std::u16string audit_description;std::array<std::uint8_t,16> audit_key{};std::uint32_t audit_flags=0;bool audit_requested=false;
    try {
        // 新資料與舊封裝共用 sandbox 金鑰儲存；UI 與稽核沿用既有 Windows 服務。
        if(!protect && arg(0)){
            memory.check(GuestAddress{arg(0)},2*unit,Access::Read);
            const auto count=memory.load(GuestAddress{arg(0)},32),data=memory.load(GuestAddress{arg(0)+unit},bits);
            if(count>=8 && data){
                std::array<std::uint8_t,8> signature{};memory.read(GuestAddress{data},signature);
                constexpr std::array<std::uint8_t,8> legacy{'X','6','4','D','P','A','P','1'};
                if(signature==legacy)return invoke_data_protection(state,memory,service);
            }
        }
        initialize_tokens();
        if(!arg(0) || !arg(6) || arg(3))throw ProtectionError{87};const auto flags=static_cast<std::uint32_t>(arg(5));if(flags&~(protect?0x35U:0x51U))throw ProtectionError{87};
        const auto read_blob=[&](std::uint64_t pointer){if(!pointer)throw ProtectionError{87};memory.check(GuestAddress{pointer},2*unit,Access::Read);const auto size=memory.load(GuestAddress{pointer},32),data=memory.load(GuestAddress{pointer+unit},bits);if(size>limit || (size && !data))throw ProtectionError{87};Bytes value(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{data},value);return value;};
        Sensitive input{read_blob(arg(0))},entropy{arg(2)?read_blob(arg(2)):Bytes{}};std::u16string descr=protect && arg(1)?read_utf16(memory,GuestAddress{arg(1)},32767):std::u16string{};
        if(protect){audit_description=descr;audit_flags=flags;audit_requested=(flags&16U) && !descr.empty();}
        std::uint32_t prompt_flags=0;std::uint64_t owner=0;std::u16string prompt;
        if(arg(4)){if(memory.load(GuestAddress{arg(4)},32)!=(x86_?16U:24U))throw ProtectionError{87};memory.check(GuestAddress{arg(4)},x86_?16:24,Access::Read);prompt_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+4},32));if(prompt_flags&~0x1bU)throw ProtectionError{87};owner=memory.load(GuestAddress{arg(4)+8},bits);const auto string=memory.load(GuestAddress{arg(4)+8+unit},bits);if(string)prompt=read_utf16(memory,GuestAddress{string},32767);if(owner && !gui_.windows.contains(owner))throw ProtectionError{1400};}
        memory.check(GuestAddress{arg(6)},2*unit,Access::Write);if(!protect && arg(1))memory.check(GuestAddress{arg(1)},unit,Access::Write);
        memory.store(GuestAddress{arg(6)},32,0);memory.store(GuestAddress{arg(6)+unit},bits,0);if(!protect && arg(1))memory.store(GuestAddress{arg(1)},bits,0);
        std::uint32_t stored_flags=flags&0x34U,stored_prompt=prompt_flags&0x19U;bool machine=protect && (flags&4U);Bytes header;std::size_t header_size=0;std::array<std::uint8_t,16> tag{};
        if(!protect){const auto& bytes=input.bytes;if(bytes.size()<102 || !std::equal(magic.begin(),magic.end(),bytes.begin()) || word(bytes,8)!=1 || word(bytes,12)!=1)throw ProtectionError{13};
            stored_flags=word(bytes,16);stored_prompt=word(bytes,20);const auto descr_size=word(bytes,24),prompt_size=word(bytes,28),data_size=word(bytes,32);header_size=word(bytes,80);
            if(stored_flags&~0x34U || stored_prompt&~0x19U || descr_size>65536 || prompt_size>65536 || header_size!=84ULL+descr_size+prompt_size || header_size>bytes.size()-16 || data_size!=bytes.size()-header_size-16)throw ProtectionError{13};
            descr=text(std::span(bytes).subspan(84,descr_size));const auto encoded_prompt=text(std::span(bytes).subspan(84+descr_size,prompt_size));if(prompt.empty())prompt=encoded_prompt;
            audit_description=descr;audit_flags=flags;audit_requested=(stored_flags&16U) && !descr.empty();std::copy_n(bytes.begin()+64,16,audit_key.begin());
            machine=(stored_flags&4U)!=0;header.assign(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(header_size));std::copy_n(bytes.end()-16,16,tag.begin());
        }
        const bool strong=((protect?prompt_flags:(prompt_flags|stored_prompt))&0x18U)!=0;
        const bool display=strong || (protect?(prompt_flags&2U)!=0:((prompt_flags|stored_prompt)&1U)!=0);
        if(display && (flags&1U))throw ProtectionError{1325};Password password;
        if(display){if(!runtime_.network.external_io)throw ProtectionError{50};if(owner && !native_gui_)enable_native_gui();const auto status=native_data_protection_prompt(owner?native_gui_->graphics_view(owner):nullptr,prompt,protect,strong,password.text);if(status)throw ProtectionError{status};if(((prompt_flags|stored_prompt)&16U) && password.text.empty())throw ProtectionError{1325};}
        const auto token=effective_token_id();if(!machine && runtime_.token_objects.at(token).type==2 && runtime_.token_objects.at(token).level<2)throw ProtectionError{1346};
        // 主金鑰由目前 sandbox 的 C:、虛擬 SID 與 machine 範圍共同隔離。
        const auto name="dpapi:v1:"+(machine?std::string("machine"):token_user_sid_string(token));Sensitive master;
        master_key(private_key_context(),name,machine,protect,runtime_.network.external_io,master);
        std::copy_n(master.bytes.begin()+12,16,audit_key.begin());Sensitive output,key;
        if(protect){const auto descbytes=utf16_bytes(descr),promptbytes=utf16_bytes(prompt);header_size=84+descbytes.size()+promptbytes.size();if(header_size>limit-16 || input.bytes.size()>limit-16-header_size)throw ProtectionError{8};header.resize(header_size);
            std::copy(magic.begin(),magic.end(),header.begin());word(header,8,1);word(header,12,1);word(header,16,stored_flags);word(header,20,stored_prompt);word(header,24,static_cast<std::uint32_t>(descbytes.size()));word(header,28,static_cast<std::uint32_t>(promptbytes.size()));word(header,32,static_cast<std::uint32_t>(input.bytes.size()));word(header,80,static_cast<std::uint32_t>(header_size));
            if(!random_bytes(std::span(header).subspan(36,28)))throw ProtectionError{8};std::copy_n(master.bytes.begin()+12,16,header.begin()+64);std::copy(descbytes.begin(),descbytes.end(),header.begin()+84);std::copy(promptbytes.begin(),promptbytes.end(),header.begin()+84+static_cast<std::ptrdiff_t>(descbytes.size()));
            derive_key(std::span(master.bytes).subspan(28,32),std::span(header).subspan(36,16),entropy.bytes,password.text,key);Bytes cipher;if(!crypto_aes_gcm(true,key.bytes,std::span(header).subspan(52,12),header,input.bytes,cipher,tag))throw ProtectionError{0x80090020};output.bytes=std::move(header);output.bytes.insert(output.bytes.end(),cipher.begin(),cipher.end());output.bytes.insert(output.bytes.end(),tag.begin(),tag.end());
        }else {if(CRYPTO_memcmp(master.bytes.data()+12,header.data()+64,16)!=0)throw ProtectionError{13};derive_key(std::span(master.bytes).subspan(28,32),std::span(header).subspan(36,16),entropy.bytes,(stored_prompt&0x18U)?std::u16string_view(password.text):std::u16string_view{},key);
            if(!crypto_aes_gcm(false,key.bytes,std::span(header).subspan(52,12),header,std::span(input.bytes).subspan(header_size,input.bytes.size()-header_size-16),output.bytes,tag))throw ProtectionError{13};}
        if(gui_.global_memory.size()>65534)throw ProtectionError{8};
        const auto local=[&](std::uint64_t& pointer,std::span<const std::uint8_t> bytes){pointer=allocate_heap(memory,std::max<std::size_t>(1,bytes.size()));if(!pointer)throw ProtectionError{8};memory.write(GuestAddress{pointer},bytes);runtime_.local_allocations.insert(pointer);gui_.global_memory.emplace(pointer,GuiGlobalMemory{pointer,bytes.size(),0,0,false});};
        local(allocated,output.bytes);
        if(!protect && arg(1)){const auto value=utf16_bytes(descr);local(description,value);}
        if((stored_flags&16U) && !descr.empty()){const auto error=data_protection_audit(descr,std::span(master.bytes).subspan(12,16),flags,protect);if(error)throw ProtectionError{error};}
        memory.store(GuestAddress{arg(6)},32,output.bytes.size());memory.store(GuestAddress{arg(6)+unit},bits,allocated);if(description)memory.store(GuestAddress{arg(1)},bits,description);result=1;allocated=description=0;
    }catch(const ProtectionError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=87;}catch(const std::bad_alloc&){last_error_=8;}catch(const std::invalid_argument&){last_error_=87;}catch(const std::runtime_error&){last_error_=0x80090020;}
    if(!result && audit_requested)(void)data_protection_audit(audit_description,audit_key,audit_flags,protect,last_error_);
    for(const auto pointer:{allocated,description})if(pointer){runtime_.local_allocations.erase(pointer);gui_.global_memory.erase(pointer);(void)free_heap(memory,pointer,state);}arg.return_to(state,result);return std::nullopt;
}
}
