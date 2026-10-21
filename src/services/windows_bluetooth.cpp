#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/security_access.hpp"
#include "bluetooth_windows.hpp"

namespace x64arm {
namespace {
using namespace bluetooth;
using namespace bluetooth::windows;
struct ApiError {std::uint32_t code;};
void output(GuestMemory& memory,std::uint64_t pointer,std::size_t size){if(!pointer)throw ApiError{87};memory.check(GuestAddress{pointer},size,Access::Write);}
void sized_output(GuestMemory& memory,std::uint64_t pointer,unsigned size){output(memory,pointer,size);if(memory.load(GuestAddress{pointer},32)!=size)throw ApiError{1306};}
unsigned gatt_access(std::uint32_t access){return (access&1U?1U:0U)|(access&2U?2U:0U);}
std::uint32_t mapped_access(std::uint32_t access){return security::mapped_access(access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});}
template<class T,class Pack> Reply array_reply(GuestMemory& memory,const Result<std::vector<T>>& result,std::uint16_t capacity,std::uint64_t pointer,std::uint64_t actual,Pack pack){
    if(result.error)return {result.error,{}};if(result.value.size()>UINT16_MAX)return {hresult(1450),{}};
    output(memory,actual,2);Bytes bytes;
    if(pointer && capacity>=result.value.size())for(const auto& item:result.value){const auto record=pack(item);bytes.insert(bytes.end(),record.begin(),record.end());}
    if(pointer && capacity>=result.value.size())memory.check(GuestAddress{pointer},bytes.size(),Access::Write);
    memory.store(GuestAddress{actual},16,result.value.size());
    if(!pointer)return {hresult(234),{}};if(capacity<result.value.size())return {hresult(1784),{}};
    memory.write(GuestAddress{pointer},bytes);return {};
}
void array_output(GuestMemory& memory,std::uint16_t count,std::uint64_t pointer,std::uint64_t actual,unsigned stride){
    output(memory,actual,2);if((!pointer && count) || (pointer && !count))throw ApiError{87};
    if(pointer)memory.check(GuestAddress{pointer},std::size_t{count}*stride,Access::Write);
}
template<class Pack> Reply value_reply(GuestMemory& memory,const Result<Bytes>& result,std::uint32_t capacity,std::uint64_t pointer,std::uint64_t actual,Pack pack){
    if(result.error)return {result.error,{}};const auto bytes=pack(result.value);if(actual)output(memory,actual,2);if(pointer && capacity>=bytes.size())output(memory,pointer,bytes.size());
    if(actual)memory.store(GuestAddress{actual},16,bytes.size());if(!pointer)return {hresult(234),{}};if(capacity<bytes.size())return {hresult(1784),{}};memory.write(GuestAddress{pointer},bytes);return {};
}
}
void WindowsServices::set_bluetooth_backend(std::shared_ptr<bluetooth::Backend> backend){
    if(bluetooth_ && (bluetooth_->family.has_resources() || !bluetooth_->operations.empty()))throw HostCallFault("Bluetooth 後端仍有使用中的 handle、註冊或請求");
    bluetooth_=backend?std::make_shared<BluetoothState>(std::move(backend),[this]{return runtime_.allocate_wait_handle();}):nullptr;
}
bool WindowsServices::prepare_bluetooth(){
    if(bluetooth_)return true;if(!runtime_.network.external_io)return false;
    auto backend=bluetooth::make_bluetooth_backend();if(!backend)return false;set_bluetooth_backend(std::move(backend));return true;
}
bool WindowsServices::bluetooth_path(std::u16string_view path)const{return bluetooth::windows::fold(std::u16string(path)).starts_with(bluetooth::windows::path_prefix);}
bool WindowsServices::bluetooth_handle(std::uint64_t handle)const{return bluetooth_ && bluetooth_->handles.contains(handle);}
bool WindowsServices::close_bluetooth(std::uint64_t handle){
    if(!bluetooth_handle(handle))return false;const auto result=bluetooth_->family.close(handle);if(result.error)return false;
    bluetooth_->handles.erase(handle);runtime_.handle_flags.erase(handle);return true;
}
bool WindowsServices::bluetooth_class(const std::array<std::uint8_t,16>& guid)const{
    using namespace bluetooth::windows;if(guid==device_class || guid==device_interface || guid==service_interface || guid==radio_interface)return true;
    if(bluetooth_)for(const auto& item:bluetooth_->interfaces())if(item.guid==guid)return true;return false;
}
bool WindowsServices::bluetooth_ready(std::uint64_t token)const{
    if(!bluetooth_)return true;for(const auto& [thread,pending]:bluetooth_->operations){(void)thread;if(pending.token==token)return pending.done();}return true;
}
bool WindowsServices::bluetooth_waiting()const{return bluetooth_ && !bluetooth_->operations.empty();}
bool WindowsServices::wait_bluetooth_operation(){
    auto& bt=*bluetooth_;bt.family.pump();const auto found=bt.operations.find(threads_.current);if(found==bt.operations.end() || found->second.done())return false;
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::NativeDevice;thread.wait.handles={found->second.token};
    threads_.yield=true;notify_concurrency_changed();return true;
}
std::optional<std::uint32_t> WindowsServices::finish_bluetooth_operation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(wait_bluetooth_operation())return std::nullopt;auto& bt=*bluetooth_;auto found=bt.operations.find(threads_.current);
    if(found==bt.operations.end() || found->second.symbol!=service.symbol)throw HostCallFault("Bluetooth 請求與客體呼叫框架不一致");
    auto pending=std::move(found->second);bt.operations.erase(found);threads_.items.at(threads_.current).wait={};
    Reply reply;try{reply=pending.finish(memory);}catch(const ApiError& error){reply={service.symbol.starts_with("BluetoothGATT")?hresult(error.code):0U,service.symbol.starts_with("BluetoothGATT")?std::nullopt:std::optional{error.code}};}
    catch(const MemoryFault&){reply={service.symbol.starts_with("BluetoothGATT")?hresult(998):0U,service.symbol.starts_with("BluetoothGATT")?std::nullopt:std::optional<std::uint32_t>{998}};}
    catch(const std::bad_alloc&){reply={service.symbol.starts_with("BluetoothGATT")?hresult(1450):0U,service.symbol.starts_with("BluetoothGATT")?std::nullopt:std::optional<std::uint32_t>{1450}};}
    if(reply.last_error)last_error_=*reply.last_error;WindowsCall(state,memory,service).return_to(state,reply.value);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_bluetooth(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const bool gatt=name.starts_with("BluetoothGATT"),direct=name=="BluetoothGetRadioInfo";
    const auto finish=[&](std::uint64_t value,std::optional<std::uint32_t> error={}){if(error)last_error_=*error;call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto failed=[&](std::uint32_t error){return finish(gatt?hresult(error):direct?error:0U,gatt || direct?std::nullopt:std::optional{error});};
    try{
        if(!prepare_bluetooth())return failed(50);auto& bt=*bluetooth_;bt.family.pump();
        if(bt.operations.contains(threads_.current))return finish_bluetooth_operation(state,memory,service);
        const auto submit=[&]<class T,class Finish>(Pending<T> request,Finish complete,std::function<void()> discard={}){
            try{bt.operations.emplace(threads_.current,operation(runtime_.allocate_wait_handle(),name,request,std::move(complete),discard));}catch(...){request->cancel();if(discard)discard();throw;}
            return finish_bluetooth_operation(state,memory,service);
        };
        const auto install_radio=[&](const Found<Radio>& found){
            Interface entry;entry.kind=Kind::Radio;entry.device=found.value.id;entry.name=found.value.name;entry.guid=radio_interface;entry.path=std::u16string(path_prefix)+u"radio#"+encoded_id(found.value.id);
            try{bt.handles.emplace(found.object,Endpoint{std::move(entry),0x12019fU,3});}
            catch(...){bt.family.close(found.object);throw;}
        };
        if(name=="BluetoothFindFirstRadio"){
            if(!call(0))return failed(87);if(memory.load(GuestAddress{call(0)},32)!=4)return failed(1306);output(memory,call(1),x86_?4U:8U);
            const auto result=bt.family.first_radio();if(result.error)return failed(win_error(result.error));
            try{install_radio(result.value);memory.store(GuestAddress{call(1)},x86_?32:64,result.value.object);}catch(...){bt.family.close(result.value.object);bt.handles.erase(result.value.object);bt.family.close_search(result.value.search,true);throw;}
            return finish(result.value.search,0);
        }
        if(name=="BluetoothFindNextRadio"){
            output(memory,call(1),x86_?4U:8U);const auto result=bt.family.next_radio(call(0));if(result.error)return failed(win_error(result.error));
            try{install_radio(result.value);memory.store(GuestAddress{call(1)},x86_?32:64,result.value.object);}catch(...){bt.family.close(result.value.object);bt.handles.erase(result.value.object);throw;}
            return finish(1);
        }
        if(name=="BluetoothFindRadioClose" || name=="BluetoothFindDeviceClose"){
            const auto result=bt.family.close_search(call(0),name=="BluetoothFindRadioClose");return result.error?failed(win_error(result.error)):finish(1);
        }
        if(name=="BluetoothGetRadioInfo"){
            sized_output(memory,call(1),520);const auto result=bt.family.radio_info(call(0));if(result.error)return finish(win_error(result.error));memory.write(GuestAddress{call(1)},wire::radio(result.value));return finish(0);
        }
        if(name=="BluetoothIsConnectable" || name=="BluetoothIsDiscoverable"){
            const auto result=bt.family.radio_state(call(0),name=="BluetoothIsDiscoverable");return result.error?failed(win_error(result.error)):finish(result.value?1U:0U);
        }
        if(name=="BluetoothEnableDiscovery" || name=="BluetoothEnableIncomingConnections")return submit(bt.family.set_radio(call(0),name=="BluetoothEnableDiscovery",static_cast<std::uint32_t>(call(1))!=0),[](GuestMemory&,const auto& result){return boolean(result);});
        if(name=="BluetoothFindFirstDevice"){
            if(!call(0))return failed(87);const unsigned size=x86_?32U:40U;const auto bytes=read(memory,call(0),size);if(wire::get(bytes,0,4)!=size)return failed(1306);sized_output(memory,call(1),560);
            Search search;search.authenticated=wire::get(bytes,4,4)!=0;search.remembered=wire::get(bytes,8,4)!=0;search.unknown=wire::get(bytes,12,4)!=0;search.connected=wire::get(bytes,16,4)!=0;search.inquiry=wire::get(bytes,20,4)!=0;search.timeout_multiplier=bytes[24];
            const auto pointer=call(1),radio=wire::get(bytes,x86_?28U:32U,x86_?4U:8U);
            auto request=bt.family.first_device(search,radio);return submit(request,[&bt,pointer](GuestMemory& target,const Result<Found<Device>>& result)->Reply{
                if(result.error)return {0,win_error(result.error)};try{sized_output(target,pointer,560);target.write(GuestAddress{pointer},wire::device(result.value.value));}catch(...){bt.family.close_search(result.value.search,false);throw;}return {result.value.search,{}};
            },[&bt,request]{const auto ready=request->poll();if(ready && !ready->error)bt.family.close_search(ready->value.search,false);});
        }
        if(name=="BluetoothFindNextDevice"){
            sized_output(memory,call(1),560);const auto result=bt.family.next_device(call(0));if(result.error)return failed(win_error(result.error));memory.write(GuestAddress{call(1)},wire::device(result.value.value));return finish(1);
        }
        if(name=="BluetoothGATTGetServices"){
            const auto count=static_cast<std::uint16_t>(call(1));const auto pointer=call(2),actual=call(3);array_output(memory,count,pointer,actual,24);
            return submit(bt.family.services(call(0),static_cast<std::uint32_t>(call(4))),[count,pointer,actual](GuestMemory& target,const auto& result){return array_reply(target,result,count,pointer,actual,[](const Service& value){return wire::service(value);});});
        }
        if(name=="BluetoothGATTGetCharacteristics" || name=="BluetoothGATTGetDescriptors" || name=="BluetoothGATTGetIncludedServices"){
            const auto count=static_cast<std::uint16_t>(call(2));const auto pointer=call(3),actual=call(4);array_output(memory,count,pointer,actual,name=="BluetoothGATTGetCharacteristics"?36U:name=="BluetoothGATTGetDescriptors"?32U:24U);const auto flags=static_cast<std::uint32_t>(call(5));
            if(name=="BluetoothGATTGetCharacteristics" || name=="BluetoothGATTGetIncludedServices"){
                std::optional<Service> parent;if(call(1))parent=wire::service(read(memory,call(1),24));
                if(name=="BluetoothGATTGetIncludedServices")return submit(bt.family.included_services(call(0),parent,flags),[count,pointer,actual](GuestMemory& target,const auto& result){return array_reply(target,result,count,pointer,actual,[](const Service& value){return wire::service(value);});});
                return submit(bt.family.characteristics(call(0),parent,flags),[count,pointer,actual](GuestMemory& target,const auto& result){return array_reply(target,result,count,pointer,actual,[](const Characteristic& value){return wire::characteristic(value);});});
            }
            if(!call(1))return failed(87);const auto parent=wire::characteristic(read(memory,call(1),36));
            return submit(bt.family.descriptors(call(0),parent,flags),[count,pointer,actual](GuestMemory& target,const auto& result){return array_reply(target,result,count,pointer,actual,[](const Descriptor& value){return wire::descriptor(value);});});
        }
        if(name=="BluetoothGATTGetCharacteristicValue" || name=="BluetoothGATTGetDescriptorValue"){
            if(!call(1) || (!call(3) && !call(4)))return failed(87);const auto capacity=static_cast<std::uint32_t>(call(2));const auto pointer=call(3),actual=call(4);const auto flags=static_cast<std::uint32_t>(call(5));
            if(!pointer && capacity)return failed(87);if(pointer)output(memory,pointer,capacity);if(actual)output(memory,actual,2);
            if(name=="BluetoothGATTGetDescriptorValue"){
                const auto value=wire::descriptor(read(memory,call(1),32));return submit(bt.family.read_descriptor(call(0),value,flags),[capacity,pointer,actual,value](GuestMemory& target,const Result<Bytes>& result){return value_reply(target,result,capacity,pointer,actual,[value](const Bytes& bytes){return wire::descriptor_value(value,bytes);});});
            }
            const auto value=wire::characteristic(read(memory,call(1),36));return submit(bt.family.read(call(0),value,flags),[capacity,pointer,actual](GuestMemory& target,const Result<Bytes>& result){
                return value_reply(target,result,capacity,pointer,actual,[](const Bytes& bytes){return characteristic_value(bytes);});
            });
        }
        if(name=="BluetoothGATTSetCharacteristicValue"){
            if(!call(1) || !call(2))return failed(87);const auto value=wire::characteristic(read(memory,call(1),36));const auto header=read(memory,call(2),4);const auto size=wire::get(header,0,4);if(size>512)return finish(att_error(13));
            auto bytes=wire::characteristic_data(read(memory,call(2),4+static_cast<std::size_t>(size)));
            return submit(bt.family.write(call(0),value,std::move(bytes),static_cast<std::uint32_t>(call(4)),call(3)),[](GuestMemory&,const auto& result)->Reply{return {result.error,{}};});
        }
        if(name=="BluetoothGATTSetDescriptorValue"){
            if(!call(1) || !call(2))return failed(87);const auto value=wire::descriptor(read(memory,call(1),32));auto bytes=read(memory,call(2),76);const auto size=wire::get(bytes,72,4);if(size>512)return finish(att_error(13));
            if(size)bytes=read(memory,call(2),76+static_cast<std::size_t>(size));auto data=wire::descriptor_data(bytes,value);
            return submit(bt.family.write_descriptor(call(0),value,std::move(data),static_cast<std::uint32_t>(call(3))),[](GuestMemory&,const auto& result)->Reply{return {result.error,{}};});
        }
        if(name=="BluetoothGATTRegisterEvent"){
            if(static_cast<std::uint32_t>(call(1)) || !call(2) || !call(3))return failed(87);output(memory,call(5),x86_?4U:8U);memory.check(GuestAddress{call(3)},1,Access::Execute);
            const auto count=wire::get(read(memory,call(2),4),0,2);if(!count)return failed(87);const auto bytes=read(memory,call(2),4+static_cast<std::size_t>(count)*36);std::vector<Characteristic> attributes;
            for(std::size_t i=0;i<count;++i)attributes.push_back(wire::characteristic(std::span(bytes).subspan(4+36*i,36)));
            const auto result=bt.family.register_event(call(0),std::move(attributes),call(3),call(4),static_cast<std::uint32_t>(call(6)));if(result.error)return finish(result.error);
            memory.store(GuestAddress{call(5)},x86_?32:64,result.value);return finish(0);
        }
        if(name=="BluetoothGATTUnregisterEvent"){
            std::uint64_t token=0;if(const auto worker=wait_workers_.find(threads_.current);worker!=wait_workers_.end())if(const auto work=registered_waits_.find(worker->second);work!=registered_waits_.end())token=work->second.bluetooth_delivery;
            return submit(bt.family.unregister_event(call(0),static_cast<std::uint32_t>(call(1)),token),[](GuestMemory&,const auto& result)->Reply{return {result.error,{}};});
        }
        throw HostCallFault("Bluetooth 家族沒有對應的呼叫契約");
    }catch(const ApiError& error){return failed(error.code);}catch(const wire::Invalid&){return failed(87);}catch(const MemoryFault&){return failed(998);}catch(const std::bad_alloc&){return failed(1450);}
}
std::optional<std::uint32_t> WindowsServices::enumerate_bluetooth_devices(GuestMemory& memory,bool low_energy){
    if(!prepare_bluetooth())return 50;auto& bt=*bluetooth_;bt.family.pump();
    if(const auto found=bt.operations.find(threads_.current);found!=bt.operations.end()){
        if(found->second.symbol!="Bluetooth device enumeration")throw HostCallFault("Bluetooth 裝置列舉與既有請求衝突");
        if(wait_bluetooth_operation())return {};auto pending=std::move(found->second);bt.operations.erase(found);threads_.items.at(threads_.current).wait={};
        const auto result=pending.finish(memory);if(result.value)return static_cast<std::uint32_t>(result.value);
        bt.enumerated=true;bt.enumeration_deadline=runtime_.clock_ticks+50000000;return 0;
    }
    const auto snapshot=bt.backend->radios();if(!snapshot.error)bt.radios=snapshot.value;else bt.radios.clear();
    if(!low_energy)return bluetooth::windows::win_error(snapshot.error);
    if(bt.enumerated && runtime_.clock_ticks<bt.enumeration_deadline)return 0;
    auto request=bt.family.discover(3000);
    try{bt.operations.emplace(threads_.current,bluetooth::windows::operation(runtime_.allocate_wait_handle(),"Bluetooth device enumeration",request,[](GuestMemory&,const auto& result)->bluetooth::windows::Reply{return {bluetooth::windows::win_error(result.error),{}};}));}
    catch(...){request->cancel();throw;}
    return enumerate_bluetooth_devices(memory,low_energy);
}
std::optional<std::uint32_t> WindowsServices::open_bluetooth(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,const std::u16string& path,const FileOpenArguments& arguments){
    const WindowsCall call(state,memory,service);const auto fail=[&](std::uint32_t error){last_error_=error;call.return_to(state,x86_?UINT32_MAX:UINT64_MAX);return std::optional<std::uint32_t>{};};
    try{
        if(!prepare_bluetooth())return fail(50);auto& bt=*bluetooth_;bt.family.pump();
        const auto access=mapped_access(static_cast<std::uint32_t>(arguments[1])),share=static_cast<std::uint32_t>(arguments[2]),flags=static_cast<std::uint32_t>(arguments[5]);
        if(access&~0x1f01ffU)return fail(5);if(share&~7U || static_cast<std::uint32_t>(arguments[4])!=3U)return fail(87);
        if(flags&~0x401f0080U)return fail(50);const auto security=read_security_attributes(memory,arguments[3],x86_,false);
        const auto interfaces=bt.interfaces();const auto key=fold(path);const auto found=std::find_if(interfaces.begin(),interfaces.end(),[&](const auto& item){return fold(item.path)==key;});
        if(found==interfaces.end())return fail(2);
        const auto categories=[](std::uint32_t desired){return (desired&(1U|0x20U)?1U:0U)|(desired&(2U|4U)?2U:0U)|(desired&0x10000U?4U:0U);};
        for(const auto& [handle,item]:bt.handles){(void)handle;if(fold(item.interface.path)==key && ((categories(access)&~item.share) || (categories(item.access)&~share)))return fail(32);}
        Result<Handle> opened;
        if(found->kind==Kind::Radio)opened=bt.family.open_radio(found->device);
        else if(found->kind==Kind::Device)opened=bt.family.open_device(found->device,gatt_access(access));
        else{
            const auto parent=bt.family.open_device(found->device,3);if(parent.error)return fail(win_error(parent.error));
            try{opened=bt.family.open_service(parent.value,*found->service,gatt_access(access));}catch(...){bt.family.close(parent.value);throw;}bt.family.close(parent.value);
        }
        if(opened.error)return fail(win_error(opened.error));
        try{bt.handles.emplace(opened.value,Endpoint{*found,access,share});if(security.inherit)runtime_.handle_flags[opened.value]=1;}
        catch(...){bt.family.close(opened.value);bt.handles.erase(opened.value);runtime_.handle_flags.erase(opened.value);throw;}
        call.return_to(state,opened.value);return {};
    }catch(const MemoryFault&){return fail(998);}catch(const SecurityDescriptorError& error){return fail(error.code);}catch(const std::bad_alloc&){return fail(1450);}
}
std::optional<std::uint32_t> WindowsServices::duplicate_bluetooth(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto source=call(1);const auto options=static_cast<std::uint32_t>(call(6));
    const bool source_process=current_process_handle(call(0),0x40),close_source=source_process && (options&1U) && !(runtime_.handle_flags[source]&2U);
    const auto close=[&]{if(close_source)(void)close_bluetooth(source);};struct Cleanup{const decltype(close)& action;~Cleanup(){action();}} cleanup{close};
    const auto fail=[&](std::uint32_t error){last_error_=error;call.return_to(state,0);return std::optional<std::uint32_t>{};};
    try{
        if(!source_process)return fail(5);if((options&1U) && !close_source)return fail(5);if(options&~3U)return fail(87);
        if(!call(2)){if(!close_source)return fail(87);call.return_to(state,1);return {};}
        if(!current_process_handle(call(2)))return fail(50);if(!current_process_handle(call(2),0x40))return fail(5);output(memory,call(3),x86_?4U:8U);
        auto item=bluetooth_->handles.at(source);const auto desired=options&2U?item.access:mapped_access(static_cast<std::uint32_t>(call(4)));if(desired&~item.access)return fail(5);
        const auto duplicated=bluetooth_->family.duplicate(source,item.interface.kind==Kind::Radio?std::nullopt:std::optional{gatt_access(desired)});if(duplicated.error)return fail(win_error(duplicated.error));item.access=desired;
        try{bluetooth_->handles.emplace(duplicated.value,std::move(item));runtime_.handle_flags[duplicated.value]=static_cast<std::uint32_t>(call(5))?1U:0U;memory.store(GuestAddress{call(3)},x86_?32:64,duplicated.value);}
        catch(...){bluetooth_->family.close(duplicated.value);bluetooth_->handles.erase(duplicated.value);runtime_.handle_flags.erase(duplicated.value);throw;}call.return_to(state,1);return {};
    }catch(const ApiError& error){return fail(error.code);}catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(1450);}
}
void WindowsServices::finish_bluetooth_delivery(std::uint64_t token){if(bluetooth_)bluetooth_->family.finish_delivery(token);}
void WindowsServices::cancel_bluetooth_thread(GuestState& state,GuestMemory& memory,std::uint64_t thread){
    if(!bluetooth_)return;auto& bt=*bluetooth_;if(const auto found=bt.operations.find(thread);found!=bt.operations.end()){found->second.cancel();bt.operations.erase(found);}
    if(const auto worker=wait_workers_.find(thread);worker!=wait_workers_.end())if(const auto work=registered_waits_.find(worker->second);work!=registered_waits_.end() && work->second.bluetooth_delivery){
        for(const auto pointer:work->second.callback_storage)(void)free_heap(memory,pointer,state);work->second.callback_storage.clear();release_registered_wait(work->first);
    }
}
void WindowsServices::poll_bluetooth(GuestMemory& memory){
    if(!bluetooth_)return;auto& bt=*bluetooth_;bt.family.pump();if(!bt.family.delivery_pending() || registered_waits_.size()>=64 || threads_.items.empty())return;
    const auto saved_error=last_error_;struct Restore{std::uint32_t& target;std::uint32_t value;~Restore(){target=value;}} restore{last_error_,saved_error};
    auto& state=threads_.items.at(threads_.current).context;const auto worker=acquire_wait_worker(state,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));if(!worker)return;
    const auto value=allocate_heap(memory,516);if(!value)return;const auto event=allocate_heap(memory,x86_?12U:24U);if(!event){(void)free_heap(memory,value,state);return;}
    std::uint64_t token=0,work_handle=0;
    try{
        const auto delivery=bt.family.take_delivery();if(!delivery){(void)free_heap(memory,event,state);(void)free_heap(memory,value,state);return;}token=delivery->token;
        const auto data=characteristic_value(delivery->value);memory.write(GuestAddress{value},data);
        memory.write(GuestAddress{event},wire::value_event(delivery->characteristic.value_handle,data.size(),value,x86_));
        RegisteredWait work;work.worker=worker;work.bluetooth_delivery=token;work.armed=false;work.ready=true;work.closing=true;
        work.callback_batch={{delivery->callback,{0,event,delivery->context}}};work.callback_storage={value,event};work_handle=next_registered_wait_++;
        registered_waits_.emplace(work_handle,std::move(work));wait_workers_[worker]=work_handle;auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};notify_concurrency_changed();
    }catch(...){
        if(work_handle)registered_waits_.erase(work_handle);wait_workers_[worker]=0;if(token)bt.family.finish_delivery(token);
        (void)free_heap(memory,event,state);(void)free_heap(memory,value,state);throw;
    }
}
}
