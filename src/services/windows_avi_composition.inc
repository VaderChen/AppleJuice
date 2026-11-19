    if(name=="AVIMakeFileFromStreams" || service.family==families[static_cast<unsigned>(Kind::composition)]){
        const auto vtable=table(Kind::composition);
        return nested_continuation(state,memory,"AVI 組合檔案",[this,service,vtable](GuestState& s,GuestMemory& m)
            ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall args(s,m,service);
            const unsigned width=x86_?4U:8U;
            const auto ptr_bits=static_cast<std::uint8_t>(width*8);
            auto pointer=[&](std::uint64_t address){return m.load(GuestAddress{address},ptr_bits);};
            auto write_pointer=[&](std::uint64_t address,std::uint64_t value){m.store(GuestAddress{address},ptr_bits,value);};
            auto slot=[&](std::uint64_t stream,unsigned index){return pointer(pointer(stream)+index*width);};
            auto finish=[&](std::uint64_t result){args.return_to(s,result);return std::optional<std::uint32_t>{};};
            auto& objects=avi_disk_->compositions;
            const auto owner=args(0);
            const auto op=service.symbol;
            if(op=="AVIMakeFileFromStreams"){
                const auto count=static_cast<std::int32_t>(args(1));
                if(!owner || count<0 || !args(2))co_return finish(bad_parameter);
                write_pointer(owner,0);
                if(static_cast<unsigned>(count)>memory_limit/sizeof(std::uint64_t))co_return finish(no_memory);
                m.check(GuestAddress{args(2)},static_cast<unsigned>(count)*width,Access::Read);
                auto file=std::make_shared<AviDiskState::Composition>();
                auto& header=file->info;
                put(header,12,static_cast<unsigned>(count));put(header,8,1);
                for(int i=0;i<count;++i){
                    auto stream=pointer(args(2)+static_cast<unsigned>(i)*width);
                    if(!stream)co_return finish(bad_handle);
                    file->streams.push_back(stream);
                }
                GuestScratch scratch(*this,m,s);
                const auto info=scratch.allocate(204);
                if(!info)co_return finish(no_memory);
                std::uint32_t status=0;
                std::size_t retained=0;
                for(auto stream:file->streams){
                    (void)co_await service_callback::call(slot(stream,1),{stream});
                    ++retained;
                    m.fill(GuestAddress{info},0,204);
                    status=static_cast<unsigned>(co_await service_callback::call(slot(stream,4),{stream,info,204}));
                    if(static_cast<std::int32_t>(status)<0)break;
                    status=0;
                    auto field=[&](unsigned offset){return static_cast<unsigned>(m.load(GuestAddress{info+offset},32));};
                    const auto scale=field(20),rate=field(24);
                    if(retained==1){put(header,28,scale && rate?scale:1);put(header,32,scale && rate?rate:100);}
                    if(scale && rate){
                        const auto denominator=std::uint64_t(rate)*word(header,28);
                        const auto numerator=static_cast<unsigned __int128>(field(32))*scale*word(header,32);
                        const auto length=(numerator+denominator/2)/denominator;
                        put(header,36,std::max(word(header,36),static_cast<unsigned>(std::min<unsigned __int128>(length,UINT32_MAX))));
                    }
                    put(header,16,std::max(word(header,16),field(40)));
                    for(unsigned axis=0;axis<2;++axis){
                        const auto low=static_cast<std::int32_t>(field(52+axis*4)),high=static_cast<std::int32_t>(field(60+axis*4));
                        if(high>low)put(header,20+axis*4,std::max(word(header,20+axis*4),static_cast<unsigned>(std::int64_t(high)-low)));
                    }
                }
                if(!status){
                    const auto object=allocate_heap(m,width);
                    if(object){
                        write_pointer(object,vtable);objects.emplace(object,file);write_pointer(owner,object);co_return finish(0);
                    }
                    status=no_memory;
                }
                for(auto stream:std::span(file->streams).first(retained))
                    (void)co_await service_callback::call(slot(stream,2),{stream});
                co_return finish(status);
            }
            const auto found=objects.find(owner);
            if(found==objects.end())co_return finish(bad_handle);
            const auto file=found->second;
            if(op=="AddRef")co_return finish(++file->refs);
            if(op=="Release"){
                if(--file->refs)co_return finish(file->refs);
                objects.erase(owner);
                for(auto stream:file->streams)(void)co_await service_callback::call(slot(stream,2),{stream});
                free_heap(m,owner,s);co_return finish(0);
            }
            if(op=="QueryInterface"){
                if(!args(2))co_return finish(pointer_error);
                write_pointer(args(2),0);
                if(!args(1))co_return finish(pointer_error);
                WindowsGuid id{};m.read(GuestAddress{args(1)},id);
                if(id!=ole_standard_guid(0) && id!=ole_standard_guid(0x20020))co_return finish(no_interface);
                ++file->refs;write_pointer(args(2),owner);co_return finish(0);
            }
            if(op=="Info"){
                const auto size=static_cast<std::int32_t>(args(2));
                if(!args(1))co_return finish(bad_parameter);
                if(size<0)co_return finish(bad_size);
                auto bytes=avi::View(file->info).first(std::min<std::size_t>(static_cast<unsigned>(size),file->info.size()));
                if(!bytes.empty())m.write(GuestAddress{args(1)},bytes);
                co_return finish(bytes.size()==file->info.size()?0:too_small);
            }
            if(op=="GetStream"){
                auto skip=static_cast<std::int32_t>(args(3));
                if(!args(1) || skip<0)co_return finish(bad_parameter);
                write_pointer(args(1),0);
                GuestScratch scratch(*this,m,s);
                const auto type=static_cast<unsigned>(args(2));
                const auto info=type?scratch.allocate(204):0;
                if(type && !info)co_return finish(no_memory);
                for(auto stream:file->streams){
                    if(type){
                        const auto hr=co_await service_callback::call(slot(stream,4),{stream,info,204});
                        if(static_cast<std::int32_t>(hr)<0)co_return finish(hr);
                        if(m.load(GuestAddress{info},32)!=type)continue;
                    }
                    if(skip--==0){
                        (void)co_await service_callback::call(slot(stream,1),{stream});
                        write_pointer(args(1),stream);co_return finish(0);
                    }
                }
                co_return finish(no_data);
            }
            // 此物件只擁有串流參照，沒有可供修改的 RIFF 檔案。
            co_return finish(op=="EndRecord"?0:unsupported);
        });
    }
