#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "mci_wave.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
namespace {
std::u16string fold(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::uint32_t file_error(std::uint32_t code,bool writing=false){return code==2 || code==3?mci::file_missing:code==8?mci::memory:writing?mci::file_write:mci::file_read;}
std::u16string_view error_text(std::uint32_t code){
    switch(code){
    case 0:return u"No error";case mci::invalid_device:return u"Invalid MCI device identifier";
    case mci::unrecognized:return u"Unrecognized MCI command";case mci::hardware:return u"The audio device is unavailable";
    case mci::memory:return u"Not enough memory";case mci::missing_parameter:return u"A required parameter is missing";
    case mci::unsupported:return u"The device does not support this function";case mci::file_missing:return u"The file was not found";
    case mci::not_ready:return u"The device is not ready";case mci::cannot_all:return u"This command cannot use all devices";
    case mci::out_of_range:return u"The parameter is out of range";case mci::conflicting_flags:return u"The command flags are incompatible";
    case mci::duplicate_alias:return u"The device alias is already in use";case mci::bad_constant:return u"Invalid command constant";
    case mci::bad_time:return u"Unsupported time format";case mci::invalid_file:return u"Invalid waveform audio file";
    case mci::null_parameters:return u"The command parameter block is null";case mci::type_required:return u"A device type is required";
    case mci::not_installed:return u"The requested MCI device is not installed";case mci::file_read:return u"The file could not be read";
    case mci::file_write:return u"The file could not be written";case 265:return u"The device is already open";
    case 324:return u"No waveform output device has been selected";case 325:return u"No waveform input device has been selected";default:return {};
    }
}
}

std::span<const WindowsServiceDescriptor> mci_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
        {"winmm.dll","mciSendCommandW",4,0,0,3},{"winmm.dll","mciSendCommandA",4,0,0,3},
        {"winmm.dll","mciGetDeviceIDW",1},{"winmm.dll","mciGetDeviceIDA",1},
        {"winmm.dll","mciGetErrorStringW",3,0,0,5},{"winmm.dll","mciGetErrorStringA",3,0,0,5}
    };return entries;
}
MciState& WindowsServices::mci_state(){if(!mci_)mci_=std::make_shared<MciState>();return *mci_;}

void WindowsServices::notify_mci(std::uint32_t id,std::uint64_t window,std::uint32_t status){
    if(window)wave_callbacks_.push_back({resolve("user32.dll","PostMessageW").value,{window,0x3b9,status,id}});
}
void WindowsServices::complete_mci_transport(std::uint32_t id,std::uint32_t notification){
    auto& bridge=mci_state();const auto found=bridge.devices.find(id);if(found==bridge.devices.end())return;auto& device=*found->second;
    device.finish_transport();
    for(auto handle:device.waiters){if(auto wait=bridge.waits.find(handle);wait!=bridge.waits.end())wait->second.error=notification==8?mci::hardware:0;
        if(auto event=runtime_.wait_objects.find(handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);}
    device.waiters.clear();notify_mci(id,device.notify_window,notification);device.notify_window=0;
}
void WindowsServices::poll_mci(){
    if(!mci_)return;
    for(const auto& [id,item]:mci_->devices){if(!item->buffer || (item->mode!=mci::Mode::Playing && item->mode!=mci::Mode::Recording))continue;
        bool complete=false,failed=false;{std::lock_guard lock(item->buffer->mutex);complete=!item->buffer->playing;failed=item->buffer->capture_failed;}
        if(complete)complete_mci_transport(id,failed?8U:1U);
    }
}
bool WindowsServices::mci_waiting()const{
    if(!mci_)return false;return std::any_of(mci_->devices.begin(),mci_->devices.end(),[](const auto& item){return item.second->mode==mci::Mode::Playing || item.second->mode==mci::Mode::Recording;});
}
void WindowsServices::discard_mci_wait(std::uint64_t handle){
    if(!mci_)return;const auto found=mci_->waits.find(handle);if(found==mci_->waits.end())return;
    if(const auto device=mci_->devices.find(found->second.device);device!=mci_->devices.end())std::erase(device->second->waiters,handle);
    mci_->waits.erase(found);runtime_.wait_objects.erase(handle);
}
std::optional<std::uint32_t> WindowsServices::resume_mci_wait(GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint64_t result){
    poll_mci();auto& bridge=mci_state();const auto found=bridge.waits.find(handle);if(found==bridge.waits.end())throw HostCallFault("MCI_WAIT 缺少等待狀態");
    if(result==0xc0U){start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,UINT32_MAX,1}}},Continuation::MciWait,handle);return {};}
    const auto code=found->second.error;discard_mci_wait(handle);complete_call(state,memory,result==0?code:mci::hardware);return {};
}

