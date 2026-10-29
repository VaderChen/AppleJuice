// 包含於 windows_files.cpp；loader、開檔與屬性查詢共用同後端客體回呼。
namespace {
const std::shared_ptr<RetainedCapacity>& native_file_temporary_budget() {
    // 開檔、屬性、handle、section、讀寫與定位共用額度；scratch 預約合計不超過 1 MiB。
    static const auto value=std::make_shared<RetainedCapacity>(retained_capacity_setting("X64ARM_NATIVE_FILE_QUERY_BYTES",1024U*1024U));
    return value;
}
bool native_file_trace_slot(bool enabled,std::atomic<unsigned>& count) {
    if(!enabled)return false;
    auto seen=count.load(std::memory_order_relaxed);
    while(seen<128){if(count.compare_exchange_weak(seen,seen+1,std::memory_order_relaxed))return true;}
    return false;
}
}
std::uint32_t WindowsServices::call_guest_native_file(GuestState& state,GuestMemory& memory,
        std::uint64_t target,std::vector<std::uint64_t> arguments) {
    if(!has_nested_runner())throw HostCallFault("目前執行範圍無法完成客體檔案回呼");
    const auto saved=state;const auto depth=runtime_.callbacks.size();const auto owner=threads_.current;
    // 有界開檔診斷只記錄參數的借用區間，不保留路徑或擴張客體配置生命週期。
    static const bool trace_enabled=std::getenv("X64ARM_TRACE_NATIVE_OPEN")!=nullptr;
    static std::atomic<unsigned> traced{};
    const bool trace=(arguments.size()==6 || arguments.size()==11) && native_file_trace_slot(trace_enabled,traced);
    const auto attributes=trace?arguments[2]:0;
    const auto describe=[&](const char* phase){
        if(!trace)return;
        bool readable=true;
        try{memory.check(GuestAddress{attributes},x86_?24:48,Access::Read);}catch(const MemoryFault&){readable=false;}
        const auto object=allocations_.upper_bound(attributes);
        const auto allocation=object==allocations_.begin()?allocations_.end():std::prev(object);
        const bool owned=allocation!=allocations_.end() && attributes-allocation->first<allocation->second;
        std::fprintf(stderr,"[native_open] phase=%s pid=%u owner=%llu current=%llu depth=%zu/%zu target=%llx attributes=%llx readable=%u allocation=%llx bytes=%zu rip=%llx rsp=%llx\n",
            phase,process_id(),static_cast<unsigned long long>(owner),static_cast<unsigned long long>(threads_.current),
            depth,runtime_.callbacks.size(),static_cast<unsigned long long>(target),static_cast<unsigned long long>(attributes),readable?1U:0U,
            static_cast<unsigned long long>(owned?allocation->first:0),owned?allocation->second:0,
            static_cast<unsigned long long>(state.rip.value),static_cast<unsigned long long>(state.reg(Gpr::Rsp)));
    };
    describe("enter");
    struct TraceEnd {const decltype(describe)& describe;~TraceEnd(){describe("leave");}} trace_end{describe};
    // 保留真正的呼叫框架；回呼只傳回結果，尚未返回外層 Windows API。
    try {start_callbacks(state,memory,{{target,std::move(arguments)}},Continuation::NestedResult);}
    catch(...){state=saved;throw;}
    const auto stopped=[&]{return threads_.current==owner && runtime_.callbacks.size()==depth && state.rip==saved.rip;};
    const auto result=run_nested(state,memory,stopped);
    if(!stopped())throw NestedExecutionStop(result);
    const auto value=static_cast<std::uint32_t>(state.reg(Gpr::Rax));const auto tsc=state.tsc;
    state=saved;state.tsc=tsc;return value;
}

std::pair<std::uint64_t,bool> WindowsServices::native_file_entry(GuestMemory& memory,std::string_view symbol) {
    const auto native=find_module(u"ntdll.dll");const auto* image=module_image(native);
    if(!image || !runtime_.builtin_images.contains(native))return {};
    const auto name=image->export_names.find(std::string(symbol));
    if(name==image->export_names.end())throw HostCallFault("缺少 Native 檔案入口");
    const auto target=native+image->export_ordinals.at(name->second),gateway=resolve("ntdll.dll",symbol).value;
    std::array<std::uint8_t,16> expected{},actual{};
    const auto put=[&](unsigned at,unsigned size,std::uint64_t value){for(unsigned j=0;j<size;++j)expected[at+j]=static_cast<std::uint8_t>(value>>(j*8));};
    if(x86_){expected[0]=0xff;expected[1]=0x25;put(2,4,target+6);put(6,4,gateway);}
    else{expected={0x4c,0x8b,0xd1,0xb8,0,0,0,0,0x0f,0x05,0xc3,0x66,0x66,0x90,0x66,0x90};put(4,4,(gateway-stub_base_.value)/16+1);}
    memory.read(GuestAddress{target},actual);return {target,actual!=expected};
}

