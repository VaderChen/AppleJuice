#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/file_security.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <set>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "windows_sddl.hpp"
#include "process_manifest.hpp"

namespace x64arm {
std::span<const WindowsServiceDescriptor> anonymous_token_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"advapi32.dll","ImpersonateAnonymousToken",1}};return table;
}
std::span<const WindowsServiceDescriptor> security_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","AllocateLocallyUniqueId",1},
        {"advapi32.dll","BuildSecurityDescriptorA",9},{"advapi32.dll","BuildSecurityDescriptorW",9},
        {"advapi32.dll","GetNamedSecurityInfoA",8},{"advapi32.dll","GetNamedSecurityInfoW",8},
        {"advapi32.dll","SetNamedSecurityInfoA",7},{"advapi32.dll","SetNamedSecurityInfoW",7},
        {"advapi32.dll","SetEntriesInAclA",4},{"advapi32.dll","SetEntriesInAclW",4},{"advapi32.dll","GetExplicitEntriesFromAclA",3},{"advapi32.dll","GetExplicitEntriesFromAclW",3},{"advapi32.dll","BuildExplicitAccessWithNameA",5},{"advapi32.dll","BuildExplicitAccessWithNameW",5},{"advapi32.dll","BuildTrusteeWithNameA",2},{"advapi32.dll","BuildTrusteeWithNameW",2},{"advapi32.dll","BuildTrusteeWithSidA",2},{"advapi32.dll","BuildTrusteeWithSidW",2},{"advapi32.dll","GetTrusteeNameA",1},{"advapi32.dll","GetTrusteeNameW",1},{"advapi32.dll","GetTrusteeTypeA",1},{"advapi32.dll","GetTrusteeTypeW",1},{"advapi32.dll","GetTrusteeFormA",1},{"advapi32.dll","GetTrusteeFormW",1},
        {"advapi32.dll","IsValidAcl",1},{"advapi32.dll","GetAclInformation",4},{"advapi32.dll","SetAclInformation",4},
        {"advapi32.dll","GetAce",3},{"advapi32.dll","DeleteAce",2},{"advapi32.dll","AddAce",5},{"advapi32.dll","FindFirstFreeAce",2},
        {"advapi32.dll","AddAccessAllowedAce",4},{"advapi32.dll","AddAccessAllowedAceEx",5},
        {"advapi32.dll","AddAccessDeniedAce",4},{"advapi32.dll","AddAccessDeniedAceEx",5},
        {"advapi32.dll","AddAuditAccessAce",6},{"advapi32.dll","AddAuditAccessAceEx",7},{"advapi32.dll","AddMandatoryAce",5},
        {"advapi32.dll","IsValidSecurityDescriptor",1},{"advapi32.dll","GetSecurityDescriptorLength",1},{"advapi32.dll","GetSecurityDescriptorControl",3},
        {"advapi32.dll","GetSecurityDescriptorOwner",3},{"advapi32.dll","GetSecurityDescriptorGroup",3},
        {"advapi32.dll","GetSecurityDescriptorDacl",4},{"advapi32.dll","GetSecurityDescriptorSacl",4},
        {"advapi32.dll","SetSecurityDescriptorOwner",3},{"advapi32.dll","SetSecurityDescriptorGroup",3},
        {"advapi32.dll","SetSecurityDescriptorSacl",4},{"advapi32.dll","SetSecurityDescriptorControl",3},
        {"advapi32.dll","MakeSelfRelativeSD",3},{"advapi32.dll","MakeAbsoluteSD",11},
        {"advapi32.dll","IsValidSid",1},
        {"advapi32.dll","GetSidLengthRequired",1},
        {"advapi32.dll","GetLengthSid",1},
        {"advapi32.dll","GetSidIdentifierAuthority",1},
        {"advapi32.dll","GetSidSubAuthorityCount",1},
        {"advapi32.dll","GetSidSubAuthority",2},
        {"advapi32.dll","EqualSid",2},
        {"advapi32.dll","EqualPrefixSid",2},
        {"advapi32.dll","CopySid",3},
        {"advapi32.dll","InitializeSid",3},
        {"advapi32.dll","AllocateAndInitializeSid",11},
        {"advapi32.dll","FreeSid",1},
        {"advapi32.dll","ConvertSidToStringSidA",2},
        {"advapi32.dll","ConvertSidToStringSidW",2},
        {"advapi32.dll","ConvertStringSidToSidA",2},
        {"advapi32.dll","ConvertStringSidToSidW",2},
        {"advapi32.dll","CreateWellKnownSid",4},
        {"advapi32.dll","IsWellKnownSid",2},
        {"advapi32.dll","GetWindowsAccountDomainSid",3},
        {"advapi32.dll","EqualDomainSid",3},
        {"ntdll.dll", "NtQueryObject", 5},{"ntdll.dll","NtSetInformationObject",4},
        {"shell32.dll", "IsUserAnAdmin", 0},
        {"advapi32.dll","ConvertStringSecurityDescriptorToSecurityDescriptorA",4},{"advapi32.dll","ConvertStringSecurityDescriptorToSecurityDescriptorW",4},
        {"advapi32.dll","InitializeSecurityDescriptor",2},{"advapi32.dll","SetSecurityDescriptorDacl",4},
        {"advapi32.dll","InitializeAcl",3},
        {"advapi32.dll","OpenThreadToken",4},{"advapi32.dll","DuplicateToken",3},{"advapi32.dll","DuplicateTokenEx",6},
        // processthreads API-set 使用 Kernel32 虛擬宿主；仍由同一安全權杖流程處理。
        {"kernel32.dll","OpenThreadToken",4},{"kernel32.dll","OpenProcessToken",3},
        {"kernel32.dll","SetThreadToken",2},
        {"advapi32.dll","SetThreadToken",2},{"advapi32.dll","RevertToSelf",0},{"advapi32.dll","ImpersonateSelf",1},
        {"advapi32.dll","ImpersonateNamedPipeClient",1},{"advapi32.dll","ImpersonateLoggedOnUser",1},{"advapi32.dll","CreateRestrictedToken",9},
        {"advapi32.dll","SetTokenInformation",4},{"advapi32.dll","IsTokenRestricted",1},
        {"advapi32.dll","CheckTokenMembership",3},{"advapi32.dll","AccessCheck",8},
        {"advapi32.dll","MapGenericMask",2},{"advapi32.dll","AreAllAccessesGranted",2},{"advapi32.dll","AreAnyAccessesGranted",2},
        {"advapi32.dll", "OpenProcessToken", 3},{"advapi32.dll","LogonUserA",6},{"advapi32.dll","LogonUserW",6},
        {"advapi32.dll", "GetKernelObjectSecurity", 5},{"advapi32.dll","SetKernelObjectSecurity",3},
        {"advapi32.dll","GetSecurityInfo",8},{"advapi32.dll","SetSecurityInfo",7},
        {"advapi32.dll", "LookupPrivilegeValueA", 3}, {"advapi32.dll", "LookupPrivilegeValueW", 3},
        {"advapi32.dll", "AdjustTokenPrivileges", 6}, {"advapi32.dll", "GetTokenInformation", 5},
        {"advapi32.dll", "LsaOpenPolicy", 4}, {"advapi32.dll", "LsaClose", 1},
        {"advapi32.dll", "LsaAddAccountRights", 4}, {"advapi32.dll", "LsaNtStatusToWinError", 1},
        {"advapi32.dll", "LookupAccountNameA", 7}, {"advapi32.dll", "LookupAccountNameW", 7},
        {"advapi32.dll", "GetFileSecurityA", 5}, {"advapi32.dll", "GetFileSecurityW", 5},
        {"advapi32.dll", "SetFileSecurityA", 3}, {"advapi32.dll", "SetFileSecurityW", 3}
    };
    return table;
}
namespace {
using namespace security;
std::uint32_t sid_rid(const std::vector<std::uint8_t>& sid,std::size_t index){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=static_cast<std::uint32_t>(sid.at(8+index*4+i))<<(i*8);return value;}
bool account_domain(const std::vector<std::uint8_t>& sid){return sid.size()>=24 && std::equal(sid.begin()+2,sid.begin()+8,std::array<std::uint8_t,6>{0,0,0,0,0,5}.begin()) && sid_rid(sid,0)==21;}
bool builtin_domain(const std::vector<std::uint8_t>& sid){return sid.size()>=12 && std::equal(sid.begin()+2,sid.begin()+8,std::array<std::uint8_t,6>{0,0,0,0,0,5}.begin()) && sid_rid(sid,0)==32;}
std::string sid_string(const std::vector<std::uint8_t>& sid){
    std::uint64_t authority=0;for(unsigned i=0;i<6;++i)authority=(authority<<8)|sid[2+i];std::string result="S-1-";
    if(authority<=UINT32_MAX)result+=std::to_string(authority);else{char hex[15]{};std::snprintf(hex,sizeof hex,"0x%012llx",static_cast<unsigned long long>(authority));result+=hex;}
    for(unsigned i=0;i<sid[1];++i)result+="-"+std::to_string(sid_rid(sid,i));return result;
}
std::vector<std::uint8_t> well_known_sid(unsigned type,const std::vector<std::uint8_t>& domain){
    // WELL_KNOWN_SID_TYPE 與 MS-DTYP 公開識別值；僅建立資料，不變更 token 或 ACL 權限。
    static const std::map<unsigned,std::string_view> fixed{
        {0,"S-1-0-0"},{1,"S-1-1-0"},{2,"S-1-2-0"},{3,"S-1-3-0"},{4,"S-1-3-1"},{5,"S-1-3-2"},{6,"S-1-3-3"},{7,"S-1-5"},
        {8,"S-1-5-1"},{9,"S-1-5-2"},{10,"S-1-5-3"},{11,"S-1-5-4"},{12,"S-1-5-6"},{13,"S-1-5-7"},{14,"S-1-5-8"},{15,"S-1-5-9"},{16,"S-1-5-10"},{17,"S-1-5-11"},{18,"S-1-5-12"},{19,"S-1-5-13"},{20,"S-1-5-14"},
        {22,"S-1-5-18"},{23,"S-1-5-19"},{24,"S-1-5-20"},{25,"S-1-5-32"},
        {26,"S-1-5-32-544"},{27,"S-1-5-32-545"},{28,"S-1-5-32-546"},{29,"S-1-5-32-547"},{30,"S-1-5-32-548"},{31,"S-1-5-32-549"},{32,"S-1-5-32-550"},{33,"S-1-5-32-551"},{34,"S-1-5-32-552"},{35,"S-1-5-32-554"},{36,"S-1-5-32-555"},{37,"S-1-5-32-556"},
        {51,"S-1-5-64-10"},{52,"S-1-5-64-21"},{53,"S-1-5-64-14"},{54,"S-1-5-15"},{55,"S-1-5-1000"},
        {56,"S-1-5-32-557"},{57,"S-1-5-32-558"},{58,"S-1-5-32-559"},{59,"S-1-5-32-560"},{60,"S-1-5-32-561"},{61,"S-1-5-32-562"},{62,"S-1-5-32-568"},{63,"S-1-5-17"},{64,"S-1-5-32-569"},
        {65,"S-1-16-0"},{66,"S-1-16-4096"},{67,"S-1-16-8192"},{68,"S-1-16-12288"},{69,"S-1-16-16384"},{70,"S-1-5-33"},{71,"S-1-3-4"},
        {76,"S-1-5-32-573"},{78,"S-1-5-32-574"},{79,"S-1-16-8448"},{80,"S-1-2-1"},{81,"S-1-2-1"},{82,"S-1-5-65-1"},{83,"S-1-15"},{84,"S-1-15-2-1"},
        {85,"S-1-15-3-1"},{86,"S-1-15-3-2"},{87,"S-1-15-3-3"},{88,"S-1-15-3-4"},{89,"S-1-15-3-5"},{90,"S-1-15-3-6"},{91,"S-1-15-3-7"},{92,"S-1-15-3-8"},{93,"S-1-15-3-9"},{94,"S-1-15-3-10"},
        {95,"S-1-5-32-575"},{96,"S-1-5-32-576"},{97,"S-1-5-32-577"},{98,"S-1-5-84-0-0-0-0-0"},{99,"S-1-5-32-578"},{101,"S-1-5-32-579"},{102,"S-1-5-32-580"},
        {103,"S-1-18-1"},{104,"S-1-18-2"},{105,"S-1-5-113"},{106,"S-1-5-114"},{108,"S-1-15-3-11"},{109,"S-1-15-3-12"},{111,"S-1-5-32-581"},{112,"S-1-5-32-582"},
        {115,"S-1-18-4"},{116,"S-1-18-5"},{117,"S-1-18-6"},{118,"S-1-18-3"},{119,"S-1-5-32-583"},{120,"S-1-5-32-584"},{121,"S-1-5-32-585"}
    };
    if(const auto item=fixed.find(type);item!=fixed.end())return sddl_sid(item->second);
    static const std::map<unsigned,unsigned> domain_rids{{38,500},{39,501},{40,502},{41,512},{42,513},{43,514},{44,515},{45,516},{46,517},{47,518},{48,519},{49,520},{50,553},{72,571},{73,572},{75,521},{77,498},{100,522},{107,525},{110,503},{113,526},{114,527}};
    if(const auto item=domain_rids.find(type);item!=domain_rids.end()){
        if(domain.empty())throw SddlError{50};if(domain[1]!=4 || !account_domain(domain))throw SddlError{87};auto result=domain;result[1]=5;result.resize(28);sddl_put(result,24,item->second);return result;
    }
    throw SddlError{type==74?50U:87U}; // 舊版 enterprise read-only SID 尚未提供；logon ID 必須由登入後端產生。
}

constexpr std::uint32_t token_query = 0x8, token_adjust_privileges = 0x20, token_all_access = 0xf01ff;
// NT 的公開 privilege LUID；僅提供本虛擬程序的查表，絕不啟用宿主權限。
constexpr std::array<std::string_view, 35> privilege_names{
    "SeCreateTokenPrivilege", "SeAssignPrimaryTokenPrivilege", "SeLockMemoryPrivilege",
    "SeIncreaseQuotaPrivilege", "SeMachineAccountPrivilege", "SeTcbPrivilege", "SeSecurityPrivilege",
    "SeTakeOwnershipPrivilege", "SeLoadDriverPrivilege", "SeSystemProfilePrivilege", "SeSystemtimePrivilege",
    "SeProfileSingleProcessPrivilege", "SeIncreaseBasePriorityPrivilege", "SeCreatePagefilePrivilege",
    "SeCreatePermanentPrivilege", "SeBackupPrivilege", "SeRestorePrivilege", "SeShutdownPrivilege",
    "SeDebugPrivilege", "SeAuditPrivilege", "SeSystemEnvironmentPrivilege", "SeChangeNotifyPrivilege",
    "SeRemoteShutdownPrivilege", "SeUndockPrivilege", "SeSyncAgentPrivilege", "SeEnableDelegationPrivilege",
    "SeManageVolumePrivilege", "SeImpersonatePrivilege", "SeCreateGlobalPrivilege", "SeTrustedCredManAccessPrivilege",
    "SeRelabelPrivilege", "SeIncreaseWorkingSetPrivilege", "SeTimeZonePrivilege", "SeCreateSymbolicLinkPrivilege",
    "SeDelegateSessionUserImpersonatePrivilege"
};
std::uint32_t token_access(std::uint32_t requested) {
    auto access = requested & 0x0fffffffU;
    if (requested & 0x80000000U) access |= 0x20008U; // GENERIC_READ
    if (requested & 0x40000000U) access |= 0x200e0U; // GENERIC_WRITE
    if (requested & 0x20000000U) access |= 0x20000U; // GENERIC_EXECUTE
    if (requested & 0x10000000U) access |= token_all_access;
    if (access & 0x02000000U) access = (access & ~0x02000000U) | token_all_access;
    return access;
}
std::uint32_t lsa_winerror(std::uint32_t status) {
    switch (status) {
    case 0: return 0;
    case 0x8000001a: return 259; // STATUS_NO_MORE_ENTRIES
    case 0xc0000002: return 120;
    case 0xc0000008: return 6;
    case 0xc000000d: return 87;
    case 0xc0000017: return 8;
    case 0xc0000022: return 5;
    case 0xc0000034: return 2;
    case 0xc0000060: return 1313;
    case 0xc0000061: return 1314;
    case 0xc0000073: return 1332;
    case 0xc000009a: return 1450;
    case 0xc00000bb: return 50;
    default: return authentication::winerror(status);
    }
}
}
std::u16string_view WindowsServices::object_type(std::uint64_t handle) const {
    if(handle==(x86_?UINT32_MAX-1:UINT64_MAX-1) || runtime_.process_thread_handles.contains(handle))return u"Thread";
    if(const auto type=process_object_type(handle);!type.empty())return type;
    if((handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100]) || files_.contains(handle) || runtime_.network.sockets.contains(handle) || runtime_.directory_searches.contains(handle))return u"File";
    if(runtime_.token_handles.contains(handle))return u"Token";
    if(power_requests_.contains(handle))return u"PowerRequest";
    if(transaction_handle(handle))return u"TmTx";
    if(job_handle(handle))return u"Job";
    if(runtime_.completion_ports.contains(handle))return u"IoCompletion";
    if(mapping_handle(handle) || runtime_.process_snapshots.contains(handle))return u"Section";
    if(runtime_.registry.handles.contains(handle))return u"Key";
    if(const auto found=runtime_.wait_objects.find(handle);found!=runtime_.wait_objects.end()){
        switch(found->second.kind){
        case WaitObject::Kind::Event:case WaitObject::Kind::MemoryResource:return u"Event";
        case WaitObject::Kind::Semaphore:return u"Semaphore";
        case WaitObject::Kind::Timer:return u"Timer";
        case WaitObject::Kind::Mutex:return u"Mutant";
        case WaitObject::Kind::ChangeNotification:return u"File";
        }
    }
    return {};
}
std::pair<std::uint64_t,std::uint32_t> WindowsServices::allocate_luid(){
#if !defined(_WIN32)
                const auto path=std::filesystem::path(mapping_session())/"local-luid.counter";
                struct Descriptor {int fd;~Descriptor(){if(fd>=0)::close(fd);}} file{::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600)};
                const auto io_failure=[](){return errno==EACCES || errno==EPERM || errno==ELOOP?5U:errno==ENOSPC?112U:31U;};
                unsigned error=0;if(file.fd<0)error=io_failure();
                else{while(::flock(file.fd,LOCK_EX)<0){if(errno==EINTR)continue;error=io_failure();break;}}
                off_t append_offset=0;std::uint64_t value=UINT64_C(0x100000000); // 保留低值給既有虛擬 token 與固定 LUID。
                if(!error){struct stat info{};if(::fstat(file.fd,&info)<0)error=io_failure();
                    else if(!S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || (info.st_size<0 || (info.st_size%8)!=0))error=13;
                    else if(info.st_size){append_offset=info.st_size;std::array<std::uint8_t,8> bytes{};ssize_t read;do{read=::pread(file.fd,bytes.data(),bytes.size(),append_offset-8);}while(read<0 && errno==EINTR);
                        if(read!=8)error=13;else{value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t{bytes[i]}<<(i*8);if(value<UINT64_C(0x100000000))error=13;}}
                }
                if(!error && (value==UINT64_MAX || append_offset>INT64_MAX-8))error=1450;
                if(!error){const auto next=value+1;std::array<std::uint8_t,8> bytes{};for(unsigned i=0;i<8;++i)bytes[i]=static_cast<std::uint8_t>(next>>(i*8));
                    ssize_t written;do{written=::pwrite(file.fd,bytes.data(),bytes.size(),append_offset);}while(written<0 && errno==EINTR);
                    if(written!=8)error=io_failure();else if(::fsync(file.fd)<0)error=io_failure();
                    // 追加 journal 後才公開 LUID；短寫留下不完整尾端，下次拒絕配置，不倒退重用。
                    }
                return {error?0:value,error};
