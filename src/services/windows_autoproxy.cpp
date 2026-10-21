#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
std::uint64_t WindowsServices::queue_http_callback(const GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint32_t status,std::span<const std::uint8_t> information,std::uint64_t context,bool ready){
    const auto found=runtime_.http.handles.find(handle);
    if(found==runtime_.http.handles.end() || !found->second.callback || !(found->second.callback_flags&status))return 0;
    if(registered_waits_.size()>=64)throw std::bad_alloc{};
    // callback 使用共用工作執行緒；宿主 PAC 執行緒不會直接進入客體程式碼。
    const auto worker=acquire_wait_worker(state,memory,65536);if(!worker)throw std::bad_alloc{};
    std::uint64_t payload=0,id=0;
    try{
        if(!information.empty()){
            payload=allocate_heap(memory,information.size());if(!payload)throw std::bad_alloc{};
            memory.write(GuestAddress{payload},information);
        }
        id=next_registered_wait_++;RegisteredWait wait;
        wait.worker=worker;wait.armed=false;wait.closing=true;wait.work_item=true;
        wait.callback_batch={{found->second.callback,{handle,context,status,payload,information.size()}}};
        if(payload)wait.callback_storage.push_back(payload);
        registered_waits_.emplace(id,std::move(wait));wait_workers_[worker]=id;
        http_callbacks_.emplace(id,HttpCallbackOperation{handle,{},ready});
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWait;
        return id;
    }catch(...){if(id){http_callbacks_.erase(id);registered_waits_.erase(id);}if(payload)free_heap(memory,payload,state);wait_workers_[worker]=0;throw;}
}
void WindowsServices::poll_autoproxy(GuestMemory& memory){
    const unsigned unit=x86_?4:8;
    for(auto& [handle,resolver]:proxy_resolvers_){
        if(!resolver.task || !resolver.task->done())continue;
        resolver.result=resolver.task->take();resolver.task.reset();resolver.completed=true;
        const auto pending=http_callbacks_.find(resolver.callback_wait);
        const auto registered=registered_waits_.find(resolver.callback_wait);
        if(pending==http_callbacks_.end() || registered==registered_waits_.end())continue;
        auto& wait=registered->second;const auto owner=runtime_.http.handles.find(handle);
        const auto status=resolver.result.error?0x200000U:0x1000000U;
        if(owner==runtime_.http.handles.end() || !owner->second.callback || !(owner->second.callback_flags&status))wait.callback_batch.clear();
        else {
            auto& callback=wait.callback_batch.front();callback.target=owner->second.callback;
            callback.arguments[2]=status;callback.arguments[4]=resolver.result.error?2*unit:0;
            if(resolver.result.error){memory.store(GuestAddress{callback.arguments[3]},x86_?32:64,6);memory.store(GuestAddress{callback.arguments[3]+unit},32,resolver.result.error);}
            else callback.arguments[3]=0;
        }
        pending->second.ready=true;
    }
    for(auto it=http_callbacks_.begin();it!=http_callbacks_.end();){
        const auto registered=registered_waits_.find(it->first);
        if(registered==registered_waits_.end()){it=http_callbacks_.erase(it);continue;}
        auto& wait=registered->second;
        if(it->second.ready && !wait.ready && !wait.active && std::none_of(it->second.dependencies.begin(),it->second.dependencies.end(),[&](auto dependency){return registered_waits_.contains(dependency);})){
            wait.ready=true;threads_.items.at(wait.worker).status=ThreadStatus::Runnable;
        }
        ++it;
    }
}
void WindowsServices::close_http_callbacks(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    if(const auto resolver=proxy_resolvers_.find(handle);resolver!=proxy_resolvers_.end()){
        if(resolver->second.task)resolver->second.task->cancel();proxy_resolvers_.erase(resolver);
    }
    std::vector<std::uint64_t> dependencies;
    for(auto it=http_callbacks_.begin();it!=http_callbacks_.end();){
        if(it->second.handle!=handle){++it;continue;}
        const auto registered=registered_waits_.find(it->first);
        if(registered!=registered_waits_.end() && registered->second.active){dependencies.push_back(it->first);++it;continue;}
        if(registered!=registered_waits_.end()){
            for(const auto pointer:registered->second.callback_storage)free_heap(memory,pointer,state);
            registered->second.callback_storage.clear();release_registered_wait(it->first);
        }
        it=http_callbacks_.erase(it);
    }
    const unsigned unit=x86_?4:8;std::vector<std::uint8_t> bytes(unit);
    for(unsigned i=0;i<unit;++i)bytes[i]=static_cast<std::uint8_t>(handle>>(i*8));
    const auto found=runtime_.http.handles.find(handle);if(found==runtime_.http.handles.end())return;
    if(const auto closing=queue_http_callback(state,memory,handle,0x800,bytes,found->second.context))http_callbacks_.at(closing).dependencies=std::move(dependencies);
}
std::optional<std::uint32_t> WindowsServices::invoke_autoproxy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto symbol=service.symbol;
    if(symbol!="WinHttpGetProxyForUrl" && symbol!="WinHttpDetectAutoProxyConfigUrl"){
        const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        struct Error{std::uint32_t code;};std::uint64_t result=0;
        const bool setter=symbol=="WinHttpSetStatusCallback",free_result=symbol=="WinHttpFreeProxyResult";
        try{
            if(free_result){
                if(arg(0)){
                    memory.check(GuestAddress{arg(0)},2*unit,Access::Write);
                    const auto pointer=memory.load(GuestAddress{arg(0)+unit},bits);
                    if(pointer){const auto owned=proxy_result_allocations_.find(pointer);if(owned==proxy_result_allocations_.end())throw Error{87};free_heap(memory,pointer,state);proxy_result_allocations_.erase(owned);}
                    memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(2*unit));
                }
            }else{
                const auto found=runtime_.http.handles.find(arg(0));if(found==runtime_.http.handles.end())throw Error{6};auto& handle=found->second;
                if(setter){
                    if(handle.inet || arg(3))throw Error{87};if(arg(1))memory.check(GuestAddress{arg(1)},1,Access::Execute);
                    result=handle.callback;handle.callback=arg(1);handle.callback_flags=static_cast<std::uint32_t>(arg(2));
                }else if(symbol=="WinHttpCreateProxyResolver"){
                    if(handle.kind!=HttpHandle::Kind::Session || !handle.asynchronous || handle.inet)throw Error{12018};
                    if(!arg(1))throw Error{87};memory.check(GuestAddress{arg(1)},unit,Access::Write);
                    if(runtime_.http.handles.size()>=4096)throw Error{8};
                    HttpHandle resolver;resolver.kind=HttpHandle::Kind::Resolver;resolver.parent=arg(0);resolver.input=handle.input;resolver.asynchronous=true;
                    resolver.callback=handle.callback;resolver.callback_flags=handle.callback_flags;resolver.context=handle.context;
                    const auto id=runtime_.http.next++;runtime_.http.handles.emplace(id,std::move(resolver));proxy_resolvers_.emplace(id,ProxyResolver{});
                    memory.store(GuestAddress{arg(1)},bits,id);
                }else{
                    if(handle.kind!=HttpHandle::Kind::Resolver)throw Error{12018};auto& resolver=proxy_resolvers_.at(arg(0));
                    if(symbol=="WinHttpGetProxyResult"){
                        if(!resolver.completed || resolver.task)throw Error{12019};if(resolver.result.error)throw Error{resolver.result.error};
                        if(!arg(1))throw Error{87};memory.check(GuestAddress{arg(1)},2*unit,Access::Write);
                        const auto& entries=resolver.result.entries;const unsigned stride=x86_?20:32,host_offset=x86_?12:16,port_offset=x86_?16:24;
                        std::vector<std::uint8_t> bytes(entries.size()*stride);
                        const auto put=[&](std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));};
                        std::vector<std::size_t> hosts(entries.size());
                        for(std::size_t i=0;i<entries.size();++i){
                            const auto& entry=entries[i];const auto at=i*stride;put(at,entry.proxy?1:0,4);put(at+4,entry.bypass?1:0,4);put(at+8,entry.scheme,4);put(at+port_offset,entry.port,2);
                            if(!entry.host.empty()){hosts[i]=bytes.size();const auto host=utf16_bytes(utf8_to_utf16(entry.host));bytes.insert(bytes.end(),host.begin(),host.end());}
                        }
                        const auto base=bytes.empty()?0:allocate_heap(memory,bytes.size());if(!bytes.empty() && !base)throw Error{8};
                        try{
                            for(std::size_t i=0;i<hosts.size();++i)if(hosts[i])put(i*stride+host_offset,base+hosts[i],unit);
                            if(base){memory.write(GuestAddress{base},bytes);proxy_result_allocations_.emplace(base,bytes.size());}
                            memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(2*unit));memory.store(GuestAddress{arg(1)},32,entries.size());memory.store(GuestAddress{arg(1)+unit},bits,base);
                        }catch(...){if(base){proxy_result_allocations_.erase(base);free_heap(memory,base,state);}throw;}
                    }else if(symbol=="WinHttpGetProxyForUrlEx"){
                        if(resolver.task)throw Error{12019};if(!arg(1) || !arg(2))throw Error{87};memory.check(GuestAddress{arg(2)},x86_?24:32,Access::Read);
                        AutoProxyInput input;input.http.timeouts=handle.input.timeouts;
                        input.target_url=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)}));
                        input.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));input.detect_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4},32));const auto script=memory.load(GuestAddress{arg(2)+8},bits);
                        if(input.flags&~0x1d000fU)throw Error{50};
                        if(!(input.flags&3) || ((input.flags&12)==12) || (input.flags&1?(!input.detect_flags || (input.detect_flags&~3U)):input.detect_flags!=0) || (input.flags&2?!script:script!=0) || memory.load(GuestAddress{arg(2)+8+unit},bits) || memory.load(GuestAddress{arg(2)+8+2*unit},32))throw Error{87};
                        input.script_url=script?utf16_to_utf8(read_utf16(memory,GuestAddress{script})):std::string{};
                        if(!handle.callback || !(handle.callback_flags&0x1200000U))throw Error{87};
                        if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行自動代理外部查詢");
                        auto task=start_native_autoproxy(std::move(input));if(!task)throw Error{50};
                        const auto callback=queue_http_callback(state,memory,arg(0),0x1200000U,std::vector<std::uint8_t>(2*unit),arg(3),false);
                        resolver.task=std::move(task);resolver.completed=false;resolver.result={};resolver.callback_wait=callback;resolver.context=arg(3);handle.context=arg(3);result=997;
                    }else throw HostCallFault("尚未實作非同步代理服務："+std::string(symbol));
                }
            }
        }catch(const Error& error){result=setter?(x86_?UINT32_MAX:UINT64_MAX):error.code;last_error_=error.code;}
        catch(const MemoryFault&){result=setter?(x86_?UINT32_MAX:UINT64_MAX):87;last_error_=87;}
        catch(const std::bad_alloc&){result=setter?(x86_?UINT32_MAX:UINT64_MAX):8;last_error_=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    const WindowsCall arg(state,memory,service);const bool discover=service.symbol=="WinHttpDetectAutoProxyConfigUrl";
    const auto token=UINT64_MAX-threads_.current;const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;std::uint64_t result=0,allocated=0;
    struct Error {std::uint32_t code;};
    const auto text=[&](std::uint64_t at){return utf16_to_utf8(read_utf16(memory,GuestAddress{at}));};
    const auto allocate=[&](const std::string& text){const auto bytes=utf16_bytes(utf8_to_utf16(text));const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)throw Error{8};try{memory.write(GuestAddress{pointer},bytes);gui_.global_memory.emplace(pointer,GuiGlobalMemory{pointer,bytes.size(),0,0,false});}catch(...){free_heap(memory,pointer,state);throw;}return pointer;};
    try{
        const auto output=arg(discover?1:3);if(!output)throw Error{87};memory.check(GuestAddress{output},discover?unit:3*unit,Access::Write);
        auto pending=autoproxy_.find(token);
        if(pending==autoproxy_.end()){
            AutoProxyInput input;input.discovery_only=discover;
            if(discover){input.detect_flags=static_cast<std::uint32_t>(arg(0));if(!input.detect_flags || (input.detect_flags&~3U))throw Error{87};memory.store(GuestAddress{output},bits,0);}
            else{
                const auto session=runtime_.http.handles.find(arg(0));if(session==runtime_.http.handles.end())throw Error{6};if(session->second.kind!=HttpHandle::Kind::Session)throw Error{12018};
                if(!arg(1) || !arg(2))throw Error{87};memory.check(GuestAddress{arg(2)},x86_?24:32,Access::Read);
                input.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));input.detect_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4},32));const auto script=memory.load(GuestAddress{arg(2)+8},bits);
                constexpr std::uint32_t supported=0x1d000f;
                if(input.flags&~supported)throw Error{50};
                if(!(input.flags&3) || ((input.flags&12)==12) || (input.flags&1?(!input.detect_flags || (input.detect_flags&~3U)):input.detect_flags!=0) || (input.flags&2?!script:script!=0) || memory.load(GuestAddress{arg(2)+8+unit},bits) || memory.load(GuestAddress{arg(2)+8+2*unit},32))throw Error{87};
                input.target_url=text(arg(1));input.script_url=script?text(script):std::string{};input.http.timeouts=session->second.input.timeouts;
            }
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行自動代理外部查詢");
            auto task=start_native_autoproxy(std::move(input));if(!task)throw Error{50};pending=autoproxy_.emplace(token,PendingAutoProxy{discover?0:arg(0),std::move(task)}).first;
        }
        if(pending->second.session && !runtime_.http.handles.contains(pending->second.session))throw Error{12017};
        if(!pending->second.task->done()){
            auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Http;thread.wait.handles={token};schedule(state,memory);return std::nullopt;
        }
        auto proxy=pending->second.task->take();autoproxy_.erase(pending);if(proxy.error)throw Error{proxy.error};
        if(!discover && proxy.proxy.empty() && !proxy.entries.empty() && std::all_of(proxy.entries.begin(),proxy.entries.end(),[](const auto& entry){return entry.proxy;}))throw Error{12178};
        const auto pointer=discover?allocate(proxy.script_url):proxy.proxy.empty()?0:allocate(proxy.proxy);allocated=pointer;
        if(discover)memory.store(GuestAddress{output},bits,pointer);
        else{memory.write(GuestAddress{output},std::vector<std::uint8_t>(3*unit));memory.store(GuestAddress{output},32,pointer?3:1);memory.store(GuestAddress{output+unit},bits,pointer);}
        result=1;
    }catch(const Error& error){autoproxy_.erase(token);last_error_=error.code;}
    catch(const MemoryFault&){autoproxy_.erase(token);last_error_=87;}
    catch(const std::bad_alloc&){autoproxy_.erase(token);last_error_=8;}
    if(!result && allocated){gui_.global_memory.erase(allocated);free_heap(memory,allocated,state);}
    arg.return_to(state,result);return std::nullopt;
}
}
