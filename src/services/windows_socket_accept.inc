// 由 windows_network.cpp 納入。普通與條件式 accept 共用 POSIX 後台及客體排程。
namespace {
struct SocketAcceptError {std::uint32_t code;};
void require_accept(bool value,std::uint32_t code){if(!value)throw SocketAcceptError{code};}
void accept_native_ok(int result){if(result<0)throw SocketAcceptError{socket_detail::socket_error(errno)};}
std::shared_ptr<NativeSocket> take_socket_connection(const std::shared_ptr<NativeSocket>& listener){
    const auto fd=socket_detail::accept_socket_connection(*listener);accept_native_ok(fd);
    std::shared_ptr<NativeSocket> result;
    try{result=std::make_shared<NativeSocket>(fd,listener->family,listener->type,listener->protocol);}
    catch(...){::close(fd);throw;}
    accept_native_ok(::fcntl(fd,F_SETFD,FD_CLOEXEC));
    accept_native_ok(socket_detail::copy_accept_options(listener->descriptor,fd));
#ifdef SO_NOSIGPIPE
    const int yes=1;accept_native_ok(::setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes)));
#endif
    result->overlapped=listener->overlapped;result->accepted_at=socket_clock_ticks();
    result->shared->data().zero_send_buffer.store(listener->shared->data().zero_send_buffer.load());
    return result;
}
std::vector<std::uint8_t> accepted_endpoint(int fd,bool local){
    sockaddr_storage address{};socklen_t size=sizeof(address);auto* native=reinterpret_cast<sockaddr*>(&address);
    const auto result=local?::getsockname(fd,native,&size): ::getpeername(fd,native,&size);
    accept_native_ok(result);return socket_detail::address_bytes(native);
}
}
std::uint64_t WindowsServices::publish_accepted_socket(GuestMemory& memory,const ConditionalAccept& operation){
    auto& network=runtime_.network;
    require_accept(network.startups!=0,10093);
    const auto source=network.sockets.find(operation.listener_handle);
    require_accept(source!=network.sockets.end() && source->second==operation.listener,10038);
    require_accept(network.sockets.size()<1024,10024);
    // 回呼可能改變映射；發布 handle 前重新核對兩個輸出，避免遺漏或部分寫入。
    if(operation.address){
        require_accept(memory.load(GuestAddress{operation.address_size},32)>=operation.peer.size(),10014);
        memory.check(GuestAddress{operation.address},operation.peer.size(),Access::Write);
        memory.check(GuestAddress{operation.address_size},4,Access::Write);
    }
    const auto handle=network.next_socket++;
    try{
        network.sockets.emplace(handle,operation.accepted);runtime_.handle_flags[handle]=1;
        if(const auto found=gui_.socket_watches.find(operation.listener_handle);found!=gui_.socket_watches.end()){
            auto watch=found->second;watch.connecting=false;watch.connected=true;watch.armed=watch.events;watch.recorded=0;watch.errors={};
            watch.generation=operation.accepted->shared->data().watch_generation.fetch_add(1)+1;
            gui_.socket_watches[handle]=watch;
        }
        if(operation.address){memory.write(GuestAddress{operation.address},operation.peer);memory.store(GuestAddress{operation.address_size},32,operation.peer.size());}
    }catch(...){network.sockets.erase(handle);runtime_.handle_flags.erase(handle);gui_.socket_watches.erase(handle);throw;}
    return handle;
}
void WindowsServices::discard_socket_accept(GuestState& state,GuestMemory& memory,std::uint64_t token){
    auto& operations=runtime_.network.conditional_accepts;const auto found=operations.find(token);if(found==operations.end())return;
    auto operation=std::move(found->second);operations.erase(found);
    operation.listener->shared->data().watch_rearm.fetch_or(8);
    if(operation.scratch)(void)free_heap(memory,operation.scratch,state);
}
std::optional<std::uint32_t> WindowsServices::resume_socket_accept(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t decision){
    auto& operations=runtime_.network.conditional_accepts;const auto found=operations.find(token);
    if(found==operations.end()){last_error_=10022;complete_call(state,memory,UINT64_MAX);return std::nullopt;}
    auto operation=std::move(found->second);operations.erase(found);
    std::uint64_t result=UINT64_MAX;bool deferred=false;
    try{
        if(static_cast<std::uint32_t>(decision)==2){
            require_accept(runtime_.network.startups!=0,10093);
            const auto listener=runtime_.network.sockets.find(operation.listener_handle);
            require_accept(listener!=runtime_.network.sockets.end() && listener->second==operation.listener,10038);
            operation.listener->deferred_accepts.push_front(operation.accepted);deferred=true;last_error_=11002;
        }else if(static_cast<std::uint32_t>(decision)==1){
            // 拒絕以 RST 結束，不能讓對方誤認為一次正常接受後的 EOF。
            const linger reset{1,0};accept_native_ok(::setsockopt(operation.accepted->descriptor,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset)));
            last_error_=10061;
        }else{
            require_accept(static_cast<std::uint32_t>(decision)==0,10022);
            // 既有 TCP/IP provider（WSASocket 同契約）不提供 socket group 或 connect-time payload。
            require_accept(memory.load(GuestAddress{operation.scratch+48},32)==0,10022);
            require_accept(memory.load(GuestAddress{operation.scratch+32},32)==0,10045);
            result=publish_accepted_socket(memory,operation);
        }
    }catch(const SocketAcceptError& error){last_error_=error.code;}
    catch(const MemoryFault&){last_error_=10014;}
    catch(const std::bad_alloc&){last_error_=10055;}
    if(!deferred)operation.listener->shared->data().watch_rearm.fetch_or(8);
    if(operation.scratch)(void)free_heap(memory,operation.scratch,state);
    complete_call(state,memory,result);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_socket_accept(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto& network=runtime_.network;std::shared_ptr<NativeSocket> listener;
    std::uint64_t result=UINT64_MAX;
    try{
        require_accept(network.startups!=0,10093);
        const auto found=network.sockets.find(arg(0));require_accept(found!=network.sockets.end(),10038);listener=found->second;
        require_accept(listener->type==SOCK_STREAM,10045);require_accept(listener->listening,10022);
        require_accept(network.sockets.size()<1024,10024);
        const bool extended=service.symbol=="WSAAccept";
        // WSAAccept 任一位址參數為 NULL 時，不回傳位址；普通 accept 維持原來的輸出檢查。
        const auto address=(extended && !arg(2))?0:arg(1),size_pointer=address?arg(2):0;
        if(address){
            const auto required=listener->family==AF_INET6?28U:16U;require_accept(size_pointer!=0,10014);
            require_accept(memory.load(GuestAddress{size_pointer},32)>=required,10014);
            memory.check(GuestAddress{address},required,Access::Write);memory.check(GuestAddress{size_pointer},4,Access::Write);
        }
        std::shared_ptr<NativeSocket> accepted;
        try{accepted=take_socket_connection(listener);}
        catch(const SocketAcceptError& error){
            if(error.code!=10035)throw;
            const auto flags=::fcntl(listener->descriptor,F_GETFL);accept_native_ok(flags);
            if(flags&O_NONBLOCK)throw;
            listener->shared->data().watch_rearm.fetch_or(8);
            auto& thread=threads_.items.at(threads_.current);thread.wait={};thread.wait.kind=ThreadWaitKind::SocketSelect;
            thread.wait.deadline=UINT64_MAX;thread.wait.handles={arg(0)};thread.wait.socket_events={POLLIN};thread.wait.alertable=true;thread.status=ThreadStatus::Waiting;
            schedule(state,memory);return std::nullopt;
        }
        if(threads_.items.at(threads_.current).wait.kind==ThreadWaitKind::SocketSelect)threads_.items.at(threads_.current).wait={};
        ConditionalAccept operation{arg(0),address,size_pointer,0,listener,accepted,accepted_endpoint(accepted->descriptor,false)};
        if(extended && arg(3)){
            require_accept(network.conditional_accepts.size()<128,10055);
            const auto token=network.next_conditional_accept++;
            network.conditional_accepts.emplace(token,operation);
            try{
                auto& pending=network.conditional_accepts.at(token);
                pending.scratch=allocate_heap(memory,128);if(!pending.scratch)throw std::bad_alloc();
                const auto scratch=pending.scratch;memory.write(GuestAddress{scratch},std::array<std::uint8_t,128>{});
                const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
                const auto local=accepted_endpoint(accepted->descriptor,true);
                memory.store(GuestAddress{scratch},32,pending.peer.size());memory.store(GuestAddress{scratch+unit},bits,scratch+64);
                memory.store(GuestAddress{scratch+16},32,local.size());memory.store(GuestAddress{scratch+16+unit},bits,scratch+96);
                memory.write(GuestAddress{scratch+64},pending.peer);memory.write(GuestAddress{scratch+96},local);
                start_callbacks(state,memory,{{arg(3),{scratch,0,0,0,scratch+16,scratch+32,scratch+48,arg(4)}}},Continuation::SocketAccept,token);
            }catch(...){discard_socket_accept(state,memory,token);throw;}
            return std::nullopt;
        }
        result=publish_accepted_socket(memory,operation);
    }catch(const SocketAcceptError& error){last_error_=error.code;}
    catch(const MemoryFault&){last_error_=10014;}
    catch(const std::bad_alloc&){last_error_=10055;}
    if(listener)listener->shared->data().watch_rearm.fetch_or(8);
    if(threads_.items.at(threads_.current).wait.kind==ThreadWaitKind::SocketSelect)threads_.items.at(threads_.current).wait={};
    arg.return_to(state,result);return std::nullopt;
}