std::optional<std::uint64_t> WindowsServices::invoke_native_file_io(GuestState& entry_state,GuestMemory& entry_memory,
        const WindowsServiceDescriptor& service) {
    return service_callback::invoke<std::optional<std::uint64_t>>(this,threads_.current,entry_state,entry_memory,"invoke_native_file_io",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint64_t>>{
    if(service.family=="internal-nt-file")co_return std::nullopt;
    const auto symbol=service.symbol;const bool cancel=symbol=="CancelIo" || symbol=="CancelIoEx";
    const bool extended=symbol.ends_with("Ex"),writing=symbol.starts_with("Write");
    const WindowsCall arg(state,memory,service);const auto handle=arg(0);
    // 這些入口另有 console／socket／裝置契約，不可送進一般檔案 provider。
    if((handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100]) || console_handle(handle) ||
       runtime_.network.sockets.contains(handle) || kernel_device_handle(handle))co_return std::nullopt;
    const auto known=files_.find(handle);if(known!=files_.end() && known->second.usb)co_return std::nullopt;
    const auto [target,changed]=native_file_entry(memory,cancel?(extended?"NtCancelIoFileEx":"NtCancelIoFile"):
        writing?"NtWriteFile":"NtReadFile");
    if(!changed)co_return std::nullopt;
    const auto saved_error=last_error_;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint64_t>{last_error_=error;return 0;};
    const auto error=[&](std::uint32_t status){return static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));};
    try {
        const auto overlap=cancel?0:arg(extended?3:4),count_pointer=cancel || extended?0:arg(3);
        if(count_pointer){memory.check(GuestAddress{count_pointer},4,Access::Write);memory.store(GuestAddress{count_pointer},32,0);}
        if(!cancel && ((extended && (!overlap || !arg(4))) || (!overlap && !count_pointer)))co_return fail(87);
        if(overlap)memory.check(GuestAddress{overlap},x86_?20:32,Access::Write);
        // 重疊 I/O 的 IOSB 就在 OVERLAPPED 前兩欄，直接沿用客體生命週期。
        // 同步與取消只配置狀態暫存；永不複製 payload 或建立每次回呼對照表。
        auto lease=overlap?std::optional<RetainedCapacityLease>{}:RetainedCapacityLease::acquire(native_file_temporary_budget(),4096);
        if(!overlap && !lease)co_return fail(8);
        const auto scratch=overlap?0:allocate_heap(memory,32);if(!overlap && !scratch)co_return fail(8);
        struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
            ~Release(){if(address)(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
        const auto ios=overlap?overlap:scratch;
        memory.store(GuestAddress{ios},bits,0x103);memory.store(GuestAddress{ios+unit},bits,0);
        std::uint32_t status;
        if(cancel)status=co_await service_callback::call(target,extended?
            std::vector<std::uint64_t>{handle,arg(1),ios}:std::vector<std::uint64_t>{handle,ios});
        else {
            const auto event=overlap && !extended?memory.load(GuestAddress{overlap+(x86_?16:24)},bits):0;
            const auto routine=extended?resolve("internal-file-apc","Complete").value:0;
            const auto context=extended?arg(4):overlap && !(event&1)?overlap:0;
            status=co_await service_callback::call(target,{handle,event,routine,context,ios,arg(1),
                static_cast<std::uint32_t>(arg(2)),overlap?overlap+2*unit:0,0});
        }
        if(!overlap && status==0x103U){
            // 等待完成前，IOSB 與額度一直存活；無逐圈輪詢或提早歸帳。
            const auto waited=co_await service_callback::call(native_file_entry(memory,"NtWaitForSingleObject").first,{handle,0,0});
            status=static_cast<std::uint32_t>(memory.load(GuestAddress{ios},bits));
            if(status==0x103U)throw HostCallFault("Native 同步 I/O 等待未完成，不能釋放仍在使用的 IOSB");
            if(static_cast<std::int32_t>(waited)<0)co_return fail(error(waited));
        }
        static const bool trace_enabled=std::getenv("X64ARM_TRACE_NATIVE_FILE_IO")!=nullptr;
        static std::atomic<unsigned> trace_count{};
        if(native_file_trace_slot(trace_enabled,trace_count))
            std::fprintf(stderr,"[native_file_io] %.*s handle=%llx count=%u bytes=%llu status=%x overlapped=%d\n",
                static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(handle),
                cancel?0U:static_cast<std::uint32_t>(arg(2)),static_cast<unsigned long long>(memory.load(GuestAddress{ios+unit},bits)),status,overlap?1:0);
        if(cancel){if(static_cast<std::int32_t>(status)<0)co_return fail(error(status));last_error_=saved_error;co_return 1;}
        if(status==0x103U){if(!extended)co_return fail(997);last_error_=0;co_return 1;}
        const bool eof=!writing && status==0xc0000011U;
        if(count_pointer && (static_cast<std::int32_t>(status)>=0 || status==0x80000005U || eof))
            memory.store(GuestAddress{count_pointer},32,eof?0:memory.load(GuestAddress{ios+unit},bits));
        if(static_cast<std::int32_t>(status)<0 && !(eof && !overlap) && !(extended && status==0x80000005U))co_return fail(error(status));
        last_error_=extended?(status==0x80000005U?234U:0U):saved_error;co_return 1;
    }catch(const MemoryFault&){co_return fail(998);}catch(const std::bad_alloc&){co_return fail(8);}

        });
}

