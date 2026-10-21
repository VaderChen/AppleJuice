#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
// DXVA2：IDirect3DDeviceManager9 的裝置 handle、鎖定與重設 token；視訊服務綁定 D3D9 裝置。
// D3D9 路徑沒有硬體解碼／視訊處理 profile，列舉回報 0 個，客體改用 D3D11 視訊或軟體解碼；
// CreateSurface 經同一個 IDirect3DDevice9 建立實際的 offscreen surface。
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,no_memory=0x8007000eU,bad_handle=0x80070006U;
constexpr std::uint32_t not_initialized=0x80041000U,new_device=0x80041001U,device_locked=0x80041002U;
using Guid=std::array<std::uint8_t,16>;
constexpr Guid unknown_iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
Guid guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){Guid r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::uint8_t>(a>>(i*8));r[4]=static_cast<std::uint8_t>(b);r[5]=static_cast<std::uint8_t>(b>>8);r[6]=static_cast<std::uint8_t>(c);r[7]=static_cast<std::uint8_t>(c>>8);std::copy(d.begin(),d.end(),r.begin()+8);return r;}
const Guid device_manager_iid=guid(0xa0cade0f,0x06d5,0x4cf4,{0xa1,0xc7,0xf3,0xcd,0xd1,0xb5,0xd7,0x9a});
const Guid acceleration_iid=guid(0xfc51a550,0xd5e7,0x11d9,{0xaf,0x55,0x00,0x05,0x4e,0x43,0xff,0x02});
const Guid decoder_iid=guid(0xfc51a551,0xd5e7,0x11d9,{0xaf,0x55,0x00,0x05,0x4e,0x43,0xff,0x02});
const Guid processor_iid=guid(0xfc51a552,0xd5e7,0x11d9,{0xaf,0x55,0x00,0x05,0x4e,0x43,0xff,0x02});
}
std::span<const WindowsServiceDescriptor> dxva2_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"dxva2.dll","DXVA2CreateDirect3DDeviceManager9",2},{"dxva2.dll","DXVA2CreateVideoService",3},
        {"internal-dxva2-manager","QueryInterface",3},{"internal-dxva2-manager","AddRef",1},{"internal-dxva2-manager","Release",1},
        {"internal-dxva2-manager","ResetDevice",3},{"internal-dxva2-manager","OpenDeviceHandle",2},{"internal-dxva2-manager","CloseDeviceHandle",2},
        {"internal-dxva2-manager","TestDevice",2},{"internal-dxva2-manager","LockDevice",4},{"internal-dxva2-manager","UnlockDevice",3},
        {"internal-dxva2-manager","GetVideoService",4},
        {"internal-dxva2-decoder","QueryInterface",3},{"internal-dxva2-decoder","AddRef",1},{"internal-dxva2-decoder","Release",1},
        {"internal-dxva2-decoder","CreateSurface",10},{"internal-dxva2-decoder","GetDecoderDeviceGuids",3},{"internal-dxva2-decoder","GetDecoderRenderTargets",4},
        {"internal-dxva2-decoder","GetDecoderConfigurations",6},{"internal-dxva2-decoder","CreateVideoDecoder",7},
        {"internal-dxva2-processor","QueryInterface",3},{"internal-dxva2-processor","AddRef",1},{"internal-dxva2-processor","Release",1},
        {"internal-dxva2-processor","CreateSurface",10},{"internal-dxva2-processor","RegisterVideoProcessorSoftwareDevice",2},
        {"internal-dxva2-processor","GetVideoProcessorDeviceGuids",4},{"internal-dxva2-processor","GetVideoProcessorRenderTargets",5},
        {"internal-dxva2-processor","GetVideoProcessorSubStreamFormats",6},{"internal-dxva2-processor","GetVideoProcessorCaps",5},
        {"internal-dxva2-processor","GetProcAmpRange",5},{"internal-dxva2-processor","GetFilterPropertyRange",5},{"internal-dxva2-processor","CreateVideoProcessor",6}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dxva2(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto method=[&](std::uint64_t object,unsigned slot){const auto table=memory.load(GuestAddress{object},bits);return memory.load(GuestAddress{table+std::uint64_t{slot}*unit},bits);};
    const auto read_guid=[&](std::uint64_t at){Guid value{};memory.read(GuestAddress{at},value);return value;};
    const auto table_for=[&](std::string_view family,std::uint64_t& slot){
        if(!slot){unsigned count=0;for(const auto& row:dxva2_service_descriptors())if(row.family==family)++count;
            const auto table=runtime_allocate(memory,{},count*unit);unsigned i=0;
            for(const auto& row:dxva2_service_descriptors())if(row.family==family)memory.store(GuestAddress{table+unit*i++},bits,resolve(row.family,row.symbol).value);slot=table;}
        return slot;
    };
    // 建立視訊服務物件，並以 AddRef 持有 D3D9 裝置。
    const auto create_service=[&](std::uint64_t device,const Guid& iid,std::uint64_t output)->std::optional<std::uint32_t>{
        const bool processor=iid==processor_iid;if(iid!=decoder_iid && iid!=acceleration_iid && !processor)return finish(no_interface);
        if(dxva2_services_.size()>=4096)return finish(no_memory);
        const auto object=allocate_heap(memory,unit);if(!object)return finish(no_memory);
        memory.store(GuestAddress{object},bits,processor?table_for("internal-dxva2-processor",dxva2_processor_table_):table_for("internal-dxva2-decoder",dxva2_decoder_table_));
        dxva2_services_.emplace(object,Dxva2Service{device,1,processor});memory.store(GuestAddress{output},bits,object);
        start_callbacks(state,memory,{{method(device,1),{device}}},Continuation::Return,0);return std::nullopt;
    };
    try{
        if(name=="DXVA2CreateDirect3DDeviceManager9"){
            if(!arg(0) || !arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(0)},4,Access::Write);memory.check(GuestAddress{arg(1)},unit,Access::Write);
            if(dxva2_managers_.size()>=4096)return finish(no_memory);
            const auto object=allocate_heap(memory,unit);if(!object)return finish(no_memory);
            memory.store(GuestAddress{object},bits,table_for("internal-dxva2-manager",dxva2_manager_table_));
            Dxva2Manager manager;manager.token=static_cast<std::uint32_t>(runtime_.allocate_wait_handle());dxva2_managers_.emplace(object,manager);
            memory.store(GuestAddress{arg(0)},32,manager.token);memory.store(GuestAddress{arg(1)},bits,object);return finish(0);
        }
        if(name=="DXVA2CreateVideoService"){
            if(!arg(0) || !arg(1) || !arg(2))return finish(pointer_error);memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.store(GuestAddress{arg(2)},bits,0);
            return create_service(arg(0),read_guid(arg(1)),arg(2));
        }
        const bool manager_family=service.family=="internal-dxva2-manager";
        if(manager_family){
            const auto found=dxva2_managers_.find(arg(0));if(found==dxva2_managers_.end())return finish(invalid);auto& manager=found->second;
            if(name=="QueryInterface"){
                if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(pointer_error);
                const auto iid=read_guid(arg(1));if(iid!=unknown_iid && iid!=device_manager_iid)return finish(no_interface);
                ++manager.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
            }
            if(name=="AddRef")return finish(++manager.references);
            if(name=="Release"){
                const auto count=--manager.references;if(count)return finish(count);
                const auto device=manager.device;dxva2_managers_.erase(found);(void)free_heap(memory,arg(0),state);
                if(device){start_callbacks(state,memory,{{method(device,2),{device}}},Continuation::Return,0);return std::nullopt;}return finish(0);
            }
            if(name=="ResetDevice"){
                if(!arg(1))return finish(invalid);if(static_cast<std::uint32_t>(arg(2))!=manager.token)return finish(invalid);
                // 換裝置後既有 handle 須以 TestDevice 偵測到 DXVA2_E_NEW_VIDEO_DEVICE。
                const auto previous=manager.device;manager.device=arg(1);++manager.generation;manager.locked=0;
                std::vector<GuestCallback> callbacks{{method(arg(1),1),{arg(1)}}};if(previous)callbacks.push_back({method(previous,2),{previous}});
                start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
            }
            if(name=="OpenDeviceHandle"){
                if(!arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(1)},unit,Access::Write);
                if(!manager.device)return finish(not_initialized);if(manager.handles.size()>=1024)return finish(no_memory);
                const auto handle=runtime_.allocate_wait_handle();manager.handles.emplace(handle,manager.generation);memory.store(GuestAddress{arg(1)},bits,handle);return finish(0);
            }
            const auto handle=manager.handles.find(arg(1));if(handle==manager.handles.end())return finish(bad_handle);
            if(name=="CloseDeviceHandle"){if(manager.locked==arg(1))manager.locked=0;manager.handles.erase(handle);return finish(0);}
            if(handle->second!=manager.generation)return finish(new_device);
            if(name=="TestDevice")return finish(0);
            if(name=="LockDevice"){
                if(!arg(2))return finish(pointer_error);memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.store(GuestAddress{arg(2)},bits,0);
                if(manager.locked && manager.locked!=arg(1))return finish(device_locked); // 本程序為單一鎖持有者，不跨執行緒等待。
                manager.locked=arg(1);memory.store(GuestAddress{arg(2)},bits,manager.device);
                start_callbacks(state,memory,{{method(manager.device,1),{manager.device}}},Continuation::Return,0);return std::nullopt;
            }
            if(name=="UnlockDevice"){if(manager.locked!=arg(1))return finish(invalid);manager.locked=0;return finish(0);}
            // GetVideoService
            if(!arg(2) || !arg(3))return finish(pointer_error);memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.store(GuestAddress{arg(3)},bits,0);
            return create_service(manager.device,read_guid(arg(2)),arg(3));
        }
        const auto found=dxva2_services_.find(arg(0));if(found==dxva2_services_.end())return finish(invalid);auto& video=found->second;
        if(name=="QueryInterface"){
            if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(pointer_error);
            const auto iid=read_guid(arg(1));
            if(iid!=unknown_iid && iid!=acceleration_iid && iid!=(video.processor?processor_iid:decoder_iid))return finish(no_interface);
            ++video.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
        }
        if(name=="AddRef")return finish(++video.references);
        if(name=="Release"){
            const auto count=--video.references;if(count)return finish(count);
            const auto device=video.device;dxva2_services_.erase(found);(void)free_heap(memory,arg(0),state);
            start_callbacks(state,memory,{{method(device,2),{device}}},Continuation::Return,0);return std::nullopt;
        }
        if(name=="CreateSurface"){
            // CreateSurface(width,height,backBuffers,format,pool,usage,DXVA2 type,ppSurface,pSharedHandle)：逐一建立 1+backBuffers 個 surface。
            const auto width=static_cast<std::uint32_t>(arg(1)),height=static_cast<std::uint32_t>(arg(2)),back=static_cast<std::uint32_t>(arg(3));
            const auto type=static_cast<std::uint32_t>(arg(7));
            if(!width || !height || back>64 || !arg(8) || type>2 || arg(9))return finish(invalid);
            memory.check(GuestAddress{arg(8)},std::size_t{back+1}*unit,Access::Write);
            std::vector<GuestCallback> callbacks;
            for(std::uint32_t i=0;i<=back;++i){memory.store(GuestAddress{arg(8)+i*unit},bits,0);callbacks.push_back({method(video.device,36),{video.device,width,height,arg(4),arg(5),arg(8)+i*unit,0}});}
            start_callbacks(state,memory,std::move(callbacks),Continuation::ReturnUntilFailure,0);return std::nullopt;
        }
        const auto zero_guids=[&](std::uint64_t count,std::uint64_t list){
            if(!count || !list)return finish(pointer_error);memory.check(GuestAddress{count},4,Access::Write);memory.check(GuestAddress{list},unit,Access::Write);
            memory.store(GuestAddress{count},32,0);memory.store(GuestAddress{list},bits,0);return finish(0);
        };
        if(name=="GetDecoderDeviceGuids")return zero_guids(arg(1),arg(2));
        if(name=="GetVideoProcessorDeviceGuids"){if(!arg(1))return finish(pointer_error);return zero_guids(arg(2),arg(3));}
        if(name=="RegisterVideoProcessorSoftwareDevice")return finish(0x80004001U); // 沒有軟體視訊處理裝置外掛點。
        // 其餘入口都以裝置 GUID 指定 profile；沒有任何 profile，因此 GUID 一律不受支援。
        return finish(invalid);
    }catch(const MemoryFault&){return finish(pointer_error);}catch(const std::bad_alloc&){return finish(no_memory);}
}
}
