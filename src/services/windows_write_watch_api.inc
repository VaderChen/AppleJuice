// GetWriteWatch／ResetWriteWatch；於共用服務邊界停止其他 vCPU 後查詢與重設。
const WindowsCall call(state,memory,service);const bool query=service.symbol=="GetWriteWatch";
const auto finish=[&](std::uint32_t error){if(error)last_error_=error;call.return_to(state,error?UINT32_MAX:0);return std::optional<std::uint32_t>{};};
try{
    const auto address=call(query?1:0),size=call(query?2:1);const auto flags=query?static_cast<std::uint32_t>(call(0)):1U;
    if((flags&~1U) || !size || address>=user_address_limit() || size>user_address_limit()-address)return finish(87);
    auto found=runtime_.virtual_regions.upper_bound(address);
    if(found==runtime_.virtual_regions.begin())return finish(87);--found;
    if(!found->second.write_watch || address-found->first>=found->second.size || size>found->second.size-(address-found->first))return finish(87);
    const auto begin=address&~UINT64_C(4095),end=(address+size+4095)&~UINT64_C(4095);
    if(!query){detail::reset_write_watch(runtime_,memory,begin,end);return finish(0);}
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto capacity=memory.load(GuestAddress{call(4)},bits);if(!capacity)return finish(87);
    if(!call(3) || !call(5))return finish(998);
    memory.check(GuestAddress{call(4)},unit,Access::Write);memory.check(GuestAddress{call(5)},4,Access::Write);
    detail::collect_write_watch(runtime_,memory,begin,end);
    std::vector<std::uint64_t> pages;
    const auto& dirty=found->second.written_pages;
    for(auto it=dirty.lower_bound(begin);it!=dirty.end() && *it<end && pages.size()<capacity;++it)pages.push_back(*it);
    memory.check(GuestAddress{call(3)},std::max<std::size_t>(unit,pages.size()*unit),Access::Write);
    // 容量不足仍成功，僅回傳及重設實際放入陣列的頁面；其餘歷史留待下一次查詢。
    if(flags&1U)for(const auto watched_page:pages)detail::reset_write_watch(runtime_,memory,watched_page,watched_page+4096);
    for(std::size_t i=0;i<pages.size();++i)memory.store(GuestAddress{call(3)+i*unit},bits,pages[i]);
    memory.store(GuestAddress{call(4)},bits,pages.size());memory.store(GuestAddress{call(5)},32,4096);return finish(0);
}catch(const MemoryFault&){return finish(998);}catch(const std::bad_alloc&){return finish(8);}catch(const std::invalid_argument&){return finish(87);}