std::optional<std::uint64_t> WindowsServices::invoke_native_file_completion(GuestState& entry_state,GuestMemory& entry_memory,
        const WindowsServiceDescriptor& service) {
    return service_callback::invoke<std::optional<std::uint64_t>>(this,threads_.current,entry_state,entry_memory,"invoke_native_file_completion",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint64_t>>{
    const WindowsCall arg(state,memory,service);const bool bind=service.symbol=="CreateIoCompletionPort";
    if(bind && arg(0)==(x86_?UINT32_MAX:UINT64_MAX))co_return std::nullopt;
    const auto [target,changed]=native_file_entry(memory,"NtSetInformationFile");if(!changed)co_return std::nullopt;
    const auto saved_error=last_error_;
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint64_t>{last_error_=error;return 0;};
    const auto modes=bind?0U:static_cast<std::uint8_t>(arg(1));
    if(!bind && (modes&~3U))co_return fail(87);
    if(bind && arg(1) && !runtime_.completion_ports.contains(arg(1)))co_return fail(6);
    if(bind && arg(1) && !(runtime_.completion_ports.at(arg(1)).access&2U))co_return fail(5);
    // 僅暫存本次建立的埠；失敗、取消或回呼例外時回收，既有埠不受影響。
    struct Port {RuntimeState& runtime;std::uint64_t handle{};bool owned{};
        ~Port(){if(owned)runtime.completion_ports.erase(handle);}} port{runtime_,bind?arg(1):0,false};
    try {
        if(bind && !port.handle){
            if(runtime_.completion_ports.size()>=1024)co_return fail(8);
            port.handle=runtime_.allocate_wait_handle();runtime_.completion_ports.emplace(port.handle,CompletionPort{});port.owned=true;
        }
        auto lease=RetainedCapacityLease::acquire(native_file_temporary_budget(),4096);if(!lease)co_return fail(8);
        const auto scratch=allocate_heap(memory,64);if(!scratch)co_return fail(8);
        struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
            ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
        const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto info=scratch+32;
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,64>{});memory.store(GuestAddress{scratch},bits,0x103U);
        if(bind){memory.store(GuestAddress{info},bits,port.handle);memory.store(GuestAddress{info+unit},bits,arg(2));}
        else memory.store(GuestAddress{info},32,modes);
        auto status=co_await service_callback::call(target,{arg(0),scratch,info,bind?2*unit:4U,bind?30U:41U});
        if(status==0x103U){
            const auto waited=co_await service_callback::call(native_file_entry(memory,"NtWaitForSingleObject").first,{arg(0),0,0});
            status=static_cast<std::uint32_t>(memory.load(GuestAddress{scratch},bits));
            if(status==0x103U)throw HostCallFault("Native 完成埠資訊等待未完成");
            if(static_cast<std::int32_t>(waited)<0)status=waited;
        }
        static const bool trace_enabled=std::getenv("X64ARM_TRACE_NATIVE_FILE_IO")!=nullptr;static std::atomic<unsigned> trace_count{};
        if(native_file_trace_slot(trace_enabled,trace_count))std::fprintf(stderr,
            "[native_completion] pid=%u api=%.*s handle=%llx port=%llx status=%x\n",process_id(),
            static_cast<int>(service.symbol.size()),service.symbol.data(),static_cast<unsigned long long>(arg(0)),
            static_cast<unsigned long long>(port.handle),static_cast<unsigned>(status));
        if(static_cast<std::int32_t>(status)<0)co_return fail(static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status})));
        port.owned=false;last_error_=saved_error;co_return bind?port.handle:1U;
    }catch(const MemoryFault&){co_return fail(998);}catch(const std::bad_alloc&){co_return fail(8);}
        });
}

std::optional<std::uint64_t> WindowsServices::invoke_native_file_position(GuestState& entry_state,GuestMemory& entry_memory,
        const WindowsServiceDescriptor& service) {
    return service_callback::invoke<std::optional<std::uint64_t>>(this,threads_.current,entry_state,entry_memory,"invoke_native_file_position",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint64_t>>{
    const bool extended=service.symbol=="SetFilePointerEx";
    if(!extended && service.symbol!="SetFilePointer")co_return std::nullopt;
    const auto query=native_file_entry(memory,"NtQueryInformationFile"),set=native_file_entry(memory,"NtSetInformationFile");
    if(!query.second && !set.second)co_return std::nullopt;
    const WindowsCall arg(state,memory,service);const auto saved_error=last_error_;
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint64_t>{last_error_=error;return extended?0U:UINT32_MAX;};
    const auto error=[&](std::uint32_t status){return static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));};
    try {
        const auto method=static_cast<std::uint32_t>(arg(3));if(method>2)co_return fail(87);
        if(arg(2))memory.check(GuestAddress{arg(2)},extended?8:4,Access::Write);
        const auto distance=extended?arg(1):arg(2)?(memory.load(GuestAddress{arg(2)},32)<<32)|static_cast<std::uint32_t>(arg(1)):
            static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1)))));
        auto lease=RetainedCapacityLease::acquire(native_file_temporary_budget(),4096);if(!lease)co_return fail(8);
        const auto scratch=allocate_heap(memory,64);if(!scratch)co_return fail(8);
        struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
            ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;const auto info=scratch+32;
        const auto invoke=[&](std::uint64_t entry,std::uint32_t size,std::uint32_t kind)->service_callback::Task<std::uint32_t>{
            memory.store(GuestAddress{scratch},bits,0x103);memory.store(GuestAddress{scratch+unit},bits,0);
            auto status=co_await service_callback::call(entry,{arg(0),scratch,info,size,kind});
            if(status==0x103U){
                const auto waited=co_await service_callback::call(native_file_entry(memory,"NtWaitForSingleObject").first,{arg(0),0,0});
                status=static_cast<std::uint32_t>(memory.load(GuestAddress{scratch},bits));
                if(status==0x103U)throw HostCallFault("Native 定位等待未完成，不能釋放仍在使用的 IOSB");
                if(static_cast<std::int32_t>(waited)<0)status=waited;
            }co_return status;
        };
        __int128 position=std::bit_cast<std::int64_t>(distance);
        if(method){const auto status=co_await invoke(query.first,method==1?8U:24U,method==1?14U:5U);
            if(static_cast<std::int32_t>(status)<0)co_return fail(error(status));position+=memory.load(GuestAddress{info+(method==1?0:8)},64);}
        if(position<0)co_return fail(131);if(position>INT64_MAX)co_return fail(87);
        memory.store(GuestAddress{info},64,static_cast<std::uint64_t>(position));
        const auto status=co_await invoke(set.first,8,14);if(static_cast<std::int32_t>(status)<0)co_return fail(error(status));
        if(arg(2))memory.store(GuestAddress{arg(2)},extended?64:32,extended?static_cast<std::uint64_t>(position):static_cast<std::uint64_t>(position)>>32);
        last_error_=extended?saved_error:0;co_return extended?1U:static_cast<std::uint32_t>(position);
    }catch(const MemoryFault&){co_return fail(998);}catch(const std::bad_alloc&){co_return fail(8);}

        });
}

