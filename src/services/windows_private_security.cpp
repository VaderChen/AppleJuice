// 私有物件安全家族：CreatePrivateObjectSecurity／Ex／WithMultipleInheritance、ConvertToAutoInheritPrivateObjectSecurity、
// Get／SetPrivateObjectSecurity(Ex) 與 DestroyPrivateObjectSecurity。描述元以自我相對格式配置於程序 heap，
// 繼承、generic 對應與擁有者檢查共用 security_access／security_descriptor 的宿主實作（ntdll 的 RtlNewSecurityObject 系列轉接到這裡）。
#include "x64arm/windows_services.hpp"
#include "x64arm/security_access.hpp"
#include "windows_sddl.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> private_security_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","CreatePrivateObjectSecurity",6},{"advapi32.dll","CreatePrivateObjectSecurityEx",8},
        {"advapi32.dll","CreatePrivateObjectSecurityWithMultipleInheritance",9},
        {"advapi32.dll","ConvertToAutoInheritPrivateObjectSecurity",6},
        {"advapi32.dll","GetPrivateObjectSecurity",5},
        {"advapi32.dll","SetPrivateObjectSecurity",5},{"advapi32.dll","SetPrivateObjectSecurityEx",6},
        {"advapi32.dll","DestroyPrivateObjectSecurity",1},
    };
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_private_security(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    using namespace security;
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    std::uint64_t result=0;
    // SEF_* 的有效位元（DACL／SACL 自動繼承、型別預設、略過權限／擁有者檢查、父層預設擁有者／群組、MACL 與擁有者限制）。
    constexpr std::uint32_t valid_flags=0x1fffU;
    try {
        initialize_tokens();
        const auto descriptor_at=[&](std::uint64_t address)->std::optional<SecurityDescriptor> {
            if(!address)return std::nullopt;return read_descriptor(memory,address,x86_,true);
        };
        const auto token_at=[&](std::uint64_t handle)->const VirtualToken* {return handle?&token_handle_object(handle,8):nullptr;};
        const auto guid_at=[&](std::uint64_t address){Guid value{};memory.read(GuestAddress{address},std::span(value));return value;};
        const auto output_at=[&](std::uint64_t address){if(!address)throw SddlError{87};memory.check(GuestAddress{address},unit,Access::Write);};
        // 新描述元與 LocalAlloc 同樣記錄在本機配置表，DestroyPrivateObjectSecurity 才能驗證並釋放。
        const auto publish=[&](const SecurityDescriptor& sd,std::uint64_t output) {
            const auto bytes=relative_descriptor(sd);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)throw SddlError{8};
            memory.write(GuestAddress{pointer},bytes);runtime_.local_allocations.insert(pointer);memory.store(GuestAddress{output},bits,pointer);
            return pointer;
        };
        const auto release=[&](std::uint64_t pointer) {
            if(!pointer)return;
            if(!runtime_.local_allocations.contains(pointer) || !free_heap(memory,pointer,state))throw SddlError{87};
            runtime_.local_allocations.erase(pointer);
        };
        if(symbol.starts_with("CreatePrivateObjectSecurity")) {
            const bool extended=symbol.ends_with("Ex"),multiple=symbol.ends_with("Inheritance");
            output_at(arg(2));memory.store(GuestAddress{arg(2)},bits,0);
            NewObjectRequest request;std::vector<Guid> types;
            const unsigned container_index=multiple?5U:extended?4U:3U;
            request.container=u32(container_index)!=0;
            request.flags=multiple || extended?u32(container_index+1):0U;
            if(request.flags&~valid_flags)throw SddlError{87};
            const auto token_index=container_index+(multiple || extended?2U:1U);
            if(extended && arg(3))types.push_back(guid_at(arg(3)));
            if(multiple) {
                const auto count=u32(4);if(count>256 || (count && !arg(3)))throw SddlError{87};
                for(std::uint32_t i=0;i<count;++i){const auto pointer=memory.load(GuestAddress{arg(3)+std::uint64_t{i}*unit},bits);if(!pointer)throw SddlError{87};types.push_back(guid_at(pointer));}
            }
            const auto parent=descriptor_at(arg(0)),creator=descriptor_at(arg(1));
            request.parent=parent?&*parent:nullptr;request.creator=creator?&*creator:nullptr;request.object_types=types;
            request.mapping=read_mapping(memory,arg(token_index+1));request.token=token_at(arg(token_index));
            publish(new_object_descriptor(request),arg(2));result=1;
        }else if(symbol=="ConvertToAutoInheritPrivateObjectSecurity") {
            output_at(arg(2));memory.store(GuestAddress{arg(2)},bits,0);
            const auto current=descriptor_at(arg(1));if(!current)throw SddlError{87};
            const auto parent=descriptor_at(arg(0));std::vector<Guid> types;if(arg(3))types.push_back(guid_at(arg(3)));
            publish(auto_inherit_descriptor(*current,parent?&*parent:nullptr,types,(arg(4)&0xffU)!=0,read_mapping(memory,arg(5))),arg(2));result=1;
        }else if(symbol=="GetPrivateObjectSecurity") {
            if(!arg(4))throw SddlError{87};memory.check(GuestAddress{arg(4)},4,Access::Write);
            const auto current=descriptor_at(arg(0));if(!current)throw SddlError{87};
            const auto bytes=relative_descriptor(select_descriptor(*current,u32(1)));
            memory.store(GuestAddress{arg(4)},32,bytes.size());
            if(u32(3)<bytes.size())throw SddlError{122};if(!arg(2))throw SddlError{87};
            memory.write(GuestAddress{arg(2)},bytes);result=1;
        }else if(symbol.starts_with("SetPrivateObjectSecurity")) {
            const bool extended=symbol.ends_with("Ex");const auto flags=extended?u32(3):0U;if(flags&~valid_flags)throw SddlError{87};
            const auto information=u32(0);if(!information)throw SddlError{87};
            if(!arg(2))throw SddlError{87};memory.check(GuestAddress{arg(2)},unit,Access::Write);
            const auto previous=memory.load(GuestAddress{arg(2)},bits);
            const auto current=descriptor_at(previous),modification=descriptor_at(arg(1));if(!current || !modification)throw SddlError{87};
            const auto mapping=read_mapping(memory,arg(extended?4:3));
            const auto updated=modify_object_descriptor(*current,*modification,information,flags,mapping,token_at(arg(extended?5:4)));
            // 先配置新描述元再釋放舊的，失敗時保留原物件安全。
            publish(updated,arg(2));release(previous);result=1;
        }else if(symbol=="DestroyPrivateObjectSecurity") {
            if(!arg(0))throw SddlError{87};
            const auto pointer=memory.load(GuestAddress{arg(0)},bits);release(pointer);memory.store(GuestAddress{arg(0)},bits,0);result=1;
        }else throw HostCallFault("未知的私有物件安全服務");
    }catch(const SddlError& error){last_error_=error.code;result=0;}
    catch(const MemoryFault&){last_error_=998;result=0;}
    catch(const std::bad_alloc&){last_error_=8;result=0;}
    arg.return_to(state,result);return std::nullopt;
}
}
