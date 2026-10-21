#include "x64arm/windows_services.hpp"
#include "x64arm/windows_address_layout.hpp"
#include "x64arm/windows_unwind.hpp"
#include "guest_crt_algorithms_code.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <numeric>

namespace x64arm {
void WindowsServices::initialize_guest_crt_algorithms(GuestMemory& memory, GuestAddress runtime_stubs) {
    static const bool enabled=[] {
        const auto* value=std::getenv("X64ARM_GUEST_CRT_ALGORITHMS");
        return !value || std::string_view(value)!="0";
    }();
    if(x86_ || !enabled || !trace_services_.empty())return;
    static const bool text_enabled=[] {
        const auto* value=std::getenv("X64ARM_GUEST_CRT_TEXT");
        return !value || std::string_view(value)!="0";
    }();
    // 既有最後一頁是 +0x96000 的 locale 鎖；新模組佔用其後獨立的頁面。
    constexpr std::uint64_t offset=0x98000;
    static_assert(offset+detail::guest_crt_image_size<=windows_address_layout::gateway_capacity);
    RuntimeImage image;
    image.base=callback_return_.value+offset;image.size=detail::guest_crt_image_size;
    image.path=u"[guest-crt]";
    image.function_table=image.base+detail::guest_crt_function_table;
    for(const auto& row:detail::guest_crt_unwind)image.functions.push_back({row.begin,row.end,row.unwind});
    for(const auto& row:detail::guest_crt_routines)
        if(!row.text || text_enabled)image.export_names.emplace(row.symbol,row.entry);
    std::vector<std::vector<std::uint8_t>> bytes;
    bytes.reserve(detail::guest_crt_sections.size());
    for(const auto& section:detail::guest_crt_sections){
        bytes.emplace_back(section.size,0);
        std::copy(section.bytes.begin(),section.bytes.end(),bytes.back().begin());
    }
    for(const auto& binding:detail::guest_crt_bindings){
        std::uint64_t value=guest_heap_state_.value;
        if(binding.kind==detail::GuestCrtBindingKind::Host){
            // 慢路徑固定指向原服務，不能被既有客體常式或新模組再攔截。
            const auto table=runtime_service_descriptors();
            const auto found=std::find_if(table.begin(),table.end(),[&](const auto& row){
                return row.family=="crt" && row.symbol==binding.symbol;
            });
            if(found==table.end())throw std::logic_error("客體 CRT 慢路徑缺少原宿主服務");
            value=runtime_stubs.value+static_cast<std::size_t>(found-table.begin())*16;
        }else if(binding.kind==detail::GuestCrtBindingKind::Call){
            value=resolve_service("msvcrt.dll",binding.symbol,true).value;
        }
        bool patched=false;
        for(std::size_t n=0;n<detail::guest_crt_sections.size();++n){
            const auto& section=detail::guest_crt_sections[n];
            if(binding.slot<section.rva || binding.slot-section.rva>section.size-8)continue;
            const auto at=binding.slot-section.rva;
            for(unsigned i=0;i<8;++i)bytes[n][at+i]=static_cast<std::uint8_t>(value>>(8*i));
            patched=true;break;
        }
        if(!patched)throw std::logic_error("客體 CRT 綁定槽沒有映射");
    }
    std::vector<UnwindMappingView> views;
    for(std::size_t n=0;n<detail::guest_crt_sections.size();++n){
        const auto& section=detail::guest_crt_sections[n];
        views.push_back({GuestAddress{image.base+section.rva},bytes[n],
            section.executable?read_execute:permission(Access::Read)});
    }
    validate_unwind(views,image);
    std::size_t mapped=0;
    try{
        for(const auto& view:views){memory.map(view.base,view.bytes.size(),view.permissions,view.bytes);++mapped;}
    }catch(...){
        while(mapped)memory.unmap(views[--mapped].base);
        throw;
    }
    guest_crt_algorithms_=std::move(image);
}

std::optional<std::uint32_t> WindowsServices::start_sort(GuestState& state, GuestMemory& memory, bool search) {
    const WindowsCall arg(state, memory, search?5:4);
    if(search) {
        if(!arg(2) || !arg(3)){arg.return_to(state,0);return std::nullopt;}
        if(arg(2)>output_limit/arg(3) || runtime_.sorts.size()>=32)throw HostCallFault("bsearch 大小或巢狀深度超過上限");
        memory.check(GuestAddress{arg(0)},1,Access::Read);
        memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(arg(2)*arg(3)),Access::Read);
        memory.check(GuestAddress{arg(4)},1,Access::Execute);
        SortState job;job.search=true;job.key=arg(0);job.base=arg(1);job.size=arg(3);job.compare=arg(4);job.right=static_cast<std::size_t>(arg(2));
        runtime_.sorts.push_back(std::move(job));return advance_sort(state,memory,0);
    }
    if (arg(1) < 2 || !arg(2)) { arg.return_to(state, 0); return std::nullopt; }
    if (arg(1) > 65536 || arg(2) > output_limit / arg(1) || runtime_.sorts.size() >= 32) throw HostCallFault("qsort 大小或巢狀深度超過上限");
    const auto count = static_cast<std::size_t>(arg(1));
    memory.check(GuestAddress{arg(0)}, static_cast<std::size_t>(arg(1) * arg(2)), Access::Read);
    memory.check(GuestAddress{arg(0)}, static_cast<std::size_t>(arg(1) * arg(2)), Access::Write);
    memory.check(GuestAddress{arg(3)}, 1, Access::Execute);
    SortState job; job.base = arg(0); job.size = arg(2); job.compare = arg(3);
    job.order.resize(count); job.merged.resize(count); std::iota(job.order.begin(), job.order.end(), 0);
    runtime_.sorts.push_back(std::move(job)); return advance_sort(state, memory, 0);
}
std::optional<std::uint32_t> WindowsServices::advance_sort(GuestState& state, GuestMemory& memory, std::uint64_t comparison, bool reuse_callback) {
    if (runtime_.sorts.empty()) throw HostCallFault("qsort callback 缺少排序狀態");
    auto& job = runtime_.sorts.back(); const auto count = job.order.size();
    const auto release_callback=[&]{
        if(!reuse_callback)return;
        state=runtime_.callbacks.back().saved;
        runtime_.callbacks.pop_back();reuse_callback=false;
    };
    const auto compare_next=[&](std::uint64_t target,std::uint64_t left,std::uint64_t right){
        if(!reuse_callback){start_callbacks(state,memory,{{target,{left,right}}},Continuation::Sort);return;}
        auto& frame=runtime_.callbacks.back();auto& call=frame.callbacks.front();
        call.target=target;call.arguments[0]=left;call.arguments[1]=right;
        call.user_mode_callback=false;classify_callback(call,Continuation::Sort);
        frame.exception_depth=exceptions_.stack.size();
        try{enter_callback(state,memory);}
        catch(...){
            // 進入失敗時與原本先離開、再建立框架的路徑相同：父狀態
            // 已還原，新框架的保留資源已釋放，排序狀態交由例外流程處理。
            state=frame.saved;finish_shell_callbacks(memory,frame);
            runtime_.callbacks.pop_back();throw;
        }
    };
    if(job.search) {
        if(job.waiting){
            const auto order=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(comparison));
            if(!order){const auto result=job.base+job.middle*job.size;release_callback();runtime_.sorts.pop_back();complete_call(state,memory,result);return std::nullopt;}
            if(order<0)job.right=job.middle;else job.left=job.middle+1;
        }
        if(job.left==job.right){release_callback();runtime_.sorts.pop_back();complete_call(state,memory,0);return std::nullopt;}
        job.middle=job.left+(job.right-job.left)/2;job.waiting=true;
        const auto target=job.compare,key=job.key,element=job.base+job.middle*job.size;
        compare_next(target,key,element);return std::nullopt;
    }
    if (job.waiting) {
        job.merged[job.k++] = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(comparison)) <= 0 ? job.order[job.i++] : job.order[job.j++];
        job.waiting = false;
    }
    for (;;) {
        if (job.left >= count) {
            job.order.swap(job.merged); job.width *= 2; job.left = 0;
            if (job.width >= count) {
                // 配置或客體讀寫若失敗，例外看到的仍是原 API 父狀態。
                release_callback();
                const auto size = static_cast<std::size_t>(job.size);
                std::vector<std::uint8_t> input(count * size), output(input.size());
                memory.read(GuestAddress{job.base}, input);
                for (std::size_t i = 0; i < count; ++i)
                    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(job.order[i] * size), size, output.begin() + static_cast<std::ptrdiff_t>(i * size));
                memory.write(GuestAddress{job.base}, output); runtime_.sorts.pop_back();
                complete_call(state,memory,0); return std::nullopt;
            }
        }
        if (!job.active) {
            job.middle = std::min(job.left + job.width, count); job.right = std::min(job.left + job.width * 2, count);
            job.i = job.left; job.j = job.middle; job.k = job.left; job.active = true;
        }
        if (job.i < job.middle && job.j < job.right) {
            const auto left = job.base + job.order[job.i] * job.size, right = job.base + job.order[job.j] * job.size;
            job.waiting = true; const auto target = job.compare;
            compare_next(target,left,right); return std::nullopt;
        }
        while (job.i < job.middle) job.merged[job.k++] = job.order[job.i++];
        while (job.j < job.right) job.merged[job.k++] = job.order[job.j++];
        job.left = job.right; job.active = false;
    }
}
}
