#pragma once
#include "msvcp_locale_ids.hpp"
#include "service_callback_task.hpp"

namespace x64arm::msvcp_locale_ids {
// 與客體 inline STL 共用 locale 鎖。等候期間另一執行緒可能已配號，取鎖後重讀一次。
inline service_callback::Task<std::uint64_t> locked_facet_id(GuestMemory& memory,GuestAddress object,
        GuestAddress count,std::uint8_t bits,std::uint64_t lock,std::uint64_t unlock){
    if(const auto id=memory.load(object,bits))co_return id;
    (void)co_await service_callback::call_value(lock,{});
    std::exception_ptr fault;
    std::uint64_t id{};
    try{id=facet_id(memory,object,count,bits);}catch(...){fault=std::current_exception();}
    (void)co_await service_callback::call_value(unlock,{});
    if(fault)std::rethrow_exception(fault);
    co_return id;
}
}
