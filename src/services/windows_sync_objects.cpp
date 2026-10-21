#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
constexpr std::uint32_t all_access=0x1f0003;
std::uint32_t sync_access(std::uint32_t mask) {
    if(mask&0x10000000U)mask|=all_access;
    if(mask&0x80000000U)mask|=0x20001;
    if(mask&0x40000000U)mask|=0x20002;
    if(mask&0x20000000U)mask|=0x120000;
    if(mask&0x02000000U)mask|=all_access;
    return mask&~0xf2000000U;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_sync_object(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto api=service.symbol;
    const bool timer=api.find("WaitableTimer")!=std::string_view::npos;
    const auto kind=timer?WaitObject::Kind::Timer:WaitObject::Kind::Semaphore;
    std::uint64_t result=0;std::uint32_t error=0;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    if(api=="ReleaseSemaphore") {
        const auto handle=runtime_.wait_objects.find(arg(0));
        if(handle==runtime_.wait_objects.end() || handle->second.kind!=WaitObject::Kind::Semaphore)error=6;
        else if(!(handle->second.access&2U))error=5;
        else {
            auto& object=runtime_.wait_state(handle->second);const auto increment=std::bit_cast<std::int32_t>(u32(1));
            if(increment<=0)error=87;
            else if(u32(1)>object.maximum-object.count)error=298;
            else {if(arg(2))memory.store(GuestAddress{arg(2)},32,object.count);object.count+=u32(1);result=1;}
        }
    } else {
        const bool create=api.starts_with("Create"),extended=api.find("Ex")!=std::string_view::npos;
        const auto name_pointer=arg(create?(timer?(extended?1U:2U):3U):2U);
        auto name=name_pointer?(api.ends_with('W')?read_utf16(memory,GuestAddress{name_pointer},261):utf8_to_utf16(read_ansi(memory,name_pointer,1041))):std::u16string{};
        if(name.size()>=260)error=206;
        if(name.starts_with(u"Local\\"))name.erase(0,6);
        if(name.find(u'\\',name.starts_with(u"Global\\")?7:0)!=name.npos)error=123;
        if(!create && name.empty())error=87;
        const auto raw_access=create?(extended?u32(timer?3U:5U):all_access):u32(0);
        auto access=sync_access(raw_access);
        if(access&0x01000000U)error=1314;
        else if(access&~all_access)error=87;
        const auto flags=create?(timer?(extended?u32(2):(u32(1)?1U:0U)):(extended?u32(4):0U)):0U;
        if(flags&~(timer?3U:0U))error=87;
        bool inherit=!create && u32(1)!=0;
        if(create && !error && arg(0)){const unsigned size=x86_?12:24;memory.check(GuestAddress{arg(0)},size,Access::Read);if(memory.load(GuestAddress{arg(0)},32)!=size)error=87;else inherit=memory.load(GuestAddress{arg(0)+(x86_?8:16)},32)!=0;}
        auto existing=runtime_.wait_states.end();
        if(!error && !name.empty()) {
            existing=std::find_if(runtime_.wait_states.begin(),runtime_.wait_states.end(),[&](const auto& item){return item.second.name==name;});
            if(existing!=runtime_.wait_states.end() && existing->second.kind!=kind)error=6;
            if(std::any_of(runtime_.mutex_objects.begin(),runtime_.mutex_objects.end(),[&](const auto& item){return item.second.name==name;}))error=6;
            if(std::any_of(runtime_.wait_objects.begin(),runtime_.wait_objects.end(),[&](const auto& item){return !item.second.identity && item.second.kind==WaitObject::Kind::Event && item.second.name==name;}))error=6;
        }
        const bool existed=existing!=runtime_.wait_states.end();
        if(!error && !create && !existed)error=2;
        if(!error && create && !existed && !timer && (std::bit_cast<std::int32_t>(u32(1))<0 || std::bit_cast<std::int32_t>(u32(2))<=0 || u32(1)>u32(2)))error=87;
        if(!error && runtime_.wait_objects.size()>=process_handle_limit)error=8;
        if(!error) try {
            if(existed)access=synchronization_access(existing->second.security_descriptor,raw_access,all_access);
            const auto handle=runtime_.allocate_wait_handle(),identity=existed?existing->first:handle;
            if(!existed) {
                WaitObject object;object.kind=kind;object.name=std::move(name);object.maximum=timer?1U:u32(2);object.count=timer?0U:u32(1);object.manual_reset=timer && (flags&1U);object.security_descriptor=synchronization_descriptor(memory,arg(0),false).bytes;
                runtime_.wait_states.emplace(identity,std::move(object));
            }
            try {
                WaitObject object;object.kind=kind;object.identity=identity;object.access=access;
                runtime_.wait_objects.emplace(handle,std::move(object));if(inherit)runtime_.handle_flags.emplace(handle,1);
            } catch(...) {runtime_.wait_objects.erase(handle);runtime_.handle_flags.erase(handle);if(!existed)runtime_.wait_states.erase(identity);throw;}
            result=handle;if(create)last_error_=existed?183U:0U;
        }catch(const SecurityDescriptorError& failure){error=failure.code;}
    }
    if(error)last_error_=error;arg.return_to(state,result);return std::nullopt;
}
}
