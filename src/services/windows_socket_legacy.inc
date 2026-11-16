// mswsock 的舊式入口（由 windows_network.cpp 的 invoke_network 引入），以協程呼叫既有 Winsock 服務：
// - WSARecvEx(s, buf, len, flags)：呼叫 recv；資料包被截斷（WSAEMSGSIZE）時改回傳 len 並在 *flags 設定 MSG_PARTIAL，其餘情況清除 MSG_PARTIAL。
// - EnumProtocolsA／W(lpiProtocols, lpProtocolBuffer, lpdwBufferLength)：由 WSCEnumProtocols 的同一份目錄轉成 PROTOCOL_INFO
//   {dwServiceFlags, iAddressFamily, iMaxSockAddr, iMinSockAddr, iSocketType, iProtocol, dwMessageSize, lpProtocol}，名稱字串放在結構陣列之後；
//   緩衝區不足時回傳 -1、ERROR_INSUFFICIENT_BUFFER 並寫回所需大小。
if(service.symbol=="WSARecvEx" || service.symbol=="EnumProtocolsA" || service.symbol=="EnumProtocolsW"){
    return nested_continuation(state,memory,service.symbol,[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall arg(s,m,service);const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(memory,block,state);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,8));if(!block)throw std::bad_alloc{};m.write(GuestAddress{block},std::vector<std::uint8_t>(std::max<std::size_t>(size,8)));blocks.push_back(block);return block;};
        const auto guest=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve("ws2_32.dll",symbol).value,std::move(arguments));};
        const auto finish=[&](std::uint64_t value){arg.return_to(s,value);return std::optional<std::uint32_t>{};};
        if(service.symbol=="WSARecvEx"){
            if(!arg(3)){last_error_=10014;co_return finish(UINT32_MAX);}
            const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{arg(3)},32))&~0x8000U;
            const auto received=static_cast<std::uint32_t>(co_await guest("recv",{arg(0),arg(1),arg(2),flags}));
            if(received==UINT32_MAX && static_cast<std::uint32_t>(co_await guest("WSAGetLastError",{}))==10040){
                m.store(GuestAddress{arg(3)},32,flags|0x8000U);last_error_=0;co_return finish(static_cast<std::uint32_t>(arg(2)));
            }
            if(received!=UINT32_MAX)m.store(GuestAddress{arg(3)},32,flags);
            co_return finish(received);
        }
        const bool wide=service.symbol.ends_with('W');
        if(!arg(2)){last_error_=87;co_return finish(UINT32_MAX);}
        const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{arg(2)},32));
        // 先取得目錄大小，再取回 WSAPROTOCOL_INFOW 陣列（每筆 628 位元組，szProtocol 在 116）。
        const auto length=scratch(4),error=scratch(4);
        (void)co_await guest("WSCEnumProtocols",{arg(0),0,length,error});
        const auto needed=static_cast<std::uint32_t>(m.load(GuestAddress{length},32));
        const auto catalog=scratch(needed);m.store(GuestAddress{length},32,needed);
        const auto count=static_cast<std::int32_t>(static_cast<std::uint32_t>(co_await guest("WSCEnumProtocols",{arg(0),catalog,length,error})));
        if(count<0){last_error_=static_cast<std::uint32_t>(m.load(GuestAddress{error},32));co_return finish(UINT32_MAX);}
        constexpr unsigned info_size=628,name_offset=116;const unsigned entry_size=x86_?32:40;
        std::vector<std::vector<std::uint8_t>> names;std::size_t total=static_cast<std::size_t>(count)*entry_size;
        for(std::int32_t i=0;i<count;++i){
            const auto text=read_utf16(m,GuestAddress{catalog+static_cast<std::uint64_t>(i)*info_size+name_offset},256);
            std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(text);else{const auto narrow=utf16_to_utf8(text);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}
            total+=(bytes.size()+unit-1)/unit*unit;names.push_back(std::move(bytes));
        }
        if(total>capacity || !arg(1)){m.store(GuestAddress{arg(2)},32,total);last_error_=122;co_return finish(UINT32_MAX);}
        auto text_at=arg(1)+static_cast<std::uint64_t>(count)*entry_size;
        for(std::int32_t i=0;i<count;++i){
            const auto info=catalog+static_cast<std::uint64_t>(i)*info_size,entry=arg(1)+static_cast<std::uint64_t>(i)*entry_size;
            // WSAPROTOCOL_INFOW：dwServiceFlags1 0、iAddressFamily 76、iMaxSockAddr 80、iMinSockAddr 84、iSocketType 88、iProtocol 92、dwMessageSize 108。
            for(const auto& [to,from]:{std::pair{0U,0U},std::pair{4U,76U},std::pair{8U,80U},std::pair{12U,84U},std::pair{16U,88U},std::pair{20U,92U},std::pair{24U,108U}})
                m.store(GuestAddress{entry+to},32,m.load(GuestAddress{info+from},32));
            m.store(GuestAddress{entry+(x86_?28U:32U)},bits,text_at);m.write(GuestAddress{text_at},names[static_cast<std::size_t>(i)]);
            text_at+=(names[static_cast<std::size_t>(i)].size()+unit-1)/unit*unit;
        }
        m.store(GuestAddress{arg(2)},32,total);co_return finish(static_cast<std::uint32_t>(count));
    });
}
