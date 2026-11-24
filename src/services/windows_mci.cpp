#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "mci_wave.hpp"
#include "mci_sequencer.hpp"
#include "mci_cdaudio.hpp"
#include "mp4_preview.hpp"
#include "mpeg_preview.hpp"
#include "mci_avi.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
namespace {
std::u16string fold(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::uint32_t file_error(std::uint32_t code,bool writing=false){return code==2 || code==3?mci::file_missing:code==8?mci::memory:writing?mci::file_write:mci::file_read;}
// mciGetDeviceID 與命令字串共用：別名、檔名，或未取別名的 waveaudio 裝置。
std::uint32_t find_device(const MciState& bridge,std::u16string_view name){
    const auto wanted=fold(std::u16string(name));if(wanted==u"all")return mci::all_devices;
    for(const auto& [id,device]:bridge.devices)if((!device->alias.empty() && fold(device->alias)==wanted) || (!device->file.empty() && fold(device->file)==wanted) || (wanted==u"waveaudio" && device->alias.empty()))return id;
    for(const auto& [id,device]:bridge.videos)
        if((!device->alias.empty() && fold(device->alias)==wanted) || fold(device->file)==wanted ||
           ((wanted==u"digitalvideo" || wanted==(device->quartz?u"mpegvideo":u"avivideo")) && device->alias.empty()))return id;
    for(const auto& [id,device]:bridge.sequences)
        if((!device->alias.empty() && fold(device->alias)==wanted) || (!device->file.empty() && fold(device->file)==wanted) ||
           (wanted==u"sequencer" && device->alias.empty()))return id;
    for(const auto& [id,device]:bridge.compact_discs)
        if((!device->alias.empty() && fold(device->alias)==wanted) || (!device->file.empty() && fold(device->file)==wanted) ||
           (wanted==u"cdaudio" && device->alias.empty()))return id;
    return 0;
}
mci::DeviceControl* mci_control(MciState& bridge,std::uint32_t id){
    if(const auto driver=bridge.installable_drivers.find(id);driver!=bridge.installable_drivers.end())return &driver->second;
    if(const auto wave=bridge.devices.find(id);wave!=bridge.devices.end())return wave->second.get();
    if(const auto video=bridge.videos.find(id);video!=bridge.videos.end())return video->second.get();
    if(const auto sequence=bridge.sequences.find(id);sequence!=bridge.sequences.end())return sequence->second.get();
    if(const auto disc=bridge.compact_discs.find(id);disc!=bridge.compact_discs.end())return disc->second.get();
    return nullptr;
}
std::u16string_view error_text(std::uint32_t code){
    switch(code){
    case 0:return u"No error";case mci::invalid_device:return u"Invalid MCI device identifier";
    case mci::unrecognized:return u"Unrecognized MCI command";case mci::hardware:return u"The audio device is unavailable";
    case mci::memory:return u"Not enough memory";case mci::missing_parameter:return u"A required parameter is missing";
    case mci::unsupported:return u"The device does not support this function";case mci::file_missing:return u"The file was not found";
    case mci::not_ready:return u"The device is not ready";case mci::cannot_all:return u"This command cannot use all devices";
    case mci::out_of_range:return u"The parameter is out of range";case mci::conflicting_flags:return u"The command flags are incompatible";
    case mci::duplicate_alias:return u"The device alias is already in use";case mci::bad_constant:return u"Invalid command constant";
    case mci::bad_time:return u"Unsupported time format";case mci::invalid_file:return u"Invalid media file";
    case mci::null_parameters:return u"The command parameter block is null";case mci::type_required:return u"A device type is required";
    case mci::not_installed:return u"The requested MCI device is not installed";case mci::file_read:return u"The file could not be read";
    case mci::file_write:return u"The file could not be written";case 265:return u"The device is already open";
    case 324:return u"No waveform output device has been selected";case 325:return u"No waveform input device has been selected";
    case mci::seq_division_incompatible:return u"The MIDI division and time format are incompatible";
    case mci::seq_port_missing:return u"The MIDI port does not exist";
    case mci::seq_port_mapping:return u"The MIDI port is unavailable";
    case mci::cd_no_identity:return u"The compact disc does not have the requested identifier";
    default:return {};
    }
}
}

std::span<const WindowsServiceDescriptor> mci_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
        {"winmm.dll","mciSendCommandW",4,0,0,3},{"winmm.dll","mciSendCommandA",4,0,0,3},
        {"winmm.dll","mciGetDeviceIDW",1},{"winmm.dll","mciGetDeviceIDA",1},
        {"winmm.dll","mciGetErrorStringW",3,0,0,5},{"winmm.dll","mciGetErrorStringA",3,0,0,5},
        {"winmm.dll","mciSendStringW",4,0,0,4},{"winmm.dll","mciSendStringA",4,0,0,4},{"winmm.dll","mciExecute",1},
        {"winmm.dll","mciGetDriverData",1,0,0,1},{"winmm.dll","mciSetDriverData",2,0,0,1},{"winmm.dll","mciGetCreatorTask",1,0,0,1},
        {"winmm.dll","mciDriverNotify",3,0,0,6},{"winmm.dll","mciDriverYield",1,0,0,1},{"winmm.dll","mciSetYieldProc",3,0,0,5},{"winmm.dll","mciGetYieldProc",2,0,0,1},
        {"winmm.dll","mciGetDeviceIDFromElementIDA",2,0,0,1},{"winmm.dll","mciGetDeviceIDFromElementIDW",2,0,0,1},
        {"winmm.dll","mciLoadCommandResource",3,0,0,4},{"winmm.dll","mciFreeCommandResource",1,0,0,1},
        {"mciavi32.dll","DriverProc",5,0,0,4},{"mciwave.dll","DriverProc",5,0,0,4},{"mciseq.dll","DriverProc",5,0,0,4},
        {"mciqtz32.dll","DriverProc",5,0,0,4},
        {"mcicda.dll","DriverProc",5,0,0,4},
        {"internal-avi-driver","PaintVideo",2,0,0,3}
    };return entries;
}
MciState& WindowsServices::mci_state(){if(!mci_)mci_=std::make_shared<MciState>();return *mci_;}

