#include "x64arm/security_descriptor.hpp"
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
SecurityDescriptor inherit_descriptor(SecurityDescriptor child,const SecurityDescriptor& parent,
                                      const Bytes& owner,const Bytes& group,bool container) {
    if(child.data[0].empty())child.data[0]=owner;
    if(child.data[1].empty())child.data[1]=group;
    const auto creator_owner=sddl_sid("CO"),creator_group=sddl_sid("CG");
    for(const unsigned field:{2U,3U}) {
        const auto present=field==3?4U:16U,protect=field==3?0x1000U:0x2000U,automatic=field==3?0x400U:0x800U;
        if(child.control&protect)continue;
        // 明確的 NULL ACL 保留其語義；沒有 ACL 才向父層取繼承項目。
        if((child.control&present) && child.data[field].empty())continue;
        std::vector<Bytes> entries;
        unsigned revision=2;
        if(!child.data[field].empty()) {
            const auto acl=parse_acl(child.data[field]);revision=acl.bytes[0];
            for(std::size_t i=0;i<acl.offsets.size();++i){const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;
                if(!(acl.bytes[at+1]&16U))entries.emplace_back(acl.bytes.begin()+static_cast<std::ptrdiff_t>(at),acl.bytes.begin()+static_cast<std::ptrdiff_t>(end));}
        }
        bool inherited=false;
        if(!parent.data[field].empty()) {
            const auto acl=parse_acl(parent.data[field]);revision=std::max(revision,unsigned{acl.bytes[0]});
            for(std::size_t i=0;i<acl.offsets.size();++i){const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;
                const auto flags=acl.bytes[at+1];
                // OI-only 在目錄中保留為繼承模板；NP 禁止它跨越目錄再傳給檔案。
                if(container ? !(flags&2U) && (!(flags&1U) || (flags&4U)) : !(flags&1U))continue;
                Bytes ace(acl.bytes.begin()+static_cast<std::ptrdiff_t>(at),acl.bytes.begin()+static_cast<std::ptrdiff_t>(end));
                ace[1]=static_cast<std::uint8_t>((flags|16U)&~8U);
                if(container && !(flags&2U))ace[1]|=8U;
                if(!container || (flags&4U))ace[1]&=static_cast<std::uint8_t>(~7U);
                if(ace[0]<=1) {
                    auto parsed=basic_ace(ace);const auto* substitute=parsed.sid==creator_owner?&child.data[0]:parsed.sid==creator_group?&child.data[1]:nullptr;
                    if(substitute && !(ace[1]&8U)) {
                        if(container && (ace[1]&3U)){auto propagating=ace;propagating[1]|=8U;entries.push_back(std::move(propagating));}
                        parsed.sid=*substitute;parsed.flags=16;ace=encode_ace(parsed);
                    }
                }else if(ace[0]!=0x11)throw SddlError{50};
                entries.push_back(std::move(ace));inherited=true;
            }
        }
        if(inherited || (child.control&present)) {child.control|=static_cast<std::uint16_t>(present|automatic);child.data[field]=encode_acl(entries,revision);}
    }
    return child;
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
Bytes encode_ace(const ExplicitAce& ace) {
    if (ace.sid.size()<8 || ace.sid[0]!=1 || ace.sid[1]>15 || ace.sid.size()!=8+4U*ace.sid[1])
        throw SddlError{1337};
    Bytes bytes(8+ace.sid.size()); bytes[0]=ace.type; bytes[1]=ace.flags;
    sddl_put(bytes,2,static_cast<std::uint32_t>(bytes.size()),2); sddl_put(bytes,4,ace.mask);
    std::copy(ace.sid.begin(),ace.sid.end(),bytes.begin()+8); return bytes;
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
Bytes unix_sid(bool group, std::uint32_t id) {
    Bytes sid(16); sid[0]=1; sid[1]=2; sid[7]=22;
    sddl_put(sid,8,group?2U:1U); sddl_put(sid,12,id); return sid;
}
Bytes read_trustee(const GuestMemory& memory, std::uint64_t address, bool x86, bool wide,
                   const NameResolver& resolve) {
    if (!address) throw SddlError{87};
    const auto layout=trustee_layout(x86); const auto bits=static_cast<std::uint8_t>(layout.unit*8);
    memory.check(GuestAddress{address},layout.size,Access::Read);
    const auto form=memory.load(GuestAddress{address+layout.unit+4},32);
    const auto name=memory.load(GuestAddress{address+layout.name_offset},bits);
    if (!name) throw SddlError{87};
    if (memory.load(GuestAddress{address},bits) || memory.load(GuestAddress{address+layout.unit},32) || form>1)
        throw SddlError{50}; // 多重 trustee 與 DS object trustee 尚未映射。
    if (!form) return read_sid(memory,name);
    return resolve(wide?utf16_to_utf8(read_utf16(memory,GuestAddress{name},1024)):read_ansi(memory,name,1024));
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
        ace.sid=read_trustee(memory,at+layout.trustee_offset,x86,wide,resolve);
        entries.push_back({mode,std::move(ace)});
    }
    return entries;
}
Bytes merge_acl(std::span<const std::uint8_t> source, const std::vector<ExplicitEntry>& entries) {
    if (entries.empty()) return source.empty()?Bytes{}:parse_acl(source).bytes;
    std::vector<Bytes> aces; unsigned revision=2;
    if (!source.empty()) {
        const auto acl=parse_acl(source); revision=acl.bytes[0];
        for (std::size_t i=0;i<acl.offsets.size();++i) {
            const auto end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;
            aces.emplace_back(acl.bytes.begin()+static_cast<std::ptrdiff_t>(acl.offsets[i]),
                              acl.bytes.begin()+static_cast<std::ptrdiff_t>(end));
        }
    }
    for (const auto& entry:entries) {
        auto addition=entry.ace; bool merged=false;
        for (auto it=aces.begin();it!=aces.end();) {
            const auto old=basic_ace(*it); const bool same=old.sid==addition.sid;
            // REVOKE 保留 deny；SET_ACCESS 取代 allow／deny，保留審計權限。
            if (same && ((entry.mode==4 && old.type!=1) || (entry.mode==2 && old.type!=2))) {
                it=aces.erase(it); continue;
            }
            if (same && entry.mode!=2 && entry.mode!=4 && old.type==addition.type && old.flags==addition.flags) {
                addition.mask|=old.mask; *it=encode_ace(addition); merged=true;
            }
            ++it;
        }
        if (entry.mode!=4 && !merged) {
            const auto insertion=addition.type?aces.begin():std::find_if(aces.begin(),aces.end(),[](const auto& ace) {
                return ace[0]==0;
            });
            aces.insert(insertion,encode_ace(addition));
        }
    }
    return encode_acl(aces,revision);
}
}
