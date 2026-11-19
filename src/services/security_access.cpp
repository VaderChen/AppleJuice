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
    return token_acl_access(token,sd,desired,mapping,{},{});
}
std::vector<std::uint32_t> token_acl_access_nodes(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,
                              const AccessMapping& mapping,std::span<const ObjectTypeNode> types,std::span<const std::uint8_t> self_sid){
    const bool maximum=desired&0x02000000U;const auto requested=desired&~0x02000000U;
    if(!(sd.control&4) || sd.data[3].empty())return std::vector<std::uint32_t>(std::max<std::size_t>(types.size(),1),maximum?(mapping[3]|requested):requested);
    const auto& acl=sd.data[3];
    if(acl.size()<8)throw SddlError{1336};
    // 物件型別樹：沒有清單時只有物件本身一個節點。
    const std::size_t nodes=std::max<std::size_t>(types.size(),1);
    const auto level=[&](std::size_t i){return types.empty()?0U:unsigned{types[i].level};};
    static constexpr std::array<std::uint8_t,12> rights_sid{1,1,0,0,0,0,0,3,4,0,0,0},principal_self{1,1,0,0,0,0,0,5,10,0,0,0};
    const auto pass=[&](bool restricted){
        std::vector<std::uint32_t> allowed(nodes),denied(nodes);
        const bool owner=!restricted && sid_matches(token,sd.data[0],false,false);
        bool owner_rights=false;
        const auto grant=[&](std::size_t i,std::uint32_t mask,bool deny){
            if(deny)denied[i]|=mask&~allowed[i];else allowed[i]|=mask&~denied[i];
        };
        const auto apply=[&](std::span<const std::uint8_t> bytes){
            if(bytes[1]&8)return;
            AceRecord object;std::span<const std::uint8_t> sid;std::uint32_t mask{};bool deny{};std::optional<std::size_t> node;
            if(bytes[0]<=1){const auto ace=basic_ace_view(bytes);sid=ace.sid;mask=ace.mask;deny=ace.type==1;}
            else if(bytes[0]==5 || bytes[0]==6){
                // 物件 ACE：未帶 ObjectType 時等同一般 ACE；帶 ObjectType 時只作用在清單中相同 GUID 的節點。
                object=read_ace(bytes);sid=object.sid;mask=object.mask;deny=bytes[0]==6;
                if(object.objects->flags&1U){
                    const auto found=std::find_if(types.begin(),types.end(),[&](const ObjectTypeNode& item){return item.type==object.objects->type;});
                    if(found==types.end())return;
                    node=static_cast<std::size_t>(found-types.begin());
                }
            }else throw SddlError{50};
            const bool rights=std::equal(sid.begin(),sid.end(),rights_sid.begin(),rights_sid.end());
            if(rights)owner_rights=true;
            if(!self_sid.empty() && std::equal(sid.begin(),sid.end(),principal_self.begin(),principal_self.end()))sid=self_sid;
            if(!(rights && owner) && !sid_matches(token,sid,deny,restricted))return;
            const auto mapped=mapped_access(mask,mapping);
            if(!node){for(std::size_t i=0;i<nodes;++i)grant(i,mapped,deny);return;}
            // 授權作用於節點及其子樹；拒絕另外上溯到所有祖先節點。
            for(std::size_t i=*node;i<nodes && (i==*node || level(i)>level(*node));++i)grant(i,mapped,deny);
            if(deny)for(std::size_t i=*node,current=level(*node);i-->0;)if(level(i)<current){grant(i,mapped,true);current=level(i);}
        };
        const auto count=unsigned{acl[4]}|(unsigned{acl[5]}<<8);std::size_t cursor=8;
        for(unsigned index=0;index<count;++index){
            if(cursor+4>acl.size())throw SddlError{1336};const auto size=std::size_t{acl[cursor+2]}|(std::size_t{acl[cursor+3]}<<8);
            if(size<4)throw SddlError{1336};if(size>acl.size()-cursor)throw SddlError{1336};
            apply(std::span(acl).subspan(cursor,size));cursor+=size;
        }
        if(owner && !owner_rights)for(auto& value:allowed)value|=0x60000U;
        // 子節點全部取得的權限同時授予父節點；清單中子節點必在父節點之後。
        for(std::size_t i=nodes;i-->0;){
            std::optional<std::uint32_t> common;
            for(std::size_t j=i+1;j<nodes && level(j)>level(i);++j)if(level(j)==level(i)+1)common=common?*common&allowed[j]:allowed[j];
            if(common)allowed[i]|=*common&~denied[i];
        }
        return allowed;
    };
    auto granted=pass(false);
    if(token.restriction_check){
        const auto restricted=pass(true);
        for(std::size_t i=0;i<nodes;++i){
            if(token.write_restricted){const auto write=mapping[3]&~(mapping[0]|mapping[2]|0x120000U);granted[i]&=restricted[i]|~write;}
            else granted[i]&=restricted[i];
        }
    }
    for(auto& value:granted)value=(requested&value)!=requested?0U:maximum?value:requested;
    return granted;
}
std::uint32_t token_acl_access(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,
                              const AccessMapping& mapping,std::span<const ObjectTypeNode> types,std::span<const std::uint8_t> self_sid){
    return token_acl_access_nodes(token,sd,desired,mapping,types,self_sid)[0];
}
AccessCheckResult access_check(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,const AccessMapping& mapping,
                               std::span<const ObjectTypeNode> types,std::span<const std::uint8_t> self_sid){
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
    const auto dacl_granted=token_acl_access(token,sd,remaining,mapping,types,self_sid)&~system_security;
    const auto requested=remaining&~maximum_allowed;
    if((dacl_granted&requested)!=requested || ((remaining&maximum_allowed) && !dacl_granted && !result.granted)){
        result.granted=0;result.error=5;return result;
    }
    result.granted|=dacl_granted;return result;
}
std::vector<AccessCheckResult> access_check_list(const VirtualToken& token,const SecurityDescriptor& sd,std::uint32_t desired,const AccessMapping& mapping,
                                                 std::span<const ObjectTypeNode> types,std::span<const std::uint8_t> self_sid){
    // 特權（ACCESS_SYSTEM_SECURITY、WRITE_OWNER）對整個物件裁決一次；DACL 依物件型別樹逐節點裁決，節點結果互不影響。
    AccessCheckResult base;
    constexpr std::uint32_t system_security=0x01000000U,write_owner=0x80000U,maximum_allowed=0x02000000U;
    auto remaining=desired;const auto nodes=std::max<std::size_t>(types.size(),1);
    const auto use=[&](std::uint32_t right,std::uint64_t luid){
        if(!(remaining&right) || !token.has_privilege(luid,true))return false;
        remaining&=~right;base.granted|=right;base.privileges.push_back({luid,0x80000000U});return true;
    };
    if((desired&system_security) && !use(system_security,8)){base.error=1314;return std::vector<AccessCheckResult>(nodes,base);}
    (void)use(write_owner,9);
    const auto per_node=token_acl_access_nodes(token,sd,remaining,mapping,types,self_sid);
    const auto requested=remaining&~maximum_allowed;
    std::vector<AccessCheckResult> results(nodes,base);
    for(std::size_t i=0;i<nodes;++i){
        const auto dacl_granted=per_node[i]&~system_security;
        if((dacl_granted&requested)!=requested || ((remaining&maximum_allowed) && !dacl_granted && !base.granted)){results[i].granted=0;results[i].error=5;}
        else results[i].granted|=dacl_granted;
    }
    return results;
}
namespace {
constexpr std::uint32_t sef_dacl_auto_inherit=1,sef_sacl_auto_inherit=2,sef_default_descriptor_for_object=4,sef_avoid_privilege_check=8,
    sef_avoid_owner_check=0x10,sef_default_owner_from_parent=0x20,sef_default_group_from_parent=0x40;
bool label_ace(std::uint8_t type){return type==0x11 || type==0x13 || type==0x14;}
// 擁有者只能是 token 使用者、具 SE_GROUP_OWNER 的已啟用群組，或持有已啟用的 SeRestorePrivilege（LUID 18）。
bool owner_assignable(const VirtualToken& token,const Bytes& owner){
    if(owner==token.user.sid || token.has_privilege(18,true))return true;
    return std::any_of(token.groups.begin(),token.groups.end(),[&](const TokenSid& group){return group.sid==owner && (group.attributes&8U) && (group.attributes&4U);});
}
// 明確 ACE：自動繼承時捨棄 INHERITED_ACE；有效 ACE 套用 generic 對應，仍要往下傳的容器 ACE 拆成僅繼承原樣與有效對應兩筆。
std::vector<Bytes> explicit_aces(std::span<const std::uint8_t> acl,bool automatic,bool container,const AccessMapping& mapping){
    std::vector<Bytes> result;
    for(auto& bytes:acl_entries(acl)){
        if(automatic && (bytes[1]&16U))continue;
        auto ace=read_ace(bytes);
        if((ace.flags&8U) || label_ace(ace.type) || !(ace.mask&0xf0000000U)){result.push_back(std::move(bytes));continue;}
        if(container && (ace.flags&3U)){bytes[1]|=8U;result.push_back(bytes);ace.flags&=static_cast<std::uint8_t>(~15U);}
        ace.mask=mapped_access(ace.mask,mapping);result.push_back(encode_ace(ace));
    }
    return result;
}
struct AclFields {std::uint16_t present,defaulted,automatic,protect;std::uint32_t sef;};
constexpr AclFields acl_fields(unsigned field){return field==3?AclFields{4,8,0x400,0x1000,sef_dacl_auto_inherit}:AclFields{16,32,0x800,0x2000,sef_sacl_auto_inherit};}
}
SecurityDescriptor new_object_descriptor(const NewObjectRequest& request){
    const auto* parent=request.parent;const auto* creator=request.creator;const auto* token=request.token;
    const auto matches=[&](const Guid& type){return std::find(request.object_types.begin(),request.object_types.end(),type)!=request.object_types.end();};
    // SEF_DEFAULT_DESCRIPTOR_FOR_OBJECT：父層若有針對此型別的物件 ACE，建立者描述元只是型別預設值，改由繼承決定。
    if(creator && parent && (request.flags&sef_default_descriptor_for_object))
        for(const unsigned field:{2U,3U})for(const auto& bytes:acl_entries(parent->data[field]))
            if(object_ace_type(bytes[0])){const auto ace=read_ace(bytes);if((ace.objects->flags&2U) && matches(ace.objects->inherited))creator=nullptr;}
    SecurityDescriptor result;
    const auto pick=[&](unsigned part,std::uint32_t from_parent)->Bytes{
        if(creator && !creator->data[part].empty()){result.control|=creator->control&(part?2U:1U);return creator->data[part];}
        if((request.flags&from_parent) && parent && !parent->data[part].empty())return parent->data[part];
        if(!token)throw SecurityDescriptorError{1008};
        return part?token->primary_group:token->owner;
    };
    result.data[0]=pick(0,sef_default_owner_from_parent);result.data[1]=pick(1,sef_default_group_from_parent);
    if(creator && !creator->data[0].empty() && token && !(request.flags&sef_avoid_owner_check) && !owner_assignable(*token,result.data[0]))
        throw SecurityDescriptorError{1307};
    for(const unsigned field:{3U,2U}){
        const auto bits=acl_fields(field);const bool automatic=request.flags&bits.sef;
        const bool given=creator && (creator->control&bits.present),defaulted=given && (creator->control&bits.defaulted);
        const bool guarded=creator && (creator->control&bits.protect);
        // 建立者自訂 SACL（完整性標籤以外）需要 SeSecurityPrivilege（LUID 8）。
        if(field==2 && given && !(request.flags&sef_avoid_privilege_check)){
            const auto entries=acl_entries(creator->data[2]);
            if(std::any_of(entries.begin(),entries.end(),[](const Bytes& ace){return !label_ace(ace[0]);})){
                if(!token)throw SecurityDescriptorError{1008};if(!token->has_privilege(8,true))throw SecurityDescriptorError{1314};
            }
        }
        std::vector<Bytes> inherited;
        if(parent && !guarded)inherited=inherited_aces(parent->data[field],result.data[0],result.data[1],{&request.mapping,request.object_types,request.container,automatic});
        std::vector<Bytes> entries;bool present=false,null_acl=false;
        if(given && !defaulted){
            // 明確 ACL 優先；只有自動繼承且未保護時才在其後附加繼承項目。
            present=true;null_acl=creator->data[field].empty();
            if(!null_acl){entries=explicit_aces(creator->data[field],automatic,request.container,request.mapping);
                if(automatic)entries.insert(entries.end(),inherited.begin(),inherited.end());}
        }else if(!inherited.empty()){present=true;entries=std::move(inherited);}
        else if(given){present=true;result.control|=bits.defaulted;null_acl=creator->data[field].empty();
            if(!null_acl)entries=explicit_aces(creator->data[field],automatic,request.container,request.mapping);}
        else if(field==3){
            // 沒有指定、繼承或建立者 DACL 時取 token 的預設 DACL。
            if(!token)throw SecurityDescriptorError{1008};
            if(!token->default_dacl.empty()){present=true;entries=explicit_aces(token->default_dacl,false,request.container,request.mapping);}
        }
        if(present){result.control|=bits.present;if(!null_acl)result.data[field]=encode_acl(entries,acl_revision(entries));if(automatic)result.control|=bits.automatic;}
        if(guarded)result.control|=bits.protect;
    }
    return result;
}
SecurityDescriptor modify_object_descriptor(SecurityDescriptor current,const SecurityDescriptor& modification,std::uint32_t information,
                                            std::uint32_t flags,const AccessMapping& mapping,const VirtualToken* token){
    if(information&1U){
        if(modification.data[0].empty())throw SecurityDescriptorError{1307};if(!token)throw SecurityDescriptorError{1008};
        if(!(flags&sef_avoid_owner_check) && !owner_assignable(*token,modification.data[0]))throw SecurityDescriptorError{1307};
        current.data[0]=modification.data[0];current.control=static_cast<std::uint16_t>((current.control&~1U)|(modification.control&1U));
    }
    if(information&2U){
        if(modification.data[1].empty())throw SecurityDescriptorError{1308};
        current.data[1]=modification.data[1];current.control=static_cast<std::uint16_t>((current.control&~2U)|(modification.control&2U));
    }
    for(const unsigned field:{3U,2U}){
        const auto bits=acl_fields(field);const std::uint32_t request=field==3?4U:8U,guard=field==3?0x80000000U:0x40000000U,release=field==3?0x20000000U:0x10000000U;
        if(!(information&request))continue;
        const bool automatic=flags&bits.sef;
        const bool guarded=(information&guard)?true:(information&release)?false:(modification.control&bits.protect)!=0;
        const auto previous=current.data[field];
        current.control=static_cast<std::uint16_t>(current.control&~(bits.present|bits.defaulted|bits.automatic|bits.protect));current.data[field].clear();
        if(modification.control&bits.present){
            current.control|=static_cast<std::uint16_t>(bits.present|(modification.control&bits.defaulted));
            if(!modification.data[field].empty()){
                // 修改描述元不知道物件是否為容器；可繼承的 generic ACE 保留原樣供子物件繼承。
                auto entries=explicit_aces(modification.data[field],automatic,true,mapping);
                if(automatic && !guarded)for(auto& ace:acl_entries(previous))if(ace[1]&16U)entries.push_back(std::move(ace));
                current.data[field]=encode_acl(entries,acl_revision(entries));
            }
        }
        if(automatic)current.control|=bits.automatic;if(guarded)current.control|=bits.protect;
    }
    if((information&16U) && !(information&8U)){
        // 只修改完整性標籤：保留其他 SACL 項目，以修改描述元的標籤取代。
        std::vector<Bytes> entries;for(auto& ace:acl_entries(current.data[2]))if(ace[0]!=0x11)entries.push_back(std::move(ace));
        for(auto& ace:acl_entries(modification.data[2]))if(ace[0]==0x11)entries.push_back(std::move(ace));
        if(!entries.empty() || (current.control&16U)){current.control|=16U;current.data[2]=encode_acl(entries,acl_revision(entries));}
    }
    return current;
}
SecurityDescriptor auto_inherit_descriptor(SecurityDescriptor current,const SecurityDescriptor* parent,std::span<const Guid> object_types,
                                           bool container,const AccessMapping& mapping){
    for(const unsigned field:{3U,2U}){
        const auto bits=acl_fields(field);
        if(!(current.control&bits.present) || (current.control&bits.automatic))continue;
        current.control|=bits.automatic;if(current.data[field].empty())continue;
        const auto inherited=parent?inherited_aces(parent->data[field],current.data[0],current.data[1],{&mapping,object_types,container,true}):std::vector<Bytes>{};
        auto entries=acl_entries(current.data[field]);std::vector<bool> used(entries.size());bool complete=true;
        // 每筆應繼承的 ACE 依序在目前 ACL 中尋找（不比較 INHERITED_ACE 旗標）；全部找到才視為由父層繼承。
        for(const auto& ace:inherited){
            bool found=false;
            for(std::size_t i=0;i<entries.size() && !found;++i){
                if(used[i] || entries[i].size()!=ace.size())continue;
                auto left=entries[i],right=ace;left[1]&=static_cast<std::uint8_t>(~16U);right[1]&=static_cast<std::uint8_t>(~16U);
                if(left==right)used[i]=found=true;
            }
            complete=complete && found;
        }
        std::vector<Bytes> result;
        if(!complete){current.control|=bits.protect;result=std::move(entries);}
        else {for(std::size_t i=0;i<entries.size();++i)if(!used[i]){entries[i][1]&=static_cast<std::uint8_t>(~16U);result.push_back(std::move(entries[i]));}
            result.insert(result.end(),inherited.begin(),inherited.end());}
        current.data[field]=encode_acl(result,acl_revision(result));
    }
    return current;
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
