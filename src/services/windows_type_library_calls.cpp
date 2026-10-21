#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <array>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::finish_type_library_call(GuestState& state,GuestMemory& memory,std::uint64_t value){
    auto& pending=runtime_.type_libraries.pending_unloads;
    if(pending.empty()){complete_call(state,memory,value);return std::nullopt;}
    std::vector<GuestCallback> calls;calls.reserve(pending.size());const auto target=resolve("kernel32.dll","FreeLibrary").value;
    for(const auto module:pending)calls.push_back({target,{module}});
    start_callbacks(state,memory,std::move(calls),Continuation::Return,value);pending.clear();return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::start_type_library_address(GuestState& state,GuestMemory& memory,
    std::uint64_t owner,std::uint64_t output,std::u16string dll,std::string entry,std::optional<std::uint16_t> ordinal){
    auto& view=runtime_.type_libraries.views.at(owner);
    if(view.references==UINT32_MAX || view.modules.size()>=4096)return finish_type_library_call(state,memory,0x8007000eU);
    const auto path=utf16_bytes(dll);if(path.size()>65536 || entry.size()>65535)return finish_type_library_call(state,memory,0x80070057U);
    view.modules.reserve(view.modules.size()+1);
    const auto scratch=allocate_heap(memory,40+path.size()+entry.size()+1);if(!scratch)return finish_type_library_call(state,memory,0x8007000eU);
    try{
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,40>{});memory.store(GuestAddress{scratch},64,owner);
        memory.store(GuestAddress{scratch+8},64,output);memory.store(GuestAddress{scratch+16},64,ordinal?*ordinal:scratch+40+path.size());
        memory.write(GuestAddress{scratch+40},path);std::vector<std::uint8_t> name(entry.begin(),entry.end());name.push_back(0);memory.write(GuestAddress{scratch+40+path.size()},name);
    }catch(...){(void)free_heap(memory,scratch,state);throw;}
    ++view.references;
    try{start_callbacks(state,memory,{{resolve("kernel32.dll","LoadLibraryW").value,{scratch+40}}},Continuation::TypeLibraryAddress,scratch);}
    catch(...){discard_type_library_address(state,memory,scratch);throw;}return std::nullopt;
}
void WindowsServices::discard_type_library_address(GuestState& state,GuestMemory& memory,std::uint64_t scratch){
    if(!allocations_.contains(scratch))return;
    const auto owner=memory.load(GuestAddress{scratch},64),unowned=memory.load(GuestAddress{scratch+32},64);
    if(unowned)runtime_.type_libraries.pending_unloads.push_back(unowned);
    (void)release_type_library(state,memory,owner);(void)free_heap(memory,scratch,state);
}
std::optional<std::uint32_t> WindowsServices::resume_type_library_address(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint64_t result){
    if(x86_)result&=UINT32_MAX;
    const auto finish=[&](std::uint32_t status){discard_type_library_address(state,memory,scratch);return finish_type_library_call(state,memory,status);};
    try{
        const auto phase=memory.load(GuestAddress{scratch+24},32);
        if(!phase){
            if(!result)return finish(0x80029c4aU);
            memory.store(GuestAddress{scratch+32},64,result);const auto owner=memory.load(GuestAddress{scratch},64);
            runtime_.type_libraries.views.at(owner).modules.push_back(result);memory.store(GuestAddress{scratch+32},64,0);
            memory.store(GuestAddress{scratch+24},32,1);const auto entry=memory.load(GuestAddress{scratch+16},64);
            start_callbacks(state,memory,{{resolve("kernel32.dll","GetProcAddress").value,{result,entry}}},Continuation::TypeLibraryAddress,scratch);return std::nullopt;
        }
        if(!result)return finish(0x8002802fU);
        memory.store(GuestAddress{memory.load(GuestAddress{scratch+8},64)},x86_?32:64,result);return finish(0);
    }catch(const MemoryFault&){return finish(0x80004003U);}
    catch(const std::bad_alloc&){return finish(0x8007000eU);}
    catch(...){discard_type_library_address(state,memory,scratch);throw;}
}
}
