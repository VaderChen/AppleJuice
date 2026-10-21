#include "x64arm/windows_services.hpp"
#include "x64arm/audio.hpp"
#include "x64arm/unicode.hpp"
#include "pcm_wave_format.hpp"
#include "pcm_wave_file.hpp"
#include "wave_device_message.hpp"
#include "audio_endpoints.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <numbers>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
struct WinmmBeepTone {
    std::shared_ptr<audio::Buffer> buffer;
    std::unique_ptr<audio::Output> output;
    std::chrono::steady_clock::time_point deadline;
    bool finished{},waveform{},asynchronous{};
};
void WindowsServices::discard_beep(std::uint64_t handle){
    if(const auto found=beep_tones_.find(handle);found!=beep_tones_.end()){
        {std::lock_guard lock(found->second->buffer->mutex);found->second->buffer->stop();}
        beep_tones_.erase(found);
    }
    if(play_sound_handle_==handle)play_sound_handle_=0;
    runtime_.wait_objects.erase(handle);
}
std::optional<std::uint32_t> WindowsServices::finish_beep(GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint32_t status){
    if(status==0xc0U){
        const auto tone=beep_tones_.find(handle);
        if(tone!=beep_tones_.end() && !tone->second->finished){
            start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,UINT32_MAX,1}}},Continuation::BeepTone,handle);return std::nullopt;
        }
        status=0;
    }
    discard_beep(handle);complete_call(state,memory,status==0);return std::nullopt;
}
void WindowsServices::stop_play_sound(){
    const auto handle=play_sound_handle_;play_sound_handle_=0;
    const auto found=beep_tones_.find(handle);if(found==beep_tones_.end())return;
    auto& tone=*found->second;{std::lock_guard lock(tone.buffer->mutex);tone.buffer->stop();}
    tone.output.reset();tone.finished=true;
    if(const auto event=runtime_.wait_objects.find(handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);
    // 同步呼叫的 event 由原呼叫執行緒釋放；另一執行緒停止它時仍須正常喚醒。
    if(tone.asynchronous)discard_beep(handle);
}
std::optional<std::uint32_t> WindowsServices::invoke_play_sound(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool legacy=service.symbol.starts_with("snd"),wide=service.symbol.ends_with('W');
    const auto flags=static_cast<std::uint32_t>(arg(legacy?1U:2U));const auto module=legacy?0:arg(1);
    const auto finish=[&](bool result,unsigned error=0){if(error)last_error_=error;arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    constexpr std::uint64_t limit=64U*1024U*1024U;
    // NULL 專用來停止本程序的 PlaySound，不干涉其他程序或 waveOut／Beep。
    if(!arg(0)){stop_play_sound();return finish(true);}
    const auto source=flags&0x70000U;const bool resource=source==0x40000U,alias=source==0x10000U;
    if((flags&~0x3f20dfU) || ((flags&8U) && !(flags&1U)) ||
       (source && source!=0x10000U && source!=0x20000U && source!=0x40000U) ||
       (resource && !(flags&4U)) || (!resource && (flags&4U) && source) ||
       (module && !resource) || ((flags&0x100000U) && !alias))return finish(false,87);
    // 這些旗標另有系統通知／輔助工具契約；尚未接入時明確拒絕。
    if(flags&0x280040U)return finish(false,50);
    if(!runtime_.network.external_io)throw HostCallFault("原生音效播放不支援 deterministic compare");
    poll_wave_output(memory);
    if(play_sound_handle_ && (flags&0x10U))return finish(false);
    stop_play_sound();
    try{
        std::vector<std::uint8_t> bytes;
        const auto lower=[](std::u16string text){for(auto& unit:text)unit=unicode_case_unit(unit,false);return text;};
        const auto read_bytes=[](const GuestMemory& source_memory,std::uint64_t address,std::size_t size){std::vector<std::uint8_t> data(size);source_memory.read(GuestAddress{address},data);return data;};
        const auto file_bytes=[&](std::u16string_view path,std::uint64_t maximum=limit)->std::vector<std::uint8_t>{
            const auto opened=filesystem_.open_reader(utf16_to_utf8(path));
            if(opened.error || !opened.file || (opened.metadata.attributes&16U) || opened.metadata.size>maximum)return {};
            std::vector<std::uint8_t> data(static_cast<std::size_t>(opened.metadata.size));std::uint64_t at=0;
            while(at<data.size()){const auto read=opened.file->read(std::span(data).subspan(static_cast<std::size_t>(at)),at);if(read.error || !read.bytes)return {};at+=read.bytes;}
            return data;
        };
        const auto expand=[&](std::u16string text){std::u16string result;for(std::size_t i=0;i<text.size();){
            const auto end=text[i]==u'%'?text.find(u'%',i+1):text.npos;
            if(end==text.npos){result.push_back(text[i++]);continue;}
            const auto found=runtime_.environment.find(lower(text.substr(i+1,end-i-1)));
            result+=found==runtime_.environment.end()?text.substr(i,end-i+1):found->second.second;i=end+1;
        }return result;};
        const auto wave_file=[&](std::u16string path)->std::vector<std::uint8_t>{
            path=expand(std::move(path));auto data=file_bytes(path);if(!data.empty() || path.find_first_of(u"\\/:")!=path.npos)return data;
            std::vector<std::u16string> directories{u"C:\\Windows",u"C:\\Windows\\System32"};
            if(const auto found=runtime_.environment.find(u"path");found!=runtime_.environment.end()){
                const auto& paths=found->second.second;for(std::size_t at=0;at<paths.size();){const auto end=paths.find(u';',at);auto directory=paths.substr(at,end==paths.npos?paths.size()-at:end-at);if(directory.size()>1 && directory.front()==u'"' && directory.back()==u'"')directory=directory.substr(1,directory.size()-2);if(!directory.empty())directories.push_back(std::move(directory));if(end==paths.npos)break;at=end+1;}}
            for(const auto& directory:directories){data=file_bytes(directory+u"\\"+path);if(!data.empty())return data;}return {};
        };
        const auto alias_path=[&](std::u16string name)->std::u16string{
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return {};
            initialize_tokens();auto sid=lower(utf8_to_utf16(token_user_sid_string(effective_token_id())));
            const auto primary=lower(utf8_to_utf16(token_user_sid_string(1)));std::u16string hive=std::u16string(1,3)+u"\\"+sid;
            if(sid==primary)hive=std::u16string(1,1);else if(!runtime_.registry.keys.contains(hive))hive=std::u16string(1,3)+u"\\.default";
            std::u16string application=u".default";
            if(flags&0x80U){application=runtime_.image.path;const auto at=application.find_last_of(u"\\/");if(at!=application.npos)application.erase(0,at+1);const auto dot=application.find_last_of(u'.');if(dot!=application.npos)application.erase(dot);}
            const auto key=lower(hive+u"\\AppEvents\\Schemes\\Apps\\"+application+u"\\"+name+u"\\.Current");
            const auto found=runtime_.registry.keys.find(key);
            if(found!=runtime_.registry.keys.end() && (registry_security_access(key,1)&1U)){
                const auto value=found->second.find(u"");if(value!=found->second.end() && (value->second.type==1 || value->second.type==2) && !(value->second.bytes.size()&1U)){
                    std::u16string path;const auto& data=value->second.bytes;for(std::size_t i=0;i+1<data.size();i+=2){const auto unit=static_cast<char16_t>(unsigned{data[i]}|(unsigned{data[i+1]}<<8));if(!unit)break;path.push_back(unit);}return expand(std::move(path));}}
            if(flags&0x80U)return {};
            // 舊程式的 [sounds] 設定同樣只從 Sandbox 的 Windows 目錄讀取。
            const auto ini=file_bytes(u"C:\\Windows\\win.ini",65536);std::u16string text;
            if(ini.size()>=2 && ini[0]==0xff && ini[1]==0xfe){for(std::size_t i=2;i+1<ini.size();i+=2)text.push_back(static_cast<char16_t>(unsigned{ini[i]}|(unsigned{ini[i+1]}<<8)));}
            else if(ini.size()>=3 && ini[0]==0xef && ini[1]==0xbb && ini[2]==0xbf)text=utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(ini.data()+3),ini.size()-3));
            else text=cp1252_to_utf16(std::string_view(reinterpret_cast<const char*>(ini.data()),ini.size()));
            const auto trim=[](std::u16string value){const auto first=value.find_first_not_of(u" \t\r"),last=value.find_last_not_of(u" \t\r");return first==value.npos?std::u16string{}:value.substr(first,last-first+1);};
            bool sounds=false;for(std::size_t at=0;at<text.size();){const auto end=text.find(u'\n',at);auto line=trim(text.substr(at,end==text.npos?text.size()-at:end-at));
                if(!line.empty() && line.front()==u'[')sounds=lower(line)==u"[sounds]";
                else if(sounds && !line.empty() && line.front()!=u';'){const auto equal=line.find(u'=');if(equal!=line.npos && lower(trim(line.substr(0,equal)))==lower(name)){auto path=trim(line.substr(equal+1));const auto comma=path.find(u',');if(comma!=path.npos)path=trim(path.substr(0,comma));if(path.size()>1 && path.front()==u'"' && path.back()==u'"')path=path.substr(1,path.size()-2);return expand(std::move(path));}}
                if(end==text.npos)break;at=end+1;}return {};
        };
        if(resource){
            const auto type=allocate_heap(memory,10);if(!type)return finish(false,8);
            memory.write(GuestAddress{type},wide?utf16_bytes(u"WAVE"):std::vector<std::uint8_t>{'W','A','V','E',0});std::uint64_t entry=0;
            try{entry=gui_resource(memory,module,arg(0),type,wide);}catch(...){(void)free_heap(memory,type,state);throw;}(void)free_heap(memory,type,state);
            const auto* image=resource_image(module?module:runtime_.image.base);
            if(entry && image){const auto size=memory.load(GuestAddress{entry+4},32);if(size<=limit)bytes=read_bytes(memory,image->base+memory.load(GuestAddress{entry},32),static_cast<std::size_t>(size));}
        }else if(flags&4U){
            const auto header=read_bytes(memory,arg(0),12);const auto size=std::uint64_t{riff::word(std::span(header).subspan(4))}+8;
            if(riff::word(header)==riff::riff_id && size>=12 && size<=limit)bytes=read_bytes(memory,arg(0),static_cast<std::size_t>(size));
        }else{
            std::u16string name;
            if(flags&0x100000U){switch(arg(0)){case 0x2a53:name=u"SystemAsterisk";break;case 0x2153:name=u"SystemExclamation";break;case 0x6453:name=u"SystemDefault";break;case 0x4553:name=u"SystemExit";break;case 0x4853:name=u"SystemHand";break;case 0x3f53:name=u"SystemQuestion";break;case 0x5353:name=u"SystemStart";break;case 0x5753:name=u"SystemWelcome";break;default:return finish(false,87);}}
            else {name=wide?read_utf16(memory,GuestAddress{arg(0)},256):utf8_to_utf16_lossy(read_ansi(memory,arg(0),256));if(name.size()>=256)return finish(false,87);}
            if(source!=0x20000U){const auto path=alias_path(name);if(!path.empty())bytes=wave_file(path);}
            if(!alias && bytes.empty())bytes=wave_file(name);
        }
        auto wave=parse_pcm_wave_file(bytes);
        if(!wave && !(flags&2U) && !(flags&4U)){const auto path=alias_path(u"SystemDefault");if(!path.empty())bytes=wave_file(path);wave=parse_pcm_wave_file(bytes);}
        if(!wave)return finish(false,2);
        if(beep_tones_.size()>=64)return finish(false,8);
        auto tone=std::make_shared<WinmmBeepTone>();tone->buffer=std::make_shared<audio::Buffer>();auto& buffer=*tone->buffer;
        buffer.channels=wave->format.channels;buffer.bits=wave->format.bits;buffer.rate=buffer.frequency=wave->format.rate;buffer.floating=wave->format.floating;
        buffer.pcm.assign(wave->samples.begin(),wave->samples.end());buffer.looping=(flags&8U)!=0;buffer.playing=true;
        tone->waveform=true;tone->asynchronous=(flags&1U)!=0;
        try{tone->output=audio::open_output(tone->buffer);}catch(const std::runtime_error&){return finish(false,50);}if(!tone->output)return finish(false,50);
        const auto handle=runtime_.allocate_wait_handle();WaitObject event;event.kind=WaitObject::Kind::Event;event.manual_reset=true;event.access=0x100000U;
        runtime_.wait_objects.emplace(handle,std::move(event));beep_tones_.emplace(handle,std::move(tone));play_sound_handle_=handle;
        if(std::getenv("X64ARM_TRACE_AUDIO"))std::fprintf(stderr,"[playsound] handle=%llu channels=%u bits=%u rate=%u bytes=%zu async=%u loop=%u\n",static_cast<unsigned long long>(handle),buffer.channels,buffer.bits,buffer.rate,buffer.pcm.size(),(flags&1U)!=0,(flags&8U)!=0);
        if(flags&1U)return finish(true);
        try{start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,UINT32_MAX,1}}},Continuation::BeepTone,handle);}catch(...){discard_beep(handle);throw;}return std::nullopt;
    }catch(const std::bad_alloc&){return finish(false,8);}catch(const MemoryFault&){return finish(false,87);}
}
struct WinmmWaveOutput {
    std::shared_ptr<audio::Buffer> buffer;
    std::unique_ptr<audio::Output> output;
    std::uint64_t callback{},target{},instance{},completed_bytes{};
    std::uint32_t callback_type{},volume{UINT32_MAX},device{};bool paused{};
    struct Header {std::uint64_t data{};std::uint32_t capacity{};bool queued{};};
    struct Block {std::uint64_t header{},end{};};
    std::map<std::uint64_t,Header> headers;
    std::deque<Block> queue;
};
namespace {
std::optional<GuestCallback> notification(const WinmmWaveOutput& output,std::uint64_t handle,unsigned message,std::uint64_t header=0){
    if(!output.callback_type)return {};
    if(output.callback_type==0x30000)return GuestCallback{output.target,{handle,message,output.instance,header,0}};
    if(output.callback_type==0x10000 || output.callback_type==0x20000)return GuestCallback{output.target,{output.callback,message,handle,header}};
    if(message==0x3bd)return GuestCallback{output.target,{output.callback}};
    return {};
}
}
bool WindowsServices::wave_output_waiting()const{
    if(mci_waiting())return true;
    for(const auto& [handle,tone]:beep_tones_){(void)handle;if(!tone->finished)return true;}
    if(!wave_callbacks_.empty())return true;
    if(wave_input_waiting())return true;
    for(const auto& [handle,output]:wave_outputs_){(void)handle;if(!output->paused && !output->queue.empty())return true;}return false;
}
void WindowsServices::poll_wave_output(GuestMemory& memory){
    poll_mci();
    const auto now=std::chrono::steady_clock::now();
    for(auto it=beep_tones_.begin();it!=beep_tones_.end();){
        const auto handle=it->first;auto& tone=it->second;bool done=tone->finished;
        if(!done){std::lock_guard lock(tone->buffer->mutex);done=tone->waveform?!tone->buffer->playing:now>=tone->deadline;if(done)tone->buffer->stop();}
        if(done && !tone->finished){tone->output.reset();tone->finished=true;
            if(const auto event=runtime_.wait_objects.find(handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);}
        if(done && tone->asynchronous){++it;discard_beep(handle);}else ++it;
    }
    poll_winmm_mixer();
    const unsigned offset=x86_?16:24;
    for(auto& [handle,output]:wave_outputs_){
        if(output->queue.empty())continue;
        std::lock_guard lock(output->buffer->mutex);auto& buffer=*output->buffer;
        const auto consumed=buffer.playing || output->paused?static_cast<std::uint64_t>(buffer.cursor)*buffer.frame_bytes():buffer.pcm.size();
        std::uint64_t removed=0;
        while(!output->queue.empty() && output->queue.front().end<=consumed){
            const auto block=output->queue.front();const auto flags=memory.load(GuestAddress{block.header+offset},32);
            memory.store(GuestAddress{block.header+offset},32,(flags&~0x10U)|1U);output->headers.at(block.header).queued=false;
            if(auto callback=notification(*output,handle,0x3bd,block.header))wave_callbacks_.push_back(std::move(*callback));
            removed=block.end;output->queue.pop_front();
        }
        if(removed){
            buffer.pcm.erase(buffer.pcm.begin(),buffer.pcm.begin()+static_cast<std::ptrdiff_t>(removed));
            buffer.cursor=buffer.playing || output->paused?std::max(0.0,buffer.cursor-static_cast<double>(removed/buffer.frame_bytes())):0;
            for(auto& block:output->queue)block.end-=removed;output->completed_bytes+=removed;
        }
    }
    if(threads_.items.empty())return;
    const auto saved_error=last_error_;
    if(wave_callback_registration_ && !registered_waits_.contains(wave_callback_registration_))wave_callback_registration_=0;
    while(!wave_callback_registration_ && !wave_callbacks_.empty() && registered_waits_.size()<64){
        const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));if(!worker)break;
        RegisteredWait work;work.worker=worker;
        // 同一通知派送器依完成順序執行；避免不同 worker 重排 WOM_DONE／CLOSE。
        const auto count=std::min<std::size_t>(64,wave_callbacks_.size());
        for(std::size_t i=0;i<count;++i){work.callback_batch.push_back(std::move(wave_callbacks_.front()));wave_callbacks_.pop_front();}
        work.armed=false;work.ready=true;work.closing=true;work.work_item=true;
        const auto id=next_registered_wait_++;registered_waits_.emplace(id,std::move(work));wait_workers_[worker]=id;wave_callback_registration_=id;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
    }
    last_error_=saved_error;
}
std::optional<std::uint32_t> WindowsServices::invoke_wave_output(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    if(name=="Beep"){
        const auto finish=[&](bool result,unsigned error=0){if(error)last_error_=error;arg.return_to(state,result);return std::optional<std::uint32_t>{};};
        const auto frequency=d(0),duration=d(1);if(frequency<37 || frequency>32767)return finish(false,87);if(!duration)return finish(true);
        if(!runtime_.network.external_io)throw HostCallFault("原生音訊輸出不支援 deterministic compare");
        if(beep_tones_.size()>=64)return finish(false,8);if(!audio::output_available())return finish(false,50);
        std::uint64_t handle=0;
        try{
            auto tone=std::make_shared<WinmmBeepTone>();tone->buffer=std::make_shared<audio::Buffer>();auto& buffer=*tone->buffer;
            constexpr unsigned rate=96000;buffer.rate=buffer.frequency=rate;buffer.bits=16;buffer.channels=2;
            // 以最短整數週期循環，不隨 DWORD duration 配置巨型 PCM；左右聲道均保留。
            const auto frames=rate/std::gcd(rate,frequency);buffer.pcm.resize(static_cast<std::size_t>(frames)*4);
            for(unsigned i=0;i<frames;++i){const auto sample=static_cast<std::int16_t>(std::lround(std::sin(2*std::numbers::pi*frequency*i/rate)*9830));
                const auto value=static_cast<std::uint16_t>(sample);for(unsigned channel=0;channel<2;++channel){const auto at=static_cast<std::size_t>(i)*4+channel*2;buffer.pcm[at]=static_cast<std::uint8_t>(value);buffer.pcm[at+1]=static_cast<std::uint8_t>(value>>8);}}
            buffer.looping=true;buffer.playing=true;
            try{tone->output=audio::open_output(tone->buffer);}catch(const std::runtime_error&){return finish(false,50);}if(!tone->output)return finish(false,50);
            tone->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(duration);
            handle=runtime_.next_wait_handle;runtime_.next_wait_handle+=4;WaitObject event;event.kind=WaitObject::Kind::Event;event.manual_reset=true;event.access=0x100000U;
            runtime_.wait_objects.emplace(handle,std::move(event));beep_tones_.emplace(handle,std::move(tone));
            try{start_callbacks(state,memory,{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,UINT32_MAX,1}}},Continuation::BeepTone,handle);}
            catch(...){discard_beep(handle);throw;}return std::nullopt;
        }catch(const std::bad_alloc&){if(handle)discard_beep(handle);return finish(false,8);}
    }
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8,header_size=x86_?32:48,flags_offset=x86_?16:24;
    std::uint64_t result=0;poll_wave_output(memory);
    const auto notify=[&](const WinmmWaveOutput& out,std::uint64_t handle,unsigned message,std::uint64_t header=0){if(auto callback=notification(out,handle,message,header))wave_callbacks_.push_back(std::move(*callback));};
    try {
        // WinMM 裝置編號依共用端點目錄：0 為系統預設輸出，其後為其他 Core Audio 輸出裝置。
        // 只有查詢裝置的入口才列舉 Core Audio；waveOutWrite 等串流呼叫不查詢。
        const bool lists=name=="waveOutMessage" || name=="waveOutGetNumDevs" || name.starts_with("waveOutGetDevCaps") || name=="waveOutOpen";
        const auto endpoints=lists && audio::output_available()?audio_endpoint::render():std::vector<audio_endpoint::Endpoint>{};
        if(name=="waveOutMessage"){std::vector<std::u16string> ids;for(const auto& item:endpoints)ids.push_back(item.id);result=wave::device_message(memory,arg(0),d(1),arg(2),arg(3),ids,wave_outputs_.contains(arg(0)),x86_);}
        else if(name=="waveOutGetNumDevs")result=endpoints.size();
        else if(name.starts_with("waveOutGetDevCaps")){
            const bool mapper=arg(0)==UINT32_MAX || arg(0)==UINT64_MAX;const auto opened=wave_outputs_.find(arg(0));
            const auto index=opened!=wave_outputs_.end()?std::uint64_t{opened->second->device}:arg(0);
            if(!mapper && opened==wave_outputs_.end() && index>=endpoints.size())result=2;
            else if(endpoints.empty() || (!mapper && index>=endpoints.size()))result=2;
            else if(!arg(1) && d(2))result=11;
            else {
                const bool wide=name.ends_with('W');std::vector<std::uint8_t> caps(wide?84:52);
                const auto put=[&](unsigned at,std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)caps[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                // szPname 上限 MAXPNAMELEN（32）含結尾；WAVE_MAPPER 為 Microsoft Sound Mapper。
                const auto& host=endpoints[mapper?0:static_cast<std::size_t>(index)].device;
                auto label=mapper?std::u16string(u"Microsoft Sound Mapper"):host.name.empty()?std::u16string(u"Core Audio"):host.name;if(label.size()>31)label.resize(31);
                std::vector<std::uint8_t> text;if(wide)text=utf16_bytes(label);else {const auto narrow=utf16_to_utf8(label);text.assign(narrow.begin(),narrow.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(narrow.size(),31)));}
                put(0,1,2);put(2,mapper?2:0x64,2);put(4,0x100,4);std::copy(text.begin(),text.end(),caps.begin()+8);const unsigned at=wide?72:40;
                put(at,0xfffff,4);put(at+4,!mapper && host.output_channels==1?1:2,2);put(at+8,0xc,4); // 11／22／44／48／96 kHz PCM；左右獨立音量。
                if(d(2))memory.write(GuestAddress{arg(1)},std::span(caps).first(std::min<std::size_t>(caps.size(),d(2))));
            }
        }else if(name=="waveOutOpen"){
            const auto flags=d(5),kind=flags&0x70000;const bool query=(flags&1)!=0;
            if(!arg(2) || (!query && !arg(0)))result=11;
            else {
                if(arg(0))memory.store(GuestAddress{arg(0)},bits,0);
                const auto format=read_pcm_wave_format(memory,arg(2));
                if(d(1)!=UINT32_MAX && d(1)>=endpoints.size())result=endpoints.empty()?3:2;
                else if((flags&~0x7000fU) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000))result=10;
                else if(!format)result=32;
                else if(!audio::output_available())result=3;
                else if(query)result=0;
                else if((kind && !arg(3)) || (!kind && arg(3)))result=11;
                else if(kind==0x10000 && !gui_.windows.contains(arg(3)))result=11;
                else if(kind==0x20000 && (!thread_from_identifier(static_cast<std::uint32_t>(arg(3))) || threads_.items.at(thread_from_identifier(static_cast<std::uint32_t>(arg(3)))).status==ThreadStatus::Exited))result=11;
                else if(kind==0x50000 && (!runtime_.wait_objects.contains(arg(3)) || runtime_.wait_objects.at(arg(3)).kind!=WaitObject::Kind::Event || !(runtime_.wait_objects.at(arg(3)).access&2U)))result=11;
                else if(wave_outputs_.size()>=64)result=4;
                else {
                    if(kind==0x30000)memory.check(GuestAddress{arg(3)},1,Access::Execute);
                    if(!runtime_.network.external_io)throw HostCallFault("原生 PCM 輸出不支援 deterministic compare");
                    auto out=std::make_shared<WinmmWaveOutput>();out->buffer=std::make_shared<audio::Buffer>();out->buffer->channels=format->channels;out->buffer->bits=format->bits;out->buffer->rate=out->buffer->frequency=format->rate;
                    // WAVE_MAPPER 跟隨系統預設輸出；指定編號綁定該 Core Audio 裝置。
                    out->device=d(1)==UINT32_MAX?0U:d(1);
                    try {out->output=d(1)==UINT32_MAX?audio::open_output(out->buffer):audio::open_output(out->buffer,endpoints[d(1)].device.uid);}catch(const std::runtime_error&){result=3;}
                    if(out->output){
                        out->callback_type=kind;out->callback=arg(3);out->instance=arg(4);out->target=kind==0x30000?arg(3):kind==0x10000?resolve("user32.dll","PostMessageW").value:kind==0x20000?resolve("user32.dll","PostThreadMessageW").value:resolve("kernel32.dll","SetEvent").value;
                        const auto handle=runtime_.allocate_wait_handle();wave_outputs_.emplace(handle,out);memory.store(GuestAddress{arg(0)},bits,handle);notify(*out,handle,0x3bb);
                    }
                }
            }
        }else {
            const auto found=wave_outputs_.find(arg(0));
            if(found==wave_outputs_.end())result=5;
            else {
                const auto out=found->second;auto& buffer=*out->buffer;std::lock_guard lock(buffer.mutex);
                if(name=="waveOutClose"){
                    if(!out->queue.empty())result=33;else {notify(*out,arg(0),0x3bc);wave_outputs_.erase(found);}
                }else if(name=="waveOutGetID"){if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,out->device);}
                else if(name=="waveOutPause"){out->paused=true;buffer.playing=false;}
                else if(name=="waveOutRestart"){out->paused=false;buffer.playing=!out->queue.empty();}
                else if(name=="waveOutReset"){
                    for(const auto& block:out->queue){const auto flags=memory.load(GuestAddress{block.header+flags_offset},32);memory.store(GuestAddress{block.header+flags_offset},32,(flags&~0x10U)|1U);out->headers.at(block.header).queued=false;notify(*out,arg(0),0x3bd,block.header);}
                    out->queue.clear();buffer.pcm.clear();buffer.cursor=0;buffer.playing=false;out->paused=false;out->completed_bytes=0;
                }else if(name=="waveOutSetVolume"){out->volume=d(1);buffer.left=static_cast<float>(d(1)&65535)/65535;buffer.right=static_cast<float>(d(1)>>16)/65535;}
                else if(name=="waveOutGetVolume"){if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,out->volume);}
                else if(name=="waveOutGetPosition"){
                    if(!arg(1) || d(2)<12)result=11;
                    else {memory.check(GuestAddress{arg(1)},12,Access::Write);auto type=memory.load(GuestAddress{arg(1)},32);if(type!=1 && type!=2 && type!=4)type=4;
                        const auto frames=out->completed_bytes/buffer.frame_bytes()+static_cast<std::uint64_t>(buffer.cursor);
                        memory.store(GuestAddress{arg(1)},32,type);memory.store(GuestAddress{arg(1)+4},32,type==1?frames*1000/buffer.rate:type==2?frames:frames*buffer.frame_bytes());}
                }else {
                    const auto header=arg(1);
                    if(!header || d(2)<header_size)result=11;
                    else {
                        memory.check(GuestAddress{header},header_size,Access::Read);memory.check(GuestAddress{header+flags_offset},4,Access::Write);
                        const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{header+flags_offset},32));const auto prepared=out->headers.find(header);
                        if(name=="waveOutUnprepareHeader"){
                            if(prepared!=out->headers.end()){if(prepared->second.queued)result=33;else {out->headers.erase(prepared);memory.store(GuestAddress{header+flags_offset},32,flags&~2U);}}
                        }else if(name=="waveOutPrepareHeader"){
                            if(prepared==out->headers.end()){
                                const auto data=memory.load(GuestAddress{header},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if((!data && size) || size>output_limit || (flags&~0xfU))result=11;
                                else if(out->headers.size()>=1024)result=7;
                                else {if(size)memory.check(GuestAddress{data},size,Access::Read);out->headers.emplace(header,WinmmWaveOutput::Header{data,size,false});memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|2U);}
                            }
                        }else if(name=="waveOutWrite"){
                            if(prepared==out->headers.end() || !(flags&2U))result=34;
                            else if(prepared->second.queued)result=33;
                            else if(flags&12U)result=8; // 多緩衝區 loop 尚未映射，不能忽略 loop 位元。
                            else if(wave_callbacks_.size()>=4096)result=7;
                            else {
                                const auto data=memory.load(GuestAddress{header},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if(data!=prepared->second.data || size>prepared->second.capacity || size%buffer.frame_bytes())result=11;
                                else if(size>output_limit-buffer.pcm.size())result=7;
                                else {std::vector<std::uint8_t> bytes(size);if(size)memory.read(GuestAddress{data},bytes);buffer.pcm.insert(buffer.pcm.end(),bytes.begin(),bytes.end());out->queue.push_back({header,buffer.pcm.size()});prepared->second.queued=true;memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|0x10U);buffer.playing=!out->paused && !buffer.pcm.empty();}
                            }
                        }else throw HostCallFault("未知的 WaveOut 服務");
                    }
                }
            }
        }
    }catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}
    arg.return_to(state,result);return std::nullopt;
}
}
