// D3DKMT 同步物件家族（gdi32）：CreateSynchronizationObject(2)／OpenSynchronizationObject／DestroySynchronizationObject、
// SignalSynchronizationObjectFromCpu／WaitForSynchronizationObjectFromCpu，以及 keyed mutex（Create／Open／Destroy／Acquire／Release 與 2 版）。
// 沙箱內沒有核心模式顯示驅動：fence 只由 CPU 推進；monitored fence 另提供 CPU 可讀的目前值。等待以 Win32 事件與既有等待服務承接，
// 物件與共用 handle 只在本程序內有效。狀態放在 gui_（與 D3DKMT 裝置同處），句柄與 adapter／device 共用 D3DKMT handle 空間。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t kmt_ok=0,kmt_timeout=0x102,kmt_invalid=0xc000000dU,kmt_no_memory=0xc0000017U,kmt_not_supported=0xc00000bbU;
std::uint32_t event_status(const RuntimeState& runtime,std::uint64_t handle){
    const auto event=runtime.wait_objects.find(handle);
    if(event==runtime.wait_objects.end())return 0xc0000008U;
    if(event->second.kind!=WaitObject::Kind::Event)return 0xc0000024U;
    return event->second.access&2U?kmt_ok:0xc0000022U;
}
}
std::span<const WindowsServiceDescriptor> d3dkmt_sync_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","D3DKMTCreateSynchronizationObject",1},{"gdi32.dll","D3DKMTCreateSynchronizationObject2",1},
        {"gdi32.dll","D3DKMTOpenSynchronizationObject",1},{"gdi32.dll","D3DKMTDestroySynchronizationObject",1},
        {"gdi32.dll","D3DKMTSignalSynchronizationObjectFromCpu",1},{"gdi32.dll","D3DKMTWaitForSynchronizationObjectFromCpu",1},
        {"gdi32.dll","D3DKMTCreateKeyedMutex",1},{"gdi32.dll","D3DKMTCreateKeyedMutex2",1},{"gdi32.dll","D3DKMTOpenKeyedMutex",1},{"gdi32.dll","D3DKMTOpenKeyedMutex2",1},
        {"gdi32.dll","D3DKMTDestroyKeyedMutex",1},{"gdi32.dll","D3DKMTAcquireKeyedMutex",1},{"gdi32.dll","D3DKMTAcquireKeyedMutex2",1},
        {"gdi32.dll","D3DKMTReleaseKeyedMutex",1},{"gdi32.dll","D3DKMTReleaseKeyedMutex2",1},
    };return table;
}
// 等待中的 CPU 等待者：fence 達到目標值或 keyed mutex 以指定 key 釋放時設定事件。
void WindowsServices::kmt_wake_waiters(GuestState& state,GuestMemory& memory){
    std::vector<std::pair<std::uint64_t,bool>> events;
    events.reserve(gui_.kmt_waiters.size());
    std::erase_if(gui_.kmt_waiters,[&](const GuiState::KmtWaiter& waiter){
        bool done=false;
        if(waiter.mutex){const auto found=gui_.kmt_mutexes.find(waiter.mutex);done=found==gui_.kmt_mutexes.end() || (!found->second.owned && found->second.key==waiter.key);}
        else {
            std::size_t reached=0;
            for(const auto& [object,value]:waiter.targets){const auto found=gui_.kmt_sync_objects.find(object);if(found==gui_.kmt_sync_objects.end() || found->second.value>=value)++reached;}
            done=waiter.any?reached>0:reached==waiter.targets.size();
        }
        if(done)events.emplace_back(waiter.event,!waiter.mutex);return done;
    });
    for(const auto& [event,owned]:events){
        (void)nested_service(state,memory,"kernel32.dll","SetEvent",{event});
        if(owned)(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{event});
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_d3dkmt_sync(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    const auto retain_event=[this](GuestState& s,GuestMemory& m,std::uint64_t event){
        GuestScratch scratch(*this,m,s);
        const auto slot=scratch.allocate(8);
        if(!slot)return UINT64_C(0);
        const auto self=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
        if(!nested_service(s,m,"kernel32.dll","DuplicateHandle",{self,event,self,slot,0,0,2}))return UINT64_C(0);
        return m.load(GuestAddress{slot},x86_?32:64);
    };
    if(name=="D3DKMTWaitForSynchronizationObjectFromCpu" || name.starts_with("D3DKMTAcquireKeyedMutex"))
        return nested_continuation(state,memory,"D3DKMT 等待",[this,service,retain_event](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto p=call(0);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
            if(!p)co_return finish(kmt_invalid);
            const auto make_event=[&]{return nested_service(s,m,"kernel32.dll","CreateEventW",{0,1,0,0});};
            if(service.symbol=="D3DKMTWaitForSynchronizationObjectFromCpu")try{
                // {hDevice, ObjectCount, ObjectHandleArray, FenceValueArray, hAsyncEvent, Flags(WaitAny)}：全部（或任一）fence 達到目標值即完成；
                // 有 hAsyncEvent 時立即返回，達到時設定該事件。
                const auto device=static_cast<std::uint32_t>(m.load(GuestAddress{p},32)),count=static_cast<std::uint32_t>(m.load(GuestAddress{p+4},32));
                const auto handles=m.load(GuestAddress{p+8},bits),values=m.load(GuestAddress{p+8+unit},bits),event=m.load(GuestAddress{p+8+2*unit},bits);
                const auto flags=m.load(GuestAddress{p+8+3*unit},32);
                if(flags>1)co_return finish(kmt_invalid);
                const bool any=flags!=0;
                if(event)if(const auto status=event_status(runtime_,event))co_return finish(status);
                if(!gui_.kmt_devices.contains(device) || !count || count>64 || !handles || !values)co_return finish(kmt_invalid);
                GuiState::KmtWaiter waiter;waiter.any=any;std::size_t reached=0;
                for(std::uint32_t i=0;i<count;++i){
                    const auto handle=static_cast<std::uint32_t>(m.load(GuestAddress{handles+i*4},32));const auto object=gui_.kmt_sync_handles.find(handle);
                    if(object==gui_.kmt_sync_handles.end())co_return finish(kmt_invalid);const auto& sync=gui_.kmt_sync_objects.at(object->second);
                    if(sync.type!=3 && sync.type!=5)co_return finish(kmt_invalid);
                    const auto target=m.load(GuestAddress{values+i*8},64);if(sync.value>=target)++reached;waiter.targets.emplace_back(object->second,target);
                }
                if(any?reached>0:reached==count){if(event)(void)nested_service(s,m,"kernel32.dll","SetEvent",{event});co_return finish(kmt_ok);}
                // fence 等待持有自己的事件參照，呼叫者關閉原 handle 後仍可喚醒同一物件。
                gui_.kmt_waiters.reserve(gui_.kmt_waiters.size()+1);
                const auto message=event?event:make_event();
                if(!message)co_return finish(kmt_no_memory);
                waiter.event=retain_event(s,m,message);
                if(!waiter.event){
                    if(!event)(void)nested_service(s,m,"kernel32.dll","CloseHandle",{message});
                    co_return finish(kmt_no_memory);
                }
                gui_.kmt_waiters.push_back(std::move(waiter));
                if(!event){
                    (void)co_await service_callback::call(resolve("kernel32.dll","WaitForSingleObject").value,{message,UINT32_MAX});
                    (void)nested_service(s,m,"kernel32.dll","CloseHandle",{message});
                }
                co_return finish(kmt_ok);
            }catch(const MemoryFault&){co_return finish(0xc0000005U);}
             catch(const std::bad_alloc&){co_return finish(kmt_no_memory);}
            // AcquireKeyedMutex(2)：{hKeyedMutex, Key, pTimeout, FenceValue（輸出）[, 私有資料, 大小]}：mutex 以相同 key 釋放後才取得；
            // pTimeout 為 100 ns 單位（負值為相對、正值為絕對時間），NULL 為無限，逾時回 STATUS_TIMEOUT。
            const bool second=service.symbol.ends_with('2');const auto handle=static_cast<std::uint32_t>(m.load(GuestAddress{p},32));
            const auto key=m.load(GuestAddress{p+8},64),timeout_pointer=m.load(GuestAddress{p+16},bits);
            if(!gui_.kmt_mutex_handles.contains(handle))co_return finish(kmt_invalid);const auto mutex=gui_.kmt_mutex_handles.at(handle);
            // 期限以系統時間（100 ns）保存，被喚醒但又被其他持有者搶先時以剩餘時間繼續等待。
            std::optional<std::int64_t> deadline;
            if(timeout_pointer){const auto value=std::bit_cast<std::int64_t>(m.load(GuestAddress{timeout_pointer},64));
                const auto now=static_cast<std::int64_t>(runtime_.system_filetime());deadline=value<0?(value<now-INT64_MAX?INT64_MAX:now-value):value;}
            for(;;){
                auto found=gui_.kmt_mutexes.find(mutex);if(found==gui_.kmt_mutexes.end())co_return finish(kmt_invalid);auto& state_=found->second;
                if(!state_.owned && state_.key==key){
                    m.check(GuestAddress{p+24},8,Access::Write);
                    if(second){
                        const auto data=m.load(GuestAddress{p+32},bits);
                        const auto size=m.load(GuestAddress{p+32+unit},32);
                        if(size && !data)co_return finish(kmt_invalid);
                        if(data && size)m.write(GuestAddress{data},std::span(state_.private_data).first(std::min<std::size_t>(size,state_.private_data.size())));
                    }
                    m.store(GuestAddress{p+24},64,state_.fence);
                    state_.owned=true;state_.owner=handle;
                    co_return finish(kmt_ok);
                }
                std::uint32_t milliseconds=UINT32_MAX;
                if(deadline){const auto left=*deadline-static_cast<std::int64_t>(runtime_.system_filetime());
                    milliseconds=left<=0?0U:static_cast<std::uint32_t>(std::min<std::int64_t>(left/10000+(left%10000!=0),0xfffffffe));}
                if(!milliseconds)co_return finish(kmt_timeout);
                GuiState::KmtWaiter waiter;waiter.mutex=mutex;waiter.key=key;waiter.event=make_event();if(!waiter.event)co_return finish(kmt_no_memory);
                const auto own=waiter.event;gui_.kmt_waiters.push_back(std::move(waiter));
                const auto wait=static_cast<std::uint32_t>(co_await service_callback::call(resolve("kernel32.dll","WaitForSingleObject").value,{own,milliseconds}));
                std::erase_if(gui_.kmt_waiters,[&](const GuiState::KmtWaiter& item){return item.event==own;});(void)nested_service(s,m,"kernel32.dll","CloseHandle",{own});
                if(wait==kmt_timeout){
                    // 逾時與釋放可能同時發生：最後再檢查一次是否已可取得。
                    const auto again=gui_.kmt_mutexes.find(mutex);if(again==gui_.kmt_mutexes.end() || again->second.owned || again->second.key!=key)co_return finish(kmt_timeout);
                    deadline=static_cast<std::int64_t>(runtime_.system_filetime())+1;
                }
            }
        });
    const WindowsCall arg(state,memory,service);const auto p=arg(0);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    const auto handle_count=gui_.kmt_sync_handles.size()+gui_.kmt_mutex_handles.size();
    const auto new_handle=[&]{return gui_.next_kmt_sync+=4;};
    if(!p)return finish(kmt_invalid);
    try{
        if(name.starts_with("D3DKMTCreateSynchronizationObject")){
            // 一般版 {hDevice, Info{Type, union[64]}, hSyncObject}：只有 mutex（1）與 semaphore（2）；
            // 2 版 {hDevice, Info2{Type, Flags, union[64]（8 位元組對齊）, SharedHandle}, hSyncObject}：另含 fence（3）、CPU 通知（4）與 monitored fence（5）。
            // 週期性 monitored fence（6）由 VSync 推進且需 adapter／VidPn target，沙箱沒有對應的顯示時序來源，回 STATUS_NOT_SUPPORTED。
            const bool second=name.ends_with('2');const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            const auto info=p+(second?8U:4U);const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{info},32));const auto data=info+(second?8U:4U);
            if(!gui_.kmt_devices.contains(device) || !type || type>(second?6U:2U))return finish(kmt_invalid);
            if(type==6)return finish(kmt_not_supported);
            if(handle_count>=4096)return finish(kmt_no_memory);
            memory.check(GuestAddress{p+(second?88U:72U)},4,Access::Write);
            if(second)memory.check(GuestAddress{info+72},4,Access::Write);
            const bool shared_object=second && (memory.load(GuestAddress{info+4},32)&1U);
            GuiState::KmtSync sync;sync.type=type;sync.device=device;
            if(type==1)sync.value=memory.load(GuestAddress{data},32)!=0;
            else if(type==2){sync.maximum=memory.load(GuestAddress{data},32);sync.value=memory.load(GuestAddress{data+4},32);if(!sync.maximum || sync.value>sync.maximum)return finish(kmt_invalid);}
            else if(type==3 || type==5)sync.value=memory.load(GuestAddress{data},64);
            else {
                const auto event=memory.load(GuestAddress{data},bits);
                if(!event)return finish(kmt_invalid);
                if(const auto status=event_status(runtime_,event))return finish(status);
                sync.event=retain_event(state,memory,event);
                if(!sync.event)return finish(kmt_no_memory);
            }
            if(type==5){
                // monitored fence 的目前值放在 CPU 可讀的 8 位元組；沒有 GPU 虛擬位址。
                memory.check(GuestAddress{data+8},unit,Access::Write);
                memory.check(GuestAddress{data+16},8,Access::Write);
                sync.cpu_address=allocate_heap(memory,8);if(!sync.cpu_address)return finish(kmt_no_memory);memory.store(GuestAddress{sync.cpu_address},64,sync.value);
                memory.store(GuestAddress{data+8},bits,sync.cpu_address);memory.store(GuestAddress{data+16},64,0);
            }
            const auto object=gui_.next_kmt_object++;const auto handle=new_handle();gui_.kmt_sync_objects.emplace(object,sync);gui_.kmt_sync_handles.emplace(handle,object);
            // Flags.Shared（bit 0）建立全域共用 handle；未共用時 SharedHandle 為 0。
            if(second){std::uint32_t shared=0;if(shared_object){shared=new_handle();gui_.kmt_sync_shared.emplace(shared,object);}memory.store(GuestAddress{info+72},32,shared);}
            memory.store(GuestAddress{p+(second?88U:72U)},32,handle);return finish(kmt_ok);
        }
        if(name=="D3DKMTOpenSynchronizationObject"){
            const auto shared=gui_.kmt_sync_shared.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));if(shared==gui_.kmt_sync_shared.end())return finish(kmt_invalid);
            if(handle_count>=4096)return finish(kmt_no_memory);
            memory.check(GuestAddress{p+4},4,Access::Write);
            const auto handle=new_handle();gui_.kmt_sync_handles.emplace(handle,shared->second);memory.store(GuestAddress{p+4},32,handle);return finish(kmt_ok);
        }
        if(name=="D3DKMTDestroySynchronizationObject"){
            const auto found=gui_.kmt_sync_handles.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));if(found==gui_.kmt_sync_handles.end())return finish(kmt_invalid);
            const auto object=found->second;
            if(gui_.kmt_sync_objects.at(object).paging)return finish(kmt_invalid);
            gui_.kmt_sync_handles.erase(found);
            if(std::none_of(gui_.kmt_sync_handles.begin(),gui_.kmt_sync_handles.end(),[&](const auto& item){return item.second==object;})){
                if(const auto address=gui_.kmt_sync_objects.at(object).cpu_address)(void)free_heap(memory,address,state);
                if(const auto event=gui_.kmt_sync_objects.at(object).event)(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{event});
                gui_.kmt_sync_objects.erase(object);std::erase_if(gui_.kmt_sync_shared,[&](const auto& item){return item.second==object;});
                kmt_wake_waiters(state,memory); // 物件消失時等待者不再無限等待。
            }
            return finish(kmt_ok);
        }
        if(name=="D3DKMTSignalSynchronizationObjectFromCpu"){
            // {hDevice, ObjectCount, ObjectHandleArray, FenceValueArray, Flags}：fence 只往前推進，Flags.AllowFenceRewind（bit 2）允許倒回；
            // monitored fence 同步更新 CPU 可讀的值。
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)),count=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
            const auto handles=memory.load(GuestAddress{p+8},bits),values=memory.load(GuestAddress{p+8+unit},bits);
            const auto flags=memory.load(GuestAddress{p+8+2*unit},32);
            if(flags&~UINT64_C(7))return finish(kmt_invalid);
            const bool rewind=(flags&4U)!=0;
            if(!gui_.kmt_devices.contains(device) || !count || count>64 || !handles || !values)return finish(kmt_invalid);
            std::vector<std::pair<std::uint64_t,std::uint64_t>> updates;
            for(std::uint32_t i=0;i<count;++i){
                const auto found=gui_.kmt_sync_handles.find(static_cast<std::uint32_t>(memory.load(GuestAddress{handles+i*4},32)));if(found==gui_.kmt_sync_handles.end())return finish(kmt_invalid);
                const auto& sync=gui_.kmt_sync_objects.at(found->second);if(sync.paging || (sync.type!=3 && sync.type!=5))return finish(kmt_invalid);
                if(sync.cpu_address)memory.check(GuestAddress{sync.cpu_address},8,Access::Write);
                updates.emplace_back(found->second,memory.load(GuestAddress{values+i*8},64));
            }
            for(const auto& [object,value]:updates){auto& sync=gui_.kmt_sync_objects.at(object);sync.value=rewind?value:std::max(sync.value,value);if(sync.cpu_address)memory.store(GuestAddress{sync.cpu_address},64,sync.value);}
            kmt_wake_waiters(state,memory);return finish(kmt_ok);
        }
        if(name.starts_with("D3DKMTCreateKeyedMutex")){
            // {InitialValue, hSharedHandle（輸出）, hKeyedMutex（輸出）[, 私有資料, 大小, Flags]}：建立時即以 InitialValue 為 key 的釋放狀態。
            if(handle_count>=4096)return finish(kmt_no_memory);GuiState::KmtMutex mutex;mutex.key=memory.load(GuestAddress{p},64);
            bool nt_sharing=false;
            if(name.ends_with('2')){const auto data=memory.load(GuestAddress{p+16},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{p+16+unit},32));
                const auto flags=memory.load(GuestAddress{p+20+unit},32);
                if(flags&~1U)return finish(kmt_invalid);
                nt_sharing=(flags&1U)!=0;
                if(size>65536 || (size && !data))return finish(kmt_invalid);std::vector<std::uint8_t> bytes(size);if(size)memory.read(GuestAddress{data},bytes);mutex.private_data=std::move(bytes);}
            memory.check(GuestAddress{p+8},8,Access::Write);
            const auto object=gui_.next_kmt_object++;const auto shared=nt_sharing?0U:new_handle(),handle=new_handle();
            gui_.kmt_mutexes.emplace(object,std::move(mutex));if(shared)gui_.kmt_mutex_shared.emplace(shared,object);gui_.kmt_mutex_handles.emplace(handle,object);
            memory.store(GuestAddress{p+8},32,shared);memory.store(GuestAddress{p+12},32,handle);return finish(kmt_ok);
        }
        if(name.starts_with("D3DKMTOpenKeyedMutex")){
            const auto shared=gui_.kmt_mutex_shared.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));if(shared==gui_.kmt_mutex_shared.end())return finish(kmt_invalid);
            if(handle_count>=4096)return finish(kmt_no_memory);
            memory.check(GuestAddress{p+4},4,Access::Write);
            // 2 版的私有資料是輸入：keyed mutex 尚無私有資料時才複製。
            if(name.ends_with('2')){const auto data=memory.load(GuestAddress{p+8},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8+unit},32));
                if(size>65536 || (size && !data))return finish(kmt_invalid);auto& stored=gui_.kmt_mutexes.at(shared->second).private_data;
                if(stored.empty() && size){std::vector<std::uint8_t> initial(size);memory.read(GuestAddress{data},initial);stored=std::move(initial);}}
            const auto handle=new_handle();gui_.kmt_mutex_handles.emplace(handle,shared->second);memory.store(GuestAddress{p+4},32,handle);return finish(kmt_ok);
        }
        if(name=="D3DKMTDestroyKeyedMutex"){
            const auto found=gui_.kmt_mutex_handles.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));if(found==gui_.kmt_mutex_handles.end())return finish(kmt_invalid);
            const auto object=found->second;const auto handle=found->first;gui_.kmt_mutex_handles.erase(found);
            auto& mutex=gui_.kmt_mutexes.at(object);if(mutex.owned && mutex.owner==handle)mutex.owned=false;
            if(std::none_of(gui_.kmt_mutex_handles.begin(),gui_.kmt_mutex_handles.end(),[&](const auto& item){return item.second==object;})){
                gui_.kmt_mutexes.erase(object);std::erase_if(gui_.kmt_mutex_shared,[&](const auto& item){return item.second==object;});}
            kmt_wake_waiters(state,memory);return finish(kmt_ok);
        }
        if(name.starts_with("D3DKMTReleaseKeyedMutex")){
            // {hKeyedMutex, Key, FenceValue[, 私有資料, 大小]}：只有持有者可釋放；以新 key 釋放並喚醒等待該 key 的取得者。
            const auto found=gui_.kmt_mutex_handles.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));if(found==gui_.kmt_mutex_handles.end())return finish(kmt_invalid);
            auto& mutex=gui_.kmt_mutexes.at(found->second);if(!mutex.owned || mutex.owner!=found->first)return finish(kmt_invalid);
            const auto key=memory.load(GuestAddress{p+8},64),fence=memory.load(GuestAddress{p+16},64);
            if(name.ends_with('2')){const auto data=memory.load(GuestAddress{p+24},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{p+24+unit},32));
                if(size>65536 || (size && !data))return finish(kmt_invalid);std::vector<std::uint8_t> bytes(size);if(size)memory.read(GuestAddress{data},bytes);mutex.private_data=std::move(bytes);}
            mutex.key=key;mutex.fence=fence;mutex.owned=false;mutex.owner=0;
            kmt_wake_waiters(state,memory);return finish(kmt_ok);
        }
    }catch(const MemoryFault&){return finish(0xc0000005U);}
    throw HostCallFault("未知的 D3DKMT 同步服務");
}
}
