#include "x64arm/windows_services.hpp"
#include "cng_algorithms.hpp"
#include "cng_asymmetric.hpp"
#include "x64arm/unicode.hpp"
#include "crypto_text.hpp"
#include "crypto_digest.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "crypto_kdf.hpp"
#include "message_digest.hpp"
#include <array>
#include <span>
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
#include "windows_system_function.inc"
std::span<const WindowsServiceDescriptor> crypto_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"crypt32.dll","CryptHashCertificate",7},{"crypt32.dll","CryptHashCertificate2",7},
        {"crypt32.dll","CryptBinaryToStringA",5},{"crypt32.dll","CryptBinaryToStringW",5},
        {"crypt32.dll","CryptStringToBinaryA",7},{"crypt32.dll","CryptStringToBinaryW",7},
        {"advapi32.dll","CryptAcquireContextA",5},{"advapi32.dll","CryptAcquireContextW",5},
        {"advapi32.dll","CryptReleaseContext",2},{"advapi32.dll","CryptGenRandom",3},{"advapi32.dll","SystemFunction036",2},
        // LSA／NTLM 的 DES、MD4、RC4 輔助函式（windows_system_function.inc）；035 為 CheckSignatureInFile。
        {"advapi32.dll","SystemFunction001",3},{"advapi32.dll","SystemFunction002",3},{"advapi32.dll","SystemFunction003",2},{"advapi32.dll","SystemFunction004",3},{"advapi32.dll","SystemFunction005",3},{"advapi32.dll","SystemFunction006",2},{"advapi32.dll","SystemFunction007",2},{"advapi32.dll","SystemFunction008",3},
        {"advapi32.dll","SystemFunction009",3},{"advapi32.dll","SystemFunction010",3},{"advapi32.dll","SystemFunction011",3},{"advapi32.dll","SystemFunction012",3},{"advapi32.dll","SystemFunction013",3},{"advapi32.dll","SystemFunction014",3},{"advapi32.dll","SystemFunction015",3},{"advapi32.dll","SystemFunction016",3},
        {"advapi32.dll","SystemFunction017",3},{"advapi32.dll","SystemFunction018",3},{"advapi32.dll","SystemFunction019",3},{"advapi32.dll","SystemFunction020",3},{"advapi32.dll","SystemFunction021",3},{"advapi32.dll","SystemFunction022",3},{"advapi32.dll","SystemFunction023",3},{"advapi32.dll","SystemFunction024",3},
        {"advapi32.dll","SystemFunction025",3},{"advapi32.dll","SystemFunction026",3},{"advapi32.dll","SystemFunction027",3},{"advapi32.dll","SystemFunction030",2},{"advapi32.dll","SystemFunction031",2},{"advapi32.dll","SystemFunction032",2},{"advapi32.dll","SystemFunction035",1},
        {"advapi32.dll","CryptCreateHash",5},{"advapi32.dll","CryptHashData",4},{"advapi32.dll","CryptDeriveKey",5},{"advapi32.dll","CryptHashSessionKey",3},
        {"advapi32.dll","CryptGetDefaultProviderA",5},{"advapi32.dll","CryptGetDefaultProviderW",5},{"advapi32.dll","CryptSetProviderA",2},{"advapi32.dll","CryptSetProviderW",2},
        {"advapi32.dll","CryptSetProviderExA",4},{"advapi32.dll","CryptSetProviderExW",4},
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
        {"bcrypt.dll","BCryptDeriveKeyPBKDF2",9,32},
        {"bcrypt.dll","BCryptSetProperty",5},{"bcrypt.dll","BCryptImportKey",9},
        {"bcrypt.dll","BCryptImportKeyPair",7},{"bcrypt.dll","BCryptExportKey",7},
        {"bcrypt.dll","BCryptGenerateSymmetricKey",7},{"bcrypt.dll","BCryptGenerateKeyPair",4},
        {"bcrypt.dll","BCryptFinalizeKeyPair",2},{"bcrypt.dll","BCryptDestroyKey",1},
        {"bcrypt.dll","BCryptSecretAgreement",4},{"bcrypt.dll","BCryptDeriveKey",7},
        {"bcrypt.dll","BCryptDestroySecret",1},{"bcrypt.dll","BCryptEncrypt",10},
        {"bcrypt.dll","BCryptDecrypt",10},{"bcrypt.dll","BCryptVerifySignature",7},{"bcrypt.dll","BCryptSignHash",8},
        {"bcrypt.dll","BCryptHash",7},{"bcrypt.dll","BCryptDuplicateKey",5},{"bcrypt.dll","BCryptEnumAlgorithms",4},{"bcrypt.dll","BCryptFreeBuffer",1},
        // CNG 設定（windows_cng_configuration.cpp）；ncrypt 的同名匯出經服務解析轉到 bcrypt。
        {"bcrypt.dll","BCryptEnumContextFunctions",5},{"bcrypt.dll","BCryptAddContextFunction",5},{"bcrypt.dll","BCryptRemoveContextFunction",4},
        {"bcrypt.dll","BCryptAddContextFunctionProvider",6},{"bcrypt.dll","BCryptRemoveContextFunctionProvider",5},{"bcrypt.dll","BCryptRegisterProvider",3},{"bcrypt.dll","BCryptUnregisterProvider",1},
        {"bcrypt.dll","BCryptGetFipsAlgorithmMode",1},{"bcrypt.dll","BCryptDeriveKeyCapi",5},{"bcrypt.dll","BCryptKeyDerivation",6},
        {"ncrypt.dll","NCryptOpenStorageProvider",3},{"ncrypt.dll","NCryptFreeObject",1},
        {"ncrypt.dll","NCryptCreatePersistedKey",6},{"ncrypt.dll","NCryptFinalizeKey",2},
        {"ncrypt.dll","NCryptOpenKey",5},{"ncrypt.dll","NCryptDeleteKey",2},
        {"ncrypt.dll","NCryptImportKey",8},{"ncrypt.dll","NCryptExportKey",8},
        {"ncrypt.dll","NCryptSetProperty",5},{"ncrypt.dll","NCryptGetProperty",6},
        {"ncrypt.dll","NCryptIsKeyHandle",1},{"ncrypt.dll","NCryptIsAlgSupported",3},{"ncrypt.dll","NCryptEnumAlgorithms",5,0,0,0x12},{"ncrypt.dll","NCryptEnumKeys",5,0,0,0x10},{"ncrypt.dll","NCryptFreeBuffer",1},
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
        {"wintrust.dll","CryptCATAdminReleaseCatalogContext",3,0,0,4},{"wintrust.dll","CryptCATAdminReleaseContext",2,0,0,2},
        {"wintrust.dll","WVTAsn1SpcLinkEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1SpcLinkDecode",7,0,0,0x19},{"wintrust.dll","WVTAsn1SpcPeImageDataEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1SpcPeImageDataDecode",7,0,0,0x19},{"wintrust.dll","WVTAsn1SpcIndirectDataContentEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1SpcIndirectDataContentDecode",7,0,0,0x19},{"wintrust.dll","WVTAsn1SpcSpOpusInfoEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1SpcSpOpusInfoDecode",7,0,0,0x19},{"wintrust.dll","WVTAsn1SpcFinancialCriteriaInfoEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1SpcFinancialCriteriaInfoDecode",7,0,0,0x19},
        {"wintrust.dll","WTHelperGetFileHandle",1},{"wintrust.dll","WTHelperGetFileName",1},{"wintrust.dll","WintrustGetRegPolicyFlags",1},{"wintrust.dll","WintrustSetRegPolicyFlags",1,0,0,0x1},{"wintrust.dll","TrustIsCertificateSelfSigned",3,0,0,0x6},
        {"wintrust.dll","CryptCATOpen",5,0,0,0x1a},{"wintrust.dll","CryptCATClose",1},{"wintrust.dll","CryptCATEnumerateMember",2},{"wintrust.dll","CryptCATGetMemberInfo",2},
        {"wintrust.dll","CryptCATEnumerateAttr",3},{"wintrust.dll","CryptCATGetAttrInfo",3},{"wintrust.dll","CryptCATEnumerateCatAttr",2},{"wintrust.dll","CryptCATGetCatAttrInfo",2},
        {"wintrust.dll","CryptCATPutMemberInfo",7,0,0,0x30},{"wintrust.dll","CryptCATPutAttrInfo",6,0,0,0x18},{"wintrust.dll","CryptCATPutCatAttrInfo",5,0,0,0xc},{"wintrust.dll","CryptCATPersistStore",1},
        {"wintrust.dll","WVTAsn1CatNameValueEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1CatNameValueDecode",7,0,0,0x19},{"wintrust.dll","WVTAsn1CatMemberInfoEncode",5,0,0,0x1},{"wintrust.dll","WVTAsn1CatMemberInfoDecode",7,0,0,0x19},
        {"wintrust.dll","CryptCATAdminAddCatalog",4,0,0,8},{"wintrust.dll","CryptCATAdminRemoveCatalog",3,0,0,4},{"wintrust.dll","CryptCATAdminResolveCatalogPath",4,0,0,8},{"wintrust.dll","IsCatalogFile",2},
        {"crypt32.dll","CryptSIPRetrieveSubjectGuid",3},{"crypt32.dll","CryptSIPRetrieveSubjectGuidForCatalogFile",3},{"crypt32.dll","CryptSIPLoad",3},
        {"crypt32.dll","CryptSIPAddProvider",1},{"crypt32.dll","CryptSIPRemoveProvider",1},{"crypt32.dll","CryptSIPGetSignedDataMsg",5},
        {"crypt32.dll","CryptSIPPutSignedDataMsg",5},{"crypt32.dll","CryptSIPRemoveSignedDataMsg",2},{"crypt32.dll","CryptSIPCreateIndirectData",3},
        {"crypt32.dll","CryptSIPVerifyIndirectData",2},{"wintrust.dll","CryptSIPGetSignedDataMsg",5},{"wintrust.dll","CryptSIPPutSignedDataMsg",5},
        {"wintrust.dll","CryptSIPRemoveSignedDataMsg",2},{"wintrust.dll","CryptSIPCreateIndirectData",3},{"wintrust.dll","CryptSIPVerifyIndirectData",2}
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
    if(symbol.starts_with("CryptSIP"))return invoke_sip(state,memory,service);
    if(symbol.starts_with("SystemFunction0") && symbol!="SystemFunction036"){
        if(symbol=="SystemFunction035"){
            // CheckSignatureInFile：早期 Windows 檢查 CSP DLL 內嵌的簽章資源，現代 Windows 已不要求 CSP 簽章。
            // 這裡確認路徑可讀且為 PE 映像（MZ 標頭與 PE 簽名）才回傳 TRUE，否則 FALSE 並設定錯誤碼。
            std::uint32_t result=0;
            if(!arg(0))last_error_=87;
            else{
                if(!runtime_.network.external_io)throw HostCallFault("compare 不允許 CheckSignatureInFile 檔案 I/O");
                const auto opened=filesystem_.open_reader(read_ansi(memory,arg(0),32767));
                if(opened.error)last_error_=opened.error;
                else if(!opened.file || (opened.metadata.attributes&0x10))last_error_=5;
                else{
                    std::array<std::uint8_t,1024> header{};const auto read=opened.file->read(header,0);
                    const auto word=[&](std::size_t at){return std::uint32_t{header[at]}|(std::uint32_t{header[at+1]}<<8)|(std::uint32_t{header[at+2]}<<16)|(std::uint32_t{header[at+3]}<<24);};
                    const auto offset=read.error || read.bytes<64?0U:word(60);
                    if(read.error)last_error_=read.error;
                    else if(read.bytes<64 || header[0]!='M' || header[1]!='Z' || offset<64 || offset+4>read.bytes || word(offset)!=0x00004550U)last_error_=193;
                    else{result=1;last_error_=0;}
                }
            }
            arg.return_to(state,result);return std::nullopt;
        }
        arg.return_to(state,system_function_call(memory,arg,symbol,x86_));return std::nullopt;
    }
    if(service.family=="wintrust.dll"){
        const auto result=trust_call(state,memory,arg,symbol);
        // 獨立的唯讀診斷不關閉常駐服務，也不改變驗證結果或信任來源。
        static const bool trace=[] {const auto* value=std::getenv("X64ARM_TRACE_WINTRUST");return value && std::string_view(value)=="1";}();
        if(trace && (symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx")){
            try {
                const auto data=arg(2);const unsigned bits=x86_?32:64;
                const auto load=[&](std::uint64_t base,unsigned at,unsigned width=32){return memory.load(GuestAddress{base+at},static_cast<std::uint8_t>(width));};
                const auto size=load(data,0),choice=load(data,x86_?20:32),operation=load(data,x86_?28:48);
                const auto flags=load(data,x86_?40:72),revocation=load(data,x86_?16:28);
                const auto settings=size>=(x86_?52U:88U)?load(data,x86_?48:80,bits):0;
                std::string path;
                // CLOSE 不讀取呼叫端可能已釋放的檔案資訊。
                if(operation!=2 && operation!=4 && (choice==1 || choice==2)){
                    const auto info=load(data,x86_?24:40,bits);
                    const auto pointer=load(info,choice==1?(x86_?4:8):(x86_?16:24),bits);
                    if(pointer)path=utf16_to_utf8(read_utf16(memory,GuestAddress{pointer},32768));
                }
                for(auto& c:path)if(static_cast<unsigned char>(c)<32)c='?';
                std::fprintf(stderr,"[wintrust] pid=%u thread=%llu api=%.*s status=%08x choice=%llu operation=%llu flags=%llx revocation=%llu signature_settings=%u path=%s\n",
                    process_id(),static_cast<unsigned long long>(threads_.current),static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned>(result),
                    static_cast<unsigned long long>(choice),static_cast<unsigned long long>(operation),static_cast<unsigned long long>(flags),static_cast<unsigned long long>(revocation),settings?1U:0U,path.c_str());
            }catch(const MemoryFault&){}catch(const std::exception&){}
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.family=="imagehlp.dll")status=image_certificate_call(memory,arg,symbol);
    else if(symbol.find("ContextFunction")!=std::string_view::npos || symbol=="BCryptRegisterProvider" || symbol=="BCryptUnregisterProvider")status=cng_configuration_call(state,memory,arg,symbol);
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
    else if (symbol=="CryptAcquireContextA" || symbol=="CryptAcquireContextW" || symbol.starts_with("CryptEnumProvider") || symbol=="CryptGetProvParam" || symbol=="CryptSetProvParam" ||
        symbol.starts_with("CryptGetDefaultProvider") || symbol.starts_with("CryptSetProvider"))status=legacy_crypto_provider(memory,arg,symbol);
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
        else if ((arg(2) && wide(2)!="Microsoft Primitive Provider") || (!hash_info(name) && name!="RNG" && name!="AES" && !cng::supported(name) && name!="DH" && !crypto_kdf::supported(name))) status=0xc0000225U;
        else if (!cng_function_enabled(utf8_to_utf16(name))) status=0xc0000225U; // Default 內容已移除此演算法
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
    } else if (symbol=="BCryptDeriveKeyPBKDF2") {
        // RFC 8018 PBKDF2：PRF 為以 BCRYPT_ALG_HANDLE_HMAC_FLAG 開啟的 HMAC 演算法；每個區塊 T_i = U_1 ^ … ^ U_c，
        // U_1 = PRF(密碼, 鹽 ‖ INT(i))、U_j = PRF(密碼, U_{j-1})。以密碼金鑰化一次，之後每次複製雜湊狀態。
        const auto algorithm=runtime_.crypto_algorithms.find(arg(0));const auto iterations=arg(5);const auto length=u32(7);
        if (algorithm==runtime_.crypto_algorithms.end()) status=invalid_handle;
        else if (!algorithm->second.hmac || !hash_info(algorithm->second.name) || !iterations || u32(8) || (length && !arg(6)) || (u32(2) && !arg(1)) || (u32(4) && !arg(3))) status=invalid_parameter;
        else {
            memory.check(GuestAddress{arg(6)},length,Access::Write);
            const auto password=buffer(arg(1),u32(2)),salt=buffer(arg(3),u32(4));
            memory.write(GuestAddress{arg(6)},crypto_kdf::pbkdf2(algorithm->second.name,password,salt,iterations,length));
        }
    } else if (symbol=="BCryptHash") {
        // (hAlgorithm, pbSecret, cbSecret, pbInput, cbInput, pbOutput, cbOutput)：一次完成雜湊或 HMAC（HMAC 演算法以 pbSecret 為金鑰）。
        const auto algorithm=runtime_.crypto_algorithms.find(arg(0));
        if (algorithm==runtime_.crypto_algorithms.end()) status=invalid_handle;
        else if (!hash_info(algorithm->second.name)) status=not_supported;
        else if (!arg(5) || u32(6)!=hash_info(algorithm->second.name)->digest || (!algorithm->second.hmac && u32(2)) || (u32(2) && !arg(1)) || (u32(4) && !arg(3))) status=invalid_parameter;
        else {
            memory.check(GuestAddress{arg(5)},u32(6),Access::Write);
            CryptoHash hash(algorithm->second.name,algorithm->second.hmac,buffer(arg(1),u32(2)));hash.update(buffer(arg(3),u32(4)));memory.write(GuestAddress{arg(5)},hash.digest());
        }
    } else if (symbol=="BCryptEnumAlgorithms") {
        // (dwAlgOperations, pAlgCount, ppAlgList, dwFlags)：BCRYPT_ALGORITHM_IDENTIFIER 陣列與名稱放在同一塊程序堆積，以 BCryptFreeBuffer 釋放。
        using Entry=cng_catalog::Algorithm;const auto& entries=cng_catalog::algorithms;
        const auto operations=u32(0);
        if (operations&~0x7fU || u32(3) || !arg(1) || !arg(2)) status=invalid_parameter;
        else {
            std::vector<const Entry*> selected;for (const auto& entry:entries) if (!operations || (operations&(1U<<(entry.kind-1)))) selected.push_back(&entry);
            const unsigned unit=x86_?4U:8U,entry_size=x86_?12U:16U;std::size_t size=selected.size()*entry_size;for (const auto* entry:selected) size+=(std::char_traits<char16_t>::length(entry->name)+1)*2;
            memory.check(GuestAddress{arg(1)},4,Access::Write);memory.check(GuestAddress{arg(2)},unit,Access::Write);
            const auto block=allocate_heap(memory,size);
            if (!block) status=0xc0000017U;
            else {
                auto text=block+selected.size()*entry_size;
                for (std::size_t i=0;i<selected.size();++i) {
                    const auto at=block+i*entry_size;const auto bytes=utf16_bytes(selected[i]->name);memory.write(GuestAddress{text},bytes);
                    memory.store(GuestAddress{at},x86_?32:64,text);memory.store(GuestAddress{at+unit},32,selected[i]->kind);memory.store(GuestAddress{at+unit+4},32,0);text+=bytes.size();
                }
                memory.store(GuestAddress{arg(1)},32,selected.size());memory.store(GuestAddress{arg(2)},x86_?32:64,block);
            }
        }
    } else if (symbol=="BCryptFreeBuffer") {
        if (arg(0)) (void)free_heap(memory,arg(0),state);
    } else if (symbol=="BCryptGetFipsAlgorithmMode") {
        // HKLM\SYSTEM\CurrentControlSet\Control\Lsa\FipsAlgorithmPolicy 的 Enabled（Vista 起），或 Lsa 的 FIPSAlgorithmPolicy 值；預設停用。
        if (!arg(0)) status=invalid_parameter;
        else {
            bool enabled=false;
            if (registry_file_.empty() || runtime_.network.external_io) {
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                const auto dword=[&](const std::u16string& path,std::u16string_view name)->std::optional<std::uint32_t>{
                    const auto key=runtime_.registry.keys.find(path);if (key==runtime_.registry.keys.end()) return std::nullopt;
                    const auto value=key->second.find(std::u16string(name));if (value==key->second.end() || value->second.type!=4 || value->second.bytes.size()<4) return std::nullopt;
                    const auto& b=value->second.bytes;return std::uint32_t{b[0]}|(std::uint32_t{b[1]}<<8)|(std::uint32_t{b[2]}<<16)|(std::uint32_t{b[3]}<<24);};
                if (!transaction.error) {
                    const std::u16string lsa=std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\lsa";
                    if (const auto value=dword(lsa+u"\\fipsalgorithmpolicy",u"enabled")) enabled=*value!=0;
                    else if (const auto legacy=dword(lsa,u"fipsalgorithmpolicy")) enabled=*legacy!=0;
                }
            }
            memory.store(GuestAddress{arg(0)},8,enabled?1:0);
        }
    } else if (symbol=="BCryptDeriveKeyCapi") {
        // (hHash, hTargetAlg, pbDerivedKey, cbDerivedKey, dwFlags)：與 CryptDeriveKey 相同，完成雜湊後取雜湊值，需要更長時以 0x36／0x5C 延伸；雜湊隨之結束。
        const auto found=runtime_.crypto_hashes.find(arg(0));
        if (found==runtime_.crypto_hashes.end()) status=invalid_handle;
        else if (arg(1) && !runtime_.crypto_algorithms.contains(arg(1))) status=invalid_handle;
        else if (u32(4) || !arg(2) || !u32(3) || found->second.finished || !hash_info(found->second.algorithm.name) || u32(3)>hash_info(found->second.algorithm.name)->digest*2U) status=invalid_parameter;
        else {
            memory.check(GuestAddress{arg(2)},u32(3),Access::Write);auto& object=found->second;
            memory.write(GuestAddress{arg(2)},crypto_kdf::capi_expand(object.algorithm.name,object.hash.digest(),u32(3)));
            if (object.algorithm.reusable) object.hash=CryptoHash(object.algorithm.name,object.algorithm.hmac,object.key);else object.finished=true;
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
