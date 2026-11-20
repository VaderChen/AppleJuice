#include "vidpn_ownership.hpp"
#include "windows_display_identity.hpp"
// D3DKMT 配置與資源共享家族（gdi32）：CreateAllocation(2)／DestroyAllocation(2)、QueryResourceInfo(FromNtHandle)、
// OpenResource(2)／OpenResourceFromNtHandle、ShareObjects、OpenNtHandleFromName、OpenSyncObjectFromNtHandle(2)、
// OpenSyncObjectNtHandleFromName、OpenKeyedMutexFromNtHandle 與 Escape。
// 沙箱沒有核心模式顯示驅動：配置不佔視訊記憶體，只保存執行階段與驅動私有資料，供其他裝置以全域共用 handle 或 NT handle 開啟。
// NT handle 是 NtCreateSection 建立的區段（名稱語義與一般區段相同），內容為共用物件的序列化描述：同一程序直接指回原物件，
// 其他程序由描述重建資源（keyed mutex 與同步物件的狀態不跨程序，只能在建立它的程序內開啟）。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <numeric>

namespace x64arm {
namespace {
constexpr std::uint32_t kmt_ok=0,kmt_invalid=0xc000000dU,kmt_no_memory=0xc0000017U,kmt_not_supported=0xc00000bbU,kmt_too_small=0xc0000023U;
constexpr std::uint32_t share_magic=0x53544d4bU; // 'KMTS'
constexpr std::size_t private_limit=1U<<20;
struct ShareBlob {
    std::uint32_t pid{},flags{};std::uint64_t resource{},mutex{},sync{};
    std::vector<std::uint8_t> runtime,driver,mutex_data;std::vector<std::vector<std::uint8_t>> allocations;
};
void put32(std::vector<std::uint8_t>& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
void put64(std::vector<std::uint8_t>& out,std::uint64_t value){for(unsigned i=0;i<8;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
std::vector<std::uint8_t> serialize(const ShareBlob& blob){
    std::vector<std::uint8_t> out;put32(out,share_magic);put32(out,1);put32(out,blob.pid);put32(out,blob.flags);
    put64(out,blob.resource);put64(out,blob.mutex);put64(out,blob.sync);
    put32(out,static_cast<std::uint32_t>(blob.runtime.size()));put32(out,static_cast<std::uint32_t>(blob.driver.size()));
    put32(out,static_cast<std::uint32_t>(blob.allocations.size()));put32(out,static_cast<std::uint32_t>(blob.mutex_data.size()));
    out.insert(out.end(),blob.runtime.begin(),blob.runtime.end());out.insert(out.end(),blob.driver.begin(),blob.driver.end());
    for(const auto& item:blob.allocations){put32(out,static_cast<std::uint32_t>(item.size()));out.insert(out.end(),item.begin(),item.end());}
    out.insert(out.end(),blob.mutex_data.begin(),blob.mutex_data.end());return out;
}
std::optional<ShareBlob> deserialize(std::span<const std::uint8_t> in){
    std::size_t at=0;bool ok=true;
    const auto get32=[&]{if(at+4>in.size()){ok=false;return 0U;}std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(in[at+i])<<(8*i);at+=4;return v;};
    const auto get64=[&]{const std::uint64_t low=get32();return low|(std::uint64_t{get32()}<<32);};
    const auto bytes=[&](std::uint32_t size){std::vector<std::uint8_t> out;if(size>private_limit || at+size>in.size()){ok=false;return out;}out.assign(in.begin()+static_cast<std::ptrdiff_t>(at),in.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;return out;};
    if(get32()!=share_magic || get32()!=1)return std::nullopt;
    ShareBlob blob;blob.pid=get32();blob.flags=get32();blob.resource=get64();blob.mutex=get64();blob.sync=get64();
    const auto runtime=get32(),driver=get32(),count=get32(),mutex=get32();if(!ok || count>4096)return std::nullopt;
    blob.runtime=bytes(runtime);blob.driver=bytes(driver);for(std::uint32_t i=0;i<count && ok;++i)blob.allocations.push_back(bytes(get32()));
    blob.mutex_data=bytes(mutex);if(!ok)return std::nullopt;return blob;
}
std::vector<std::uint8_t> read_private(const GuestMemory& memory,std::uint64_t pointer,std::uint64_t size){
    if(!size)return {};if(!pointer || size>private_limit)throw std::invalid_argument("private data");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));memory.read(GuestAddress{pointer},out);return out;
}
}
std::span<const WindowsServiceDescriptor> d3dkmt_resource_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"win32u.dll","NtGdiDdDDICreatePagingQueue",1},{"win32u.dll","NtGdiDdDDIDestroyPagingQueue",1},
        {"win32u.dll","NtGdiDdDDIOfferAllocations",1},{"win32u.dll","NtGdiDdDDIReclaimAllocations",1},
        {"win32u.dll","NtGdiDdDDIReclaimAllocations2",1},{"win32u.dll","NtGdiDdDDIQueryAllocationResidency",1},
        {"win32u.dll","NtGdiDdDDIGetAllocationPriority",1},{"win32u.dll","NtGdiDdDDISetAllocationPriority",1},
        {"gdi32.dll","D3DKMTGetSharedResourceAdapterLuid",1},
        {"gdi32.dll","D3DKMTDestroyDevice",1},
        {"gdi32.dll","D3DKMTCreateAllocation",1},{"gdi32.dll","D3DKMTCreateAllocation2",1},{"gdi32.dll","D3DKMTDestroyAllocation",1},{"gdi32.dll","D3DKMTDestroyAllocation2",1},
        {"gdi32.dll","D3DKMTQueryResourceInfo",1},{"gdi32.dll","D3DKMTQueryResourceInfoFromNtHandle",1},
        {"gdi32.dll","D3DKMTOpenResource",1},{"gdi32.dll","D3DKMTOpenResource2",1},{"gdi32.dll","D3DKMTOpenResourceFromNtHandle",1},
        {"gdi32.dll","D3DKMTShareObjects",5,0,0,0x9},{"gdi32.dll","D3DKMTOpenNtHandleFromName",1},{"gdi32.dll","D3DKMTOpenSyncObjectNtHandleFromName",1},
        {"gdi32.dll","D3DKMTOpenSyncObjectFromNtHandle",1},{"gdi32.dll","D3DKMTOpenSyncObjectFromNtHandle2",1},{"gdi32.dll","D3DKMTOpenKeyedMutexFromNtHandle",1},
        {"gdi32.dll","D3DKMTEscape",1}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_d3dkmt_resource(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto new_handle=[this]{return gui_.next_kmt_sync+=4;};
    // 本機資源 handle 全部關閉、也沒有全域共用 handle 時釋放資源。
    const auto release_resource=[this](std::uint64_t object){
        const bool used=std::any_of(gui_.kmt_resource_handles.begin(),gui_.kmt_resource_handles.end(),[&](const auto& item){return item.second.second==object;});
        if(!used){gui_.kmt_resources.erase(object);std::erase_if(gui_.kmt_resource_shared,[&](const auto& item){return item.second==object;});
            std::erase_if(gui_.kmt_allocations,[&](const auto& item){return item.second.resource==object;});}
    };
    // 開啟資源的共同部分：建立本機資源與配置 handle，複製執行階段、資源驅動資料，並把各配置的驅動資料接續寫入總緩衝區。
    struct OpenFields {std::uint64_t count,info,runtime,runtime_size,driver,driver_size,total,total_size,resource;bool second;};
    const auto open_resource=[&](std::uint32_t device,std::uint64_t object,const OpenFields& f,std::uint64_t p)->std::uint32_t{
        const auto& resource=gui_.kmt_resources.at(object);
        const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{p+f.count},32));
        if(count!=resource.allocations.size() || (count && !memory.load(GuestAddress{p+f.info},bits)))return kmt_invalid;
        const auto runtime=memory.load(GuestAddress{p+f.runtime},bits),driver=memory.load(GuestAddress{p+f.driver},bits);
        const auto fits=[&](std::uint64_t buffer,std::uint64_t size_at,std::size_t required){
            if(!required)return true;
            if(!buffer || memory.load(GuestAddress{p+size_at},32)<required)return false;
            memory.check(GuestAddress{buffer},required,Access::Write);return true;
        };
        const auto total=std::accumulate(resource.allocations.begin(),resource.allocations.end(),std::size_t{},[](auto sum,const auto& item){return sum+item.size();});
        const auto buffer=memory.load(GuestAddress{p+f.total},bits);
        if(!fits(runtime,f.runtime_size,resource.runtime_data.size()) || !fits(driver,f.driver_size,resource.driver_data.size()) ||
            !fits(buffer,f.total_size,total))return kmt_too_small;
        const auto info=memory.load(GuestAddress{p+f.info},bits);const auto stride=f.second?(x86_?48U:80U):(x86_?12U:24U);
        if(count)memory.check(GuestAddress{info},std::size_t{count}*stride,Access::Write);
        memory.check(GuestAddress{p+f.resource},4,Access::Write);
        if(!resource.runtime_data.empty())memory.write(GuestAddress{runtime},resource.runtime_data);
        if(!resource.driver_data.empty())memory.write(GuestAddress{driver},resource.driver_data);
        const auto local=new_handle();gui_.kmt_resource_handles.emplace(local,std::pair{device,object});
        std::uint64_t cursor=buffer;
        for(std::uint32_t i=0;i<count;++i){
            const auto entry=info+i*stride;const auto handle=new_handle();const auto& data=resource.allocations[i];
            auto& allocation=gui_.kmt_allocations.emplace(handle,GuiState::KmtAllocation{device,object,data,resource.allocation_ids[i],false}).first->second;
            if(auto it=resource.backings.find(allocation.entry);it!=resource.backings.end())allocation.backing=it->second;
            memory.store(GuestAddress{entry},32,handle);memory.store(GuestAddress{entry+unit},bits,data.empty()?0:cursor);memory.store(GuestAddress{entry+2*unit},32,data.size());
            if(f.second)memory.store(GuestAddress{entry+(x86_?16U:24U)},64,0); // 沒有 GPU 虛擬位址空間。
            if(!data.empty()){memory.write(GuestAddress{cursor},data);cursor+=data.size();}
        }
        memory.store(GuestAddress{p+f.resource},32,local);return kmt_ok;
    };
    // NT handle（區段）的讀寫：區段內容為 ShareBlob。
    const auto read_share=[this](GuestState& s,GuestMemory& m,std::uint64_t handle)->std::optional<ShareBlob>{
        if(!handle)return std::nullopt;const auto view=nested_service(s,m,"kernel32.dll","MapViewOfFile",{handle,4,0,0,0});if(!view)return std::nullopt;
        std::optional<ShareBlob> blob;
        try{
            // 描述長度依標頭與各配置前的長度欄逐段累計；超出映射範圍視為無效的共用 handle。
            const auto u32=[&](std::uint64_t at){return static_cast<std::uint32_t>(m.load(GuestAddress{view+at},32));};
            if(u32(0)==share_magic){
                std::uint64_t size=56+std::uint64_t{u32(40)}+u32(44);
                for(std::uint32_t i=0,count=std::min(u32(48),4096U);i<count && size<=64*private_limit;++i)size+=4+std::uint64_t{u32(size)};
                size+=u32(52);
                if(size<=64*private_limit){std::vector<std::uint8_t> whole(static_cast<std::size_t>(size));m.read(GuestAddress{view},whole);blob=deserialize(whole);}
            }
        }catch(const MemoryFault&){blob.reset();}
        (void)nested_service(s,m,"kernel32.dll","UnmapViewOfFile",{view});return blob;
    };
    const auto blob_resource=[this](const ShareBlob& blob)->std::optional<std::uint64_t>{
        if(!(blob.flags&1U))return std::nullopt;
        if(blob.pid==process_id() && gui_.kmt_resources.contains(blob.resource))return blob.resource;
        // 其他程序（或原資源已釋放）：依描述重建一份資源。
        GuiState::KmtResource resource;resource.runtime_data=blob.runtime;resource.driver_data=blob.driver;resource.allocations=blob.allocations;
        for(std::size_t i=0;i<blob.allocations.size();++i)resource.allocation_ids.push_back(gui_.next_kmt_object++);
        const auto object=gui_.next_kmt_object++;gui_.kmt_resources.emplace(object,std::move(resource));return object;
    };
    // 區段的建立與具名開啟經 ntdll 服務（非同步原生操作），其餘讀取共用描述的入口直接完成。
    if(name=="D3DKMTShareObjects" || name.find("NtHandleFromName")!=name.npos)
        return nested_continuation(state,memory,"D3DKMT 共用",[this,service,unit,bits,read_share](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
            const auto symbol=service.symbol;
            if(symbol=="D3DKMTOpenNtHandleFromName" || symbol=="D3DKMTOpenSyncObjectNtHandleFromName"){
                // {dwDesiredAccess, OBJECT_ATTRIBUTES*, hNtHandle（輸出）}：以 NtOpenSection 開啟具名的共用區段。
                const auto p=call(0);if(!p)co_return finish(kmt_invalid);const auto attributes=m.load(GuestAddress{p+unit},bits);if(!attributes)co_return finish(kmt_invalid);
                const auto out=allocate_heap(m,8);if(!out)co_return finish(kmt_no_memory);
                const auto status=static_cast<std::uint32_t>(co_await service_callback::call(resolve("ntdll.dll","NtOpenSection").value,{out,0x5,attributes}));
                const auto handle=m.load(GuestAddress{out},bits);(void)free_heap(m,out,s);if(status)co_return finish(status);
                if(!read_share(s,m,handle)){(void)nested_service(s,m,"kernel32.dll","CloseHandle",{handle});co_return finish(kmt_invalid);}
                m.store(GuestAddress{p+2*unit},bits,handle);co_return finish(kmt_ok);
            }
            if(symbol=="D3DKMTShareObjects"){
                // (cObjects, hObjects[], OBJECT_ATTRIBUTES*, dwDesiredAccess, HANDLE*)：資源、keyed mutex、同步物件各至多一個。
                const auto count=static_cast<std::uint32_t>(call(0));const std::uint64_t list=call(1),attributes=call(2),out=call(4);
                if(!count || count>3 || !list || !out)co_return finish(kmt_invalid);
                ShareBlob blob;blob.pid=process_id();
                for(std::uint32_t i=0;i<count;++i){
                    const auto handle=static_cast<std::uint32_t>(m.load(GuestAddress{list+i*4},32));
                    if(const auto resource=gui_.kmt_resource_handles.find(handle);resource!=gui_.kmt_resource_handles.end() && !(blob.flags&1U)){
                        const auto& item=gui_.kmt_resources.at(resource->second.second);blob.flags|=1U;blob.resource=resource->second.second;
                        blob.runtime=item.runtime_data;blob.driver=item.driver_data;blob.allocations=item.allocations;
                    }else if(const auto mutex=gui_.kmt_mutex_handles.find(handle);mutex!=gui_.kmt_mutex_handles.end() && !(blob.flags&2U)){
                        blob.flags|=2U;blob.mutex=mutex->second;blob.mutex_data=gui_.kmt_mutexes.at(mutex->second).private_data;
                    }else if(const auto sync=gui_.kmt_sync_handles.find(handle);sync!=gui_.kmt_sync_handles.end() && !(blob.flags&4U) && !gui_.kmt_sync_objects.at(sync->second).paging){blob.flags|=4U;blob.sync=sync->second;}
                    else co_return finish(kmt_invalid);
                }
                if((blob.flags&2U) && !(blob.flags&1U))co_return finish(kmt_invalid); // keyed mutex 只能與資源一起共用。
                const auto bytes=serialize(blob);const auto scratch=allocate_heap(m,16);if(!scratch)co_return finish(kmt_no_memory);
                m.store(GuestAddress{scratch+8},64,bytes.size());
                const auto status=static_cast<std::uint32_t>(co_await service_callback::call(resolve("ntdll.dll","NtCreateSection").value,{scratch,0xf001f,attributes,scratch+8,4,0x8000000,0}));
                const auto handle=m.load(GuestAddress{scratch},bits);(void)free_heap(m,scratch,s);if(status)co_return finish(status);
                const auto view=nested_service(s,m,"kernel32.dll","MapViewOfFile",{handle,2,0,0,0});
                if(!view){(void)nested_service(s,m,"kernel32.dll","CloseHandle",{handle});co_return finish(kmt_no_memory);}
                m.write(GuestAddress{view},bytes);(void)nested_service(s,m,"kernel32.dll","UnmapViewOfFile",{view});
                m.store(GuestAddress{out},bits,handle);co_return finish(kmt_ok);
            }
            co_return finish(kmt_invalid);
        });
    const WindowsCall call(state,memory,service);const auto finish=[&](std::uint32_t status){call.return_to(state,status);return std::optional<std::uint32_t>{};};
    const auto p=call(0);if(!p)return finish(kmt_invalid);
    try{
        const auto backed=[&](const GuiState::KmtBacking& b){return b.section || (b.address && memory.mapped(GuestAddress{b.address}));};
        const auto close_queue=[&](const GuiState::KmtPagingQueue& queue){
            const auto object=gui_.kmt_sync_handles.at(queue.sync);
            const auto address=gui_.kmt_sync_objects.at(object).cpu_address;
            (void)nested_service(state,memory,"kernel32.dll","VirtualFree",{address,0,0x8000});
            gui_.kmt_sync_handles.erase(queue.sync);
            gui_.kmt_sync_objects.erase(object);
            kmt_wake_waiters(state,memory);
        };
        if(name=="NtGdiDdDDICreatePagingQueue"){
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            const auto priority=static_cast<std::int32_t>(memory.load(GuestAddress{p+4},32));
            if(!gui_.kmt_devices.contains(device) || priority< -1 || priority>1 || memory.load(GuestAddress{p+16+unit},32))return finish(kmt_invalid);
            memory.check(GuestAddress{p+8},8+unit,Access::Write);
            if(gui_.kmt_sync_handles.size()>=4096)return finish(kmt_no_memory);
            const auto address=nested_service(state,memory,"kernel32.dll","VirtualAlloc",{0,4096,0x3000,2});
            if(!address)return finish(kmt_no_memory);
            const auto object=gui_.next_kmt_object++;
            const auto fence=new_handle();
            const auto handle=new_handle();
            GuiState::KmtSync sync;sync.type=5;sync.device=device;sync.cpu_address=address;sync.paging=true;
            gui_.kmt_sync_objects.emplace(object,sync);
            gui_.kmt_sync_handles.emplace(fence,object);
            gui_.kmt_paging_queues.emplace(handle,GuiState::KmtPagingQueue{device,fence,priority});
            memory.store(GuestAddress{p+8},32,handle);memory.store(GuestAddress{p+12},32,fence);
            memory.store(GuestAddress{p+16},bits,address);return finish(kmt_ok);
        }
        if(name=="NtGdiDdDDIDestroyPagingQueue"){
            const auto q=gui_.kmt_paging_queues.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));
            if(q==gui_.kmt_paging_queues.end())return finish(kmt_invalid);
            close_queue(q->second);gui_.kmt_paging_queues.erase(q);return finish(kmt_ok);
        }
        if(name=="NtGdiDdDDIOfferAllocations" || name.starts_with("NtGdiDdDDIReclaimAllocations")){
            const bool offer=name=="NtGdiDdDDIOfferAllocations",second=name.ends_with('2');
            auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            if(second){
                const auto q=gui_.kmt_paging_queues.find(device);
                if(q==gui_.kmt_paging_queues.end())return finish(kmt_invalid);
                device=q->second.device;
            }
            const auto owner=gui_.kmt_devices.find(device);
            if(owner==gui_.kmt_devices.end())return finish(kmt_invalid);
            const auto base=p+(second?8:unit);
            const auto resources=memory.load(GuestAddress{base},bits),allocations=memory.load(GuestAddress{base+unit},bits);
            const auto count=memory.load(GuestAddress{second?p+4:base+(offer?2:3)*unit},32);
            if(!count || bool(resources)==bool(allocations))return finish(kmt_invalid);
            const auto list=resources?resources:allocations;
            memory.check(GuestAddress{list},count*4,Access::Read);
            std::uint32_t priority{},flags{};
            if(offer){
                priority=static_cast<std::uint32_t>(memory.load(GuestAddress{base+2*unit+4},32));
                flags=static_cast<std::uint32_t>(memory.load(GuestAddress{base+2*unit+8},32));
                if(!priority || priority>4 || (flags&~3U))return finish(kmt_invalid);
            }
            const auto output=offer?0:memory.load(GuestAddress{base+2*unit},bits);
            if(output)memory.check(GuestAddress{output},count*4,Access::Write);
            const auto fence=p+(x86_?24U:32U);
            if(second)memory.check(GuestAddress{fence},8,Access::Write);
            auto& offers=owner->second.offers;
            std::map<std::uint64_t,GuiState::KmtOffer> batch;
            std::vector<std::uint32_t> results;
            for(std::uint64_t i=0;i<count;++i){
                std::uint32_t result=0;
                const auto add=[&](std::uint64_t id,bool resident){
                    const auto old=offers.find(id);
                    if(offer!=(old==offers.end()))return false;
                    if(!offer && old->second.backed && !resident)
                        result=std::max(result,second && (old->second.flags&2U)?2U:1U);
                    batch.emplace(id,GuiState::KmtOffer{priority,flags,resident});
                    return true;
                };
                const auto handle=static_cast<std::uint32_t>(memory.load(GuestAddress{list+4*i},32));
                if(resources){
                    const auto it=gui_.kmt_resource_handles.find(handle);
                    if(it==gui_.kmt_resource_handles.end() || it->second.first!=device)return finish(kmt_invalid);
                    const auto& resource=gui_.kmt_resources.at(it->second.second);
                    if(resource.allocation_ids.empty())return finish(kmt_invalid);
                    for(const auto id:resource.allocation_ids){
                        const auto b=resource.backings.find(id);
                        if(!add(id,b!=resource.backings.end() && backed(b->second)))return finish(kmt_invalid);
                    }
                }else{
                    const auto it=gui_.kmt_allocations.find(handle);
                    if(it==gui_.kmt_allocations.end() || it->second.device!=device || !add(it->second.entry,backed(it->second.backing)))return finish(kmt_invalid);
                }
                if(output)results.push_back(result);
            }
            if(offer)offers.merge(batch);
            else{
                for(const auto& item:batch)offers.erase(item.first);
                for(std::size_t i=0;i<results.size();++i)memory.store(GuestAddress{output+4*i},32,results[i]);
                // 本機取回同步完成，呼叫端無須等待後續分頁工作。
                if(second)memory.store(GuestAddress{fence},64,0);
            }
            return finish(kmt_ok);
        }
        const bool residency=name=="NtGdiDdDDIQueryAllocationResidency";
        if(residency || name=="NtGdiDdDDIGetAllocationPriority" || name=="NtGdiDdDDISetAllocationPriority"){
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            const auto resource=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
            const auto list=memory.load(GuestAddress{p+8},bits);
            const auto count=memory.load(GuestAddress{p+8+unit},32);
            const auto buffer=memory.load(GuestAddress{p+8+2*unit},bits);
            if(!gui_.kmt_devices.contains(device) || !buffer)return finish(kmt_invalid);
            // 同一程序再次開啟資源時會產生新的 handle，優先權隨配置識別碼共用。
            struct Target { GuiState::KmtResource* resource; std::uint64_t entry; GuiState::KmtAllocation* standalone; };
            std::vector<Target> targets;
            if(resource){
                const auto handle=gui_.kmt_resource_handles.find(resource);
                if(list || count || handle==gui_.kmt_resource_handles.end() || handle->second.first!=device)return finish(kmt_invalid);
                auto& object=gui_.kmt_resources.at(handle->second.second);
                for(const auto entry:object.allocation_ids)targets.push_back({&object,entry,nullptr});
                if(targets.empty())return finish(kmt_invalid);
            }else{
                if(!list || !count)return finish(kmt_invalid);
                memory.check(GuestAddress{list},count*4,Access::Read);
                for(std::uint64_t i=0;i<count;++i){
                    const auto it=gui_.kmt_allocations.find(static_cast<std::uint32_t>(memory.load(GuestAddress{list+i*4},32)));
                    if(it==gui_.kmt_allocations.end() || it->second.device!=device)return finish(kmt_invalid);
                    auto& allocation=it->second;
                    targets.push_back({allocation.resource?&gui_.kmt_resources.at(allocation.resource):nullptr,allocation.entry,&allocation});
                }
            }
            const bool set=name=="NtGdiDdDDISetAllocationPriority";
            const auto values=resource?1:count;
            memory.check(GuestAddress{buffer},values*4,set?Access::Read:Access::Write);
            if(residency){
                std::uint32_t worst=0;
                for(std::size_t i=0;i<targets.size();++i){
                    const auto& target=targets[i];
                    const GuiState::KmtBacking* backing=nullptr;
                    if(target.standalone)backing=&target.standalone->backing;
                    else if(auto it=target.resource->backings.find(target.entry);it!=target.resource->backings.end())backing=&it->second;
                    // KMT 以既有區段或客體映射作共享記憶體；僅有驅動描述的配置沒有 GPU backing。
                    const bool shared=backing && backed(*backing);
                    const auto status=shared?2U:3U;
                    if(resource)worst=std::max(worst,status);
                    else memory.store(GuestAddress{buffer+i*4},32,status);
                }
                if(resource)memory.store(GuestAddress{buffer},32,worst);
                return finish(kmt_ok);
            }
            for(std::size_t i=0;i<(set?targets.size():values);++i){
                const auto& target=targets[i];
                const GuestAddress address{buffer+(resource?0:i*4)};
                if(set){
                    const auto priority=static_cast<std::uint32_t>(memory.load(address,32));
                    if(target.resource)target.resource->priorities[target.entry]=priority;
                    else target.standalone->priority=priority;
                }else if(target.resource){
                    const auto it=target.resource->priorities.find(target.entry);
                    memory.store(address,32,it==target.resource->priorities.end()?0x78000000U:it->second);
                }else memory.store(address,32,target.standalone->priority);
            }
            return finish(kmt_ok);
        }
        if(name=="D3DKMTDestroyDevice"){
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            if(!gui_.kmt_devices.contains(device))return finish(kmt_invalid);
            std::set<std::uint64_t> resources;
            for(const auto& [handle,owner]:gui_.kmt_resource_handles)
                if(owner.first==device)resources.insert(owner.second);
            std::erase_if(gui_.kmt_resource_handles,[&](const auto& item){return item.second.first==device;});
            std::erase_if(gui_.kmt_allocations,[&](const auto& item){return item.second.device==device;});
            for(const auto object:resources)release_resource(object);
            if(gui_.kmt_vidpn_owner==device){
                std::uint32_t previous{};
                vidpn_ownership(gui_,processes_ && runtime_.network.external_io?mapping_session():std::string{},process_id(),process_id(),device,0,previous);
            }
            std::erase_if(gui_.kmt_paging_queues,[&](const auto& item){
                if(item.second.device!=device)return false;
                close_queue(item.second);return true;
            });
            gui_.kmt_devices.erase(device);
            return finish(kmt_ok);
        }
        if(name=="D3DKMTOpenSyncObjectFromNtHandle" || name=="D3DKMTOpenSyncObjectFromNtHandle2"){
            // 1 版 {hNtHandle, hSyncObject}；2 版 {hNtHandle, hDevice, Flags, hSyncObject, MonitoredFence{CPU 位址, GPU 位址, EngineAffinity}}。
            const bool second=name.ends_with('2');const auto blob=read_share(state,memory,memory.load(GuestAddress{p},bits));
            if(!blob || !(blob->flags&4U) || blob->pid!=process_id() || !gui_.kmt_sync_objects.contains(blob->sync))return finish(kmt_invalid);
            if(second && !gui_.kmt_devices.contains(static_cast<std::uint32_t>(memory.load(GuestAddress{p+unit},32))))return finish(kmt_invalid);
            const auto handle=new_handle();gui_.kmt_sync_handles.emplace(handle,blob->sync);
            memory.store(GuestAddress{p+(second?unit+8:unit)},32,handle);
            if(second){const auto& sync=gui_.kmt_sync_objects.at(blob->sync);const auto fence=p+(x86_?16U:24U);
                memory.store(GuestAddress{fence},bits,sync.type==5?sync.cpu_address:0);memory.store(GuestAddress{fence+8},64,0);}
            return finish(kmt_ok);
        }
        if(name=="D3DKMTOpenKeyedMutexFromNtHandle"){
            // {hNtHandle, hKeyedMutex（輸出）, pPrivateRuntimeData, PrivateRuntimeDataSize}。
            const auto blob=read_share(state,memory,memory.load(GuestAddress{p},bits));
            if(!blob || !(blob->flags&2U) || blob->pid!=process_id() || !gui_.kmt_mutexes.contains(blob->mutex))return finish(kmt_invalid);
            auto& initial=gui_.kmt_mutexes.at(blob->mutex).private_data;
            const auto buffer=memory.load(GuestAddress{p+2*unit},bits),size=memory.load(GuestAddress{p+3*unit},32);
            if(size>65536 || (size && !buffer))return finish(kmt_invalid);
            if(gui_.kmt_sync_handles.size()+gui_.kmt_mutex_handles.size()>=4096)return finish(kmt_no_memory);
            memory.check(GuestAddress{p+unit},4,Access::Write);
            if(initial.empty() && size)initial=read_private(memory,buffer,size);
            const auto handle=new_handle();gui_.kmt_mutex_handles.emplace(handle,blob->mutex);memory.store(GuestAddress{p+unit},32,handle);return finish(kmt_ok);
        }
        if(name=="D3DKMTGetSharedResourceAdapterLuid"){
            const auto global=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            const auto nt=memory.load(GuestAddress{p+unit},bits);
            if(bool(global)==bool(nt))return finish(kmt_invalid);
            if(global){
                if(!gui_.kmt_resource_shared.contains(global))return finish(kmt_invalid);
            }else{
                const auto blob=read_share(state,memory,nt);
                if(!blob || !(blob->flags&1U) || !blob->resource)return finish(kmt_invalid);
            }
            memory.store(GuestAddress{p+2*unit},64,display_identity::adapter_luid);
            return finish(kmt_ok);
        }
        if(name=="D3DKMTQueryResourceInfoFromNtHandle"){
            // {hDevice, hNtHandle, pPrivateRuntimeData, PrivateRuntimeDataSize, TotalPrivateDriverDataSize, ResourcePrivateDriverDataSize, NumAllocations}。
            if(!gui_.kmt_devices.contains(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32))))return finish(kmt_invalid);
            const auto blob=read_share(state,memory,memory.load(GuestAddress{p+unit},bits));if(!blob || !(blob->flags&1U))return finish(kmt_invalid);
            const auto base=p+2*unit;const auto buffer=memory.load(GuestAddress{base},bits);
            if(buffer && !blob->runtime.empty()){if(memory.load(GuestAddress{base+unit},32)<blob->runtime.size())return finish(kmt_too_small);memory.write(GuestAddress{buffer},blob->runtime);}
            const auto total=std::accumulate(blob->allocations.begin(),blob->allocations.end(),std::uint64_t{},[](auto sum,const auto& item){return sum+item.size();});
            memory.store(GuestAddress{base+unit},32,blob->runtime.size());memory.store(GuestAddress{base+unit+4},32,total);memory.store(GuestAddress{base+unit+8},32,blob->driver.size());memory.store(GuestAddress{base+unit+12},32,blob->allocations.size());
            return finish(kmt_ok);
        }
        if(name=="D3DKMTOpenResourceFromNtHandle"){
        // 開啟資源，並一併開啟同時共用的 keyed mutex 與同步物件（只在建立它們的程序內）。
        const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));if(!gui_.kmt_devices.contains(device))return finish(kmt_invalid);
        const auto blob=read_share(state,memory,memory.load(GuestAddress{p+unit},bits));if(!blob)return finish(kmt_invalid);
        const auto object=blob_resource(*blob);if(!object)return finish(kmt_invalid);
        const OpenFields fields=x86_?OpenFields{8,12,20,16,28,24,36,32,40,true}:OpenFields{16,24,40,32,56,48,72,64,80,true};
        if(const auto status=open_resource(device,*object,fields,p)){release_resource(*object);return finish(status);}
        const auto mutex_at=p+(x86_?44U:84U),sync_at=p+(x86_?56U:100U);std::uint32_t mutex_handle=0,sync_handle=0;
        if((blob->flags&2U) && blob->pid==process_id() && gui_.kmt_mutexes.contains(blob->mutex)){
            const auto& data=gui_.kmt_mutexes.at(blob->mutex).private_data;const auto buffer=memory.load(GuestAddress{p+(x86_?48U:88U)},bits);
            if(!data.empty() && buffer && memory.load(GuestAddress{p+(x86_?52U:96U)},32)>=data.size())memory.write(GuestAddress{buffer},data);
            mutex_handle=new_handle();gui_.kmt_mutex_handles.emplace(mutex_handle,blob->mutex);
        }
        if((blob->flags&4U) && blob->pid==process_id() && gui_.kmt_sync_objects.contains(blob->sync)){sync_handle=new_handle();gui_.kmt_sync_handles.emplace(sync_handle,blob->sync);}
        memory.store(GuestAddress{mutex_at},32,mutex_handle);memory.store(GuestAddress{sync_at},32,sync_handle);return finish(kmt_ok);
        }
        if(name.starts_with("D3DKMTCreateAllocation")){
            // {hDevice, hResource（輸入／輸出）, hGlobalShare（輸出）, 執行階段資料, 大小, 資源驅動資料（或標準配置描述）, 大小, 配置數, 配置資訊陣列, Flags, hPrivateRuntimeResourceHandle}。
            // Flags：CreateResource（bit 0）、CreateShared（bit 1）、ExistingSysMem（bit 5）、NtSecuritySharing（bit 6，改由 ShareObjects 取得 NT handle）、StandardAllocation（bit 16）。
            const bool second=name.ends_with('2');const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            const auto at=[&](unsigned x86_offset,unsigned x64_offset){return p+(x86_?x86_offset:x64_offset);};
            const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{at(36,56)},32));const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{at(28,44)},32));
            const auto info=memory.load(GuestAddress{at(32,48)},bits);auto resource_handle=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
            const bool create_resource=flags&1U,shared=flags&2U,system_memory=flags&0x20U,nt_sharing=flags&0x40U,standard=flags&0x10000U;
            if(!gui_.kmt_devices.contains(device) || !count || count>4096 || !info || (shared && !create_resource) || (create_resource && resource_handle))return finish(kmt_invalid);
            std::uint64_t object=0;
            if(resource_handle){const auto found=gui_.kmt_resource_handles.find(resource_handle);if(found==gui_.kmt_resource_handles.end() || found->second.first!=device)return finish(kmt_invalid);object=found->second.second;}
            const auto stride=second?(x86_?56U:96U):(x86_?24U:40U);
            std::vector<std::vector<std::uint8_t>> data;data.reserve(count);
            std::vector<std::uint32_t> priorities(count,0x78000000U);
            std::vector<GuiState::KmtBacking> backings(count);
            for(std::uint32_t i=0;i<count;++i){const auto entry=info+i*stride;
                const auto backing=memory.load(GuestAddress{entry+unit},bits);
                if(flags&0x20000U){
                    if(!second || !(backings[i].section=retain_mapping_object(backing)))return finish(kmt_invalid);
                }else{
                    if((system_memory && !backing) || (backing && !memory.mapped(GuestAddress{backing})))return finish(kmt_invalid);
                    backings[i].address=backing;
                }
                if(second && (memory.load(GuestAddress{entry+(x86_?20U:32U)},32)&4U))
                    priorities[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+(x86_?32U:48U)},32));
                data.push_back(read_private(memory,memory.load(GuestAddress{entry+2*unit},bits),memory.load(GuestAddress{entry+3*unit},32)));}
            memory.check(GuestAddress{p+4},8,Access::Write);
            for(std::uint32_t i=0;i<count;++i){
                const auto entry=info+i*stride;
                memory.check(GuestAddress{entry},4,Access::Write);
                if(second)memory.check(GuestAddress{entry+(x86_?24U:40U)},8,Access::Write);
            }
            if(create_resource){
                GuiState::KmtResource resource;resource.runtime_data=read_private(memory,memory.load(GuestAddress{at(12,16)},bits),memory.load(GuestAddress{at(16,24)},32));
                if(!standard)resource.driver_data=read_private(memory,memory.load(GuestAddress{at(20,32)},bits),memory.load(GuestAddress{at(24,40)},32));
                object=gui_.next_kmt_object++;gui_.kmt_resources.emplace(object,std::move(resource));
                resource_handle=new_handle();gui_.kmt_resource_handles.emplace(resource_handle,std::pair{device,object});
            }
            std::uint32_t share=0;if(shared && !nt_sharing){share=new_handle();gui_.kmt_resource_shared.emplace(share,object);}
            for(std::uint32_t i=0;i<count;++i){
                const auto handle=new_handle();const auto entry=gui_.next_kmt_object++;gui_.kmt_allocations.emplace(handle,GuiState::KmtAllocation{device,object,data[i],entry,true,priorities[i],backings[i]});
                if(object){auto& resource=gui_.kmt_resources.at(object);resource.allocations.push_back(data[i]);resource.allocation_ids.push_back(entry);
                    if(backings[i].address || backings[i].section)resource.backings.emplace(entry,backings[i]);
                    if(priorities[i]!=0x78000000U)resource.priorities.emplace(entry,priorities[i]);}
                memory.store(GuestAddress{info+i*stride},32,handle);
                if(second)memory.store(GuestAddress{info+i*stride+(x86_?24U:40U)},64,0);
            }
            memory.store(GuestAddress{p+4},32,resource_handle);memory.store(GuestAddress{p+8},32,share);return finish(kmt_ok);
        }
        if(name.starts_with("D3DKMTDestroyAllocation")){
            // {hDevice, hResource, phAllocationList, AllocationCount[, Flags]}：先銷毀列出的配置，有 hResource 時再關閉該資源 handle。
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)),resource_handle=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
            const std::uint64_t list=memory.load(GuestAddress{p+8},bits);const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8+unit},32));
            if(!gui_.kmt_devices.contains(device) || (count && !list) || (!count && !resource_handle))return finish(kmt_invalid);
            const auto resource_it=gui_.kmt_resource_handles.find(resource_handle);
            if(resource_handle && (resource_it==gui_.kmt_resource_handles.end() || resource_it->second.first!=device))return finish(kmt_invalid);
            for(std::uint32_t i=0;i<count;++i){const auto found=gui_.kmt_allocations.find(static_cast<std::uint32_t>(memory.load(GuestAddress{list+i*4},32)));
                if(found==gui_.kmt_allocations.end() || found->second.device!=device)return finish(kmt_invalid);}
            for(std::uint32_t i=0;i<count;++i){
                const auto found=gui_.kmt_allocations.find(static_cast<std::uint32_t>(memory.load(GuestAddress{list+i*4},32)));if(found==gui_.kmt_allocations.end())continue;
                if(found->second.owner && found->second.resource)if(const auto resource=gui_.kmt_resources.find(found->second.resource);resource!=gui_.kmt_resources.end()){
                    auto& ids=resource->second.allocation_ids;if(const auto at=std::find(ids.begin(),ids.end(),found->second.entry);at!=ids.end()){
                        resource->second.backings.erase(*at);resource->second.priorities.erase(*at);
                        resource->second.allocations.erase(resource->second.allocations.begin()+(at-ids.begin()));ids.erase(at);}}
                gui_.kmt_allocations.erase(found);
            }
            if(resource_handle){
                const auto object=resource_it->second.second;gui_.kmt_resource_handles.erase(resource_it);release_resource(object);
            }
            auto& offers=gui_.kmt_devices.at(device).offers;
            if(!offers.empty()){
                std::set<std::uint64_t> live;
                for(const auto& [handle,allocation]:gui_.kmt_allocations)
                    if(allocation.device==device)live.insert(allocation.entry);
                for(const auto& [handle,local]:gui_.kmt_resource_handles)
                    if(local.first==device){
                        const auto& ids=gui_.kmt_resources.at(local.second).allocation_ids;
                        live.insert(ids.begin(),ids.end());
                    }
                std::erase_if(offers,[&](const auto& pair){return !live.contains(pair.first);});
            }
            return finish(kmt_ok);
        }
        if(name=="D3DKMTQueryResourceInfo"){
            // {hDevice, hGlobalShare, pPrivateRuntimeData, PrivateRuntimeDataSize, TotalPrivateDriverDataSize, ResourcePrivateDriverDataSize, NumAllocations}。
            const auto shared=gui_.kmt_resource_shared.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32)));
            if(!gui_.kmt_devices.contains(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32))) || shared==gui_.kmt_resource_shared.end())return finish(kmt_invalid);
            const auto& resource=gui_.kmt_resources.at(shared->second);const auto buffer=memory.load(GuestAddress{p+8},bits),base=p+8+unit;
            if(buffer && !resource.runtime_data.empty()){if(memory.load(GuestAddress{base},32)<resource.runtime_data.size())return finish(kmt_too_small);memory.write(GuestAddress{buffer},resource.runtime_data);}
            const auto total=std::accumulate(resource.allocations.begin(),resource.allocations.end(),std::uint64_t{},[](auto sum,const auto& item){return sum+item.size();});
            memory.store(GuestAddress{base},32,resource.runtime_data.size());memory.store(GuestAddress{base+4},32,total);memory.store(GuestAddress{base+8},32,resource.driver_data.size());memory.store(GuestAddress{base+12},32,resource.allocations.size());
            return finish(kmt_ok);
        }
        if(name=="D3DKMTOpenResource" || name=="D3DKMTOpenResource2"){
            // {hDevice, hGlobalShare, NumAllocations, 開啟配置資訊陣列, 執行階段資料, 大小, 資源驅動資料, 大小, 總驅動資料緩衝區, 大小, hResource（輸出）}。
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));const auto shared=gui_.kmt_resource_shared.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32)));
            if(!gui_.kmt_devices.contains(device) || shared==gui_.kmt_resource_shared.end())return finish(kmt_invalid);
            const bool second=name.ends_with('2');
            const OpenFields fields=x86_?OpenFields{8,12,16,20,24,28,32,36,40,second}:OpenFields{8,16,24,32,40,48,56,64,68,second};
            return finish(open_resource(device,shared->second,fields,p));
        }
        if(name=="D3DKMTEscape"){
            // {hAdapter, hDevice, Type, Flags, pPrivateDriverData, PrivateDriverDataSize, hContext}：虛擬顯示配接器的驅動沒有定義任何跳脫協定。
            const auto adapter=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)),device=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
            if(!gui_.kmt_adapters.contains(adapter) || (device && !gui_.kmt_devices.contains(device)))return finish(kmt_invalid);
            const auto data=memory.load(GuestAddress{p+16},bits),size=memory.load(GuestAddress{p+16+unit},32);if(size && !data)return finish(kmt_invalid);
            return finish(kmt_not_supported);
        }
    }catch(const std::invalid_argument&){return finish(kmt_invalid);}
    return std::nullopt;
}
}
