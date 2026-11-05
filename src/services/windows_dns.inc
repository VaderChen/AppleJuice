if(service.family=="dnsapi.dll"){
    const WindowsCall dns_call(state,memory,service);std::uint32_t status=0;const unsigned pointer_size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(pointer_size*8);
    if(service.symbol=="DnsFree" || service.symbol=="DnsRecordListFree"){
        auto pointer=dns_call(0);const bool list=service.symbol=="DnsRecordListFree" || dns_call(1)!=0;
        while(pointer && runtime_.network.addresses.erase(pointer)){const auto next=list?memory.load(GuestAddress{pointer},width):0;(void)free_heap(memory,pointer,state);pointer=next;}
        dns_call.return_to(state,0);return {};
    }
    std::vector<std::uint64_t> allocated;
    try{
        if(!dns_call(0) || dns_call(3) || dns_call(5))throw dns_detail::Error{87};
        if(dns_call(4)){memory.check(GuestAddress{dns_call(4)},pointer_size,Access::Write);memory.store(GuestAddress{dns_call(4)},width,0);}
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行外部 DNS 查詢");
        const auto name=service.symbol=="DnsQuery_W"?utf16_to_utf8(read_utf16(memory,GuestAddress{dns_call(0)},254)):read_ansi(memory,dns_call(0),1024);
        const auto records=dns_detail::query(name,static_cast<std::uint16_t>(dns_call(1)),static_cast<std::uint32_t>(dns_call(2)));
        if(dns_call(4))for(const auto& record:records){
            auto blob=dns_detail::pack(record,pointer_size,service.symbol=="DnsQuery_W"?1U:service.symbol=="DnsQuery_UTF8"?2U:3U);
            const auto address=allocate_heap(memory,blob.bytes.size());if(!address)throw dns_detail::Error{8};allocated.push_back(address);
            for(const auto offset:blob.pointers){std::uint64_t relative=0;for(unsigned i=0;i<pointer_size;++i)relative|=std::uint64_t{blob.bytes[offset+i]}<<(i*8);const auto pointer=address+relative;for(unsigned i=0;i<pointer_size;++i)blob.bytes[offset+i]=static_cast<std::uint8_t>(pointer>>(i*8));}
            memory.write(GuestAddress{address},blob.bytes);runtime_.network.addresses.insert(address);
            if(allocated.size()>1)memory.store(GuestAddress{allocated[allocated.size()-2]},width,address);
        }
        if(dns_call(4))memory.store(GuestAddress{dns_call(4)},width,allocated.empty()?0:allocated.front());
    }catch(const dns_detail::Error& error){status=error.code;}catch(const MemoryFault&){status=87;}catch(const std::bad_alloc&){status=8;}
    if(status)for(const auto address:allocated){runtime_.network.addresses.erase(address);(void)free_heap(memory,address,state);}
    dns_call.return_to(state,status);return {};
}
