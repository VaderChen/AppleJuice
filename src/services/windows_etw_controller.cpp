#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_etw_support.hpp"
#include <algorithm>

namespace x64arm {
namespace {
class Properties {
public:
    Properties(GuestMemory& memory,std::uint64_t address,bool x86,bool wide):memory_(memory),address_(address),shape_(etw::guest::layout(x86)),wide_(wide){
        if(!address)throw etw::guest::Fault{87};size_=get(0);if(size_<shape_.properties)throw etw::guest::Fault{24};
        if(size_>1048576)throw etw::guest::Fault{87};memory.check(GuestAddress{address},size_,Access::Read);memory.check(GuestAddress{address},size_,Access::Write);
    }
    std::uint32_t get(unsigned at)const{return static_cast<std::uint32_t>(memory_.load(GuestAddress{address_+at},32));}
    void put(unsigned at,std::uint64_t value,unsigned bits=32){memory_.store(GuestAddress{address_+at},static_cast<std::uint8_t>(bits),value);}
    etw::Guid guid()const{etw::Guid id{};memory_.read(GuestAddress{address_+24},id);return id;}
    std::uint32_t size()const{return size_;}
    std::uint32_t file_offset()const{return get(shape_.property_file);}
    std::uint32_t name_offset()const{return get(shape_.property_name);}
    std::u16string text(std::uint32_t offset)const{
        if(!offset)return {};validate(offset);std::string narrow;std::u16string wide;const auto unit=wide_?2U:1U;
        for(std::uint32_t at=offset;at<=size_-unit;at+=unit){const auto c=memory_.load(GuestAddress{address_+at},static_cast<std::uint8_t>(unit*8));
            if(!c)return wide_?wide:utf8_to_utf16(narrow);if(wide_)wide.push_back(static_cast<char16_t>(c));else narrow.push_back(static_cast<char>(c));}
        throw etw::guest::Fault{87};
    }
    std::uint32_t output(std::uint64_t handle,const etw::Session& session,std::uint32_t minimum,std::uint32_t maximum,std::uint32_t timer,std::uint32_t log_lost,std::size_t used){
        const auto& header=session.options.header;const auto name=bytes(header.session_name),file=bytes(header.file_name);
        const auto name_at=name_offset()?name_offset():shape_.properties;
        const auto file_at=file_offset()?file_offset():static_cast<std::uint32_t>(name_at+name.size());
        if(name_at<shape_.properties || name_at>size_ || (wide_ && name_at%2))throw etw::guest::Fault{87};
        if(!header.file_name.empty() && (file_at<shape_.properties || (wide_ && file_at%2)))throw etw::guest::Fault{87};
        const auto required=std::max(std::uint64_t{name_at}+name.size(),header.file_name.empty()?UINT64_C(0):std::uint64_t{file_at}+file.size());
        if(!header.file_name.empty() && name_at<std::uint64_t{file_at}+file.size() && file_at<std::uint64_t{name_at}+name.size())throw etw::guest::Fault{87};
        put(8,handle,64);memory_.write(GuestAddress{address_+24},session.options.id);put(40,header.clock);put(44,0x20000);
        put(48,header.buffer_size/1024);put(52,minimum);put(56,maximum);put(60,header.maximum_file_size);put(64,header.mode);put(68,timer);put(72,0);
        const auto allocated=std::max(std::uint64_t{minimum},static_cast<std::uint64_t>(used));put(80,allocated);put(84,allocated-used);
        put(88,session.events_lost);put(92,session.options.file?session.file_buffers:session.next_buffer-1);put(96,log_lost);put(100,session.realtime_buffers_lost);
        put(104,0,shape_.unit*8);put(shape_.property_name,name_at);put(shape_.property_file,header.file_name.empty()?0:file_at);
        if(required>size_){put(0,required);return 234;}memory_.write(GuestAddress{address_+name_at},name);
        if(!header.file_name.empty())memory_.write(GuestAddress{address_+file_at},file);return 0;
    }
    void require_name(std::u16string_view name)const{const auto data=bytes(name);const auto offset=name_offset();validate(offset);if(data.size()>size_-offset)throw etw::guest::Fault{24};
        const auto file=file_offset();if(file){const auto size=bytes(text(file)).size();if(offset<std::uint64_t{file}+size && file<std::uint64_t{offset}+data.size())throw etw::guest::Fault{87};}}
private:
    GuestMemory& memory_;std::uint64_t address_;etw::guest::Layout shape_;bool wide_;std::uint32_t size_{};
    void validate(std::uint32_t offset)const{if(offset<shape_.properties || offset>=size_ || (wide_ && offset%2))throw etw::guest::Fault{87};}
    etw::Bytes bytes(std::u16string_view value)const{if(wide_)return utf16_bytes(value);const auto text=utf16_to_utf8(value);etw::Bytes result(text.begin(),text.end());result.push_back(0);return result;}
};
etw::Filter filter(const GuestMemory& memory,std::uint64_t descriptor){
    memory.check(GuestAddress{descriptor},16,Access::Read);const auto ptr=memory.load(GuestAddress{descriptor},64),size=memory.load(GuestAddress{descriptor+8},32);
    if(size>1024 || (size && !ptr))throw etw::guest::Fault{87};etw::Filter value;value.type=static_cast<std::uint32_t>(memory.load(GuestAddress{descriptor+12},32));
    value.data.resize(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{ptr},value.data);return value;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_etw_controller(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const auto shape=etw::guest::layout(x86_);
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto text=[&](std::uint64_t address){if(!address)throw etw::guest::Fault{87};return wide?read_utf16(memory,GuestAddress{address}):utf8_to_utf16(read_ansi(memory,address,32767));};
    const auto deadline=[&](EtwSessionInfo& session){const auto seconds=session.flush_timer?session.flush_timer:session.mode&0x100U?1U:0U;
        session.deadline=seconds && runtime_.clock_ticks<=UINT64_MAX-std::uint64_t{seconds}*10000000?runtime_.clock_ticks+std::uint64_t{seconds}*10000000:UINT64_MAX;};
    const auto used=[&](std::uint64_t handle){const auto core=runtime_.traces.session(handle);std::size_t count=core->backlog.size()+(core->pending.empty()?0U:1U);
        for(const auto& [id,info]:etw_consumers_){const auto consumer=runtime_.traces.consumer(id);if(consumer && consumer->session==handle)count+=consumer->buffers.size()+consumer->ready_buffers.size();}return count;};
    try{
        if(name.starts_with("EnableTrace")){
            auto& thread=threads_.items.at(threads_.current);const auto waiting=etw_enable_waits_.find(threads_.current);
            if(waiting!=etw_enable_waits_.end()){bool pending=false;for(auto id:waiting->second)pending|=runtime_.traces.notification_pending(id);
                if(!pending || thread.wait.deadline<=runtime_.clock_ticks){const auto result=pending?1460U:0U;etw_enable_waits_.erase(waiting);thread.wait={};return finish(result);}
                thread.status=ThreadStatus::Waiting;schedule(state,memory);return {};}
            std::uint64_t handle=0,guid=0;std::uint32_t code=0,property=0,timeout=0;etw::Enable config;
            if(name=="EnableTrace"){handle=arg(4);guid=arg(3);code=arg(0)?1U:0U;config.level=static_cast<std::uint8_t>(arg(2));config.any=static_cast<std::uint32_t>(arg(1));}
            else if(name=="EnableTraceEx"){handle=arg(2);guid=arg(0);code=static_cast<std::uint32_t>(arg(3));config.level=static_cast<std::uint8_t>(arg(4));config.any=arg(5);config.all=arg(6);property=static_cast<std::uint32_t>(arg(7));
                if(arg(1))memory.read(GuestAddress{arg(1)},config.source);if(arg(8))config.filters.push_back(filter(memory,arg(8)));}
            else{handle=arg(0);guid=arg(1);code=static_cast<std::uint32_t>(arg(2));config.level=static_cast<std::uint8_t>(arg(3));config.any=arg(4);config.all=arg(5);timeout=static_cast<std::uint32_t>(arg(6));
                if(arg(7)){const auto at=arg(7);const auto version=memory.load(GuestAddress{at},32);if(version!=1 && version!=2)return finish(87);
                    const unsigned descriptor=x86_?28:32;memory.check(GuestAddress{at},version==1?descriptor+shape.unit:x86_?36:48,Access::Read);
                    property=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32));if(memory.load(GuestAddress{at+8},32))return finish(87);memory.read(GuestAddress{at+12},config.source);
                    const auto ptr=memory.load(GuestAddress{at+descriptor},static_cast<std::uint8_t>(shape.unit*8));const auto count=version==1?(ptr?1U:0U):memory.load(GuestAddress{at+descriptor+shape.unit},32);
                    if(count>13 || (count && !ptr))return finish(87);for(std::uint64_t i=0;i<count;++i)config.filters.push_back(filter(memory,ptr+i*16));}}
            if(!guid || !handle || code>(name=="EnableTraceEx2"?2U:1U))return finish(87);if(property&~0x10U)return finish(50);config.ignore_keyword_zero=(property&0x10U)!=0;
            const auto found=etw_sessions_.find(handle);const auto core=runtime_.traces.session(handle);if(found==etw_sessions_.end() || !core || !core->active)return finish(6);
            if(!etw_session_access(found->second))return finish(5);etw::Guid id{};memory.read(GuestAddress{guid},id);
            if(code==1 && core->bindings.contains(id) && std::none_of(runtime_.event_providers.begin(),runtime_.event_providers.end(),[&](const auto& item){return item.second.id==id;}))return finish(1);
            const auto previous=runtime_.traces.notification_ids();const auto status=runtime_.traces.enable(handle,id,code,std::move(config));if(status!=etw::Status::Success)return finish(etw::win_error(status));
            std::vector<std::uint64_t> callbacks;for(auto pending:runtime_.traces.notification_ids())if(std::find(previous.begin(),previous.end(),pending)==previous.end())callbacks.push_back(pending);
            (void)dispatch_etw_notifications(state,memory);if(!timeout || callbacks.empty())return finish(0);
            etw_enable_waits_.emplace(threads_.current,std::move(callbacks));thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::EtwEnable;
            thread.wait.deadline=timeout==UINT32_MAX || runtime_.clock_ticks>UINT64_MAX-std::uint64_t{timeout}*10000?UINT64_MAX:runtime_.clock_ticks+std::uint64_t{timeout}*10000;
            schedule(state,memory);return {};
        }
        Properties props(memory,arg(2),x86_,wide);
        if(name.starts_with("StartTrace")){
            if(!arg(0) || !arg(1))return finish(87);memory.check(GuestAddress{arg(0)},8,Access::Write);memory.store(GuestAddress{arg(0)},64,0);
            const auto session_name=text(arg(1));if(session_name.empty() || session_name.size()>1024)return finish(87);props.require_name(session_name);
            const auto flags=props.get(44);if(!(flags&0x20000U))return finish(87);if(flags&0x800000U)return finish(50);
            const auto mode=props.get(64);constexpr std::uint32_t allowed=1U|0x100U|0x800U|0x20000U|0x10000000U;
            if(mode&~allowed)return finish(50);if((mode&0x20000U) && !(mode&0x800U))return finish(87);if(props.get(72))return finish(50);
            etw::SessionOptions options;options.id=props.guid();options.realtime=(mode&0x100U)!=0;options.header.session_name=session_name;options.header.file_name=props.text(props.file_offset());
            if(!options.header.file_name.empty() && (mode&0x800U))options.header.file_name+=u"_"+utf8_to_utf16(std::to_string(process_id()));
            options.file=!options.header.file_name.empty();if(!options.file && !options.realtime)return finish(161);if(!options.file && (mode&1U))return finish(87);
            EtwSessionInfo info;initialize_tokens();info.owner=token_user_sid_string(effective_token_id());info.private_logger=(mode&0x20000U)!=0;
            if(!etw_session_access(info))return finish(5);if(mode&0x800U){const bool registered=std::any_of(runtime_.event_providers.begin(),runtime_.event_providers.end(),[&](const auto& item){return item.second.id==options.id;});if(!registered)return finish(87);}
            if(options.id==etw::Guid{}){if(mode&0x800U)return finish(87);if(!random_bytes(options.id))return finish(8);options.id[7]=(options.id[7]&15U)|64U;options.id[8]=(options.id[8]&63U)|128U;}
            auto& header=options.header;const auto size=props.get(48);if(size>1024)return finish(50);header.buffer_size=std::max(4U,size?size:64U)*1024;
            header.clock=props.get(40)?props.get(40):1U;if(header.clock>3)return finish(87);header.mode=mode|(options.file?1U:0U);header.x86=x86_;header.pointer_size=shape.unit;
            header.processors=memory.processor_count();header.cpu_mhz=2400;header.frequency=10000000;header.start_time=etw_timestamp(2,state);header.boot_time=runtime_.filetime_base;
            header.start_timestamp=etw_timestamp(header.clock,state);const auto minimum=mode&0x10000000U?2U:memory.processor_count()*2;
            info.minimum=std::max(minimum,props.get(52));info.maximum=std::max(info.minimum,props.get(56)?props.get(56):64U);if(info.maximum>65536)return finish(8);
            options.buffer_limit=info.maximum;header.maximum_file_size=props.get(60);info.flush_timer=props.get(68);info.mode=header.mode;deadline(info);
            const auto result=runtime_.traces.start(options);if(result.status!=etw::Status::Success)return finish(etw::win_error(result.status));
            if(options.file){auto opened=filesystem_.open_host(utf16_to_utf8(header.file_name),0xc0000000U,2,0,1);if(opened.error){(void)runtime_.traces.stop(result.handle,header.start_time);(void)runtime_.traces.forget_session(result.handle);return finish(opened.error==32?161U:opened.error);}info.file=std::move(opened.file);info.key=std::move(opened.key);}
            etw_sessions_.emplace(result.handle,std::move(info));const auto error=flush_etw_file(result.handle);if(error){(void)runtime_.traces.stop(result.handle,header.start_time);etw_sessions_.erase(result.handle);(void)runtime_.traces.forget_session(result.handle);return finish(error);}
            const auto& saved=etw_sessions_.at(result.handle);const auto output=props.output(result.handle,*runtime_.traces.session(result.handle),saved.minimum,saved.maximum,saved.flush_timer,saved.file_errors,used(result.handle));
            memory.store(GuestAddress{arg(0)},64,result.handle);return finish(output);
        }
        if(!name.starts_with("ControlTrace") && !name.starts_with("StopTrace") && !name.starts_with("QueryTrace") && !name.starts_with("FlushTrace"))throw HostCallFault("未知 ETW controller 入口");if(!arg(0) && !arg(1))return finish(87);
        const auto handle=arg(0)?arg(0):runtime_.traces.find_session(text(arg(1)));auto found=etw_sessions_.find(handle);const auto core=runtime_.traces.session(handle);
        if(found==etw_sessions_.end() || !core || !core->active)return finish(4201);if(!etw_session_access(found->second))return finish(5);
        const auto code=name.starts_with("StopTrace")?1U:name.starts_with("QueryTrace")?0U:name.starts_with("FlushTrace")?3U:static_cast<std::uint32_t>(arg(3));if(code>3)return finish(code<=5?50U:87U);std::uint32_t error=0;
        if(code==2){auto options=core->options;auto info=found->second;const auto input_mode=props.get(64),timer=props.get(68),maximum=props.get(56);
            if(props.get(72))return finish(50);if(input_mode&~0x100U)return finish(87);
            if(info.private_logger){if(input_mode || maximum)return finish(87);}else{options.realtime=(input_mode&0x100U)!=0;options.header.mode=(options.header.mode&~0x100U)|input_mode;
                if(maximum){info.maximum=std::max(info.minimum,maximum);options.buffer_limit=info.maximum;}}
            if(timer)info.flush_timer=timer;bool rollover=false;OpenHostFile opened;
            if(props.file_offset()){const auto file=props.text(props.file_offset());if(file.empty() || file==options.header.file_name)return finish(87);options.file=true;options.header.file_name=file;if(info.mode&0x800U)options.header.file_name+=u"_"+utf8_to_utf16(std::to_string(process_id()));options.header.mode|=1U;rollover=true;}
            if(!options.file && !options.realtime)return finish(87);
            // 先在值複本驗證更新，確保檔案切換失敗不破壞現有 session。
            auto checked=runtime_.traces;const auto validation=checked.update(handle,options,rollover);if(validation!=etw::Status::Success && validation!=etw::Status::QueueFull && validation!=etw::Status::RealtimeFull)return finish(etw::win_error(validation));
            error=flush_etw_file(handle);if(error)return finish(error);
            if(rollover){opened=filesystem_.open_host(utf16_to_utf8(options.header.file_name),0xc0000000U,2,0,1);if(opened.error)return finish(opened.error==32?161U:opened.error);}
            error=etw::win_error(runtime_.traces.update(handle,options,rollover));if(rollover){info.file=std::move(opened.file);info.key=std::move(opened.key);}info.mode=options.header.mode;deadline(info);found->second=std::move(info);
            const auto file_error=flush_etw_file(handle);if(!error)error=file_error;
        }else if(code==1 || code==3){const auto status=code==1?runtime_.traces.stop(handle,etw_timestamp(2,state)):runtime_.traces.flush(handle);error=etw::win_error(status);
            const auto file_error=flush_etw_file(handle);if(!error)error=file_error;if(code==3)deadline(found->second);(void)dispatch_etw_notifications(state,memory);}
        const auto snapshot=*runtime_.traces.session(handle);const auto info=found->second;const auto allocated=used(handle);
        if(code==1 && !snapshot.active){etw_sessions_.erase(found);(void)runtime_.traces.forget_session(handle);}
        const auto output=props.output(handle,snapshot,info.minimum,info.maximum,info.flush_timer,info.file_errors,allocated);return finish(error?error:output);
    }catch(const etw::guest::Fault& error){return finish(error.code);}catch(const MemoryFault&){return finish(998);}catch(const etw::Error& error){return finish(error.failure==etw::Failure::TooLarge?534:87);}
}
}