void WindowsServices::notify_mci(std::uint32_t id,std::uint64_t window,std::uint32_t status){
    static const bool trace=std::getenv("X64ARM_TRACE_MCI")!=nullptr;
    if(trace)std::fprintf(stderr,"[mci-notify] id=%u window=0x%llx status=%u local=%d\n",id,static_cast<unsigned long long>(window),status,gui_.windows.contains(window)?1:0);
    if(window)wave_callbacks_.push_back({resolve("user32.dll","PostMessageW").value,{window,0x3b9,status,id}});
}
void WindowsServices::complete_mci_transport(std::uint32_t id,std::uint32_t notification){
    auto& bridge=mci_state();
    const auto signal=[&](auto& device){
        for(auto handle:device.waiters){
            if(auto wait=bridge.waits.find(handle);wait!=bridge.waits.end())wait->second.error=notification==8?mci::hardware:0;
            if(auto event=runtime_.wait_objects.find(handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);
        }
        device.waiters.clear();
        notify_mci(id,std::exchange(device.notify_window,0),notification);
    };
    if(const auto wave=bridge.devices.find(id);wave!=bridge.devices.end()){
        wave->second->finish_transport();
        signal(*wave->second);
    }else if(const auto avi=bridge.videos.find(id);avi!=bridge.videos.end()){
        avi->second->stop(runtime_.clock_ticks);
        signal(*avi->second);
    }else if(const auto sequence=bridge.sequences.find(id);sequence!=bridge.sequences.end()){
        sequence->second->finish(runtime_.clock_ticks);
        signal(*sequence->second);
    }else if(const auto disc=bridge.compact_discs.find(id);disc!=bridge.compact_discs.end()){
        disc->second->finish();
        signal(*disc->second);
    }
}
void WindowsServices::poll_mci(){
    if(!mci_)return;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> finished;
    for(const auto& [id,item]:mci_->devices){if(!item->buffer || (item->mode!=mci::Mode::Playing && item->mode!=mci::Mode::Recording))continue;
        bool complete=false,failed=false;{std::lock_guard lock(item->buffer->mutex);complete=!item->buffer->playing;failed=item->buffer->capture_failed;}
        if(complete)finished.emplace_back(id,failed?8U:1U);
    }
    for(const auto& [id,item]:mci_->videos){
        // 診斷：X64ARM_TRACE_MCI=1 時每 100 次輪詢列出影片自動繪製的條件（前 40 筆；=2 不限筆數並附客體時鐘）。
        static const auto* trace_setting=std::getenv("X64ARM_TRACE_MCI");static const bool trace_mci=trace_setting!=nullptr;
        static const unsigned trace_limit=trace_setting && std::string_view(trace_setting)=="2"?UINT32_MAX:40U;static unsigned traced=0,polls=0;
        if(trace_mci && traced<trace_limit && !(polls++%100)){++traced;
            if(trace_limit==UINT32_MAX)std::fprintf(stderr,"[mci-video-clock] ticks=%llu ended=%d\n",static_cast<unsigned long long>(runtime_.clock_ticks),item->ended(runtime_.clock_ticks)?1:0);
            std::fprintf(stderr,"[mci-video] id=%u mode=%d visual=%d presentation=%u hidden=%d window=0x%llx exists=%d pending=%d frame=%llu presented=%llu length=%llu callbacks=%zu\n",id,static_cast<int>(item->mode),
                item->clip->visual()?1:0,item->presentation,item->hidden?1:0,static_cast<unsigned long long>(item->window),gui_.windows.contains(item->window)?1:0,item->paint_pending?1:0,
                static_cast<unsigned long long>(item->current(runtime_.clock_ticks)),static_cast<unsigned long long>(item->presented_frame),static_cast<unsigned long long>(item->length()),wave_callbacks_.size());}
        if(item->ended(runtime_.clock_ticks))finished.emplace_back(id,1);
        if(!item->clip->visual() || !item->presentation || item->hidden || !item->window || !gui_.windows.contains(item->window) ||
           item->paint_pending || wave_callbacks_.size()>=64)continue;
        const auto frame=std::min(item->current(runtime_.clock_ticks),item->length()-1);
        if(frame==item->presented_frame)continue;
        wave_callbacks_.push_back({resolve("internal-avi-driver","PaintVideo").value,{id,item->presentation}});
        item->paint_pending=true;
    }
    for(const auto& [id,item]:mci_->sequences)
        if(const auto status=item->completion(runtime_.clock_ticks))finished.emplace_back(id,status);
    for(const auto& [id,item]:mci_->compact_discs)
        if(const auto status=item->pump())finished.emplace_back(id,status);
    for(const auto& [id,notification]:finished){
        complete_mci_transport(id,notification);
        if(const auto found=mci_->devices.find(id);found!=mci_->devices.end() && found->second->auto_close)mci_->devices.erase(found);
        if(const auto found=mci_->videos.find(id);found!=mci_->videos.end() && found->second->auto_close)mci_->videos.erase(found);
        if(const auto found=mci_->sequences.find(id);found!=mci_->sequences.end() && found->second->auto_close)mci_->sequences.erase(found);
        if(const auto found=mci_->compact_discs.find(id);found!=mci_->compact_discs.end() && found->second->auto_close)mci_->compact_discs.erase(found);
    }
}
bool WindowsServices::mci_waiting()const{
    if(!mci_)return false;
    return std::any_of(mci_->devices.begin(),mci_->devices.end(),[](const auto& item){return item.second->mode==mci::Mode::Playing || item.second->mode==mci::Mode::Recording;}) ||
        std::any_of(mci_->videos.begin(),mci_->videos.end(),[](const auto& item){return item.second->mode==mci::Mode::Playing;}) ||
        std::any_of(mci_->sequences.begin(),mci_->sequences.end(),[](const auto& item){return item.second->mode==mci::Mode::Playing;}) ||
        std::any_of(mci_->compact_discs.begin(),mci_->compact_discs.end(),[](const auto& item){return item.second->mode==mci::Mode::Playing;});
}
void WindowsServices::discard_mci_wait(std::uint64_t handle){
    if(!mci_)return;const auto found=mci_->waits.find(handle);if(found==mci_->waits.end())return;
    if(const auto device=mci_->devices.find(found->second.device);device!=mci_->devices.end())std::erase(device->second->waiters,handle);
    if(const auto device=mci_->videos.find(found->second.device);device!=mci_->videos.end())std::erase(device->second->waiters,handle);
    if(const auto device=mci_->sequences.find(found->second.device);device!=mci_->sequences.end())std::erase(device->second->waiters,handle);
    if(const auto device=mci_->compact_discs.find(found->second.device);device!=mci_->compact_discs.end())std::erase(device->second->waiters,handle);
    mci_->waits.erase(found);runtime_.wait_objects.erase(handle);
}
std::optional<std::uint32_t> WindowsServices::resume_mci_wait(GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint64_t result){
    poll_mci();auto& bridge=mci_state();const auto found=bridge.waits.find(handle);if(found==bridge.waits.end())throw HostCallFault("MCI_WAIT 缺少等待狀態");
    // 裝置設有 yield 程序時以 100 ms 為間隔等待，逾時呼叫 yield 程序；傳回非零即結束等待（命令仍成功，播放／錄音繼續）。
    auto& wait=found->second;const auto* control=mci_control(bridge,wait.device);const auto yield=control?control->yield_proc:0;
    const auto again=[&]{start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,yield?100U:UINT32_MAX,1}}},Continuation::MciWait,handle);};
    if(wait.yielding){
        wait.yielding=false;
        if(static_cast<std::uint32_t>(result)){discard_mci_wait(handle);complete_call(state,memory,0);return {};}
        again();return {};
    }
    if(result==0x102U && yield){wait.yielding=true;start_callbacks(state,memory,{{yield,{wait.device,control->yield_data}}},Continuation::MciWait,handle);return {};}
    if(result==0xc0U || result==0x102U){again();return {};}
    const auto code=wait.error;discard_mci_wait(handle);complete_call(state,memory,result==0?code:mci::hardware);return {};
}

