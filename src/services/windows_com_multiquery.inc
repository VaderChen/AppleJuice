// 包含於 invoke_com_class；多介面啟用仍透過既有客體 COM 與可重入 callback 流程。
if(service.symbol=="CoCreateInstanceEx")return service_callback::invoke<std::optional<std::uint32_t>>(
    this,threads_.current,entry_state,entry_memory,"CoCreateInstanceEx",
    [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
        start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
    },
    [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(state,memory,service);const auto saved_error=last_error_;
        const auto done=[&](std::uint32_t status){last_error_=saved_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
        const auto unit=x86_?4U:8U,stride=x86_?12U:24U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto count=static_cast<std::uint32_t>(call(4)),context=static_cast<std::uint32_t>(call(2));const auto output=call(5),outer=call(1);
        constexpr std::array<std::uint8_t,16> unknown_iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
        struct QueryResult {std::array<std::uint8_t,16> iid{};std::uint64_t object{};std::uint32_t status{no_interface};};
        std::vector<QueryResult> results;std::array<std::uint8_t,16> clsid{};std::uint32_t validation=0;
        try{
            if(!count || !output || !call(0))co_return done(invalid);
            if(count>4096)co_return done(no_memory);
            memory.check(GuestAddress{output},static_cast<std::size_t>(count)*stride,Access::Write);
            memory.check(GuestAddress{output},static_cast<std::size_t>(count)*stride,Access::Read);
            memory.read(GuestAddress{call(0)},clsid);results.resize(count);
            for(unsigned i=0;i<count;++i){
                const auto row=output+static_cast<std::uint64_t>(i)*stride,iid=memory.load(GuestAddress{row},bits);
                if(!iid || memory.load(GuestAddress{row+unit},bits) || memory.load(GuestAddress{row+2*unit},32))co_return done(invalid);
                memory.read(GuestAddress{iid},results[i].iid);
            }
            if(call(3)){
                const auto server=call(3);memory.check(GuestAddress{server},x86_?16U:32U,Access::Read);
                if(memory.load(GuestAddress{server},32) || memory.load(GuestAddress{server+3*unit},32))validation=invalid;
                const auto name=memory.load(GuestAddress{server+unit},bits),auth=memory.load(GuestAddress{server+2*unit},bits);
                if(auth)validation=not_implemented; // 尚無遠端 DCOM authentication provider，不忽略安全設定。
                if(name){
                    auto host=read_utf16(memory,GuestAddress{name},32768);for(auto& c:host)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32U);
                    if(!host.empty() && host!=u"." && host!=u"localhost" && host!=u"x64arm")validation=not_implemented;
                }
            }
            if(!validation && !com_apartment(threads_.current))validation=not_initialized;
            if(!validation && outer && (count!=1 || results[0].iid!=unknown_iid))validation=0x80040110U;
            for(unsigned i=0;i<count;++i){const auto row=output+static_cast<std::uint64_t>(i)*stride;
                memory.store(GuestAddress{row+unit},bits,0);memory.store(GuestAddress{row+2*unit},32,validation?validation:no_interface);}
        }catch(const MemoryFault&){co_return done(pointer_error);}catch(const std::bad_alloc&){co_return done(no_memory);}
        if(validation)co_return done(validation);
        const auto scratch=allocate_heap(memory,64+static_cast<std::size_t>(count)*16);if(!scratch)co_return done(no_memory);
        struct ReleaseScratch {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t address;
            ~ReleaseScratch(){(void)self.free_heap(memory,address,state);}} release_scratch{*this,state,memory,scratch};
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,64>{});memory.write(GuestAddress{scratch},clsid);memory.write(GuestAddress{scratch+16},unknown_iid);
        for(unsigned i=0;i<count;++i)memory.write(GuestAddress{scratch+64+static_cast<std::uint64_t>(i)*16},results[i].iid);
        const auto created=co_await service_callback::call(resolve("ole32.dll","CoCreateInstance").value,{scratch,outer,context,scratch+16,scratch+32});
        const auto object=memory.load(GuestAddress{scratch+32},bits);std::uint32_t overall=created;unsigned succeeded=0;
        if(!(created&0x80000000U) && object){
            if(outer){results[0].object=object;results[0].status=0;succeeded=1;} // 交付聚合物件的 nondelegating IUnknown。
            else {
                for(unsigned i=0;i<count;++i){
                    memory.store(GuestAddress{scratch+40},bits,0);
                    auto& result=results[i];result.status=co_await service_callback::call(method(memory,object,0),{object,scratch+64+static_cast<std::uint64_t>(i)*16,scratch+40});
                    if(!(result.status&0x80000000U)){result.object=memory.load(GuestAddress{scratch+40},bits);if(result.object)++succeeded;else result.status=no_interface;}
                }
                (void)co_await service_callback::call(method(memory,object,2),{object});
            }
            overall=succeeded==count?0U:succeeded?0x00080012U:no_interface;
        }else {
            if(!(overall&0x80000000U))overall=no_interface;
            for(auto& result:results)result.status=overall;
        }
        // 客體 QueryInterface 可重入並改變輸出範圍；發布前重新驗證。無法交付的
        // 已取得介面逐一 Release，不遺漏成功的前半部，也不靠重新建立物件取得其他 IID。
        bool publish_failed=false;
        try{
            memory.check(GuestAddress{output},static_cast<std::size_t>(count)*stride,Access::Write);
            for(unsigned i=0;i<count;++i){const auto row=output+static_cast<std::uint64_t>(i)*stride;
                memory.store(GuestAddress{row+unit},bits,results[i].object);memory.store(GuestAddress{row+2*unit},32,results[i].status);}
        }catch(const MemoryFault&){publish_failed=true;}
        if(publish_failed){for(const auto& result:results)if(result.object)(void)co_await service_callback::call(method(memory,result.object,2),{result.object});co_return done(pointer_error);}
        co_return done(overall);
    });
