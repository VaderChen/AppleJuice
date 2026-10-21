#include "x64arm/windows_services.hpp"
#include "cng_asymmetric.hpp"
#include "x64arm/unicode.hpp"
#include "crypto_text.hpp"
#include "crypto_digest.hpp"
#include <algorithm>
#include <random>
namespace x64arm {
namespace {
struct HashInfo {std::string_view name;unsigned digest,block;};
constexpr HashInfo hashes[]{{"MD5",16,64},{"SHA1",20,64},{"SHA256",32,64},{"SHA384",48,128},{"SHA512",64,128}};
const HashInfo* hash_info(std::string_view name) {
    const auto found=std::find_if(std::begin(hashes),std::end(hashes),[&](const auto& entry){return entry.name==name;});
    return found==std::end(hashes)?nullptr:found;
}
constexpr std::uint32_t invalid_parameter=0xc000000dU, invalid_handle=0xc0000008U, not_supported=0xc00000bbU;
// Windows 10 的預定義 RNG algorithm handle；不需開啟 provider，亦不擁有物件。
constexpr std::uint64_t rng_algorithm_handle=0x81U;
constexpr unsigned object_length=256;
}
std::span<const WindowsServiceDescriptor> crypto_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"crypt32.dll","CryptHashCertificate",7},{"crypt32.dll","CryptHashCertificate2",7},
        {"crypt32.dll","CryptBinaryToStringA",5},{"crypt32.dll","CryptBinaryToStringW",5},
        {"crypt32.dll","CryptStringToBinaryA",7},{"crypt32.dll","CryptStringToBinaryW",7},
        {"advapi32.dll","CryptAcquireContextA",5},{"advapi32.dll","CryptAcquireContextW",5},
        {"advapi32.dll","CryptReleaseContext",2},{"advapi32.dll","CryptGenRandom",3},{"advapi32.dll","SystemFunction036",2},
        {"advapi32.dll","CryptCreateHash",5},{"advapi32.dll","CryptHashData",4},
        {"advapi32.dll","CryptGetHashParam",5},{"advapi32.dll","CryptSetHashParam",4},
        {"advapi32.dll","CryptDuplicateHash",4},{"advapi32.dll","CryptDestroyHash",1},
        {"advapi32.dll","CryptImportKey",6},{"advapi32.dll","CryptExportKey",6},
        {"advapi32.dll","CryptGetUserKey",3},{"advapi32.dll","CryptDuplicateKey",4},
        {"advapi32.dll","CryptGetKeyParam",5},{"advapi32.dll","CryptDestroyKey",1},
        {"advapi32.dll","CryptEncrypt",7},{"advapi32.dll","CryptDecrypt",6},
        {"advapi32.dll","CryptGenKey",4},{"advapi32.dll","CryptSetKeyParam",4},
        {"advapi32.dll","CryptEnumProvidersA",6},{"advapi32.dll","CryptEnumProvidersW",6},
        {"advapi32.dll","CryptEnumProviderTypesA",6},{"advapi32.dll","CryptEnumProviderTypesW",6},
        {"advapi32.dll","CryptGetProvParam",5},
        {"advapi32.dll","CryptSetProvParam",4},
        {"advapi32.dll","CryptSignHashA",6},{"advapi32.dll","CryptSignHashW",6},
        {"advapi32.dll","CryptVerifySignatureA",6},{"advapi32.dll","CryptVerifySignatureW",6},
        {"bcrypt.dll","BCryptGenRandom",4},
        {"bcrypt.dll","BCryptOpenAlgorithmProvider",4},{"bcrypt.dll","BCryptCloseAlgorithmProvider",2},
        {"bcrypt.dll","BCryptGetProperty",6},{"bcrypt.dll","BCryptCreateHash",7},
        {"bcrypt.dll","BCryptHashData",4},{"bcrypt.dll","BCryptFinishHash",4},
        {"bcrypt.dll","BCryptDestroyHash",1},{"bcrypt.dll","BCryptDuplicateHash",5},
        {"bcrypt.dll","BCryptSetProperty",5},{"bcrypt.dll","BCryptImportKey",9},
        {"bcrypt.dll","BCryptImportKeyPair",7},{"bcrypt.dll","BCryptExportKey",7},
        {"bcrypt.dll","BCryptGenerateSymmetricKey",7},{"bcrypt.dll","BCryptGenerateKeyPair",4},
        {"bcrypt.dll","BCryptFinalizeKeyPair",2},{"bcrypt.dll","BCryptDestroyKey",1},
        {"bcrypt.dll","BCryptSecretAgreement",4},{"bcrypt.dll","BCryptDeriveKey",7},
        {"bcrypt.dll","BCryptDestroySecret",1},{"bcrypt.dll","BCryptEncrypt",10},
        {"bcrypt.dll","BCryptDecrypt",10},{"bcrypt.dll","BCryptVerifySignature",7},{"bcrypt.dll","BCryptSignHash",8},
        {"ncrypt.dll","NCryptOpenStorageProvider",3},{"ncrypt.dll","NCryptFreeObject",1},
        {"ncrypt.dll","NCryptCreatePersistedKey",6},{"ncrypt.dll","NCryptFinalizeKey",2},
        {"ncrypt.dll","NCryptOpenKey",5},{"ncrypt.dll","NCryptDeleteKey",2},
        {"ncrypt.dll","NCryptImportKey",8},{"ncrypt.dll","NCryptExportKey",8},
        {"ncrypt.dll","NCryptSetProperty",5},{"ncrypt.dll","NCryptGetProperty",6},
        {"ncrypt.dll","NCryptIsKeyHandle",1},{"ncrypt.dll","NCryptIsAlgSupported",3},
        {"ncrypt.dll","NCryptSignHash",8},{"ncrypt.dll","NCryptVerifySignature",7},
        {"ncrypt.dll","NCryptEncrypt",8},{"ncrypt.dll","NCryptDecrypt",8},
        {"imagehlp.dll","ImageAddCertificate",3},{"imagehlp.dll","ImageEnumerateCertificates",5},
        {"imagehlp.dll","ImageRemoveCertificate",2},{"imagehlp.dll","ImageGetCertificateData",4},
        {"imagehlp.dll","ImageGetCertificateHeader",3},
        {"wintrust.dll","WinVerifyTrust",3},{"wintrust.dll","WinVerifyTrustEx",3},
        {"wintrust.dll","WTHelperProvDataFromStateData",1},{"wintrust.dll","WTHelperGetProvSignerFromChain",4,0,0,14},{"wintrust.dll","WTHelperGetProvCertFromChain",2,0,0,2},
        {"wintrust.dll","CryptCATAdminAcquireContext",3,0,0,4},{"wintrust.dll","CryptCATAdminAcquireContext2",5,0,0,16},
        {"wintrust.dll","CryptCATAdminCalcHashFromFileHandle",4,0,0,8},{"wintrust.dll","CryptCATAdminCalcHashFromFileHandle2",5,0,0,16},
        {"wintrust.dll","CryptCATAdminEnumCatalogFromHash",5,0,0,12},{"wintrust.dll","CryptCATCatalogInfoFromContext",3,0,0,4},
        {"wintrust.dll","CryptCATAdminReleaseCatalogContext",3,0,0,4},{"wintrust.dll","CryptCATAdminReleaseContext",2,0,0,2}
    };return table;
}
bool WindowsServices::fill_random(GuestMemory& memory,std::uint64_t address,std::uint64_t size) {
    if (size>output_limit) return false;
    memory.check(GuestAddress{address},static_cast<std::size_t>(size),Access::Write);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!random_bytes(bytes)) return false;
    memory.write(GuestAddress{address},bytes);return true;
}
bool WindowsServices::random_bytes(std::span<std::uint8_t> bytes) {
    const auto words=(bytes.size()+3)/4;auto& pool=*runtime_.entropy;
    if (words>output_limit/4 || runtime_.entropy_index>output_limit/4-words) return false;
    if (runtime_.entropy_index+words>pool.size()) {
        std::random_device source;if (source.entropy()<=0) return false;
        const auto target=std::min<std::size_t>(output_limit/4,std::max(pool.size()+256,runtime_.entropy_index+words));
        std::vector<std::uint32_t> extra(target-pool.size());for (auto& value:extra) value=source();pool.insert(pool.end(),extra.begin(),extra.end());
    }
    for (std::size_t i=0;i<bytes.size();++i) bytes[i]=static_cast<std::uint8_t>(pool[runtime_.entropy_index+i/4]>>((i%4)*8));
    runtime_.entropy_index+=words;return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_crypto(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto wide=[&](unsigned i){return utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)},1024));};
    std::uint32_t status=0;
    const auto buffer=[&](std::uint64_t pointer,std::uint32_t count) {
        if (count>output_limit) throw HostCallFault("單次密碼服務資料超過 16 MiB 上限");
        std::vector<std::uint8_t> bytes(count);memory.read(GuestAddress{pointer},bytes);return bytes;
    };
    if(service.family=="wintrust.dll"){const auto result=trust_call(state,memory,arg,symbol);arg.return_to(state,result);return std::nullopt;}
    if(service.family=="imagehlp.dll")status=image_certificate_call(memory,arg,symbol);
    else if(symbol.starts_with("CryptBinaryToString") || symbol.starts_with("CryptStringToBinary")){
        const bool encoding=symbol.starts_with("CryptBinaryToString"),unicode=symbol.ends_with("W");
        try {
            if(!arg(0) || !arg(4) || u32(1)>output_limit/(!encoding && unicode?2U:1U))last_error_=87;
            else {
                memory.check(GuestAddress{arg(4)},4,Access::Write);
                const auto capacity=memory.load(GuestAddress{arg(4)},32);
                const auto flags=u32(2),format=flags&0xfffffffU;
                std::vector<std::uint8_t> output;std::uint32_t count=0,skip=0,used=format;
                bool valid=true,binary=format==2;
                if(encoding){
                    const auto input=buffer(arg(0),u32(1));
                    if(binary){if(flags&0x1ffffff0U)throw std::invalid_argument("編碼旗標不合法");output=input;count=u32(1);}
                    else {
                        const auto text=crypto_text::encode(input,flags);count=static_cast<std::uint32_t>(text.size());
                        if(unicode){output.reserve(2*(text.size()+1));for(const auto c:text){output.push_back(static_cast<std::uint8_t>(c));output.push_back(0);}output.push_back(0);output.push_back(0);}
                        else {output.assign(text.begin(),text.end());output.push_back(0);}
                    }
                }else {
                    const auto width=unicode?2U:1U;std::u16string text;
                    const auto limit=u32(1)?u32(1):output_limit/width;
                    for(std::uint32_t i=0;i<limit;++i){const auto c=memory.load(GuestAddress{arg(0)+static_cast<std::uint64_t>(i)*width},static_cast<std::uint8_t>(width*8));if(!u32(1) && !c)break;text+=static_cast<char16_t>(c);}
                    if(!u32(1) && text.size()==limit)throw std::invalid_argument("字串超過上限");
                    auto decoded=crypto_text::decode(text,flags);valid=decoded.has_value();
                    if(valid){used=decoded->format;skip=decoded->skip;binary=used==2;
                        if(binary)output=buffer(arg(0),static_cast<std::uint32_t>(text.size()*width));else output=std::move(decoded->bytes);
                        count=static_cast<std::uint32_t>(output.size());
                    }
                }
                if(!valid)last_error_=13;
                else if(output.size()>output_limit)last_error_=8;
                else {
                    const auto required=count+(encoding && !binary?1U:0U);
                    if(!encoding){if(arg(5))memory.check(GuestAddress{arg(5)},4,Access::Write);if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);}
                    if(arg(3) && capacity<required){memory.store(GuestAddress{arg(4)},32,required);last_error_=234;}
                    else {
                        if(arg(3))memory.write(GuestAddress{arg(3)},output);
                        memory.store(GuestAddress{arg(4)},32,arg(3)?count:required);
                        if(!encoding){if(arg(5))memory.store(GuestAddress{arg(5)},32,skip);if(arg(6))memory.store(GuestAddress{arg(6)},32,used);}
                        status=1;
                    }
                }
            }
        }catch(const MemoryFault&){last_error_=998;}
        catch(const std::bad_alloc&){last_error_=8;}
        catch(const std::invalid_argument&){last_error_=encoding?87:13;}
        catch(const std::length_error&){last_error_=8;}
    }
    else if(symbol=="CryptHashCertificate" || symbol=="CryptHashCertificate2"){
        const bool cng=symbol=="CryptHashCertificate2";
        try {
            std::string algorithm;
            if(cng){if(arg(0))algorithm=wide(0);}
            else if(const auto* info=crypto_detail::hash_info(u32(1)?u32(1):0x8004U))algorithm=info->name;
            if(!arg(6) || (!arg(3) && u32(4)) || u32(4)>output_limit || (cng && (!arg(0) || arg(2))))last_error_=87;
            else if(u32(cng?1:2))last_error_=cng?87U:0x80090009U;
            else if(!hash_info(algorithm) && !(algorithm=="MD5-SHA1" && !cng))last_error_=cng?1168U:0x80090008U;
            else {
                memory.check(GuestAddress{arg(6)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(6)},32);
                const auto input=buffer(arg(3),u32(4));const auto digest=crypto_detail::digest(algorithm,input);
                memory.store(GuestAddress{arg(6)},32,digest.size());
                if(arg(5) && capacity<digest.size())last_error_=234;
                else {if(arg(5))memory.write(GuestAddress{arg(5)},digest);status=1;}
            }
        }catch(const MemoryFault&){last_error_=998;}
        catch(const std::bad_alloc&){last_error_=8;}
        catch(const std::invalid_argument&){last_error_=87;}
        catch(const std::runtime_error&){last_error_=0x80090020U;}
    }
    else if(symbol.starts_with("NCrypt"))status=storage_crypto_call(memory,arg,symbol);
    else if (symbol=="CryptAcquireContextA" || symbol=="CryptAcquireContextW" || symbol.starts_with("CryptEnumProvider") || symbol=="CryptGetProvParam" || symbol=="CryptSetProvParam")status=legacy_crypto_provider(memory,arg,symbol);
    else if(symbol=="CryptEncrypt" || symbol=="CryptDecrypt" || symbol=="CryptGenKey" || symbol=="CryptSetKeyParam")status=legacy_crypto_cipher(memory,arg,symbol);
    else if (symbol=="CryptReleaseContext") {
        if(u32(1))last_error_=87;else if(!release_crypto_provider(arg(0)))last_error_=6;else status=1;
    } else if(symbol=="CryptContextAddRef") {
        if(arg(1) || u32(2))last_error_=87;else status=retain_crypto_provider(arg(0));
    } else if (symbol=="CryptGenRandom" || symbol=="SystemFunction036") {
        if (symbol=="CryptGenRandom" && !runtime_.crypt_providers.contains(arg(0))) last_error_=6;
        else if (fill_random(memory,arg(symbol=="CryptGenRandom"?2:0),u32(1))) status=1;else last_error_=8;
    } else if(symbol.starts_with("Crypt"))status=legacy_crypto_call(memory,arg,symbol);
    else if (symbol=="BCryptGenRandom") {
        const auto algorithm=runtime_.crypto_algorithms.find(arg(0));
        const bool rng=arg(0)==rng_algorithm_handle ||
            (algorithm!=runtime_.crypto_algorithms.end() && algorithm->second.name=="RNG");
        if (u32(3)&~3U || ((u32(3)&2) && arg(0))) status=invalid_parameter;
        else if (!(u32(3)&2) && !rng) status=invalid_handle;
        else if (!fill_random(memory,arg(1),u32(2))) status=0xc0000017;
    } else if (symbol=="BCryptOpenAlgorithmProvider") {
        const auto name=wide(1);const auto flags_value=u32(3);
        if (!arg(0) || flags_value&~0x28U) status=invalid_parameter;
        else if (flags_value && !hash_info(name)) status=invalid_parameter;
        else if ((arg(2) && wide(2)!="Microsoft Primitive Provider") || (!hash_info(name) && name!="RNG" && name!="AES" && !cng::supported(name) && name!="DH")) status=0xc0000225U;
        else if (runtime_.crypto_algorithms.size()>=1024) status=0xc0000017U;
        else {
            memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);
            const auto handle=runtime_.next_crypto_handle++;
            runtime_.crypto_algorithms.emplace(handle,CryptoAlgorithm{name,bool(flags_value&8),bool(flags_value&32)});
            memory.store(GuestAddress{arg(0)},x86_?32:64,handle);
        }
    } else if (symbol=="BCryptCloseAlgorithmProvider") {
        if (u32(1)) status=invalid_parameter;
        else if (!runtime_.crypto_algorithms.erase(arg(0))) status=invalid_handle;
    } else if (symbol=="BCryptGetProperty") {
        const auto algorithm=runtime_.crypto_algorithms.find(arg(0));const auto hash=runtime_.crypto_hashes.find(arg(0));
        if (runtime_.crypto_keys.contains(arg(0)) || (algorithm!=runtime_.crypto_algorithms.end() && !hash_info(algorithm->second.name) && algorithm->second.name!="RNG")) {
            status=crypto_key_call(memory,arg,symbol);arg.return_to(state,status);return std::nullopt;
        }
        if (algorithm==runtime_.crypto_algorithms.end() && hash==runtime_.crypto_hashes.end()) status=invalid_handle;
        else if (u32(5) || !arg(4)) status=invalid_parameter;
        else {
            const auto& name=algorithm!=runtime_.crypto_algorithms.end()?algorithm->second.name:hash->second.algorithm.name;
            const auto* info=hash_info(name);const auto property=wide(1);std::vector<std::uint8_t> bytes;
            if (property=="AlgorithmName") bytes=utf16_bytes(utf8_to_utf16(name));
            else {
                const auto value=property=="ObjectLength"?object_length:property=="HashDigestLength" && info?info->digest:property=="HashBlockLength" && info?info->block:0;
                if (!value) status=not_supported;
                else for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));
            }
            if (!status) {
                memory.check(GuestAddress{arg(4)},4,Access::Write);
                if (arg(2) && u32(3)>=bytes.size()) memory.check(GuestAddress{arg(2)},bytes.size(),Access::Write);
                memory.store(GuestAddress{arg(4)},32,bytes.size());
                if (arg(2)) {if (u32(3)<bytes.size()) status=0xc0000023U;else memory.write(GuestAddress{arg(2)},bytes);}
            }
        }
    } else if (symbol=="BCryptCreateHash" || symbol=="BCryptDuplicateHash") {
        const bool duplicate=symbol=="BCryptDuplicateHash";
        const auto algorithm=runtime_.crypto_algorithms.find(arg(0));const auto source=runtime_.crypto_hashes.find(arg(0));
        if (duplicate?source==runtime_.crypto_hashes.end():algorithm==runtime_.crypto_algorithms.end()) status=invalid_handle;
        else if (!arg(1) || u32(duplicate?4:6)&~(duplicate?0U:32U) || (arg(2)?u32(3)<object_length:u32(3)!=0)) status=invalid_parameter;
        else if (!duplicate && !hash_info(algorithm->second.name)) status=not_supported;
        else if (runtime_.crypto_hashes.size()>=4096) status=0xc0000017U;
        else {
            memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);
            if (arg(2)) memory.check(GuestAddress{arg(2)},object_length,Access::Write);
            CryptoHashObject object;
            if (duplicate) object=source->second;
            else {
                object.algorithm=algorithm->second;object.algorithm.reusable|=bool(u32(6)&32);
                if (object.algorithm.hmac) object.key=buffer(arg(4),u32(5));
                object.hash=CryptoHash(object.algorithm.name,object.algorithm.hmac,object.key);
            }
            object.buffer=arg(2);const auto handle=runtime_.next_crypto_handle++;
            runtime_.crypto_hashes.emplace(handle,std::move(object));
            if (arg(2)) {memory.write(GuestAddress{arg(2)},std::array<std::uint8_t,object_length>{});memory.store(GuestAddress{arg(2)},64,handle);}
            memory.store(GuestAddress{arg(1)},x86_?32:64,handle);
        }
    } else if (symbol=="BCryptDestroyHash") {
        if (!runtime_.crypto_hashes.erase(arg(0))) status=invalid_handle;
    } else if (symbol!="BCryptHashData" && symbol!="BCryptFinishHash") {
        status=crypto_key_call(memory,arg,symbol);
    } else {
        const auto found=runtime_.crypto_hashes.find(arg(0));
        if (found==runtime_.crypto_hashes.end()) status=invalid_handle;
        else if (found->second.finished) status=invalid_parameter;
        else if (u32(3)) status=invalid_parameter;
        else if (symbol=="BCryptHashData") found->second.hash.update(buffer(arg(1),u32(2)));
        else if (symbol=="BCryptFinishHash") {
            auto& object=found->second;const auto bytes=object.hash.digest();
            if (!arg(1) || u32(2)!=bytes.size()) status=invalid_parameter;
            else {
                memory.write(GuestAddress{arg(1)},bytes);
                if (object.algorithm.reusable) object.hash=CryptoHash(object.algorithm.name,object.algorithm.hmac,object.key);
                else object.finished=true;
            }
        } else throw HostCallFault("未知的密碼服務");
    }
    arg.return_to(state,status);return std::nullopt;
}
}