std::optional<std::uint32_t> WindowsServices::invoke_mci(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.starts_with("mciSendString") || service.symbol=="mciExecute")return invoke_mci_string(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const bool avi_installable=service.family=="mciavi32.dll",quartz_installable=service.family=="mciqtz32.dll";
    const bool video_installable=avi_installable || quartz_installable;
    const bool wave_installable=service.family=="mciwave.dll";
    const bool sequencer_installable=service.family=="mciseq.dll";
    const bool cd_installable=service.family=="mcicda.dll";
    const bool installable=video_installable || wave_installable || sequencer_installable || cd_installable;
    const bool wide=installable || name.ends_with('W');
    const unsigned p=x86_?4:8,bits=p*8;
    const auto message=installable?static_cast<std::uint32_t>(arg(2)):0;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto string=[&](std::uint64_t address){if(!address)throw mci::Error(mci::missing_parameter);auto text=wide?read_utf16(memory,GuestAddress{address},32768):ansi_to_utf16_lossy(read_ansi(memory,address,32768));if(text.size()>=32768)throw mci::Error(mci::out_of_range);return text;};
    const auto write_string=[&](std::uint64_t output,std::uint32_t capacity,std::u16string_view value){
        const auto encoded=wide?std::string{}:utf16_to_ansi(value);
        const auto bytes=wide?utf16_bytes(value):std::vector<std::uint8_t>(encoded.begin(),encoded.end());
        const auto count=wide?value.size()+1:bytes.size()+1;if(!output || !capacity)throw mci::Error(mci::missing_parameter);
        if(capacity<count)throw mci::Error(mci::out_of_range);
        memory.write(GuestAddress{output},bytes);if(!wide)memory.store(GuestAddress{output+bytes.size()},8,0);
    };
    const auto paint_video=[&](mci::AviDevice& video,std::uint64_t dc,const GuiRect& rect,std::uint64_t frame){
        if(!video.clip->visual())return mci::unsupported;
        const auto context=gui_.contexts.find(dc);
        if(context==gui_.contexts.end())return mci::not_ready;
        std::optional<GuiBitmap> bitmap;
        try{bitmap=video.clip->at(static_cast<std::size_t>(frame));}
        catch(const std::runtime_error&){return mci::invalid_file;}
        if(!bitmap)return mci::invalid_file;
        const auto source=make_hardware_raster(bitmap->width,bitmap->height,bitmap->pixels);
        return blit_raster(memory,dc,source.get(),rect,video.source,context->second,UINT32_MAX,{})?0U:mci::hardware;
    };
    try{
        auto& bridge=mci_state();
        if(service.family=="internal-avi-driver"){
            const auto entry=bridge.videos.find(static_cast<std::uint32_t>(arg(0)));
            if(entry==bridge.videos.end() || entry->second->presentation!=arg(1))return finish(0);
            auto& video=*entry->second;
            video.paint_pending=false;
            const auto window=gui_.windows.find(video.window);
            if(window==gui_.windows.end())return finish(0);
            const auto dc=video.window+0x10000000U;
            gui_.contexts.try_emplace(dc);
            const auto rect=video.fixed_target?video.target:GuiRect{0,0,static_cast<std::int32_t>(window->second.width),
                                                                         static_cast<std::int32_t>(window->second.height)};
            video.presented_frame=std::min(video.current(runtime_.clock_ticks),video.length()-1);
            return finish(paint_video(video,dc,rect,video.presented_frame));
        }
        poll_mci();
#include "windows_mci_driver.inc"
#include "windows_mci_installable.inc"
        if(name.starts_with("mciGetDeviceID")){
            return finish(find_device(bridge,string(arg(0))));
        }
        if(name.starts_with("mciGetErrorString")){
            const auto text=error_text(static_cast<std::uint32_t>(arg(0))&0xffffU);if(text.empty())return finish(0);
            try{write_string(arg(1),static_cast<std::uint32_t>(arg(2)),text);}catch(const mci::Error&){return finish(0);}return finish(1);
        }
        const auto id=static_cast<std::uint32_t>(arg(0));
        auto command=installable?message:static_cast<std::uint32_t>(arg(1));
        if(installable){
            if(message==mci::open_driver)command=mci::open;
            if(message==mci::close_driver)command=mci::close;
        }
        const auto flags=arg(installable?3:2),parameters=arg(installable?4:3);
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
        const auto read_file=[&](std::u16string_view path,std::size_t limit=mci::maximum_pcm+65536){
            const auto opened=filesystem_.open_reader(utf16_to_utf8(path));if(opened.error || !opened.file)throw mci::Error(file_error(opened.error));
            if(opened.metadata.attributes&0x10U)throw mci::Error(mci::invalid_file);if(opened.metadata.size>limit)throw mci::Error(mci::memory);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));std::size_t at=0;
            while(at<bytes.size()){const auto read=opened.file->read(std::span(bytes).subspan(at),at);if(read.error || !read.bytes)throw mci::Error(file_error(read.error));at+=static_cast<std::size_t>(read.bytes);}return bytes;
        };
        const auto wait_transport=[&](std::vector<std::uint64_t>& waiters)->std::optional<std::uint32_t>{
            const auto handle=runtime_.allocate_wait_handle();
            try{
                WaitObject event;
                event.kind=WaitObject::Kind::Event;
                event.manual_reset=true;
                event.access=0x100000U;
                runtime_.wait_objects.emplace(handle,std::move(event));
                bridge.waits.emplace(handle,MciState::Wait{id,0,threads_.current});
                waiters.push_back(handle);
                const auto* control=mci_control(bridge,id);
                start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,
                    {handle,control && control->yield_proc?100U:UINT32_MAX,1}}},Continuation::MciWait,handle);
            }catch(...){discard_mci_wait(handle);runtime_.wait_objects.erase(handle);throw;}
            return {};
        };