FileInformation WindowsServices::query_native_file_attributes(GuestState& entry_state,GuestMemory& entry_memory,
        const std::u16string& requested,bool full) {
    return service_callback::invoke<FileInformation>(this,threads_.current,entry_state,entry_memory,"query_native_file_attributes",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,requested,full](GuestState& state,GuestMemory& memory)->service_callback::Task<FileInformation>{
    const auto [target,changed]=native_file_entry(memory,full?"NtQueryFullAttributesFile":"NtQueryAttributesFile");
    if(!changed)co_return filesystem_.information(utf16_to_utf8(requested));
    FileInformation result;
    if(requested.empty()){result.error=3;co_return result;}
    auto path=full_windows_environment_path(requested,runtime_.current_directory,runtime_.environment);
    if(path.starts_with(u"\\\\?\\") || path.starts_with(u"\\\\.\\"))path=u"\\??\\"+path.substr(4);
    else if(path.starts_with(u"\\\\"))path=u"\\??\\UNC\\"+path.substr(2);
    else path=u"\\??\\"+path;
    if(path.size()>32766){result.error=206;co_return result;}
    // 每次最多 68 KiB；一般查詢計入 4 KiB，128 層回呼亦低於 1 MiB。
    // 額度涵蓋跨執行緒與巢狀呼叫，最後一個本次持有者離開即歸帳；不快取屬性。
    const auto size=128+path.size()*2+2;
    auto lease=RetainedCapacityLease::acquire(native_file_temporary_budget(),(size+4095)&~std::size_t{4095});
    if(!lease){result.error=8;co_return result;}
    const auto scratch=allocate_heap(memory,size);if(!scratch){result.error=8;co_return result;}
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
        ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto info=scratch,attributes=scratch+64,name=scratch+112,text=scratch+128;
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,128>{});memory.write(GuestAddress{text},utf16_bytes(path));
    memory.store(GuestAddress{name},16,path.size()*2);memory.store(GuestAddress{name+2},16,path.size()*2+2);memory.store(GuestAddress{name+(x86_?4:8)},bits,text);
    memory.store(GuestAddress{attributes},32,x86_?24:48);memory.store(GuestAddress{attributes+(x86_?8:16)},bits,name);memory.store(GuestAddress{attributes+(x86_?12:24)},32,0x40);
    const auto status=co_await service_callback::call(target,{attributes,info});
    if(static_cast<std::int32_t>(status)<0){result.error=static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));co_return result;}
    auto& m=result.metadata;m.created=memory.load(GuestAddress{info},64);m.accessed=memory.load(GuestAddress{info+8},64);
    m.modified=memory.load(GuestAddress{info+16},64);m.changed=memory.load(GuestAddress{info+24},64);
    if(full){m.allocation_size=memory.load(GuestAddress{info+32},64);m.size=memory.load(GuestAddress{info+40},64);}
    m.attributes=static_cast<std::uint32_t>(memory.load(GuestAddress{info+(full?48:32)},32));co_return result;

        });
}

