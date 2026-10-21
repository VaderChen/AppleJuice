#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <map>

namespace x64arm {
// msvcp140_1.dll：MSVC STL 的 std::pmr 記憶體資源（extern "C" 匯出）。
// memory_resource 的 vtable：純量刪除解構子、do_allocate、do_deallocate、do_is_equal。
struct PmrState {
    std::array<std::uint64_t,3> resources{}; // 對齊 new_delete、未對齊 new_delete、null
    std::uint64_t default_resource{};        // 兩組 get/set 共用；0 表示改用各自的 new_delete
    std::map<std::uint64_t,std::uint64_t> aligned; // 超過 heap 對齊的配置：回傳位址→heap block
};

std::span<const WindowsServiceDescriptor> msvcp_pmr_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"msvcp140_1.dll","_Aligned_get_default_resource",0,0,0,0,true},
        {"msvcp140_1.dll","_Aligned_set_default_resource",1,0,0,0,true},
        {"msvcp140_1.dll","_Aligned_new_delete_resource",0,0,0,0,true},
        {"msvcp140_1.dll","_Unaligned_get_default_resource",0,0,0,0,true},
        {"msvcp140_1.dll","_Unaligned_set_default_resource",1,0,0,0,true},
        {"msvcp140_1.dll","_Unaligned_new_delete_resource",0,0,0,0,true},
        {"msvcp140_1.dll","null_memory_resource",0,0,0,0,true},
        // Win32 為 thiscall（this 在 ECX，不計入堆疊參數）；x64 參數含 this。
        {"internal-msvcp140_1","PmrDelete32",1},{"internal-msvcp140_1","PmrDelete64",2},
        {"internal-msvcp140_1","PmrAllocate32",2},{"internal-msvcp140_1","PmrAllocate64",3},
        {"internal-msvcp140_1","PmrDeallocate32",3},{"internal-msvcp140_1","PmrDeallocate64",4},
        {"internal-msvcp140_1","PmrIsEqual32",1},{"internal-msvcp140_1","PmrIsEqual64",2}};
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_msvcp_pmr(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const std::string_view symbol=service.symbol;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    // Windows heap 與 __STDCPP_DEFAULT_NEW_ALIGNMENT__：x64 為 16，Win32 為 8。
    const std::uint64_t heap_alignment=x86_?8:16;
    if(!msvcp_pmr_){
        auto created=std::make_shared<PmrState>();const std::string suffix=x86_?"32":"64";
        // vtable 前一格為 RTTI complete object locator（不提供，dynamic_cast 不適用於這些內部型別）。
        const auto storage=runtime_allocate(memory,{},5*unit),table=storage+unit;unsigned slot=0;
        for(const auto* name:{"PmrDelete","PmrAllocate","PmrDeallocate","PmrIsEqual"})
            memory.store(GuestAddress{table+slot++*unit},bits,resolve("internal-msvcp140_1",std::string(name)+suffix).value);
        for(auto& resource:created->resources){resource=runtime_allocate(memory,{},unit,unit);memory.store(GuestAddress{resource},bits,table);}
        msvcp_pmr_=std::move(created);
    }
    auto& pmr=*msvcp_pmr_;
    const auto done=[&](std::uint64_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return std::nullopt;};
    const bool aligned_family=symbol.starts_with("_Aligned_");
    const auto fallback=pmr.resources[aligned_family?0:1];
    if(symbol.ends_with("_new_delete_resource"))return done(fallback);
    if(symbol=="null_memory_resource")return done(pmr.resources[2]);
    if(symbol.ends_with("_get_default_resource"))return done(pmr.default_resource?pmr.default_resource:fallback);
    if(symbol.ends_with("_set_default_resource")){
        const auto previous=pmr.default_resource;pmr.default_resource=arg(0);
        return done(previous?previous:fallback);
    }
    const auto self=x86_?state.reg(Gpr::Rcx)&UINT32_MAX:arg(0);
    const auto a=[&](unsigned index){return arg(index+(x86_?0:1));};
    const auto kind=static_cast<std::size_t>(std::find(pmr.resources.begin(),pmr.resources.end(),self)-pmr.resources.begin());
    if(kind>=pmr.resources.size())throw HostCallFault("memory_resource 物件不是 msvcp140_1 提供的資源");
    const auto method=symbol.substr(0,symbol.size()-2);
    if(method=="PmrDelete")return done(self); // 靜態資源不釋放；回傳 this 與 MSVC 純量刪除解構子一致。
    if(method=="PmrIsEqual")return done(self==a(0));
    if(method=="PmrAllocate"){
        const auto bytes=a(0),alignment=a(1);
        // 對齊須為 2 的冪；未對齊版本（/Zc:alignedNew-）不支援超過預設 new 對齊；null 資源一律失敗。
        const bool valid=std::has_single_bit(alignment) && (kind==0 || alignment<=heap_alignment) && kind!=2;
        const auto size=std::max<std::uint64_t>(bytes,1);
        if(valid && alignment<=heap_alignment){if(const auto at=allocate_heap(memory,size))return done(at);}
        else if(valid && size<=UINT64_MAX-alignment){
            if(const auto base=allocate_heap(memory,size+alignment)){
                const auto at=(base+alignment)&~(alignment-1);pmr.aligned.emplace(at,base);return done(at);
            }
        }
        throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return std::nullopt;
    }
    if(method=="PmrDeallocate"){
        const auto pointer=a(0);
        if(kind==2 || !pointer)return done(0);
        auto base=pointer;
        if(const auto found=pmr.aligned.find(pointer);found!=pmr.aligned.end()){base=found->second;pmr.aligned.erase(found);}
        if(!free_heap(memory,base,state))throw HostCallFault("memory_resource::deallocate 收到無效配置");
        return done(0);
    }
    throw HostCallFault("未知的 msvcp140_1 服務");
}
}
