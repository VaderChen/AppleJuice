#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <new>

namespace x64arm {
namespace {
// 表格由客體持有；快照只供當次查詢使用，不能代替呼叫者後續寫入的資料。
void refresh_table(const GuestMemory& memory,RuntimeFunctionTable& table) {
    auto& image=table.image;
    memory.check(GuestAddress{image.function_table},static_cast<std::size_t>(table.count)*12,Access::Read);
    std::vector<RuntimeFunction> functions;functions.reserve(table.count);
    std::uint32_t previous_begin=0,previous_end=0;
    for(std::uint32_t i=0;i<table.count;++i) {
        const auto at=image.function_table+static_cast<std::uint64_t>(i)*12;
        RuntimeFunction row{static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)),
            static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{at+8},32))};
        const bool folded=!functions.empty() && row.begin==previous_begin && row.end==previous_end;
        if(row.begin>row.end || row.begin<previous_begin || (row.begin<row.end && row.begin<previous_end && !folded) || row.end>image.size || row.unwind>=image.size)
            throw HostCallFault("動態 RUNTIME_FUNCTION 必須排序、具有有效 RVA，重疊時範圍必須完全相同");
        previous_begin=row.begin;if(row.begin<row.end)previous_end=row.end;functions.push_back(row);
    }
    image.functions=std::move(functions);
}
}
const RuntimeImage* WindowsServices::unwind_image(GuestMemory& memory,std::uint64_t pc,std::uint64_t function_entry) {
    const auto* module=module_image(pc,true);
    if(module && lookup_function(*module,pc,function_entry))return module;
    if(!x86_)for(auto& table:runtime_.dynamic_function_tables) {
        auto& image=table.image;
        const auto address=image.function_table;
        if(pc<image.base || pc>=table.range_end)continue;
        if(function_entry && (function_entry<address || (function_entry-address)%12 || (function_entry-address)/12>=table.count))continue;
        refresh_table(memory,table);
        if(lookup_function(image,pc,function_entry))return &image;
    }
    return function_entry?nullptr:module;
}
std::optional<std::uint32_t> WindowsServices::invoke_function_table(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto address=arg(0);bool result=false;
    if(service.symbol=="RtlAddGrowableFunctionTable") {
        std::uint32_t status=0xc000000dU;
        const auto entries=arg(1),base=arg(4),end=arg(5);
        const auto count=static_cast<std::uint32_t>(arg(2)),capacity=static_cast<std::uint32_t>(arg(3));
        if(!x86_ && address && entries && count<=capacity && capacity && capacity<=memory.limit()/12 &&
           end>base && end-base<=UINT64_C(0x100000000) && entries<=UINT64_MAX-static_cast<std::uint64_t>(capacity)*12) {
            try {
                memory.check(GuestAddress{address},8,Access::Write);
                memory.check(GuestAddress{entries},static_cast<std::size_t>(capacity)*12,Access::Read);
                RuntimeFunctionTable table;table.count=count;table.capacity=capacity;table.range_end=end;
                table.image.base=base;table.image.size=end-base;table.image.function_table=entries;
                refresh_table(memory,table);
                table.handle=runtime_.next_function_table_handle++;
                runtime_.dynamic_function_tables.push_back(std::move(table));
                memory.store(GuestAddress{address},64,runtime_.dynamic_function_tables.back().handle);status=0;
            }catch(const MemoryFault&) {status=0xc0000005U;}
             catch(const HostCallFault&) {status=0xc000000dU;}
             catch(const std::bad_alloc&) {status=0xc0000017U;}
        }
        arg.return_to(state,status);return std::nullopt;
    }
    if(service.symbol=="RtlGrowFunctionTable" || service.symbol=="RtlDeleteGrowableFunctionTable") {
        const auto found=std::find_if(runtime_.dynamic_function_tables.begin(),runtime_.dynamic_function_tables.end(),[&](const auto& table){return table.handle && table.handle==address;});
        if(x86_ || found==runtime_.dynamic_function_tables.end())throw HostCallFault("可成長函式表參照無效");
        if(service.symbol=="RtlDeleteGrowableFunctionTable")runtime_.dynamic_function_tables.erase(found);
        else {
            const auto count=static_cast<std::uint32_t>(arg(1));
            if(count<=found->count || count>found->capacity)throw HostCallFault("可成長函式表必須增加筆數且不超過容量");
            auto candidate=*found;candidate.count=count;refresh_table(memory,candidate);*found=std::move(candidate);
        }
        arg.return_to(state,0);return std::nullopt;
    }
    if(!x86_ && service.symbol=="RtlDeleteFunctionTable") {
        const auto found=std::find_if(runtime_.dynamic_function_tables.begin(),runtime_.dynamic_function_tables.end(),[&](const auto& table){return !table.handle && table.image.function_table==address;});
        if(found!=runtime_.dynamic_function_tables.end()){runtime_.dynamic_function_tables.erase(found);result=true;}
    }
    else if(!x86_ && service.symbol=="RtlAddFunctionTable") {
        const auto count=static_cast<std::uint32_t>(arg(1));const auto base=arg(2);
        if((address || !count) && count<=65536 && address<=UINT64_MAX-static_cast<std::uint64_t>(count)*12) {
            RuntimeFunctionTable table;table.count=count;table.image.base=base;table.image.function_table=address;
            table.image.size=std::min(UINT64_C(0x100000000),UINT64_MAX-base);
            try {
                refresh_table(memory,table);table.range_end=base+(count?table.image.functions.back().end:0);
                runtime_.dynamic_function_tables.push_back(std::move(table));result=true;
            }
            catch(const HostCallFault&) {result=false;}
            catch(const std::bad_alloc&) {result=false;}
        }
    }
    // BOOLEAN 失敗沒有 Win32 LastError 契約。
    arg.return_to(state,result?1:0);return std::nullopt;
}
}
