#include "ansi_codepage.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_abi.hpp"
#include "windows_sddl.hpp"
#include <algorithm>
#include <cctype>

namespace x64arm::security {
Bytes read_sid(const GuestMemory& memory, std::uint64_t address) {
    if (!address) throw SddlError{1337};
    memory.check(GuestAddress{address},8,Access::Read);
    const auto count=memory.load(GuestAddress{address+1},8);
    if (memory.load(GuestAddress{address},8)!=1 || count>15) throw SddlError{1337};
    Bytes bytes(8+static_cast<std::size_t>(count)*4);
    memory.read(GuestAddress{address},bytes); return bytes;
}
SecurityAcl parse_acl(std::span<const std::uint8_t> bytes) {
    if (bytes.size()<8) throw SddlError{1336};
    const auto size=unsigned{bytes[2]}|(unsigned{bytes[3]}<<8);
    const auto count=unsigned{bytes[4]}|(unsigned{bytes[5]}<<8);
    if ((bytes[0]!=2 && bytes[0]!=4) || size<8 || (size&3) || size>bytes.size())
        throw SddlError{1336};
    SecurityAcl acl; acl.bytes.assign(bytes.begin(),bytes.begin()+size);
    for (unsigned i=0;i<count;++i) {
        if (acl.used+4>size) throw SddlError{1336};
        const auto length=unsigned{bytes[acl.used+2]}|(unsigned{bytes[acl.used+3]}<<8);
        if (length<4 || (length&3) || length>size-acl.used) throw SddlError{1336};
        acl.offsets.push_back(acl.used); acl.used+=length;
    }
    return acl;
}
SecurityAcl read_acl(const GuestMemory& memory, std::uint64_t address) {
    if (!address) throw SddlError{1336};
    memory.check(GuestAddress{address},8,Access::Read);
    const auto size=memory.load(GuestAddress{address+2},16);
    if (size<8) throw SddlError{1336};
    Bytes bytes(static_cast<std::size_t>(size)); memory.read(GuestAddress{address},bytes);
    return parse_acl(bytes);
}
std::array<std::uint32_t,4> read_mapping(const GuestMemory& memory, std::uint64_t address) {
    if (!address) throw SddlError{87};
    memory.check(GuestAddress{address},16,Access::Read); std::array<std::uint32_t,4> mapping{};
    for (unsigned i=0;i<4;++i) mapping[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{address+i*4},32));
    return mapping;
}
SecurityDescriptor read_descriptor(const GuestMemory& memory, std::uint64_t address, bool x86, bool contents) {
    if (!address) throw SddlError{87};
    memory.check(GuestAddress{address},4,Access::Read);
    if (memory.load(GuestAddress{address},8)!=1) throw SddlError{1305};
    SecurityDescriptor sd;
    sd.control=static_cast<std::uint16_t>(memory.load(GuestAddress{address+2},16));
    sd.rm_control=static_cast<std::uint8_t>(memory.load(GuestAddress{address+1},8));
    const bool relative=sd.control&0x8000;
    const auto first=relative || x86?4U:8U, unit=relative || x86?4U:8U;
    memory.check(GuestAddress{address},first+unit*4,Access::Read);
    for (unsigned i=0;i<4;++i) {
        if (i>=2 && !(sd.control&(i==2?16:4))) continue;
        const auto value=memory.load(GuestAddress{address+first+i*unit},static_cast<std::uint8_t>(unit*8));
        if (!value) continue;
        if (relative && (value<20 || (value&3) || value>UINT64_MAX-address)) throw SddlError{1338};
        sd.parts[i]=relative?address+value:value;
        if (contents) sd.data[i]=i<2?read_sid(memory,sd.parts[i]):read_acl(memory,sd.parts[i]).bytes;
    }
    return sd;
}
Bytes relative_descriptor(const SecurityDescriptor& sd) {
    Bytes bytes(20); bytes[0]=1; bytes[1]=sd.rm_control; sddl_put(bytes,2,sd.control|0x8000U,2);
    for (unsigned i=0;i<4;++i) if (!sd.data[i].empty() && (i<2 || (sd.control&(i==2?16:4)))) {
        if (sd.data[i].size()>UINT32_MAX-bytes.size()) throw SddlError{1344};
        sddl_put(bytes,4+i*4,static_cast<std::uint32_t>(bytes.size()));
        bytes.insert(bytes.end(),sd.data[i].begin(),sd.data[i].end());
    }
    return bytes;
}
SecurityDescriptor parse_relative_descriptor(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<20 || bytes.size()>262144 || !(bytes[3]&0x80U))throw SddlError{1338};
    try {
        GuestMemory memory;memory.map(GuestAddress{0x10000},bytes.size(),read_write,bytes);
        return read_descriptor(memory,0x10000,false,true);
    }catch(const MemoryFault&){throw SddlError{1338};}
}
SecurityDescriptor select_descriptor(const SecurityDescriptor& sd,std::uint32_t information) {
    SecurityDescriptor out;out.rm_control=sd.rm_control;
    if(information&1U){out.data[0]=sd.data[0];out.control|=sd.control&1U;}
    if(information&2U){out.data[1]=sd.data[1];out.control|=sd.control&2U;}
    if(information&4U){out.data[3]=sd.data[3];out.control|=sd.control&(4U|8U|0x100U|0x400U|0x1000U);}
    if(information&8U){out.data[2]=sd.data[2];out.control|=sd.control&(16U|32U|0x200U|0x800U|0x2000U);}
    else if((information&16U) && (sd.control&16U)) {
        // 只要求完整性標籤時，SACL 只保留 SYSTEM_MANDATORY_LABEL_ACE。
        std::vector<Bytes> labels;for(auto& ace:acl_entries(sd.data[2]))if(ace[0]==0x11)labels.push_back(std::move(ace));
        if(!labels.empty()){out.control|=16U;out.data[2]=encode_acl(labels);}
    }
    return out;
}
SecurityDescriptor inherit_descriptor(SecurityDescriptor child,const SecurityDescriptor& parent,
                                      const Bytes& owner,const Bytes& group,bool container) {
    if(child.data[0].empty())child.data[0]=owner;
    if(child.data[1].empty())child.data[1]=group;
    for(const unsigned field:{2U,3U}) {
        const auto present=field==3?4U:16U,protect=field==3?0x1000U:0x2000U,automatic=field==3?0x400U:0x800U;
        if(child.control&protect)continue;
        // 明確的 NULL ACL 保留其語義；沒有 ACL 才向父層取繼承項目。
        if((child.control&present) && child.data[field].empty())continue;
        std::vector<Bytes> entries;
        unsigned revision=2;
        if(!child.data[field].empty()) {
            revision=parse_acl(child.data[field]).bytes[0];
            for(auto& ace:acl_entries(child.data[field]))if(!(ace[1]&16U))entries.push_back(std::move(ace));
        }
        bool inherited=false;
        if(!parent.data[field].empty()) {
            revision=std::max(revision,unsigned{parse_acl(parent.data[field]).bytes[0]});
            auto items=inherited_aces(parent.data[field],child.data[0],child.data[1],{nullptr,{},container,true});
            inherited=!items.empty();
            for(auto& ace:items)entries.push_back(std::move(ace));
        }
        if(inherited || (child.control&present)) {child.control|=static_cast<std::uint16_t>(present|automatic);child.data[field]=encode_acl(entries,acl_revision(entries,revision));}
    }
    return child;
}
AceRecord read_ace(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<8)throw SddlError{1336};
    const auto size=std::size_t{bytes[2]}|(std::size_t{bytes[3]}<<8);
    if(size<8 || (size&3U) || size>bytes.size())throw SddlError{1336};
    AceRecord ace;ace.type=bytes[0];ace.flags=bytes[1];
    for(unsigned i=0;i<4;++i)ace.mask|=std::uint32_t{bytes[4+i]}<<(i*8);
    std::size_t at=8;
    if(object_ace_type(ace.type)) {
        if(size<12)throw SddlError{1336};
        AceObjects objects;for(unsigned i=0;i<4;++i)objects.flags|=std::uint32_t{bytes[8+i]}<<(i*8);
        if(objects.flags&~3U)throw SddlError{1336};at=12;
        for(const unsigned bit:{1U,2U})if(objects.flags&bit) {
            if(size<at+16)throw SddlError{1336};
            std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),16,(bit==1?objects.type:objects.inherited).begin());at+=16;
        }
        ace.objects=objects;
    }else if(ace.type>3 && ace.type!=0x11 && ace.type!=0x13 && ace.type!=0x14)throw SddlError{50}; // callback／條件式 ACE 不在此版面。
    if(size<at+8 || bytes[at]!=1 || bytes[at+1]>15 || size<at+8+4U*bytes[at+1])throw SddlError{1336};
    ace.sid.assign(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+8+4U*bytes[at+1]));
    return ace;
}
Bytes encode_ace(const ExplicitAce& ace) {
    if(ace.sid.size()<8 || ace.sid[0]!=1 || ace.sid[1]>15 || ace.sid.size()!=8+4U*ace.sid[1])throw SddlError{1337};
    Bytes bytes(8);bytes[0]=ace.type;bytes[1]=ace.flags;sddl_put(bytes,4,ace.mask);
    if(object_ace_type(ace.type)) {
        const auto objects=ace.objects.value_or(AceObjects{});if(objects.flags&~3U)throw SddlError{87};
        bytes.resize(12);sddl_put(bytes,8,objects.flags);
        if(objects.flags&1U)bytes.insert(bytes.end(),objects.type.begin(),objects.type.end());
        if(objects.flags&2U)bytes.insert(bytes.end(),objects.inherited.begin(),objects.inherited.end());
    }
    bytes.insert(bytes.end(),ace.sid.begin(),ace.sid.end());
    if(bytes.size()>65535)throw SddlError{1344};
    sddl_put(bytes,2,static_cast<std::uint32_t>(bytes.size()),2);return bytes;
}
std::vector<Bytes> acl_entries(std::span<const std::uint8_t> bytes) {
    std::vector<Bytes> entries;if(bytes.empty())return entries;
    const auto acl=parse_acl(bytes);entries.reserve(acl.offsets.size());
    for(std::size_t i=0;i<acl.offsets.size();++i) {
        const auto end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;
        entries.emplace_back(acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.offsets[i]),acl.bytes.begin()+static_cast<std::ptrdiff_t>(end));
    }
    return entries;
}
unsigned acl_revision(const std::vector<Bytes>& entries,unsigned minimum) {
    return std::any_of(entries.begin(),entries.end(),[](const Bytes& ace){return !ace.empty() && object_ace_type(ace[0]);})?4U:minimum;
}
std::vector<Bytes> inherited_aces(std::span<const std::uint8_t> parent,const Bytes& owner,const Bytes& group,const InheritOptions& options) {
    std::vector<Bytes> result;
    const auto creator_owner=sddl_sid("CO"),creator_group=sddl_sid("CG");
    const bool container=options.container;
    for(auto& bytes:acl_entries(parent)) {
        const auto flags=bytes[1];
        // OI-only 在目錄中保留為繼承模板；NP 禁止它跨越目錄再傳給檔案。
        if(container ? !(flags&2U) && (!(flags&1U) || (flags&4U)) : !(flags&1U))continue;
        auto ace=read_ace(bytes);
        std::uint8_t out=static_cast<std::uint8_t>(options.auto_inherit?(flags|16U)&~8U:flags&~24U);
        if(container && !(flags&2U))out|=8U;
        if(!container || (flags&4U))out&=static_cast<std::uint8_t>(~7U);
        // InheritedObjectType 指定的子物件型別不符時，容器只把它當作僅繼承模板往下傳。
        if(ace.objects && (ace.objects->flags&2U) &&
           std::find(options.object_types.begin(),options.object_types.end(),ace.objects->inherited)==options.object_types.end()) {
            if(!container || !(out&3U))continue;
            out|=8U;
        }
        const bool label=ace.type==0x11 || ace.type==0x13 || ace.type==0x14;
        const Bytes* substitute=label?nullptr:ace.sid==creator_owner?&owner:ace.sid==creator_group?&group:nullptr;
        const bool generic=options.mapping && !label && (ace.mask&0xf0000000U);
        if(!(out&8U) && (substitute || generic)) {
            if(container && (out&3U)){bytes[1]=static_cast<std::uint8_t>(out|8U);result.push_back(bytes);}
            if(substitute)ace.sid=*substitute;
            if(generic)ace.mask=mapped_access(ace.mask,*options.mapping);
            ace.flags=static_cast<std::uint8_t>(out&16U);result.push_back(encode_ace(ace));
        }else {bytes[1]=out;result.push_back(std::move(bytes));}
    }
    return result;
}
ExplicitAceView basic_ace_view(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes[0]>2) throw SddlError{50};
    if (bytes.size()<16 || bytes[8]!=1 || bytes[9]>15 || 16U+4U*bytes[9]>bytes.size())
        throw SddlError{1336};
    ExplicitAceView ace; ace.type=bytes[0]; ace.flags=bytes[1];
    for (unsigned i=0;i<4;++i) ace.mask|=std::uint32_t{bytes[4+i]}<<(i*8);
    ace.sid=bytes.subspan(8,8+4U*bytes[9]); return ace;
}
ExplicitAce basic_ace(std::span<const std::uint8_t> bytes) {
    const auto view=basic_ace_view(bytes);
    return {view.type,view.flags,view.mask,Bytes(view.sid.begin(),view.sid.end())};
}
Bytes encode_acl(const std::vector<Bytes>& aces, unsigned revision) {
    std::size_t size=8;
    for (const auto& ace:aces) size+=ace.size();
    if (size>65535 || aces.size()>65535) throw SddlError{1344};
    Bytes bytes(8); bytes[0]=static_cast<std::uint8_t>(revision);
    sddl_put(bytes,2,static_cast<std::uint32_t>(size),2); sddl_put(bytes,4,static_cast<std::uint32_t>(aces.size()),2);
    for (const auto& ace:aces) bytes.insert(bytes.end(),ace.begin(),ace.end());
    return bytes;
}
bool same_name(std::string_view left, std::string_view right) {
    return left.size()==right.size() && std::equal(left.begin(),left.end(),right.begin(),[](char a,char b) {
        return std::tolower(static_cast<unsigned char>(a))==std::tolower(static_cast<unsigned char>(b));
    });
}
namespace {
// 固定 well-known SID 只建立資料，不代表授予宿主帳號或權限。
constexpr std::array known_accounts{
    KnownAccount{"Everyone","",1,1,{0,0},5},
    KnownAccount{"SYSTEM","NT AUTHORITY",5,1,{18,0},5},
    KnownAccount{"LOCAL SERVICE","NT AUTHORITY",5,1,{19,0},5},
    KnownAccount{"NETWORK SERVICE","NT AUTHORITY",5,1,{20,0},5},
    KnownAccount{"Authenticated Users","NT AUTHORITY",5,1,{11,0},5},
    KnownAccount{"ANONYMOUS LOGON","NT AUTHORITY",5,1,{7,0},5},
    KnownAccount{"CREATOR OWNER","",3,1,{0,0},5},
    KnownAccount{"CREATOR GROUP","",3,1,{1,0},5},
    KnownAccount{"Administrators","BUILTIN",5,2,{32,544},4},
    KnownAccount{"Users","BUILTIN",5,2,{32,545},4},
    KnownAccount{"Guests","BUILTIN",5,2,{32,546},4}
};
}
const KnownAccount* find_account(std::string_view name) {
    const auto found=std::find_if(known_accounts.begin(),known_accounts.end(),[&](const auto& account) {
        return same_name(name,account.name) || (!account.domain.empty() &&
            same_name(name,std::string(account.domain)+"\\"+std::string(account.name)));
    });
    return found==known_accounts.end()?nullptr:&*found;
}
Bytes account_sid(const KnownAccount& account) {
    Bytes sid(8+4U*account.count); sid[0]=1; sid[1]=account.count; sid[7]=account.authority;
    for (unsigned i=0;i<account.count;++i) sddl_put(sid,8+i*4,account.subauthorities[i]);
    return sid;
}
const KnownAccount* find_account(std::span<const std::uint8_t> sid) {
    const auto found=std::find_if(known_accounts.begin(),known_accounts.end(),[&](const auto& account){
        const auto encoded=account_sid(account);return std::equal(sid.begin(),sid.end(),encoded.begin(),encoded.end());
    });
    return found==known_accounts.end()?nullptr:&*found;
}
std::optional<AccountIdentity> resolve_account(std::string_view name, std::span<const std::uint8_t> primary_sid) {
    // 虛擬使用者與 well-known SID 共用雙向查找；不查詢宿主使用者資料庫。
    if (!primary_sid.empty() && (same_name(name,"user") || same_name(name,"X64ARM\\user")))
        return AccountIdentity{{primary_sid.begin(),primary_sid.end()},"user","X64ARM",1};
    if (const auto* account=find_account(name))
        return AccountIdentity{account_sid(*account),account->name,account->domain,account->type};
    return std::nullopt;
}
std::optional<AccountIdentity> resolve_account(std::span<const std::uint8_t> sid, std::span<const std::uint8_t> primary_sid) {
    if (!primary_sid.empty() && std::equal(sid.begin(),sid.end(),primary_sid.begin(),primary_sid.end()))
        return AccountIdentity{{sid.begin(),sid.end()},"user","X64ARM",1};
    if (const auto* account=find_account(sid))
        return AccountIdentity{account_sid(*account),account->name,account->domain,account->type};
    return std::nullopt;
}
Bytes unix_sid(bool group, std::uint32_t id) {
    Bytes sid(16); sid[0]=1; sid[1]=2; sid[7]=22;
    sddl_put(sid,8,group?2U:1U); sddl_put(sid,12,id); return sid;
}
Bytes read_trustee(const GuestMemory& memory, std::uint64_t address, bool x86, bool wide,
                   const NameResolver& resolve, std::optional<AceObjects>* objects) {
    if (!address) throw SddlError{87};
    const auto layout=trustee_layout(x86); const auto bits=static_cast<std::uint8_t>(layout.unit*8);
    memory.check(GuestAddress{address},layout.size,Access::Read);
    const auto form=memory.load(GuestAddress{address+layout.unit+4},32);
    const auto name=memory.load(GuestAddress{address+layout.name_offset},bits);
    if (!name) throw SddlError{87};
    if (memory.load(GuestAddress{address},bits) || memory.load(GuestAddress{address+layout.unit},32) || form>4)
        throw SddlError{50}; // 多重 trustee（impersonation）尚未映射。
    const auto text=[&](std::uint64_t pointer) {
        return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{pointer},1024)):ansi_to_utf8(read_ansi(memory,pointer,1024));
    };
    if (form==0) return read_sid(memory,name);
    if (form==1) return resolve(text(name));
    if (form==2) throw SddlError{87}; // TRUSTEE_BAD_FORM
    // 擁有者／群組與有效權限查詢只接受 SID 或名稱；物件形式只用於 EXPLICIT_ACCESS 項目。
    if (!objects) throw SddlError{50};
    // OBJECTS_AND_SID：ObjectsPresent、ObjectTypeGuid、InheritedObjectTypeGuid、pSid；OBJECTS_AND_NAME：ObjectsPresent、ObjectType、
    // ObjectTypeName、InheritedObjectTypeName、ptstrName。名稱形式的物件型別須由目錄服務結構描述對應，本機沒有該結構描述。
    AceObjects value; value.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{name},32));
    if (value.flags&~3U) throw SddlError{87};
    Bytes sid;
    if (form==3) {
        memory.read(GuestAddress{name+4},std::span(value.type)); memory.read(GuestAddress{name+20},std::span(value.inherited));
        if (!(value.flags&1U)) value.type={};
        if (!(value.flags&2U)) value.inherited={};
        sid=read_sid(memory,memory.load(GuestAddress{name+(x86?36U:40U)},bits));
    } else {
        if (value.flags) throw SddlError{1332};
        const auto pointer=memory.load(GuestAddress{name+8+2*layout.unit},bits); if (!pointer) throw SddlError{87};
        sid=resolve(text(pointer));
    }
    if (objects) *objects=value;
    return sid;
}
std::vector<ExplicitEntry> read_explicit_entries(const GuestMemory& memory, std::uint64_t address,
                                                std::uint32_t count, bool x86, bool wide,
                                                const NameResolver& resolve) {
    if (count>4096 || (count && !address)) throw SddlError{87};
    const auto layout=trustee_layout(x86);
    if (count) memory.check(GuestAddress{address},static_cast<std::size_t>(count)*layout.entry_size,Access::Read);
    std::vector<ExplicitEntry> entries; entries.reserve(count);
    for (unsigned i=0;i<count;++i) {
        const auto at=address+std::uint64_t{i}*layout.entry_size;
        const auto mode=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32));
        const auto inheritance=memory.load(GuestAddress{at+8},32);
        if (mode<1 || mode>7 || (inheritance&~31U)) throw SddlError{87};
        ExplicitAce ace;
        ace.type=mode>=5?2:mode==3?1:0;
        ace.flags=static_cast<std::uint8_t>(inheritance|(mode==5?0x40:mode==6?0x80:mode==7?0xc0:0));
        ace.mask=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
        ace.sid=read_trustee(memory,at+layout.trustee_offset,x86,wide,resolve,&ace.objects);
        // OBJECTS_AND_* trustee 產生對應的物件 ACE（允許 5、拒絕 6、稽核 7）。
        if (ace.objects) ace.type=static_cast<std::uint8_t>(ace.type+5);
        entries.push_back({mode,std::move(ace)});
    }
    return entries;
}
Bytes merge_acl(std::span<const std::uint8_t> source, const std::vector<ExplicitEntry>& entries) {
    if (entries.empty()) return source.empty()?Bytes{}:parse_acl(source).bytes;
    unsigned revision=source.empty()?2U:unsigned{parse_acl(source).bytes[0]};
    auto aces=acl_entries(source);
    // 物件 ACE 與一般 ACE 依基本種類（允許 0、拒絕 1、稽核 2）比較，SID 與物件 GUID 都相同才視為同一 trustee。
    const auto kind=[](unsigned type){return object_ace_type(type)?type-5U:type;};
    for (const auto& entry:entries) {
        auto addition=entry.ace; bool merged=false;
        for (auto it=aces.begin();it!=aces.end();) {
            const auto old=read_ace(*it); const bool same=old.sid==addition.sid && old.objects==addition.objects;
            // REVOKE 保留 deny；SET_ACCESS 取代 allow／deny，保留審計權限。
            if (same && ((entry.mode==4 && kind(old.type)!=1) || (entry.mode==2 && kind(old.type)!=2))) {
                it=aces.erase(it); continue;
            }
            if (same && entry.mode!=2 && entry.mode!=4 && old.type==addition.type && old.flags==addition.flags) {
                addition.mask|=old.mask; *it=encode_ace(addition); merged=true;
            }
            ++it;
        }
        if (entry.mode!=4 && !merged) {
            const auto insertion=kind(addition.type)?aces.begin():std::find_if(aces.begin(),aces.end(),[&](const auto& ace) {
                return kind(ace[0])==0;
            });
            aces.insert(insertion,encode_ace(addition));
        }
    }
    return encode_acl(aces,acl_revision(aces,revision));
}
}