std::optional<std::uint32_t> WindowsServices::invoke_mci(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const unsigned p=x86_?4:8,bits=p*8;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto string=[&](std::uint64_t address){if(!address)throw mci::Error(mci::missing_parameter);auto text=wide?read_utf16(memory,GuestAddress{address},32768):utf8_to_utf16_lossy(read_ansi(memory,address,32768));if(text.size()>=32768)throw mci::Error(mci::out_of_range);return text;};
    const auto write_string=[&](std::uint64_t output,std::uint32_t capacity,std::u16string_view value){
        const auto encoded=wide?std::string{}:utf16_to_utf8(value);
        const auto bytes=wide?utf16_bytes(value):std::vector<std::uint8_t>(encoded.begin(),encoded.end());
        const auto count=wide?value.size()+1:bytes.size()+1;if(!output || !capacity)throw mci::Error(mci::missing_parameter);
        if(capacity<count)throw mci::Error(mci::out_of_range);
        memory.write(GuestAddress{output},bytes);if(!wide)memory.store(GuestAddress{output+bytes.size()},8,0);
    };
    try{
        auto& bridge=mci_state();poll_mci();
        if(name.starts_with("mciGetDeviceID")){
            const auto wanted=fold(string(arg(0)));if(wanted==u"all")return finish(mci::all_devices);
            for(const auto& [id,device]:bridge.devices)if((!device->alias.empty() && fold(device->alias)==wanted) || (!device->file.empty() && fold(device->file)==wanted) || (wanted==u"waveaudio" && device->alias.empty()))return finish(id);
            return finish(0);
        }
        if(name.starts_with("mciGetErrorString")){
            const auto text=error_text(static_cast<std::uint32_t>(arg(0))&0xffffU);if(text.empty())return finish(0);
            try{write_string(arg(1),static_cast<std::uint32_t>(arg(2)),text);}catch(const mci::Error&){return finish(0);}return finish(1);
        }
        const auto id=static_cast<std::uint32_t>(arg(0)),command=static_cast<std::uint32_t>(arg(1));const auto flags=arg(2),parameters=arg(3);
        const auto check_flags=[&](std::uint64_t allowed){if(flags&~(allowed|mci::notify|mci::wait))throw mci::Error(mci::conflicting_flags);};
        const auto block=[&](std::size_t size){if(!parameters)throw mci::Error(mci::null_parameters);memory.check(GuestAddress{parameters},size,Access::Read);};
        const auto field=[&](unsigned at,unsigned size=4){block(at+size);return memory.load(GuestAddress{parameters+at},static_cast<std::uint8_t>(size*8));};
        const auto callback=flags&mci::notify?field(0,p):0;
        const auto input_available=[&](){return runtime_.network.external_io && (audio_input_backend_?audio_input_backend_->available():audio::input_available());};
        const auto output_available=[&](){return runtime_.network.external_io && audio::output_available();};
        const auto immediate_notify=[&](std::uint32_t device){
            if(!(flags&mci::notify))return;
            if(const auto found=bridge.devices.find(device);found!=bridge.devices.end() && found->second->notify_window){notify_mci(device,found->second->notify_window,2);found->second->notify_window=0;}
            notify_mci(device,callback,1);
        };
        const auto read_file=[&](std::u16string_view path){
            const auto opened=filesystem_.open_reader(utf16_to_utf8(path));if(opened.error || !opened.file)throw mci::Error(file_error(opened.error));
            if(opened.metadata.attributes&0x10U)throw mci::Error(mci::invalid_file);if(opened.metadata.size>mci::maximum_pcm+65536)throw mci::Error(mci::memory);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));std::size_t at=0;
            while(at<bytes.size()){const auto read=opened.file->read(std::span(bytes).subspan(at),at);if(read.error || !read.bytes)throw mci::Error(file_error(read.error));at+=static_cast<std::size_t>(read.bytes);}return bytes;
        };
        if(command==mci::open){
            // WinMM 參數使用 pack(1)；MCIDEVICEID 之後的指標沒有 x64 對齊填補。
            check_flags(0x13f00);block(4*p+4);memory.check(GuestAddress{parameters+p},4,Access::Write);
            const unsigned type_at=p+4;auto device=std::make_shared<mci::Wave>();device->creator=threads_.current;device->shareable=(flags&0x100U)!=0;
            if((flags&0x1000U) && !(flags&0x2000U))throw mci::Error(mci::conflicting_flags);
            if(flags&0x800U)throw mci::Error(mci::unsupported);
            if(flags&0x2000U){const auto type=field(type_at,p);if(flags&0x1000U){if(type!=mci::wave_device)throw mci::Error(mci::not_installed);}else if(fold(string(type))!=u"waveaudio")throw mci::Error(mci::not_installed);}
            if(flags&0x200U){device->file=string(field(type_at+p,p));if(!(flags&0x2000U)){const auto lower=fold(device->file);if(!lower.ends_with(u".wav") && !lower.ends_with(u".wave"))throw mci::Error(mci::not_installed);}device->load(read_file(device->file));}
            else if(!(flags&0x2000U))throw mci::Error(mci::type_required);
            if(flags&0x400U){device->alias=string(field(type_at+2*p,p));if(device->alias.empty())throw mci::Error(mci::missing_parameter);}
            if(flags&0x10000U){device->buffer_seconds=static_cast<std::uint32_t>(field(type_at+3*p));if(!device->buffer_seconds || device->buffer_seconds>60)throw mci::Error(mci::out_of_range);}
            for(const auto& [existing,current]:bridge.devices){(void)existing;if(!device->alias.empty() && fold(device->alias)==fold(current->alias))throw mci::Error(mci::duplicate_alias);
                if(!device->file.empty() && fold(device->file)==fold(current->file) && (!device->shareable || !current->shareable))throw mci::Error(265);}
            if(bridge.devices.size()>=256 || !bridge.next_device || bridge.next_device==mci::all_devices)throw mci::Error(mci::memory);
            const auto created=bridge.next_device++;bridge.devices.emplace(created,std::move(device));memory.store(GuestAddress{parameters+p},32,created);immediate_notify(created);return finish(0);
        }
        if(command==mci::sysinfo){
            check_flags(0xf00);block(2*p+12);const auto output=field(p,p);const auto capacity=static_cast<std::uint32_t>(field(2*p)),number=static_cast<std::uint32_t>(field(2*p+4)),type=static_cast<std::uint32_t>(field(2*p+8));
            if(type && type!=mci::wave_device){if(flags&0x100U){if(!output || capacity<4)throw mci::Error(mci::out_of_range);memory.store(GuestAddress{output},32,0);return finish(0);}throw mci::Error(mci::not_installed);}
            if((flags&0xd00U)==0x100U){if(!output || capacity<4)throw mci::Error(mci::out_of_range);memory.store(GuestAddress{output},32,flags&0x200U?bridge.devices.size():1);}
            else if((flags&0xd00U)==0x400U){if(flags&0x200U){if(!number || number>bridge.devices.size())throw mci::Error(mci::out_of_range);auto current=bridge.devices.begin();std::advance(current,number-1);write_string(output,capacity,current->second->alias.empty()?current->second->file:current->second->alias);}else{if(number!=1)throw mci::Error(mci::out_of_range);write_string(output,capacity,u"waveaudio");}}
            else if((flags&0xd00U)==0x800U){if(!bridge.devices.contains(id))throw mci::Error(mci::invalid_device);write_string(output,capacity,u"waveaudio");}
            else throw mci::Error(mci::conflicting_flags);immediate_notify(id);return finish(0);
        }
        if(id==mci::all_devices){
            check_flags(0);if(flags&mci::notify || (command!=mci::close && command!=mci::stop && command!=mci::pause && command!=mci::resume))throw mci::Error(mci::cannot_all);
            std::vector<std::uint32_t> ids;for(const auto& [device,item]:bridge.devices){(void)item;ids.push_back(device);}
            for(auto device:ids){auto& item=*bridge.devices.at(device);if(command==mci::close || command==mci::stop)complete_mci_transport(device,4);
                else if(command==mci::pause && item.buffer && item.mode!=mci::Mode::Paused){item.paused_mode=item.mode;{std::lock_guard lock(item.buffer->mutex);item.buffer->playing=false;}item.mode=mci::Mode::Paused;}
                else if(command==mci::resume && item.mode==mci::Mode::Paused && item.buffer){{std::lock_guard lock(item.buffer->mutex);item.buffer->playing=true;}item.mode=item.paused_mode;}
                if(command==mci::close)bridge.devices.erase(device);}
            return finish(0);
        }
        const auto found=bridge.devices.find(id);if(found==bridge.devices.end())throw mci::Error(mci::invalid_device);auto& device=*found->second;
        if(command==mci::close || command==mci::stop){check_flags(0);complete_mci_transport(id,4);immediate_notify(id);if(command==mci::close)bridge.devices.erase(id);return finish(0);}
        if(command==mci::pause || command==mci::resume){
            check_flags(0);if(!device.buffer)throw mci::Error(mci::not_ready);
            if(command==mci::pause){if(device.mode!=mci::Mode::Paused){device.paused_mode=device.mode;std::lock_guard lock(device.buffer->mutex);device.buffer->playing=false;device.mode=mci::Mode::Paused;}}
            else{if(device.mode!=mci::Mode::Paused)throw mci::Error(mci::not_ready);std::lock_guard lock(device.buffer->mutex);device.buffer->playing=true;device.mode=device.paused_mode;}
            immediate_notify(id);return finish(0);
        }
        if(command==mci::seek){
            check_flags(mci::to|0x300U);const auto choices=flags&(mci::to|0x300U);if(!choices || (choices&(choices-1)))throw mci::Error(mci::conflicting_flags);
            const auto position=flags&0x100U?0:flags&0x200U?device.frames():mci::frame(static_cast<std::uint32_t>(field(p)),device.time_format,device.format);
            if(position>device.frames())throw mci::Error(mci::out_of_range);complete_mci_transport(id,4);device.position=position;immediate_notify(id);return finish(0);
        }
        if(command==mci::status || command==mci::getcaps){
            check_flags(command==mci::status?0xc00310U:0x100U);if(!(flags&0x100U))throw mci::Error(mci::missing_parameter);
            const auto item=static_cast<std::uint32_t>(field(command==mci::status?2*p:p+4));const auto result_at=parameters+p;memory.check(GuestAddress{result_at},command==mci::status?p:4,Access::Write);std::uint64_t value=0;
            if(command==mci::getcaps){switch(item){case 1:value=input_available();break;case 2:case 5:case 6:value=1;break;case 3:case 7:value=0;break;case 4:value=mci::wave_device;break;case 8:value=output_available();break;case 9:value=filesystem_.writable();break;case 0x4001:value=input_available()?1:0;break;case 0x4002:value=output_available()?1:0;break;default:throw mci::Error(mci::unsupported);}}
            else{if((flags&mci::from) || ((flags&mci::to)))throw mci::Error(mci::conflicting_flags);if((flags&0x10U) && field(2*p+4)!=1)throw mci::Error(mci::out_of_range);
                switch(item){case 1:value=mci::time(device.media_frames(),device.time_format,device.format);break;case 2:value=mci::time(flags&0x200U?0:device.current(),device.time_format,device.format);break;case 3:case 8:value=1;break;case 4:value=device.status_mode();break;case 5:value=device.media_frames()!=0;break;case 6:value=device.time_format;break;case 7:value=1;break;
                case 0x4001:value=device.format.floating?3:1;break;case 0x4002:value=device.format.channels;break;case 0x4003:value=device.format.rate;break;case 0x4004:value=device.format.rate*device.format.align;break;case 0x4005:value=device.format.align;break;case 0x4006:value=device.format.bits;break;
                case 0x4007:value=device.level();break;
                case 0x400000:if(!device.input && !device.input_set)throw mci::Error(325);value=device.input?0:device.input_id;break;
                case 0x800000:if(!device.output && !device.output_set)throw mci::Error(324);value=device.output?0:device.output_id;break;
                default:throw mci::Error(mci::unsupported);}}
            memory.store(GuestAddress{result_at},static_cast<std::uint8_t>(command==mci::status?bits:32),value);immediate_notify(id);return finish(0);
        }
        if(command==mci::info){
            check_flags(0x300U);const auto item=flags&0x300U;if(item!=0x100U && item!=0x200U)throw mci::Error(mci::conflicting_flags);
            write_string(field(p,p),static_cast<std::uint32_t>(field(2*p)),item==0x100U?std::u16string_view{u"PCM waveform audio"}:std::u16string_view{device.file});immediate_notify(id);return finish(0);
        }
        if(command==mci::set){
            check_flags(0xcff6c00U);auto format=device.format;const auto format_flags=flags&0x3f0000U;
            if(flags&0x400U){const auto selected=static_cast<std::uint32_t>(field(p));if(!mci::time_format(selected))throw mci::Error(mci::bad_time);}
            if(flags&0x800U){if(bool(flags&0x2000U)==bool(flags&0x4000U) || field(p+4)>2)throw mci::Error(mci::conflicting_flags);}
            else if(flags&0x6000U)throw mci::Error(mci::conflicting_flags);
            if(format_flags){if(device.mode!=mci::Mode::Stopped)throw mci::Error(mci::not_ready);
                if(flags&0x10000U){const auto tag=field(p+16,2);if(tag!=1 && tag!=3)throw mci::Error(mci::invalid_file);format.floating=tag==3;}
                if(flags&0x20000U)format.channels=static_cast<unsigned>(field(p+20,2));if(flags&0x40000U)format.rate=static_cast<unsigned>(field(p+24));if(flags&0x200000U)format.bits=static_cast<unsigned>(field(p+36,2));
                format.align=format.channels*format.bits/8;const auto bytes=encode_pcm_wave_file(format,{});if(!bytes || (flags&0x80000U && field(p+28)!=format.rate*format.align) || (flags&0x100000U && field(p+32,2)!=format.align))throw mci::Error(mci::invalid_file);
                if(!device.pcm.empty() && (format.channels!=device.format.channels || format.rate!=device.format.rate || format.bits!=device.format.bits || format.floating!=device.format.floating))throw mci::Error(mci::not_ready);
            }
            for(const auto [flag,at]:{std::pair{0x400000U,p+8},std::pair{0x800000U,p+12}})if((flags&flag) && field(at)!=0 && field(at)!=UINT32_MAX)throw mci::Error(mci::bad_constant);
            device.format=format;if(flags&0x400U)device.time_format=static_cast<std::uint32_t>(field(p));
            if(flags&0x400000U){device.input_id=static_cast<std::uint32_t>(field(p+8));device.input_set=true;}if(flags&0x800000U){device.output_id=static_cast<std::uint32_t>(field(p+12));device.output_set=true;}
            if(flags&0x4000000U){device.input_id=UINT32_MAX;device.input_set=true;}if(flags&0x8000000U){device.output_id=UINT32_MAX;device.output_set=true;}
            if(flags&0x800U){const auto channels=field(p+4);const auto volume=flags&0x2000U?1.F:0.F;if(channels!=2)device.left=volume;if(channels!=1)device.right=volume;
                if(device.buffer){std::lock_guard lock(device.buffer->mutex);device.buffer->left=device.left;device.buffer->right=device.right;}}
            immediate_notify(id);return finish(0);
        }
        if(command==mci::load || command==mci::save){
            check_flags(0x100U);if(!(flags&0x100U))throw mci::Error(mci::missing_parameter);const auto path=string(field(p,p));if(device.mode!=mci::Mode::Stopped)throw mci::Error(mci::not_ready);
            if(command==mci::load){const auto bytes=read_file(path);device.load(bytes);device.file=path;}
            else{const auto encoded=encode_pcm_wave_file(device.format,device.pcm);if(!encoded)throw mci::Error(mci::invalid_file);
                const auto opened=filesystem_.open_host(utf16_to_utf8(path),0x40000000U,2,0,0);if(opened.error || !opened.file)throw mci::Error(file_error(opened.error,true));std::size_t at=0;
                while(at<encoded->size()){const auto write=opened.file->write(std::span(*encoded).subspan(at),at);if(write.error || !write.bytes)throw mci::Error(file_error(write.error,true));at+=static_cast<std::size_t>(write.bytes);}if(opened.file->flush())throw mci::Error(mci::file_write);device.file=path;device.dirty=false;}
            immediate_notify(id);return finish(0);
        }
        if(command==mci::erase){
            check_flags(mci::from|mci::to);if(device.mode!=mci::Mode::Stopped)throw mci::Error(mci::not_ready);
            const auto first=flags&mci::from?mci::frame(static_cast<std::uint32_t>(field(p)),device.time_format,device.format):device.position,last=flags&mci::to?mci::frame(static_cast<std::uint32_t>(field(p+4)),device.time_format,device.format):device.frames();
            if(first>last || last>device.frames())throw mci::Error(mci::out_of_range);device.pcm.erase(device.pcm.begin()+static_cast<std::ptrdiff_t>(first*device.format.align),device.pcm.begin()+static_cast<std::ptrdiff_t>(last*device.format.align));device.position=first;device.dirty=true;immediate_notify(id);return finish(0);
        }
        if(command==mci::cue){
            check_flags(0xc00000U);if((flags&0xc00000U)==0xc00000U)throw mci::Error(mci::conflicting_flags);
            if(device.mode!=mci::Mode::Stopped)throw mci::Error(mci::not_ready);
            std::shared_ptr<audio::Buffer> buffer;std::unique_ptr<audio::Output> output;std::unique_ptr<audio::Input> input;
            if(flags&0x400000U){if(!input_available())throw mci::Error(mci::hardware);buffer=std::make_shared<audio::Buffer>();buffer->channels=device.format.channels;buffer->bits=device.format.bits;buffer->rate=buffer->frequency=device.format.rate;buffer->floating=device.format.floating;
                const auto size=std::uint64_t{device.buffer_seconds}*device.format.rate*device.format.align;if(size>mci::maximum_pcm)throw mci::Error(mci::memory);buffer->pcm.resize(static_cast<std::size_t>(size));
                try{input=audio_input_backend_?audio_input_backend_->open(buffer):audio::open_input(buffer);}catch(const std::runtime_error&){throw mci::Error(mci::hardware);}if(!input)throw mci::Error(mci::hardware);
            }else{if(!output_available() || device.pcm.empty())throw mci::Error(mci::not_ready);buffer=device.playback(device.position,device.frames());try{output=audio::open_output(buffer);}catch(const std::runtime_error&){throw mci::Error(mci::hardware);}if(!output)throw mci::Error(mci::hardware);}
            complete_mci_transport(id,4);device.buffer=std::move(buffer);device.output=std::move(output);device.input=std::move(input);
            immediate_notify(id);return finish(0);
        }
        if(command==mci::play || command==mci::record){
            check_flags(command==mci::play?(mci::from|mci::to):(mci::from|mci::to|0x300U));
            if(command==mci::record && (flags&0x300U)==0x300U)throw mci::Error(mci::conflicting_flags);
            if(device.mode==mci::Mode::Recording || (device.mode==mci::Mode::Paused && device.paused_mode==mci::Mode::Recording))throw mci::Error(mci::not_ready);
            const auto first=flags&mci::from?mci::frame(static_cast<std::uint32_t>(field(p)),device.time_format,device.format):device.current();
            const auto limit=command==mci::play?device.frames():mci::maximum_pcm/device.format.align;
            const auto last=flags&mci::to?mci::frame(static_cast<std::uint32_t>(field(p+4)),device.time_format,device.format):limit;
            if(first>last || last>limit || first>device.frames())throw mci::Error(mci::out_of_range);
            std::shared_ptr<audio::Buffer> buffer;std::unique_ptr<audio::Output> output;std::unique_ptr<audio::Input> input;std::vector<std::uint8_t> tail;
            if(command==mci::play){if(device.pcm.empty())throw mci::Error(mci::not_ready);if(!output_available())throw mci::Error(mci::hardware);buffer=device.playback(first,last);if(first!=last){try{output=audio::open_output(buffer);}catch(const std::runtime_error&){throw mci::Error(mci::hardware);}if(!output)throw mci::Error(mci::hardware);}}
            else{if(!input_available())throw mci::Error(mci::hardware);buffer=std::make_shared<audio::Buffer>();buffer->channels=device.format.channels;buffer->bits=device.format.bits;buffer->rate=buffer->frequency=device.format.rate;buffer->floating=device.format.floating;buffer->pcm.resize(static_cast<std::size_t>((last-first)*device.format.align));
                if(flags&0x100U){tail.assign(device.pcm.begin()+static_cast<std::ptrdiff_t>(first*device.format.align),device.pcm.end());if(buffer->pcm.size()>mci::maximum_pcm-device.pcm.size())throw mci::Error(mci::memory);}
                if(first!=last){try{input=audio_input_backend_?audio_input_backend_->open(buffer):audio::open_input(buffer);}catch(const std::runtime_error&){throw mci::Error(mci::hardware);}if(!input)throw mci::Error(mci::hardware);}}
            // 同方向、同終點的播放仍可滿足舊的完成條件。保留其等待者；
            // 只有新的 NOTIFY 取代舊通知，終點／作業改變才中止舊作業。
            const bool continuing=command==mci::play && last==device.end &&
                (device.mode==mci::Mode::Playing || (device.mode==mci::Mode::Paused && device.paused_mode==mci::Mode::Playing));
            if(continuing){
                device.finish_transport();
                if(flags&mci::notify){notify_mci(id,device.notify_window,2);device.notify_window=callback;}
            }else{complete_mci_transport(id,4);device.notify_window=callback;}
            device.buffer=std::move(buffer);device.output=std::move(output);device.input=std::move(input);device.begin=first;device.end=last;device.position=first;device.captured=0;device.insert_record=(flags&0x100U)!=0;device.record_tail=std::move(tail);device.mode=command==mci::play?mci::Mode::Playing:mci::Mode::Recording;
            {std::lock_guard lock(device.buffer->mutex);device.buffer->playing=first!=last;}
            if(first==last){complete_mci_transport(id,1);return finish(0);}
            if(flags&mci::wait){const auto handle=runtime_.allocate_wait_handle();
                try{WaitObject event;event.kind=WaitObject::Kind::Event;event.manual_reset=true;event.access=0x100000U;runtime_.wait_objects.emplace(handle,std::move(event));bridge.waits.emplace(handle,MciState::Wait{id,0,threads_.current});device.waiters.push_back(handle);
                    start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,UINT32_MAX,1}}},Continuation::MciWait,handle);
                }catch(...){discard_mci_wait(handle);runtime_.wait_objects.erase(handle);throw;}return {};}
            return finish(0);
        }
        throw mci::Error(mci::unrecognized);
    }catch(const mci::Error& error){return finish(name.starts_with("mciGet")?0:error.code);}catch(const MemoryFault&){return finish(name.starts_with("mciGet")?0:mci::missing_parameter);}catch(const std::bad_alloc&){return finish(name.starts_with("mciGet")?0:mci::memory);}
}
}
