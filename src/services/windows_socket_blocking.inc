// Winsock 1.1 的阻塞掛勾（由 windows_network.cpp 的 invoke_network 引入）：WSAIsBlocking、WSASetBlockingHook、WSAUnhookBlockingHook、WSACancelBlockingCall。
// 規格：協商 2.0 以上時這組函式已移除，Set／Unhook／Cancel 回 SOCKET_ERROR＋WSAEOPNOTSUPP；1.x 時每個執行緒各有掛勾，Set 回傳先前的掛勾，
// Unhook 恢復預設掛勾。本引擎的阻塞呼叫在宿主等待、不會在等待期間回到客體執行掛勾，因此 WSAIsBlocking 一律為 FALSE，
// 沒有進行中的阻塞呼叫可取消（WSACancelBlockingCall 回 WSAEINVAL）。
// 預設掛勾與 Windows 相同：PeekMessage(PM_REMOVE) 取一則訊息並 Translate／Dispatch，有訊息時回 TRUE。
if(service.symbol=="WSAIsBlocking" || service.symbol=="WSASetBlockingHook" || service.symbol=="WSAUnhookBlockingHook" || service.symbol=="WSACancelBlockingCall" ||
   service.family=="internal-wsock-blocking"){
    if(service.family=="internal-wsock-blocking")
        return nested_continuation(state,memory,"預設阻塞掛勾",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);GuestScratch scratch(*this,m,s);const auto message=scratch.allocate(x86_?28U:48U);if(!message){call.return_to(s,0);co_return std::optional<std::uint32_t>{};}
            const auto user=[&](std::string_view name){return resolve("user32.dll",name).value;};
            if(!co_await service_callback::call(user("PeekMessageW"),{message,0,0,0,1})){call.return_to(s,0);co_return std::optional<std::uint32_t>{};}
            (void)co_await service_callback::call(user("TranslateMessage"),{message});(void)co_await service_callback::call(user("DispatchMessageW"),{message});
            call.return_to(s,1);co_return std::optional<std::uint32_t>{};
        });
    const WindowsCall call(state,memory,service);auto& network=runtime_.network;const auto symbol=service.symbol;
    const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto error=[&](std::uint32_t code,std::uint64_t value){last_error_=code;return done(value);};
    if(symbol=="WSAIsBlocking")return done(0);
    const auto failure=symbol=="WSASetBlockingHook"?0U:UINT32_MAX; // FARPROC 失敗為 NULL，其餘為 SOCKET_ERROR。
    if(!network.startups)return error(10093,failure);                       // WSANOTINITIALISED
    if((network.negotiated_version&0xffU)>=2)return error(10045,failure);    // WSAEOPNOTSUPP
    if(symbol=="WSACancelBlockingCall")return error(10022,UINT32_MAX);       // WSAEINVAL：沒有進行中的阻塞呼叫
    const auto standard=resolve("internal-wsock-blocking","DefaultHook").value;const auto thread=threads_.current;
    const auto found=network.blocking_hooks.find(thread);const auto previous=found==network.blocking_hooks.end()?standard:found->second;
    if(symbol=="WSAUnhookBlockingHook"){network.blocking_hooks.erase(thread);return done(0);}
    if(!call(0))return error(10014,0);                                       // WSAEFAULT
    network.blocking_hooks[thread]=call(0);return done(previous);
}
