// dnsapi 的 DNS-SD 與 mDNS（由 windows_dns_records.cpp 的 invoke_dns_records 引入），以宿主 Bonjour（dns_sd）承接：
// - DnsServiceBrowse（v1 回呼收 PTR 記錄串列、v2 收 DNS_QUERY_RESULT）、DnsServiceResolve（SRV／TXT 後以 DNSServiceGetAddrInfo 取位址，回 DNS_SERVICE_INSTANCE）、
//   DnsServiceRegister（實例全名拆成名稱、服務型別與網域；有主機名稱與位址時另登錄 A／AAAA；回實際註冊的名稱）、DnsServiceDeRegister、DnsStartMulticastQuery。
// - 非同步操作回 DNS_REQUEST_PENDING（mDNS 查詢回 ERROR_SUCCESS），操作 id 存在 DNS_SERVICE_CANCEL.reserved 或 MDNS_QUERY_HANDLE.pSubscription。
// - 回呼在執行緒池由 internal-dnssd Worker 送出：輪詢 dns_sd socket（沒有資料時 Sleep 20 毫秒），回呼期間 delivering 為該執行緒；
//   取消在回呼正於其他執行緒執行時等它結束，在回呼中取消則回呼返回後由工作執行緒釋放。記錄串列由呼叫端以 DnsRecordListFree、實例以 DnsServiceFreeInstance 釋放。
// - DnsAcquireContextHandle_A／W／UTF8 保存 SEC_WINNT_AUTH_IDENTITY 的使用者與網域（不保存密碼），DnsReleaseContextHandle 釋放。
if(service.family=="internal-dnssd" && symbol=="Worker"){
    return nested_continuation(state,memory,"DNS-SD 回呼",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall frame(s,m,service);const auto id=frame(0);const unsigned unit=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(unit*8);
        const auto sleep=resolve("kernel32.dll","Sleep").value;
        for(;;){
            if(!dns_services_)break;auto& operations=dns_services_->operations;const auto found=operations.find(id);if(found==operations.end())break;
            auto& operation=*found->second;
            if(operation.canceled){operations.erase(found);break;}
            if(!operation.events.empty()){
                auto event=std::move(operation.events.front());operation.events.pop_front();
                std::vector<std::uint64_t> arguments;std::uint64_t result_block=0,records=0,instance=0;
                try{
                    if(!event.records.empty())records=dns_store_records(s,m,event.records,1);
                    if(event.instance)instance=dns_store_instance(m,*event.instance);
                }catch(const dns_detail::Error& error){event.status=error.code;records=0;}
                using Kind=DnsServiceState::Kind;
                if(operation.kind==Kind::BrowseV2 || operation.kind==Kind::Multicast){
                    // DNS_QUERY_RESULT：Version、QueryStatus、QueryOptions、pQueryRecords、Reserved；結構只在回呼期間有效。
                    result_block=allocate_heap(m,x86_?24U:32U);
                    if(result_block){m.write(GuestAddress{result_block},std::vector<std::uint8_t>(x86_?24U:32U));m.store(GuestAddress{result_block},32,1);m.store(GuestAddress{result_block+4},32,event.status);m.store(GuestAddress{result_block+16},width,records);}
                    arguments=operation.kind==Kind::Multicast?std::vector<std::uint64_t>{operation.context,operation.handle,result_block}:std::vector<std::uint64_t>{operation.context,result_block};
                }else if(operation.kind==Kind::BrowseV1)arguments={event.status,operation.context,records};
                else arguments={event.status,operation.context,instance};
                if(operation.kind==Kind::Resolve || operation.kind==Kind::DeRegister)operation.done=true;
                if(operation.kind==Kind::Register){if(event.status)operation.done=true;else operation.idle=true;}
                operation.delivering=threads_.current;const auto target=operation.callback;
                if(!result_block && (operation.kind==Kind::BrowseV2 || operation.kind==Kind::Multicast)){/* 配置失敗：不呼叫回呼，記錄交還 */
                    operation.delivering=0;if(records)for(auto at=records;at;){const auto next=m.load(GuestAddress{at},width);runtime_.network.addresses.erase(at);(void)free_heap(m,at,s);at=next;}continue;}
                (void)co_await service_callback::call(target,arguments);
                if(result_block)(void)free_heap(m,result_block,s);
                if(dns_services_)if(const auto again=dns_services_->operations.find(id);again!=dns_services_->operations.end())again->second->delivering=0;
                continue;
            }
            if(operation.done){operations.erase(found);break;}
            // 註冊完成後不需要再處理回應：保留連線（服務維持註冊）並讓出工作執行緒。
            if(operation.idle){break;}
            if(operation.kind==DnsServiceState::Kind::Resolve && std::chrono::steady_clock::now()>operation.deadline){operation.events.push_back({1460,{},std::nullopt});operation.done=true;continue;}
            std::array<pollfd,3> fds{};std::array<DNSServiceRef,3> refs{};unsigned count=0;
            for(const auto ref:{operation.ref,operation.address_ref,operation.connection})if(ref){fds[count]={DNSServiceRefSockFD(ref),POLLIN,0};refs[count++]=ref;}
            const auto ready=count?::poll(fds.data(),count,0):0;
            if(ready>0){
                for(unsigned i=0;i<count;++i)if(fds[i].revents&(POLLIN|POLLHUP|POLLERR)){
                    if(const auto error=DNSServiceProcessResult(refs[i])){operation.events.push_back({dnssd_status(error),{},std::nullopt});operation.done=true;break;}
                    // 回呼可能已變更操作（例如解析開始取位址），其餘 fd 下一輪再處理。
                    break;
                }
                continue;
            }
            (void)co_await service_callback::call(sleep,{20});
        }
        frame.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
}
if(symbol=="DnsServiceBrowseCancel" || symbol=="DnsServiceResolveCancel" || symbol=="DnsServiceRegisterCancel" || symbol=="DnsStopMulticastQuery"){
    // (pCancelHandle) 或 (pHandle)：找不到操作時回 ERROR_CANCELLED（mDNS 為 ERROR_INVALID_PARAMETER）。整個呼叫都在續行中處理：
    // 等待其他執行緒的回呼結束後，服務重新進入時由同一續行取回結果，不重新判斷操作是否存在。
    return nested_continuation(state,memory,"取消 DNS-SD",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall frame(s,m,service);const bool multicast=service.symbol=="DnsStopMulticastQuery";const unsigned width=x86_?32U:64U;
        if(!frame(0)){frame.return_to(s,87);co_return std::optional<std::uint32_t>{};}
        const auto id=m.load(GuestAddress{frame(0)+(multicast?(x86_?516U:520U):0U)},static_cast<std::uint8_t>(width));
        if(!dns_services_ || !dns_services_->operations.contains(id)){frame.return_to(s,multicast?87U:1223U);co_return std::optional<std::uint32_t>{};}
        // 已送出最後結果的操作（解析完成）沒有可取消的內容，由工作執行緒自行釋放。
        if(const auto& operation=*dns_services_->operations.at(id);operation.done && operation.events.empty()){frame.return_to(s,multicast?87U:1223U);co_return std::optional<std::uint32_t>{};}
        dns_services_->operations.at(id)->canceled=true;const auto sleep=resolve("kernel32.dll","Sleep").value;
        for(;;){
            if(!dns_services_)break;const auto item=dns_services_->operations.find(id);if(item==dns_services_->operations.end())break;
            // 在回呼中取消：回呼返回後由工作執行緒釋放。
            if(item->second->delivering==threads_.current)break;
            if(!item->second->delivering){dns_services_->operations.erase(item);break;}
            (void)co_await service_callback::call(sleep,{1});
        }
        frame.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
}
if(symbol=="DnsServiceBrowse" || symbol=="DnsServiceResolve" || symbol=="DnsServiceRegister" || symbol=="DnsServiceDeRegister" || symbol=="DnsStartMulticastQuery"){
    // 請求結構：Version、InterfaceIndex 之後依序為名稱或實例指標、回呼、內容（註冊另有 hCredentials、unicastEnabled）；MDNS_QUERY_REQUEST 見下。
    constexpr std::uint32_t pending=9506;
    const auto request=call(0),cancel=call(1);
    const auto fail=[&](std::uint32_t code){call.return_to(state,code);return std::optional<std::uint32_t>{};};
    const bool multicast=symbol=="DnsStartMulticastQuery";
    if(!request || (!cancel && (symbol=="DnsServiceBrowse" || symbol=="DnsServiceResolve" || multicast)))return fail(87);
    const auto version=static_cast<std::uint32_t>(load(request,32));const auto interface_index=static_cast<std::uint32_t>(load(request+4,32));
    if(version!=1 && !(version==2 && symbol=="DnsServiceBrowse"))return fail(87);
    const auto name_at=load(request+8,width);
    // 名稱或實例指標在偏移 8，之後為回呼與內容（x86 為 12／16，x64 為 16／24）。
    const auto callback=multicast?load(request+(x86_?28U:40U),width):load(request+8+unit,width),context=multicast?load(request+(x86_?32U:48U),width):load(request+8+2*unit,width);
    if(!name_at || !callback)return fail(87);
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行 DNS-SD");
    try{
    if(!dns_services_)dns_services_=std::make_shared<DnsServiceState>();
    auto& services=*dns_services_;if(services.operations.size()>=256)return fail(14);
    auto operation=std::make_unique<DnsServiceState::Operation>();operation->callback=callback;operation->context=context;operation->interface_index=interface_index;
    DNSServiceErrorType error=kDNSServiceErr_NoError;
    // 實例：讀取 DNS_SERVICE_INSTANCE（名稱、主機、位址、連接埠、優先序、權重、屬性、介面）。
    const auto read_instance=[&](std::uint64_t at){
        DnsServiceInstanceData data;const auto wide=[&](std::uint64_t pointer){return read_utf16(memory,GuestAddress{pointer},1024);};
        if(const auto p=load(at,width))data.name=wide(p);if(const auto p=load(at+unit,width))data.host=wide(p);
        if(const auto p=load(at+2*unit,width)){std::array<std::uint8_t,4> ip{};memory.read(GuestAddress{p},ip);data.ip4=ip;}
        if(const auto p=load(at+3*unit,width)){std::array<std::uint8_t,16> ip{};memory.read(GuestAddress{p},ip);data.ip6=ip;}
        data.port=static_cast<std::uint16_t>(load(at+4*unit,16));data.priority=static_cast<std::uint16_t>(load(at+4*unit+2,16));data.weight=static_cast<std::uint16_t>(load(at+4*unit+4,16));
        const auto count=static_cast<std::uint32_t>(load(at+(x86_?24U:40U),32));const auto keys=load(at+(x86_?28U:48U),width),values=load(at+(x86_?32U:56U),width);
        if(count>256 || (count && (!keys || !values)))throw Error{87};
        for(std::uint32_t i=0;i<count;++i){const auto key=load(keys+i*unit,width),value=load(values+i*unit,width);data.properties.push_back({key?std::optional{wide(key)}:std::nullopt,value?std::optional{wide(value)}:std::nullopt});}
        data.interface_index=static_cast<std::uint32_t>(load(at+(x86_?36U:64U),32));return data;
    };
    if(symbol=="DnsServiceBrowse"){
        const auto parts=dnssd_split(utf16_to_utf8(read_utf16(memory,GuestAddress{name_at},1024)),false);if(!parts)return fail(123);
        operation->kind=version==2?DnsServiceState::Kind::BrowseV2:DnsServiceState::Kind::BrowseV1;
        error=DNSServiceBrowse(&operation->ref,0,interface_index,(*parts)[1].c_str(),(*parts)[2].empty()?nullptr:(*parts)[2].c_str(),dnssd_browse_reply,operation.get());
    }else if(symbol=="DnsServiceResolve"){
        const auto parts=dnssd_split(utf16_to_utf8(read_utf16(memory,GuestAddress{name_at},1024)),true);if(!parts)return fail(123);
        operation->kind=DnsServiceState::Kind::Resolve;operation->deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        operation->instance.name=read_utf16(memory,GuestAddress{name_at},1024);
        error=DNSServiceResolve(&operation->ref,0,interface_index,(*parts)[0].c_str(),(*parts)[1].c_str(),(*parts)[2].empty()?"local.":(*parts)[2].c_str(),dnssd_resolve_reply,operation.get());
    }else if(multicast){
        // MDNS_QUERY_REQUEST：Version、ulRefCount、Query、QueryType、QueryOptions、InterfaceIndex、pQueryCallback、pQueryContext；MDNS_QUERY_HANDLE：nameBuf[256]、wType、pSubscription。
        const auto query=read_utf16(memory,GuestAddress{name_at},255);const auto type=static_cast<std::uint16_t>(load(request+(x86_?12U:16U),16));
        const auto index=static_cast<std::uint32_t>(load(request+(x86_?24U:32U),32));if(!type)return fail(87);
        operation->kind=DnsServiceState::Kind::Multicast;operation->handle=cancel;operation->interface_index=index;
        memory.check(GuestAddress{cancel},x86_?532U:544U,Access::Write);
        auto name_bytes=utf16_bytes(query);name_bytes.resize(512);memory.write(GuestAddress{cancel},name_bytes);memory.store(GuestAddress{cancel+512},16,type);
        error=DNSServiceQueryRecord(&operation->ref,0,index,utf16_to_utf8(query).c_str(),type,kDNSServiceClass_IN,dnssd_query_reply,operation.get());
    }else{
        auto data=read_instance(name_at);if(!data.name)return fail(87);
        const auto parts=dnssd_split(utf16_to_utf8(*data.name),true);if(!parts)return fail(123);
        if(symbol=="DnsServiceDeRegister"){
            // 找到同名的註冊並撤銷（關閉 Bonjour 連線即撤銷），完成時以本請求的回呼回報 ERROR_SUCCESS。
            const auto target=dns_detail::lower(utf16_to_utf8(*data.name));
            const auto registered=std::find_if(services.operations.begin(),services.operations.end(),[&](const auto& item){
                return item.second->kind==DnsServiceState::Kind::Register && !item.second->canceled && item.second->instance.name && dns_detail::lower(utf16_to_utf8(*item.second->instance.name))==target;});
            if(registered==services.operations.end())return fail(1168);
            if(registered->second->delivering)registered->second->canceled=true;else services.operations.erase(registered);
            operation->kind=DnsServiceState::Kind::DeRegister;operation->events.push_back({0,{},data});
        }else{
            operation->kind=DnsServiceState::Kind::Register;operation->instance=data;
            const auto txt=dnssd_txt(data);const auto host=data.host?utf16_to_utf8(*data.host):std::string{};
            if(!host.empty() && (data.ip4 || data.ip6)){
                // 主機的 A／AAAA 與服務共用同一個 Bonjour 連線，撤銷時一併移除。
                error=DNSServiceCreateConnection(&operation->connection);
                for(int family=0;!error && family<2;++family){
                    if(family==0 && !data.ip4)continue;if(family==1 && !data.ip6)continue;DNSRecordRef record{};
                    error=DNSServiceRegisterRecord(operation->connection,&record,kDNSServiceFlagsShared,interface_index,host.c_str(),family?kDNSServiceType_AAAA:kDNSServiceType_A,kDNSServiceClass_IN,
                        family?16:4,family?static_cast<const void*>(data.ip6->data()):static_cast<const void*>(data.ip4->data()),120,dnssd_record_reply,operation.get());
                    if(!error)operation->address_records.push_back(record);
                }
            }
            if(!error)error=DNSServiceRegister(&operation->ref,0,interface_index,(*parts)[0].c_str(),(*parts)[1].c_str(),(*parts)[2].empty()?nullptr:(*parts)[2].c_str(),host.empty()?nullptr:host.c_str(),
                htons(data.port),static_cast<std::uint16_t>(txt.size()),txt.data(),dnssd_register_reply,operation.get());
        }
    }
    if(error)return fail(dnssd_status(error));
    const auto id=services.next++;auto* raw=operation.get();services.operations.emplace(id,std::move(operation));
    if(cancel && !multicast){memory.check(GuestAddress{cancel},unit,Access::Write);memory.store(GuestAddress{cancel},width,id);}
    if(multicast)memory.store(GuestAddress{cancel+(x86_?516U:520U)},width,id);
    (void)raw;
    if(const auto queued=queue_pool_callback(state,memory,{resolve("internal-dnssd","Worker").value,{id}})){services.operations.erase(id);return fail(queued);}
    return fail(multicast?0U:pending);
    }catch(const Error& error){return fail(error.code);}catch(const MemoryFault&){return fail(87);}
}
if(symbol.starts_with("DnsAcquireContextHandle_") || symbol=="DnsReleaseContextHandle"){
    if(symbol=="DnsReleaseContextHandle"){if(dns_services_)dns_services_->contexts.erase(call(0));call.return_to(state,0);return std::nullopt;}
    // (CredentialFlags, pCredentials, pContextHandle)：pCredentials 為 SEC_WINNT_AUTH_IDENTITY（User、UserLength、Domain、DomainLength、Password、PasswordLength、Flags）或 NULL（目前使用者）。
    if(!call(2)){call.return_to(state,87);return std::nullopt;}
    DnsServiceState::Context context;
    if(const auto identity=call(1)){
        const std::uint64_t user=load(identity,width),domain=load(identity+(x86_?8U:16U),width);
        const auto user_length=static_cast<std::uint32_t>(load(identity+unit,32)),domain_length=static_cast<std::uint32_t>(load(identity+(x86_?12U:24U),32));
        const auto flags=static_cast<std::uint32_t>(load(identity+(x86_?24U:44U),32));const bool wide=symbol.ends_with("_W")?true:symbol.ends_with("_A")?false:(flags&2U)!=0;
        if(user_length>256 || domain_length>256){call.return_to(state,87);return std::nullopt;}
        const auto string=[&](std::uint64_t at,std::uint32_t length)->std::u16string{if(!at)return {};if(wide){std::u16string out;for(std::uint32_t i=0;i<length;++i)out.push_back(static_cast<char16_t>(load(at+i*2,16)));return out;}
            std::string out;for(std::uint32_t i=0;i<length;++i)out.push_back(static_cast<char>(load(at+i,8)));return utf8_to_utf16_lossy(out);};
        context.user=string(user,user_length);context.domain=string(domain,domain_length);
    }
    if(!dns_services_)dns_services_=std::make_shared<DnsServiceState>();if(dns_services_->contexts.size()>=1024){call.return_to(state,14);return std::nullopt;}
    const auto handle=dns_services_->next_context++;dns_services_->contexts.emplace(handle,std::move(context));
    memory.check(GuestAddress{call(2)},unit,Access::Write);memory.store(GuestAddress{call(2)},width,handle);call.return_to(state,0);return std::nullopt;
}
