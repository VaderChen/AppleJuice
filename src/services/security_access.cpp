#include "x64arm/security_access.hpp"
#include "windows_sddl.hpp"
#include <algorithm>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace x64arm::security {
std::uint32_t mapped_access(std::uint32_t mask,const std::array<std::uint32_t,4>& mapping){
    const auto generic=mask;mask&=0x0fffffffU;
    for(unsigned i=0;i<4;++i)if(generic&(0x80000000U>>i))mask|=mapping[i];return mask;
}
std::uint32_t creation_access(std::uint32_t desired,const AccessMapping& mapping){
    const auto mapped=mapped_access(desired&~0x02000000U,mapping);
    if(mapped&0x01000000U)throw SddlError{1314};
    if(mapped&~mapping[3])throw SddlError{87};
    return mapped|((desired&0x02000000U)?mapping[3]:0U);
}
namespace {
bool sid_matches(const VirtualToken& token,std::span<const std::uint8_t> sid,bool deny,bool restricting){
    const auto equal=[&](const Bytes& candidate){return std::equal(candidate.begin(),candidate.end(),sid.begin(),sid.end());};
    const auto matches=[&](const TokenSid& item){return equal(item.sid) && ((item.attributes&0x10)?deny:bool(item.attributes&4));};
    if(restricting)return std::any_of(token.restricted.begin(),token.restricted.end(),matches);
    if(equal(token.user.sid) && (!(token.user.attributes&0x10) || deny))return true;
    return std::any_of(token.groups.begin(),token.groups.end(),matches);
}
}
bool token_sid_matches(const VirtualToken& token,const Bytes& sid,bool deny,bool restricting){
    return sid_matches(token,sid,deny,restricting);
}
std::uint32_t token_acl_access(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,
                              const std::array<std::uint32_t,4>& mapping){
    const bool maximum=desired&0x02000000U;const auto requested=desired&~0x02000000U;
    if(!(sd.control&4) || sd.data[3].empty())return maximum?(mapping[3]|requested):requested;
    const auto& acl=sd.data[3];
    if(acl.size()<8)throw SddlError{1336};
    const auto pass=[&](bool restricted){
        std::uint32_t allowed=0,denied=0;
        const bool owner=!restricted && sid_matches(token,sd.data[0],false,false);
        bool owner_rights=false;
        static constexpr std::array<std::uint8_t,12> rights_sid{1,1,0,0,0,0,0,3,4,0,0,0};
        const auto count=unsigned{acl[4]}|(unsigned{acl[5]}<<8);std::size_t cursor=8;
        for(unsigned index=0;index<count;++index){
            if(cursor+4>acl.size())throw SddlError{1336};const auto size=std::size_t{acl[cursor+2]}|(std::size_t{acl[cursor+3]}<<8);
            if(size<4)throw SddlError{1336};if(size>acl.size()-cursor)throw SddlError{1336};
            if(!(acl[cursor+1]&8)){
                const auto ace=basic_ace_view(std::span(acl).subspan(cursor,size));
                if(ace.type>1)throw SddlError{50};
                const bool rights=std::equal(ace.sid.begin(),ace.sid.end(),rights_sid.begin(),rights_sid.end());
                if(rights)owner_rights=true;
                if((rights && owner) || sid_matches(token,ace.sid,ace.type==1,restricted)){
                    const auto mask=mapped_access(ace.mask,mapping);
                    if(ace.type==1)denied|=mask&~allowed;else allowed|=mask&~denied;
                }
            }
            cursor+=size;
        }
        if(owner && !owner_rights)allowed|=0x60000U;
        return allowed;
    };
    auto granted=pass(false);
    if(token.restriction_check){
        const auto restricted=pass(true);
        if(token.write_restricted){const auto write=mapping[3]&~(mapping[0]|mapping[2]|0x120000U);granted&=restricted|~write;}
        else granted&=restricted;
    }
    if((requested&granted)!=requested)return 0;
    return maximum?granted:requested;
}
AccessCheckResult access_check(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,const AccessMapping& mapping){
    AccessCheckResult result;
    constexpr std::uint32_t system_security=0x01000000U,write_owner=0x80000U,maximum_allowed=0x02000000U;
    auto remaining=desired;
    // 與虛擬權杖的公開 LUID 表共用 8／9；不查詢或授予宿主權限。
    const auto use=[&](std::uint32_t right,std::uint64_t luid){
        if(!(remaining&right) || !token.has_privilege(luid,true))return false;
        remaining&=~right;result.granted|=right;result.privileges.push_back({luid,0x80000000U});return true;
    };
    if((desired&system_security) && !use(system_security,8)){result.error=1314;return result;}
    (void)use(write_owner,9);
    const auto dacl_granted=token_acl_access(token,sd,remaining,mapping)&~system_security;
    const auto requested=remaining&~maximum_allowed;
    if((dacl_granted&requested)!=requested || ((remaining&maximum_allowed) && !dacl_granted && !result.granted)){
        result.granted=0;result.error=5;return result;
    }
    result.granted|=dacl_granted;return result;
}
std::uint32_t integrity_access(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t granted,const AccessMapping& mapping){
    std::uint32_t integrity=0x2000,policy=1;
    if(!sd.data[2].empty()){
        const auto acl=parse_acl(sd.data[2]);if(acl.offsets.size()>1)throw SddlError{1336};
        for(const auto at:acl.offsets){
            if(acl.bytes[at]!=0x11)throw SddlError{50};
            if((acl.bytes[at+1]&~31U) || acl.bytes[at+2]!=20 || acl.bytes[at+3] || !std::equal(acl.bytes.data()+at+8,acl.bytes.data()+at+16,sddl_sid("S-1-16-0").begin()))throw SddlError{1336};
            if(acl.bytes[at+1]&8U)continue;
            integrity=0;policy=0;for(unsigned i=0;i<4;++i){policy|=std::uint32_t{acl.bytes[at+4+i]}<<(i*8);integrity|=std::uint32_t{acl.bytes[at+16+i]}<<(i*8);}
            if(policy&~7U || (integrity&0xfffU))throw SddlError{1336};
        }
    }
    if(token.integrity<integrity && policy){
        // 依允許的 generic 類別取聯集；READ_CONTROL／SYNCHRONIZE 不屬於寫入。
        auto allowed=0x120000U;
        if(!(policy&2U))allowed|=mapping[0];
        if(!(policy&4U))allowed|=mapping[2];
        if(!(policy&1U))allowed|=mapping[1]|(mapping[3]&~(mapping[0]|mapping[2]|0x120000U));
        granted&=allowed;
    }
    return granted;
}
std::uint32_t duplicate_object_access(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t source,std::uint32_t desired,const AccessMapping& mapping){
    const auto requested=mapped_access(desired&~0x02000000U,mapping)&(mapping[3]|0x01000000U);
    if(!(desired&0x02000000U) && !(requested&~source))return requested;
    // 同步物件可在複製時重新授權；既有能力的子集不因 DACL 後續變更失效。
    // 先依物件型別移除無效位元，再比較有效權限；不把 Event 位元套用到 Mutex。
    if(requested&0x01000000U)throw SddlError{1314};
    auto granted=token_acl_access(token,sd,requested|(desired&0x02000000U),mapping);
    granted=integrity_access(token,sd,granted,mapping);
    if(desired&0x02000000U)granted&=mapping[3]|requested;
    if((granted&requested)!=requested || ((desired&0x02000000U) && !granted))throw SddlError{5};
    return (desired&0x02000000U)?granted:requested;
}
std::uint32_t object_access(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,const AccessMapping& mapping){
    const auto mapped=mapped_access(desired&~0x02000000U,mapping),all=mapping[3];
    if(mapped&0x01000000U)throw SddlError{1314};if(mapped&~all)throw SddlError{87};
    const auto granted=integrity_access(token,sd,token_acl_access(token,sd,mapped|(desired&0x02000000U),mapping)&all,mapping);
    if((granted&mapped)!=mapped || (desired&0x02000000U && !granted))throw SddlError{5};return granted;
}
std::uint32_t file_token_access(const FileTokenContext& context,const SecurityDescriptor& descriptor,std::uint32_t desired){
#if !defined(_WIN32)
    auto token=context.token;auto sd=descriptor;
    const auto alias=[&](const Bytes& sid){if(sid==context.primary_user)return unix_sid(false,static_cast<std::uint32_t>(::geteuid()));if(sid==context.primary_group)return unix_sid(true,static_cast<std::uint32_t>(::getegid()));return sid;};
    token.user.sid=alias(token.user.sid);for(auto& group:token.groups)group.sid=alias(group.sid);for(auto& group:token.restricted)group.sid=alias(group.sid);
    sd.data[0]=alias(sd.data[0]);sd.data[1]=alias(sd.data[1]);
    if(!sd.data[3].empty()){
        const auto acl=parse_acl(sd.data[3]);std::vector<Bytes> entries;
        for(std::size_t i=0;i<acl.offsets.size();++i){const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;auto ace=basic_ace(std::span{acl.bytes}.subspan(at,end-at));ace.sid=alias(ace.sid);entries.push_back(encode_ace(ace));}sd.data[3]=encode_acl(entries,acl.bytes[0]);
    }
    return object_access(token,sd,desired,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
#else
    (void)context;(void)descriptor;(void)desired;throw SddlError{50};
#endif
}
std::uint32_t file_integrity_access(const FileTokenContext& context,const SecurityDescriptor& descriptor,std::uint32_t desired){
    return integrity_access(context.token,descriptor,desired,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
}
Bytes integrity_label(std::uint32_t level,std::uint8_t flags,std::uint32_t policy){
    if((level&0xfffU) || level>0x5000 || flags&~31U || policy&~7U)throw SddlError{87};
    Bytes sid{1,1,0,0,0,0,0,16,0,0,0,0};for(unsigned i=0;i<4;++i)sid[8+i]=static_cast<std::uint8_t>(level>>(i*8));
    return encode_acl({encode_ace({0x11,flags,policy,std::move(sid)})});
}
}
