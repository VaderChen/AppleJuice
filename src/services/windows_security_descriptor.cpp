#include "x64arm/windows_services.hpp"
#include "x64arm/file_security.hpp"
#include "x64arm/unicode.hpp"
#include <cstdio>
#include <cstdlib>

namespace x64arm {
security::Bytes WindowsServices::security_trustee_name(std::string_view name) {
    if (security::same_name(name,"CURRENT_USER")) {
        initialize_tokens(); return runtime_.token_objects.at(effective_token_id()).user.sid;
    }
    if (const auto account=security::find_account(name)) return security::account_sid(*account);
    if (!runtime_.network.external_io) throw SecurityDescriptorError{50};
    if (const auto sid=security::native_account_sid(name)) return *sid;
    throw SecurityDescriptorError{1332};
}
std::optional<std::uint32_t> WindowsServices::invoke_security_descriptor(
        GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
    using namespace security;
    const WindowsCall call(state,memory,service);
    const auto symbol=service.symbol;
    const auto u32=[&](unsigned i) { return static_cast<std::uint32_t>(call(i)); };
    const auto bits=static_cast<std::uint8_t>(x86_?32:64); const unsigned unit=x86_?4:8;
    const bool builder=symbol.starts_with("BuildSecurityDescriptor");
    const bool setting=symbol.starts_with("Set");
    const bool kernel=symbol=="GetKernelObjectSecurity" || symbol=="SetKernelObjectSecurity";
    const bool buffer=kernel || symbol.starts_with("GetFileSecurity") || symbol.starts_with("SetFileSecurity");
    const bool handle=kernel || symbol=="GetSecurityInfo" || symbol=="SetSecurityInfo";
    const bool wide=symbol.ends_with('W');
    const NameResolver resolve=[&](std::string_view name) { return security_trustee_name(name); };
    std::uint32_t result{};
    const char* security_stage="parameters";
    std::string security_path;
    // 輸出先檢查，再讀完輸入，最後才發布；失敗不留下配置或部分指標。
    const auto publish=[&](const Bytes& bytes, std::uint64_t descriptor, bool parts) {
        const auto output=allocate_heap(memory,bytes.size());
        if (!output) throw SecurityDescriptorError{8};
        try {
            memory.write(GuestAddress{output},bytes); runtime_.local_allocations.insert(output);
            if (parts) for (unsigned i=0;i<4;++i) {
                const auto argument=i==2?6U:i==3?5U:i+3;
                if (!call(argument)) continue;
                std::uint32_t offset{};
                for (unsigned j=0;j<4;++j) offset|=std::uint32_t{bytes[4+i*4+j]}<<(j*8);
                memory.store(GuestAddress{call(argument)},bits,offset?output+offset:0);
            }
            if (builder) memory.store(GuestAddress{call(7)},32,bytes.size());
            memory.store(GuestAddress{descriptor},bits,output);
        } catch (...) {
            runtime_.local_allocations.erase(output); free_heap(memory,output,state); throw;
        }
    };
    try {
        if((symbol.starts_with("GetNamedSecurityInfo") || symbol.starts_with("SetNamedSecurityInfo")) && u32(1)==1 && call(0)) {
            auto name=wide?read_utf16(memory,GuestAddress{call(0)}):utf8_to_utf16(read_ansi(memory,call(0)));
            for(std::size_t i=0;i<std::min<std::size_t>(9,name.size());++i)if(name[i]>=u'A' && name[i]<=u'Z')name[i]+=u'a'-u'A';
            if(name.starts_with(u"\\\\.\\pipe\\"))return invoke_object_security(state,memory,service);
        }
        if(setting) {
            const auto flags=u32(buffer?1:2);
            if(flags&~0xa000001fU || (flags&0xa0000000U)==0xa0000000U)throw SecurityDescriptorError{87};
            if(flags&8U)throw SecurityDescriptorError{1314};
            if(!buffer && (u32(1)<1 || u32(1)>13))throw SecurityDescriptorError{87};
            if(!buffer && u32(1)!=1)throw SecurityDescriptorError{50};
            if(!runtime_.network.external_io)throw SecurityDescriptorError{50};
            SecurityDescriptor sd;
            if(buffer) {
                if(!call(2))throw SecurityDescriptorError{87};
                sd=read_descriptor(memory,call(2),x86_,true);
                if((flags&4U) && !(sd.control&4U))throw SecurityDescriptorError{1338};
            } else {
                if(flags&1U)sd.data[0]=read_sid(memory,call(3));
                if(flags&2U)sd.data[1]=read_sid(memory,call(4));
                if(flags&4U) {sd.control|=4;if(call(5))sd.data[3]=read_acl(memory,call(5)).bytes;}
                if(flags&16U){sd.control|=16;if(call(6))sd.data[2]=read_acl(memory,call(6)).bytes;}
            }
            if(flags&16U){sd.control|=16;sd=security::parse_relative_descriptor(object_security_descriptor(std::move(sd),false));}
            std::shared_ptr<HostFile> file;
            std::string object_path;
            if(handle) {
                const auto found=files_.find(call(0));if(found==files_.end())throw SecurityDescriptorError{6};
                const auto access=found->second.access;
                if(((flags&0x13U) && !(access&0x10080000U)) ||
                   ((flags&0xa0000004U) && !(access&0x10040000U)))throw SecurityDescriptorError{5};
                file=found->second.native?found->second.native:found->second.identity;
                if(!file || found->second.pipe || found->second.serial || found->second.hid)throw SecurityDescriptorError{50};
                if((flags&0x20000004U) && !buffer) {
                    const auto final=filesystem_.final_path(*file,found->second.path);
                    if(final.error)throw SecurityDescriptorError{final.error};object_path=final.key;
                }
            } else {
                if(!call(0))throw SecurityDescriptorError{87};
                const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{call(0)})):read_ansi(memory,call(0));
                security_stage="pin_root";security_path=path;
                const auto opened=filesystem_.pin_security(path,true);if(opened.error)throw SecurityDescriptorError{opened.error};
                file=opened.file;object_path=opened.key;
            }
            initialize_tokens();const auto& token=runtime_.token_objects.at(1);
            if(!handle) {
                const auto required=(flags&0x13U?0x80000U:0U)|(flags&0xa0000004U?0x40000U:0U);
                security_stage="read_root_security";const auto existing=read_native_file_security(*file,true);
                security_stage="check_root_token";
                if(native_file_token_access(file_descriptor(existing,0x17),required)!=required)throw SecurityDescriptorError{5};
            }
            struct Plan {std::string path;FileMetadata identity;SecurityDescriptor sd;};
            std::vector<Plan> descendants;
            const auto decode=[](const Bytes& bytes) {
                std::vector<ExplicitAce> entries;if(bytes.empty())return entries;
                const auto acl=parse_acl(bytes);
                for(std::size_t i=0;i<acl.offsets.size();++i) {
                    const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;
                    entries.push_back(basic_ace(std::span{acl.bytes}.subspan(at,end-at)));
                }
                return entries;
            };
            const auto inherit_entries=[&](const std::vector<ExplicitAce>& inherited,const NativeFileSecurity& native) {
                std::vector<ExplicitAce> entries;
                    for(auto ace:inherited) {
                        if(native.directory) {
                            if(!(ace.flags&3U))continue;
                            const bool applies=ace.flags&2U,limited=ace.flags&4U;
                            ace.flags=static_cast<std::uint8_t>((ace.flags|16U)&~8U);
                            if(!applies)ace.flags|=8U;
                            if(limited)ace.flags&=static_cast<std::uint8_t>(~7U);
                            if((ace.flags&8U) && !(ace.flags&3U))continue;
                        } else {
                            if(!(ace.flags&1U))continue;ace.flags=16;
                        }
                        const bool creator_owner=ace.sid==security::account_sid(*security::find_account("CREATOR OWNER"));
                        const bool creator_group=ace.sid==security::account_sid(*security::find_account("CREATOR GROUP"));
                        if((creator_owner || creator_group) && !(ace.flags&8U)) {
                            if(native.directory && (ace.flags&3U)) {auto template_ace=ace;template_ace.flags|=8U;entries.push_back(std::move(template_ace));}
                            ace.sid=unix_sid(creator_group,creator_group?native.gid:native.uid);ace.flags=16;
                        }
                        entries.push_back(std::move(ace));
                    }
                return entries;
            };
            auto effective_flags=flags;
            if(!buffer && (flags&0x20000004U)) {
                const auto native=read_native_file_security(*file,true);
                if(!(flags&4U))sd=file_descriptor(native,4);
                if(!(flags&0xa0000000U) && native.protected_acl)sd.control|=0x1000U;
                if(flags&0x80000000U)sd.control|=0x1000U;
                if(flags&0x20000000U)sd.control&=static_cast<std::uint16_t>(~0x1000U);
                // 非保護 DACL 合併父目錄的繼承項；NULL DACL 仍保持完整授權契約。
                if(!(sd.control&0x1000U) && !sd.data[3].empty() && !object_path.empty()) {
                    auto entries=decode(sd.data[3]);std::erase_if(entries,[](const auto& ace){return (ace.flags&16U)!=0;});
                    const auto separator=object_path.find_last_of('/');
                    if(separator!=std::string::npos && separator>=2 && separator+1<object_path.size()) {
                        const auto parent_path=separator==2?object_path.substr(0,3):object_path.substr(0,separator);
                        const auto parent=filesystem_.security_information(parent_path,true);
                        const auto additions=inherit_entries(decode(file_descriptor(parent,4).data[3]),native);
                        entries.insert(entries.end(),additions.begin(),additions.end());
                    }
                    std::vector<Bytes> encoded;for(const auto& ace:entries)encoded.push_back(encode_ace(ace));
                    sd.data[3]=encode_acl(encoded);effective_flags|=4U;
                }
            }
            // 先驗證整棵樹的映射邊界並保存 inode，不同時保留每個子項的檔案描述元。
            // 寫入前重新核對 inode，避免大型安裝目錄耗盡 handles，也避免更新被替換的檔案。
            const auto collect=[&](auto&& self,const std::string& parent_path,const SecurityDescriptor& parent,unsigned depth)->void {
                if(depth>64)throw SecurityDescriptorError{8};
                security_stage="list_children";security_path=parent_path;
                const auto snapshot=filesystem_.list_security(parent_path);if(snapshot.error)throw SecurityDescriptorError{snapshot.error};
                const auto inherited=decode(parent.data[3]);
                for(const auto& item:snapshot.entries) {
                    if(item.name=="." || item.name==".." || (item.metadata.attributes&0x400U))continue;
                    const auto path=parent_path+"/"+item.name;
                    security_stage="pin_child";security_path=path;
                    const auto opened=filesystem_.pin_security(path,true);if(opened.error)throw SecurityDescriptorError{opened.error};
                    const auto native=read_native_file_security(*opened.file,true);
                    if(native.protected_acl)continue;
                    auto child=file_descriptor(native,4);auto entries=decode(child.data[3]);
                    std::erase_if(entries,[](const auto& ace){return (ace.flags&16U)!=0;});
                    const auto additions=inherit_entries(inherited,native);
                    entries.insert(entries.end(),additions.begin(),additions.end());
                    std::vector<Bytes> encoded;for(const auto& ace:entries)encoded.push_back(encode_ace(ace));
                    child.data[3]=encode_acl(encoded);child.control=4;
                    descendants.push_back({path,opened.metadata,child});
                    if(native.directory)self(self,path,child,depth+1);
                }
            };
            if((effective_flags&4U) && !buffer && !object_path.empty() && (file->information().metadata.attributes&0x10U))
                collect(collect,object_path,sd,0);
            // 由葉往根套用，避免新父 ACL 在本次繼承完成前阻斷子項的路徑穿越。
            security_stage="write_child_security";
            for(auto it=descendants.rbegin();it!=descendants.rend();++it){
                const auto& plan=*it;
                security_path=plan.path;
                const auto opened=filesystem_.pin_security(plan.path,true);if(opened.error)throw SecurityDescriptorError{opened.error};
                if(opened.metadata.device!=plan.identity.device || opened.metadata.id!=plan.identity.id)throw SecurityDescriptorError{1237};
                write_native_file_security(*opened.file,plan.sd,4,token.user.sid,token.primary_group);
            }
            security_stage="write_root_security";security_path=object_path;
            write_native_file_security(*file,sd,effective_flags,token.user.sid,token.primary_group);
            result=buffer?1U:0U;
        } else if (builder) {
            if (!call(7) || !call(8)) throw SecurityDescriptorError{87};
            memory.check(GuestAddress{call(7)},4,Access::Write);
            memory.check(GuestAddress{call(8)},unit,Access::Write);
            if ((u32(2) && !call(3)) || (u32(4) && !call(5))) throw SecurityDescriptorError{87};
            SecurityDescriptor sd;
            if (call(6)) {
                sd=read_descriptor(memory,call(6),x86_,true);
                if (!(sd.control&0x8000U)) throw SecurityDescriptorError{1338};
            }
            if (call(0)) {
                sd.data[0]=read_trustee(memory,call(0),x86_,wide,resolve);
                sd.control&=static_cast<std::uint16_t>(~1U);
            }
            if (call(1)) {
                sd.data[1]=read_trustee(memory,call(1),x86_,wide,resolve);
                sd.control&=static_cast<std::uint16_t>(~2U);
            }
            for (unsigned i=0;i<2;++i) {
                const unsigned array=i?5:3, count=i?4:2, field=i?2:3;
                if (!call(array)) continue;
                const auto entries=read_explicit_entries(memory,call(array),u32(count),x86_,wide,resolve);
                sd.data[field]=merge_acl(sd.data[field],entries);
                const auto present=i?16U:4U, defaulted=i?32U:8U;
                sd.control=static_cast<std::uint16_t>((sd.control&~defaulted)|present);
            }
            publish(relative_descriptor(sd),call(8),false);
        } else {
            const auto flags=u32(buffer?1:2);
            if (flags&~31U) throw SecurityDescriptorError{50};
            if (flags&8U) throw SecurityDescriptorError{1314};
            if (buffer) {
                if (!call(0) || !call(4)) throw SecurityDescriptorError{87};
                memory.check(GuestAddress{call(4)},4,Access::Write);
            } else {
                if (u32(1)<1 || u32(1)>13) throw SecurityDescriptorError{87};
                if (u32(1)!=1) throw SecurityDescriptorError{50}; // 此批映射磁碟檔案／目錄。
                if (!call(7) && (call(3) || call(4) || call(5) || call(6))) throw SecurityDescriptorError{87};
                for (unsigned i=3;i<=7;++i) if (call(i)) memory.check(GuestAddress{call(i)},unit,Access::Write);
            }
            if (!runtime_.network.external_io) throw SecurityDescriptorError{50};
            NativeFileSecurity native;
            if (handle) {
                const auto found=files_.find(call(0));
                if (found==files_.end()) throw SecurityDescriptorError{6};
                if (!(found->second.access&0xf0020000U)) throw SecurityDescriptorError{5}; // READ_CONTROL 或相應 generic access。
                const auto file=found->second.native?found->second.native:found->second.identity;
                if (!file || found->second.pipe || found->second.serial || found->second.hid) throw SecurityDescriptorError{50};
                native=read_native_file_security(*file,flags&4U);
            } else {
                if (!call(0)) throw SecurityDescriptorError{87};
                const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{call(0)})):read_ansi(memory,call(0));
                native=filesystem_.security_information(path,true);
                (void)native_file_token_access(file_descriptor(native,0x17),0x20000U);
            }
            const auto bytes=relative_descriptor(file_descriptor(native,flags));
            if (buffer) {
                if (!call(2) || u32(3)<bytes.size()) {
                    memory.store(GuestAddress{call(4)},32,bytes.size()); throw SecurityDescriptorError{122};
                }
                memory.check(GuestAddress{call(2)},bytes.size(),Access::Write);
                memory.write(GuestAddress{call(2)},bytes); memory.store(GuestAddress{call(4)},32,bytes.size()); result=1;
            } else if (call(7)) publish(bytes,call(7),true);
        }
    } catch (const SecurityDescriptorError& error) {
        if (buffer) { last_error_=error.code; result=0; } else result=error.code;
    } catch (const MemoryFault&) {
        if (buffer) { last_error_=998; result=0; } else result=998;
    } catch (const std::invalid_argument&) {
        if (buffer) { last_error_=87; result=0; } else result=87;
    } catch (const std::bad_alloc&) {
        if (buffer) { last_error_=8; result=0; } else result=8;
    }
    if(setting && (buffer?!result:result!=0) && std::getenv("X64ARM_TRACE_SECURITY_ERRORS"))std::fprintf(stderr,"[security_set_error] pid=%u symbol=%.*s stage=%s path=%s error=%u\n",process_id(),static_cast<int>(symbol.size()),symbol.data(),security_stage,security_path.c_str(),buffer?last_error_:result);
    call.return_to(state,result); return std::nullopt;
}
}
