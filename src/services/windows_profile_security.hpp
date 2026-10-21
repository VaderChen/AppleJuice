#pragma once
#include "x64arm/file_security.hpp"
#include "x64arm/readonly_files.hpp"

namespace x64arm {
// 系統建立的使用者 profile 需要可繼承的使用者權限。否則程式加入自己的
// capability ACE 後，新子項只會繼承 capability，連建立它的桌面程序也無法重開。
// 僅升級尚未設定 Windows ACL、或舊引擎單一預設使用者 ACE 的 profile 根目錄；
// 自訂／保護 ACL 維持原狀。子項共用一般 Windows 繼承規則，保留明確的 deny。
inline void initialize_windows_profile_security(ReadOnlyFiles& files, const std::string& path,
                                                const security::Bytes& user,
                                                const security::Bytes& group) {
#if defined(__APPLE__)
    using namespace security;
    const auto pinned=files.pin_security(path);
    if(pinned.error)throw SecurityDescriptorError{pinned.error};
    const auto native=read_native_file_security(*pinned.file,true);
    if(!native.directory || native.protected_acl)return;
    if(native.mapped_dacl) {
        if(native.mapped_dacl->empty())return; // 明確的 NULL DACL 不是預設 profile。
        const auto acl=parse_acl(*native.mapped_dacl);
        if(acl.offsets.size()!=1)return;
        const auto ace=basic_ace(std::span{acl.bytes}.subspan(acl.offsets.front()));
        if(ace.type || ace.flags || ace.sid!=user ||
           (ace.mask!=0x10000000U && ace.mask!=0x1f01ffU))return;
    } else if(!native.extended.empty())return;

    auto profile=file_descriptor(native,0x17);
    profile.control|=4U;
    profile.data[3]=encode_acl({encode_ace({0,3,0x1f01ffU,user})});
    struct Planned {std::string path;FileMetadata identity;SecurityDescriptor descriptor;};
    std::vector<Planned> plans;
    // 先完成整棵樹的讀取與驗證，再由葉往根套用；不持有每個檔案的 FD。
    // 根目錄最後更新，所以中途失敗後仍可在下一次初始化重試。
    const auto collect=[&](auto&& self,const std::string& parent_path,
                          const SecurityDescriptor& parent,unsigned depth)->void {
        if(depth>64)throw SecurityDescriptorError{8};
        const auto children=files.list_security(parent_path);
        if(children.error)throw SecurityDescriptorError{children.error};
        for(const auto& child:children.entries) {
            if(child.name=="." || child.name==".." || (child.metadata.attributes&0x400U))continue;
            const auto child_path=parent_path+"/"+child.name;
            const auto opened=files.pin_security(child_path);
            if(opened.error)throw SecurityDescriptorError{opened.error};
            const auto child_native=read_native_file_security(*opened.file,true);
            if(child_native.protected_acl)continue;
            auto descriptor=file_descriptor(child_native,0x17);
            const auto owner=descriptor.data[0],primary_group=descriptor.data[1];
            descriptor=inherit_descriptor(std::move(descriptor),parent,
                                          owner,primary_group,child_native.directory);
            plans.push_back({child_path,opened.metadata,descriptor});
            if(child_native.directory)self(self,child_path,descriptor,depth+1);
        }
    };
    collect(collect,path,profile,0);
    plans.insert(plans.begin(),{path,pinned.metadata,profile});
    for(auto it=plans.rbegin();it!=plans.rend();++it) {
        const auto opened=files.pin_security(it->path);
        if(opened.error)throw SecurityDescriptorError{opened.error};
        if(opened.metadata.device!=it->identity.device || opened.metadata.id!=it->identity.id)
            throw SecurityDescriptorError{1237};
        write_native_file_security(*opened.file,it->descriptor,4,user,group);
    }
#else
    (void)files;(void)path;(void)user;(void)group;
#endif
}
}