std::optional<std::uint64_t> WindowsServices::open_native_file(GuestState& entry_state,GuestMemory& entry_memory,
        const std::u16string& requested,const FileOpenArguments& values) {
    return service_callback::invoke<std::optional<std::uint64_t>>(this,threads_.current,entry_state,entry_memory,"open_native_file",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,requested,values](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint64_t>>{
    const auto [target,changed]=native_file_entry(memory,"NtCreateFile");if(!changed)co_return std::nullopt;
    const auto disposition=static_cast<std::uint32_t>(values[4]),flags=static_cast<std::uint32_t>(values[5]);
    // 範本建立沿用既有 NativeFile metadata 複製流程；虛擬範本的 EA 契約另行整合。
    // OPEN_EXISTING／TRUNCATE_EXISTING 不建立檔案，依 Win32 契約忽略範本。
    if(values[6] && disposition!=3 && disposition!=5)co_return std::nullopt;
    const auto invalid=[&](std::uint32_t error)->std::optional<std::uint64_t>{last_error_=error;return x86_?UINT32_MAX:UINT64_MAX;};
    if(disposition<1 || disposition>5 || (values[2]&~UINT64_C(7)))co_return invalid(87);
    if(flags&~0xfe3f01a7U)co_return invalid(50);
    const auto security=read_security_attributes(memory,values[3],x86_,false);
    auto path=full_windows_environment_path(requested,runtime_.current_directory,runtime_.environment);
    if(requested.empty())co_return invalid(123);
    if(path.starts_with(u"\\\\?\\") || path.starts_with(u"\\\\.\\"))path=u"\\??\\"+path.substr(4);
    else if(path.starts_with(u"\\\\"))path=u"\\??\\UNC\\"+path.substr(2);
    else path=u"\\??\\"+path;
    if(path.size()>32766)co_return invalid(206);
    const auto size=128+path.size()*2+2;
    auto lease=RetainedCapacityLease::acquire(native_file_temporary_budget(),(size+4095)&~std::size_t{4095});
    if(!lease)co_return invalid(8);
    const auto scratch=allocate_heap(memory,size);if(!scratch)co_return invalid(8);
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
        ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;
    const auto output=scratch,ios=scratch+16,attributes=scratch+32,name=scratch+80,qos=scratch+96,text=scratch+128;
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,128>{});memory.write(GuestAddress{text},utf16_bytes(path));
    memory.store(GuestAddress{name},16,path.size()*2);memory.store(GuestAddress{name+2},16,path.size()*2+2);memory.store(GuestAddress{name+(x86_?4:8)},bits,text);
    memory.store(GuestAddress{attributes},32,x86_?24:48);memory.store(GuestAddress{attributes+(x86_?8:16)},bits,name);
    memory.store(GuestAddress{attributes+(x86_?12:24)},32,0x40U|(security.inherit?2U:0U));
    if(values[3])memory.store(GuestAddress{attributes+(x86_?16:32)},bits,memory.load(GuestAddress{values[3]+unit},bits));
    if(flags&0x00100000U){
        memory.store(GuestAddress{qos},32,12);memory.store(GuestAddress{qos+4},32,(flags>>16)&3U);
        memory.store(GuestAddress{qos+8},8,(flags&0x40000U)!=0);memory.store(GuestAddress{qos+9},8,(flags&0x80000U)!=0);
        memory.store(GuestAddress{attributes+(x86_?20:40)},bits,qos);
    }
    auto access=static_cast<std::uint32_t>(values[1])|0x100080U; // 同步等待及 Win32 中繼資料查詢權限。
    std::uint32_t options=(flags&0x02000000U)?0x4000U:0x40U;
    if(!(flags&0x40000000U))options|=0x20U;
    if(flags&0x80000000U)options|=2U;if(flags&0x08000000U)options|=4U;if(flags&0x20000000U)options|=8U;
    if(flags&0x10000000U)options|=0x800U;if(flags&0x00200000U)options|=0x200000U;if(flags&0x00100000U)options|=0x400000U;
    if(flags&0x04000000U){options|=0x1000U;access|=0x10000U;}
    constexpr std::array<std::uint32_t,5> native_disposition{2,5,1,3,4};
    const auto saved_error=last_error_;
    auto status=co_await service_callback::call(target,{output,access,attributes,ios,0,flags&0xffffU,values[2],native_disposition[disposition-1],options,0,0});
    if(status==0x103U){
        // 等待期間 scratch 與額度仍存活；不可先返回並釋放待完成的 IOSB。
        const auto handle=memory.load(GuestAddress{output},bits);
        const auto waiter=native_file_entry(memory,"NtWaitForSingleObject").first;
        const auto waited=co_await service_callback::call(waiter,{handle,0,0});
        status=static_cast<std::int32_t>(waited)<0?waited:static_cast<std::uint32_t>(memory.load(GuestAddress{ios},bits));
        if(static_cast<std::int32_t>(status)<0 || status==0x103U){
            (void)co_await service_callback::call(native_file_entry(memory,"NtClose").first,{handle});
            if(status==0x103U)co_return invalid(996);
        }
    }
    if(static_cast<std::int32_t>(status)<0)co_return invalid(disposition==1 && status==0xc0000035U?80U:static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status})));
    const auto handle=memory.load(GuestAddress{output},bits),information=memory.load(GuestAddress{ios+unit},bits);
    if(!handle || handle==(x86_?UINT32_MAX:UINT64_MAX))co_return invalid(6);
    last_error_=(disposition==2 || disposition==4)?(information==1 || information==3?183U:0U):saved_error;
    co_return handle;

        });
}

