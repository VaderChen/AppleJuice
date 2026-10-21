#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "microsoft_demangle.hpp"
#include "windows_slist.hpp"
#include <algorithm>
#include <bit>
#include <exception>
#include <set>

namespace x64arm {
namespace {
struct DemangleOperation {
    WindowsServiceDescriptor service;
    std::string output;
    std::uint64_t buffer{}, release{}, result{};
    std::size_t capacity{};
    bool freeing{};
    std::exception_ptr fault;
};
std::vector<std::uint8_t> terminated(std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(),text.end()); bytes.push_back(0); return bytes;
}
std::uint64_t offset(std::uint64_t base, unsigned bytes) {
    if (!base || base > UINT64_MAX - bytes) throw HostCallFault("CRT 型別資訊位址無效");
    return base + bytes;
}
}
struct DemangleState {std::map<std::uint64_t,DemangleOperation> operations;std::uint64_t next{1};};
void WindowsServices::discard_demangle(std::uint64_t token) {
    if (demangle_) demangle_->operations.erase(token);
}
std::optional<std::uint32_t> WindowsServices::resume_demangle(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t callback_result) {
    if (!demangle_ || !demangle_->operations.contains(token)) throw HostCallFault("名稱解析配置回呼缺少狀態");
    auto& operation = demangle_->operations.at(token);
    const WindowsCall call(state,memory,operation.service);
    const auto finish = [&](std::uint64_t result) -> std::optional<std::uint32_t> {
        auto fault = operation.fault; demangle_->operations.erase(token);
        if (fault) std::rethrow_exception(fault);
        call.return_to(state,result); return std::nullopt;
    };
    if (operation.freeing) return finish(operation.result);
    const auto allocation = x86_ ? callback_result & UINT32_MAX : callback_result;
    if (!allocation) {memory.store(GuestAddress{runtime_.errno_cell},32,12); return finish(0);}
    try {
        memory.write(GuestAddress{allocation},terminated(operation.output));
        if (operation.buffer) {
            const auto count = std::min(operation.output.size(),operation.capacity - 1);
            memory.write(GuestAddress{operation.buffer},terminated(std::string_view(operation.output).substr(0,count)));
        }
    } catch (...) {operation.fault = std::current_exception();}
    if ((operation.buffer || operation.fault) && operation.release) {
        operation.freeing = true; operation.result = operation.buffer;
        // 暫存輸出確實存於客體配置，再複製至 caller buffer，交由同一配置器釋放。
        start_callbacks(state,memory,{{operation.release,{allocation}}},Continuation::DemangleOperation,token);
        return std::nullopt;
    }
    return finish(allocation);
}
std::optional<std::uint32_t> WindowsServices::invoke_demangle(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall call(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint64_t result)->std::optional<std::uint32_t>{call.return_to(state,result);return std::nullopt;};
    const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    if(name.starts_with("__std_type_info_")) {
        if(name=="__std_type_info_destroy_list") {
            const auto root=call(0);slist::check(memory,root,x86_,Access::Write);
            std::vector<std::uint64_t> entries;std::set<std::uint64_t> seen;
            for(auto entry=slist::first(memory,root,x86_);entry;entry=memory.load(GuestAddress{entry},bits)) {
                if(entries.size()>=65536 || entry%(x86_?4:16) || !seen.insert(entry).second || !allocations_.contains(entry))
                    throw HostCallFault("CRT 型別名稱清單含無效配置、循環或超出上限");
                entries.push_back(entry);
            }
            (void)slist::flush(memory,root,x86_);
            for(const auto entry:entries)if(!free_heap(memory,entry,state))throw HostCallFault("CRT 型別名稱配置已失效");
            return done(0);
        }
        const auto type=call(0);
        if(name=="__std_type_info_compare" && type==call(1))return done(0);
        if(name=="__std_type_info_name") {const auto cached=memory.load(GuestAddress{type},bits);if(cached)return done(cached);}
        const auto raw=read_ansi(memory,offset(type,unit),microsoft_name_limit);
        const auto mangled=raw.empty()?std::string_view{}:std::string_view(raw).substr(1);
        if(name=="__std_type_info_compare") {
            const auto right=read_ansi(memory,offset(call(1),unit),microsoft_name_limit);
            const auto other=right.empty()?std::string_view{}:std::string_view(right).substr(1);
            const auto result=mangled.compare(other);return done(static_cast<std::uint32_t>(result<0?-1:result>0?1:0));
        }
        if(name=="__std_type_info_hash") {
            std::uint64_t hash=x86_?UINT64_C(0x811c9dc5):UINT64_C(0xcbf29ce484222325);
            for(const auto c:mangled) {hash=(hash^static_cast<unsigned char>(c))*(x86_?UINT64_C(0x1000193):UINT64_C(0x100000001b3));if(x86_)hash&=UINT32_MAX;}
            if(!x86_)hash^=hash>>32;return done(hash);
        }
        if(name!="__std_type_info_name")throw HostCallFault("未知 CRT 型別資訊入口");
        memory.check(GuestAddress{type},unit,Access::Write);slist::check(memory,call(1),x86_,Access::Write);
        const auto parsed=demangle_microsoft_name(mangled,0x2800,true);
        if(!parsed){memory.store(GuestAddress{runtime_.errno_cell},32,34);return done(0);}
        auto value=*parsed;while(!value.empty() && value.back()==' ')value.pop_back();
        const auto bytes=terminated(value);const unsigned prefix=x86_?4U:16U;
        const auto allocation=allocate_heap(memory,prefix+bytes.size());
        if(!allocation){memory.store(GuestAddress{runtime_.errno_cell},32,12);return done(0);}
        try {memory.write(GuestAddress{allocation+prefix},bytes);(void)slist::push(memory,call(1),x86_,allocation);memory.store(GuestAddress{type},bits,allocation+prefix);}
        catch(...){(void)free_heap(memory,allocation,state);throw;}
        return done(allocation+prefix);
    }
    const bool extended=name=="__unDNameEx",crt=name=="__unDName" || extended,wide=name.ends_with('W');
    const auto input=call(crt?1:0),buffer=call(crt?0:1);
    const auto invalid=[&](std::uint32_t error)->std::optional<std::uint32_t>{if(crt)memory.store(GuestAddress{runtime_.errno_cell},32,error);else last_error_=error;return done(0);};
    const auto signed_size=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(call(2)));
    const auto capacity=crt?static_cast<std::uint32_t>(signed_size):static_cast<std::uint32_t>(call(2));
    const auto flags=static_cast<std::uint16_t>(call(crt?(extended?6:5):3));
    if(extended && call(5))return invalid(22); // 非零延伸參數的未公開契約尚未接入。
    if(!input || (crt && signed_size<0) || (!crt && (!buffer || !capacity || (call(3)&~UINT64_C(0xffff)))))return invalid(crt?22U:87U);
    const auto source=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{input},microsoft_name_limit)):read_ansi(memory,input,microsoft_name_limit);
    auto parsed=demangle_microsoft_name(source,flags,crt && (flags&0x2000U));
    if(!parsed)return invalid(crt?34U:8U);
    if(!crt) {
        if(wide) {
            auto text=utf8_to_utf16(*parsed);text.resize(std::min<std::size_t>(text.size(),capacity-1));
            memory.write(GuestAddress{buffer},utf16_bytes(text));return done(text.size());
        }
        parsed->resize(std::min<std::size_t>(parsed->size(),capacity-1));memory.write(GuestAddress{buffer},terminated(*parsed));return done(parsed->size());
    }
    const auto allocator=call(3),release=call(4);
    if(!allocator)return invalid(22);
    if(buffer && capacity) {
        const auto count=std::min<std::size_t>(parsed->size(),capacity-1);memory.check(GuestAddress{buffer},count+1,Access::Write);
        // 未提供 deallocator 時不建立 caller 無法取回的客體暫存配置。
        if(!release){memory.write(GuestAddress{buffer},terminated(std::string_view(*parsed).substr(0,count)));return done(buffer);}
    }
    if(!handles(GuestAddress{allocator}))memory.check(GuestAddress{allocator},1,Access::Execute);
    if(buffer && capacity && !handles(GuestAddress{release}))memory.check(GuestAddress{release},1,Access::Execute);
    if(!demangle_)demangle_=std::make_shared<DemangleState>();
    const auto token=demangle_->next++;DemangleOperation operation;operation.service=service;operation.output=std::move(*parsed);
    operation.buffer=capacity?buffer:0;operation.capacity=capacity;operation.release=release;
    const auto size=operation.output.size()+1;demangle_->operations.emplace(token,std::move(operation));
    try {start_callbacks(state,memory,{{allocator,{size}}},Continuation::DemangleOperation,token);}
    catch(...){discard_demangle(token);throw;}
    return std::nullopt;
}
}