#else
                return {0,50};
#endif
}
std::optional<WindowsServices::ObjectMetadata> WindowsServices::object_metadata(std::uint64_t handle)const{
    if(const auto mapping=mapping_object_metadata(handle))return mapping;
    if(const auto job=job_object_metadata(handle))return job;
    if(const auto process=process_object_metadata(handle))return process;
    const auto type=object_type(handle);if(type.empty())return {};ObjectMetadata out;out.identity=handle;out.handles=1;out.pointers=1;
    if(const auto file=files_.find(handle);file!=files_.end()){
        out.access=security::mapped_access(file->second.access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});out.identity=file->second.object;out.handles=0;
        for(const auto& [h,item]:files_){(void)h;if(item.object==out.identity)++out.handles;}out.pointers=out.handles;
        for(const auto& io:pending_file_io_)if(const auto at=files_.find(io.handle);at!=files_.end() && at->second.object==out.identity)++out.pointers;
    }else if(const auto token=runtime_.token_handles.find(handle);token!=runtime_.token_handles.end()){
        out.access=token->second;out.identity=runtime_.token_handle_objects.at(handle);out.handles=0;for(const auto& [h,id]:runtime_.token_handle_objects){(void)h;if(id==out.identity)++out.handles;}out.pointers=out.handles;if(out.identity==1)++out.pointers;
    }else if(const auto wait=runtime_.wait_objects.find(handle);wait!=runtime_.wait_objects.end()){
        const auto& source=wait->second;out.access=source.access;out.identity=source.shared_event?reinterpret_cast<std::uintptr_t>(source.shared_event.get()):source.mutex?source.mutex:source.identity?source.identity:handle;out.handles=0;
        for(const auto& [h,item]:runtime_.wait_objects){if(item.kind!=source.kind)continue;const auto identity=item.shared_event?reinterpret_cast<std::uintptr_t>(item.shared_event.get()):item.mutex?item.mutex:item.identity?item.identity:h;if(identity==out.identity)++out.handles;}out.pointers=out.handles;
    }else if(const auto key=runtime_.registry.handles.find(handle);key!=runtime_.registry.handles.end()){
        out.access=key->second.access;out.identity=std::hash<std::u16string>{}(key->second.path);out.handles=0;for(const auto& [h,item]:runtime_.registry.handles){(void)h;if(item.path==key->second.path)++out.handles;}out.pointers=out.handles;
    }else if(type==u"Thread"){
        out.access=0x1fffff;out.identity=thread_id(handle);out.handles=0;for(const auto& [h,item]:runtime_.process_thread_handles){(void)h;if(item.first==out.identity)++out.handles;}out.pointers=out.handles+(threads_.items.contains(out.identity)?1U:0U);
    }else if(handle>=0x100 && handle<=0x102)out.access=handle==0x100?0x120089U:0x120116U;
    else if(runtime_.completion_ports.contains(handle))out.access=0x1f0003;
    else if(runtime_.process_snapshots.contains(handle))out.access=0xf001f;
    else if(runtime_.network.sockets.contains(handle)){out.access=0x1f01ff;out.identity=reinterpret_cast<std::uintptr_t>(runtime_.network.sockets.at(handle).get());out.handles=0;for(const auto& [h,socket]:runtime_.network.sockets){(void)h;if(reinterpret_cast<std::uintptr_t>(socket.get())==out.identity)++out.handles;}out.pointers=out.handles;}
    else if(runtime_.directory_searches.contains(handle))out.access=0x120089;
    else if(const auto transaction=transaction_identity(handle)){out.access=0x12003fU;out.identity=transaction->first;out.handles=transaction->second;out.pointers=out.handles;}
    else if(power_requests_.contains(handle)){out.access=0x1f0003;out.identity=reinterpret_cast<std::uintptr_t>(power_requests_.at(handle).get());out.handles=0;for(const auto& [h,request]:power_requests_){(void)h;if(reinterpret_cast<std::uintptr_t>(request.get())==out.identity)++out.handles;}out.pointers=out.handles;}
    return out;
}
std::optional<std::uint32_t> WindowsServices::invoke_security(GuestState& state, GuestMemory& memory,
                                                           const WindowsServiceDescriptor& service) {
    security::NativeFileIdentityScope identity_scope;
    if(service.symbol=="GetSecurityInfo" || service.symbol=="SetSecurityInfo" || service.symbol.starts_with("GetNamedSecurityInfo") || service.symbol.starts_with("SetNamedSecurityInfo")){
        const auto kind=WindowsCall(state,memory,service)(1);
        if(kind==4 || kind==12 || kind==13)return invoke_registry_security(state,memory,service);
    }
    if(service.symbol=="LogonUserA" || service.symbol=="LogonUserW")return invoke_lsa(state,memory,service);
    if(service.symbol=="GetSecurityInfo" || service.symbol=="SetSecurityInfo" || service.symbol=="GetKernelObjectSecurity" || service.symbol=="SetKernelObjectSecurity"){
        const WindowsCall call(state,memory,service);
        const bool info=service.symbol=="GetSecurityInfo" || service.symbol=="SetSecurityInfo";
        if(((runtime_.wait_objects.contains(call(0)) || mapping_handle(call(0)) || job_handle(call(0))) && (!info || call(1)==6)) ||
           (info && call(1)==7) ||
           (files_.contains(call(0)) && files_.at(call(0)).pipe && (!info || call(1)==1)) ||
           ((!process_object_type(call(0)).empty() || thread_id(call(0))) && (!info || call(1)==6)))return invoke_object_security(state,memory,service);
    }
    if(service.symbol.starts_with("BuildSecurityDescriptor") || service.symbol.starts_with("GetNamedSecurityInfo") ||
       service.symbol.starts_with("SetNamedSecurityInfo") || service.symbol.starts_with("GetFileSecurity") ||
       service.symbol.starts_with("SetFileSecurity") ||
       ((service.symbol=="GetSecurityInfo" || service.symbol=="SetSecurityInfo") && WindowsCall(state,memory,service)(1)==1))
        return invoke_security_descriptor(state,memory,service);
    for(const auto name:{"GetSecurityInfo","SetSecurityInfo","SetKernelObjectSecurity","GetKernelObjectSecurity","OpenProcessToken","OpenThreadToken","DuplicateToken","DuplicateTokenEx","SetThreadToken","RevertToSelf","ImpersonateSelf","ImpersonateAnonymousToken","ImpersonateLoggedOnUser","ImpersonateNamedPipeClient","CreateRestrictedToken","SetTokenInformation","GetTokenInformation","AdjustTokenPrivileges","IsTokenRestricted","CheckTokenMembership","AccessCheck","MapGenericMask","AreAllAccessesGranted","AreAnyAccessesGranted"})
        if(service.symbol==name)return invoke_token(state,memory,service);
    const WindowsCall arg(state, memory, service);
    const auto u32 = [&](unsigned index) { return static_cast<std::uint32_t>(arg(index)); };
    const auto symbol = service.symbol;
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) { last_error_ = error; result = 0; };
    if(symbol=="AllocateLocallyUniqueId"){
        if(!arg(0))fail(87);
        else{
            try{memory.check(GuestAddress{arg(0)},8,Access::Write);
                const auto [value,error]=allocate_luid();
                if(error)fail(error);else{memory.store(GuestAddress{arg(0)},64,value);result=1;}
            }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
        }
    }else if(symbol.starts_with("BuildTrusteeWith") || symbol.starts_with("BuildExplicitAccessWithName") || symbol.starts_with("GetTrustee")){
        const unsigned unit=x86_?4:8,trustee_size=x86_?20:32,name_offset=x86_?16:24;const std::uint8_t bits=x86_?32:64;
        if(symbol.starts_with("GetTrustee")){
            if(symbol.starts_with("GetTrusteeName"))result=memory.load(GuestAddress{arg(0)+name_offset},bits);
            else result=memory.load(GuestAddress{arg(0)+unit+(symbol.starts_with("GetTrusteeType")?8U:4U)},32);
        }else {
            const bool explicit_entry=symbol.starts_with("BuildExplicitAccessWithName");const auto trustee=arg(0)+(explicit_entry?(x86_?12U:16U):0U);
            memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(explicit_entry?(x86_?32U:48U):trustee_size));
            if(explicit_entry){memory.store(GuestAddress{arg(0)},32,u32(2));memory.store(GuestAddress{arg(0)+4},32,u32(3));memory.store(GuestAddress{arg(0)+8},32,u32(4));}
            memory.store(GuestAddress{trustee+unit+4},32,symbol.starts_with("BuildTrusteeWithSid")?0U:1U);memory.store(GuestAddress{trustee+name_offset},bits,arg(1));
        }
    }else if(symbol.starts_with("SetEntriesInAcl") || symbol.starts_with("GetExplicitEntriesFromAcl")){
        const bool setting=symbol.starts_with("Set");const unsigned unit=x86_?4:8,entry_size=x86_?32:48,trustee_offset=x86_?12:16,name_offset=x86_?16:24;const std::uint8_t bits=x86_?32:64;
        try {
            if(!arg(setting?3:2) || (!setting && !arg(1)))throw SddlError{87};
            memory.store(GuestAddress{arg(setting?3:2)},bits,0);if(!setting)memory.store(GuestAddress{arg(1)},32,0);
            std::vector<std::vector<std::uint8_t>> aces;std::vector<std::uint8_t> previous;
            const auto source=arg(setting?2:0);
            if(source){const auto acl=read_acl(memory,source);previous=acl.bytes;for(std::size_t i=0;i<acl.offsets.size();++i){const auto end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;aces.emplace_back(acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.offsets[i]),acl.bytes.begin()+static_cast<std::ptrdiff_t>(end));}}
            else if(!setting)throw SddlError{1336};
            if(setting){
                const auto entries=read_explicit_entries(memory,arg(1),u32(0),x86_,symbol.ends_with('W'),
                    [&](std::string_view name){return security_trustee_name(name);});
                const auto bytes=merge_acl(previous,entries);
                if(!bytes.empty()){
                    const auto output=allocate_heap(memory,bytes.size());if(!output)throw SddlError{8};
                    memory.write(GuestAddress{output},bytes);runtime_.local_allocations.insert(output);
                    memory.store(GuestAddress{arg(3)},bits,output);
                }
            }else {
                std::vector<ExplicitAce> entries;std::size_t size=aces.size()*entry_size;for(const auto& ace:aces){entries.push_back(basic_ace(ace));size+=entries.back().sid.size();}
                if(!entries.empty()){
                    const auto output=allocate_heap(memory,size);if(!output)throw SddlError{8};std::vector<std::uint8_t> bytes(size);std::size_t sid_at=entries.size()*entry_size;
                    for(std::size_t i=0;i<entries.size();++i){const auto& ace=entries[i];const auto entry=i*entry_size,trustee=entry+trustee_offset;
                        const auto mode=ace.type==0?1U:ace.type==1?3U:(ace.flags&0xc0)==0xc0?7U:(ace.flags&0x80)?6U:5U;
                        sddl_put(bytes,entry,ace.mask);sddl_put(bytes,entry+4,mode);sddl_put(bytes,entry+8,ace.flags&31U);
                        for(unsigned byte=0;byte<unit;++byte)bytes[trustee+name_offset+byte]=static_cast<std::uint8_t>((output+sid_at)>>(byte*8));std::copy(ace.sid.begin(),ace.sid.end(),bytes.begin()+static_cast<std::ptrdiff_t>(sid_at));sid_at+=ace.sid.size();
                    }
                    memory.write(GuestAddress{output},bytes);runtime_.local_allocations.insert(output);memory.store(GuestAddress{arg(2)},bits,output);
                }
                memory.store(GuestAddress{arg(1)},32,entries.size());
            }
        }catch(const SddlError& error){result=error.code;}catch(const MemoryFault&){result=87;}catch(const std::invalid_argument&){result=87;}
    }else if(symbol=="IsValidAcl" || symbol=="GetAclInformation" || symbol=="SetAclInformation" || symbol=="GetAce" || symbol=="DeleteAce" || symbol=="AddAce" || symbol=="FindFirstFreeAce" || symbol.starts_with("AddAccessAllowedAce") || symbol.starts_with("AddAccessDeniedAce") || symbol.starts_with("AddAuditAccessAce") || symbol=="AddMandatoryAce"){
        try{
            if(symbol=="FindFirstFreeAce" && arg(1))memory.store(GuestAddress{arg(1)},x86_?32:64,0);
            auto acl=read_acl(memory,arg(0));
            if(symbol=="IsValidAcl")result=1;
            else if(symbol=="FindFirstFreeAce"){if(!arg(1))throw SddlError{87};memory.store(GuestAddress{arg(1)},x86_?32:64,arg(0)+acl.used);result=1;}
            else if(symbol=="GetAce"){
                if(!arg(2) || u32(1)>=acl.offsets.size())throw SddlError{87};memory.store(GuestAddress{arg(2)},x86_?32:64,arg(0)+acl.offsets[u32(1)]);result=1;
            }else if(symbol=="GetAclInformation"){
                if(u32(3)!=1 && u32(3)!=2)throw SddlError{87};const unsigned size=u32(3)==1?4U:12U;if(!arg(1))throw SddlError{87};if(u32(2)<size)throw SddlError{122};
                std::vector<std::uint8_t> bytes(size);if(u32(3)==1)sddl_put(bytes,0,acl.bytes[0]);else{sddl_put(bytes,0,static_cast<std::uint32_t>(acl.offsets.size()));sddl_put(bytes,4,static_cast<std::uint32_t>(acl.used));sddl_put(bytes,8,static_cast<std::uint32_t>(acl.bytes.size()-acl.used));}memory.write(GuestAddress{arg(1)},bytes);result=1;
            }else if(symbol=="SetAclInformation"){
                if(u32(3)!=1 || !arg(1))throw SddlError{87};if(u32(2)<4)throw SddlError{122};const auto revision=memory.load(GuestAddress{arg(1)},32);if((revision!=2 && revision!=4) || revision<acl.bytes[0])throw SddlError{87};memory.store(GuestAddress{arg(0)},8,revision);result=1;
            }else if(symbol=="DeleteAce"){
                const auto index=u32(1);if(index>=acl.offsets.size())throw SddlError{87};const auto start=acl.offsets[index],end=index+1<acl.offsets.size()?acl.offsets[index+1]:acl.used;
                std::move(acl.bytes.begin()+static_cast<std::ptrdiff_t>(end),acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.used),acl.bytes.begin()+static_cast<std::ptrdiff_t>(start));
                std::fill(acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.used-(end-start)),acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.used),0);sddl_put(acl.bytes,4,static_cast<std::uint32_t>(acl.offsets.size()-1),2);memory.write(GuestAddress{arg(0)},acl.bytes);result=1;
            }else{
                const auto revision=u32(1);if(revision!=2 && revision!=4)throw SddlError{1305};std::vector<std::uint8_t> addition;std::size_t count=0,index=acl.offsets.size();
                if(symbol=="AddAce"){
                    if(u32(2)!=UINT32_MAX){index=u32(2);if(index>acl.offsets.size())throw SddlError{87};}if(u32(4)>65535 || (u32(4) && !arg(3)))throw SddlError{87};addition.resize(u32(4));memory.read(GuestAddress{arg(3)},addition);
                    for(std::size_t at=0;at<addition.size();){if(addition.size()-at<4)throw SddlError{87};const auto length=static_cast<unsigned>(addition[at+2])|(static_cast<unsigned>(addition[at+3])<<8);if(length<4 || (length&3) || length>addition.size()-at)throw SddlError{87};const auto type=addition[at];if(((type>=5 && type<=8) || type==11 || type==12 || type==15 || type==16) && revision<4)throw SddlError{87};at+=length;++count;}
                }else{
                    const bool mandatory=symbol=="AddMandatoryAce",extended=mandatory || symbol.ends_with("Ex"),audit=symbol.starts_with("AddAuditAccessAce");const unsigned mask_index=extended?3U:2U,sid_index=mask_index+1;
                    auto flags=extended?u32(2):0U;if(flags&~31U)throw SddlError{1004};if(audit){if(arg(sid_index+1))flags|=0x40;if(arg(sid_index+2))flags|=0x80;}
                    const auto sid=read_sid(memory,arg(sid_index));if(mandatory && ((u32(mask_index)&~7U) || sid[1]!=1 || sid[7]!=16 || !std::all_of(sid.begin()+2,sid.begin()+7,[](auto byte){return !byte;})))throw SddlError{87};
                    addition.resize(8+sid.size());addition[0]=static_cast<std::uint8_t>(mandatory?17:audit?2:symbol.starts_with("AddAccessDenied")?1:0);addition[1]=static_cast<std::uint8_t>(flags);sddl_put(addition,2,static_cast<std::uint32_t>(addition.size()),2);sddl_put(addition,4,u32(mask_index));std::copy(sid.begin(),sid.end(),addition.begin()+8);count=1;
                }
                if(addition.size()>acl.bytes.size()-acl.used || count>65535-acl.offsets.size())throw SddlError{symbol=="AddAce"?122U:1344U};
                const auto at=index<acl.offsets.size()?acl.offsets[index]:acl.used;
                std::move_backward(acl.bytes.begin()+static_cast<std::ptrdiff_t>(at),acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.used),acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.used+addition.size()));std::copy(addition.begin(),addition.end(),acl.bytes.begin()+static_cast<std::ptrdiff_t>(at));
                acl.bytes[0]=static_cast<std::uint8_t>(std::max<unsigned>(acl.bytes[0],revision));sddl_put(acl.bytes,4,static_cast<std::uint32_t>(acl.offsets.size()+count),2);memory.write(GuestAddress{arg(0)},acl.bytes);result=1;
            }
        }catch(const SddlError& error){fail(error.code);}catch(const MemoryFault&){fail(1336);}
    }else if(symbol=="IsValidSecurityDescriptor" || symbol.starts_with("GetSecurityDescriptor") || symbol.starts_with("SetSecurityDescriptor") || symbol=="MakeSelfRelativeSD" || symbol=="MakeAbsoluteSD"){
        try{
            const bool contents=symbol=="IsValidSecurityDescriptor" || symbol=="GetSecurityDescriptorLength" || symbol=="MakeSelfRelativeSD" || symbol=="MakeAbsoluteSD";
            const auto sd=read_descriptor(memory,arg(0),x86_,contents);const auto control=sd.control;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U,first=x86_?4U:8U;
            if(symbol=="IsValidSecurityDescriptor")result=1;
            else if(symbol=="GetSecurityDescriptorLength")result=relative_descriptor(sd).size();
            else if(symbol=="GetSecurityDescriptorControl"){
                if(!arg(1) || !arg(2))throw SddlError{87};memory.check(GuestAddress{arg(1)},2,Access::Write);memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(1)},16,control);memory.store(GuestAddress{arg(2)},32,1);result=1;
            }else if(symbol=="SetSecurityDescriptorControl"){
                const auto interest=u32(1)&0xffffU,value=u32(2)&0xffffU;if((interest|value)&~0x3f00U)throw SddlError{87};memory.store(GuestAddress{arg(0)+2},16,(control&~interest)|(value&interest));result=1;
            }else if(symbol=="MakeSelfRelativeSD"){
                if(!arg(2))throw SddlError{87};const auto bytes=relative_descriptor(sd);const auto capacity=memory.load(GuestAddress{arg(2)},32);memory.store(GuestAddress{arg(2)},32,bytes.size());if(!arg(1) || capacity<bytes.size())throw SddlError{122};memory.write(GuestAddress{arg(1)},bytes);result=1;
            }else if(symbol=="MakeAbsoluteSD"){
                if(!(control&0x8000))throw SddlError{1338};const std::array<unsigned,4> arguments{7,9,5,3};std::array<std::uint64_t,5> sizes{first+unit*4,sd.data[3].size(),sd.data[2].size(),sd.data[0].size(),sd.data[1].size()};bool fits=true;
                for(unsigned i=0;i<5;++i){const auto output=1+i*2,size_pointer=output+1;if(!arg(size_pointer))throw SddlError{87};memory.check(GuestAddress{arg(size_pointer)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(size_pointer)},32);if(sizes[i] && (!arg(output) || capacity<sizes[i]))fits=false;}
                for(unsigned i=0;i<5;++i)memory.store(GuestAddress{arg(2+i*2)},32,sizes[i]);if(!fits)throw SddlError{122};
                for(unsigned i=0;i<5;++i)if(sizes[i])memory.check(GuestAddress{arg(1+i*2)},sizes[i],Access::Write);
                std::vector<std::uint8_t> header(first+unit*4);header[0]=1;header[1]=sd.rm_control;sddl_put(header,2,control&~0x8000U,2);
                // sddl_put 是 32-bit 欄位工具；絕對指標依客體寬度寫入，保留高 32 bits。
                for(unsigned i=0;i<4;++i)if(!sd.data[i].empty()){const auto pointer=arg(arguments[i]);for(unsigned j=0;j<unit;++j)header[first+i*unit+j]=static_cast<std::uint8_t>(pointer>>(j*8));memory.write(GuestAddress{pointer},sd.data[i]);}
                memory.write(GuestAddress{arg(1)},header);result=1;
            }else{
                const bool setting=symbol.starts_with("Set"),acl=symbol.ends_with("Dacl") || symbol.ends_with("Sacl");const unsigned part=symbol.ends_with("Owner")?0U:symbol.ends_with("Group")?1U:symbol.ends_with("Sacl")?2U:3U;
                const auto defaulted=part==0?1U:part==1?2U:part==2?32U:8U,present=part==2?16U:4U;
                if(setting){
                    if(control&0x8000)throw SddlError{1338};const bool active=!acl || arg(1),is_default=arg(acl?3:2)!=0;auto updated=static_cast<unsigned>(control);
                    if(acl)updated=(updated&~(present|defaulted))|(active?present:0U)|(active && is_default?defaulted:0U);else updated=(updated&~defaulted)|(is_default?defaulted:0U);
                    memory.check(GuestAddress{arg(0)},first+unit*4,Access::Write);if(active)memory.store(GuestAddress{arg(0)+first+part*unit},bits,arg(acl?2:1));memory.store(GuestAddress{arg(0)+2},16,updated);result=1;
                }else{
                    if(!arg(1) || !arg(2) || (acl && !arg(3)))throw SddlError{87};memory.check(GuestAddress{arg(1)},acl?4:unit,Access::Write);memory.check(GuestAddress{arg(2)},acl?unit:4,Access::Write);if(acl)memory.check(GuestAddress{arg(3)},4,Access::Write);
                    if(acl){memory.store(GuestAddress{arg(1)},32,(control&present)!=0);if(control&present){memory.store(GuestAddress{arg(2)},bits,sd.parts[part]);memory.store(GuestAddress{arg(3)},32,(control&defaulted)!=0);}}
                    else{memory.store(GuestAddress{arg(1)},bits,sd.parts[part]);memory.store(GuestAddress{arg(2)},32,(control&defaulted)!=0);}result=1;
                }
            }
        }catch(const SddlError& error){fail(error.code);}catch(const MemoryFault&){fail(1338);}
    }else
    if(symbol=="IsValidSid" || symbol=="GetLengthSid" || symbol=="GetSidIdentifierAuthority" || symbol=="GetSidSubAuthorityCount" || symbol=="GetSidSubAuthority" || symbol=="EqualSid" || symbol=="EqualPrefixSid" || symbol=="CopySid" || symbol.starts_with("ConvertSidToStringSid") || symbol.starts_with("ConvertStringSidToSid") || symbol=="GetSidLengthRequired" || symbol=="InitializeSid" || symbol=="AllocateAndInitializeSid" || symbol=="FreeSid" || symbol=="CreateWellKnownSid" || symbol=="IsWellKnownSid" || symbol=="GetWindowsAccountDomainSid" || symbol=="EqualDomainSid"){
        try{
            const auto allocate=[&](const std::vector<std::uint8_t>& bytes,std::uint64_t output){
                if(!output)throw SddlError{87};memory.check(GuestAddress{output},x86_?4:8,Access::Write);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)throw SddlError{8};
                memory.write(GuestAddress{pointer},bytes);runtime_.local_allocations.insert(pointer);memory.store(GuestAddress{output},x86_?32:64,pointer);result=1;
            };
            if(symbol=="GetSidLengthRequired")result=8+4*(u32(0)&255U);
            else if(symbol=="FreeSid"){
                if(arg(0) && (!runtime_.local_allocations.contains(arg(0)) || !free_heap(memory,arg(0),state))){last_error_=87;result=arg(0);}else{runtime_.local_allocations.erase(arg(0));result=0;}
            }else if(symbol=="InitializeSid" || symbol=="AllocateAndInitializeSid"){
                const bool allocated=symbol=="AllocateAndInitializeSid";const auto authority=arg(allocated?0:1);const auto count=u32(allocated?1:2)&255U;
                if(!authority || count>(allocated?8U:15U) || (!allocated && !arg(0)))throw SddlError{87};
                std::vector<std::uint8_t> bytes(8+count*4);bytes[0]=1;bytes[1]=static_cast<std::uint8_t>(count);memory.read(GuestAddress{authority},std::span(bytes).subspan(2,6));
                if(allocated){for(unsigned i=0;i<count;++i)sddl_put(bytes,8+i*4,u32(2+i));allocate(bytes,arg(10));}
                else{memory.write(GuestAddress{arg(0)},bytes);result=1;}
            }else if(symbol.starts_with("ConvertStringSidToSid")){
                if(!arg(0))throw SddlError{87};const auto text=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));allocate(sddl_sid(text),arg(1));
            }else if(symbol=="CreateWellKnownSid"){
                if(!arg(3))throw SddlError{87};const auto bytes=well_known_sid(u32(0),arg(1)?read_sid(memory,arg(1)):std::vector<std::uint8_t>{});
                const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.store(GuestAddress{arg(3)},32,bytes.size());if(!arg(2) || capacity<bytes.size())throw SddlError{122};memory.write(GuestAddress{arg(2)},bytes);result=1;
            }else{
                const auto sid=read_sid(memory,arg(symbol=="CopySid"?2:0));
                if(symbol=="IsValidSid")result=1;
                else if(symbol=="GetLengthSid")result=sid.size();
                else if(symbol=="GetSidIdentifierAuthority")result=arg(0)+2;
                else if(symbol=="GetSidSubAuthorityCount")result=arg(0)+1;
                else if(symbol=="GetSidSubAuthority"){if(u32(1)>=sid[1])throw SddlError{87};result=arg(0)+8+u32(1)*4;}
                else if(symbol=="CopySid"){if(u32(0)<sid.size())throw SddlError{122};if(!arg(1))throw SddlError{87};memory.write(GuestAddress{arg(1)},sid);result=1;}
                else if(symbol=="EqualSid" || symbol=="EqualPrefixSid"){
                    auto other=read_sid(memory,arg(1));const auto size=symbol=="EqualSid" || !sid[1]?sid.size():sid.size()-4;result=other.size()==sid.size() && std::equal(sid.begin(),sid.begin()+static_cast<std::ptrdiff_t>(size),other.begin());
                }else if(symbol.starts_with("ConvertSidToStringSid")){
                    const auto text=sid_string(sid);if(symbol.ends_with('W'))allocate(utf16_bytes(utf8_to_utf16(text)),arg(1));else{std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);allocate(bytes,arg(1));}
                }else if(symbol=="IsWellKnownSid"){
                    if(u32(1)==21)result=sid[1]==3 && sid[7]==5 && std::all_of(sid.begin()+2,sid.begin()+7,[](auto byte){return !byte;}) && sid_rid(sid,0)==5;
                    else{auto domain=sid;if(domain[1]==5 && account_domain(domain)){domain.resize(24);domain[1]=4;}else domain.clear();result=sid==well_known_sid(u32(1),domain);}
                }else if(symbol=="GetWindowsAccountDomainSid"){
                    if(!arg(2))throw SddlError{87};if(!account_domain(sid))throw SddlError{1257};const auto capacity=memory.load(GuestAddress{arg(2)},32);memory.store(GuestAddress{arg(2)},32,24);
                    if(!arg(1) || capacity<24)throw SddlError{122};auto domain=sid;domain.resize(24);domain[1]=4;memory.write(GuestAddress{arg(1)},domain);result=1;
                }else if(symbol=="EqualDomainSid"){
                    if(!arg(2))throw SddlError{87};const auto other=read_sid(memory,arg(1));const bool first_account=account_domain(sid),second_account=account_domain(other),first_builtin=builtin_domain(sid),second_builtin=builtin_domain(other);if((!first_account && !first_builtin) || (!second_account && !second_builtin))throw SddlError{1257};const bool same=(first_builtin && second_builtin) || (first_account && second_account && std::equal(sid.begin()+2,sid.begin()+24,other.begin()+2));memory.store(GuestAddress{arg(2)},32,same);result=1;
                }
            }
        }catch(const SddlError& error){fail(error.code);}catch(const MemoryFault&){fail(1337);}
    }else
    if(symbol.starts_with("ConvertStringSecurityDescriptorToSecurityDescriptor")){
        if(!arg(0) || !arg(2))fail(87);else if(u32(1)!=1)fail(1305);
        else {try {
            const auto text=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));const auto bytes=sddl_descriptor(text);
            memory.check(GuestAddress{arg(2)},x86_?4:8,Access::Write);if(arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
            const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)fail(8);else {memory.write(GuestAddress{pointer},bytes);runtime_.local_allocations.insert(pointer);memory.store(GuestAddress{arg(2)},x86_?32:64,pointer);if(arg(3))memory.store(GuestAddress{arg(3)},32,bytes.size());result=1;}
        }catch(const SddlError& error){fail(error.code);}}
    }else if(symbol=="NtSetInformationObject"){
        if(object_type(arg(0)).empty() || arg(0)==(x86_?UINT32_MAX:UINT64_MAX) || arg(0)==(x86_?UINT32_MAX-1:UINT64_MAX-1))result=0xc0000008U;
        else if(u32(1)!=4)result=0xc0000003U;
        else if(u32(3)!=2)result=0xc0000004U;
        else try{const auto inherit=memory.load(GuestAddress{arg(2)},8),protect=memory.load(GuestAddress{arg(2)+1},8);if(inherit>1 || protect>1)result=0xc000000dU;else runtime_.handle_flags[arg(0)]=static_cast<std::uint32_t>(inherit|(protect<<1));}catch(const MemoryFault&){result=0xc0000005U;}
    }else if(symbol=="NtQueryObject"){
        const auto type=object_type(arg(0));
        if(u32(1)==3)try{
            // 物件型別目錄與現存控制代碼共用同一份資料；不列入 GUI/GDI 這類 User handle。
            constexpr std::array<std::u16string_view,12> types{u"Process",u"Thread",u"File",u"Token",u"PowerRequest",u"Job",u"IoCompletion",u"Section",u"Key",u"Event",u"Semaphore",u"Timer"};
            std::vector<std::u16string_view> names(types.begin(),types.end());names.push_back(u"Mutant");const unsigned unit=x86_?4:8,header=x86_?96:104;
            std::size_t total=unit;for(auto text:names)total=(total+header+(text.size()+1)*2+unit-1)&~static_cast<std::size_t>(unit-1);
            if(arg(4))memory.store(GuestAddress{arg(4)},32,total);if(u32(3)<total)result=0xc0000004U;
            else {memory.check(GuestAddress{arg(2)},total,Access::Write);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(total));memory.store(GuestAddress{arg(2)},32,names.size());std::size_t at=unit;
                const auto handles=local_process_handles();for(auto text:names){const auto pointer=arg(2)+at;const auto bytes=utf16_bytes(text);memory.store(GuestAddress{pointer},16,bytes.size()-2);memory.store(GuestAddress{pointer+2},16,bytes.size());memory.store(GuestAddress{pointer+(x86_?4:8)},x86_?32:64,pointer+header);memory.write(GuestAddress{pointer+header},bytes);
                    std::set<std::uint64_t> objects;unsigned open=0;for(auto handle:handles)if(object_type(handle)==text){if(const auto info=object_metadata(handle))objects.insert(info->identity);++open;}memory.store(GuestAddress{pointer+(x86_?8:16)},32,objects.size());memory.store(GuestAddress{pointer+(x86_?12:20)},32,open);at=(at+header+bytes.size()+unit-1)&~static_cast<std::size_t>(unit-1);}}
        }catch(const MemoryFault&){result=0xc0000005U;}catch(const std::bad_alloc&){result=0xc0000017U;}
        else if(type.empty())result=0xc0000008U;
        else if(u32(1)==0 || u32(1)==4)try{
            const unsigned size=u32(1)==0?56:2;if(arg(4))memory.store(GuestAddress{arg(4)},32,size);
            if(u32(3)<size)result=0xc0000004U;
            else {memory.check(GuestAddress{arg(2)},size,Access::Write);const auto flags=runtime_.handle_flags.contains(arg(0))?runtime_.handle_flags.at(arg(0)):0;
                if(u32(1)==4){memory.store(GuestAddress{arg(2)},8,flags&1);memory.store(GuestAddress{arg(2)+1},8,(flags>>1)&1);}
                else {const auto info=object_metadata(arg(0));if(!info)result=0xc0000008U;else {memory.write(GuestAddress{arg(2)},std::array<std::uint8_t,56>{});memory.store(GuestAddress{arg(2)},32,((flags&1)?2U:0U)|((flags&2)?1U:0U));memory.store(GuestAddress{arg(2)+4},32,info->access);memory.store(GuestAddress{arg(2)+8},32,info->handles);memory.store(GuestAddress{arg(2)+12},32,info->pointers);}}}
        }catch(const MemoryFault&){result=0xc0000005U;}
        else if(u32(1)!=1 && u32(1)!=2)result=u32(1)<=5?0xc0000002:0xc0000003;
        else try{
            std::u16string text(type);
            if(u32(1)==1){
                text.clear();
                if(const auto found=files_.find(arg(0));found!=files_.end()){
                    const auto& file=found->second;
                    if(file.pipe)text=u"\\Device\\NamedPipe"+file.pipe->file_name();
                    else {
                        auto path=file.path;if(file.native){const auto final=filesystem_.final_path(*file.native,file.path);if(!final.error)path=final.key;}
                        text=utf8_to_utf16(path);std::replace(text.begin(),text.end(),u'/',u'\\');
                        if(text.size()>=3 && text[1]==u':')text=windows_device_name(text[0])+text.substr(2);
                    }
                }else if(const auto wait_found=runtime_.wait_objects.find(arg(0));wait_found!=runtime_.wait_objects.end())text=wait_found->second.name;
                else if(mapping_handle(arg(0)))text=mapping_object_name(arg(0));
                else if(job_handle(arg(0)))text=job_object_name(arg(0));
                else if(const auto key_found=runtime_.registry.handles.find(arg(0));key_found!=runtime_.registry.handles.end()){
                    const auto& path=registry_logical_path(key_found->second);initialize_tokens();const auto sid=utf8_to_utf16(token_user_sid_string(1));
                    if(!path.empty()){
                        text=path[0]==1?u"\\REGISTRY\\USER\\"+sid:path[0]==2?u"\\REGISTRY\\MACHINE":path[0]==3?u"\\REGISTRY\\USER":u"\\REGISTRY\\MACHINE\\Software\\Classes";
                        for(std::size_t start=2;start<path.size();){const auto end=path.find(u'\\',start),stop=end==path.npos?path.size():end;const auto display=runtime_.registry.display_names.find(path.substr(0,stop));text+=u'\\';text+=display==runtime_.registry.display_names.end()?path.substr(start,stop-start):display->second;if(end==path.npos)break;start=end+1;}
                    }
                }
                if(!text.empty() && text.front()!=u'\\'){
                    if(text.starts_with(u"Global\\"))text=u"\\BaseNamedObjects\\"+text.substr(7);
                    else {if(text.starts_with(u"Local\\"))text.erase(0,6);text=u"\\Sessions\\1\\BaseNamedObjects\\"+text;}
                }
            }
            const auto header=u32(1)==2?(x86_?96U:104U):(x86_?8U:16U);
            const auto bytes=text.empty() && u32(1)==1?std::vector<std::uint8_t>{}:utf16_bytes(text);
            if(bytes.size()>UINT16_MAX)result=0xc000000dU;
            else {
                const auto size=static_cast<std::uint32_t>(header+bytes.size());if(arg(4))memory.store(GuestAddress{arg(4)},32,size);
                if(u32(3)<size)result=0xc0000004U;
                else {memory.check(GuestAddress{arg(2)},size,Access::Write);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(header));
                    if(!bytes.empty()){memory.store(GuestAddress{arg(2)},16,bytes.size()-2);memory.store(GuestAddress{arg(2)+2},16,bytes.size());memory.store(GuestAddress{arg(2)+(x86_?4U:8U)},x86_?32:64,arg(2)+header);memory.write(GuestAddress{arg(2)+header},bytes);}
                }
            }
        }catch(const MemoryFault&){result=0xc0000005U;}catch(const std::bad_alloc&){result=0xc0000017U;}
    }else if(symbol=="InitializeSecurityDescriptor"){
        if(!arg(0))fail(87);else if(u32(1)!=1)fail(1305);
        else {memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(x86_?20:40));memory.store(GuestAddress{arg(0)},8,1);result=1;}
    }else if(symbol=="InitializeAcl"){
        if(!arg(0) || u32(1)<8 || u32(1)>65535)fail(87);
        else if(u32(2)!=2 && u32(2)!=4)fail(1305);
        else {memory.write(GuestAddress{arg(0)},std::array<std::uint8_t,8>{});memory.store(GuestAddress{arg(0)},8,u32(2));memory.store(GuestAddress{arg(0)+2},16,u32(1));result=1;}
    }else if (symbol == "IsUserAnAdmin") {
        initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());const auto administrators=sddl_sid("BA");
        result=token_sid_matches(token,administrators,false) && (!token.restriction_check || token_sid_matches(token,administrators,false,true));
    } else if (symbol == "LsaOpenPolicy") {
        // LSA 使用 NTSTATUS，不寫 LastError。虛擬低權限 token 不能開啟系統 policy。
        result = 0xc0000022;
        if (!arg(1) || !arg(3)) result = 0xc000000d;
        else {
            memory.check(GuestAddress{arg(1)}, 48, Access::Read); // x64 LSA_OBJECT_ATTRIBUTES
            memory.check(GuestAddress{arg(3)}, 8, Access::Write);
            if (arg(0)) {
                memory.check(GuestAddress{arg(0)}, 16, Access::Read); // LSA_UNICODE_STRING
                const auto length = memory.load(GuestAddress{arg(0)}, 16);
                const auto capacity = memory.load(GuestAddress{arg(0) + 2}, 16);
                const auto buffer = memory.load(GuestAddress{arg(0) + 8}, 64);
                if ((length & 1) || length > capacity || (length && !buffer)) result = 0xc000000d;
                else if (length) {
                    memory.check(GuestAddress{buffer}, static_cast<std::size_t>(length), Access::Read);
                    result = 0xc00000bb; // 沒有遠端 LSA 後端。
                }
            }
        }
    } else if (symbol == "LsaClose" || symbol == "LsaAddAccountRights") {
        // 目前沒有可取得的 policy handle；token、file、任意整數均不是 LSA handle。
        result = 0xc0000008;
    } else if (symbol == "LsaNtStatusToWinError") {
        result = lsa_winerror(u32(0));
    } else if (symbol == "LookupAccountNameA" || symbol == "LookupAccountNameW") {
        const bool wide = symbol.back() == 'W';
        const auto text = [&](std::uint64_t address) {
            return wide ? utf16_to_utf8(read_utf16(memory, GuestAddress{address}, 1024)) : read_ansi(memory, address, 1024);
        };
        if (!arg(1) || !arg(3) || !arg(5)) fail(87);
        else if (arg(0) && !text(arg(0)).empty()) fail(50);
        else {
            const auto name = text(arg(1));
            const auto found=find_account(name);
            if (!found) fail(1332); // ERROR_NONE_MAPPED
            else {
                const auto sid_size = 8U + 4U * found->count;
                const auto domain_size = static_cast<std::uint32_t>(found->domain.size() + 1);
                const auto sid_capacity = memory.load(GuestAddress{arg(3)}, 32);
                const auto domain_capacity = memory.load(GuestAddress{arg(5)}, 32);
                memory.check(GuestAddress{arg(3)}, 4, Access::Write);
                memory.check(GuestAddress{arg(5)}, 4, Access::Write);
                if (sid_capacity < sid_size || domain_capacity < domain_size) {
                    memory.store(GuestAddress{arg(3)}, 32, sid_size);
                    memory.store(GuestAddress{arg(5)}, 32, domain_size);
                    fail(122);
                } else if (!arg(2) || !arg(4) || !arg(6)) fail(87);
                else {
                    std::vector<std::uint8_t> sid(sid_size);
                    sid[0] = 1; sid[1] = found->count; sid[7] = found->authority;
                    for (std::uint32_t i = 0; i < found->count; ++i)
                        for (unsigned byte = 0; byte < 4; ++byte)
                            sid[8 + i * 4 + byte] = static_cast<std::uint8_t>(found->subauthorities[i] >> (byte * 8));
                    std::vector<std::uint8_t> domain;
                    if (wide) domain = utf16_bytes(utf8_to_utf16(found->domain));
                    else { domain.assign(found->domain.begin(), found->domain.end()); domain.push_back(0); }
                    memory.check(GuestAddress{arg(2)}, sid.size(), Access::Write);
                    memory.check(GuestAddress{arg(4)}, domain.size(), Access::Write);
                    memory.check(GuestAddress{arg(6)}, 4, Access::Write);
                    memory.write(GuestAddress{arg(2)}, sid); memory.write(GuestAddress{arg(4)}, domain);
                    memory.store(GuestAddress{arg(3)}, 32, sid_size);
                    memory.store(GuestAddress{arg(5)}, 32, domain_size - 1);
                    memory.store(GuestAddress{arg(6)}, 32, found->type);
                    result = 1;
                }
            }
        }
    } else if (symbol == "LookupPrivilegeValueA" || symbol == "LookupPrivilegeValueW") {
        const bool wide = symbol.back() == 'W';
        const auto text = [&](std::uint64_t address) {
            return wide ? utf16_to_utf8(read_utf16(memory, GuestAddress{address}, 1024)) : read_ansi(memory, address, 1024);
        };
        if (!arg(1) || !arg(2)) fail(87);
        else if (arg(0) && !text(arg(0)).empty()) fail(50); // 遠端安全性授權服務未映射。
        else {
            const auto name = text(arg(1));
            const auto found = std::find_if(privilege_names.begin(), privilege_names.end(),
                                           [&](const auto candidate) { return same_name(name, candidate); });
            if (found == privilege_names.end()) fail(1313); // ERROR_NO_SUCH_PRIVILEGE
            else {
                memory.check(GuestAddress{arg(2)}, 8, Access::Write);
                memory.store(GuestAddress{arg(2)}, 64, 2 + static_cast<std::size_t>(found - privilege_names.begin()));
                result = 1;
            }
        }
    } else throw HostCallFault("未知的 Windows token 服務");
    arg.return_to(state, result);
    return std::nullopt;
}
namespace {
void token_put(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint64_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(8*i));}
SecurityDescriptor stored_token_descriptor(const std::vector<std::uint8_t>& bytes){
    SecurityDescriptor sd;sd.control=static_cast<std::uint16_t>(bytes.at(2)|(unsigned{bytes.at(3)}<<8));sd.rm_control=bytes.at(1);
    for(unsigned i=0;i<4;++i){std::size_t offset=0;for(unsigned j=0;j<4;++j)offset|=std::size_t{bytes.at(4+i*4+j)}<<(j*8);if(!offset)continue;
        const auto size=i<2?8U+4U*bytes.at(offset+1):unsigned{bytes.at(offset+2)}|(unsigned{bytes.at(offset+3)}<<8);
        if(offset>bytes.size() || size>bytes.size()-offset)throw SddlError{1338};sd.data[i].assign(bytes.data()+offset,bytes.data()+offset+size);
    }return sd;
}

}
std::vector<std::uint8_t> encode_virtual_token(const VirtualToken& token){
    std::vector<std::uint8_t> bytes;
    const auto number=[&](std::uint32_t value){const auto offset=bytes.size();bytes.resize(offset+4);token_put(bytes,offset,value);};
    const auto blob=[&](const std::vector<std::uint8_t>& value){number(static_cast<std::uint32_t>(value.size()));bytes.insert(bytes.end(),value.begin(),value.end());};
    number(0x354b5458);number(token.type);number(token.level);number(token.integrity);number(token.session);
    number(static_cast<std::uint32_t>(token.application_model));
    number((token.restriction_check?1U:0U)|(token.write_restricted?2U:0U)|(token.sandbox_inert?4U:0U)|(token.virtualization_enabled?8U:0U)|(token.child_process_restricted?16U:0U));
    number(token.user.attributes);blob(token.user.sid);blob(token.owner);blob(token.primary_group);blob(token.default_dacl);blob(token.object_descriptor);
    for(const auto* list:{&token.groups,&token.restricted}){number(static_cast<std::uint32_t>(list->size()));for(const auto& item:*list){number(item.attributes);blob(item.sid);}}
    {for(const auto value:{token.authentication_id,token.source_id,token.expires}){number(static_cast<std::uint32_t>(value));number(static_cast<std::uint32_t>(value>>32));}bytes.insert(bytes.end(),token.source.begin(),token.source.end());}
    number(static_cast<std::uint32_t>(token.privileges.size()));
    for(const auto& privilege:token.privileges){number(static_cast<std::uint32_t>(privilege.luid));number(static_cast<std::uint32_t>(privilege.luid>>32));number(privilege.attributes);}
    number(static_cast<std::uint32_t>(token.modified_id));number(static_cast<std::uint32_t>(token.modified_id>>32));
    return bytes;
}
std::optional<VirtualToken> decode_virtual_token(std::span<const std::uint8_t> bytes){
    const auto parse=[&](bool legacy_lsa)->std::optional<VirtualToken>{
    try {
        if(bytes.size()>262144)throw SddlError{13};std::size_t cursor=0;
        const auto number=[&](){if(bytes.size()-cursor<4)throw SddlError{13};std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[cursor++]}<<(i*8);return value;};
        const auto blob=[&](){const auto size=number();if(size>bytes.size()-cursor)throw SddlError{13};std::vector<std::uint8_t> value(bytes.data()+cursor,bytes.data()+cursor+size);cursor+=size;return value;};
        const auto valid_sid=[](const std::vector<std::uint8_t>& sid){return sid.size()>=8 && sid[0]==1 && sid[1]<=15 && sid.size()==8U+4U*sid[1];};
        const auto version=number();if(version!=0x314b5458 && version!=0x324b5458 && version!=0x334b5458 && version!=0x344b5458 && version!=0x354b5458)throw SddlError{13};VirtualToken token;token.type=number();token.level=number();token.integrity=number();token.session=number();
        const auto model=version!=0x314b5458 && !legacy_lsa?number():legacy_lsa?0U:1U;if(model>1)throw SddlError{13};token.application_model=static_cast<ApplicationModel>(model);const auto flags=number();
        if((token.type!=1 && token.type!=2) || token.level>3 || token.integrity>0x5000 || (flags&~(version>=0x334b5458?31U:15U)))throw SddlError{13};
        token.restriction_check=flags&1;token.write_restricted=flags&2;token.sandbox_inert=flags&4;token.virtualization_enabled=flags&8;
        token.child_process_restricted=flags&16;
        token.user.attributes=number();token.user.sid=blob();token.owner=blob();token.primary_group=blob();token.default_dacl=blob();token.object_descriptor=blob();
        if(!valid_sid(token.user.sid) || !valid_sid(token.owner) || !valid_sid(token.primary_group) || token.object_descriptor.size()<20)throw SddlError{13};
        for(auto* list:{&token.groups,&token.restricted}){const auto count=number();if(count>1024)throw SddlError{13};for(unsigned i=0;i<count;++i){const auto attributes=number();auto sid=blob();if(!valid_sid(sid))throw SddlError{13};list->push_back({std::move(sid),attributes});}}
        if(version>=0x344b5458 || (version==0x324b5458 && legacy_lsa)){const auto wide=[&](){const auto low=number(),high=number();return std::uint64_t{low}|(std::uint64_t{high}<<32);};token.authentication_id=wide();token.source_id=wide();token.expires=wide();if(bytes.size()-cursor<8)throw SddlError{13};std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(cursor),8,token.source.begin());cursor+=8;}
        if(version==0x354b5458){
            const auto count=number();if(count>privilege_names.size())throw SddlError{13};std::set<std::uint64_t> seen;
            for(unsigned i=0;i<count;++i){const auto low=number(),high=number(),attributes=number();const auto luid=std::uint64_t{low}|(std::uint64_t{high}<<32);
                if(luid<2 || luid>=2+privilege_names.size() || (attributes&~0x80000003U) || !seen.insert(luid).second)throw SddlError{13};token.privileges.push_back({luid,attributes});}
            const auto low=number(),high=number();token.modified_id=std::uint64_t{low}|(std::uint64_t{high}<<32);
        }
        if(cursor!=bytes.size())throw SddlError{13};(void)stored_token_descriptor(token.object_descriptor);return token;
    }catch(const SddlError&){return {};}catch(const std::out_of_range&){return {};}
    };
    if(auto token=parse(false))return token;
    if(bytes.size()>=4 && bytes[0]==0x58 && bytes[1]==0x54 && bytes[2]==0x4b && bytes[3]==0x32)return parse(true);
    return {};
}
std::pair<std::vector<std::uint8_t>,std::uint32_t> WindowsServices::pipe_client_token(std::uint32_t flags){
    initialize_tokens();
    if((flags&0x100000U) && (flags&0x40000U))return {{},50}; // 動態追蹤須隨每次訊息傳遞權杖，尚未接入。
    auto token=runtime_.token_objects.at(effective_token_id());const auto level=(flags&0x100000U)?(flags>>16)&3U:2U;
    if(token.type==2 && level>token.level)return {{},1346};token.type=2;token.level=level;
    return {encode_virtual_token(token),0};
}
std::pair<std::vector<std::uint8_t>,std::uint32_t> WindowsServices::process_creation_token(std::uint64_t handle,unsigned privilege){
    initialize_tokens();std::uint64_t id=1;
    if(handle){const auto found=runtime_.token_handles.find(handle);if(found==runtime_.token_handles.end())return {{},6};if((found->second&0xbU)!=0xbU)return {{},5};id=runtime_.token_handle_objects.at(handle);}
    const auto& token=runtime_.token_objects.at(id);if(token.type!=1)return {{},1349};
    if(token.user.sid!=runtime_.token_objects.at(1).user.sid)return {{},1314};
    if(token.integrity>runtime_.token_objects.at(1).integrity)return {{},1314};
    if(!privilege)return {encode_virtual_token(token),0};
    // 僅 Shell runas／客體 SCM 可要求此分支；不變更宿主 UID 或執行 macOS 提權。
    if(privilege>2 || handle || effective_token_id()!=1 || !filesystem_.writable() ||
       token.integrity<0x2000 || token.restriction_check || !token.restricted.empty() ||
       (token.user.attributes&0x10U) || std::any_of(token.groups.begin(),token.groups.end(),[](const auto& group){return group.attributes&0x10U;}))return {{},1314};
    auto elevated=token;elevated.integrity=std::max(token.integrity,privilege==2?0x4000U:0x3000U);
    const auto admins=sddl_sid("BA");
    if(const auto group=std::find_if(elevated.groups.begin(),elevated.groups.end(),[&](const auto& item){return item.sid==admins;});group!=elevated.groups.end())group->attributes|=7U;else elevated.groups.push_back({admins,7});
    if(privilege==2){elevated.user.sid=sddl_sid("SY");elevated.owner=elevated.user.sid;elevated.primary_group=admins;elevated.session=0;}
    // 權限只由既有 Sandbox broker 產生；一般使用者與 restricted token 不取得新 privilege。
    static constexpr unsigned administrator_privileges[]{4,5,8,9,10,11,12,13,14,15,17,18,19,20,22,23,24,28,29,30,33,34,35,36};
    for(unsigned luid=2;luid<2+privilege_names.size();++luid){
        if(privilege!=2 && std::find(std::begin(administrator_privileges),std::end(administrator_privileges),luid)==std::end(administrator_privileges))continue;
        if(std::none_of(elevated.privileges.begin(),elevated.privileges.end(),[&](const auto& item){return item.luid==luid;}))elevated.privileges.push_back({luid,0});
    }
    elevated.default_dacl=encode_acl({encode_ace({0,0,0x10000000U,elevated.user.sid}),encode_ace({0,0,0x10000000U,admins})});
    SecurityDescriptor sd;sd.control=4;sd.data[0]=elevated.owner;sd.data[1]=elevated.primary_group;sd.data[3]=elevated.default_dacl;elevated.object_descriptor=relative_descriptor(sd);
    return {encode_virtual_token(elevated),0};
}
std::vector<std::uint8_t> WindowsServices::process_startup_blob(const std::vector<std::uint8_t>& token,const std::vector<std::uint8_t>& process,const std::vector<std::uint8_t>& thread){
    if(token.size()>262144 || process.size()>131072 || thread.size()>131072)throw SddlError{8};
    const auto decoded=decode_virtual_token(token);if(!decoded)throw SddlError{13};
    const auto privilege=decoded->user.sid==sddl_sid("SY")?2U:decoded->integrity>0x2000U?1U:0U;
    std::vector<std::uint8_t> bytes(20);token_put(bytes,0,0x32535058U);token_put(bytes,4,static_cast<std::uint32_t>(token.size()));token_put(bytes,8,static_cast<std::uint32_t>(process.size()));token_put(bytes,12,static_cast<std::uint32_t>(thread.size()));token_put(bytes,16,privilege);
    for(const auto* part:{&token,&process,&thread})bytes.insert(bytes.end(),part->begin(),part->end());return bytes;
}
void WindowsServices::apply_manifest_elevation(const std::filesystem::path& executable){
    process_manifest::Level level{};
    try{level=process_manifest::executable(executable);}catch(...){return;} // manifest 錯誤由一般載入與 CreateProcess 回報。
    // 虛擬使用者可經 broker 免帳密提升，相當於 UAC 管理員；highestAvailable 與 requireAdministrator 同樣在啟動時取得提升權杖。
    if(level!=process_manifest::Level::Administrator && level!=process_manifest::Level::HighestAvailable)return;
    initialize_tokens();
    // 與 Shell runas 同一 broker：需可寫映射，否則維持一般權杖，程式會像未同意 UAC 時一樣取得拒絕存取。
    const auto [bytes,error]=process_creation_token(0,1);
    const auto token=error?std::nullopt:decode_virtual_token(bytes);
    std::fprintf(stderr,"manifest_elevation=%s\n",token?"administrator":"denied");
    if(!token)return;
    runtime_.token_objects[1]=*token;refresh_file_security_context();
}
void WindowsServices::import_primary_token(int descriptor){
#if !defined(_WIN32)
    struct stat info{};if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size<=0 || info.st_size>524308)throw HostCallFault("子程序權杖資料無效");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));std::size_t offset=0;
    while(offset<bytes.size()){const auto read=::pread(descriptor,bytes.data()+offset,bytes.size()-offset,static_cast<off_t>(offset));if(read<0 && errno==EINTR)continue;if(read<=0)throw HostCallFault("子程序權杖資料截斷");offset+=static_cast<std::size_t>(read);}
    std::vector<std::uint8_t> process_sd,thread_sd;unsigned privilege=0;
    if(bytes.size()>=16 && bytes[0]==0x58 && bytes[1]==0x50 && bytes[2]==0x53 && (bytes[3]==0x31 || bytes[3]==0x32)){
        const auto length=[&](unsigned at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(i*8);return value;};
        const auto header=bytes[3]==0x32?20U:16U;if(bytes.size()<header)throw HostCallFault("子程序啟動安全標頭截斷");if(header==20)privilege=length(16);
        const auto token_size=length(4),process_size=length(8),thread_size=length(12);
        if(token_size>262144 || process_size<20 || process_size>131072 || thread_size<20 || thread_size>131072 || std::uint64_t{header}+token_size+process_size+thread_size!=bytes.size() || privilege>2)throw HostCallFault("子程序啟動安全資料截斷");
        const auto at=bytes.begin()+header+token_size;process_sd.assign(at,at+process_size);thread_sd.assign(at+process_size,bytes.end());bytes.erase(bytes.begin()+header+token_size,bytes.end());bytes.erase(bytes.begin(),bytes.begin()+header);
        try{(void)security::parse_relative_descriptor(process_sd);(void)security::parse_relative_descriptor(thread_sd);}catch(const SddlError&){throw HostCallFault("子程序物件安全描述元無效");}
    }
    auto token=decode_virtual_token(bytes);if(!token || token->type!=1)throw HostCallFault("子程序 primary token 格式無效");
    initialize_tokens();const auto original=runtime_.token_objects.at(1);
    const bool broker_token=privilege && !token->restriction_check && token->restricted.empty() &&
        std::any_of(token->groups.begin(),token->groups.end(),[&](const auto& group){return group.sid==sddl_sid("BA") && (group.attributes&0x14U)==4U;});
    if(token->user.sid!=original.user.sid && !(broker_token && privilege==2 && token->user.sid==sddl_sid("SY")))throw HostCallFault("跨使用者 primary token 尚未支援");
    if(token->integrity>original.integrity && !(broker_token && token->integrity<=(privilege==2?0x4000U:0x3000U)))throw HostCallFault("子程序 primary token 不可提高完整性");
    if(!broker_token)for(const auto& item:token->privileges){
        if(!original.has_privilege(item.luid) || ((item.attributes&2U) && !original.has_privilege(item.luid,true)))throw HostCallFault("子程序 primary token 不可增加 privilege");
    }
    runtime_.token_objects[1]=std::move(*token);
    if(const auto error=process_creation_token(0).second){runtime_.token_objects[1]=original;throw HostCallFault("子程序權杖限制尚未支援："+std::to_string(error));}
    runtime_.child_process_policy=runtime_.token_objects.at(1).child_process_restricted?1U:0U;
    if(!process_sd.empty()){runtime_.process_security_descriptor=std::move(process_sd);runtime_.initial_thread_security_descriptor=std::move(thread_sd);}
    if(threads_.items.contains(1))threads_.items.at(1).security_descriptor=runtime_.initial_thread_security_descriptor;
    refresh_file_security_context();
