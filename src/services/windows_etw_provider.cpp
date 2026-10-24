#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_etw_support.hpp"
#include <algorithm>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_etw_provider(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto shape=etw::guest::layout(x86_);const auto bits=x86_?32:64;
    const auto finish=[&](std::uint64_t value,bool wide=false){arg.return_to(state,value,wide);return std::optional<std::uint32_t>{};};
    try{
        if(name=="EventActivityIdControl"){
            const auto code=static_cast<std::uint32_t>(arg(0));if(code<1 || code>5 || !arg(1))return finish(87);
            memory.check(GuestAddress{arg(1)},16,code==2?Access::Read:Access::Write);auto& current=threads_.items.at(threads_.current).activity_id;
            etw::Guid value{};if(code==2 || code==4)memory.read(GuestAddress{arg(1)},value);
            if(code==3 || code==5){if(!random_bytes(value))return finish(8);value[7]=(value[7]&15U)|64U;value[8]=(value[8]&63U)|128U;}
            if(code==1)memory.write(GuestAddress{arg(1)},current);else if(code==2)current=value;else if(code==3)memory.write(GuestAddress{arg(1)},value);
            else {const auto previous=current;current=value;memory.write(GuestAddress{arg(1)},previous);}return finish(0);
        }
        if(name=="EventRegister" || name=="RegisterTraceGuidsA" || name=="RegisterTraceGuidsW"){
            const bool classic=name!="EventRegister";const auto guid=arg(classic?2:0),output=arg(classic?7:3),callback=arg(classic?0:1),context=arg(classic?1:2);
            const auto count=classic?static_cast<std::uint32_t>(arg(3)):0U;
            if(!output)return finish(87);memory.check(GuestAddress{output},8,Access::Write);memory.store(GuestAddress{output},64,0);
            if(!guid || (classic && (!callback || (count && !arg(4)) || count>1024 || arg(5) || arg(6)))){memory.store(GuestAddress{output},64,0);return finish(87);}
            if(runtime_.event_providers.size()>=1024){memory.store(GuestAddress{output},64,0);return finish(8);}
            EventProviderRegistration registration;registration.classic=classic;registration.callback=callback;registration.context=context;memory.read(GuestAddress{guid},registration.id);
            if(count)memory.check(GuestAddress{arg(4)},std::size_t{count}*shape.unit*2,Access::Write);
            for(std::uint32_t i=0;i<count;++i){const auto ptr=memory.load(GuestAddress{arg(4)+std::uint64_t{i}*shape.unit*2},static_cast<std::uint8_t>(bits));
                if(!ptr){memory.store(GuestAddress{output},64,0);return finish(87);}etw::Guid id{};memory.read(GuestAddress{ptr},id);registration.trace_classes.emplace_back(0,id);}
            const auto handle=runtime_.next_event_provider;const auto status=runtime_.traces.register_provider(handle,registration.id,classic,callback!=0);
            if(status!=etw::Status::Success){memory.store(GuestAddress{output},64,0);return finish(etw::win_error(status));}
            ++runtime_.next_event_provider;for(std::uint32_t i=0;i<count;++i){registration.trace_classes[i].first=runtime_.next_event_provider++;
                memory.store(GuestAddress{arg(4)+std::uint64_t{i}*shape.unit*2+shape.unit},static_cast<std::uint8_t>(bits),registration.trace_classes[i].first);}
            runtime_.event_providers.emplace(handle,std::move(registration));memory.store(GuestAddress{output},64,handle);
            if(callback && dispatch_etw_notifications(state,memory,true,handle))return {};return finish(0);
        }
        if(name=="EventUnregister" || name=="UnregisterTraceGuids"){
            auto& thread=threads_.items.at(threads_.current);const auto waiting=etw_enable_waits_.find(threads_.current);
            if(waiting!=etw_enable_waits_.end()){const bool pending=std::any_of(waiting->second.begin(),waiting->second.end(),[&](auto id){return runtime_.traces.notification_pending(id);});
                if(pending){thread.status=ThreadStatus::Waiting;schedule(state,memory);return {};}etw_enable_waits_.erase(waiting);thread.wait={};return finish(0);}
            const bool classic=name=="UnregisterTraceGuids";const auto status=runtime_.traces.unregister_provider(arg(0),classic);
            if(status!=etw::Status::Success)return finish(etw::win_error(status));runtime_.event_providers.erase(arg(0));
            std::vector<std::uint64_t> cancelled,pending;
            for(const auto& [id,operation]:etw_callbacks_)if(operation.notification.registration==arg(0)){
                const auto worker=wait_workers_.find(operation.worker);const auto wait=worker==wait_workers_.end()?registered_waits_.end():registered_waits_.find(worker->second);
                if(operation.worker && wait!=registered_waits_.end() && !wait->second.active)cancelled.push_back(id);
                else if(operation.owner!=threads_.current)pending.push_back(id);
            }
            for(auto id:cancelled){const auto worker=etw_callbacks_.at(id).worker;const auto wait=wait_workers_.at(worker);discard_etw_callback(state,memory,id);release_registered_wait(wait);}
            if(pending.empty())return finish(0);etw_enable_waits_.emplace(threads_.current,std::move(pending));thread.status=ThreadStatus::Waiting;
            thread.wait={};thread.wait.kind=ThreadWaitKind::EtwEnable;schedule(state,memory);return {};
        }
        if(name=="GetTraceLoggerHandle"){
            if(arg(0)){const auto logger=memory.load(GuestAddress{arg(0)+8},64);if(runtime_.traces.classic_info(logger)){last_error_=0;return finish(logger,true);}}
            last_error_=6;return finish(UINT64_MAX,true);
        }
        if(name=="GetTraceEnableFlags" || name=="GetTraceEnableLevel"){
            const auto binding=runtime_.traces.classic_info(arg(0));if(!binding){last_error_=6;return finish(0);}last_error_=0;
            return finish(name=="GetTraceEnableLevel"?binding->enable.level:static_cast<std::uint32_t>(binding->enable.any));
        }
        if(name=="TraceEvent"){
            if(!runtime_.traces.classic_info(arg(0)))return finish(6);auto record=etw::guest::classic(memory,arg(1),x86_);
            std::uint32_t clock=0;for(const auto& [id,info]:etw_sessions_){const auto session=runtime_.traces.session(id);if(!session || !session->active)continue;
                for(const auto& [guid,binding]:session->bindings)if(binding.logger==arg(0))clock=session->options.header.clock;}
            if(!clock)return finish(6);if(!(memory.load(GuestAddress{arg(1)+44},32)&0x200U))record.timestamp=etw_timestamp(clock,state);
            record.thread=thread_identifier(threads_.current);record.process=static_cast<std::uint32_t>(process_id());record.processor=static_cast<std::uint16_t>(state.processor_id);
            record.processor_time=threads_.items.at(threads_.current).cycles;return finish(etw::win_error(runtime_.traces.write_classic(arg(0),record)));
        }
        if(name=="TraceMessage" || name=="TraceMessageVa"){
            // WPP 訊息：依旗標前置序號、GUID／元件 ID、時間戳與系統資訊，再串接 (指標, 長度) 參數直到 NULL。
            if(!runtime_.traces.classic_info(arg(0)))return finish(6);const auto flags=static_cast<std::uint32_t>(arg(1));
            if(flags&~0x3fU)return finish(87);if((flags&2U) && (flags&4U))return finish(87);if(!arg(2))return finish(87);
            std::uint32_t clock=0;for(const auto& [id,info]:etw_sessions_){const auto session=runtime_.traces.session(id);if(!session || !session->active)continue;
                for(const auto& [guid,binding]:session->bindings)if(binding.logger==arg(0))clock=session->options.header.clock;}
            if(!clock)return finish(6);
            etw::Record record;record.kind=etw::Kind::Classic;record.x86=x86_;record.version=static_cast<std::uint16_t>(arg(3));
            record.timestamp=etw_timestamp(clock,state);record.thread=thread_identifier(threads_.current);record.process=static_cast<std::uint32_t>(process_id());
            record.processor=static_cast<std::uint16_t>(state.processor_id);record.processor_time=threads_.items.at(threads_.current).cycles;
            const auto append=[&](std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)record.data.push_back(static_cast<std::uint8_t>(value>>(i*8)));};
            if(flags&1U)append(++runtime_.trace_message_sequence,4);
            if(flags&2U)memory.read(GuestAddress{arg(2)},record.provider);else if(flags&4U)append(memory.load(GuestAddress{arg(2)},32),4);
            if(flags&(8U|16U))append(record.timestamp,8);
            if(flags&32U){append(record.thread,4);append(record.process,4);}
            const unsigned unit=x86_?4:8;
            const auto pair=[&](unsigned index)->std::pair<std::uint64_t,std::uint64_t>{
                if(name=="TraceMessage")return {arg(4+index*2),arg(5+index*2)};
                const auto list=arg(4);return {memory.load(GuestAddress{list+std::uint64_t{index}*2*unit},static_cast<std::uint8_t>(bits)),memory.load(GuestAddress{list+std::uint64_t{index}*2*unit+unit},static_cast<std::uint8_t>(bits))};
            };
            for(unsigned index=0;;++index){
                const auto [pointer,size]=pair(index);if(!pointer)break;
                if(index>=256 || size>65535 || record.data.size()+size>65535)return finish(534);
                etw::Bytes chunk(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{pointer},chunk);record.data.insert(record.data.end(),chunk.begin(),chunk.end());
            }
            return finish(etw::win_error(runtime_.traces.write_classic(arg(0),record)));
        }
        const auto registration=runtime_.event_providers.find(arg(0));
        if(name=="EventProviderEnabled" || name=="EventEnabled"){
            if(registration==runtime_.event_providers.end() || registration->second.classic || !runtime_.traces.aggregate(arg(0)).enabled)return finish(0);
            if(name=="EventProviderEnabled")return finish(runtime_.traces.provider_enabled(arg(0),static_cast<std::uint8_t>(arg(1)),arg(2)));
            const auto descriptor=etw::guest::descriptor(memory,arg(1));return finish(runtime_.traces.provider_enabled(arg(0),descriptor.level,descriptor.keyword));
        }
        if(name!="EventWrite" && name!="EventWriteTransfer" && name!="EventWriteEx" && name!="EventWriteString")throw HostCallFault("未知 ETW provider 入口");
        // EventWriteEx 只多 Filter（排除的 session）與 Flags；旗標只有 NO_FAULTING(1)、INPRIVATE(2)。
        if(name=="EventWriteEx" && (static_cast<std::uint32_t>(arg(3))&~3U))return finish(87);
        if(!arg(0))return finish(0);if(registration==runtime_.event_providers.end() || registration->second.classic)return finish(6);
        // 未啟用時不讀取 descriptor、資料指標、count 或 activity，保留原本有效契約。
        if(!runtime_.traces.aggregate(arg(0)).enabled)return finish(0);
        etw::Record record;record.x86=x86_;record.flags=static_cast<std::uint16_t>((x86_?0x20U:0x40U)|0x10U);record.provider=registration->second.id;
        record.thread=thread_identifier(threads_.current);record.process=static_cast<std::uint32_t>(process_id());record.processor=static_cast<std::uint16_t>(state.processor_id);
        record.processor_time=threads_.items.at(threads_.current).cycles;record.activity=threads_.items.at(threads_.current).activity_id;
        if(name=="EventWriteString"){record.descriptor.level=static_cast<std::uint8_t>(arg(1));record.descriptor.keyword=arg(2);record.flags|=4;
            if(!runtime_.traces.provider_enabled(arg(0),record.descriptor.level,record.descriptor.keyword))return finish(0);
            if(!arg(3))return finish(87);record.data=utf16_bytes(read_utf16(memory,GuestAddress{arg(3)}));
        }else{
            record.descriptor=etw::guest::descriptor(memory,arg(1));if(!runtime_.traces.provider_enabled(arg(0),record.descriptor.level,record.descriptor.keyword))return finish(0);
            const bool transfer=name=="EventWriteTransfer" || name=="EventWriteEx";const unsigned activity=name=="EventWriteEx"?4:2;
            if(transfer){if(arg(activity))memory.read(GuestAddress{arg(activity)},record.activity);if(arg(activity+1)){etw::Bytes related(16);memory.read(GuestAddress{arg(activity+1)},related);record.extensions.push_back({1,0,std::move(related)});}}
            const auto count=static_cast<std::uint32_t>(arg(transfer?activity+2:2));const auto source=arg(transfer?activity+3:3);
            etw::guest::data(memory,record,count,source,registration->second.use_descriptor_type);
        }
        if(!registration->second.traits.empty() && std::none_of(record.extensions.begin(),record.extensions.end(),[](const auto& e){return e.type==12;}))record.extensions.push_back({12,0,registration->second.traits});
        return finish(etw::win_error(runtime_.traces.write_timed(arg(0),record,etw_timestamp(2,state))));
    }catch(const etw::guest::Fault& error){return finish(error.code);}catch(const MemoryFault&){if(name=="GetTraceLoggerHandle"){last_error_=998;return finish(UINT64_MAX,true);}return finish(998);}catch(const etw::Error& error){return finish(error.failure==etw::Failure::TooLarge?534:87);}
}
}
