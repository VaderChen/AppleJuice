if(service.family=="internal-dnssd")return invoke_dns_records(state,memory,service);
if(service.family=="dnsapi.dll"){
    if(!service.symbol.starts_with("DnsQuery_") && service.symbol!="DnsFree" && service.symbol!="DnsRecordListFree")return invoke_dns_records(state,memory,service);
    const WindowsCall dns_call(state,memory,service);std::uint32_t status=0;const unsigned pointer_size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(pointer_size*8);
    if(service.symbol=="DnsFree" || service.symbol=="DnsRecordListFree"){
        auto pointer=dns_call(0);const bool list=service.symbol=="DnsRecordListFree" || dns_call(1)!=0;
        while(pointer && runtime_.network.addresses.erase(pointer)){const auto next=list?memory.load(GuestAddress{pointer},width):0;(void)free_heap(memory,pointer,state);pointer=next;}
        dns_call.return_to(state,0);return {};
    }
    try{
        if(!dns_call(0) || dns_call(3) || dns_call(5))throw dns_detail::Error{87};
        if(dns_call(4)){memory.check(GuestAddress{dns_call(4)},pointer_size,Access::Write);memory.store(GuestAddress{dns_call(4)},width,0);}
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行外部 DNS 查詢");
        const auto name=service.symbol=="DnsQuery_W"?utf16_to_utf8(read_utf16(memory,GuestAddress{dns_call(0)},254)):ansi_to_utf8(read_ansi(memory,dns_call(0),1024));
        const auto records=dns_detail::query(name,static_cast<std::uint16_t>(dns_call(1)),static_cast<std::uint32_t>(dns_call(2)));
        if(dns_call(4))memory.store(GuestAddress{dns_call(4)},width,dns_store_records(state,memory,records,service.symbol=="DnsQuery_W"?1U:service.symbol=="DnsQuery_UTF8"?2U:3U));
    }catch(const dns_detail::Error& error){status=error.code;}catch(const MemoryFault&){status=87;}catch(const std::bad_alloc&){status=8;}
    dns_call.return_to(state,status);return {};
}