#else
    (void)descriptor;throw HostCallFault("此平台未支援子程序權杖");
#endif
}
void WindowsServices::initialize_tokens(){
    if(runtime_.token_objects.contains(1))return;
    VirtualToken token;
    // 穩定的虛擬本機使用者，不借用 macOS root／管理員身分。
    // run-pe／CreateProcess 執行一般 Win32 PE；尚未提供 MSIX／UWP 啟動。
    token.application_model=ApplicationModel::ClassicDesktop;
    token.user.sid=sddl_sid("S-1-5-21-1000-1000-1000-1000");
    for(const auto sid:{"S-1-1-0","S-1-5-11","S-1-5-4","S-1-5-32-545"})token.groups.push_back({sddl_sid(sid),7});
    // 與 TokenStatistics 的虛擬 AuthenticationId=1 共用登入工作階段；子程序及複製權杖保留此 SID。
    token.groups.push_back({sddl_sid("S-1-5-5-0-1"),0xc0000007U});
    token.owner=token.user.sid;token.primary_group=sddl_sid("S-1-5-32-545");
    // 初始互動式權杖的新物件預設授權使用者與 LocalSystem；自訂 DACL 仍由原存取檢查裁決。
    token.default_dacl=security::encode_acl({encode_ace({0,0,0x10000000U,token.user.sid}),
        encode_ace({0,0,0x10000000U,sddl_sid("S-1-5-18")})});
    SecurityDescriptor sd;sd.control=4;sd.data[0]=token.owner;sd.data[1]=token.primary_group;sd.data[3]=token.default_dacl;
    token.object_descriptor=relative_descriptor(sd);runtime_.process_security_descriptor=token.object_descriptor;runtime_.initial_thread_security_descriptor=token.object_descriptor;runtime_.token_objects.emplace(1,std::move(token));
}
void WindowsServices::refresh_file_security_context(){
    initialize_tokens();
    if(!runtime_.network.external_io){shared_tokens_.clear();shared_token_revisions_.clear();}
    else if(!mutating_shared_token_)for(const auto& [id,token]:shared_tokens_)token->refresh(runtime_.token_objects.at(id),shared_token_revisions_[id]);
    const auto& primary=runtime_.token_objects.at(1);
    filesystem_.set_security_context(runtime_.token_objects.at(effective_token_id()),primary.user.sid,primary.primary_group);
}
std::uint32_t WindowsServices::kernel_security_access(const std::vector<std::uint8_t>& bytes,std::uint32_t desired,bool thread)const{
    if(bytes.empty())throw SddlError{1338};
    const auto& mapping=thread?security::thread_access_mapping:security::process_access_mapping;
    auto granted=security::object_access(runtime_.token_objects.at(effective_token_id()),security::parse_relative_descriptor(bytes),desired,mapping);
    if(thread){if(granted&0x40U)granted|=0x800U;if(granted&0x20U)granted|=0x400U;}
    else if(granted&0x400U)granted|=0x1000U;
    return granted;
}
std::uint32_t WindowsServices::created_thread_pseudo_access(const std::vector<std::uint8_t>& bytes)const{
    if(!threads_.items.at(threads_.current).impersonation_token)return 0x1fffffU;
    try{return security::object_access(runtime_.token_objects.at(1),security::parse_relative_descriptor(bytes),0x02000000U,security::thread_access_mapping);}
    catch(const SecurityDescriptorError& error){if(error.code==5)return 0;throw;}
}
std::string WindowsServices::token_user_sid_string(std::uint64_t id) const {return sid_string(runtime_.token_objects.at(id).user.sid);}
std::uint32_t WindowsServices::native_file_token_access(const security::SecurityDescriptor& descriptor,std::uint32_t desired) {
    initialize_tokens();const auto& primary=runtime_.token_objects.at(1);
    return security::file_token_access({runtime_.token_objects.at(effective_token_id()),primary.user.sid,primary.primary_group},descriptor,desired);
}