std::optional<std::uint32_t> WindowsServices::invoke_file_close(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service) {
    return service_callback::invoke<std::optional<std::uint32_t>>(this,threads_.current,entry_state,entry_memory,"invoke_file_close",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
    const WindowsCall frame(state,memory,service);
    if(service.family!="internal-nt-file") {
        const auto [target,changed]=native_file_entry(memory,"NtClose");
        if(changed) {
            const auto saved_error=last_error_;
            const auto status=co_await service_callback::call(target,{frame(0)});
            const bool success=static_cast<std::int32_t>(status)>=0;
            last_error_=success?saved_error:static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));
            frame.return_to(state,success);co_return std::nullopt;
        }
    }
    std::uint64_t returned=0;const auto fail=[&](std::uint32_t error){last_error_=error;returned=0;};
        const auto handle = frame(0);
        if (const auto flags=runtime_.handle_flags.find(handle);flags!=runtime_.handle_flags.end() && (flags->second&2))fail(6);
        else if (handle >= 0x100 && handle <= 0x102 && standard_open_[handle - 0x100]) {
            auto& duplicates=standard_duplicates_[handle - 0x100];
            if(duplicates)--duplicates;else standard_open_[handle - 0x100] = false;
            returned = 1;
        }
        else if (const auto file=files_.find(handle);file!=files_.end()) {
            close_transfer_handle(memory,handle);
            returned=1;
        }
        else if (kernel_device_handle(handle)) co_return close_kernel_device(state,memory,handle);
        else if (power_requests_.erase(handle)) returned = 1;
        else if (close_transaction(handle)) returned = 1;
        else if (close_bluetooth(handle)) returned = 1;
        else if (close_mapping(handle)) returned = 1;
        else if (close_job(handle)) returned = 1;
        else if (close_process(handle)) returned = 1;
        else if (close_wait_packet(memory,handle) || close_completion_port(memory,handle)) returned = 1;
        else if (runtime_.process_snapshots.erase(handle)) returned = 1;
        else if (close_wait_object(handle)) returned=1;
        else if (runtime_.process_thread_handles.erase(handle)) {runtime_.thread_handle_access.erase(handle);returned = 1;}
        else if (runtime_.token_handles.erase(handle)) {runtime_.token_handle_objects.erase(handle);collect_tokens();returned = 1;}
        else fail(6);
        if(returned)runtime_.handle_flags.erase(handle);
    frame.return_to(state,returned);co_return std::nullopt;

        });
}

std::optional<std::uint64_t> WindowsServices::invoke_native_file_information(GuestState& entry_state,GuestMemory& entry_memory,
        const WindowsServiceDescriptor& service) {
    return service_callback::invoke<std::optional<std::uint64_t>>(this,threads_.current,entry_state,entry_memory,"invoke_native_file_information",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint64_t>>{
    const auto symbol=service.symbol;
    const bool size=symbol=="GetFileSize",size_ex=symbol=="GetFileSizeEx",time=symbol=="GetFileTime";
    const bool type=symbol=="GetFileType",full=symbol=="GetFileInformationByHandle",extended=symbol=="GetFileInformationByHandleEx";
    if(!size && !size_ex && !time && !type && !full && !extended)co_return std::nullopt;
    const WindowsCall arg(state,memory,service);const auto handle=arg(0);
    if(type){
        // 這些既有宿主物件不是 files_ 的 Native 檔案；保留其既有類型與錯誤契約。
        const auto known=files_.find(handle);
        if((handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100]) ||
           runtime_.network.sockets.contains(handle) || console_handle(handle) || bluetooth_handle(handle) ||
           kernel_device_handle(handle) || (known!=files_.end() && (known->second.serial || known->second.hid || known->second.usb)))co_return std::nullopt;
    }
    const auto kind=extended?static_cast<std::uint32_t>(arg(1)):0U;
    const bool directory=extended && (kind==10 || kind==11 || kind==14 || kind==15);
    const auto [target,changed]=native_file_entry(memory,type?"NtQueryVolumeInformationFile":directory?"NtQueryDirectoryFile":"NtQueryInformationFile");
    if(!changed)co_return std::nullopt;
    const auto saved_error=last_error_;
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint64_t>{last_error_=error;return size?UINT32_MAX:0U;};
    const auto status_error=[&](std::uint32_t status){return static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));};
    std::uint32_t native_class=type?4U:time?4U:full?18U:5U,minimum=0;
    if(extended)switch(kind){
        case 0:native_class=4;minimum=40;break;case 1:native_class=5;minimum=24;break;
        case 2:native_class=9;minimum=4;break;case 9:native_class=35;minimum=8;break;
        case 10:case 11:native_class=37;minimum=104;break;case 14:case 15:native_class=2;minimum=68;break;
        case 18:native_class=59;minimum=24;break;default:co_return fail(87);
    }
    try{
        if(extended){
            if(static_cast<std::uint32_t>(arg(3))<minimum)co_return fail(24);
            if(!arg(2))co_return fail(87);
            memory.check(GuestAddress{arg(2)},static_cast<std::uint32_t>(arg(3)),Access::Write);
        }else if(full || size_ex){
            if(!arg(1))co_return fail(87);memory.check(GuestAddress{arg(1)},full?52:8,Access::Write);
        }else if(size && arg(1))memory.check(GuestAddress{arg(1)},4,Access::Write);
        else if(time)for(unsigned i=1;i<4;++i)if(arg(i))memory.check(GuestAddress{arg(i)},8,Access::Write);
        // 固定暫存只保留本次 IOSB 與 metadata；變長輸出直接使用客體緩衝。
        auto lease=RetainedCapacityLease::acquire(native_file_temporary_budget(),4096);
        if(!lease)co_return fail(8);
        const auto scratch=allocate_heap(memory,256);if(!scratch)co_return fail(8);
        struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
            ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,256>{});
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;
        const auto ios=scratch,info=scratch+32,volume=scratch+144;
        const auto invoke=[&](std::uint64_t entry,std::vector<std::uint64_t> args)->service_callback::Task<std::uint32_t>{
            memory.store(GuestAddress{ios},bits,0x103);memory.store(GuestAddress{ios+unit},bits,0);
            auto status=co_await service_callback::call(entry,std::move(args));
            if(status==0x103U){
                // 等待與回呼結束前不歸還 IOSB 或 scratch 額度。
                const auto waited=co_await service_callback::call(native_file_entry(memory,"NtWaitForSingleObject").first,{handle,0,0});
                status=static_cast<std::int32_t>(waited)<0?waited:static_cast<std::uint32_t>(memory.load(GuestAddress{ios},bits));
                if(status==0x103U)status=0xc00000e5U;
            }
            co_return status;
        };
        std::uint32_t status;
        if(directory)status=co_await invoke(target,{handle,0,0,0,ios,arg(2),static_cast<std::uint32_t>(arg(3)),native_class,0,0,kind==11 || kind==15});
        else status=co_await invoke(target,{handle,ios,extended?arg(2):info,extended?static_cast<std::uint32_t>(arg(3)):type?8U:time?40U:full?104U:24U,native_class});
        if(static_cast<std::int32_t>(status)<0 && !(full && status==0x80000005U))co_return fail(status_error(status));
        if(extended){last_error_=saved_error;co_return 1;}
        if(type){
            const auto device=static_cast<std::uint32_t>(memory.load(GuestAddress{info},32));
            const bool character=device==0x15 || device==0x16 || device==0x1b || device==0x1f || device==0x22 || device==0x50;
            last_error_=saved_error;co_return device==0x11?3U:character?2U:1U;
        }
        if(size || size_ex){
            const auto length=memory.load(GuestAddress{info+8},64);
            if(size_ex)memory.store(GuestAddress{arg(1)},64,length);
            else if(arg(1))memory.store(GuestAddress{arg(1)},32,length>>32);
            last_error_=size?0:saved_error;co_return size?static_cast<std::uint32_t>(length):1U;
        }
        if(time){
            for(unsigned i=1;i<4;++i)if(arg(i))memory.store(GuestAddress{arg(i)},64,memory.load(GuestAddress{info+(i-1)*8},64));
        }else{
            // FILE_ALL_INFORMATION 的固定區與卷序號組成舊版 BY_HANDLE_FILE_INFORMATION。
            status=co_await invoke(native_file_entry(memory,"NtQueryVolumeInformationFile").first,{handle,ios,volume,64,1});
            if(static_cast<std::int32_t>(status)<0 && status!=0x80000005U)co_return fail(status_error(status));
            const auto output=arg(1),length=memory.load(GuestAddress{info+48},64),id=memory.load(GuestAddress{info+64},64);
            memory.store(GuestAddress{output},32,memory.load(GuestAddress{info+32},32));
            for(unsigned i=0;i<3;++i)memory.store(GuestAddress{output+4+i*8},64,memory.load(GuestAddress{info+i*8},64));
            memory.store(GuestAddress{output+28},32,memory.load(GuestAddress{volume+8},32));
            memory.store(GuestAddress{output+32},32,length>>32);memory.store(GuestAddress{output+36},32,length);
            memory.store(GuestAddress{output+40},32,memory.load(GuestAddress{info+56},32));
            memory.store(GuestAddress{output+44},32,id>>32);memory.store(GuestAddress{output+48},32,id);
        }
        last_error_=saved_error;co_return 1;
    }catch(const MemoryFault&){co_return fail(998);}catch(const std::bad_alloc&){co_return fail(8);}

        });
}

