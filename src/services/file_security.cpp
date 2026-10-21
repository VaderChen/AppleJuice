#include "x64arm/file_security.hpp"
#include "x64arm/readonly_files.hpp"
#include "x64arm/security_access.hpp"
#include "windows_sddl.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#if !defined(_WIN32)
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <membership.h>
#include <sys/acl.h>
#include <sys/attr.h>
#include <sys/kauth.h>
#include <sys/xattr.h>
#include <sys/unistd.h>
#endif

namespace x64arm::security {
namespace {
#if !defined(_WIN32)
std::uint32_t native_error(int code=errno) {
    switch (code) {
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EACCES: case EPERM: case ELOOP: return 5;
    case EBADF: return 6;
    case ENOMEM: return 8;
    case EMFILE: case ENFILE: return 4;
    case EINVAL: return 87;
    case EAGAIN: return 1237;
    case ENOTSUP: return 50;
    default: return 1117;
    }
}
template<class Lookup> auto account_lookup(Lookup&& lookup) {
    for (std::size_t size=16384;size<=1048576;size*=2) {
        std::vector<char> buffer(size);
        auto [error,value]=lookup(buffer);
        if (error==ERANGE) continue;
        if (error && error!=ENOENT) throw SddlError{native_error(error)};
        return value;
    }
    throw SddlError{8};
}
std::optional<std::uint32_t> account_name(std::string_view name, bool group) {
    const std::string text(name);
    return account_lookup([&](std::vector<char>& buffer) {
        if (group) {
            struct group entry{}, *found{};
            const auto status=::getgrnam_r(text.c_str(),&entry,buffer.data(),buffer.size(),&found);
            return std::pair{status,found?std::optional{static_cast<std::uint32_t>(entry.gr_gid)}:std::nullopt};
        }
        struct passwd entry{}, *found{};
        const auto status=::getpwnam_r(text.c_str(),&entry,buffer.data(),buffer.size(),&found);
        return std::pair{status,found?std::optional{static_cast<std::uint32_t>(entry.pw_uid)}:std::nullopt};
    });
}
#endif
#if defined(__APPLE__)
struct IdentityLookups {
    unsigned depth{};
    std::map<std::pair<int,Bytes>,std::array<unsigned char,16>> uuids;
    std::map<std::array<unsigned char,16>,Bytes> sids;
    void clear() noexcept {uuids.clear();sids.clear();}
};
thread_local IdentityLookups identity_lookups;
int identity_uuid(int type,const void* identifier,std::size_t size,unsigned char* uuid) {
    const auto* bytes=static_cast<const std::uint8_t*>(identifier);
    const auto key=std::pair{type,Bytes(bytes,bytes+size)};
    if(const auto found=identity_lookups.uuids.find(key);found!=identity_lookups.uuids.end()) {
        std::copy(found->second.begin(),found->second.end(),uuid);return 0;
    }
    const auto error=::mbr_identifier_to_uuid(type,identifier,size,uuid);
    // 失敗可在同一操作稍後重試；避免把暫時性身分服務錯誤保存為永久映射。
    if(!error && identity_lookups.depth && identity_lookups.uuids.size()<4096) {
        std::array<unsigned char,16> value{};std::copy_n(uuid,value.size(),value.begin());
        identity_lookups.uuids.emplace(key,value);
    }
    return error;
}
struct AclAllocation {
    void* value{};
    ~AclAllocation() { if (value) ::acl_free(value); }
};
bool flag(acl_flagset_t flags, acl_flag_t bit) {
    const auto present=::acl_get_flag_np(flags,bit);
    if (present<0) throw SddlError{native_error()};
    return present!=0;
}
Bytes uuid_sid(const uuid_t uuid) {
    std::array<unsigned char,16> key{};std::copy_n(uuid,key.size(),key.begin());
    if(const auto found=identity_lookups.sids.find(key);found!=identity_lookups.sids.end())return found->second;
    const auto remember=[&](Bytes sid) {
        if(identity_lookups.depth && identity_lookups.sids.size()<4096)identity_lookups.sids.emplace(key,sid);
        return sid;
    };
    // well-known UUID 不能轉成同名 Unix group，否則往返會改變 trustee。
    for(const auto name:{"WD","CO","CG"}) {
        const auto sid=sddl_sid(name);nt_sid_t native{};native.sid_kind=1;native.sid_authcount=sid[1];
        std::copy(sid.begin()+2,sid.begin()+8,native.sid_authority);
        for(unsigned i=0;i<sid[1];++i)for(unsigned j=0;j<4;++j)native.sid_authorities[i]|=std::uint32_t{sid[8+i*4+j]}<<(j*8);
        uuid_t known{};
        if(!identity_uuid(ID_TYPE_SID,&native,sizeof native,known) && !std::memcmp(uuid,known,sizeof known))return remember(sid);
    }
    uid_t id{}; int type{};
    const auto status=::mbr_uuid_to_id(uuid,&id,&type);
    if (!status && (type==ID_TYPE_UID || type==ID_TYPE_GID)) {
        // mbr 也會產生暫時 ID；只有能在帳號資料庫查到的 ID 才作持久 Unix SID。
        const auto present=account_lookup([&](std::vector<char>& buffer) {
            if (type==ID_TYPE_GID) {
                struct group entry{}, *found{};
                const auto code=::getgrgid_r(static_cast<gid_t>(id),&entry,buffer.data(),buffer.size(),&found);
                return std::pair{code,found!=nullptr};
            }
            struct passwd entry{}, *found{};
            const auto code=::getpwuid_r(id,&entry,buffer.data(),buffer.size(),&found);
            return std::pair{code,found!=nullptr};
        });
        if (present) return remember(unix_sid(type==ID_TYPE_GID,static_cast<std::uint32_t>(id)));
    }
    nt_sid_t native{};
    if (::mbr_uuid_to_sid(uuid,&native) || native.sid_kind!=1 || native.sid_authcount>15)
        throw SddlError{1332}; // 不丟掉無法辨識的 ACE，也不把暫時 ID 當成持久身分。
    Bytes bytes(8+4U*native.sid_authcount); bytes[0]=1; bytes[1]=native.sid_authcount;
    std::copy(std::begin(native.sid_authority),std::end(native.sid_authority),bytes.begin()+2);
    for (unsigned i=0;i<native.sid_authcount;++i) sddl_put(bytes,8+i*4,native.sid_authorities[i]);
    return remember(std::move(bytes));
}
std::uint32_t native_mask(acl_entry_t entry) {
    acl_permset_mask_t mask{};
    if (::acl_get_permset_mask_np(entry,&mask)<0) throw SddlError{native_error()};
    static constexpr std::array mapping{
        std::pair{ACL_READ_DATA,1U}, std::pair{ACL_WRITE_DATA,2U}, std::pair{ACL_APPEND_DATA,4U},
        std::pair{ACL_READ_EXTATTRIBUTES,8U}, std::pair{ACL_WRITE_EXTATTRIBUTES,16U},
        std::pair{ACL_EXECUTE,32U}, std::pair{ACL_DELETE_CHILD,64U},
        std::pair{ACL_READ_ATTRIBUTES,128U}, std::pair{ACL_WRITE_ATTRIBUTES,256U},
        std::pair{ACL_DELETE,0x10000U}, std::pair{ACL_READ_SECURITY,0x20000U},
        std::pair{ACL_WRITE_SECURITY,0x40000U}, std::pair{ACL_CHANGE_OWNER,0x80000U},
        std::pair{ACL_SYNCHRONIZE,0x100000U}
    };
    acl_permset_mask_t known{}; std::uint32_t result{};
    for (const auto& [native,windows]:mapping) {
        known|=static_cast<acl_permset_mask_t>(native);
        if (mask&static_cast<acl_permset_mask_t>(native)) result|=windows;
    }
    if (mask&~known) throw SddlError{50};
    return result;
}
constexpr std::array<unsigned char,8> label_marker{'X','6','4','A','R','M','I','L'};
bool is_label_marker(const unsigned char* uuid) {return std::equal(label_marker.begin(),label_marker.end(),uuid);}
constexpr std::array<unsigned char,4> dacl_marker{'X','A','C','L'};
bool is_dacl_marker(const unsigned char* uuid){return std::equal(dacl_marker.begin(),dacl_marker.end(),uuid);}
void hash_byte(std::uint64_t& hash,unsigned char byte){hash=(hash^byte)*UINT64_C(1099511628211);}
std::uint64_t native_acl_hash(acl_t acl){
    std::uint64_t hash=UINT64_C(14695981039346656037);acl_entry_t entry{};
    for(int cursor=ACL_FIRST_ENTRY;;cursor=ACL_NEXT_ENTRY){
        if(::acl_get_entry(acl,cursor,&entry)<0){if(errno==EINVAL)break;throw SddlError{native_error()};}
        AclAllocation qualifier{::acl_get_qualifier(entry)};if(!qualifier.value)throw SddlError{native_error()};
        const auto* uuid=static_cast<const unsigned char*>(qualifier.value);if(is_label_marker(uuid)||is_dacl_marker(uuid))continue;
        acl_tag_t tag{};acl_flagset_t flags{};if(::acl_get_tag_type(entry,&tag)<0 || ::acl_get_flagset_np(entry,&flags)<0)throw SddlError{native_error()};
        for(unsigned i=0;i<4;++i)hash_byte(hash,static_cast<unsigned char>(static_cast<unsigned>(tag)>>(i*8)));for(unsigned i=0;i<16;++i)hash_byte(hash,uuid[i]);
        const auto mask=native_mask(entry);for(unsigned i=0;i<4;++i)hash_byte(hash,static_cast<unsigned char>(mask>>(i*8)));
        unsigned inheritance=0;for(const auto& [native,windows]:std::array{std::pair{ACL_ENTRY_FILE_INHERIT,1U},std::pair{ACL_ENTRY_DIRECTORY_INHERIT,2U},std::pair{ACL_ENTRY_LIMIT_INHERIT,4U},std::pair{ACL_ENTRY_ONLY_INHERIT,8U},std::pair{ACL_ENTRY_INHERITED,16U}})if(flag(flags,native))inheritance|=windows;
        hash_byte(hash,static_cast<unsigned char>(inheritance));
    }return hash;
}
struct AclProjection {std::optional<Bytes> dacl,descriptor;};
AclProjection acl_projection(acl_t acl,const NativeFileSecurity& native){
    std::map<unsigned,std::array<unsigned char,10>> chunks;acl_entry_t entry{};
    for(int cursor=ACL_FIRST_ENTRY;;cursor=ACL_NEXT_ENTRY){
        if(::acl_get_entry(acl,cursor,&entry)<0){if(errno==EINVAL)break;throw SddlError{native_error()};}
        AclAllocation qualifier{::acl_get_qualifier(entry)};if(!qualifier.value)throw SddlError{native_error()};const auto* uuid=static_cast<const unsigned char*>(qualifier.value);
        if(!is_dacl_marker(uuid))continue;acl_tag_t tag{};acl_flagset_t flags{};
        if(::acl_get_tag_type(entry,&tag)<0 || ::acl_get_flagset_np(entry,&flags)<0)throw SddlError{native_error()};
        const unsigned index=unsigned{uuid[4]}|(unsigned{uuid[5]}<<8);
        if(tag!=ACL_EXTENDED_ALLOW || native_mask(entry) || flag(flags,ACL_ENTRY_FILE_INHERIT) || flag(flags,ACL_ENTRY_DIRECTORY_INHERIT) || flag(flags,ACL_ENTRY_LIMIT_INHERIT) || flag(flags,ACL_ENTRY_ONLY_INHERIT) || flag(flags,ACL_ENTRY_INHERITED) || index>=6556)throw SddlError{1338};
        std::array<unsigned char,10> payload{};std::copy(uuid+6,uuid+16,payload.begin());if(!chunks.emplace(index,payload).second)throw SddlError{1338};
    }
    if(chunks.empty())return {};if(!chunks.contains(0))throw SddlError{1338};
    const auto& first=chunks.at(0);std::uint32_t size=0;for(unsigned i=0;i<4;++i)size|=std::uint32_t{first[i]}<<(i*8);
    if(size<16 || size>65551 || chunks.size()!=(size+9U)/10U)throw SddlError{1338};Bytes bytes;bytes.reserve(chunks.size()*10U);
    for(unsigned i=0;i<chunks.size();++i){const auto found=chunks.find(i);if(found==chunks.end())throw SddlError{1338};bytes.insert(bytes.end(),found->second.begin(),found->second.end());}
    if(std::any_of(bytes.begin()+size,bytes.end(),[](auto byte){return byte!=0;}))throw SddlError{1338};bytes.resize(size);
    std::uint64_t expected=0;for(unsigned i=0;i<8;++i)expected|=std::uint64_t{bytes[4+i]}<<(i*8);
    if((bytes[12]!=1 && bytes[12]!=2) || bytes[13]>1 || bytes[14] || bytes[15])throw SddlError{1338};
    if(expected!=native_acl_hash(acl))return {}; // 外部 ACL 變更後，以當前原生 ACL 授權。
    if(bytes[12]==2){
        if(bytes[13] || bytes.size()<52)throw SddlError{1338};
        const auto word=[&](unsigned at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(8*i);return value;};
        if(word(16)!=native.uid || word(20)!=native.gid || word(24)!=static_cast<unsigned>(native.directory) || word(28))return {};
        Bytes descriptor(bytes.begin()+32,bytes.end());(void)parse_relative_descriptor(descriptor);return {{},std::move(descriptor)};
    }
    Bytes dacl(bytes.begin()+16,bytes.end());if(bytes[13]){if(!dacl.empty())throw SddlError{1338};}else (void)parse_acl(dacl);return {std::move(dacl),{}};
}
void append_projection(acl_t& acl,const Bytes& payload,unsigned version,bool null_dacl=false){
    Bytes bytes(16);const auto size=16U+payload.size();if(size>65551)throw SddlError{1344};
    unsigned count=0;acl_entry_t existing{};for(int cursor=ACL_FIRST_ENTRY;;cursor=ACL_NEXT_ENTRY){if(::acl_get_entry(acl,cursor,&existing)<0){if(errno==EINVAL)break;throw SddlError{native_error()};}++count;}
    if(count+(size+9U)/10U>ACL_MAX_ENTRIES)throw SddlError{1344};
    for(unsigned i=0;i<4;++i)bytes[i]=static_cast<unsigned char>(size>>(i*8));const auto hash=native_acl_hash(acl);
    for(unsigned i=0;i<8;++i)bytes[4+i]=static_cast<unsigned char>(hash>>(i*8));bytes[12]=static_cast<unsigned char>(version);bytes[13]=null_dacl;bytes.insert(bytes.end(),payload.begin(),payload.end());
    for(unsigned index=0;index*10U<bytes.size();++index){
        uuid_t uuid{};std::copy(dacl_marker.begin(),dacl_marker.end(),uuid);uuid[4]=static_cast<unsigned char>(index);uuid[5]=static_cast<unsigned char>(index>>8);
        for(unsigned i=0;i<10 && index*10U+i<bytes.size();++i)uuid[6+i]=bytes[index*10U+i];acl_entry_t entry{};acl_permset_t perms{};
        if(::acl_create_entry(&acl,&entry)<0 || ::acl_set_tag_type(entry,ACL_EXTENDED_ALLOW)<0 || ::acl_set_qualifier(entry,uuid)<0 || ::acl_get_permset(entry,&perms)<0 || ::acl_clear_perms(perms)<0)throw SddlError{native_error()};
    }
}
void append_acl_projection(acl_t& acl,const Bytes& dacl){append_projection(acl,dacl,1,dacl.empty());}
void read_extended_acl(NativeFileSecurity& result, acl_t acl, bool with_dacl=true) {
    AclAllocation allocation{acl}; acl_flagset_t flags{};
    if (::acl_get_flagset_np(acl,&flags)<0) throw SddlError{native_error()};
    result.protected_acl=flag(flags,ACL_FLAG_NO_INHERIT);
    result.deferred_inherit=flag(flags,ACL_FLAG_DEFER_INHERIT);
    const auto projection=acl_projection(acl,result);result.mapped_descriptor=projection.descriptor;
    if(with_dacl)result.mapped_dacl=projection.dacl;
    acl_entry_t entry{};
    for (int cursor=ACL_FIRST_ENTRY;;cursor=ACL_NEXT_ENTRY) {
        if (::acl_get_entry(acl,cursor,&entry)<0) {
            if (errno==EINVAL) break; // macOS 以 EINVAL 表示已到尾端，成功取得項目時回傳 0。
            throw SddlError{native_error()};
        }
        if (result.extended.size()>=4096) throw SddlError{1344};
        acl_tag_t tag{};
        if (::acl_get_tag_type(entry,&tag)<0 || ::acl_get_flagset_np(entry,&flags)<0)
            throw SddlError{native_error()};
        AclAllocation qualifier{::acl_get_qualifier(entry)};
        if (!qualifier.value) throw SddlError{native_error()};
        ExplicitAce ace; ace.type=tag==ACL_EXTENDED_DENY?1:0;
        if (flag(flags,ACL_ENTRY_FILE_INHERIT)) ace.flags|=1;
        if (flag(flags,ACL_ENTRY_DIRECTORY_INHERIT)) ace.flags|=2;
        if (flag(flags,ACL_ENTRY_LIMIT_INHERIT)) ace.flags|=4;
        if (flag(flags,ACL_ENTRY_ONLY_INHERIT)) ace.flags|=8;
        if (flag(flags,ACL_ENTRY_INHERITED)) ace.flags|=16;
        const auto* uuid=static_cast<const unsigned char*>(qualifier.value);
        if(is_dacl_marker(uuid))continue;
        if(is_label_marker(uuid)) {
            const auto level=std::uint32_t{uuid[10]}|(std::uint32_t{uuid[11]}<<8);
            if(tag!=ACL_EXTENDED_ALLOW || native_mask(entry) || uuid[8]!=1 || (uuid[9]&~7U) ||
               (level&0xfffU) || level>0x5000 || uuid[12] || uuid[13] || uuid[14] || uuid[15]!=0xa5 ||
               !result.mandatory_label.empty())throw SddlError{1338};
            result.mandatory_label=integrity_label(level,ace.flags,uuid[9]);continue;
        }
        if(!with_dacl)continue;
        if (tag!=ACL_EXTENDED_ALLOW && tag!=ACL_EXTENDED_DENY) throw SddlError{50};
        constexpr std::array<unsigned char,8> virtual_sid{'X','6','4','A','R','S','I','D'};
        if(std::equal(virtual_sid.begin(),virtual_sid.end(),uuid)){if(!result.mapped_dacl)throw SddlError{50};continue;}
        ace.sid=uuid_sid(uuid); ace.mask=native_mask(entry);
        result.extended.push_back(std::move(ace));
    }
}
bool volume_has_no_acl(int directory, const char* name) {
    struct attrlist requested{}; requested.bitmapcount=ATTR_BIT_MAP_COUNT;
    requested.volattr=ATTR_VOL_INFO|ATTR_VOL_CAPABILITIES;
    struct { std::uint32_t length; vol_capabilities_attr_t capabilities; } volume{};
    if (::getattrlistat(directory,name,&requested,&volume,sizeof volume,FSOPT_NOFOLLOW|FSOPT_REPORT_FULLSIZE)<0 ||
        volume.length!=sizeof volume) return false;
    const auto index=VOL_CAPABILITIES_INTERFACES, bit=VOL_CAP_INT_EXTENDED_SECURITY;
    return (volume.capabilities.valid[index]&bit) && !(volume.capabilities.capabilities[index]&bit);
}
acl_t native_acl_at(int directory,const char* name){
    struct attrlist requested{};requested.bitmapcount=ATTR_BIT_MAP_COUNT;requested.commonattr=ATTR_CMN_EXTENDED_SECURITY;Bytes buffer(4096);
    for(;;){
        if(::getattrlistat(directory,name,&requested,buffer.data(),buffer.size(),FSOPT_NOFOLLOW|FSOPT_REPORT_FULLSIZE)<0){
            if(errno==ENOTSUP || (errno==EINVAL && volume_has_no_acl(directory,name))){errno=ENOENT;return nullptr;}throw SddlError{native_error()};
        }
        std::uint32_t length{};std::memcpy(&length,buffer.data(),4);if(length>1048576 || length<4+sizeof(attrreference_t))throw SddlError{13};
        if(length>buffer.size()){buffer.resize(length);continue;}attrreference_t reference{};std::memcpy(&reference,buffer.data()+4,sizeof(reference));
        if(!reference.attr_length){errno=ENOENT;return nullptr;} // 無原生 ACL 是有效空結果。
        if(reference.attr_dataoffset<static_cast<std::int32_t>(sizeof(reference)))throw SddlError{13};const auto offset=4+static_cast<std::size_t>(reference.attr_dataoffset);
        if((offset&3U) || offset>length || reference.attr_length>length-offset || reference.attr_length<KAUTH_FILESEC_SIZE(0))throw SddlError{13};
        struct kauth_filesec header{};std::memcpy(&header,buffer.data()+offset,KAUTH_FILESEC_SIZE(0));if(header.fsec_magic!=KAUTH_FILESEC_MAGIC)throw SddlError{13};
        if(header.fsec_entrycount==KAUTH_FILESEC_NOACL){errno=ENOENT;return nullptr;}
        if(header.fsec_entrycount>4096 || KAUTH_FILESEC_SIZE(header.fsec_entrycount)>reference.attr_length)throw SddlError{13};
        const auto acl=::acl_copy_int_native(buffer.data()+offset);if(!acl)throw SddlError{native_error()};return acl;
    }
}
int set_native_acl_at(const HostFile& file,acl_t acl){
    const auto& target=*file.security_target();const auto info=file.information();if(info.error){errno=info.error==1237?EAGAIN:EACCES;return -1;}
    const auto size=::acl_size(acl);if(size<0)return -1;Bytes buffer(sizeof(attrreference_t)+static_cast<std::size_t>(size));
    attrreference_t reference{static_cast<std::int32_t>(sizeof(attrreference_t)),static_cast<std::uint32_t>(size)};std::memcpy(buffer.data(),&reference,sizeof(reference));
    if(::acl_copy_ext_native(buffer.data()+sizeof(reference),acl,size)<0)return -1;
    struct attrlist requested{};requested.bitmapcount=ATTR_BIT_MAP_COUNT;requested.commonattr=ATTR_CMN_EXTENDED_SECURITY;
    if(::setattrlistat(target.directory,target.leaf.c_str(),&requested,buffer.data(),buffer.size(),FSOPT_NOFOLLOW)<0)return -1;
    if(file.information().error){errno=EAGAIN;return -1;}return 0;
}
constexpr char projection_name[]="user.x64arm.file-dacl-v1";
Bytes projection_acl(const NativeFileSecurity& native, bool footer, bool implicit_owner=false) {
    std::vector<Bytes> entries;
    const auto count=native.extended.size()-(footer?1U:0U);
    for(std::size_t i=implicit_owner?1U:0U;i<count;++i)entries.push_back(encode_ace(native.extended[i]));
    return encode_acl(entries);
}
bool projection_footer(const NativeFileSecurity& native) {
    if(native.extended.empty())return false;
    const auto& last=native.extended.back();
    return last.type==1 && last.mask==0x1f01ffU && !(last.flags&~3U) && last.sid==sddl_sid("WD");
}
std::optional<Bytes> read_projection(int fd) {
    const auto size=::fgetxattr(fd,projection_name,nullptr,0,0,0);
    if(size<0 && (errno==ENOATTR || errno==ENOTSUP || errno==EACCES || errno==EPERM))return {};
    if(size<0)throw SddlError{native_error()};
    if(size>65552)return {}; // 外部 metadata 不代表宿主 ACL，絕不據此授權。
    Bytes bytes(static_cast<std::size_t>(size));
    if(::fgetxattr(fd,projection_name,bytes.data(),bytes.size(),0,0)!=size)throw SddlError{1237};
    return bytes;
}
void read_mapped_dacl(NativeFileSecurity& native, int fd) {
    if(native.mapped_dacl)return;
    const auto saved=read_projection(fd);
    constexpr std::array<std::uint8_t,8> magic{'X','6','4','A','C','L','0','2'};
    if(!saved || saved->size()<17 || !std::equal(magic.begin(),magic.end(),saved->begin()) ||
       (*saved)[8]>1 || !projection_footer(native))return;
    const auto current=projection_acl(native,true);
    if(saved->size()!=9+current.size() || !std::equal(current.begin(),current.end(),saved->begin()+9))return;
    const auto& implicit=native.extended.front();
    if(implicit.type || implicit.flags || implicit.mask!=0x60000U || implicit.sid!=unix_sid(false,native.uid))return;
    if((*saved)[8]) {
        if(native.extended.size()!=3 || native.extended[1].type || native.extended[1].flags ||
           native.extended[1].mask!=0x1f01ffU || native.extended[1].sid!=sddl_sid("WD"))return;
        native.mapped_dacl=Bytes{};
    } else native.mapped_dacl=projection_acl(native,true,true);
}
#endif
// POSIX 先選 owner，再選 group，最後才選 other；後續 ACE 不得補回前一類沒有的內容權限。
constexpr std::uint32_t content_mask=0x137U, common_mask=0x120088U;
std::uint32_t mode_mask(unsigned bits) {
    return (bits&4?1U:0U)|(bits&2?0x116U:0U)|(bits&1?0x20U:0U);
}
}
std::optional<Bytes> native_account_sid(std::string_view name) {
#if defined(_WIN32)
    (void)name; return std::nullopt;
#else
    const auto separator=name.find('\\');
    if (separator!=name.npos) {
        const auto domain=name.substr(0,separator); name.remove_prefix(separator+1);
        if (same_name(domain,"Unix User")) {
            const auto id=account_name(name,false); return id?std::optional{unix_sid(false,*id)}:std::nullopt;
        }
        if (same_name(domain,"Unix Group")) {
            const auto id=account_name(name,true); return id?std::optional{unix_sid(true,*id)}:std::nullopt;
        }
        return std::nullopt;
    }
    if (const auto uid=account_name(name,false)) return unix_sid(false,*uid);
    if (const auto gid=account_name(name,true)) return unix_sid(true,*gid);
    return std::nullopt;
#endif
}
NativeFileIdentityScope::NativeFileIdentityScope() noexcept {
#if defined(__APPLE__)
    if(!identity_lookups.depth++)identity_lookups.clear();
#endif
}
NativeFileIdentityScope::~NativeFileIdentityScope() {
#if defined(__APPLE__)
    if(!--identity_lookups.depth)identity_lookups.clear();
#endif
}
namespace {
#if !defined(_WIN32)
NativeFileSecurity read_native_at(int,std::string_view,bool);
std::optional<std::uint32_t> unix_id(const Bytes& sid, bool group) {
    if(sid.size()!=16 || sid[0]!=1 || sid[1]!=2 || sid[7]!=22 ||
       !std::all_of(sid.begin()+2,sid.begin()+7,[](auto b){return b==0;}))return {};
    const auto get=[&](unsigned at){std::uint32_t value{};for(unsigned i=0;i<4;++i)value|=std::uint32_t{sid[at+i]}<<(i*8);return value;};
    if(get(8)!=(group?2U:1U))return {};
    return get(12);
}
std::uint32_t file_mask(std::uint32_t mask) {
    const auto generic=mask&0xf0000000U;mask&=0x0fffffffU;
    if(generic&0x10000000U)mask|=0x1f01ffU;
    if(generic&0x80000000U)mask|=0x120089U;
    if(generic&0x40000000U)mask|=0x120116U;
    if(generic&0x20000000U)mask|=0x1200a0U;
    if(mask&~0x1f01ffU)throw SddlError{50};
    return mask;
}
#if defined(__APPLE__)
void sid_uuid(const Bytes& sid, unsigned char* uuid, const Bytes& user, const Bytes& group) {
    const auto resolve_id=[&](bool is_group,std::uint32_t id) {
        return is_group?identity_uuid(ID_TYPE_GID,&id,sizeof id,uuid):identity_uuid(ID_TYPE_UID,&id,sizeof id,uuid);
    };
    int error{};
    if(!user.empty() && sid==user)error=resolve_id(false,::geteuid());
    else if(!group.empty() && sid==group)error=resolve_id(true,::getegid());
    else if(const auto uid=unix_id(sid,false))error=resolve_id(false,*uid);
    else if(const auto gid=unix_id(sid,true))error=resolve_id(true,*gid);
    else {
        if(sid.size()<8 || sid[0]!=1 || sid[1]>15 || sid.size()!=8+4U*sid[1])throw SddlError{1337};
        nt_sid_t native{};native.sid_kind=1;native.sid_authcount=sid[1];
        std::copy(sid.begin()+2,sid.begin()+8,native.sid_authority);
        for(unsigned i=0;i<sid[1];++i)for(unsigned j=0;j<4;++j)native.sid_authorities[i]|=std::uint32_t{sid[8+i*4+j]}<<(j*8);
        error=identity_uuid(ID_TYPE_SID,&native,sizeof native,uuid);
    }
    if(error){
        // 未對應宿主帳號的客體 SID 保存在零權限描述元項目；原生 qualifier 不匹配任何宿主帳號。
        constexpr std::array<unsigned char,8> prefix{'X','6','4','A','R','S','I','D'};std::copy(prefix.begin(),prefix.end(),uuid);
        std::uint64_t hash=UINT64_C(14695981039346656037);for(const auto byte:sid)hash_byte(hash,byte);for(unsigned i=0;i<8;++i)uuid[8+i]=static_cast<unsigned char>(hash>>(i*8));
    }
}
void append_native_ace(acl_t& acl, ExplicitAce ace, const Bytes& user, const Bytes& group,const Bytes& owner,const Bytes& owner_group) {
    if(ace.type>1 || (ace.flags&~31U))throw SddlError{50};
    const auto& native_sid=(ace.sid==sddl_sid("S-1-3-4") || ace.sid==sddl_sid("CO"))?owner:ace.sid==sddl_sid("CG")?owner_group:ace.sid;
    uuid_t uuid{};sid_uuid(native_sid,uuid,user,group);auto mask=file_mask(ace.mask);
    // macOS 的 child DELETE deny 會蓋過父層 DELETE_CHILD；客體保留原 ACE 並在共用刪除流程判定。
    if(ace.type==1)mask&=~0x10000U;
    acl_entry_t entry{};
    if(::acl_create_entry(&acl,&entry)<0 || ::acl_set_tag_type(entry,ace.type?ACL_EXTENDED_DENY:ACL_EXTENDED_ALLOW)<0 ||
       ::acl_set_qualifier(entry,uuid)<0)throw SddlError{native_error()};
    acl_permset_t perms{};acl_flagset_t flags{};
    if(::acl_get_permset(entry,&perms)<0 || ::acl_get_flagset_np(entry,&flags)<0)throw SddlError{native_error()};
    static constexpr std::array mapping{
        std::pair{ACL_READ_DATA,1U},std::pair{ACL_WRITE_DATA,2U},std::pair{ACL_APPEND_DATA,4U},
        std::pair{ACL_READ_EXTATTRIBUTES,8U},std::pair{ACL_WRITE_EXTATTRIBUTES,16U},std::pair{ACL_EXECUTE,32U},
        std::pair{ACL_DELETE_CHILD,64U},std::pair{ACL_READ_ATTRIBUTES,128U},std::pair{ACL_WRITE_ATTRIBUTES,256U},
        std::pair{ACL_DELETE,0x10000U},std::pair{ACL_READ_SECURITY,0x20000U},std::pair{ACL_WRITE_SECURITY,0x40000U},
        std::pair{ACL_CHANGE_OWNER,0x80000U},std::pair{ACL_SYNCHRONIZE,0x100000U}};
    for(const auto& [native,windows]:mapping)if((mask&windows) && ::acl_add_perm(perms,native)<0)throw SddlError{native_error()};
    static constexpr std::array flag_mapping{std::pair{ACL_ENTRY_FILE_INHERIT,1U},std::pair{ACL_ENTRY_DIRECTORY_INHERIT,2U},
        std::pair{ACL_ENTRY_LIMIT_INHERIT,4U},std::pair{ACL_ENTRY_ONLY_INHERIT,8U},std::pair{ACL_ENTRY_INHERITED,16U}};
    for(const auto& [native,windows]:flag_mapping)if((ace.flags&windows) && ::acl_add_flag_np(flags,native)<0)throw SddlError{native_error()};
}
void apply_native_label(acl_t& acl,const Bytes& bytes) {
    SecurityDescriptor sd;sd.control=16;sd.data[2]=bytes;
    (void)integrity_access(VirtualToken{},sd,0,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
    acl_entry_t entry{};int cursor=ACL_FIRST_ENTRY;
    for(;;) {
        if(::acl_get_entry(acl,cursor,&entry)<0){if(errno==EINVAL)break;throw SddlError{native_error()};}
        AclAllocation qualifier{::acl_get_qualifier(entry)};if(!qualifier.value)throw SddlError{native_error()};
        if(is_label_marker(static_cast<const unsigned char*>(qualifier.value))) {
            if(::acl_delete_entry(acl,entry)<0)throw SddlError{native_error()};cursor=ACL_FIRST_ENTRY;
        }else cursor=ACL_NEXT_ENTRY;
    }
    if(bytes.empty())return;const auto parsed=parse_acl(bytes);if(parsed.offsets.empty())return;
    const auto at=parsed.offsets.front();std::uint32_t policy{},level{};
    for(unsigned i=0;i<4;++i){policy|=std::uint32_t{parsed.bytes[at+4+i]}<<(i*8);level|=std::uint32_t{parsed.bytes[at+16+i]}<<(i*8);}
    if((policy&~7U) || (level&0xfffU) || level>0x5000)throw SddlError{1336};
    uuid_t uuid{};std::copy(label_marker.begin(),label_marker.end(),uuid);uuid[8]=1;
    uuid[9]=static_cast<unsigned char>(policy);uuid[10]=static_cast<unsigned char>(level);uuid[11]=static_cast<unsigned char>(level>>8);uuid[15]=0xa5;
    if(::acl_create_entry(&acl,&entry)<0 || ::acl_set_tag_type(entry,ACL_EXTENDED_ALLOW)<0 || ::acl_set_qualifier(entry,uuid)<0)throw SddlError{native_error()};
    // 原生零權限 ACE 只保存標籤，不授予宿主檔案權限；ACL 查詢不需要內容或 xattr 讀權。
    acl_permset_t perms{};acl_flagset_t flags{};
    if(::acl_get_permset(entry,&perms)<0 || ::acl_clear_perms(perms)<0 || ::acl_get_flagset_np(entry,&flags)<0)throw SddlError{native_error()};
    static constexpr std::array mapping{std::pair{ACL_ENTRY_FILE_INHERIT,1U},std::pair{ACL_ENTRY_DIRECTORY_INHERIT,2U},
        std::pair{ACL_ENTRY_LIMIT_INHERIT,4U},std::pair{ACL_ENTRY_ONLY_INHERIT,8U},std::pair{ACL_ENTRY_INHERITED,16U}};
    for(const auto& [native,windows]:mapping)if((parsed.bytes[at+1]&windows) && ::acl_add_flag_np(flags,native)<0)throw SddlError{native_error()};
}
#endif
#endif
}
bool virtual_file_owner(const SecurityDescriptor& sd){
    const auto native=[](const Bytes& sid,bool group){
        if(sid.empty())return true;if(sid.size()!=16 || sid[0]!=1 || sid[1]!=2 || sid[2] || sid[3] || sid[4] || sid[5] || sid[6] || sid[7]!=22)return false;
        std::uint32_t kind=0;for(unsigned i=0;i<4;++i)kind|=std::uint32_t{sid[8+i]}<<(8*i);return kind==(group?2U:1U);
    };
    return !native(sd.data[0],false) || !native(sd.data[1],true);
}
void write_virtual_file_security(const HostFile& file,const SecurityDescriptor& sd,std::uint32_t information){
    NativeFileIdentityScope identity_scope;if(!file.security_writable())throw SddlError{5};
    if(information&~0xa0000017U || ((information&0xa0000000U)==0xa0000000U))throw SddlError{87};
#if defined(__APPLE__)
    struct Descriptor{int fd;~Descriptor(){if(fd>=0)::close(fd);}} descriptor{file.duplicate_descriptor()};const auto target=file.security_target();
    if(descriptor.fd<0 && !target)throw SddlError{native_error()};
    // 完整替換 DACL 時，新檔原生繼承的虛擬 SID 尚無子項投影；不先解碼即將被替換的 DACL。
    // 部分 owner／group／label 更新仍須讀取並保留原 DACL，既有完整描述元及 label 亦持續核對。
    const auto native=read_native_file_security(file,(information&4U)==0);if(native.uid!=static_cast<std::uint32_t>(::geteuid()))throw SddlError{5};
    auto effective=file_descriptor(native,0x17);
    if(information&1U){if(sd.data[0].empty())throw SddlError{1337};effective.data[0]=sd.data[0];effective.control=static_cast<std::uint16_t>((effective.control&~1U)|(sd.control&1U));}
    if(information&2U){if(sd.data[1].empty())throw SddlError{1337};effective.data[1]=sd.data[1];effective.control=static_cast<std::uint16_t>((effective.control&~2U)|(sd.control&2U));}
    if(information&4U){effective.data[3]=sd.data[3];effective.control=static_cast<std::uint16_t>((effective.control&~0x150cU)|(sd.control&0x150cU));}
    if(information&16U){effective.data[2]=sd.data[2];effective.control=static_cast<std::uint16_t>((effective.control&~0x2830U)|(sd.control&0x2830U));}
    if(information&0x80000000U)effective.control|=0x1000U;if(information&0x20000000U)effective.control&=static_cast<std::uint16_t>(~0x1000U);
    const auto bytes=relative_descriptor(effective);(void)parse_relative_descriptor(bytes);
    if(information&16U)(void)integrity_access(VirtualToken{},effective,0,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
    // 宿主只有儲存權限；Windows 的 owner／group／DACL 由同一 ACL 綁定的投影裁決。
    // 不把 SYSTEM 或其他 Windows SID 交給宿主帳號服務解析，也不執行 chown。
    AclAllocation original{target?native_acl_at(target->directory,target->leaf.c_str()): ::acl_get_fd_np(descriptor.fd,ACL_TYPE_EXTENDED)};
    if(!original.value && errno!=ENOENT)throw SddlError{native_error()};
    acl_t replacement=::acl_init(0);if(!replacement)throw SddlError{native_error()};AclAllocation allocation{replacement};
    const auto owner=unix_sid(false,native.uid),group=unix_sid(true,native.gid);
    try{append_native_ace(replacement,{0,0,0x1f01ffU,owner},{},{},owner,group);allocation.value=replacement;
        append_native_ace(replacement,{1,0,0x1f01ffU,sddl_sid("WD")},{},{},owner,group);allocation.value=replacement;
        acl_flagset_t flags{};if(::acl_get_flagset_np(replacement,&flags)<0 || ::acl_add_flag_np(flags,ACL_FLAG_NO_INHERIT)<0)throw SddlError{native_error()};
        Bytes payload(16);sddl_put(payload,0,native.uid);sddl_put(payload,4,native.gid);sddl_put(payload,8,native.directory?1U:0U);payload.insert(payload.end(),bytes.begin(),bytes.end());
        append_projection(replacement,payload,2);allocation.value=replacement;
    }catch(...){allocation.value=replacement;throw;}
    const auto assign=[&](acl_t value){return target?set_native_acl_at(file,value): ::acl_set_fd_np(descriptor.fd,value,ACL_TYPE_EXTENDED);};
    if(assign(replacement)<0)throw SddlError{native_error()};
    try{const auto after=read_native_file_security(file,true);if(!after.mapped_descriptor || *after.mapped_descriptor!=bytes)throw SddlError{1237};}
    catch(...){if(original.value)(void)assign(static_cast<acl_t>(original.value));else{AclAllocation empty{::acl_init(0)};if(empty.value)(void)assign(static_cast<acl_t>(empty.value));}throw;}
#else
    (void)sd;(void)information;throw SddlError{50};
#endif
}
void write_native_file_security(const HostFile& file, const SecurityDescriptor& sd, std::uint32_t information,
                                const Bytes& user, const Bytes& group) {
    NativeFileIdentityScope identity_scope;
    if(!file.security_writable())throw SddlError{5};
    if(information&~0xa000001fU || ((information&0xa0000000U)==0xa0000000U))throw SddlError{87};
    if(information&8U)throw SddlError{1314};
    // 此處只選擇完整虛擬描述元的儲存路徑；新建檔案的原生繼承項尚未套用客體 DACL，
    // 不要求先把這份中間狀態解碼成最終 DACL。既有 label 與完整描述元仍由共用 reader 保留。
    if(virtual_file_owner(sd) || read_native_file_security(file,false).mapped_descriptor){write_virtual_file_security(file,sd,information);return;}
#if defined(__APPLE__)
    struct Descriptor {int value;~Descriptor(){if(value>=0)::close(value);}} fd{file.duplicate_descriptor()};
    const auto target=file.security_target();if(fd.value<0 && !target)throw SddlError{native_error()};
    struct stat before{};
    if(target){const auto info=file.information();if(info.error)throw SddlError{info.error};if(::fstatat(target->directory,target->leaf.c_str(),&before,AT_SYMLINK_NOFOLLOW)<0)throw SddlError{native_error()};}
    else if(::fstat(fd.value,&before)<0)throw SddlError{native_error()};
    if(!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode))throw SddlError{50};
    auto uid=static_cast<std::uint32_t>(before.st_uid),gid=static_cast<std::uint32_t>(before.st_gid);
    if(information&16U)(void)integrity_access(VirtualToken{},sd,0,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
    if(information&1U) {
        if(!user.empty() && sd.data[0]==user)uid=static_cast<std::uint32_t>(::geteuid());
        else if(const auto id=unix_id(sd.data[0],false))uid=*id;
        else throw SddlError{1332};
    }
    if(information&2U) {
        if(!group.empty() && sd.data[1]==group)gid=static_cast<std::uint32_t>(::getegid());
        else if(const auto id=unix_id(sd.data[1],true))gid=*id;
        else throw SddlError{1332};
    }
    const bool dacl=information&4U, protection=information&0xa0000000U, label=information&16U;
    AclAllocation old{(dacl || protection || label)?(target?native_acl_at(target->directory,target->leaf.c_str()): ::acl_get_fd_np(fd.value,ACL_TYPE_EXTENDED)):nullptr};
    if((dacl || protection || label) && !old.value && errno!=ENOENT)throw SddlError{native_error()};
    acl_t replacement{};
    if(dacl) {
        replacement=::acl_init(0);if(!replacement)throw SddlError{native_error()};
    } else if(protection || label) {
        replacement=old.value?::acl_dup(static_cast<acl_t>(old.value)): ::acl_init(0);
        if(!replacement)throw SddlError{native_error()};
    }
    AclAllocation allocation{replacement};
    if(replacement) {
        // acl_create_entry 可移動配置；RAII 於每次加入後同步保存新位址。
        const auto append=[&](ExplicitAce ace){
            try {append_native_ace(replacement,std::move(ace),user,group,unix_sid(false,uid),unix_sid(true,gid));allocation.value=replacement;}
            catch(...) {allocation.value=replacement;throw;}
        };
        if(dacl) {
            std::uint8_t inherit{};
            // 原生自動繼承會先於客體子項 DACL 初始化。中繼資料能力也須繼承，
            // 否則 inherited deny 會封住新檔案的 fstat／ACL，連初始化與回滾都無法完成。
            // 已授權 handle 仍可完成時間／DOS 屬性更新；Darwin 的 fd metadata 操作會重查 ACL。
            // 此維護項不授予內容讀寫，客體的屬性操作仍依原始 DACL／handle 權限判定。
            append({0,static_cast<std::uint8_t>(S_ISDIR(before.st_mode)?3U:0U),0x60198U,unix_sid(false,uid)});
            // WRITE_DAC 的繼承更新由引擎列舉子項。原生目錄維護 ACE 僅 CI，
            // 不繼承到一般檔案，也不加入客體 DACL；一般列舉仍檢查 LIST_DIRECTORY。
            if(S_ISDIR(before.st_mode))append({0,2,1U,unix_sid(false,uid)});
            if(sd.data[3].empty())append({0,0,0x1f01ffU,sddl_sid("WD")});
            else {
                const auto acl=parse_acl(sd.data[3]);
                for(std::size_t i=0;i<acl.offsets.size();++i) {
                    const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;
                    auto ace=basic_ace(std::span{acl.bytes}.subspan(at,end-at));
                    inherit|=ace.flags&3U;append(std::move(ace));
                }
            }
            // 原生 POSIX mode 位於 extended ACL 之後；明確 deny 防止補回 Windows DACL 沒授予的權限。
            append({1,inherit,0x1f01ffU,sddl_sid("WD")});
        }
        if(dacl || label) {
            NativeFileSecurity previous;
            if(old.value){const auto copied=::acl_dup(static_cast<acl_t>(old.value));if(!copied)throw SddlError{native_error()};read_extended_acl(previous,copied,false);}
            try{apply_native_label(replacement,label?sd.data[2]:previous.mandatory_label);allocation.value=replacement;}
            catch(...){allocation.value=replacement;throw;}
        }
        if(dacl){
            Bytes canonical;
            if(!sd.data[3].empty()){
                const auto acl=parse_acl(sd.data[3]);std::vector<Bytes> entries;
                for(std::size_t i=0;i<acl.offsets.size();++i){const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;auto ace=basic_ace(std::span{acl.bytes}.subspan(at,end-at));ace.mask=file_mask(ace.mask);entries.push_back(encode_ace(ace));}
                canonical=encode_acl(entries,acl.bytes[0]);
            }
            try{append_acl_projection(replacement,canonical);allocation.value=replacement;}catch(...){allocation.value=replacement;throw;}
        }
        acl_flagset_t flags{};
        if(::acl_get_flagset_np(replacement,&flags)<0)throw SddlError{native_error()};
        bool protected_acl=sd.control&0x1000U;
        if(!dacl && old.value) {
            acl_flagset_t previous{};if(::acl_get_flagset_np(old.value,&previous)<0)throw SddlError{native_error()};
            protected_acl=flag(previous,ACL_FLAG_NO_INHERIT);
        }
        if(information&0x80000000U)protected_acl=true;
        if(information&0x20000000U)protected_acl=false;
        if((protected_acl?::acl_add_flag_np(flags,ACL_FLAG_NO_INHERIT): ::acl_delete_flag_np(flags,ACL_FLAG_NO_INHERIT))<0 ||
           ::acl_valid(replacement)<0)throw SddlError{native_error()}; // macOS 的 acl_valid_fd_np 一律 ENOTSUP。
    }
    const auto set_acl=[&](acl_t acl){return target?set_native_acl_at(file,acl): ::acl_set_fd_np(fd.value,acl,ACL_TYPE_EXTENDED);};
    const auto chown=[&](std::uint32_t owner,std::uint32_t owner_group){
        if(!target)return ::fchown(fd.value,owner,owner_group);const auto info=file.information();if(info.error){errno=EAGAIN;return -1;}
        return ::fchownat(target->directory,target->leaf.c_str(),owner,owner_group,AT_SYMLINK_NOFOLLOW);
    };
    const auto restore_acl=[&]{
        AclAllocation empty{old.value?nullptr: ::acl_init(0)};
        (void)set_acl(static_cast<acl_t>(old.value?old.value:empty.value));
    };
    // 所有結構／SID／mask 驗證都在任何宿主變更之前完成。
    const bool ownership=uid!=before.st_uid || gid!=before.st_gid;
    if(ownership && chown(uid,gid)<0)throw SddlError{native_error()};
    if(replacement && set_acl(replacement)<0) {
        const auto error=native_error();
        if(ownership)(void)chown(before.st_uid,before.st_gid);
        restore_acl();
        throw SddlError{error};
    }
    struct stat after{};
    if((target?::fstatat(target->directory,target->leaf.c_str(),&after,AT_SYMLINK_NOFOLLOW): ::fstat(fd.value,&after))<0)throw SddlError{native_error()};
    // noowners 磁碟可能忽略 chown；確認原生結果，不能回報已更換 owner。
    if(after.st_uid!=uid || after.st_gid!=gid) {
        if(ownership)(void)chown(before.st_uid,before.st_gid);
        restore_acl();throw SddlError{50};
    }
#else
    (void)sd;(void)user;(void)group;throw SddlError{50};
#endif
}
#if !defined(_WIN32)
namespace {
// 原生安全性快取：同一 inode 的 mode／ACL／xattr 投影只在 ctime 改變時重新查詢。
// chmod、chown、acl_set、fsetxattr 都會更新 ctime，本程序與其他程序的變更都會讓舊鍵失效；
// 身分 UUID→SID 的解析結果隨之保留，避免每個路徑元件都向 opendirectoryd 往返。
using SecurityCacheKey=std::tuple<std::uint64_t,std::uint64_t,std::int64_t,std::int64_t,bool>;
struct SecurityCache {
    std::mutex mutex;
    std::map<SecurityCacheKey,std::shared_ptr<const NativeFileSecurity>> entries;
    std::deque<SecurityCacheKey> order;
};
SecurityCache& security_cache(){static SecurityCache cache;return cache;}
SecurityCacheKey security_cache_key(const struct stat& info,bool with_acl){
#if defined(__APPLE__)
    const auto& changed=info.st_ctimespec;
#else
    const auto& changed=info.st_ctim;
#endif
    return {static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::int64_t>(changed.tv_sec),static_cast<std::int64_t>(changed.tv_nsec),with_acl};
}
std::shared_ptr<const NativeFileSecurity> cached_security(const SecurityCacheKey& key){
    auto& cache=security_cache();std::lock_guard<std::mutex> guard(cache.mutex);
    const auto found=cache.entries.find(key);return found==cache.entries.end()?nullptr:found->second;
}
void remember_security(const SecurityCacheKey& key,const NativeFileSecurity& value){
    auto& cache=security_cache();std::lock_guard<std::mutex> guard(cache.mutex);
    constexpr std::size_t limit=4096;
    while(cache.entries.size()>=limit && !cache.order.empty()){cache.entries.erase(cache.order.front());cache.order.pop_front();}
    if(cache.entries.emplace(key,std::make_shared<const NativeFileSecurity>(value)).second)cache.order.push_back(key);
}
}
void invalidate_native_file_security_cache(){
    auto& cache=security_cache();std::lock_guard<std::mutex> guard(cache.mutex);cache.entries.clear();cache.order.clear();
}
#else
void invalidate_native_file_security_cache(){}
#endif
NativeFileSecurity read_native_file_security(const HostFile& file, bool with_acl) {
    NativeFileIdentityScope identity_scope;
#if defined(_WIN32)
    (void)file; (void)with_acl; throw SddlError{50};
#else
    if(const auto& target=file.security_target()){
        const auto info=file.information();if(info.error)throw SddlError{info.error};return read_native_at(target->directory,target->leaf,with_acl);
    }
    struct Descriptor { int value; ~Descriptor() { if (value>=0) ::close(value); } } fd{file.duplicate_descriptor()};
    if (fd.value<0) throw SddlError{native_error()};
    struct stat info{};
    if (::fstat(fd.value,&info)<0) throw SddlError{native_error()};
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) throw SddlError{50};
    const auto cache_key=security_cache_key(info,with_acl);
    if(const auto cached=cached_security(cache_key))return *cached;
    NativeFileSecurity result;
    result.uid=static_cast<std::uint32_t>(info.st_uid); result.gid=static_cast<std::uint32_t>(info.st_gid);
    result.mode=static_cast<std::uint32_t>(info.st_mode&07777); result.directory=S_ISDIR(info.st_mode);
#if defined(__APPLE__)
    const auto acl=::acl_get_fd_np(fd.value,ACL_TYPE_EXTENDED);
    if (!acl) {
        if (errno==ENOENT || errno==ENOTSUP) {remember_security(cache_key,result);return result;} // 此檔案系統沒有 extended ACL，POSIX mode 仍是真實資料。
        throw SddlError{native_error()};
    }
    read_extended_acl(result,acl,with_acl);
    if(with_acl)read_mapped_dacl(result,fd.value);
    remember_security(cache_key,result);
    return result;
#else
    throw SddlError{50}; // 其他宿主的 extended ACL 後端尚未提供，不能省略潛在的原生 ACL。
#endif
#endif
}
namespace {
#if !defined(_WIN32)
NativeFileSecurity read_native_at(int directory, std::string_view leaf, bool with_acl) {
    NativeFileIdentityScope identity_scope;
    if (leaf.empty() || leaf.find('/')!=leaf.npos || leaf.find('\0')!=leaf.npos || leaf=="..") throw SddlError{87};
    const std::string name(leaf); struct stat before{};
    if (::fstatat(directory,name.c_str(),&before,AT_SYMLINK_NOFOLLOW)<0) throw SddlError{native_error()};
    if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode) && !S_ISLNK(before.st_mode)) throw SddlError{5};
    const auto cache_key=security_cache_key(before,with_acl);
    if(const auto cached=cached_security(cache_key))return *cached;
    NativeFileSecurity result;
    result.uid=static_cast<std::uint32_t>(before.st_uid); result.gid=static_cast<std::uint32_t>(before.st_gid);
    result.mode=static_cast<std::uint32_t>(before.st_mode&07777); result.directory=S_ISDIR(before.st_mode);
#if defined(__APPLE__)
    struct attrlist requested{}; requested.bitmapcount=ATTR_BIT_MAP_COUNT;
    requested.commonattr=ATTR_CMN_EXTENDED_SECURITY;
    std::vector<std::uint8_t> buffer(4096);
    for (;;) {
        if (::getattrlistat(directory,name.c_str(),&requested,buffer.data(),buffer.size(),FSOPT_NOFOLLOW|FSOPT_REPORT_FULLSIZE)<0) {
            const auto error=errno;
            // ExFAT 等磁碟對此屬性回覆 EINVAL；只有原生能力確認沒有 ACL 才投影 POSIX mode。
            if (error==ENOTSUP || (error==EINVAL && volume_has_no_acl(directory,name.c_str()))) break;
            throw SddlError{native_error(error)};
        }
        std::uint32_t length{}; std::memcpy(&length,buffer.data(),sizeof length);
        if (length>buffer.size()) {
            if (length>131072) throw SddlError{1344};
            buffer.resize(length); continue;
        }
        if (length<4+sizeof(attrreference_t)) throw SddlError{13};
        attrreference_t reference{}; std::memcpy(&reference,buffer.data()+4,sizeof reference);
        if (!reference.attr_length) break;
        if (reference.attr_dataoffset<static_cast<std::int32_t>(sizeof reference)) throw SddlError{13};
        const auto offset=std::size_t{4}+static_cast<std::size_t>(reference.attr_dataoffset);
        if ((offset&3U) || offset>length || reference.attr_length>length-offset || reference.attr_length<KAUTH_FILESEC_SIZE(0))
            throw SddlError{13};
        struct kauth_filesec header{};
        std::memcpy(&header,buffer.data()+offset,KAUTH_FILESEC_SIZE(0));
        if (header.fsec_magic!=KAUTH_FILESEC_MAGIC) throw SddlError{13};
        if (header.fsec_entrycount==KAUTH_FILESEC_NOACL) break;
        if (header.fsec_entrycount>4096 || KAUTH_FILESEC_SIZE(header.fsec_entrycount)>reference.attr_length)
            throw SddlError{13};
        const auto acl=::acl_copy_int_native(buffer.data()+offset);
        if (!acl) throw SddlError{native_error()};
        read_extended_acl(result,acl,with_acl); break;
    }
    // 符號連結的 ACL 以 NOFOLLOW 查詢自身；不用內容 fd，也不跟隨至 target 的 descriptor。
    if(!S_ISLNK(before.st_mode)){
    struct Descriptor {int value;~Descriptor(){if(value>=0)::close(value);}} pinned{
        ::openat(directory,name.c_str(),O_EVTONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
    if(pinned.value<0) {
        if(errno!=EACCES && errno!=EPERM)throw SddlError{native_error()};
        // 標籤已由原生 ACL 取得。內容／xattr 權限不足不妨礙 owner 查詢 READ_CONTROL。
    } else {
        struct stat identity{};
        if(::fstat(pinned.value,&identity)<0)throw SddlError{native_error()};
        if(identity.st_dev!=before.st_dev || identity.st_ino!=before.st_ino)throw SddlError{1237};
        if(with_acl)read_mapped_dacl(result,pinned.value);
    }
    }
    // stat 與 ACL 查詢皆相對於同一個目錄描述符，不跟隨 leaf symlink；外部替換／變更則回覆 retry。
    struct stat after{};
    if (::fstatat(directory,name.c_str(),&after,AT_SYMLINK_NOFOLLOW)<0) throw SddlError{native_error()};
    if (before.st_dev!=after.st_dev || before.st_ino!=after.st_ino || before.st_uid!=after.st_uid ||
        before.st_gid!=after.st_gid || before.st_mode!=after.st_mode ||
        before.st_ctimespec.tv_sec!=after.st_ctimespec.tv_sec || before.st_ctimespec.tv_nsec!=after.st_ctimespec.tv_nsec)
        throw SddlError{1237};
    remember_security(cache_key,result);
    return result;
#else
    throw SddlError{50};
#endif
}
#endif
}
SecurityDescriptor file_descriptor(const NativeFileSecurity& file, std::uint32_t information) {
    if (information&~31U) throw SddlError{87};
    if (information&8U) throw SddlError{1314};
    if(file.mapped_descriptor){auto sd=parse_relative_descriptor(*file.mapped_descriptor);
        if(!(information&1U))sd.data[0].clear();if(!(information&2U))sd.data[1].clear();if(!(information&4U)){sd.data[3].clear();sd.control&=static_cast<std::uint16_t>(~0x150cU);}
        if(!(information&16U)){sd.data[2].clear();sd.control&=static_cast<std::uint16_t>(~0x2830U);}return sd;
    }
    SecurityDescriptor sd;
    if((information&16U) && !file.mandatory_label.empty()){sd.control|=16;sd.data[2]=file.mandatory_label;}
    const auto owner=unix_sid(false,file.uid), group=unix_sid(true,file.gid), everyone=sddl_sid("WD");
    if (information&1U) sd.data[0]=owner;
    if (information&2U) sd.data[1]=group;
    if (information&4U) {
        sd.control|=static_cast<std::uint16_t>(4U|(file.protected_acl?0x1000U:0U)|(file.deferred_inherit?0x100U:0U));
        if(file.mapped_dacl) {sd.data[3]=*file.mapped_dacl;return sd;}
        std::vector<Bytes> aces;
        for (const auto& ace:file.extended) aces.push_back(encode_ace(ace));
        // 末尾的全域 deny 已封住 POSIX fallback；繼承 ACL 不得再把 mode 投影成額外 explicit ACE。
        if(!file.extended.empty()) {
            const auto& end=file.extended.back();
            if(end.type==1 && (end.mask==0x1f01ffU || end.mask==0x1e01ffU) && !(end.flags&8U) && end.sid==everyone) {
                sd.data[3]=encode_acl(aces);return sd;
            }
        }
        const auto add=[&](const Bytes& sid, unsigned bits, bool is_owner) {
            const auto mask=mode_mask(bits)|((file.directory && (bits&3U)==3U)?0x40U:0U);
            // POSIX owner 可 chmod／更新屬性；唯讀內容的 mode 不取消此能力。
            aces.push_back(encode_ace({0,0,common_mask|mask|(is_owner?0x40100U:0U),sid}));
            if (const auto denied=content_mask&~mask) aces.push_back(encode_ace({1,0,denied,sid}));
        };
        add(owner,(file.mode>>6)&7U,true); add(group,(file.mode>>3)&7U,false);
        aces.push_back(encode_ace({0,0,common_mask|mode_mask(file.mode&7U),everyone}));
        sd.data[3]=encode_acl(aces);
    }
    return sd;
}
NativeFileSecurity read_native_file_security_at(int directory,std::string_view leaf,bool with_acl){
#if !defined(_WIN32)
    return read_native_at(directory,leaf,with_acl);
#else
    (void)directory;(void)leaf;(void)with_acl;throw SddlError{50};
#endif
}
void write_native_file_label(const HostFile& file,const Bytes& bytes){
#if defined(__APPLE__)
    if(!file.security_writable())throw SddlError{5};SecurityDescriptor sd;sd.control=16;sd.data[2]=bytes;
    if(file.security_target()){write_native_file_security(file,sd,16);return;}
    (void)integrity_access(VirtualToken{},sd,0,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
    struct Descriptor{int value;~Descriptor(){if(value>=0)::close(value);}} fd{file.duplicate_descriptor()};if(fd.value<0)throw SddlError{native_error()};
    AclAllocation original{::acl_get_fd_np(fd.value,ACL_TYPE_EXTENDED)};if(!original.value && errno!=ENOENT)throw SddlError{native_error()};
    auto stage=original.value?::acl_dup(static_cast<acl_t>(original.value)): ::acl_init(0);if(!stage)throw SddlError{native_error()};AclAllocation temporary{stage};
    try{apply_native_label(stage,bytes);temporary.value=stage;}catch(...){temporary.value=stage;throw;}
    if(::acl_valid(stage)<0 || ::acl_set_fd_np(fd.value,stage,ACL_TYPE_EXTENDED)<0)throw SddlError{native_error()};
#else
    (void)file;(void)bytes;throw SddlError{50};
#endif
}
}

namespace x64arm {
DirectorySnapshot ReadOnlyFiles::list_security(std::string path) const {
    security::NativeFileIdentityScope identity_scope;
    auto view=*this;
    view.security_context_.reset();
    const auto pinned=view.pin_security(path);
    if(pinned.error){if(std::getenv("X64ARM_TRACE_SECURITY_ERRORS"))std::fprintf(stderr,"[security_list_error] phase=pin path=%s error=%u native_error=%d\n",path.c_str(),pinned.error,errno);return {{},pinned.error};}
    if(!(pinned.metadata.attributes&0x10U))return {{},267};
#if defined(__APPLE__)
    // 只升級具有有效原生 ACL 指紋的舊引擎投影。外部 ACL／POSIX 限制仍不可覆寫。
    try{
        const auto native=security::read_native_file_security(*pinned.file,true);
        const auto owner=security::unix_sid(false,native.uid);
        const bool maintained=std::any_of(native.extended.begin(),native.extended.end(),[&](const auto& ace){return !ace.type && ace.flags==2U && ace.mask==1U && ace.sid==owner;});
        if(native.mapped_dacl && !maintained){
            const auto descriptor=security::file_descriptor(native,4);
            security::write_native_file_security(*pinned.file,descriptor,4,security_context_?security_context_->primary_user:security::Bytes{},security_context_?security_context_->primary_group:security::Bytes{});
        }
    }catch(const SecurityDescriptorError& error){return {{},error.code};}
#endif
    auto result=view.list(path);
    if(result.error && std::getenv("X64ARM_TRACE_SECURITY_ERRORS"))std::fprintf(stderr,"[security_list_error] phase=list path=%s error=%u native_error=%d\n",path.c_str(),result.error,errno);
    return result;
}
OpenHostFile ReadOnlyFiles::pin_security(std::string path,bool image_metadata) const {
    security::NativeFileIdentityScope identity_scope;
    if(!writable_)return {{},{},{},5,false};
    auto resolved=resolve(std::move(path),false,nullptr,true);
#if defined(__APPLE__)
    struct Descriptor {int value;~Descriptor(){if(value>=0)::close(value);}} parent{resolved.descriptor};
    if(resolved.error)return {resolved.key,{},{},resolved.error,false};
    const auto name=resolved.security_leaf.empty()?".":resolved.security_leaf.c_str();
    if(resolved.readonly_file){
        const auto image=readonly_files_.find(resolved.key);
        if(!image_metadata || image==readonly_files_.end() || !writable_image_source(resolved.key,image->second))return {resolved.key,{},{},5,false};
        // 映像內容仍唯讀。只交付具 inode 驗證的安全描述子目標，不能用於讀寫或外部編輯。
        struct stat info{};if(::fstatat(parent.value,name,&info,AT_SYMLINK_NOFOLLOW)<0)return {resolved.key,{},{},security::native_error(),false};
        if(!S_ISREG(info.st_mode))return {resolved.key,{},{},5,false};
        auto target=std::make_shared<HostSecurityTarget>();target->leaf=name;target->identity.device=static_cast<std::uint64_t>(info.st_dev);target->identity.id=info.st_ino;target->directory=parent.value;parent.value=-1;
        auto file=std::make_shared<HostFile>(std::move(target));const auto details=file->information();return {resolved.key,std::move(file),details.metadata,details.error,true};
    }
    Descriptor fd{::openat(parent.value,name,O_EVTONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
    if(fd.value<0){
        if(errno!=EACCES && errno!=EPERM)return {resolved.key,{},{},security::native_error(),false};
        struct stat info{};if(::fstatat(parent.value,name,&info,AT_SYMLINK_NOFOLLOW)<0)return {resolved.key,{},{},security::native_error(),false};
        if(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))return {resolved.key,{},{},5,false};
        const auto device=static_cast<std::uint64_t>(info.st_dev);if(readonly_identity(device,info.st_ino))return {resolved.key,{},{},5,false};
        auto target=std::make_shared<HostSecurityTarget>();target->leaf=name;target->identity.device=device;target->identity.id=info.st_ino;target->directory=parent.value;parent.value=-1;
        auto file=std::make_shared<HostFile>(std::move(target));const auto details=file->information();return {resolved.key,std::move(file),details.metadata,details.error,true};
    }
    if(readonly_descriptor(fd.value))return {resolved.key,{},{},5,false};
    auto file=std::make_shared<HostFile>(fd.value);fd.value=-1;
    const auto info=file->information();if(info.error)return {resolved.key,{},{},info.error,false};
    return {resolved.key,std::move(file),info.metadata,0,true};
#else
    (void)resolved;(void)image_metadata;return {{},{},{},50,false};
#endif
}
security::NativeFileSecurity ReadOnlyFiles::security_information(std::string path, bool with_acl) const {
    security::NativeFileIdentityScope identity_scope;
    const auto resolved=resolve(std::move(path),false,nullptr,true);
#if defined(_WIN32)
    (void)resolved; (void)with_acl; throw SecurityDescriptorError{50};
#else
    struct Descriptor { int value; ~Descriptor() { if (value>=0) ::close(value); } } descriptor{resolved.descriptor};
    if (resolved.error) throw SecurityDescriptorError{resolved.error};
    return security::read_native_at(descriptor.value,resolved.security_leaf,with_acl);
#endif
}
std::uint32_t ReadOnlyFiles::security_access(std::string path,std::uint32_t desired)const{
    security::NativeFileIdentityScope identity_scope;
    if(!security_context_)return 0;
    const auto resolved=resolve(std::move(path),false,nullptr,true);
#if !defined(_WIN32)
    struct Descriptor{int value;~Descriptor(){if(value>=0)::close(value);}} parent{resolved.descriptor};
    if(resolved.error)return resolved.error;
    return authorize_at(parent.value,resolved.security_leaf.empty()?".":resolved.security_leaf,desired);
#else
    (void)resolved;(void)desired;return 50;
#endif
}
}