SynchronizationDescriptor WindowsServices::synchronization_descriptor(const GuestMemory& memory,std::uint64_t address,bool check_length){
    auto security=read_security_attributes(memory,address,x86_,true,check_length);
    return {security.inherit,object_security_descriptor(std::move(security.descriptor),true,true)};
}
std::vector<std::uint8_t> WindowsServices::file_creation_descriptor(const GuestMemory& memory,std::uint64_t address){
    try{
        auto sd=read_security_attributes(memory,address,x86_).descriptor;const bool dacl_present=sd.control&4U;
        sd=security::parse_relative_descriptor(object_security_descriptor(std::move(sd),true,false));
        // 父層繼承先於 token 的預設 DACL；驗證時補入的 ACL 不可變成明示 ACL。
        if(!dacl_present){sd.control&=static_cast<std::uint16_t>(~4U);sd.data[3].clear();}
        return security::relative_descriptor(sd);
    }catch(const MemoryFault&){throw SecurityDescriptorError{1338};}
}
std::vector<std::uint8_t> WindowsServices::object_security_descriptor(SecurityDescriptor sd,bool check_owner,bool default_label){
    initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
    if(sd.data[0].empty())sd.data[0]=token.owner;
    else if(check_owner && sd.data[0]!=token.user.sid && std::none_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==sd.data[0] && (group.attributes&8U);}))throw SddlError{1307};
    if(sd.data[1].empty())sd.data[1]=token.primary_group;
    if(!(sd.control&4)){sd.control|=4;sd.data[3]=token.default_dacl;}
    if(default_label && sd.data[2].empty() && token.integrity!=0x2000){sd.control|=16;sd.data[2]=encode_acl({encode_ace({0x11,0,1,sddl_sid("S-1-16-"+std::to_string(token.integrity))})});}
    if(!default_label && (sd.control&16U) && sd.data[2].empty() && token.integrity<0x2000)throw SddlError{1314};
    if(!sd.data[3].empty()){
        const auto acl=parse_acl(sd.data[3]);for(std::size_t i=0;i<acl.offsets.size();++i){const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;const auto ace=basic_ace(std::span{acl.bytes}.subspan(at,end-at));if(ace.type>1 || (ace.flags&~31U))throw SddlError{50};}
    }
    if(!sd.data[2].empty()){
        const auto acl=parse_acl(sd.data[2]);
        if(acl.offsets.size()>1)throw SddlError{1336};
        for(const auto at:acl.offsets){
            if(acl.bytes[at]!=0x11)throw SddlError{1314};
            if((acl.bytes[at+1]&~31U) || acl.bytes[at+2]!=20 || acl.bytes[at+3] || !std::equal(acl.bytes.data()+at+8,acl.bytes.data()+at+16,sddl_sid("S-1-16-0").begin()))throw SddlError{1336};
            const auto mask=std::uint32_t{acl.bytes[at+4]}|(std::uint32_t{acl.bytes[at+5]}<<8)|(std::uint32_t{acl.bytes[at+6]}<<16)|(std::uint32_t{acl.bytes[at+7]}<<24);
            const auto level=std::uint32_t{acl.bytes[at+16]}|(std::uint32_t{acl.bytes[at+17]}<<8)|(std::uint32_t{acl.bytes[at+18]}<<16)|(std::uint32_t{acl.bytes[at+19]}<<24);
            if(mask&~7U || level>token.integrity || (level&0xfffU))throw SddlError{1314};
        }
    }
    return relative_descriptor(sd);
}
std::optional<std::uint32_t> WindowsServices::invoke_object_security(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto api=service.symbol;
    const bool named=api.starts_with("GetNamedSecurityInfo") || api.starts_with("SetNamedSecurityInfo");
    const bool info=named || api=="GetSecurityInfo" || api=="SetSecurityInfo",setting=api.starts_with("Set");
    const bool user=api=="GetUserObjectSecurity" || api=="SetUserObjectSecurity";
    const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    std::uint32_t result=setting?(info?0:1):0;
    try{
        if(user && !call(1))throw SddlError{87};
        const auto flags=static_cast<std::uint32_t>(user?memory.load(GuestAddress{call(1)},32):call(info?2:1));
        if(!flags || flags&~(setting?0xa000001fU:0x1fU) || (flags&0xa0000000U)==0xa0000000U)throw SddlError{87};
        if(flags&8U)throw SddlError{1314};
        const auto needed=setting?((flags&3U?0x80000U:0U)|(flags&0xa0000004U?0x40000U:0U)|(flags&16U?0x80000U:0U)):0x20000U;
        bool thread_security=false;
        const auto operation=[&](int native,std::vector<std::uint8_t>* local,bool by_name){
        auto descriptor=native>=0?shared_object_security(native,thread_security):*local;
        if(descriptor.empty()){if(by_name || (files_.contains(call(0)) && files_.at(call(0)).pipe))throw SddlError{1338};descriptor=synchronization_descriptor(memory,0).bytes;}
        if(by_name)(void)object_security_access(descriptor,needed,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
        GuestMemory input;input.map(GuestAddress{0x10000},descriptor.size(),read_write,descriptor);
        auto sd=read_descriptor(input,0x10000,false,true);
        if(setting){
            SecurityDescriptor next;
            if(info){if(flags&1U)next.data[0]=read_sid(memory,call(3));if(flags&2U)next.data[1]=read_sid(memory,call(4));if(flags&4U){next.control|=4;if(call(5))next.data[3]=read_acl(memory,call(5)).bytes;}if(flags&16U){next.control|=16;if(call(6))next.data[2]=read_acl(memory,call(6)).bytes;}}
            else {if(!call(2))throw SddlError{87};next=read_descriptor(memory,call(2),x86_,true);if((flags&4U) && !(next.control&4U))throw SddlError{1338};}
            if((flags&1U && next.data[0].empty()) || (flags&2U && next.data[1].empty()))throw SddlError{1337};
            if(flags&1U){sd.data[0]=next.data[0];sd.control&=static_cast<std::uint16_t>(~1U);}if(flags&2U){sd.data[1]=next.data[1];sd.control&=static_cast<std::uint16_t>(~2U);}
            if(flags&4U){sd.control|=4;sd.data[3]=next.data[3];}
            if(flags&16U){sd.control|=16;sd.data[2]=next.data[2];}
            if(flags&0x80000000U)sd.control|=0x1000U;if(flags&0x20000000U)sd.control&=static_cast<std::uint16_t>(~0x1000U);
            auto bytes=object_security_descriptor(std::move(sd),(flags&1U)!=0,(flags&16U)==0);
            if(native>=0)set_shared_object_security(native,bytes,thread_security);else *local=std::move(bytes);
        }else{
            if(!(flags&1U))sd.data[0].clear();if(!(flags&2U))sd.data[1].clear();
            if(!(flags&16U)){sd.data[2].clear();sd.control&=static_cast<std::uint16_t>(~0x30U);}
            if(!(flags&4U)){sd.data[3].clear();sd.control&=static_cast<std::uint16_t>(~12U);}
            const auto bytes=relative_descriptor(sd);
            if(info){
                if(!call(7))throw SddlError{87};for(unsigned i=3;i<=7;++i)if(call(i))memory.check(GuestAddress{call(i)},unit,Access::Write);
                const auto output=allocate_heap(memory,bytes.size());if(!output)throw SddlError{8};memory.write(GuestAddress{output},bytes);runtime_.local_allocations.insert(output);
                for(unsigned i=0;i<4;++i){const auto argument=i==2?6U:i==3?5U:i+3;if(!call(argument))continue;std::uint32_t at{};for(unsigned j=0;j<4;++j)at|=std::uint32_t{bytes[4+i*4+j]}<<(j*8);memory.store(GuestAddress{call(argument)},bits,at?output+at:0);}
                memory.store(GuestAddress{call(7)},bits,output);
            }else{
                if(!call(4))throw SddlError{87};memory.store(GuestAddress{call(4)},32,bytes.size());if(!call(2) || call(3)<bytes.size())throw SddlError{122};if(user && call(2)%4)throw SddlError{87};memory.write(GuestAddress{call(2)},bytes);result=1;
            }
        }
        };
        if(named){
            if(call(1)!=1)throw SddlError{50};if(!call(0))throw SddlError{87};
            if(!runtime_.network.external_io)throw SddlError{50};
            const auto name=api.ends_with('W')?read_utf16(memory,GuestAddress{call(0)},256):utf8_to_utf16(read_ansi(memory,call(0),256));
            const auto error=NamedPipe::security(mapping_session(),name,[&](int fd){operation(fd,nullptr,true);});if(error)throw SddlError{error};
        }else{
        std::vector<std::uint8_t>* local{};int native=-1,lock=-1;
        if(user || (info && call(1)==7)){
            const auto handle=gui_.user_handles.find(call(0));if(handle==gui_.user_handles.end())throw SddlError{6};
            if((handle->second.access&needed)!=needed)throw SddlError{5};
            local=&gui_.user_objects.at(handle->second.object).security_descriptor;
        }else if(!process_object_type(call(0)).empty() || thread_id(call(0))){
            const auto error=process_security_location(call(0),needed,local,native,lock,thread_security);if(error)throw SddlError{error};
        }else if(mapping_handle(call(0)) || job_handle(call(0))){
            const auto error=mapping_handle(call(0))?mapping_security_location(call(0),needed,local,native,lock):job_security_location(call(0),needed,native,lock);if(error)throw SddlError{error};
        }else if(files_.contains(call(0)) && files_.at(call(0)).pipe){
            const auto& file=files_.at(call(0));if((file.access&needed)!=needed)throw SddlError{5};
            native=file.pipe->descriptor();lock=file.pipe->lock_descriptor();
        }else{
            const auto found=runtime_.wait_objects.find(call(0));if(found==runtime_.wait_objects.end())throw SddlError{6};
            auto& handle=found->second;if(handle.kind==WaitObject::Kind::ChangeNotification)throw SddlError{50};
            if((handle.access&needed)!=needed)throw SddlError{5};
            if(handle.kind==WaitObject::Kind::Mutex){auto& mutex=runtime_.mutex_objects.at(handle.mutex);if(mutex.shared){native=mutex.shared->descriptor();lock=mutex.shared->lock_descriptor();}else local=&mutex.security_descriptor;}
            else {auto& object=runtime_.wait_state(handle);if(object.shared_event){native=object.shared_event->descriptor();lock=object.shared_event->lock_descriptor();}else local=&object.security_descriptor;}
        }
        SharedEventLock object_lock(lock);
        operation(native,local,false);
        }
    }catch(const SddlError& error){if(info)result=error.code;else {last_error_=error.code;result=0;}}
    catch(const MemoryFault&){if(info)result=998;else {last_error_=998;result=0;}}
    catch(const std::bad_alloc&){if(info)result=8;else {last_error_=8;result=0;}}
    call.return_to(state,result);return std::nullopt;
}
std::uint32_t WindowsServices::synchronization_access(const std::vector<std::uint8_t>& bytes,std::uint32_t desired,std::uint32_t all){
    const std::array<std::uint32_t,4> mapping{0x20001U,all==0x1f0001U?0x20001U:0x20002U,0x120000U,all};
    return object_security_access(bytes,desired,mapping);
}
std::uint32_t WindowsServices::object_security_access(const std::vector<std::uint8_t>& bytes,std::uint32_t desired,const std::array<std::uint32_t,4>& mapping){
    initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
    const auto descriptor=bytes.empty()?synchronization_descriptor(GuestMemory{},0).bytes:bytes;
    return security::object_access(token,security::parse_relative_descriptor(descriptor),desired,mapping);
}

std::uint64_t WindowsServices::effective_token_id() const {
    const auto thread=threads_.items.find(threads_.current);
    return thread!=threads_.items.end() && thread->second.impersonation_token?thread->second.impersonation_token:1;
}
void WindowsServices::collect_tokens(){
    std::set<std::uint64_t> live{1};
    for(const auto& [handle,id]:runtime_.token_handle_objects){(void)handle;live.insert(id);}
    for(const auto& [id,thread]:threads_.items){(void)id;if(thread.status!=ThreadStatus::Exited)live.insert(thread.impersonation_token);}
    std::erase_if(runtime_.token_objects,[&](const auto& entry){return !live.contains(entry.first);});
    std::erase_if(runtime_.lsa.sessions,[&](const auto& entry){return std::none_of(runtime_.token_objects.begin(),runtime_.token_objects.end(),[&](const auto& token){return token.second.authentication_id==entry.first;});});
    std::erase_if(shared_tokens_,[&](const auto& entry){return !live.contains(entry.first);});
    std::erase_if(shared_token_revisions_,[&](const auto& entry){return !live.contains(entry.first);});
}
std::shared_ptr<SharedToken> WindowsServices::share_token(std::uint64_t id){
    if(!runtime_.network.external_io)throw SddlError{50};
    if(const auto found=shared_tokens_.find(id);found!=shared_tokens_.end())return found->second;
    auto token=SharedToken::create(runtime_.token_objects.at(id));shared_tokens_.emplace(id,token);return token;
}
std::uint64_t WindowsServices::attach_shared_token(const std::shared_ptr<SharedToken>& token){
    for(const auto& [id,existing]:shared_tokens_)if(existing->identity()==token->identity()){runtime_.token_objects.at(id)=existing->read();return id;}
    const auto value=token->read();const auto id=runtime_.next_token_object++;
    runtime_.token_objects.emplace(id,value);shared_tokens_.emplace(id,token);return id;
}
std::uint32_t WindowsServices::local_token_operation(std::uint32_t operation,int descriptor,std::uint32_t desired,std::uint32_t& granted,int& output){
    try{
        refresh_file_security_context();if(operation>2)return 87;
        const auto thread=threads_.items.find(1);if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited || thread->second.terminating)return 87;
        if(operation==0){
            std::uint64_t id=0;
            if(descriptor>=0){auto shared=SharedToken::inherit(descriptor);const auto value=shared->read();if(value.type!=2)return 1349;if(value.level<2)return 1346;
                if(value.user.sid!=runtime_.token_objects.at(1).user.sid && value.user.sid!=sddl_sid("S-1-5-7"))return 1314;
                id=attach_shared_token(shared);}
            thread->second.impersonation_token=id;collect_tokens();if(threads_.current==1)refresh_file_security_context();return 0;
        }
        if(descriptor<0)return 13;
        const auto caller=SharedToken::inherit(descriptor)->read();if(caller.type==2 && caller.level<2)return 1346;
        const auto id=operation==1?1:thread->second.impersonation_token;if(!id)return 1008;
        const auto& token=runtime_.token_objects.at(id);if(operation==2 && !token.level)return 1347;
        const auto mapped=token_access(desired&~0x02000000U);if(mapped&~token_all_access)return 5;
        const auto wanted=mapped|(desired&0x02000000U);
        granted=token_acl_access(caller,stored_token_descriptor(token.object_descriptor),wanted,{0x20008,0x200e0,0x20000,token_all_access})&token_all_access;
        if((mapped&granted)!=mapped || (wanted && !granted))return 5;
        output=share_token(id)->descriptor();return 0;
    }catch(const SddlError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::optional<std::uint32_t> WindowsServices::invoke_token(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const unsigned unit=x86_?4:8,stride=x86_?8:16;const std::uint8_t bits=x86_?32:64;
    std::uint64_t result=0;initialize_tokens();
    const auto token_for=[&](std::uint64_t handle,std::uint32_t rights)->VirtualToken&{
        const auto access=runtime_.token_handles.find(handle);if(access==runtime_.token_handles.end())throw SddlError{6};
        if((access->second&rights)!=rights)throw SddlError{5};return runtime_.token_objects.at(runtime_.token_handle_objects.at(handle));
    };
    const auto new_handle=[&](std::uint64_t id,std::uint32_t rights,std::uint64_t output,bool inherit=false){
        if(!output)throw SddlError{87};memory.check(GuestAddress{output},unit,Access::Write);
        if(runtime_.token_handles.size()>=1024)throw SddlError{8};const auto handle=runtime_.allocate_wait_handle();
        runtime_.token_handles.emplace(handle,rights);runtime_.token_handle_objects.emplace(handle,id);runtime_.handle_flags[handle]=inherit?1:0;
        memory.store(GuestAddress{output},bits,handle);result=1;
    };
    const auto mapping_at=[&](std::uint64_t address){
        if(!address)throw SddlError{87};memory.check(GuestAddress{address},16,Access::Read);std::array<std::uint32_t,4> mapping{};
        for(unsigned i=0;i<4;++i)mapping[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{address+i*4},32));return mapping;
    };
    const auto granted_token_access=[&](std::uint64_t id,std::uint32_t desired,std::uint64_t caller){
        const auto mapped=token_access(desired&~0x02000000U);if(mapped&~token_all_access)throw SddlError{5};
        const auto wanted=mapped|(desired&0x02000000U);const auto& object=runtime_.token_objects.at(id);
        const auto granted=token_acl_access(runtime_.token_objects.at(caller),stored_token_descriptor(object.object_descriptor),wanted,{0x20008,0x200e0,0x20000,token_all_access})&token_all_access;
        if((mapped&granted)!=mapped || (wanted && !granted))throw SddlError{5};return granted;
    };
    const auto install=[&](std::uint64_t thread,std::uint64_t id){
        if(!thread)throw SddlError{6};
        if(id){const auto& token=runtime_.token_objects.at(id);if(token.type!=2)throw SddlError{1349};if(token.level<2)throw SddlError{1346};
            // 外來使用者尚未整合宿主身分切換，不可安裝後沿用本機使用者權限；匿名 token 沿用既有 ACL 限制流程。
            if(token.user.sid!=runtime_.token_objects.at(1).user.sid && token.user.sid!=sddl_sid("S-1-5-7"))throw SddlError{1314};
        }
        threads_.items.at(thread).impersonation_token=id;collect_tokens();if(thread==threads_.current)refresh_file_security_context();result=1;
    };
    std::unique_ptr<SharedToken::Transaction> mutation;
    VirtualToken original;
    try {
        const bool mutating_api=symbol=="SetTokenInformation" || symbol=="SetSecurityInfo" || symbol=="SetKernelObjectSecurity" || symbol=="AdjustTokenPrivileges";
        if(mutating_api && runtime_.token_handle_objects.contains(arg(0))){
            const auto id=runtime_.token_handle_objects.at(arg(0));const auto shared=shared_tokens_.find(id);
            if(shared!=shared_tokens_.end()){
                mutation=std::make_unique<SharedToken::Transaction>(*shared->second);original=mutation->read();
                runtime_.token_objects.at(id)=original;mutating_shared_token_=id;
            }
        }
        if(symbol=="GetSecurityInfo" || symbol=="SetSecurityInfo" || symbol=="GetKernelObjectSecurity" || symbol=="SetKernelObjectSecurity"){
            const bool info=symbol=="GetSecurityInfo" || symbol=="SetSecurityInfo",setting=symbol.starts_with("Set");
            const auto flags=u32(info?2:1);
            if(!setting && !info && arg(4))memory.store(GuestAddress{arg(4)},32,0);
            if(info && u32(1)!=6)throw SddlError{50};if(!flags || (flags&~0xa000000fU))throw SddlError{50};
            if(flags&8U)throw SddlError{1314};
            if(!runtime_.token_handles.contains(arg(0))){
                if(!setting && !info && arg(4))memory.store(GuestAddress{arg(4)},32,0);
                throw SddlError{object_type(arg(0)).empty()?6U:50U};
            }
            const std::uint32_t rights=setting?((flags&3U?0x80000U:0U)|(flags&0xa0000004U?0x40000U:0U)):0x20000U;
            auto& token=token_for(arg(0),rights);auto sd=stored_token_descriptor(token.object_descriptor);
            if(setting){
                SecurityDescriptor source;
                if(info){
                    if(flags&1U)source.data[0]=read_sid(memory,arg(3));
                    if(flags&2U)source.data[1]=read_sid(memory,arg(4));
                    if(flags&4U){source.control=4;if(arg(5))source.data[3]=read_acl(memory,arg(5)).bytes;}
                }else source=read_descriptor(memory,arg(2),x86_,true);
                if(flags&1U){const auto& caller=runtime_.token_objects.at(effective_token_id());if(source.data[0]!=caller.user.sid)throw SddlError{1307};sd.data[0]=source.data[0];sd.control&=static_cast<std::uint16_t>(~1U);}
                if(flags&2U){if(source.data[1].empty())throw SddlError{1337};sd.data[1]=source.data[1];sd.control&=static_cast<std::uint16_t>(~2U);}
                if(flags&4U){sd.data[3]=source.data[3];sd.control=static_cast<std::uint16_t>((sd.control&~12U)|(source.control&12U));
                    // 僅接受已具備存取檢查語義的基本 ACE；拒絕 callback/object ACE，避免悄悄放行。
                    if(!sd.data[3].empty()){const auto& acl=sd.data[3];const auto count=unsigned{acl[4]}|(unsigned{acl[5]}<<8);std::size_t cursor=8;
                        for(unsigned i=0;i<count;++i){const auto size=std::size_t{acl.at(cursor+2)}|(std::size_t{acl.at(cursor+3)}<<8);const auto ace=basic_ace(std::vector<std::uint8_t>(acl.data()+cursor,acl.data()+cursor+size));if(ace.type>1)throw SddlError{50};cursor+=size;}
                    }
                }
                if(flags&0x80000000U)sd.control|=0x1000;if(flags&0x20000000U)sd.control&=static_cast<std::uint16_t>(~0x1000U);
                token.object_descriptor=relative_descriptor(sd);result=info?0:1;
            }else {
                if(!(flags&1))sd.data[0].clear();if(!(flags&2))sd.data[1].clear();sd.data[2].clear();sd.control&=static_cast<std::uint16_t>(~0x30U);
                if(!(flags&4)){sd.data[3].clear();sd.control&=static_cast<std::uint16_t>(~12U);}
                const auto bytes=relative_descriptor(sd);
                if(info){
                    if(!arg(7))throw SddlError{87};for(unsigned i=3;i<=7;++i)if(arg(i))memory.check(GuestAddress{arg(i)},unit,Access::Write);
                    const auto output=allocate_heap(memory,bytes.size());if(!output)throw SddlError{8};memory.write(GuestAddress{output},bytes);runtime_.local_allocations.insert(output);
                    for(unsigned i=0;i<4;++i){const auto pointerarg=i==2?6U:i==3?5U:i+3;std::uint32_t offset=0;for(unsigned j=0;j<4;++j)offset|=std::uint32_t{bytes[4+i*4+j]}<<(j*8);if(arg(pointerarg))memory.store(GuestAddress{arg(pointerarg)},bits,offset?output+offset:0);}
                    memory.store(GuestAddress{arg(7)},bits,output);result=0;
                }else {
                    if(!arg(4))throw SddlError{87};memory.store(GuestAddress{arg(4)},32,bytes.size());if(u32(3)<bytes.size())throw SddlError{122};if(!arg(2))throw SddlError{87};memory.write(GuestAddress{arg(2)},bytes);result=1;
                }
            }
        }else
        if(symbol=="AreAllAccessesGranted")result=(u32(0)&u32(1))==u32(1);
        else if(symbol=="AreAnyAccessesGranted")result=bool(u32(0)&u32(1));
        else if(symbol=="MapGenericMask"){
            const auto mapping=mapping_at(arg(1));const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));memory.store(GuestAddress{arg(0)},32,mapped_access(mask,mapping));
        }else if(symbol=="OpenProcessToken" || symbol=="OpenThreadToken"){
            std::uint64_t id=1;const auto access=token_access(u32(1)&~0x02000000U);if(access&~token_all_access)throw SddlError{5};
            const auto operation=symbol=="OpenProcessToken"?1U:2U;
            const auto output=arg(operation==1?2:3);if(!output)throw SddlError{87};memory.check(GuestAddress{output},unit,Access::Write);
            const auto caller=operation==2 && u32(2)?1:effective_token_id();
            std::uint32_t pid{},rights{};std::uint64_t birth{};std::shared_ptr<SharedToken> shared;
            const auto remote=process_token_target(memory,arg(0),operation,pid,birth);
            if(remote){if(*remote)throw SddlError{*remote};const auto caller_token=share_token(caller);
                const auto error=remote_token_operation(memory,pid,birth,operation,caller_token->descriptor(),u32(1),rights,shared);if(error)throw SddlError{error};
                id=attach_shared_token(shared);
            }else{
                if(operation==1){
                    if(process_object_type(arg(0))!=u"Process")throw SddlError{6};
                    if(!process_handle_pid(arg(0),0x400) && !process_handle_pid(arg(0),0x1000))throw SddlError{5};
                }else {
                    const auto thread=thread_id(arg(0));if(!thread)throw SddlError{6};if(!(thread_access(arg(0))&0x840))throw SddlError{5};
                    id=threads_.items.at(thread).impersonation_token;if(!id)throw SddlError{1008};if(runtime_.token_objects.at(id).level==0)throw SddlError{1347};
                }
                if(runtime_.token_objects.at(caller).type==2 && runtime_.token_objects.at(caller).level<2)throw SddlError{1346};
                rights=granted_token_access(id,u32(1),caller);
            }
            new_handle(id,rights,output);
        }else if(symbol=="DuplicateToken" || symbol=="DuplicateTokenEx" || symbol=="CreateRestrictedToken"){
            auto token=token_for(arg(0),2);const auto source_rights=runtime_.token_handles.at(arg(0));std::uint32_t access=source_rights;bool inherit=false;
            const bool restricted=symbol=="CreateRestrictedToken",extended=symbol=="DuplicateTokenEx";
            const auto output=arg(restricted?8:extended?5:2);if(!output)throw SddlError{87};memory.check(GuestAddress{output},unit,Access::Write);
            if(restricted){
                if(u32(1)&~15U)throw SddlError{87};
                if(!(u32(1)&1U) && u32(4)){if(u32(4)>1024 || !arg(5))throw SddlError{87};memory.check(GuestAddress{arg(5)},std::size_t{u32(4)}*12,Access::Read);}
                if(u32(1)&1U)std::erase_if(token.privileges,[](const auto& item){return item.luid!=23;});
                else if(u32(4)){
                    std::set<std::uint64_t> removed;
                    for(unsigned i=0;i<u32(4);++i)removed.insert(memory.load(GuestAddress{arg(5)+i*12U},64));
                    std::erase_if(token.privileges,[&](const auto& item){return removed.contains(item.luid);});
                }
                const auto sids=[&](unsigned countarg,unsigned ptrarg){
                    std::vector<TokenSid> list;if(u32(countarg)>1024 || (u32(countarg) && !arg(ptrarg)))throw SddlError{87};
                    for(unsigned i=0;i<u32(countarg);++i){const auto ptr=memory.load(GuestAddress{arg(ptrarg)+std::uint64_t{i}*stride},bits);auto sid=read_sid(memory,ptr);if(std::none_of(list.begin(),list.end(),[&](const auto& item){return item.sid==sid;}))list.push_back({std::move(sid),7});}return list;
                };
                for(const auto& disabled:sids(2,3)){
                    if(token.user.sid==disabled.sid)token.user.attributes=(token.user.attributes&~6U)|0x10;
                    for(auto& group:token.groups)if(group.sid==disabled.sid)group.attributes=(group.attributes&~6U)|0x10;
                }
                if(u32(6)){
                    auto list=sids(6,7);if(token.restriction_check)std::erase_if(list,[&](const auto& item){return std::none_of(token.restricted.begin(),token.restricted.end(),[&](const auto& old){return old.sid==item.sid;});});
                    token.restricted=std::move(list);token.restriction_check=true;
                }
                token.write_restricted=token.write_restricted || bool(u32(1)&8U);
                // 本機一般使用者原本就沒有管理員 SID／privilege；LUA 不增加權限。
            }else {
                const auto level=u32(extended?3:1),type=extended?u32(4):2;
                if(level>3 || (type!=1 && type!=2))throw SddlError{87};if(token.type==2 && level>token.level)throw SddlError{1346};
                token.type=type;token.level=level;access=extended?(u32(1)?token_access(u32(1)):source_rights):0xcU;
                if(access&~token_all_access)throw SddlError{5};
                access=granted_token_access(runtime_.token_handle_objects.at(arg(0)),extended?(u32(1)?u32(1):source_rights):0xcU,effective_token_id());
                if(extended){const auto sa=synchronization_security(memory,arg(2),x86_);if(sa.error)throw SddlError{sa.error};inherit=sa.inherit;}
            }
            collect_tokens();const auto id=runtime_.next_token_object++;runtime_.token_objects.emplace(id,std::move(token));new_handle(id,access,output,inherit);
        }else if(symbol=="ImpersonateAnonymousToken"){
            const auto thread=thread_id(arg(0));if(!thread)throw SddlError{6};
            if(!(thread_access(arg(0))&0x100))throw SddlError{5}; // THREAD_IMPERSONATE，非 DIRECT_IMPERSONATION。
            const auto& target=threads_.items.at(thread);if(target.status==ThreadStatus::Exited || target.terminating)throw SddlError{31};
            if(runtime_.token_objects.at(1).restriction_check)throw SddlError{5};
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw SddlError{transaction.error};
            bool everyone=false;const auto key=runtime_.registry.keys.find(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\lsa");
            if(key!=runtime_.registry.keys.end())if(const auto value=key->second.find(u"everyoneincludesanonymous");value!=key->second.end() && value->second.type==4 && value->second.bytes.size()==4)
                everyone=value->second.bytes==std::vector<std::uint8_t>{1,0,0,0};
            VirtualToken token;token.type=2;token.user.sid=sddl_sid("S-1-5-7");token.owner=token.primary_group=token.user.sid;
            if(everyone)token.groups.push_back({sddl_sid("S-1-1-0"),7});
            // 匿名身分是獨立 token，不繼承一般使用者的 Authenticated Users／管理員群組。
            token.default_dacl.resize(8);token.default_dacl[0]=2;
            const auto ace=encode_ace({0,0,0x10000000U,token.user.sid});token.default_dacl.insert(token.default_dacl.end(),ace.begin(),ace.end());
            token_put(token.default_dacl,2,static_cast<std::uint32_t>(token.default_dacl.size()),2);token_put(token.default_dacl,4,1,2);
            SecurityDescriptor sd;sd.control=4;sd.data[0]=token.owner;sd.data[1]=token.primary_group;
            // 系統提供的匿名 token 可由本程序查詢；物件 DACL 與實際存取檢查仍走共用流程。
            sd.data[3]=token.default_dacl;const auto query=encode_ace({0,0,token_all_access,runtime_.token_objects.at(1).user.sid});sd.data[3].insert(sd.data[3].end(),query.begin(),query.end());
            token_put(sd.data[3],2,static_cast<std::uint32_t>(sd.data[3].size()),2);token_put(sd.data[3],4,2,2);token.object_descriptor=relative_descriptor(sd);
            collect_tokens();const auto id=runtime_.next_token_object++;runtime_.token_objects.emplace(id,std::move(token));install(thread,id);
        }else if(symbol=="ImpersonateNamedPipeClient"){
            const auto file=files_.find(arg(0));if(file==files_.end() || !file->second.pipe || !file->second.pipe->server())throw SddlError{6};
            std::vector<std::uint8_t> bytes;const auto error=file->second.pipe->client_context(bytes);if(error)throw SddlError{error};
            auto token=decode_virtual_token(bytes);if(!token)throw SddlError{13};
            if(token->user.sid!=runtime_.token_objects.at(effective_token_id()).user.sid)throw SddlError{1314};if(token->level<2)throw SddlError{50};
            const auto id=runtime_.next_token_object++;runtime_.token_objects.emplace(id,std::move(*token));install(threads_.current,id);
        }else if(symbol=="SetThreadToken"){
            const auto handle=arg(0)?memory.load(GuestAddress{arg(0)},bits):(x86_?UINT32_MAX-1:UINT64_MAX-1);const auto thread=thread_id(handle);
            if(thread && !(thread_access(handle)&0x80))throw SddlError{5};if(arg(1))token_for(arg(1),4);
            std::uint32_t pid{},rights{};std::uint64_t birth{};const auto remote=process_token_target(memory,handle,0,pid,birth);
            if(remote){if(*remote)throw SddlError{*remote};std::shared_ptr<SharedToken> shared;
                const auto token=arg(1)?share_token(runtime_.token_handle_objects.at(arg(1))):std::shared_ptr<SharedToken>{};
                const auto error=remote_token_operation(memory,pid,birth,0,token?token->descriptor():-1,0,rights,shared);if(error)throw SddlError{error};result=1;
            }else install(thread,arg(1)?runtime_.token_handle_objects.at(arg(1)):0);
        }else if(symbol=="RevertToSelf")install(threads_.current,0);
        else if(symbol=="ImpersonateSelf" || symbol=="ImpersonateLoggedOnUser"){
            VirtualToken token=symbol=="ImpersonateSelf"?runtime_.token_objects.at(1):token_for(arg(0),8);
            if(symbol=="ImpersonateSelf"){if(u32(0)>3)throw SddlError{87};token.level=u32(0);}
            else token_for(arg(0),token.type==1?2:4);
            token.type=2;const auto id=runtime_.next_token_object++;runtime_.token_objects.emplace(id,std::move(token));install(threads_.current,id);
        }else if(symbol=="IsTokenRestricted")result=token_for(arg(0),8).restriction_check;
        else if(symbol=="CheckTokenMembership"){
            const auto& token=arg(0)?token_for(arg(0),8):runtime_.token_objects.at(effective_token_id());
            if(arg(0) && token.type!=2)throw SddlError{1309};if(!arg(2))throw SddlError{87};const auto sid=read_sid(memory,arg(1));
            bool member=token_sid_matches(token,sid,false);if(token.restriction_check)member=member && token_sid_matches(token,sid,false,true);
            memory.store(GuestAddress{arg(2)},32,member?1:0);result=1;
        }else if(symbol=="AccessCheck"){
            const auto& token=token_for(arg(1),8);if(token.type!=2)throw SddlError{1309};if(token.level<1)throw SddlError{1346};
            if(u32(2)&0xf0000000U)throw SddlError{1360};const auto mapping=mapping_at(arg(3));
            if(!arg(5) || !arg(6) || !arg(7))throw SddlError{87};
            const auto size=memory.load(GuestAddress{arg(5)},32);memory.store(GuestAddress{arg(5)},32,8);if(size<8 || !arg(4))throw SddlError{122};
            const auto sd=read_descriptor(memory,arg(0),x86_,true);if(sd.data[0].empty() || sd.data[1].empty())throw SddlError{1338};
            memory.check(GuestAddress{arg(4)},8,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);memory.check(GuestAddress{arg(7)},4,Access::Write);
            const auto granted=token_acl_access(token,sd,u32(2),mapping);const bool allowed=(u32(2)==0 || granted!=0) && !(u32(2)&0x01000000U);
            memory.store(GuestAddress{arg(4)},64,0);memory.store(GuestAddress{arg(6)},32,allowed?granted:0);memory.store(GuestAddress{arg(7)},32,allowed?1:0);
            result=1;if(!allowed)last_error_=u32(2)&0x01000000U?1314:5;
        }else if(symbol=="AdjustTokenPrivileges"){
            auto& token=token_for(arg(0),token_adjust_privileges|(arg(4)?token_query:0));
            if((!u32(1) && !arg(2)) || (arg(4) && !arg(5)))throw SddlError{87};
            auto changed=token.privileges;std::vector<TokenPrivilege> previous;bool missing=false;
            const auto modify=[&](std::uint64_t luid,unsigned attributes){
                const auto item=std::find_if(changed.begin(),changed.end(),[&](const auto& value){return value.luid==luid;});
                if(item==changed.end()){missing=true;return;}
                if(attributes&4U){changed.erase(item);return;}
                const auto value=(item->attributes&~2U)|(attributes&2U);
                if(value!=item->attributes){previous.push_back(*item);item->attributes=value;}
            };
            if(u32(1)){for(const auto& item:token.privileges)modify(item.luid,0);}
            else {
                const auto count=memory.load(GuestAddress{arg(2)},32);if(count>output_limit/12)throw SddlError{87};
                memory.check(GuestAddress{arg(2)},4+static_cast<std::size_t>(count)*12,Access::Read);
                for(std::uint64_t i=0;i<count;++i){const auto at=arg(2)+4+i*12;modify(memory.load(GuestAddress{at},64),static_cast<unsigned>(memory.load(GuestAddress{at+8},32)));}
            }
            const auto needed=4+previous.size()*12;
            if(arg(4)){
                memory.check(GuestAddress{arg(5)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,needed);
                if(u32(3)<needed)throw SddlError{122};memory.check(GuestAddress{arg(4)},needed,Access::Write);
                memory.store(GuestAddress{arg(4)},32,previous.size());
                for(std::size_t i=0;i<previous.size();++i){memory.store(GuestAddress{arg(4)+4+i*12},64,previous[i].luid);memory.store(GuestAddress{arg(4)+12+i*12},32,previous[i].attributes);}
            }
            if(changed!=token.privileges){token.privileges=std::move(changed);++token.modified_id;}
            result=1;last_error_=missing?1300:0;
        }else if(symbol=="SetTokenInformation"){
            auto& token=token_for(arg(0),0x80);const auto kind=u32(1);
            if(kind!=4 && kind!=5 && kind!=6 && kind!=12 && kind!=25 && kind!=24)throw SddlError{87};
            const auto minimum=kind==12 || kind==24?4U:kind==25?stride:unit;
            if(u32(3)<minimum)throw SddlError{24};if(!arg(2))throw SddlError{87};memory.check(GuestAddress{arg(2)},minimum,Access::Read);
            if(kind==12)throw SddlError{1314};
            if(kind==24){if(memory.load(GuestAddress{arg(2)},32))throw SddlError{50};token.virtualization_enabled=false;}
            else {
                const auto pointer=memory.load(GuestAddress{arg(2)},bits);
                if(kind==6)token.default_dacl=pointer?read_acl(memory,pointer).bytes:std::vector<std::uint8_t>{};
                else {const auto sid=read_sid(memory,pointer);
                    if(kind==25){if(sid.size()!=12 || !std::equal(sid.begin(),sid.begin()+8,sddl_sid("S-1-16-0").begin()))throw SddlError{1337};const auto integrity=sid_rid(sid,0);if(integrity>token.integrity)throw SddlError{1314};token.integrity=integrity;refresh_file_security_context();}
                    else if(kind==4){if(sid!=token.user.sid && std::none_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==sid && (group.attributes&8);}))throw SddlError{1307};token.owner=sid;}
                    else {if(sid!=token.user.sid && std::none_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==sid;}))throw SddlError{1308};token.primary_group=sid;}
                }
            }
            result=1;
        }else if(symbol=="GetTokenInformation"){
            const auto kind=u32(1);const auto& token=token_for(arg(0),kind==7?0x10:8);if(!arg(4))throw SddlError{87};
            std::vector<std::uint8_t> bytes;const auto value=[&](std::uint32_t v){bytes.resize(4);token_put(bytes,0,v);};
            const auto sid_field=[&](const std::vector<std::uint8_t>& sid,std::uint32_t attributes,bool attrs){
                const auto prefix=attrs?stride:unit;bytes.resize(prefix);if(!sid.empty()){token_put(bytes,0,arg(2)+prefix,unit);bytes.insert(bytes.end(),sid.begin(),sid.end());}if(attrs)token_put(bytes,unit,attributes);
            };
            switch(kind){
            case 1:sid_field(token.user.sid,token.user.attributes,true);break;
            case 2:case 11:case 28:case 30:{
                std::vector<TokenSid> list;
                if(kind==2)list=token.groups;else if(kind==11)list=token.restricted;
                else if(kind==28)for(const auto& group:token.groups)if((group.attributes&0xc0000000U)==0xc0000000U)list.push_back(group);
                // 目前只提供一般桌面權杖；沒有 AppContainer，因此 capabilities 清單為空。
                bytes.resize(unit+stride*list.size());token_put(bytes,0,static_cast<std::uint32_t>(list.size()));
                for(std::size_t i=0;i<list.size();++i){token_put(bytes,unit+i*stride,arg(2)+bytes.size(),unit);token_put(bytes,unit+i*stride+unit,list[i].attributes);bytes.insert(bytes.end(),list[i].sid.begin(),list[i].sid.end());}break;
            }
            case 3:
                bytes.resize(std::max<std::size_t>(16,4+token.privileges.size()*12));token_put(bytes,0,token.privileges.size());
                for(std::size_t i=0;i<token.privileges.size();++i){token_put(bytes,4+i*12,token.privileges[i].luid,8);token_put(bytes,12+i*12,token.privileges[i].attributes);}break;
            case 4:sid_field(token.owner,0,false);break;
            case 5:sid_field(token.primary_group,0,false);break;
            case 6:sid_field(token.default_dacl,0,false);break;
            case 7:bytes.resize(16);std::copy(token.source.begin(),token.source.end(),bytes.begin());token_put(bytes,8,token.source_id,8);break;
            case 8:value(token.type);break;
            case 9:if(token.type!=2)throw SddlError{87};value(token.level);break;
            case 10:bytes.resize(56);token_put(bytes,0,runtime_.token_handle_objects.at(arg(0)),8);token_put(bytes,8,token.authentication_id,8);token_put(bytes,16,token.expires,8);token_put(bytes,24,token.type);token_put(bytes,28,token.level);token_put(bytes,40,static_cast<std::uint32_t>(token.groups.size()));token_put(bytes,44,token.privileges.size());token_put(bytes,48,token.modified_id,8);break;
            case 12:value(token.session);break;
            case 15:value(token.sandbox_inert);break;
            case 18:case 20:{const bool elevated=token.integrity>=0x3000U && std::any_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==sddl_sid("BA") && (group.attributes&0x14U)==4U;});value(kind==18?(elevated?2U:1U):(elevated?1U:0U));break;}
            case 23:case 26:case 29:case 32:value(0);break;
            case 24:value(token.virtualization_enabled);break;
            case 31:bytes.resize(unit);break;
            case 21:value(token.restriction_check || token.user.attributes&0x10 || std::any_of(token.groups.begin(),token.groups.end(),[](const auto& group){return group.attributes&0x10;}));break;
            case 25:sid_field(sddl_sid("S-1-16-"+std::to_string(token.integrity)),0x60,true);break;
            case 27:value(1);break;
            default:throw SddlError{kind==0 || kind>=50?87U:50U};
            }
            memory.store(GuestAddress{arg(4)},32,bytes.size());if(u32(3)<bytes.size())throw SddlError{122};if(!arg(2))throw SddlError{87};memory.write(GuestAddress{arg(2)},bytes);result=1;
        }else throw HostCallFault("未知的 Windows 權杖服務");
        if(mutation){mutation->write(runtime_.token_objects.at(mutating_shared_token_));mutation.reset();mutating_shared_token_=0;refresh_file_security_context();}
    }catch(const SddlError& error){collect_tokens();if(symbol=="GetSecurityInfo" || symbol=="SetSecurityInfo")result=error.code;else{last_error_=error.code;result=0;}}
    catch(const MemoryFault&){if(symbol=="GetSecurityInfo" || symbol=="SetSecurityInfo")result=998;else{last_error_=998;result=0;}}
    catch(const std::bad_alloc&){if(symbol=="GetSecurityInfo" || symbol=="SetSecurityInfo")result=8;else{last_error_=8;result=0;}}
    if(mutation){runtime_.token_objects.at(mutating_shared_token_)=std::move(original);mutation.reset();mutating_shared_token_=0;refresh_file_security_context();}
    collect_tokens();arg.return_to(state,result);return std::nullopt;
}

}
