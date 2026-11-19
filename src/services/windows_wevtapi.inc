// wevtapi（Windows 事件記錄 API）：由 windows_eventlog.cpp 末端引入，與舊版事件記錄 API 共用沙箱中的記錄檔、XPath 比對、
// 備份檔格式（匯出檔）與登錄中的記錄設定（SYSTEM\CurrentControlSet\Services\EventLog\<通道>）。
// 本機工作階段、查詢、訂閱（拉取與推送）、定位、通道列舉與設定、記錄資訊、清除、匯出、書籤與呈現。
namespace {
constexpr std::u16string_view eventlog_prefix=u"\\system\\currentcontrolset\\services\\eventlog\\";
std::u16string builtin_channel(std::u16string_view lowered){
    return lowered==u"application"?u"Application":lowered==u"system"?u"System":lowered==u"security"?u"Security":u"";
}
std::uint64_t filetime_of(const timespec& value){
    return static_cast<std::uint64_t>(value.tv_sec)*10000000U+static_cast<std::uint64_t>(value.tv_nsec)/100U+UINT64_C(116444736000000000);
}
// 書籤 XML：與 EvtRender(EvtRenderBookmark) 相同的格式。
std::u16string bookmark_xml(const EventBookmark& bookmark){
    std::u16string xml=u"<BookmarkList>\r\n";
    if(bookmark.set)xml+=u"  <Bookmark Channel='"+bookmark.channel+u"' RecordId='"+utf8_to_utf16(std::to_string(bookmark.record))+u"' IsCurrent='true'/>\r\n";
    return xml+u"</BookmarkList>";
}
}
std::optional<std::uint32_t> WindowsServices::invoke_wevtapi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint64_t result=0;
    auto& logs=runtime_.eventlog;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto log_directory=[this]{return registry_file_.empty()?std::filesystem::path(mapping_session()):registry_file_.parent_path();};
    const auto string=[&](std::uint64_t pointer){if(!pointer)throw EventError{87};return read_utf16(memory,GuestAddress{pointer},16384);};
    const auto matches=[](const EventRecord& record,const std::string& query){
        const auto decoded=decode_event(record);auto doc=event_document(decoded.xml);auto matching=evaluate(doc.get(),query);return matching->nodesetval && matching->nodesetval->nodeNr>0;
    };
    // 推送模式的訂閱：在執行緒池執行，依紀錄編號逐筆呼叫 EVT_SUBSCRIBE_CALLBACK(EvtSubscribeActionDeliver, Context, Event)；
    // 事件 handle 只在回呼期間有效。訂閱在回呼中被關閉時停止。
    if(service.family=="internal-evt-subscription")
        return nested_continuation(state,memory,"事件訂閱回呼",[this,service,matches,log_directory](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto handle=call(0);auto& state_logs=runtime_.eventlog;
            for(;;){
                const auto found=state_logs.subscriptions.find(handle);if(found==state_logs.subscriptions.end())break;
                auto& subscription=found->second;std::optional<EventRecord> pending;
                if(subscription.closing){subscription.queued=false;break;}
                try{
                    for(auto& record:read_records(log_directory()/record_filename(subscription.channel),subscription.display))
                        if(record_number(record)>=subscription.next && matches(record,subscription.query)){pending=std::move(record);break;}
                }catch(const EventError&){pending.reset();}
                if(!pending || state_logs.events.size()>=4096){subscription.queued=false;break;}
                subscription.next=record_number(*pending)+1;pending->parent=handle;subscription.delivering=threads_.current;
                const auto event=state_logs.next++;const auto target=subscription.callback,context=subscription.context;state_logs.events.emplace(event,std::move(*pending));
                (void)co_await service_callback::call(target,{1,context,event});
                state_logs.events.erase(event);
                if(const auto again=state_logs.subscriptions.find(handle);again!=state_logs.subscriptions.end())again->second.delivering=0;
            }
            call.return_to(s,0);co_return std::optional<std::uint32_t>{};
        });
    // EvtClose 在推送回呼正於其他執行緒執行時等它結束（Windows 的 EvtClose 會等待未完成的回呼）。等待後服務會重新進入並由同一續行取回結果，
    // 因此訂閱保留到續行完成（closing＝2）才移除，重新進入時仍走續行，不落到一般路徑。
    if(symbol=="EvtClose"){
        const auto found=logs.subscriptions.find(arg(0));
        if(found!=logs.subscriptions.end() && (found->second.closing || (found->second.delivering && found->second.delivering!=threads_.current))){
            const auto handle=arg(0);if(!found->second.closing)found->second.closing=1;
            const auto closed=nested_continuation(state,memory,"關閉事件訂閱",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const auto sleep=resolve("kernel32.dll","Sleep").value;
                for(;;){const auto item=runtime_.eventlog.subscriptions.find(call(0));if(item==runtime_.eventlog.subscriptions.end())break;
                    if(!item->second.delivering || item->second.delivering==threads_.current){item->second.closing=2;break;}
                    (void)co_await service_callback::call(sleep,{1});}
                call.return_to(s,1);co_return std::optional<std::uint32_t>{};
            });
            if(const auto item=logs.subscriptions.find(handle);item!=logs.subscriptions.end() && item->second.closing==2){
                logs.subscriptions.erase(item);std::erase_if(logs.events,[&](const auto& event){return event.second.parent==handle;});}
            return closed;
        }
    }
    try{
        if(!runtime_.network.external_io && symbol!="EvtClose" && symbol!="EvtCreateRenderContext" && symbol!="EvtRender" && symbol!="EvtCreateBookmark" && symbol!="EvtUpdateBookmark" && symbol!="EvtGetExtendedStatus")
            throw HostCallFault("比較模式不允許事件記錄 I/O");
        const auto handles=[&]{return logs.queries.size()+logs.events.size()+logs.contexts.size()+logs.sessions.size()+logs.channel_enums.size()+logs.channel_configs.size()+logs.log_files.size()+logs.bookmarks.size()+logs.subscriptions.size();};
        const auto new_handle=[&](){if(handles()>=4096)throw EventError{8};return logs.next++;};
        const auto session=[&](std::uint64_t handle){if(handle && !logs.sessions.contains(handle))throw EventError{6};};
        // 通道：Application／System／Security 以外的記錄必須在登錄中登記；回傳小寫名稱與顯示名稱。
        const auto channel=[&](std::u16string requested)->std::pair<std::u16string,std::u16string>{
            const auto lowered=lower(requested);if(lowered.empty() || lowered.find_first_of(u"\\/:")!=lowered.npos)throw EventError{15007};
            if(const auto fixed=builtin_channel(lowered);!fixed.empty())return {lowered,fixed};
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
            const auto key=std::u16string(1,u'\2')+std::u16string(eventlog_prefix)+lowered;if(!runtime_.registry.keys.contains(key))throw EventError{15007};
            const auto display=runtime_.registry.display_names.find(key);
            return {lowered,display!=runtime_.registry.display_names.end() && !display->second.empty()?display->second.substr(display->second.find_last_of(u'\\')+1):requested};
        };
        const auto query_text=[&](std::uint64_t pointer){
            const auto query=pointer?utf16_to_utf8(string(pointer)):std::string("*");if(query.empty())throw EventError{15001};if(query.front()=='<')throw EventError{50};
            auto empty=event_document("<Event><System/></Event>");auto validation=evaluate(empty.get(),query);if(validation->type!=XPATH_NODESET)throw EventError{15001};return query;
        };
        // 匯出檔與 OpenBackupEventLog 相同的備份格式；紀錄沒有保存通道名稱，以檔名（不含副檔名）作為通道。
        const auto file_records=[&](const std::u16string& path){
            if(path.empty())throw EventError{87};auto opened=filesystem_.open_reader(utf16_to_utf8(path));if(opened.error)throw EventError{opened.error};
            if(opened.metadata.size>32*1024*1024)throw EventError{1500};std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));
            const auto read=opened.file->read(bytes,0);if(read.error)throw EventError{read.error};bytes.resize(static_cast<std::size_t>(read.bytes));
            auto name=path.substr(path.find_last_of(u"\\/")+1);if(const auto dot=name.find_last_of(u'.');dot!=name.npos && dot)name.resize(dot);
            return parse_backup(bytes,name);
        };
        const auto select=[&](std::uint64_t path,std::uint64_t query,bool file){
            const auto filter=query_text(query);std::vector<EventRecord> selected;
            auto records=file?file_records(string(path)):[&]{const auto [lowered,display]=channel(string(path));return read_records(log_directory()/record_filename(lowered),display);}();
            for(auto& record:records)if(matches(record,filter))selected.push_back(std::move(record));
            return selected;
        };
        // EVT_VARIANT（值 8 位元組、Count、Type）加上之後的字串／陣列資料；緩衝區不足時回 ERROR_INSUFFICIENT_BUFFER 並寫入需要的大小。
        const auto write_variant=[&](std::uint32_t type,std::uint64_t number,const std::vector<std::u16string>* strings,std::uint64_t buffer,std::uint32_t size,std::uint64_t used){
            std::vector<std::uint8_t> output(16);std::uint32_t count=0;
            if(strings){
                const bool array=type&0x80U;count=array?static_cast<std::uint32_t>(strings->size()):0;const auto table=16U;std::size_t at=16+(array?strings->size()*unit:0);
                std::vector<std::uint64_t> pointers;std::vector<std::uint8_t> text;
                for(const auto& value:*strings){pointers.push_back(buffer+at+text.size());const auto bytes=utf16_bytes(value);text.insert(text.end(),bytes.begin(),bytes.end());}
                if(array){output.resize(16+strings->size()*unit);for(std::size_t i=0;i<pointers.size();++i)for(unsigned b=0;b<unit;++b)output[table+i*unit+b]=static_cast<std::uint8_t>(pointers[i]>>(8*b));number=buffer+table;}
                else number=strings->empty()?0:pointers.front();
                output.insert(output.end(),text.begin(),text.end());
            }
            for(unsigned b=0;b<8;++b)output[b]=static_cast<std::uint8_t>(number>>(8*b));put(output,8,count);put(output,12,type);
            if(!used)throw EventError{87};memory.store(GuestAddress{used},32,output.size());
            if(size<output.size() || !buffer)throw EventError{122};memory.write(GuestAddress{buffer},output);
        };
        // 登錄中的通道設定（DWORD／字串值）；沒有值時使用預設。
        const auto registry_value=[&](const std::u16string& lowered,std::u16string_view name)->const RegistryValue*{
            const auto key=runtime_.registry.keys.find(std::u16string(1,u'\2')+std::u16string(eventlog_prefix)+lowered);if(key==runtime_.registry.keys.end())return nullptr;
            const auto value=key->second.find(std::u16string(name));return value==key->second.end()?nullptr:&value->second;
        };
        const auto dword_value=[&](const RegistryValue* value,std::uint32_t fallback){return value && value->bytes.size()>=4 && (value->type==4 || value->type==3)?integer(value->bytes):fallback;};
        const auto string_value_of=[&](const RegistryValue* value,std::u16string fallback){
            if(!value || (value->type!=1 && value->type!=2) || value->bytes.size()<2)return fallback;std::u16string text;
            for(std::size_t i=0;i+1<value->bytes.size();i+=2){const auto c=static_cast<char16_t>(value->bytes[i]|(value->bytes[i+1]<<8));if(!c)break;text.push_back(c);}return text;
        };
        if(symbol=="EvtOpenSession"){
            // (LoginClass, Login, Timeout, Flags)：EvtRpcLogin（EVT_RPC_LOGIN 的 Server）只接受本機；其他伺服器沒有 RPC 端點。
            if(static_cast<std::uint32_t>(arg(0))!=1 || !arg(1) || static_cast<std::uint32_t>(arg(3)))throw EventError{87};
            const auto server_pointer=memory.load(GuestAddress{arg(1)},bits);const auto server=server_pointer?lower(string(server_pointer)):std::u16string{};
            if(!server.empty() && server!=u"." && server!=u"localhost" && server!=u"127.0.0.1" && server!=u"x64arm")throw EventError{1722};
            const auto handle=new_handle();logs.sessions.insert(handle);result=handle;
        }else if(symbol=="EvtQuery"){
            // (Session, Path, Query, Flags)：EvtQueryChannelPath（1）或 EvtQueryFilePath（2），方向 0x100／0x200，TolerateQueryErrors 0x1000。
            session(arg(0));const auto flags=static_cast<unsigned>(arg(3));
            if(flags&~0x1303U)throw EventError{50};if(((flags&3)!=1 && (flags&3)!=2) || (flags&0x300)==0x300)throw EventError{87};
            EventQuery selected;selected.thread=threads_.current;selected.records=select(arg(1),arg(2),(flags&3)==2);
            if(flags&0x200)std::reverse(selected.records.begin(),selected.records.end());const auto handle=new_handle();logs.queries.emplace(handle,std::move(selected));result=handle;
        }else if(symbol=="EvtSubscribe"){
            // (Session, SignalEvent, ChannelPath, Query, Bookmark, Context, Callback, Flags)：ToFutureEvents（1）、StartAtOldestRecord（2）、StartAfterBookmark（3），
            // TolerateQueryErrors 0x1000、Strict 0x10000；SignalEvent 與 Callback 擇一。
            session(arg(0));const auto flags=static_cast<std::uint32_t>(arg(7));const auto origin=flags&3U;
            if((flags&~0x11003U) || !origin || (!arg(1))==(!arg(6)) || ((origin==3)!=(arg(4)!=0)))throw EventError{87};
            EventSubscription subscription;const auto [lowered,display]=channel(string(arg(2)));subscription.channel=lowered;subscription.display=display;subscription.query=query_text(arg(3));
            const auto records=read_records(log_directory()/record_filename(lowered),display);
            if(origin==1)subscription.next=records.empty()?0:record_number(records.back())+1;
            else if(origin==3){
                const auto bookmark=logs.bookmarks.find(arg(4));if(bookmark==logs.bookmarks.end())throw EventError{6};
                const bool known=bookmark->second.set && lower(bookmark->second.channel)==lowered && std::any_of(records.begin(),records.end(),[&](const auto& record){return record_number(record)==bookmark->second.record;});
                if(!known && (flags&0x10000))throw EventError{1168};subscription.next=bookmark->second.set && lower(bookmark->second.channel)==lowered?bookmark->second.record+1:0;
            }
            if(arg(1)){const auto event=runtime_.wait_objects.find(arg(1));if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)throw EventError{6};subscription.signal=arg(1);}
            else{memory.check(GuestAddress{arg(6)},1,Access::Execute);subscription.callback=arg(6);subscription.context=arg(5);}
            const bool pending=std::any_of(records.begin(),records.end(),[&](const auto& record){return record_number(record)>=subscription.next && matches(record,subscription.query);});
            const auto handle=new_handle();
            if(pending && subscription.callback){if(const auto error=queue_pool_callback(state,memory,{resolve("internal-evt-subscription","Deliver").value,{handle}}))throw EventError{error};subscription.queued=true;}
            const auto signal=subscription.signal;logs.subscriptions.emplace(handle,std::move(subscription));result=handle;
            if(pending && signal){const auto event=runtime_.wait_objects.find(signal);event->second.set_signal(true);(void)resolve_thread_waits(memory,ThreadWakeScope::Objects);}
        }else if(symbol=="EvtNext"){
            // (ResultSet, EventsSize, Events, Timeout, Flags, Returned)：查詢依結果集位置；拉取式訂閱重新讀取記錄檔，交付紀錄編號不小於 next 且符合查詢的事件。
            // Timeout、Flags 是 DWORD：x64 的高 32 位元可能殘留舊值，只取低位。
            const auto capacity=static_cast<unsigned>(arg(1));if(!capacity || capacity>1024 || !arg(2) || !arg(5) || static_cast<std::uint32_t>(arg(4)))throw EventError{87};
            std::vector<EventRecord> batch;
            if(const auto subscription=logs.subscriptions.find(arg(0));subscription!=logs.subscriptions.end()){
                if(subscription->second.callback)throw EventError{87};memory.check(GuestAddress{arg(5)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,0);
                for(auto& record:read_records(log_directory()/record_filename(subscription->second.channel),subscription->second.display)){
                    if(batch.size()>=capacity)break;if(record_number(record)>=subscription->second.next && matches(record,subscription->second.query))batch.push_back(std::move(record));}
                if(batch.empty())throw EventError{259};
                if(handles()+batch.size()>4096)throw EventError{8};subscription->second.next=record_number(batch.back())+1;
            }else{
                const auto found=logs.queries.find(arg(0));if(found==logs.queries.end())throw EventError{6};auto& query=found->second;if(query.thread!=threads_.current)throw EventError{87};
                memory.check(GuestAddress{arg(5)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,0);
                const auto count=std::min<std::size_t>(capacity,query.records.size()-std::min(query.position,query.records.size()));if(!count)throw EventError{259};
                if(handles()+count>4096)throw EventError{8};batch.assign(query.records.begin()+static_cast<std::ptrdiff_t>(query.position),query.records.begin()+static_cast<std::ptrdiff_t>(query.position+count));query.position+=count;
            }
            memory.check(GuestAddress{arg(2)},batch.size()*unit,Access::Write);
            for(std::size_t i=0;i<batch.size();++i){auto event=std::move(batch[i]);event.parent=arg(0);const auto handle=logs.next++;logs.events.emplace(handle,std::move(event));memory.store(GuestAddress{arg(2)+i*unit},bits,handle);}
            memory.store(GuestAddress{arg(5)},32,batch.size());result=1;
        }else if(symbol=="EvtSeek"){
            // (ResultSet, Position, Bookmark, Timeout, Flags)：RelativeToFirst 1、RelativeToLast 2、RelativeToCurrent 3、RelativeToBookmark 4、Strict 0x10000。
            const auto flags=static_cast<std::uint32_t>(arg(4));const auto origin=flags&7U;const auto offset=static_cast<std::int64_t>(arg(1));
            if(logs.subscriptions.contains(arg(0)))throw EventError{50};const auto found=logs.queries.find(arg(0));if(found==logs.queries.end())throw EventError{6};
            if((flags&~0x10007U) || !origin || origin>4 || static_cast<std::uint32_t>(arg(3)) || ((origin==4)!=(arg(2)!=0)))throw EventError{87};
            auto& query=found->second;const auto size=static_cast<std::int64_t>(query.records.size());std::int64_t base=0;
            if(origin==2)base=size-1;else if(origin==3)base=static_cast<std::int64_t>(query.position);
            else if(origin==4){
                const auto bookmark=logs.bookmarks.find(arg(2));if(bookmark==logs.bookmarks.end())throw EventError{6};
                const auto at=std::find_if(query.records.begin(),query.records.end(),[&](const auto& record){return bookmark->second.set && lower(record.channel)==lower(bookmark->second.channel) && record_number(record)==bookmark->second.record;});
                if(at==query.records.end())throw EventError{1168};base=at-query.records.begin();
            }
            const auto target=base+offset;if(target<0 || target>size)throw EventError{1168};query.position=static_cast<std::size_t>(target);result=1;
        }else if(symbol=="EvtClose"){
            const auto handle=arg(0);
            if(logs.queries.erase(handle) || logs.subscriptions.erase(handle)){std::erase_if(logs.events,[&](const auto& item){return item.second.parent==handle;});result=1;}
            else if(logs.events.erase(handle) || logs.contexts.erase(handle) || logs.sessions.erase(handle) || logs.channel_enums.erase(handle) || logs.channel_configs.erase(handle) || logs.log_files.erase(handle) || logs.bookmarks.erase(handle))result=1;
            else throw EventError{6};
        }else if(symbol=="EvtCancel"){
            // 本實作的查詢、匯出與訂閱都在呼叫期間完成，沒有可取消的進行中作業；有效的 handle 回 TRUE。
            const auto handle=arg(0);
            if(!(logs.queries.contains(handle) || logs.subscriptions.contains(handle) || logs.sessions.contains(handle) || logs.events.contains(handle) || logs.channel_enums.contains(handle) || logs.log_files.contains(handle)))throw EventError{6};
            result=1;
        }else if(symbol=="EvtGetExtendedStatus"){
            // (BufferSize, Buffer, BufferUsed)：沒有延伸錯誤訊息，回傳空字串；回傳值是錯誤碼（不是 BOOL）。
            if(!arg(2)){last_error_=87;arg.return_to(state,87);return std::nullopt;}
            memory.store(GuestAddress{arg(2)},32,1);if(static_cast<std::uint32_t>(arg(0))<1 || !arg(1)){arg.return_to(state,122);return std::nullopt;}
            memory.store(GuestAddress{arg(1)},16,0);arg.return_to(state,0);return std::nullopt;
        }else if(symbol=="EvtCreateRenderContext"){
            EventRenderContext context;context.flags=static_cast<unsigned>(arg(2));const auto count=static_cast<unsigned>(arg(0));if(context.flags>2 || count>128 || (context.flags && (count || arg(1))) || (count && !arg(1)))throw EventError{87};
            auto empty=event_document("<Event/>");
            for(unsigned i=0;i<count;++i){auto path=utf16_to_utf8(string(memory.load(GuestAddress{arg(1)+i*unit},bits)));auto checked=evaluate(empty.get(),path);if(checked->type!=XPATH_NODESET)throw EventError{15001};context.paths.push_back(std::move(path));}
            const auto handle=new_handle();logs.contexts.emplace(handle,std::move(context));result=handle;
        }else if(symbol=="EvtRender"){
            // (Context, Fragment, Flags, BufferSize, Buffer, BufferUsed, PropertyCount)：EvtRenderEventValues（0）、EventXml（1）、Bookmark（2）。
            if(!arg(5) || !arg(6))throw EventError{87};memory.check(GuestAddress{arg(5)},4,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);
            std::vector<std::uint8_t> output;unsigned properties=0;
            if(static_cast<std::uint32_t>(arg(2))==2){
                if(arg(0))throw EventError{87};const auto bookmark=logs.bookmarks.find(arg(1));if(bookmark==logs.bookmarks.end())throw EventError{6};
                output=utf16_bytes(bookmark_xml(bookmark->second));
            }else{
                const auto found=logs.events.find(arg(1));if(found==logs.events.end())throw EventError{6};const auto decoded=decode_event(found->second);
                if(static_cast<std::uint32_t>(arg(2))==1){if(arg(0))throw EventError{87};auto xml=decoded.xml;const auto at=xml.find('>');if(at==xml.npos)throw EventError{1500};xml.insert(at," xmlns=\"http://schemas.microsoft.com/win/2004/08/events/event\"");output=utf16_bytes(utf8_to_utf16(xml));}
                else if(!static_cast<std::uint32_t>(arg(2))){
                    const auto context=logs.contexts.find(arg(0));if(context==logs.contexts.end())throw EventError{6};std::vector<EventValue> values;
                    if(context->second.flags==1)values.assign(decoded.system.begin(),decoded.system.end());else if(context->second.flags==2)values=decoded.user;
                    else{auto doc=event_document(decoded.xml);for(const auto& path:context->second.paths)values.push_back(selected_value(doc.get(),path,decoded));}
                    properties=static_cast<unsigned>(values.size());output.resize(values.size()*16);
                    for(std::size_t i=0;i<values.size();++i){const auto& value=values[i];put(output,i*16+12,value.type);std::uint64_t data=value.number;
                        if(value.type==1 || value.type==14 || value.type==19){output.resize((output.size()+7)&~std::size_t{7});data=arg(4)+output.size();output.insert(output.end(),value.bytes.begin(),value.bytes.end());if(value.type==14)put(output,i*16+8,static_cast<unsigned>(value.bytes.size()));}
                        put(output,i*16,static_cast<unsigned>(data));put(output,i*16+4,static_cast<unsigned>(data>>32));}
                }else throw EventError{50};
            }
            memory.store(GuestAddress{arg(5)},32,output.size());memory.store(GuestAddress{arg(6)},32,properties);
            if(static_cast<std::uint32_t>(arg(3))<output.size() || (!output.empty() && !arg(4)))throw EventError{122};if(!output.empty())memory.write(GuestAddress{arg(4)},output);result=1;
        }else if(symbol=="EvtCreateBookmark"){
            // (BookmarkXml)：NULL 為空書籤；XML 取 IsCurrent 的 Bookmark（沒有時取第一個）的 Channel 與 RecordId。
            EventBookmark bookmark;
            if(arg(0)){
                const auto xml=utf16_to_utf8(string(arg(0)));auto doc=xmlReadMemory(xml.data(),static_cast<int>(xml.size()),nullptr,nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING);if(!doc)throw EventError{87};
                const Xml<xmlDoc,xmlFreeDoc> owner(doc,xmlFreeDoc);const auto root=xmlDocGetRootElement(doc);
                if(!root || xmlStrcmp(root->name,BAD_CAST "BookmarkList"))throw EventError{87};xmlNode* chosen=nullptr;
                for(auto node=root->children;node;node=node->next){if(node->type!=XML_ELEMENT_NODE || xmlStrcmp(node->name,BAD_CAST "Bookmark"))continue;if(!chosen)chosen=node;
                    const XmlText current(xmlGetProp(node,BAD_CAST "IsCurrent"));if(current && !xmlStrcasecmp(current.get(),BAD_CAST "true")){chosen=node;break;}}
                if(chosen){
                    const XmlText name(xmlGetProp(chosen,BAD_CAST "Channel")),record(xmlGetProp(chosen,BAD_CAST "RecordId"));if(!name || !record)throw EventError{87};
                    const std::string digits(reinterpret_cast<const char*>(record.get()));if(digits.empty() || digits.size()>10 || digits.find_first_not_of("0123456789")!=digits.npos)throw EventError{87};
                    const auto number=std::stoull(digits);if(number>UINT32_MAX)throw EventError{87};
                    bookmark.channel=utf8_to_utf16(reinterpret_cast<const char*>(name.get()));bookmark.record=static_cast<std::uint32_t>(number);bookmark.set=true;
                }
            }
            const auto handle=new_handle();logs.bookmarks.emplace(handle,std::move(bookmark));result=handle;
        }else if(symbol=="EvtUpdateBookmark"){
            const auto bookmark=logs.bookmarks.find(arg(0));const auto event=logs.events.find(arg(1));if(bookmark==logs.bookmarks.end() || event==logs.events.end())throw EventError{6};
            bookmark->second.channel=event->second.channel;bookmark->second.record=record_number(event->second);bookmark->second.set=true;result=1;
        }else if(symbol=="EvtOpenChannelEnum"){
            // 內建的三個記錄加上登錄 EventLog 下登記的其他記錄，依名稱排序（不分大小寫）。
            session(arg(0));if(static_cast<std::uint32_t>(arg(1)))throw EventError{87};
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
            std::map<std::u16string,std::u16string> names{{u"application",u"Application"},{u"security",u"Security"},{u"system",u"System"}};
            const auto prefix=std::u16string(1,u'\2')+std::u16string(eventlog_prefix);
            for(const auto& [key,values]:runtime_.registry.keys){(void)values;if(!key.starts_with(prefix))continue;const auto rest=key.substr(prefix.size());if(rest.empty() || rest.find(u'\\')!=rest.npos || names.contains(rest))continue;
                const auto display=runtime_.registry.display_names.find(key);names.emplace(rest,display!=runtime_.registry.display_names.end() && !display->second.empty()?display->second.substr(display->second.find_last_of(u'\\')+1):rest);}
            EventChannelEnum enumeration;for(auto& [lowered,display]:names){(void)lowered;enumeration.names.push_back(display);}
            const auto handle=new_handle();logs.channel_enums.emplace(handle,std::move(enumeration));result=handle;
        }else if(symbol=="EvtNextChannelPath"){
            // (ChannelEnum, BufferSize, Buffer, BufferUsed)：大小以字元計且含結尾 NUL；緩衝不足時不前進。
            const auto found=logs.channel_enums.find(arg(0));if(found==logs.channel_enums.end())throw EventError{6};if(!arg(3))throw EventError{87};
            auto& enumeration=found->second;if(enumeration.position>=enumeration.names.size())throw EventError{259};
            const auto& name=enumeration.names[enumeration.position];memory.store(GuestAddress{arg(3)},32,name.size()+1);
            if(static_cast<std::uint32_t>(arg(1))<name.size()+1 || !arg(2))throw EventError{122};
            memory.write(GuestAddress{arg(2)},utf16_bytes(name));++enumeration.position;result=1;
        }else if(symbol=="EvtOpenChannelConfig"){
            session(arg(0));if(static_cast<std::uint32_t>(arg(2)))throw EventError{87};
            const auto [lowered,display]=channel(string(arg(1)));
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
            EventChannelConfig config;config.channel=lowered;config.display=display;
            config.max_size=dword_value(registry_value(lowered,u"maxsize"),20971520U);config.retention=dword_value(registry_value(lowered,u"retention"),0)!=0;
            config.auto_backup=dword_value(registry_value(lowered,u"autobackuplogfiles"),0)!=0;
            config.file=string_value_of(registry_value(lowered,u"file"),u"%SystemRoot%\\System32\\Winevt\\Logs\\"+display+u".evtx");
            // CustomSD 沒有設定時：SYSTEM 完整控制、系統管理員讀寫清除，Security 記錄只給 SYSTEM 與系統管理員讀取、Event Log Readers 讀取。
            config.access=string_value_of(registry_value(lowered,u"customsd"),lowered==u"security"?u"O:BAG:SYD:(A;;0xf0005;;;SY)(A;;0x5;;;BA)(A;;0x1;;;S-1-5-32-573)":
                u"O:BAG:SYD:(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x3;;;IU)(A;;0x3;;;SU)(A;;0x3;;;S-1-5-3)(A;;0x1;;;S-1-5-32-573)");
            const auto handle=new_handle();logs.channel_configs.emplace(handle,std::move(config));result=handle;
        }else if(symbol=="EvtGetChannelConfigProperty"){
            // (ChannelConfig, PropertyId, Flags, BufferSize, Buffer, BufferUsed)：傳統記錄沒有發佈設定（10～18、20），值為 EvtVarTypeNull。
            const auto found=logs.channel_configs.find(arg(0));if(found==logs.channel_configs.end())throw EventError{6};if(static_cast<std::uint32_t>(arg(2)))throw EventError{87};
            const auto& config=found->second;const auto id=static_cast<std::uint32_t>(arg(1));const auto size=static_cast<std::uint32_t>(arg(3));
            const auto isolation=config.channel==u"application"?0U:config.channel==u"system" || config.channel==u"security"?1U:0U;
            std::vector<std::u16string> strings;
            switch(id){
            case 0:case 4:write_variant(13,1,nullptr,arg(4),size,arg(5));break;                    // Enabled、ClassicEventlog
            case 1:write_variant(8,isolation,nullptr,arg(4),size,arg(5));break;                    // Isolation
            case 2:write_variant(8,0,nullptr,arg(4),size,arg(5));break;                            // Type：EvtChannelTypeAdmin
            case 3:strings={u""};write_variant(1,0,&strings,arg(4),size,arg(5));break;            // OwningPublisher
            case 5:strings={config.access};write_variant(1,0,&strings,arg(4),size,arg(5));break;  // Access（SDDL）
            case 6:write_variant(13,config.retention,nullptr,arg(4),size,arg(5));break;
            case 7:write_variant(13,config.auto_backup,nullptr,arg(4),size,arg(5));break;
            case 8:write_variant(10,config.max_size,nullptr,arg(4),size,arg(5));break;
            case 9:strings={config.file};write_variant(1,0,&strings,arg(4),size,arg(5));break;
            case 19:{
                // PublisherList：登錄中登記到這個記錄的事件來源（子機碼）。
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
                const auto prefix=std::u16string(1,u'\2')+std::u16string(eventlog_prefix)+config.channel+u"\\";std::map<std::u16string,std::u16string> sources;
                for(const auto& [key,values]:runtime_.registry.keys){(void)values;if(!key.starts_with(prefix))continue;const auto rest=key.substr(prefix.size());if(rest.empty() || rest.find(u'\\')!=rest.npos)continue;
                    const auto display=runtime_.registry.display_names.find(key);sources.emplace(rest,display!=runtime_.registry.display_names.end() && !display->second.empty()?display->second.substr(display->second.find_last_of(u'\\')+1):rest);}
                for(auto& [lowered,display]:sources){(void)lowered;strings.push_back(display);}
                write_variant(0x81,0,&strings,arg(4),size,arg(5));break;
            }
            default:if(id<=20){write_variant(0,0,nullptr,arg(4),size,arg(5));break;}throw EventError{87};
            }
            result=1;
        }else if(symbol=="EvtSetChannelConfigProperty"){
            // (ChannelConfig, PropertyId, Flags, PropertyValue)：可設定 Access、Retention、AutoBackup、MaxSize、LogFilePath；傳統記錄不能停用。
            const auto found=logs.channel_configs.find(arg(0));if(found==logs.channel_configs.end())throw EventError{6};if(static_cast<std::uint32_t>(arg(2)) || !arg(3))throw EventError{87};
            auto& config=found->second;const auto id=static_cast<std::uint32_t>(arg(1));const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+12},32));
            const auto value=memory.load(GuestAddress{arg(3)},64);const auto pointer=memory.load(GuestAddress{arg(3)},bits);
            const auto need=[&](std::uint32_t expected){if(type!=expected)throw EventError{87};};
            if(id==0){need(13);if(!static_cast<std::uint32_t>(value))throw EventError{87};}
            else if(id==5){need(1);const auto text=string(pointer);if(text.empty() || text.size()>4096 || text.find_first_of(u"OGDS")!=0 || text[1]!=u':')throw EventError{87};config.access=text;config.dirty|=1U<<5;}
            else if(id==6){need(13);config.retention=static_cast<std::uint32_t>(value)!=0;config.dirty|=1U<<6;}
            else if(id==7){need(13);config.auto_backup=static_cast<std::uint32_t>(value)!=0;config.dirty|=1U<<7;}
            else if(id==8){need(10);if(value<0x10000 || value>0xffff0000U)throw EventError{87};config.max_size=(value+0xffff)&~UINT64_C(0xffff);config.dirty|=1U<<8;}
            else if(id==9){need(1);const auto text=string(pointer);if(text.empty() || text.size()>260)throw EventError{87};config.file=text;config.dirty|=1U<<9;}
            else throw EventError{87};
            result=1;
        }else if(symbol=="EvtSaveChannelConfig"){
            // 只寫回改過的值；經 advapi32 的登錄 API，HKLM 的存取檢查與一般程式相同。
            const auto found=logs.channel_configs.find(arg(0));if(found==logs.channel_configs.end())throw EventError{6};if(static_cast<std::uint32_t>(arg(1)))throw EventError{87};
            auto& config=found->second;GuestScratch scratch(*this,memory,state);
            const auto text=[&](const std::u16string& value){const auto bytes=utf16_bytes(value);const auto at=scratch.allocate(bytes.size());if(!at)throw EventError{8};memory.write(GuestAddress{at},bytes);return std::pair{at,static_cast<std::uint64_t>(bytes.size())};};
            const auto key_slot=scratch.allocate(16);if(!key_slot)throw EventError{8};memory.store(GuestAddress{key_slot},bits,0);
            const auto path=text(u"SYSTEM\\CurrentControlSet\\Services\\EventLog\\"+config.display);
            if(const auto error=static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll","RegCreateKeyExW",{0x80000002U,path.first,0,0,0,0x2U,0,key_slot,0})))throw EventError{error};
            const auto key=memory.load(GuestAddress{key_slot},bits);std::uint32_t error=0;
            const auto set_dword=[&](const char16_t* name,std::uint32_t data){if(error)return;const auto value_name=text(name);const auto cell=scratch.allocate(4);if(!cell){error=8;return;}memory.store(GuestAddress{cell},32,data);
                error=static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll","RegSetValueExW",{key,value_name.first,0,4,cell,4}));};
            const auto set_text=[&](const char16_t* name,const std::u16string& data,std::uint32_t type){if(error)return;const auto value_name=text(name);const auto value=text(data);
                error=static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll","RegSetValueExW",{key,value_name.first,0,type,value.first,value.second}));};
            if(config.dirty&(1U<<8))set_dword(u"MaxSize",static_cast<std::uint32_t>(config.max_size));
            if(config.dirty&(1U<<6))set_dword(u"Retention",config.retention?UINT32_MAX:0U);
            if(config.dirty&(1U<<7))set_dword(u"AutoBackupLogFiles",config.auto_backup?1U:0U);
            if(config.dirty&(1U<<9))set_text(u"File",config.file,2);
            if(config.dirty&(1U<<5))set_text(u"CustomSD",config.access,1);
            (void)nested_service(state,memory,"advapi32.dll","RegCloseKey",{key});if(error)throw EventError{error};config.dirty=0;result=1;
        }else if(symbol=="EvtOpenLog"){
            // (Session, Path, Flags)：EvtOpenChannelPath（1）或 EvtOpenFilePath（2）。
            session(arg(0));const auto flags=static_cast<std::uint32_t>(arg(2));if(flags!=1 && flags!=2)throw EventError{87};
            EventLogFile log;
            if(flags==1){const auto [lowered,display]=channel(string(arg(1)));log.channel=lowered;log.display=display;}
            else{log.path=string(arg(1));(void)file_records(log.path);log.from_file=true;}
            const auto handle=new_handle();logs.log_files.emplace(handle,std::move(log));result=handle;
        }else if(symbol=="EvtGetLogInfo"){
            // (Log, PropertyId, BufferSize, Buffer, BufferUsed)：CreationTime、LastAccessTime、LastWriteTime、FileSize、Attributes、NumberOfLogRecords、OldestRecordNumber、Full。
            const auto found=logs.log_files.find(arg(0));if(found==logs.log_files.end())throw EventError{6};const auto id=static_cast<std::uint32_t>(arg(1));if(id>7)throw EventError{87};
            const auto& log=found->second;std::array<std::uint64_t,5> info{};std::vector<EventRecord> records;
            if(log.from_file){
                auto opened=filesystem_.open_reader(utf16_to_utf8(log.path));if(opened.error)throw EventError{opened.error};
                info={opened.metadata.created,opened.metadata.accessed,opened.metadata.modified,opened.metadata.size,opened.metadata.attributes};records=file_records(log.path);
            }else{
                const auto path=log_directory()/record_filename(log.channel);struct stat status{};
                if(::stat(path.c_str(),&status)==0)info={filetime_of(status.st_birthtimespec),filetime_of(status.st_atimespec),filetime_of(status.st_mtimespec),static_cast<std::uint64_t>(status.st_size),0x20};
                records=read_records(path,log.display);
            }
            const std::array<std::uint32_t,8> types{17,17,17,10,8,10,10,13};
            const std::uint64_t value=id<5?info[id]:id==5?records.size():id==6?(records.empty()?0:record_number(records.front())):(!log.from_file && info[3]+0x40000>=32*1024*1024);
            write_variant(types[id],value,nullptr,arg(3),static_cast<std::uint32_t>(arg(2)),arg(4));result=1;
        }else if(symbol=="EvtClearLog"){
            // (Session, ChannelPath, TargetFilePath, Flags)：有目標檔時先匯出，匯出失敗不清除；Security 記錄需要 SeSecurityPrivilege。
            session(arg(0));if(static_cast<std::uint32_t>(arg(3)))throw EventError{87};const auto [lowered,display]=channel(string(arg(1)));if(lowered==u"security")throw EventError{5};
            const auto path=log_directory()/record_filename(lowered);
            if(arg(2)){
                const auto bytes=backup_bytes(read_records(path,display));auto opened=filesystem_.open_host(utf16_to_utf8(string(arg(2))),0x40000000U,1,0x80,0);if(opened.error)throw EventError{opened.error};
                const auto written=opened.file->write(bytes,0);if(written.error)throw EventError{written.error};if(written.bytes!=bytes.size())throw EventError{112};if(const auto flushed=opened.file->flush())throw EventError{flushed};
            }
            clear_records(path);for(auto& [handle,reader]:logs.readers){(void)handle;if(!reader.from_backup && reader.channel==lowered){reader.position=0;reader.positioned=false;}}
            for(auto& [handle,subscription]:logs.subscriptions){(void)handle;if(subscription.channel==lowered)subscription.next=0;}
            result=1;
        }else if(symbol=="EvtExportLog"){
            // (Session, Path, Query, TargetFilePath, Flags)：ChannelPath 1／FilePath 2、TolerateQueryErrors 0x1000、Overwrite 0x2000；目標檔已存在且未指定覆寫時回 ERROR_FILE_EXISTS。
            session(arg(0));const auto flags=static_cast<std::uint32_t>(arg(4));if((flags&~0x3003U) || ((flags&3)!=1 && (flags&3)!=2) || !arg(3))throw EventError{87};
            const auto bytes=backup_bytes(select(arg(1),arg(2),(flags&3)==2));
            auto opened=filesystem_.open_host(utf16_to_utf8(string(arg(3))),0x40000000U,(flags&0x2000)?2U:1U,0x80,0);if(opened.error)throw EventError{opened.error};
            const auto written=opened.file->write(bytes,0);if(written.error)throw EventError{written.error};if(written.bytes!=bytes.size())throw EventError{112};if(const auto flushed=opened.file->flush())throw EventError{flushed};
            result=1;
        }else throw HostCallFault("未知的 wevtapi 服務："+std::string(symbol));
    }catch(const EventError& error){last_error_=error.code;result=0;}catch(const MemoryFault&){last_error_=998;result=0;}catch(const std::bad_alloc&){last_error_=8;result=0;}
    arg.return_to(state,result);return std::nullopt;
}
