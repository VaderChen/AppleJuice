#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "pe_signature.hpp"
#include "certificate_packet.hpp"
#include "sandbox_certificate_roots.hpp"
#include <algorithm>
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
    const auto file_bytes=[&](std::uint64_t handle,std::uint64_t path){
        std::shared_ptr<HostFile> native;std::shared_ptr<const std::vector<std::uint8_t>> snapshot;
        if(handle && handle!=(x86_?UINT32_MAX:UINT64_MAX)){
            const auto found=files_.find(handle);require(found!=files_.end(),6);require((found->second.access&0x80000001U)!=0,5);require(!found->second.directory->loaded && !(found->second.metadata.attributes&0x10) && !found->second.pipe && !found->second.serial && !found->second.hid,6);native=found->second.native;snapshot=found->second.bytes;
        }else{require(path);if(!runtime_.network.external_io)throw HostCallFault("compare 不允許 WinTrust 檔案 I/O");const auto opened=filesystem_.open_reader(wide(path));require(!opened.error,opened.error);native=opened.file;}
        if(native){if(!runtime_.network.external_io)throw HostCallFault("compare 不允許 WinTrust 原生檔案 I/O");const auto info=native->information();require(!info.error,info.error);require(!(info.metadata.attributes&0x10),5);require(info.metadata.size<=256ULL*1024*1024,8);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.metadata.size));const auto read=native->read(bytes,0);require(!read.error,read.error);require(read.bytes==bytes.size(),38);return bytes;}
        require(bool(snapshot),6);return *snapshot;
    };
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
            if(size>=(x86_?52U:88U) && load(data,x86_?48:80,bits))return 0x800b0003U;
            const auto roots=sandbox_certificate_roots(filesystem_);require(!roots.error,roots.error);
            TrustVerification verified;
            if(choice==1){require(load(info,0)>=(x86_?16U:32U));const auto bytes=file_bytes(load(info,x86_?8:16,bits),load(info,x86_?4:8,bits));verified=verify_authenticode(bytes,roots.pem);}
            else if(choice==2){require(load(info,0)>=(x86_?40U:72U));const auto bytes=file_bytes(load(info,x86_?20:32,bits),load(info,x86_?16:24,bits));const auto count=static_cast<std::uint32_t>(load(info,x86_?28:48));require(count==0 || count==20 || count==32);const auto admin=load(info,x86_?36:64,bits);std::string algorithm=count==32?"SHA256":"SHA1";if(admin){const auto held=runtime_.catalog_admins.find(admin);require(held!=runtime_.catalog_admins.end(),6);algorithm=held->second.algorithm;}const auto hash=catalog_file_hash(bytes,algorithm);
                if(count){std::vector<std::uint8_t> supplied(count);memory.read(GuestAddress{load(info,x86_?24:40,bits)},supplied);if(hash!=supplied)return 0x80096010U;}
                const auto catalog=file_bytes(0,load(info,8,bits));verified=verify_catalog(catalog,hash,roots.pem);
            }else return 0x800b0003U;
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
        throw HostCallFault("未實作 WinTrust API");
    }catch(const Error& error){last_error_=error.code;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x80070000U|error.code:0;}
    catch(const certificate_detail::CertificateError& error){last_error_=error.code;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?error.code:0;}
    catch(const MemoryFault&){last_error_=87;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x80070057U:0;}
    catch(const std::bad_alloc&){last_error_=8;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x8007000eU:0;}
    catch(const std::runtime_error&){ERR_clear_error();last_error_=13;return symbol=="WinVerifyTrust" || symbol=="WinVerifyTrustEx"?0x800b0003U:0;}
}
}