std::optional<std::uint64_t> WindowsServices::invoke_native_mapping(GuestState& entry_state,GuestMemory& entry_memory,
        const WindowsServiceDescriptor& service) {
    return service_callback::invoke<std::optional<std::uint64_t>>(this,threads_.current,entry_state,entry_memory,"invoke_native_mapping",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint64_t>>{
    const auto symbol=service.symbol;
    // 按需、限量觀測 Native 邊界；不保存 payload 或 handle 對照。
    static const bool trace_enabled=std::getenv("X64ARM_TRACE_NATIVE_MAPPING")!=nullptr;
    static std::atomic<unsigned> trace_count{};
    const bool trace=native_file_trace_slot(trace_enabled,trace_count);
    const bool create=symbol=="CreateFileMappingA" || symbol=="CreateFileMappingW";
    const bool open=symbol=="OpenFileMappingA" || symbol=="OpenFileMappingW";
    const bool map=symbol=="MapViewOfFile" || symbol=="MapViewOfFileEx";
    const bool unmap=symbol=="UnmapViewOfFile",flush=symbol=="FlushViewOfFile";
    if(!create && !open && !map && !unmap && !flush)co_return std::nullopt;
    const auto [target,changed]=native_file_entry(memory,create?"NtCreateSection":open?"NtOpenSection":
        map?"NtMapViewOfSection":unmap?"NtUnmapViewOfSection":"NtFlushVirtualMemory");
    if(!changed)co_return std::nullopt;
    const WindowsCall arg(state,memory,service);const auto saved_error=last_error_;
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint64_t>{last_error_=error;return 0;};
    const auto status_error=[&](std::uint32_t status){return static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));};
    const auto current=x86_?UINT32_MAX:UINT64_MAX;
    // 解除映射不借用額度；即使 scratch 已滿仍可釋放客體持有的 view。
    if(unmap){
        const auto status=co_await service_callback::call(target,{current,arg(0)});
        if(static_cast<std::int32_t>(status)<0)co_return fail(status_error(status));
        last_error_=saved_error;co_return 1;
    }
    std::u16string name;
    std::uint32_t protect{},allocation{},access{};
    std::uint64_t file{},length{};
    CreationSecurity security{};
    if(create || open){
        const auto name_at=arg(create?5:2);
        if(name_at)name=symbol.ends_with('W')?read_utf16(memory,GuestAddress{name_at},32768):utf8_to_utf16(read_ansi(memory,name_at));
        if(open && (!name_at || name.empty()))co_return fail(87);
        if(name.starts_with(u"Local\\"))name.erase(0,6);
        if(!name.empty())name=u"\\BaseNamedObjects\\"+name;
        if(name.size()>32766)co_return fail(206);
        if(create){
            constexpr std::uint32_t section_flags=0xdd800000U;
            const auto flags=static_cast<std::uint32_t>(arg(2));protect=flags&~section_flags;allocation=flags&section_flags;
            if(!allocation)allocation=0x8000000U;
            if(protect!=2 && protect!=4 && protect!=8 && protect!=0x20 && protect!=0x40 && protect!=0x80)co_return fail(87);
            access=0xf0005U|((protect==4 || protect==0x40)?2U:0U)|(protect>=0x20?8U:0U);
            file=arg(0);length=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(3)))<<32)|static_cast<std::uint32_t>(arg(4));
            if(!file)co_return fail(6);
            if(file==current){file=0;if(!length)co_return fail(87);}
            if(length>INT64_MAX)co_return fail(87);
            security=read_security_attributes(memory,arg(1),x86_,false,false);
        }else{access=static_cast<std::uint32_t>(arg(0));if(access==1)access=4;security.inherit=arg(1)!=0;}
    }
    const auto size=128+name.size()*2+2;
    auto lease=RetainedCapacityLease::acquire(native_file_temporary_budget(),(size+4095)&~std::size_t{4095});
    if(!lease)co_return fail(8);
    const auto scratch=allocate_heap(memory,size);if(!scratch)co_return fail(8);
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t address;
        ~Release(){(void)self.free_heap(memory,address,state);}} release{*this,memory,state,scratch};
    const auto bits=static_cast<std::uint8_t>(x86_?32:64),unit=static_cast<std::uint8_t>(x86_?4:8);
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,128>{});
    if(create || open){
        const auto attributes=scratch+32,counted=scratch+80,text=scratch+128;
        memory.store(GuestAddress{attributes},32,x86_?24:48);
        memory.store(GuestAddress{attributes+unit*3},32,0x40U|(create?0x80U:0U)|(security.inherit?2U:0U));
        if(create && arg(1))memory.store(GuestAddress{attributes+unit*4},bits,memory.load(GuestAddress{arg(1)+unit},bits));
        if(!name.empty()){
            memory.write(GuestAddress{text},utf16_bytes(name));memory.store(GuestAddress{counted},16,name.size()*2);
            memory.store(GuestAddress{counted+2},16,name.size()*2+2);memory.store(GuestAddress{counted+unit},bits,text);
            memory.store(GuestAddress{attributes+unit*2},bits,counted);
        }
        memory.store(GuestAddress{scratch+16},64,length);
        const auto status=co_await service_callback::call(target,create?
            std::vector<std::uint64_t>{scratch,access,attributes,scratch+16,protect,allocation,file}:
            std::vector<std::uint64_t>{scratch,access,attributes});
        if(static_cast<std::int32_t>(status)<0)co_return fail(status_error(status));
        const auto handle=memory.load(GuestAddress{scratch},bits);
        if(trace)std::fprintf(stderr,"[native_mapping] %.*s target=%llx file=%llx size=%llu output=%llx handle=%llx status=%x\n",
            static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(target),static_cast<unsigned long long>(file),
            static_cast<unsigned long long>(length),static_cast<unsigned long long>(scratch),static_cast<unsigned long long>(handle),status);
        if(!handle || handle==current)co_return fail(6);
        last_error_=create?(status==0x40000000U?183U:0U):saved_error;co_return handle;
    }
    if(map){
        auto flags=static_cast<std::uint32_t>(arg(1));const bool execute=(flags&0x20U)!=0;flags&=~0x20U;
        protect=flags==1?8U:(flags&2U)?4U:(flags&4U)?2U:1U;if(execute && protect!=1)protect<<=4;
        const auto offset=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(2)))<<32)|static_cast<std::uint32_t>(arg(3));
        memory.store(GuestAddress{scratch},bits,symbol=="MapViewOfFileEx"?arg(5):0);
        memory.store(GuestAddress{scratch+16},64,offset);memory.store(GuestAddress{scratch+32},bits,arg(4));
        const auto status=co_await service_callback::call(target,{arg(0),current,scratch,0,0,scratch+16,scratch+32,1,0,protect});
        if(static_cast<std::int32_t>(status)<0)co_return fail(status_error(status));
        const auto base=memory.load(GuestAddress{scratch},bits);
        if(trace){
            std::uint64_t header{};try{header=memory.load(GuestAddress{base},32);}catch(const MemoryFault&){}
            std::fprintf(stderr,"[native_mapping] %.*s target=%llx section=%llx offset=%llu requested=%llu mapped=%llu base=%llx header=%llx status=%x\n",
                static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(target),static_cast<unsigned long long>(arg(0)),
                static_cast<unsigned long long>(offset),static_cast<unsigned long long>(arg(4)),
                static_cast<unsigned long long>(memory.load(GuestAddress{scratch+32},bits)),static_cast<unsigned long long>(base),static_cast<unsigned long long>(header),status);
        }
        last_error_=saved_error;co_return base;
    }
    memory.store(GuestAddress{scratch},bits,arg(0));memory.store(GuestAddress{scratch+16},bits,arg(1));
    const auto status=co_await service_callback::call(target,{current,scratch,scratch+16,0});
    if(static_cast<std::int32_t>(status)<0 && status!=0xc0000088U)co_return fail(status_error(status));
    last_error_=saved_error;co_return 1;

        });
}
