// WSAConnectByNameA／W（由 windows_network.cpp 的 invoke_network 引入）：
// (s, nodename, servicename, LocalAddressLength, LocalAddress, RemoteAddressLength, RemoteAddress, timeout, Reserved)。
// 依序呼叫既有的 GetAddrInfoW（與 socket 相同位址族、SOCK_STREAM／IPPROTO_TCP）與 connect，第一個成功的位址即完成；
// 指定 timeout 時暫時切成非阻塞，以 select 等候寫入或例外並讀回 SO_ERROR，結束後恢復阻塞模式。
// 成功後以 getsockname／getpeername 寫出本機與遠端位址（兩組長度與緩衝區須同時提供），失敗時回傳 FALSE 並設定 WSAGetLastError。
if(service.symbol=="WSAConnectByNameA" || service.symbol=="WSAConnectByNameW"){
    const WindowsCall call(state,memory,service);auto& network=runtime_.network;
    const auto reject=[&](std::uint32_t error){last_error_=error;call.return_to(state,0);return std::optional<std::uint32_t>{};};
    if(!network.startups)return reject(10093);
    const auto found=network.sockets.find(call(0));if(found==network.sockets.end())return reject(10038);
    if(call(8) || !call(1) || !call(2) || found->second->type!=SOCK_STREAM)return reject(10022);
    if(bool(call(3))!=bool(call(4)) || bool(call(5))!=bool(call(6)))return reject(10014);
    const int family=found->second->family==AF_INET6?23:2;
    return nested_continuation(state,memory,"WSAConnectByName",[this,service,family](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall arg(s,m,service);const bool wide=service.symbol.ends_with('W');const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(memory,block,state);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::span<const std::uint8_t> bytes)->std::uint64_t{
            const auto block=allocate_heap(m,std::max<std::size_t>(bytes.size(),8));if(!block)throw std::bad_alloc{};
            m.write(GuestAddress{block},std::vector<std::uint8_t>(std::max<std::size_t>(bytes.size(),8)));if(!bytes.empty())m.write(GuestAddress{block},std::vector<std::uint8_t>(bytes.begin(),bytes.end()));
            blocks.push_back(block);return block;};
        const auto guest=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve("ws2_32.dll",symbol).value,std::move(arguments));};
        const auto finish=[&](std::uint32_t error){last_error_=error;arg.return_to(s,error?0:1);return std::optional<std::uint32_t>{};};
        const auto text=[&](std::uint64_t address){return wide?read_utf16(m,GuestAddress{address},1024):ansi_to_utf16_lossy(read_ansi(m,address,1024));};
        const auto socket=arg(0);
        const auto node=scratch(utf16_bytes(text(arg(1)))),port=scratch(utf16_bytes(text(arg(2))));
        // ADDRINFOW {ai_flags, ai_family, ai_socktype, ai_protocol, ai_addrlen, ai_canonname, ai_addr, ai_next}
        std::vector<std::uint8_t> hints(x86_?32:48);hints[4]=static_cast<std::uint8_t>(family);hints[8]=1;hints[12]=6;
        const auto hint=scratch(hints),list=scratch({});
        const auto resolved=static_cast<std::uint32_t>(co_await guest("GetAddrInfoW",{node,port,hint,list}));
        if(resolved)co_return finish(resolved);
        const auto head=m.load(GuestAddress{list},bits);std::uint32_t error=10060;
        // 逾時：{tv_sec, tv_usec}；fd_set 為 {count, SOCKET[64]}。
        std::optional<std::array<std::uint32_t,2>> timeout;if(arg(7))timeout=std::array<std::uint32_t,2>{static_cast<std::uint32_t>(m.load(GuestAddress{arg(7)},32)),static_cast<std::uint32_t>(m.load(GuestAddress{arg(7)+4},32))};
        const auto mode=scratch({}),option=scratch({}),option_size=scratch({});const auto writes=scratch(std::vector<std::uint8_t>(unit+64*unit)),failures=scratch(std::vector<std::uint8_t>(unit+64*unit));
        const auto wait=scratch(std::vector<std::uint8_t>(8));
        bool connected=false;
        for(auto entry=head;entry && !connected;entry=m.load(GuestAddress{entry+(x86_?28U:40U)},bits)){
            const auto address=m.load(GuestAddress{entry+(x86_?24U:32U)},bits);const auto length=m.load(GuestAddress{entry+16},32);
            if(timeout){m.store(GuestAddress{mode},32,1);(void)co_await guest("ioctlsocket",{socket,0x8004667eU,mode});}
            if(!static_cast<std::uint32_t>(co_await guest("connect",{socket,address,length}))){connected=true;break;}
            error=static_cast<std::uint32_t>(co_await guest("WSAGetLastError",{}));
            if(timeout && error==10035){
                for(const auto set:{writes,failures}){m.store(GuestAddress{set},32,1);m.store(GuestAddress{set+unit},bits,socket);}
                m.store(GuestAddress{wait},32,(*timeout)[0]);m.store(GuestAddress{wait+4},32,(*timeout)[1]);
                const auto ready=static_cast<std::int32_t>(static_cast<std::uint32_t>(co_await guest("select",{0,0,writes,failures,wait})));
                if(ready==0)error=10060;
                else if(ready<0)error=static_cast<std::uint32_t>(co_await guest("WSAGetLastError",{}));
                else{m.store(GuestAddress{option_size},32,4);(void)co_await guest("getsockopt",{socket,0xffff,0x1007,option,option_size});error=static_cast<std::uint32_t>(m.load(GuestAddress{option},32));connected=!error;}
            }
            if(timeout){m.store(GuestAddress{mode},32,0);(void)co_await guest("ioctlsocket",{socket,0x8004667eU,mode});}
        }
        (void)co_await guest("FreeAddrInfoW",{head});
        if(!connected)co_return finish(error);
        // 寫出本機與遠端位址：長度不足時以 WSAEFAULT 失敗，連線仍保留。
        for(const auto& [length,buffer,symbol]:{std::tuple{arg(3),arg(4),std::string_view{"getsockname"}},std::tuple{arg(5),arg(6),std::string_view{"getpeername"}}}){
            if(!length)continue;
            if(static_cast<std::uint32_t>(co_await guest(symbol,{socket,buffer,length})))co_return finish(static_cast<std::uint32_t>(co_await guest("WSAGetLastError",{})));
        }
        co_return finish(0);
    });
}
