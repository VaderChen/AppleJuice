#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace x64arm {
// WinUSB：USB 裝置介面路徑以 CreateFile 開啟後，WinUsb_Initialize 經 IOUSBHost 開啟未被 macOS 接管的介面。
// 傳輸一律非同步送到宿主；同步呼叫等待內部事件，overlapped 呼叫照 Windows 回 ERROR_IO_PENDING，完成時更新 OVERLAPPED。
struct UsbFile {usb::Target target;};
struct WinUsbState {
    struct Pipe {std::uint32_t timeout{};bool short_terminate{},auto_clear_stall{},ignore_short{},allow_partial{true},auto_flush{},raw_io{},reset_on_resume{};};
    struct Device {std::shared_ptr<usb::Session> session;std::uint64_t file{};usb::Target target;std::map<std::uint8_t,Pipe> pipes;std::uint8_t auto_suspend{1};std::uint32_t suspend_delay{5000};};
    struct Handle {std::shared_ptr<Device> device;unsigned interface{};std::set<unsigned> associated;};
    struct Pending {std::shared_ptr<usb::Transfer> transfer;std::shared_ptr<Device> device;std::uint64_t buffer{},transferred{},overlapped{},event{};bool in{};unsigned interface{};std::uint8_t pipe{};};
    std::map<std::uint64_t,Handle> handles;std::uint64_t next{0x7b000000};
    std::map<std::uint64_t,Pending> pending; // 同步：內部事件；overlapped：OVERLAPPED 位址
    std::map<std::uint64_t,std::uint64_t> waits; // WinUsb_GetOverlappedResult 等待中：OVERLAPPED → lpNumberOfBytesTransferred
};
namespace usb {
bool interface_path(std::u16string_view text){
    if(text.size()<8)return false;std::u16string head(text.substr(0,8));for(auto& c:head)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return head==u"\\\\?\\usb#";
}
}
namespace {
// 組態描述元內，指定介面／替代設定的介面描述元與端點描述元。
struct InterfaceSetting {std::vector<std::uint8_t> descriptor;std::vector<std::vector<std::uint8_t>> endpoints;};
std::optional<InterfaceSetting> find_setting(const std::vector<std::uint8_t>& config,unsigned number,unsigned alternate){
    std::optional<InterfaceSetting> found;bool inside=false;
    for(std::size_t at=0;at+2<=config.size();){
        const auto length=config[at],type=config[at+1];if(length<2 || at+length>config.size())break;
        if(type==4 && length>=9){inside=config[at+2]==number && config[at+3]==alternate;if(inside){found.emplace();found->descriptor.assign(config.begin()+static_cast<std::ptrdiff_t>(at),config.begin()+static_cast<std::ptrdiff_t>(at+9));}}
        else if(type==5 && length>=7 && inside && found)found->endpoints.emplace_back(config.begin()+static_cast<std::ptrdiff_t>(at),config.begin()+static_cast<std::ptrdiff_t>(at+7));
        at+=length;
    }
    return found;
}
std::uint32_t nt_status(std::uint32_t error){
    switch(error){case 0:return 0;case 121:return 0xc00000b5U;case 995:return 0xc0000120U;case 1167:return 0xc000009dU;case 87:return 0xc000000dU;default:return 0xc0000001U;}
}
std::uint32_t win32_status(std::uint32_t status){
    switch(status){case 0:return 0;case 0xc00000b5U:return 121;case 0xc0000120U:return 995;case 0xc000009dU:return 1167;case 0xc000000dU:return 87;default:return 31;}
}
}
std::span<const WindowsServiceDescriptor> winusb_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"winusb.dll","WinUsb_Initialize",2},{"winusb.dll","WinUsb_Free",1},{"winusb.dll","WinUsb_GetAssociatedInterface",3,0,0,2},
        {"winusb.dll","WinUsb_GetDescriptor",7,0,0,0x2e},{"winusb.dll","WinUsb_QueryInterfaceSettings",3,0,0,2},{"winusb.dll","WinUsb_QueryDeviceInformation",4,0,0,2},
        {"winusb.dll","WinUsb_SetCurrentAlternateSetting",2,0,0,2},{"winusb.dll","WinUsb_GetCurrentAlternateSetting",2},{"winusb.dll","WinUsb_QueryPipe",4,0,0,6},
        {"winusb.dll","WinUsb_SetPipePolicy",5,0,0,0xe},{"winusb.dll","WinUsb_GetPipePolicy",5,0,0,6},{"winusb.dll","WinUsb_ReadPipe",6,0,0,0xa},{"winusb.dll","WinUsb_WritePipe",6,0,0,0xa},
        {"winusb.dll","WinUsb_ControlTransfer",6,2,0,8},{"winusb.dll","WinUsb_ResetPipe",2,0,0,2},{"winusb.dll","WinUsb_AbortPipe",2,0,0,2},{"winusb.dll","WinUsb_FlushPipe",2,0,0,2},
        {"winusb.dll","WinUsb_GetOverlappedResult",4,0,0,8},{"winusb.dll","WinUsb_SetPowerPolicy",4,0,0,6},{"winusb.dll","WinUsb_GetPowerPolicy",4,0,0,2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::open_usb(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,const std::u16string& path,const FileOpenArguments* arguments){
    const WindowsCall frame(state,memory,service);const auto arg=[&](unsigned i){return arguments?arguments->at(i):frame(i);};const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    std::uint64_t result=x86_?UINT32_MAX:UINT64_MAX;const auto fail=[&](std::uint32_t error){last_error_=error;};
    try{
        const auto security=synchronization_security(memory,arg(3),x86_);
        if(d(4)!=3 || arg(6) || (d(1)&~0xc0000003U) || (d(2)&~3U) || (d(5)&~0x40000080U))fail(87);
        else if(security.error)fail(security.error);
        else if(!runtime_.network.external_io)fail(50);
        else if(files_.size()>=1024)fail(4);
        else if(const auto target=usb_target(path);!target)fail(2);
        // WinUSB 裝置一次只允許一個 handle。
        else if(std::any_of(files_.begin(),files_.end(),[&](const auto& item){return item.second.usb && item.second.usb->target.registry_id==target->registry_id && item.second.usb->target.interfaces==target->interfaces;}))fail(5);
        else{
            FileHandle file;file.usb=std::make_shared<UsbFile>();file.usb->target=*target;file.bytes=std::make_shared<const std::vector<std::uint8_t>>();
            file.key=utf16_to_utf8(path);file.path=file.key;file.access=d(1);file.sharing=d(2);file.mode=(d(5)&0x40000000U)?0U:0x20U;
            const auto handle=runtime_.allocate_wait_handle();file.object=handle;files_.emplace(handle,std::move(file));runtime_.handle_flags[handle]=security.inherit?1U:0U;result=handle;
        }
    }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    frame.return_to(state,result);return std::nullopt;
}
bool WindowsServices::winusb_waiting() const {return winusb_ && !winusb_->pending.empty();}
void WindowsServices::poll_winusb(GuestMemory& memory){
    if(!winusb_)return;const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    for(auto& [key,item]:winusb_->pending){
        if(!item.overlapped || !item.transfer || !item.transfer->done)continue;
        // overlapped 完成：資料寫回客體緩衝區，更新 Internal／InternalHigh 並觸發 hEvent；結果由 WinUsb_GetOverlappedResult 取回。
        try{
            auto& transfer=*item.transfer;
            if(item.in && transfer.transferred)memory.write(GuestAddress{item.buffer},std::span(transfer.data).first(std::min<std::size_t>(transfer.transferred,transfer.data.size())));
            if(transfer.error==31 && item.device){const auto pipe=item.device->pipes.find(item.pipe);if(pipe!=item.device->pipes.end() && pipe->second.auto_clear_stall)(void)item.device->session->reset(item.interface,item.pipe);}
            memory.store(GuestAddress{key},bits,nt_status(transfer.error));memory.store(GuestAddress{key+unit},bits,transfer.transferred);
            if(item.event)if(const auto event=runtime_.wait_objects.find(item.event);event!=runtime_.wait_objects.end())event->second.set_signal(true);
        }catch(const MemoryFault&){}
        item.transfer.reset();
    }
    std::erase_if(winusb_->pending,[](const auto& entry){return entry.second.overlapped && !entry.second.transfer;});
    // 同步傳輸：只觸發內部事件，資料於 continuation 內寫回。
    for(auto& [key,item]:winusb_->pending)if(!item.overlapped && item.transfer && item.transfer->done)
        if(const auto event=runtime_.wait_objects.find(key);event!=runtime_.wait_objects.end())event->second.set_signal(true);
}
void WindowsServices::discard_winusb(std::uint64_t event){
    if(!winusb_)return;if(const auto found=winusb_->pending.find(event);found!=winusb_->pending.end() && found->second.device && found->second.transfer && !found->second.transfer->done)
        (void)found->second.device->session->abort(found->second.interface,found->second.pipe);
    winusb_->pending.erase(event);runtime_.wait_objects.erase(event);
}
std::optional<std::uint32_t> WindowsServices::resume_winusb(GuestState& state,GuestMemory& memory,std::uint64_t key,bool overlapped){
    if(overlapped){
        // WinUsb_GetOverlappedResult 等待 hEvent 後重新檢查 OVERLAPPED。
        const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        if(winusb_ && winusb_->pending.contains(key))poll_winusb(memory);
        const auto status=static_cast<std::uint32_t>(memory.load(GuestAddress{key},bits));const auto bytes=memory.load(GuestAddress{key+unit},bits);
        std::uint64_t out=0;if(winusb_)if(const auto wait=winusb_->waits.find(key);wait!=winusb_->waits.end()){out=wait->second;winusb_->waits.erase(wait);}
        if(winusb_ && winusb_->pending.contains(key)){last_error_=996;{complete_call(state,memory,0);return std::nullopt;}}
        if(out)memory.store(GuestAddress{out},32,bytes);
        if(status){last_error_=win32_status(status);{complete_call(state,memory,0);return std::nullopt;}}{complete_call(state,memory,1);return std::nullopt;}
    }
    if(!winusb_){complete_call(state,memory,0);return std::nullopt;}
    const auto found=winusb_->pending.find(key);runtime_.wait_objects.erase(key);
    if(found==winusb_->pending.end()){last_error_=995;{complete_call(state,memory,0);return std::nullopt;}}
    auto item=std::move(found->second);winusb_->pending.erase(found);const auto& transfer=*item.transfer;
    try{
        if(item.in && transfer.transferred)memory.write(GuestAddress{item.buffer},std::span(transfer.data).first(std::min<std::size_t>(transfer.transferred,transfer.data.size())));
        if(item.transferred)memory.store(GuestAddress{item.transferred},32,transfer.transferred);
    }catch(const MemoryFault&){last_error_=998;{complete_call(state,memory,0);return std::nullopt;}}
    if(transfer.error==31 && item.device){const auto pipe=item.device->pipes.find(item.pipe);if(pipe!=item.device->pipes.end() && pipe->second.auto_clear_stall)(void)item.device->session->reset(item.interface,item.pipe);}
    if(transfer.error){last_error_=transfer.error;{complete_call(state,memory,0);return std::nullopt;}}
    {complete_call(state,memory,1);return std::nullopt;}
}
std::optional<std::uint32_t> WindowsServices::invoke_winusb(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto& name=service.symbol;const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto failed=[&](std::uint32_t error){last_error_=error;return finish(0);};
    if(!winusb_)winusb_=std::make_shared<WinUsbState>();auto& bridge=*winusb_;
    try{
        if(name=="WinUsb_Initialize"){
            // (DeviceHandle, PWINUSB_INTERFACE_HANDLE)
            if(!arg(1))return failed(87);memory.check(GuestAddress{arg(1)},unit,Access::Write);
            const auto file=files_.find(arg(0));if(file==files_.end())return failed(6);
            if(!file->second.usb || !file->second.usb->target.winusb)return failed(1); // 不是以 WinUSB 為功能驅動的裝置。
            if(std::any_of(bridge.handles.begin(),bridge.handles.end(),[&](const auto& item){return item.second.device->file==arg(0);}))return failed(170);
            if(!runtime_.network.external_io)return failed(50);
            std::uint32_t error=0;auto session=usb::open(file->second.usb->target.registry_id,file->second.usb->target.interfaces,error);if(!session)return failed(error?error:31);
            auto device=std::make_shared<WinUsbState::Device>();device->session=std::move(session);device->file=arg(0);device->target=file->second.usb->target;
            const auto handle=bridge.next++;bridge.handles.emplace(handle,WinUsbState::Handle{device,device->target.interfaces.front(),{}});
            memory.store(GuestAddress{arg(1)},bits,handle);return finish(1);
        }
        const auto found=bridge.handles.find(arg(0));if(found==bridge.handles.end())return failed(6);
        auto& handle=found->second;auto& device=*handle.device;auto& session=*device.session;
        const auto setting=[&]{const auto alternate=session.alternate(handle.interface).value_or(0);return find_setting(session.configuration_descriptor(),handle.interface,alternate);};
        const auto owns=[&](std::uint8_t pipe){const auto current=setting();if(!current)return false;return std::any_of(current->endpoints.begin(),current->endpoints.end(),[&](const auto& endpoint){return endpoint[2]==pipe;});};
        if(name=="WinUsb_Free"){
            for(auto& [key,item]:bridge.pending)if(item.device==handle.device && item.interface==handle.interface && item.transfer && !item.transfer->done)(void)session.abort(item.interface,item.pipe);
            bridge.handles.erase(found);return finish(1);
        }
        if(name=="WinUsb_GetAssociatedInterface"){
            const auto index=static_cast<unsigned>(arg(1));if(!arg(2))return failed(87);memory.check(GuestAddress{arg(2)},unit,Access::Write);
            if(handle.interface!=device.target.interfaces.front())return failed(87);
            if(index+1>=device.target.interfaces.size())return failed(259); // ERROR_NO_MORE_ITEMS
            if(handle.associated.contains(index))return failed(183);
            handle.associated.insert(index);const auto next=bridge.next++;bridge.handles.emplace(next,WinUsbState::Handle{handle.device,device.target.interfaces[index+1],{}});
            memory.store(GuestAddress{arg(2)},bits,next);return finish(1);
        }
        if(name=="WinUsb_QueryDeviceInformation"){
            // DEVICE_SPEED（1）：LowSpeed=1、FullSpeed=2、HighSpeed=3；SuperSpeed 以上同為 HighSpeed。
            if(arg(1)!=1)return failed(87);if(!arg(2) || !arg(3))return failed(87);
            if(memory.load(GuestAddress{arg(2)},32)<1){memory.store(GuestAddress{arg(2)},32,1);return failed(122);}
            const auto speed=device.target.speed==0?1U:device.target.speed==1?2U:3U;memory.store(GuestAddress{arg(3)},8,speed);memory.store(GuestAddress{arg(2)},32,1);return finish(1);
        }
        if(name=="WinUsb_QueryInterfaceSettings"){
            if(!arg(2))return failed(87);const auto found_setting=find_setting(session.configuration_descriptor(),handle.interface,static_cast<unsigned>(arg(1)));if(!found_setting)return failed(259);
            memory.write(GuestAddress{arg(2)},found_setting->descriptor);return finish(1);
        }
        if(name=="WinUsb_GetCurrentAlternateSetting"){
            if(!arg(1))return failed(87);const auto current=session.alternate(handle.interface);if(!current)return failed(31);memory.store(GuestAddress{arg(1)},8,*current);return finish(1);
        }
        if(name=="WinUsb_SetCurrentAlternateSetting"){
            if(!find_setting(session.configuration_descriptor(),handle.interface,static_cast<unsigned>(arg(1))))return failed(87);
            if(const auto error=session.select_alternate(handle.interface,static_cast<unsigned>(arg(1))))return failed(error);return finish(1);
        }
        if(name=="WinUsb_QueryPipe"){
            // WINUSB_PIPE_INFORMATION：PipeType、PipeId、MaximumPacketSize、Interval。
            if(!arg(3))return failed(87);const auto found_setting=find_setting(session.configuration_descriptor(),handle.interface,static_cast<unsigned>(arg(1)));
            if(!found_setting || arg(2)>=found_setting->endpoints.size())return failed(259);const auto& endpoint=found_setting->endpoints[static_cast<std::size_t>(arg(2))];
            std::vector<std::uint8_t> info(12);info[0]=endpoint[3]&3U;info[4]=endpoint[2];info[6]=endpoint[4];info[7]=static_cast<std::uint8_t>(endpoint[5]&0x07U);info[8]=endpoint[6];
            memory.write(GuestAddress{arg(3)},info);return finish(1);
        }
        if(name=="WinUsb_SetPipePolicy" || name=="WinUsb_GetPipePolicy"){
            const auto pipe=static_cast<std::uint8_t>(arg(1));const auto type=static_cast<std::uint32_t>(arg(2));if(!owns(pipe))return failed(87);auto& policy=device.pipes[pipe];
            const bool boolean=type==1 || type==2 || type==4 || type==5 || type==6 || type==7 || type==9;if(!boolean && type!=3 && type!=8)return failed(87);
            const unsigned size=boolean?1U:4U;
            bool* flags[]={nullptr,&policy.short_terminate,&policy.auto_clear_stall,nullptr,&policy.ignore_short,&policy.allow_partial,&policy.auto_flush,&policy.raw_io,nullptr,&policy.reset_on_resume};
            if(name=="WinUsb_SetPipePolicy"){
                if(type==8 || static_cast<std::uint32_t>(arg(3))!=size || !arg(4))return failed(87);
                if(boolean)*flags[type]=memory.load(GuestAddress{arg(4)},8)!=0;else policy.timeout=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32));return finish(1);
            }
            if(!arg(3) || !arg(4))return failed(87);if(memory.load(GuestAddress{arg(3)},32)<size){memory.store(GuestAddress{arg(3)},32,size);return failed(122);}
            if(boolean)memory.store(GuestAddress{arg(4)},8,*flags[type]?1U:0U);else memory.store(GuestAddress{arg(4)},32,type==3?policy.timeout:4U*1024U*1024U);
            memory.store(GuestAddress{arg(3)},32,size);return finish(1);
        }
        if(name=="WinUsb_SetPowerPolicy" || name=="WinUsb_GetPowerPolicy"){
            // AUTO_SUSPEND（0x81，UCHAR）、SUSPEND_DELAY（0x83，ULONG）；宿主的電源管理由 macOS 決定，此處保存客體設定。
            const auto type=static_cast<std::uint32_t>(arg(1));if(type!=0x81 && type!=0x83)return failed(87);const unsigned size=type==0x81?1U:4U;
            if(name=="WinUsb_SetPowerPolicy"){if(static_cast<std::uint32_t>(arg(2))!=size || !arg(3))return failed(87);if(type==0x81)device.auto_suspend=memory.load(GuestAddress{arg(3)},8)!=0;else device.suspend_delay=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));return finish(1);}
            if(!arg(2) || !arg(3))return failed(87);if(memory.load(GuestAddress{arg(2)},32)<size){memory.store(GuestAddress{arg(2)},32,size);return failed(122);}
            if(type==0x81)memory.store(GuestAddress{arg(3)},8,device.auto_suspend);else memory.store(GuestAddress{arg(3)},32,device.suspend_delay);memory.store(GuestAddress{arg(2)},32,size);return finish(1);
        }
        if(name=="WinUsb_ResetPipe" || name=="WinUsb_AbortPipe" || name=="WinUsb_FlushPipe"){
            const auto pipe=static_cast<std::uint8_t>(arg(1));if(!owns(pipe))return failed(87);
            if(name=="WinUsb_FlushPipe")return finish(1); // 沒有 RAW_IO 以外的預讀快取。
            if(const auto error=name=="WinUsb_ResetPipe"?session.reset(handle.interface,pipe):session.abort(handle.interface,pipe))return failed(error);return finish(1);
        }
        if(name=="WinUsb_GetOverlappedResult"){
            // (InterfaceHandle, LPOVERLAPPED, LPDWORD, BOOL bWait)
            if(!arg(1) || !arg(2))return failed(87);poll_winusb(memory);
            const auto overlapped=arg(1);
            if(bridge.pending.contains(overlapped)){
                if(!arg(3))return failed(996); // ERROR_IO_INCOMPLETE
                const auto event=memory.load(GuestAddress{overlapped+2*unit+8},bits);if(!event)return failed(87);
                bridge.waits.insert_or_assign(overlapped,arg(2));
                start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{event,UINT32_MAX,0}}},Continuation::WinUsbOverlapped,overlapped);return std::nullopt;
            }
            const auto status=static_cast<std::uint32_t>(memory.load(GuestAddress{overlapped},bits));memory.store(GuestAddress{arg(2)},32,memory.load(GuestAddress{overlapped+unit},bits));
            if(status==0x103U)return failed(996);if(status)return failed(win32_status(status));return finish(1);
        }
        // 傳輸：ReadPipe／WritePipe(h, PipeID, Buffer, Length, LengthTransferred, Overlapped)、ControlTransfer(h, SetupPacket, Buffer, Length, LengthTransferred, Overlapped)。
        const bool control=name=="WinUsb_ControlTransfer",read=name=="WinUsb_ReadPipe";
        if(!control && !read && name!="WinUsb_WritePipe")throw HostCallFault("尚未實作 WinUSB 入口："+std::string(name));
        const auto buffer=arg(2),count=arg(4),overlapped=arg(5);const auto length=static_cast<std::uint32_t>(arg(3));
        if(!overlapped && !count)return failed(87);if(length && !buffer)return failed(998);
        if(count)memory.check(GuestAddress{count},4,Access::Write);
        std::shared_ptr<usb::Transfer> transfer;std::uint8_t pipe=0;bool in=read;
        if(control){
            const auto setup=arg(1);const auto type=static_cast<std::uint8_t>(setup),request=static_cast<std::uint8_t>(setup>>8);
            const auto value=static_cast<std::uint16_t>(setup>>16),index=static_cast<std::uint16_t>(setup>>32),requested=static_cast<std::uint16_t>(setup>>48);
            if(requested>length)return failed(87);in=(type&0x80U)!=0;
            std::vector<std::uint8_t> data(requested);if(!in && requested)memory.read(GuestAddress{buffer},data);
            transfer=session.control(handle.interface,type,request,value,index,std::move(data),5000);
        }else{
            pipe=static_cast<std::uint8_t>(arg(1));if(!owns(pipe) || ((pipe&0x80U)!=0)!=read)return failed(87);
            if(length>16U*1024U*1024U)return failed(8);std::vector<std::uint8_t> data(length);if(!read && length)memory.read(GuestAddress{buffer},data);
            transfer=session.pipe(handle.interface,pipe,std::move(data),device.pipes[pipe].timeout);
        }
        if(transfer->done && transfer->error && !transfer->transferred && transfer->data.empty()){if(count)memory.store(GuestAddress{count},32,0);return failed(transfer->error);}
        WinUsbState::Pending item{transfer,handle.device,buffer,count,overlapped,0,in,handle.interface,pipe};
        if(overlapped){
            memory.check(GuestAddress{overlapped},2*unit+8+unit,Access::Write);memory.store(GuestAddress{overlapped},bits,0x103U);memory.store(GuestAddress{overlapped+unit},bits,0);
            item.event=memory.load(GuestAddress{overlapped+2*unit+8},bits);
            if(item.event)if(const auto event=runtime_.wait_objects.find(item.event);event!=runtime_.wait_objects.end())event->second.set_signal(false);
            if(count)memory.store(GuestAddress{count},32,0);bridge.pending.insert_or_assign(overlapped,std::move(item));poll_winusb(memory);return failed(997); // ERROR_IO_PENDING
        }
        const auto event=runtime_.allocate_wait_handle();WaitObject wait;wait.kind=WaitObject::Kind::Event;wait.manual_reset=true;wait.access=0x100000U;
        runtime_.wait_objects.emplace(event,std::move(wait));bridge.pending.emplace(event,std::move(item));
        try{start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{event,UINT32_MAX,0}}},Continuation::WinUsbTransfer,event);}catch(...){discard_winusb(event);throw;}
        poll_winusb(memory);return std::nullopt;
    }catch(const MemoryFault&){return failed(998);}catch(const std::bad_alloc&){return failed(8);}
}
}