#include "windows_mci_cdaudio.inc"
#include "windows_mci_sequencer.inc"
#include "windows_mci_video.inc"
        if(command==mci::open){
            // WinMM 參數使用 pack(1)；MCIDEVICEID 之後的指標沒有 x64 對齊填補。
            check_flags(0x13f00);block(4*p+4);memory.check(GuestAddress{parameters+p},4,Access::Write);
            const unsigned type_at=p+4;auto device=std::make_shared<mci::Wave>();device->creator=threads_.current;device->shareable=(flags&0x100U)!=0;
            if(wave_installable){
                device->format={1,11025,8,1,false};
                device->input_set=device->output_set=true;
            }
            if((flags&0x1000U) && !(flags&0x2000U))throw mci::Error(mci::conflicting_flags);
            if(flags&0x800U)throw mci::Error(mci::unsupported);
            if(flags&0x2000U){const auto type=field(type_at,p);if(flags&0x1000U){if(type!=mci::wave_device)throw mci::Error(mci::not_installed);}else if(fold(string(type))!=u"waveaudio")throw mci::Error(mci::not_installed);}
            if(flags&0x200U){device->file=string(field(type_at+p,p));if(!wave_installable && !(flags&0x2000U)){const auto lower=fold(device->file);if(!lower.ends_with(u".wav") && !lower.ends_with(u".wave"))throw mci::Error(mci::not_installed);}device->load(read_file(device->file));}
            else if(!wave_installable && !(flags&0x2000U))throw mci::Error(mci::type_required);
            if(flags&0x400U){device->alias=string(field(type_at+2*p,p));if(device->alias.empty())throw mci::Error(mci::missing_parameter);}
            if(flags&0x10000U){device->buffer_seconds=static_cast<std::uint32_t>(field(type_at+3*p));if(!device->buffer_seconds || device->buffer_seconds>60)throw mci::Error(mci::out_of_range);}
            for(const auto& [existing,current]:bridge.devices){(void)existing;if(!device->alias.empty() && fold(device->alias)==fold(current->alias))throw mci::Error(mci::duplicate_alias);
                if(!device->file.empty() && fold(device->file)==fold(current->file) && (!device->shareable || !current->shareable))throw mci::Error(265);}
            if(!device->alias.empty() && std::any_of(bridge.videos.begin(),bridge.videos.end(),[&](const auto& existing){
                return !existing.second->alias.empty() && fold(existing.second->alias)==fold(device->alias);
            }))throw mci::Error(mci::duplicate_alias);
            const auto created=wave_installable?id:bridge.allocate_device();
            bridge.devices.emplace(created,std::move(device));memory.store(GuestAddress{parameters+p},32,created);
            immediate_notify(created);return finish(0);
        }
        if(command==mci::sysinfo){
            check_flags(0xf00);block(2*p+12);const auto output=field(p,p);const auto capacity=static_cast<std::uint32_t>(field(2*p)),number=static_cast<std::uint32_t>(field(2*p+4)),type=static_cast<std::uint32_t>(field(2*p+8));
            const bool waves=!type || type==mci::wave_device,avi=!type || type==mci::avi_device,
                sequences=!type || type==mci::sequencer_device,compact_discs=!type || type==mci::cd_device;
            std::vector<std::u16string_view> installed;
            if(waves)installed.push_back(u"waveaudio");
            if(avi){installed.push_back(u"avivideo");installed.push_back(u"mpegvideo");}
            if(sequences)installed.push_back(u"sequencer");
            if(compact_discs)installed.push_back(u"cdaudio");
            if((flags&0xd00U)==0x100U){
                if(!output || capacity<4)throw mci::Error(mci::out_of_range);
                const auto count=(flags&0x200U)?(waves?bridge.devices.size():0)+(avi?bridge.videos.size():0)+
                    (sequences?bridge.sequences.size():0)+(compact_discs?bridge.compact_discs.size():0):installed.size();
                memory.store(GuestAddress{output},32,count);
            }else if((flags&0xd00U)==0x400U){
                if(installed.empty())throw mci::Error(mci::not_installed);
                if(flags&0x200U){
                    std::vector<std::pair<std::uint32_t,std::u16string_view>> names;
                    const auto append=[&](const auto& devices){
                        for(const auto& [key,item]:devices)names.emplace_back(key,item->alias.empty()?item->file:item->alias);
                    };
                    if(waves)append(bridge.devices);
                    if(avi)append(bridge.videos);
                    if(sequences)append(bridge.sequences);
                    if(compact_discs)append(bridge.compact_discs);
                    std::sort(names.begin(),names.end());
                    if(!number || number>names.size())throw mci::Error(mci::out_of_range);
                    write_string(output,capacity,names[number-1].second);
                }else{
                    if(!number || number>installed.size())throw mci::Error(mci::out_of_range);
                    write_string(output,capacity,installed[number-1]);
                }
            }else if((flags&0xd00U)==0x800U){
                if(waves && bridge.devices.contains(id))write_string(output,capacity,u"waveaudio");
                else if(avi && bridge.videos.contains(id))write_string(output,capacity,bridge.videos.at(id)->quartz?u"mpegvideo":u"avivideo");
                else if(sequences && bridge.sequences.contains(id))write_string(output,capacity,u"sequencer");
                else if(compact_discs && bridge.compact_discs.contains(id))write_string(output,capacity,u"cdaudio");
                else throw mci::Error(mci::invalid_device);
            }else throw mci::Error(mci::conflicting_flags);
            immediate_notify(id);
            return finish(0);
        }
        if(id==mci::all_devices){
            check_flags(0);if(flags&mci::notify || (command!=mci::close && command!=mci::stop && command!=mci::pause && command!=mci::resume))throw mci::Error(mci::cannot_all);
            std::vector<std::uint32_t> ids;for(const auto& [device,item]:bridge.devices){(void)item;ids.push_back(device);}
            for(auto device:ids){auto& item=*bridge.devices.at(device);if(command==mci::close || command==mci::stop)complete_mci_transport(device,4);
                else if(command==mci::pause && item.buffer && item.mode!=mci::Mode::Paused){item.paused_mode=item.mode;{std::lock_guard lock(item.buffer->mutex);item.buffer->playing=false;}item.mode=mci::Mode::Paused;}
                else if(command==mci::resume && item.mode==mci::Mode::Paused && item.buffer){{std::lock_guard lock(item.buffer->mutex);item.buffer->playing=true;}item.mode=item.paused_mode;}
                if(command==mci::close)bridge.devices.erase(device);}
            for(auto it=bridge.videos.begin();it!=bridge.videos.end();){
                if(command==mci::close || command==mci::stop)complete_mci_transport(it->first,4);
                else if(command==mci::pause && it->second->mode==mci::Mode::Playing)it->second->pause(runtime_.clock_ticks);
                else if(command==mci::resume && it->second->mode==mci::Mode::Paused)it->second->resume(runtime_.clock_ticks);
                if(command==mci::close)it=bridge.videos.erase(it);
                else ++it;
            }
            const auto now=runtime_.clock_ticks;
            for(auto it=bridge.sequences.begin();it!=bridge.sequences.end();){
                auto& item=*it->second;
                if(command==mci::close || command==mci::stop)complete_mci_transport(it->first,4);
                else if(command==mci::pause && item.mode==mci::Mode::Playing)item.finish(now,mci::Mode::Paused);
                else if(command==mci::resume && item.mode==mci::Mode::Paused){
                    auto next=item.prepare(item.position,item.end,item.port,runtime_.network.external_io);
                    item.start(std::move(next),item.position,item.end,now);
                    if(item.mode==mci::Mode::Stopped)complete_mci_transport(it->first,1);
                }
                if(command==mci::close)it=bridge.sequences.erase(it);
                else ++it;
            }
            for(auto it=bridge.compact_discs.begin();it!=bridge.compact_discs.end();){
                if(command==mci::close || command==mci::stop)complete_mci_transport(it->first,4);
                else if(command==mci::pause)it->second->pause();
                else if(command==mci::resume)it->second->resume();
                if(command==mci::close)it=bridge.compact_discs.erase(it);
                else ++it;
            }
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
            unsigned returned=0;
            if(wave_installable){
                const auto numeric=static_cast<unsigned>(value);
                std::optional<unsigned> resource;
                if(command==mci::getcaps && item<=9){
                    resource=item==4?mci::resource(numeric,numeric):mci::boolean_resource(numeric!=0);
                }else if(command==mci::status){
                    if(item==4)resource=mci::resource(numeric,numeric);
                    else if(item==5 || item==7)resource=mci::boolean_resource(numeric!=0);
                    else if(item==6)resource=mci::resource(numeric,numeric+mci::format_resource_base);
                    else if(item==0x4001 && numeric==1)resource=mci::resource(1,mci::pcm_format_resource);
                    else if((item==0x400000 || item==0x800000) && numeric==UINT32_MAX)
                        resource=mci::resource(numeric,mci::wave_mapper_resource);
                }
                if(resource){value=*resource;returned=mci::resource_returned;}
            }
            memory.store(GuestAddress{result_at},static_cast<std::uint8_t>(command==mci::status?bits:32),value);
            immediate_notify(id);return finish(returned);
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
            if(flags&mci::wait)return wait_transport(device.waiters);
            return finish(0);
        }
        throw mci::Error(mci::unrecognized);
    }catch(const mci::Error& error){
        // 診斷：X64ARM_TRACE_MCI=1 時列出失敗的 MCI 命令；MCI_OPEN 另列裝置型別（字串或型別 ID）與元素檔名。
        static const bool trace_mci=std::getenv("X64ARM_TRACE_MCI")!=nullptr;
        if(trace_mci && name.starts_with("mciSendCommand"))try{
            const auto msg=static_cast<std::uint32_t>(arg(1)),open_flags=static_cast<std::uint32_t>(arg(2));const auto block_at=arg(3);
            std::string detail;
            if(msg==0x803 && block_at){
                const unsigned width=x86_?4U:8U;const auto pointer=[&](unsigned at){return memory.load(GuestAddress{block_at+at},static_cast<std::uint8_t>(width*8));};
                const auto type=pointer(width+4),element=pointer(2*width+4);
                if(open_flags&0x2000U)detail+=(open_flags&0x1000U)?" type_id="+std::to_string(type):" type="+(type?utf16_to_utf8(string(type)):std::string("null"));
                if((open_flags&0x200U) && element)detail+=" element="+utf16_to_utf8(string(element));
            }
            std::fprintf(stderr,"[mci] %.*s device=%llu message=0x%x flags=0x%x error=%u%s\n",static_cast<int>(name.size()),name.data(),static_cast<unsigned long long>(arg(0)),msg,open_flags,error.code,detail.c_str());
        }catch(...){}
        return finish(name.starts_with("mciGet") || (installable && message<0x800)?0:error.code);}
    catch(const MemoryFault&){return finish(name.starts_with("mciGet") || (installable && message<0x800)?0:mci::missing_parameter);}
    catch(const std::bad_alloc&){return finish(name.starts_with("mciGet") || (installable && message<0x800)?0:mci::memory);}
}
// mciSendStringA／W 與 mciExecute：解析 MCI 命令字串（不分大小寫、雙引號字串、「裝置類型!元素」），組成與 mciSendCommandW 相同的
// pack(1) 參數區塊並呼叫它，wait、notify、錯誤碼與裝置狀態都沿用命令介面；結果依命令表轉成字串（整數、true／false、模式、時間格式、
// 格式標記與裝置類型名稱）。不是已開啟的別名／檔名時，除 open／close／sysinfo 外自動開啟，命令結束後關閉；非同步播放／錄音則在完成後
// 自動關閉；自動開啟不可搭配 notify。A 版與 mciSendCommandA 相同以 UTF-8 轉換（引擎的 ACP 固定為 65001）。
namespace {
namespace mcis {
constexpr std::uint32_t unrecognized_keyword=259,invalid_device_name=263,missing_command=267,param_overflow=268,bad_integer=270,
    missing_parameter=273,type_required=287,bad_constant=290,missing_device=292,no_closing_quote=294,new_requires_alias=299,notify_auto_open=300;
struct DeviceType {std::u16string_view name;std::uint32_t type;};
constexpr DeviceType device_types[]{{u"vcr",513},{u"videodisc",514},{u"overlay",515},{u"cdaudio",516},{u"dat",517},{u"scanner",518},
    {u"animation",519},{u"digitalvideo",520},{u"avivideo",520},{u"mpegvideo",520},{u"other",521},{u"waveaudio",522},{u"sequencer",523}};
std::uint32_t device_type(std::u16string_view name){for(const auto& item:device_types)if(item.name==name)return item.type;return 0;}
enum class Kind {Integer,Boolean,Mode,TimeFormat,FormatTag,DeviceType,Smpte,Msf,CdTrackType,SequencerDivision,SequencerPort,SequencerSync};
struct Item {std::array<std::u16string_view,3> words;std::uint32_t item;Kind kind;std::uint32_t flags{};};
constexpr Item status_items[]{
    {{u"number",u"of",u"tracks"},3,Kind::Integer},{{u"media",u"present"},5,Kind::Boolean},{{u"time",u"format"},6,Kind::TimeFormat},
    {{u"current",u"track"},8,Kind::Integer},{{u"start",u"position"},2,Kind::Integer,0x200},{{u"format",u"tag"},0x4001,Kind::FormatTag},
    {{u"length"},1,Kind::Integer},{{u"position"},2,Kind::Integer},{{u"mode"},4,Kind::Mode},{{u"ready"},7,Kind::Boolean},
    {{u"channels"},0x4002,Kind::Integer},{{u"samplespersec"},0x4003,Kind::Integer},{{u"bytespersec"},0x4004,Kind::Integer},
    {{u"alignment"},0x4005,Kind::Integer},{{u"bitspersample"},0x4006,Kind::Integer},{{u"level"},0x4007,Kind::Integer},
    {{u"input"},0x400000,Kind::Integer},{{u"output"},0x800000,Kind::Integer}};
constexpr Item sequencer_status_items[]{
    {{u"tempo"},0x4002,Kind::Integer},{{u"port"},0x4003,Kind::SequencerPort},
    {{u"slave"},0x4007,Kind::SequencerSync},{{u"master"},0x4008,Kind::SequencerSync},
    {{u"offset"},0x4009,Kind::Smpte},{{u"division",u"type"},0x400a,Kind::SequencerDivision}};
constexpr Item capability_items[]{
    {{u"can",u"record"},1,Kind::Boolean},{{u"has",u"audio"},2,Kind::Boolean},{{u"has",u"video"},3,Kind::Boolean},{{u"device",u"type"},4,Kind::DeviceType},
    {{u"uses",u"files"},5,Kind::Boolean},{{u"compound",u"device"},6,Kind::Boolean},{{u"can",u"eject"},7,Kind::Boolean},{{u"can",u"play"},8,Kind::Boolean},
    {{u"can",u"save"},9,Kind::Boolean},{{u"inputs"},0x4001,Kind::Integer},{{u"outputs"},0x4002,Kind::Integer}};
std::u16string decimal(std::uint64_t value){const auto text=std::to_string(value);return std::u16string(text.begin(),text.end());}
std::uint32_t parse_number(std::u16string_view value){
    if(value.empty() || value.size()>10)throw mci::Error(bad_integer);
    std::uint64_t n=0;
    for(const auto c:value){if(c<u'0' || c>u'9')throw mci::Error(bad_integer);n=n*10+(c-u'0');}
    if(n>UINT32_MAX)throw mci::Error(bad_integer);
    return static_cast<std::uint32_t>(n);
}
std::uint32_t parse_smpte(std::u16string_view value){
    if(value.find(u':')==std::u16string_view::npos)return parse_number(value);
    std::uint32_t packed=0;
    for(unsigned offset=0;offset<32;offset+=8){
        const auto end=value.find(u':');
        if(offset==24 && end!=std::u16string_view::npos)throw mci::Error(bad_integer);
        const auto part=parse_number(value.substr(0,end));
        if(part>255)throw mci::Error(bad_integer);
        packed|=part<<offset;
        if(end==std::u16string_view::npos)break;
        value.remove_prefix(end+1);
    }
    return packed;
}
constexpr std::u16string_view smpte_formats[]{u"smpte 24",u"smpte 25",u"smpte 30",u"smpte 30 drop"};
std::u16string format_value(Kind kind,std::uint64_t value){
    const auto dword=static_cast<std::uint32_t>(value);
    switch(kind){
    case Kind::Boolean:return dword?u"true":u"false";
    case Kind::Mode:{static constexpr std::u16string_view modes[]{u"not ready",u"stopped",u"playing",u"recording",u"seeking",u"paused",u"open"};
        return dword>=524 && dword<=530?std::u16string(modes[dword-524]):decimal(dword);}
    case Kind::TimeFormat:
        if(dword>=mci::seq_smpte_24 && dword<=mci::seq_smpte_drop)return std::u16string(smpte_formats[dword-mci::seq_smpte_24]);
        if(dword==mci::seq_song_pointer)return u"song pointer";
        if(dword==mci::cd_msf)return u"msf";
        if(dword==mci::cd_tmsf)return u"tmsf";
        return dword==0?u"milliseconds":dword==3?u"frames":dword==8?u"bytes":dword==9?u"samples":decimal(dword);
    case Kind::FormatTag:return dword==1?u"pcm":decimal(dword);
    case Kind::DeviceType:for(const auto& item:device_types)if(item.type==dword)return std::u16string(item.name);return decimal(dword);
    case Kind::Smpte:case Kind::Msf:{
        std::u16string text;
        const unsigned size=kind==Kind::Smpte?32:24;
        for(unsigned shift=0;shift<size;shift+=8){
            const auto part=(dword>>shift)&255U;
            if(shift)text.push_back(u':');
            if(part<10)text.push_back(u'0');
            text+=decimal(part);
        }
        return text;
    }
    case Kind::CdTrackType:return dword==mci::cd_track_audio?u"audio":u"other";
    case Kind::SequencerDivision:{
        constexpr std::u16string_view names[]{u"ppqn",u"smpte 24",u"smpte 25",u"smpte 30 drop",u"smpte 30"};
        return dword>=mci::seq_division_base && dword<mci::seq_division_base+std::size(names)?
            std::u16string(names[dword-mci::seq_division_base]):decimal(dword);
    }
    case Kind::SequencerPort:return dword==mci::seq_mapper_port?u"mapper":dword==mci::seq_no_port?u"none":decimal(dword);
    case Kind::SequencerSync:return dword==0x4002?u"file":dword==mci::seq_no_port?u"none":decimal(dword);
    default:return decimal(dword);
    }
}
std::vector<std::u16string> tokenize(std::u16string_view text){
    std::vector<std::u16string> tokens;std::size_t at=0;
    const auto space=[](char16_t c){return c==u' ' || c==u'\t' || c==u'\r' || c==u'\n';};
    while(at<text.size()){
        if(space(text[at])){++at;continue;}
        if(text[at]==u'"'){const auto end=text.find(u'"',at+1);if(end==std::u16string_view::npos)throw mci::Error(no_closing_quote);tokens.emplace_back(text.substr(at+1,end-at-1));at=end+1;continue;}
        const auto begin=at;while(at<text.size() && !space(text[at]))++at;tokens.emplace_back(text.substr(begin,at-begin));
    }
    return tokens;
}
}
}
std::optional<std::uint32_t> WindowsServices::invoke_mci_string(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"MCI 命令字串",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const bool execute=service.symbol=="mciExecute",wide=service.symbol.ends_with('W');
        const unsigned p=x86_?4:8;
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw mci::Error(mci::memory);m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto put_string=[&](std::u16string_view text){const auto bytes=utf16_bytes(text);const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;};
        const auto command=[&](std::uint32_t id,std::uint32_t message,std::uint64_t flags,std::uint64_t block)->service_callback::Task<std::uint32_t>{
            co_return static_cast<std::uint32_t>(co_await service_callback::call_value(resolve("winmm.dll","mciSendCommandW").value,{id,message,flags,block}));
        };
        std::u16string result;std::uint32_t error=0,automatic=0;bool asynchronous=false;
        try{
            if(!call(0))throw mci::Error(mcis::missing_command);
            const auto text=wide?read_utf16(m,GuestAddress{call(0)},32768):ansi_to_utf16_lossy(read_ansi(m,call(0),32768));
            const auto tokens=mcis::tokenize(text);if(tokens.empty())throw mci::Error(mcis::missing_command);
            const auto verb=fold(tokens[0]);if(tokens.size()<2)throw mci::Error(mcis::missing_device);
            const auto& name=tokens[1];const auto folded=fold(name);
            std::size_t at=2;std::uint64_t flags=0;const auto block=scratch(128);
            const auto peek=[&](std::size_t k){return at+k<tokens.size()?fold(tokens[at+k]):std::u16string{};};
            const auto take=[&](std::initializer_list<std::u16string_view> words){std::size_t k=0;for(const auto word:words){if(word.empty())break;if(peek(k)!=word)return false;++k;}at+=k;return k!=0;};
            const auto word=[&]()->std::u16string{if(at>=tokens.size())throw mci::Error(mcis::missing_parameter);return tokens[at++];};
            const auto number=[&](){return mcis::parse_number(word());};
            const auto common=[&]{if(take({u"notify"})){flags|=mci::notify;return true;}if(take({u"wait"})){flags|=mci::wait;return true;}if(take({u"test"})){flags|=0x20;return true;}return false;};
            const auto store=[&](unsigned offset,unsigned bits,std::uint64_t value){m.store(GuestAddress{block+offset},static_cast<std::uint8_t>(bits),value);};
            const auto finish_flags=[&]{if((flags&mci::notify) && !execute)store(0,p*8,call(3));};
            // 開啟參數：「類型!元素」、已知裝置類型名稱、其餘當作元素（檔案）；new 代表沒有元素的新裝置。
            const auto open_target=[&](std::u16string_view target,std::u16string type,std::uint64_t open_block,std::uint64_t& open_flags){
                std::u16string element;const auto bang=target.find(u'!');const auto lower=fold(std::u16string(target));
                if(bang!=std::u16string_view::npos){type=target.substr(0,bang);element=target.substr(bang+1);}
                else if(!type.empty()){if(lower!=u"new")element=target;}
                else if(mcis::device_type(lower))type=target;
                else if(lower==u"new")throw mci::Error(mcis::type_required);
                else element=target;
                if(!type.empty()){open_flags|=0x2000;m.store(GuestAddress{open_block+p+4},static_cast<std::uint8_t>(p*8),put_string(type));}
                if(!element.empty()){open_flags|=0x200;m.store(GuestAddress{open_block+2*p+4},static_cast<std::uint8_t>(p*8),put_string(element));}
            };
            if(verb==u"open"){
                std::u16string type,alias;
                while(at<tokens.size()){
                    if(common())continue;
                    if(take({u"type"}))type=word();else if(take({u"alias"}))alias=word();else if(take({u"shareable"}))flags|=0x100;
                    else if(take({u"buffer"})){flags|=0x10000;store(4*p+4,32,number());}
                    else throw mci::Error(mcis::unrecognized_keyword);
                }
                if(folded==u"new" && alias.empty())throw mci::Error(mcis::new_requires_alias);
                open_target(name,type,block,flags);
                if(!alias.empty()){flags|=0x400;store(3*p+4,p*8,put_string(alias));}
                finish_flags();
                if(const auto code=co_await command(0,mci::open,flags,block))throw mci::Error(code);
                result=mcis::decimal(m.load(GuestAddress{block+p},32));
            }else if(verb==u"sysinfo"){
                std::uint32_t id=0,type=0;
                if(folded==u"all")id=mci::all_devices;else if(const auto known=mcis::device_type(folded))type=known;
                else if(!(id=find_device(mci_state(),name)))throw mci::Error(mcis::invalid_device_name);
                bool quantity=false;
                while(at<tokens.size()){
                    if(common())continue;
                    if(take({u"quantity"})){flags|=0x100;quantity=true;}else if(take({u"open"}))flags|=0x200;
                    else if(take({u"name"})){flags|=0x400;store(2*p+4,32,number());}else if(take({u"installname"}))flags|=0x800;
                    else throw mci::Error(mcis::unrecognized_keyword);
                }
                const auto output=scratch(2048);store(p,p*8,output);store(2*p,32,1024);store(2*p+8,32,type);finish_flags();
                if(const auto code=co_await command(id,mci::sysinfo,flags,block))throw mci::Error(code);
                result=quantity?mcis::decimal(m.load(GuestAddress{output},32)):read_utf16(m,GuestAddress{output},1024);
            }else{
                std::uint32_t id=folded==u"all"?mci::all_devices:find_device(mci_state(),name);
                if(!id){
                    if(verb==u"close")throw mci::Error(mcis::invalid_device_name);
                    for(std::size_t i=2;i<tokens.size();++i)if(fold(tokens[i])==u"notify")throw mci::Error(mcis::notify_auto_open);
                    const auto open_block=scratch(64);std::uint64_t open_flags=0;open_target(name,{},open_block,open_flags);
                    if(const auto code=co_await command(0,mci::open,open_flags,open_block))throw mci::Error(code);
                    id=automatic=static_cast<std::uint32_t>(m.load(GuestAddress{open_block+p},32));
                }
                std::uint32_t message=0;std::optional<mcis::Kind> kind;std::uint64_t output=0;
                const auto& sequences=mci_state().sequences;
                const auto sequence=sequences.find(id);
                const bool sequencer=sequence!=sequences.end();
                const bool smpte=sequencer && sequence->second->format>=mci::seq_smpte_24 &&
                    sequence->second->format<=mci::seq_smpte_drop;
                const auto& discs=mci_state().compact_discs;
                const auto cd=discs.find(id);
                const bool compact_disc=cd!=discs.end();
                const auto cd_format=compact_disc?cd->second->format:0;
                const auto position=[&](){return smpte || cd_format?mcis::parse_smpte(word()):number();};
                const auto range=[&]{
                    if(take({u"from"})){flags|=mci::from;store(p,32,position());return true;}
                    if(take({u"to"})){flags|=mci::to;store(p+4,32,position());return true;}
                    return false;
                };
                const auto match_item=[&](std::span<const mcis::Item> items)->const mcis::Item*{
                    for(const auto& entry:items)if(take({entry.words[0],entry.words[1],entry.words[2]})){flags|=entry.flags;return &entry;}
                    return nullptr;
                };
                const auto item=[&](std::span<const mcis::Item> items){
                    if(const auto* entry=match_item(items))return entry;
                    throw mci::Error(at<tokens.size()?mcis::unrecognized_keyword:mcis::missing_parameter);
                };
                if(verb==u"close" || verb==u"stop" || verb==u"pause" || verb==u"resume"){
                    message=verb==u"close"?mci::close:verb==u"stop"?mci::stop:verb==u"pause"?mci::pause:mci::resume;
                    while(at<tokens.size())if(!common())throw mci::Error(mcis::unrecognized_keyword);
                }else if(verb==u"play" || verb==u"record" || verb==u"delete"){
                    message=verb==u"play"?mci::play:verb==u"record"?mci::record:mci::erase;
                    while(at<tokens.size()){
                        if(common() || range())continue;
                        if(verb==u"record" && take({u"insert"}))flags|=0x100;else if(verb==u"record" && take({u"overwrite"}))flags|=0x200;
                        else throw mci::Error(mcis::unrecognized_keyword);
                    }
                    asynchronous=message!=mci::erase && !(flags&mci::wait);
                }else if(verb==u"seek"){
                    message=mci::seek;
                    while(at<tokens.size()){
                        if(common())continue;
                        if(take({u"to",u"start"}))flags|=0x100;else if(take({u"to",u"end"}))flags|=0x200;else if(take({u"to"})){flags|=mci::to;store(p,32,position());}
                        else throw mci::Error(mcis::unrecognized_keyword);
                    }
                }else if(verb==u"cue"){
                    message=mci::cue;
                    while(at<tokens.size()){if(common())continue;if(take({u"input"}))flags|=0x400000;else if(take({u"output"}))flags|=0x800000;else throw mci::Error(mcis::unrecognized_keyword);}
                }else if(verb==u"save" || verb==u"load"){
                    message=verb==u"save"?mci::save:mci::load;
                    if(at<tokens.size() && peek(0)!=u"notify" && peek(0)!=u"wait"){flags|=0x100;store(p,p*8,put_string(tokens[at++]));}
                    while(at<tokens.size())if(!common())throw mci::Error(mcis::unrecognized_keyword);
                }else if(verb==u"info"){
                    message=mci::info;
                    while(at<tokens.size()){
                        if(common())continue;
                        if(take({u"product"}))flags|=0x100;else if(take({u"file"}))flags|=0x200;
                        else if(sequencer && take({u"name"}))flags|=0x1000;
                        else if(sequencer && take({u"copyright"}))flags|=0x2000;
                        else if(compact_disc && take({u"upc"}))flags|=0x400;
                        else if(compact_disc && take({u"identity"}))flags|=0x800;
                        else throw mci::Error(mcis::unrecognized_keyword);
                    }
                    output=scratch(2048);store(p,p*8,output);store(2*p,32,1024);
                }else if(verb==u"status"){
                    message=mci::status;flags|=0x100;
                    constexpr std::array cd_items{mcis::Item{{u"type",u"track"},mci::cd_track_type,mcis::Kind::CdTrackType,0x10}};
                    const auto* entry=sequencer?match_item(mcis::sequencer_status_items):nullptr;
                    if(compact_disc)entry=match_item(cd_items);
                    if(!entry)entry=item(mcis::status_items);
                    store(2*p,32,entry->item);
                    kind=smpte && (entry->item==1 || entry->item==2)?mcis::Kind::Smpte:entry->kind;
                    if(cd_format && (entry->item==1 || entry->item==2))
                        kind=entry->item==2 && cd_format==mci::cd_tmsf?mcis::Kind::Smpte:mcis::Kind::Msf;
                    if(compact_disc && entry->item==mci::cd_track_type)store(2*p+4,32,number());
                    while(at<tokens.size()){
                        if(common())continue;
                        if(take({u"track"})){flags|=0x10;store(2*p+4,32,number());}
                        else if(entry->item==2 && take({u"start"}))flags|=0x200;
                        else throw mci::Error(mcis::unrecognized_keyword);
                    }
                }else if(verb==u"capability"){
                    message=mci::getcaps;flags|=0x100;const auto* entry=item(mcis::capability_items);store(p+4,32,entry->item);kind=entry->kind;
                    while(at<tokens.size())if(!common())throw mci::Error(mcis::unrecognized_keyword);
                }else if(verb==u"set"){
                    message=mci::set;
                    const bool video=mci_state().videos.contains(id);
                    while(at<tokens.size()){
                        if(common())continue;
                        if(take({u"time",u"format"})){const auto value=fold(word());flags|=0x400;
                            if(value==u"milliseconds" || value==u"ms")store(p,32,0);
                            else if(compact_disc && value==u"msf")store(p,32,mci::cd_msf);
                            else if(compact_disc && value==u"tmsf")store(p,32,mci::cd_tmsf);
                            else if(video && value==u"frames")store(p,32,3);
                            else if(!video && !sequencer && value==u"bytes")store(p,32,8);
                            else if(!video && !sequencer && value==u"samples")store(p,32,9);
                            else if(sequencer && value==u"song" && take({u"pointer"}))store(p,32,mci::seq_song_pointer);
                            else if(sequencer && value==u"smpte"){
                                const auto rate=number();
                                const auto drop=rate==30 && take({u"drop"});
                                if(rate!=24 && rate!=25 && rate!=30)throw mci::Error(mcis::bad_constant);
                                store(p,32,rate==24?mci::seq_smpte_24:rate==25?mci::seq_smpte_25:drop?mci::seq_smpte_drop:mci::seq_smpte_30);
                            }
                            else throw mci::Error(mcis::bad_constant);}
                        else if(take({u"audio"})){const auto channel=fold(word());flags|=0x800;
                            store(p+4,32,channel==u"all"?0U:channel==u"left"?1U:channel==u"right"?2U:throw mci::Error(mcis::bad_constant));
                            const auto state_word=fold(word());flags|=state_word==u"on"?0x2000U:state_word==u"off"?0x4000U:throw mci::Error(mcis::bad_constant);}
                        else if(compact_disc && take({u"door",u"open"}))flags|=0x100;
                        else if(compact_disc && take({u"door",u"closed"}))flags|=0x200;
                        else if(sequencer && take({u"tempo"})){flags|=mci::seq_tempo;store(p+8,32,number());}
                        else if(sequencer && take({u"offset"})){flags|=mci::seq_offset;store(p+24,32,mcis::parse_smpte(word()));}
                        else if(sequencer && (peek(0)==u"master" || peek(0)==u"slave")){
                            const bool master=fold(word())==u"master";
                            const auto value=fold(word());
                            const unsigned sync=value==u"none"?mci::seq_no_port:value==u"midi"?0x4003:
                                value==u"smpte"?0x4004:!master && value==u"file"?0x4002:0;
                            if(!sync)throw mci::Error(mcis::bad_constant);
                            flags|=master?mci::seq_master:mci::seq_slave;
                            store(p+(master?20:16),32,sync);
                        }
                        else if(sequencer && take({u"port"})){
                            flags|=mci::seq_port;
                            const auto value=fold(word());
                            store(p+12,32,value==u"mapper"?mci::seq_mapper_port:value==u"none"?mci::seq_no_port:mcis::parse_number(value));
                        }
                        else if(take({u"any",u"input"}))flags|=0x4000000;else if(take({u"any",u"output"}))flags|=0x8000000;
                        else if(take({u"format",u"tag"})){flags|=0x10000;const auto tag=peek(0)==u"pcm"?(++at,1U):number();store(p+16,16,tag);}
                        else if(take({u"channels"})){flags|=0x20000;store(p+20,16,number());}
                        else if(take({u"samplespersec"})){flags|=0x40000;store(p+24,32,number());}
                        else if(take({u"bytespersec"})){flags|=0x80000;store(p+28,32,number());}
                        else if(take({u"alignment"})){flags|=0x100000;store(p+32,16,number());}
                        else if(take({u"bitspersample"})){flags|=0x200000;store(p+36,16,number());}
                        else if(take({u"input"})){flags|=0x400000;store(p+8,32,number());}
                        else if(take({u"output"})){flags|=0x800000;store(p+12,32,number());}
                        else throw mci::Error(mcis::unrecognized_keyword);
                    }
                }else throw mci::Error(mci::unrecognized);
                finish_flags();
                if(const auto code=co_await command(id,message,flags,block))throw mci::Error(code);
                if(kind)result=mcis::format_value(*kind,m.load(GuestAddress{block+p},static_cast<std::uint8_t>(message==mci::status?p*8:32)));
                else if(output)result=read_utf16(m,GuestAddress{output},1024);
            }
        }catch(const mci::Error& failure){error=failure.code;}catch(const MemoryFault&){error=mci::missing_parameter;}
        if(automatic){
            auto& bridge=mci_state();
            const auto close_immediately=[&](const auto& devices){
                const auto it=devices.find(automatic);
                if(it==devices.end())return false;
                if(error || !asynchronous || it->second->mode==mci::Mode::Stopped)return true;
                it->second->auto_close=true;
                return false;
            };
            if(close_immediately(bridge.devices) || close_immediately(bridge.videos) || close_immediately(bridge.sequences) ||
                close_immediately(bridge.compact_discs)){
                const auto close_block=scratch(16);
                (void)co_await command(automatic,mci::close,0,close_block);
            }
        }
        if(execute){
            if(!error)co_return finish(1);
            auto message=std::u16string(error_text(error));if(message.empty())message=u"Unknown MCI error";
            (void)co_await service_callback::call_value(resolve("user32.dll","MessageBoxW").value,{0,put_string(message),put_string(u"MCI"),0x10});
            co_return finish(0);
        }
        if(!error && call(1)){
            const auto capacity=static_cast<std::uint32_t>(call(2));
            if(wide){if(capacity<result.size()+1)error=mcis::param_overflow;else m.write(GuestAddress{call(1)},utf16_bytes(result));}
            else{const auto encoded=utf16_to_ansi(result);if(capacity<encoded.size()+1)error=mcis::param_overflow;
                else {m.write(GuestAddress{call(1)},std::vector<std::uint8_t>(encoded.begin(),encoded.end()));m.store(GuestAddress{call(1)+encoded.size()},8,0);}}
        }
        co_return finish(error);
    });
}
}
