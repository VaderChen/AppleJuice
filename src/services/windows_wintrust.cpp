#include "ansi_codepage.hpp"
#include "service_callback_task.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "pe_signature.hpp"
#include "catalog_codec.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/windows_paths.hpp"
#include <ctime>
#include "code_signing_roots.hpp"
#include "certificate_packet.hpp"
#include "sandbox_certificate_roots.hpp"
#include <algorithm>
#include <array>
#include <tuple>
#include <charconv>
#include <cstdio>
#include <openssl/err.h>

namespace x64arm {
namespace {
struct Error {std::uint32_t code;};
void require(bool value,std::uint32_t code=87){if(!value)throw Error{code};}
std::string guid_string(std::span<const std::uint8_t> g){
    require(g.size()==16);char text[39];std::snprintf(text,sizeof(text),"{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",g[3],g[2],g[1],g[0],g[5],g[4],g[7],g[6],g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);return text;
}
constexpr std::array<std::uint8_t,16> generic_action{0x6b,0xc5,0xaa,0,0x44,0xcd,0xd0,0x11,0x8c,0xc2,0,0xc0,0x4f,0xc2,0x95,0xee};
constexpr std::array<std::uint8_t,16> driver_action{0xc3,0xe6,0x50,0xf7,0xee,0x38,0xd1,0x11,0x85,0xe5,0,0xc0,0x4f,0xc2,0x95,0xee};
}
std::uint64_t WindowsServices::trust_call(GuestState& state,GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    const unsigned bits=x86_?32:64,unit=x86_?4:8;
    const auto load=[&](std::uint64_t base,unsigned at,unsigned width=32){return memory.load(GuestAddress{base+at},static_cast<std::uint8_t>(width));};
    const auto put=[&](std::uint64_t base,unsigned at,std::uint64_t value,unsigned width=32){memory.store(GuestAddress{base+at},static_cast<std::uint8_t>(width),value);};
    const auto wide=[&](std::uint64_t pointer){require(pointer);return utf16_to_utf8(read_utf16(memory,GuestAddress{pointer},32768));};
    const auto file_bytes=[&](std::uint64_t handle,std::uint64_t path){return trust_file_contents(memory,handle,path);};
    const auto close_state=[&](WinTrustState& held){for(const auto certificate:held.certificates)(void)free_certificate(state,memory,certificate);for(auto it=held.allocations.rbegin();it!=held.allocations.rend();++it)(void)free_heap(memory,*it,state);};
    try {
        if(symbol=="WTHelperProvDataFromStateData"){
            const auto found=runtime_.trust_states.find(arg(0));if(found==runtime_.trust_states.end()){last_error_=6;return 0;}return found->second.provider;
        }
        if(symbol=="WTHelperGetProvSignerFromChain"){
            const auto found=std::find_if(runtime_.trust_states.begin(),runtime_.trust_states.end(),[&](const auto& item){return item.second.provider==arg(0);});
            require(found!=runtime_.trust_states.end(),87);if(arg(1) || arg(2) || arg(3) || !found->second.signer){last_error_=87;return 0;}return found->second.signer;
        }
        if(symbol=="WTHelperGetProvCertFromChain"){
            const auto found=std::find_if(runtime_.trust_states.begin(),runtime_.trust_states.end(),[&](const auto& item){return item.second.signer && item.second.signer==arg(0);});
            require(found!=runtime_.trust_states.end(),87);require(arg(1)<found->second.certificate_count,87);return found->second.certificate_array+arg(1)*(x86_?60:88);
        }
        if(symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"){
            require(arg(1) && arg(2));std::array<std::uint8_t,16> action{};memory.read(GuestAddress{arg(1)},action);if(action!=generic_action && action!=driver_action)return 0x800b0001U;
            const auto data=arg(2);const auto size=load(data,0);require(size>=(x86_?48U:80U));memory.check(GuestAddress{data},x86_?48:80,Access::Read);
            const auto state_offset=x86_?32U:56U,operation_offset=x86_?28U:48U;const auto operation=load(data,operation_offset),previous=load(data,state_offset,bits);memory.check(GuestAddress{data+state_offset},unit,Access::Write);
            if(operation==2 || operation==4){if(previous){const auto old=runtime_.trust_states.find(previous);require(old!=runtime_.trust_states.end(),6);close_state(old->second);runtime_.trust_states.erase(old);}put(data,state_offset,0,bits);return 0;}
            require(operation==0 || operation==1 || operation==3);require(!previous);const auto choice=load(data,x86_?20:32),info=load(data,x86_?24:40,bits);const auto flags=static_cast<std::uint32_t>(load(data,x86_?40:72));require(info);
            AuthenticodeOptions options;options.code_signing_roots=code_signing_detail::roots_pem;options.lifetime_signing=(flags&0x800U)!=0;
            const auto settings=size>=(x86_?52U:88U)?load(data,x86_?48:80,bits):0;
            std::uint32_t signature_flags{};
            if(settings){
                require(load(settings,0)>=(x86_?24U:32U));
                memory.check(GuestAddress{settings+12},8,Access::Write);
                signature_flags=static_cast<std::uint32_t>(load(settings,8));require(!(signature_flags&~3U));
                if(signature_flags&1U)options.signature_index=static_cast<std::uint32_t>(load(settings,4));
                if(const auto policy=load(settings,x86_?20:24,bits)){
                    require(load(policy,0)>=(x86_?12U:16U));const auto kind=load(policy,4),policy_info=load(policy,8,bits);require(policy_info);
                    StrongSignaturePolicy strong;
                    if(kind==2){
                        const auto value=read_ansi(memory,policy_info,256);
                        require(value=="1.3.6.1.4.1.311.72.1.1" || value=="1.3.6.1.4.1.311.72.2.1");
                        for(const auto* algorithm:{"RSA","ECDSA"}){
                            for(const auto* hash:{"SHA256","SHA384","SHA512"})strong.signature_hashes.emplace_back(algorithm,hash);
                            if(value=="1.3.6.1.4.1.311.72.2.1")strong.signature_hashes.emplace_back(algorithm,"SHA1");
                        }
                        strong.key_lengths={{"RSA",2047},{"ECDSA",256}};strong.revocation_flags=3;
                    }else{
                        require(kind==1);strong.revocation_flags=static_cast<std::uint32_t>(load(policy_info,0));require(!(strong.revocation_flags&~3U));
                        const auto pairs=[&](std::uint64_t pointer,const auto& accept){
                            require(pointer);const auto value=wide(pointer);require(!value.empty() && value.size()<=8192);std::size_t start{};
                            while(start<value.size()){
                                const auto end=value.find(';',start),last=end==value.npos?value.size():end;
                                const auto item=std::string_view(value).substr(start,last-start);const auto slash=item.find('/');require(slash!=item.npos && slash && slash+1<item.size());
                                const auto algorithm=item.substr(0,slash);require(algorithm=="RSA" || algorithm=="ECDSA" || algorithm=="DSA");
                                accept(algorithm,item.substr(slash+1));if(end==value.npos)break;start=end+1;require(start<value.size());
                            }
                        };
                        pairs(load(policy_info,unit,bits),[&](auto algorithm,auto hash){
                            require(hash=="MD5" || hash=="SHA1" || hash=="SHA256" || hash=="SHA384" || hash=="SHA512");strong.signature_hashes.emplace_back(algorithm,hash);
                        });
                        pairs(load(policy_info,unit*2,bits),[&](auto algorithm,auto text){
                            std::uint32_t minimum{};const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),minimum);
                            require(error==std::errc{} && end==text.data()+text.size() && minimum);strong.key_lengths.emplace_back(algorithm,minimum);
                        });
                    }
                    options.strong_policy=std::move(strong);
                }
            }
            const auto roots=sandbox_certificate_roots(filesystem_);require(!roots.error,roots.error);
            TrustVerification verified;
            if(choice==1){require(load(info,0)>=(x86_?16U:32U));const auto bytes=file_bytes(load(info,x86_?8:16,bits),load(info,x86_?4:8,bits));verified=verify_authenticode(bytes,roots.pem,options);}
            else if(choice==2){require(load(info,0)>=(x86_?40U:72U));const auto bytes=file_bytes(load(info,x86_?20:32,bits),load(info,x86_?16:24,bits));const auto count=static_cast<std::uint32_t>(load(info,x86_?28:48));require(count==0 || count==20 || count==32);const auto admin=load(info,x86_?36:64,bits);std::string algorithm=count==32?"SHA256":"SHA1";if(admin){const auto held=runtime_.catalog_admins.find(admin);require(held!=runtime_.catalog_admins.end(),6);algorithm=held->second.algorithm;}const auto hash=catalog_file_hash(bytes,algorithm);
                if(count){std::vector<std::uint8_t> supplied(count);memory.read(GuestAddress{load(info,x86_?24:40,bits)},supplied);if(hash!=supplied)return 0x80096010U;}
                const auto catalog=file_bytes(0,load(info,8,bits));verified=verify_catalog(catalog,hash,roots.pem,options);
            }else return 0x800b0003U;
            if(settings){if(signature_flags&2U)put(settings,12,verified.signature_count?verified.signature_count-1:0);put(settings,16,verified.verified_signature_index);}
            // 尚無 CRL／OCSP 查詢時，要求撤銷檢查的呼叫不能被宣告驗證成功。
            if(!verified.status && (load(data,x86_?16:28) || (flags&0xe0U)))verified.status=0x80092013U;
            if(operation==0)return verified.status;
            require(runtime_.trust_states.size()<1024,8);WinTrustState held;
            const auto allocate=[&](std::size_t extent){const auto pointer=allocate_heap(memory,extent);require(pointer!=0,8);held.allocations.push_back(pointer);memory.write(GuestAddress{pointer},std::vector<std::uint8_t>(extent));return pointer;};
            const auto raw=[&](std::span<const std::uint8_t> bytes){if(bytes.empty())return UINT64_C(0);const auto pointer=allocate(bytes.size());memory.write(GuestAddress{pointer},bytes);return pointer;};
            const auto string=[&](std::string value){value.push_back(0);return raw({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});};
            const auto blob=[&](std::uint64_t base,unsigned offset,std::span<const std::uint8_t> bytes){put(base,offset,bytes.size());put(base,offset+unit,raw(bytes),bits);};
            try {
                held.provider=allocate(x86_?128:224);put(held.provider,0,x86_?128:224);put(held.provider,unit,data,bits);put(held.provider,x86_?24:48,verified.status);put(held.provider,x86_?56:104,0x10001);put(held.provider,x86_?108:200,flags);put(held.provider,x86_?112:204,verified.status);
                if(!verified.certificates.empty()){
                    held.signer=allocate(x86_?44:64);put(held.signer,0,x86_?44:64);put(held.signer,4,verified.verify_time,64);put(held.signer,12,verified.certificates.size());put(held.signer,x86_?28:40,verified.status);held.certificate_count=static_cast<std::uint32_t>(verified.certificates.size());held.certificate_array=allocate(held.certificate_count*(x86_?60:88));put(held.signer,16,held.certificate_array,bits);
                    put(held.provider,x86_?64:120,1);put(held.provider,x86_?68:128,held.signer,bits);
                    for(std::size_t i=0;i<verified.certificates.size();++i){const auto& certificate=verified.certificates[i];const auto pointer=create_certificate(memory,certificate.der);held.certificates.push_back(pointer);const auto record=held.certificate_array+i*(x86_?60:88);put(record,0,x86_?60:88);put(record,unit,pointer,bits);put(record,x86_?12:20,certificate.trusted_root);put(record,x86_?16:24,certificate.self_signed);put(record,x86_?28:36,verified.status?0x10000000U:0x11000000U);put(record,x86_?32:40,verified.status);}
                    const auto signer=allocate(x86_?68:136);put(signer,0,1);put(held.signer,x86_?24:32,signer,bits);blob(signer,x86_?4:8,verified.issuer);blob(signer,x86_?12:24,verified.serial);blob(signer,x86_?44:88,verified.signature);put(signer,x86_?20:40,string(verified.hash_oid),bits);put(signer,x86_?32:64,string(verified.encryption_oid),bits);
                    const auto attributes=[&](unsigned offset,const auto& list){put(signer,offset,list.size());if(list.empty())return;const auto records=allocate(list.size()*(x86_?12:24));put(signer,offset+unit,records,bits);for(std::size_t i=0;i<list.size();++i){const auto record=records+i*(x86_?12:24);put(record,0,string(list[i].oid),bits);put(record,unit,list[i].values.size());if(list[i].values.empty())continue;const auto values=allocate(list[i].values.size()*unit*2);put(record,unit*2,values,bits);for(std::size_t j=0;j<list[i].values.size();++j)blob(values,static_cast<unsigned>(j*unit*2),list[i].values[j]);}};
                    attributes(x86_?52:104,verified.authenticated_attributes);attributes(x86_?60:120,verified.unauthenticated_attributes);
                }
                const auto handle=runtime_.next_crypto_handle++;runtime_.trust_states.emplace(handle,std::move(held));put(data,state_offset,handle,bits);
            }catch(...){close_state(held);throw;}
            return verified.status;
        }
        if(symbol=="CryptCATAdminAcquireContext" || symbol=="CryptCATAdminAcquireContext2"){
            const bool extended=symbol.ends_with("2");require(arg(0) && !arg(extended?4:2));memory.check(GuestAddress{arg(0)},unit,Access::Write);CatalogAdmin admin;std::array<std::uint8_t,16> guid=driver_action;if(arg(1))memory.read(GuestAddress{arg(1)},guid);admin.subsystem=guid_string(guid);if(extended && arg(2))admin.algorithm=wide(arg(2));require(admin.algorithm=="SHA1" || admin.algorithm=="SHA256",87);if(extended && arg(3))throw Error{50};require(runtime_.catalog_admins.size()<1024,8);const auto handle=runtime_.next_crypto_handle++;runtime_.catalog_admins.emplace(handle,std::move(admin));put(arg(0),0,handle,bits);return 1;
        }
        if(symbol=="CryptCATAdminReleaseContext"){
            require(!arg(1));require(runtime_.catalog_admins.erase(arg(0))!=0,6);std::erase_if(runtime_.catalog_contexts,[&](const auto& entry){return entry.second.admin==arg(0);});return 1;
        }
        if(symbol=="CryptCATAdminReleaseCatalogContext"){
            require(!arg(2));require(runtime_.catalog_admins.contains(arg(0)),6);const auto found=runtime_.catalog_contexts.find(arg(1));require(found!=runtime_.catalog_contexts.end() && found->second.admin==arg(0),6);runtime_.catalog_contexts.erase(found);return 1;
        }
        if(symbol=="CryptCATAdminCalcHashFromFileHandle" || symbol=="CryptCATAdminCalcHashFromFileHandle2"){
            const bool extended=symbol.ends_with("2");const unsigned base=extended?1:0;std::string algorithm="SHA1";if(extended){const auto admin=runtime_.catalog_admins.find(arg(0));require(admin!=runtime_.catalog_admins.end(),6);algorithm=admin->second.algorithm;}require(arg(base+1) && !arg(base+3));const auto capacity=load(arg(base+1),0);memory.check(GuestAddress{arg(base+1)},4,Access::Write);const auto bytes=file_bytes(arg(base),0);const auto hash=catalog_file_hash(bytes,algorithm);put(arg(base+1),0,hash.size());require(arg(base+2) && capacity>=hash.size(),122);memory.write(GuestAddress{arg(base+2)},hash);return 1;
        }
        if(symbol=="CryptCATAdminEnumCatalogFromHash"){
            const auto admin=runtime_.catalog_admins.find(arg(0));require(admin!=runtime_.catalog_admins.end(),6);require(!arg(3) && arg(1) && (arg(2)==20 || arg(2)==32));std::vector<std::uint8_t> hash(static_cast<std::size_t>(arg(2)));memory.read(GuestAddress{arg(1)},hash);std::string previous;
            if(arg(4)){const auto handle=load(arg(4),0,bits);if(handle){const auto context=runtime_.catalog_contexts.find(handle);require(context!=runtime_.catalog_contexts.end() && context->second.admin==arg(0),6);previous=context->second.path;}}
            if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生簽署目錄列舉");const std::string directory="C:\\Windows\\System32\\CatRoot\\"+admin->second.subsystem;const auto listing=filesystem_.list(directory);require(!listing.error || listing.error==2 || listing.error==3,listing.error);std::vector<std::string> paths;
            for(const auto& entry:listing.entries){std::string name=entry.name;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});if(!(entry.metadata.attributes&0x10) && name.ends_with(".cat"))paths.push_back(directory+"\\"+entry.name);}std::sort(paths.begin(),paths.end());require(paths.size()<=8192,8);
            for(const auto& path:paths){if(!previous.empty() && path<=previous)continue;const auto opened=filesystem_.open_reader(path);if(opened.error)continue;require(opened.metadata.size<=64ULL*1024*1024,8);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));const auto read=opened.file->read(bytes,0);if(read.error || read.bytes!=bytes.size() || !catalog_contains(bytes,hash))continue;require(runtime_.catalog_contexts.size()<4096,8);const auto handle=runtime_.next_crypto_handle++;runtime_.catalog_contexts.emplace(handle,CatalogContext{arg(0),path});return handle;}
            last_error_=1168;return 0;
        }
        if(symbol=="CryptCATCatalogInfoFromContext"){
            const auto found=runtime_.catalog_contexts.find(arg(0));require(found!=runtime_.catalog_contexts.end(),6);require(arg(1) && !arg(2) && load(arg(1),0)>=524);const auto name=utf16_bytes(utf8_to_utf16(found->second.path));require(name.size()<=520,206);memory.check(GuestAddress{arg(1)+4},520,Access::Write);memory.write(GuestAddress{arg(1)+4},std::vector<std::uint8_t>(520));memory.write(GuestAddress{arg(1)+4},name);return 1;
        }
        // 目錄資料庫位於 C:\Windows\System32\CatRoot\<子系統 GUID>（與 CryptCATAdminEnumCatalogFromHash 相同）。
        const auto catalog_directory=[&](const CatalogAdmin& admin){return std::string("C:\\Windows\\System32\\CatRoot\\")+admin.subsystem;};
        const auto catalog_name=[&](std::string name){const auto slash=name.find_last_of("\\/");if(slash!=std::string::npos)name=name.substr(slash+1);require(!name.empty() && name.find(':')==std::string::npos,87);return name;};
        if(symbol=="CryptCATAdminAddCatalog"){
            // (hCatAdmin, CatalogFile, SelectBaseName, Flags)：驗證為簽署目錄後複製到目錄資料庫並回傳 HCATINFO；沒有指定名稱時以內容 SHA-1 產生 {GUID 形式}.cat。
            // CRYPTCAT_ADDCATALOG_HARDLINK（1）以複製完成；名稱不可含路徑。
            const auto admin=runtime_.catalog_admins.find(arg(0));require(admin!=runtime_.catalog_admins.end(),6);require(arg(1) && !(arg(3)&~1ULL),87);
            const auto bytes=file_bytes(0,arg(1));require(is_catalog_file(bytes),11);
            std::string name;
            if(arg(2)){name=wide(arg(2));require(!name.empty() && name.find_first_of("\\/:")==std::string::npos,87);}
            else{const auto digest=catalog_file_hash(std::vector<std::uint8_t>(bytes.begin(),bytes.end()),"SHA1");name=guid_string(std::vector<std::uint8_t>(digest.begin(),digest.begin()+16))+".cat";}
            const auto directory=catalog_directory(admin->second);for(const auto* level:{"C:\\Windows","C:\\Windows\\System32","C:\\Windows\\System32\\CatRoot"})(void)filesystem_.create_directory(level);(void)filesystem_.create_directory(directory);
            const auto path=directory+"\\"+name;auto opened=filesystem_.open_host(path,0x40000000U,2,0x80,3);require(!opened.error,opened.error);
            const auto written=opened.file->write(bytes,0);require(!written.error,written.error);if(const auto error=opened.file->truncate(bytes.size()))require(false,error);
            require(runtime_.catalog_contexts.size()<4096,8);const auto handle=runtime_.next_crypto_handle++;runtime_.catalog_contexts.emplace(handle,CatalogContext{arg(0),path});last_error_=0;return handle;
        }
        if(symbol=="CryptCATAdminRemoveCatalog"){
            // (hCatAdmin, CatalogFile, Flags)：自目錄資料庫刪除該名稱（可帶路徑，只取檔名）。
            const auto admin=runtime_.catalog_admins.find(arg(0));require(admin!=runtime_.catalog_admins.end(),6);require(arg(1) && !arg(2),87);
            const auto error=filesystem_.remove(catalog_directory(admin->second)+"\\"+catalog_name(wide(arg(1))),false);require(!error,error);last_error_=0;return 1;
        }
        if(symbol=="CryptCATAdminResolveCatalogPath"){
            // (hCatAdmin, CatalogFile, CATALOG_INFO*, Flags)：回傳目錄資料庫中的完整路徑（不檢查是否存在）。
            const auto admin=runtime_.catalog_admins.find(arg(0));require(admin!=runtime_.catalog_admins.end(),6);require(arg(1) && arg(2) && !arg(3) && load(arg(2),0)>=524,87);
            const auto name=utf16_bytes(utf8_to_utf16(catalog_directory(admin->second)+"\\"+catalog_name(wide(arg(1)))));require(name.size()<=520,206);
            memory.check(GuestAddress{arg(2)+4},520,Access::Write);memory.write(GuestAddress{arg(2)+4},std::vector<std::uint8_t>(520));memory.write(GuestAddress{arg(2)+4},name);last_error_=0;return 1;
        }
        if(symbol=="IsCatalogFile"){
            // (hFile, FileName)：檔案控制代碼優先，否則讀取檔名；不是簽署目錄時回 FALSE。
            const auto handle=arg(0)==(x86_?0xffffffffULL:~0ULL)?0:arg(0);if(!handle && !arg(1))return 0;
            try{last_error_=0;return is_catalog_file(file_bytes(handle,arg(1)))?1:0;}catch(const Error&){return 0;}
        }
#include "windows_wintrust_catalog.inc"
#include "windows_wintrust_spc.inc"
        throw HostCallFault("未實作 WinTrust API");
    }catch(const Error& error){last_error_=error.code;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x80070000U|error.code:0;}
    catch(const certificate_detail::CertificateError& error){last_error_=error.code;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?error.code:0;}
    catch(const MemoryFault&){last_error_=87;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x80070057U:0;}
    catch(const std::bad_alloc&){last_error_=8;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x8007000eU:0;}
    catch(const std::runtime_error&){ERR_clear_error();last_error_=13;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x800b0003U:0;}
}
#include "windows_sip.inc"
}
