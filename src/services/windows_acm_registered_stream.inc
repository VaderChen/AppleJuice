                        if(symbol=="acmStreamOpen"){
                            if(!instance || stream)co_return finish(5);
                            const auto flags=word(7);
                            if(flags&~7U)co_return finish(10);
                            if((flags&2U) || call(5) || call(6))co_return finish(512);
                            const bool query=(flags&1U)!=0;
                            const auto result=call(0);
                            if(!query && !result)co_return finish(11);
                            if(result)m.store(GuestAddress{result},bits,0);
                            const auto source=call(2),destination=call(3),filter=call(4);
                            if(!source || !destination)co_return finish(11);
                            const auto copy_format=[&](std::uint64_t format){
                                const bool pcm=m.load(GuestAddress{format},16)==1;
                                auto bytes=std::vector<std::uint8_t>(pcm?18:18+m.load(GuestAddress{format+16},16));
                                m.read(GuestAddress{format},std::span(bytes).first(pcm?16:bytes.size()));
                                return bytes;
                            };
                            const auto input=copy_format(source),output=copy_format(destination);
                            std::vector<std::uint8_t> filter_bytes;
                            if(filter){
                                const auto size=load32(filter,0);
                                if(size<32 || size>65536)co_return finish(11);
                                filter_bytes.resize(size);
                                m.read(GuestAddress{filter},filter_bytes);
                            }
                            if(catalog.registered_streams.size()+catalog.streams.size()>=256)co_return finish(4);
                            const acm::StreamInstanceAbi layout{unit};
                            const auto bytes=layout.size()+input.size()+output.size()+filter_bytes.size();
                            const auto allocation=allocate_heap(m,bytes);
                            if(!allocation)co_return finish(7);
                            const auto handle=runtime_.allocate_wait_handle();
                            struct PendingStream {
                                WindowsServices& service;AcmState& state;GuestMemory& memory;GuestState& guest;
                                std::uint64_t handle,allocation;bool published{};
                                ~PendingStream(){if(!published){state.registered_streams.erase(handle);(void)service.free_heap(memory,allocation,guest);}}
                            } pending{*this,catalog,m,s,handle,allocation};
                            auto created=std::make_shared<acm::DriverStream>();
                            created->instance=instance;
                            created->driver_handle=id_arg;
                            created->allocation=allocation;
                            created->busy=true;
                            catalog.registered_streams.emplace(handle,created);
                            m.fill(GuestAddress{allocation},0,layout.size());
                            const auto input_at=allocation+layout.size(),output_at=input_at+input.size();
                            m.write(GuestAddress{input_at},input);
                            m.write(GuestAddress{output_at},output);
                            m.store(GuestAddress{allocation},32,layout.size());
                            m.store(GuestAddress{allocation+layout.source()},bits,input_at);
                            m.store(GuestAddress{allocation+layout.destination()},bits,output_at);
                            if(filter){
                                const auto filter_at=output_at+output.size();
                                m.write(GuestAddress{filter_at},filter_bytes);
                                m.store(GuestAddress{allocation+layout.filter()},bits,filter_at);
                            }
                            m.store(GuestAddress{allocation+layout.flags()},32,flags);
                            const auto opened=co_await send(*driver,instance->endpoint,acm::stream_open,allocation,0);
                            if(opened)co_return finish(opened);
                            bool writable=true;
                            try{if(result)m.check(GuestAddress{result},unit,Access::Write);}
                            catch(const MemoryFault&){writable=false;}
                            if(query || !writable){
                                (void)co_await send(*driver,instance->endpoint,acm::stream_close,allocation,0);
                                co_return finish(writable?0:11);
                            }
                            m.store(GuestAddress{allocation+layout.handle()},bits,handle);
                            m.store(GuestAddress{result},bits,handle);
                            created->busy=false;
                            pending.published=true;
                            co_return finish(0);
                        }
                        if(stream && symbol.starts_with("acmStream")){
                            stream->busy=true;
                            struct ResetStreamBusy {bool& busy;~ResetStreamBusy(){busy=false;}} reset{stream->busy};
                            const auto stream_message=[&](unsigned message,std::uint64_t parameter)
                                ->service_callback::Task<std::uint64_t>{
                                co_return co_await send(*driver,instance->endpoint,message,stream->allocation,parameter);
                            };
                            if(symbol=="acmStreamClose"){
                                if(word(1))co_return finish(10);
                                if(!stream->headers.empty())co_return finish(513);
                                const auto result=co_await stream_message(acm::stream_close,0);
                                if(result)co_return finish(result);
                                catalog.registered_streams.erase(id_arg);
                                (void)free_heap(m,stream->allocation,s);
                                if(stream->owns_driver){
                                    instance->busy=true;
                                    (void)co_await close(*driver,instance->endpoint);
                                    catalog.registered_instances.erase(stream->driver_handle);
                                    --driver->open_count;
                                }
                                co_return finish(0);
                            }
                            if(symbol=="acmStreamReset"){
                                if(word(1))co_return finish(10);
                                co_return finish(co_await stream_message(acm::stream_reset,0));
                            }
                            if(symbol=="acmStreamMessage"){
                                co_return finish(co_await send(*driver,instance->endpoint,word(1),call(2),call(3)));
                            }
                            if(symbol=="acmStreamSize"){
                                if(!call(2) || !word(1))co_return finish(11);
                                m.store(GuestAddress{call(2)},32,0);
                                if(word(3)>1)co_return finish(10);
                                const auto message=buffer(16);
                                m.store(GuestAddress{message},32,16);
                                m.store(GuestAddress{message+4},32,word(3));
                                m.store(GuestAddress{message+(word(3)?12U:8U)},32,word(1));
                                const auto result=co_await stream_message(acm::stream_size,message);
                                if(!result)m.store(GuestAddress{call(2)},32,load32(message,word(3)?8U:12U));
                                co_return finish(result);
                            }
                            const acm::StreamHeaderAbi abi{unit};
                            const auto header=call(1);
                            if(!header || load32(header,0)<abi.size())co_return finish(11);
                            const auto flags=word(2);
                            const bool convert=symbol=="acmStreamConvert";
                            if(flags&~(convert?0x34U:0U))co_return finish(10);
                            m.check(GuestAddress{header},abi.size(),Access::Write);
                            const auto source=m.load(GuestAddress{header+abi.source()},bits);
                            const auto destination=m.load(GuestAddress{header+abi.destination()},bits);
                            const auto source_size=load32(header,abi.source_length());
                            const auto destination_size=load32(header,abi.destination_length());
                            constexpr unsigned prepared=acm::status_prepared,done=acm::status_done;
                            auto status=load32(header,4);
                            m.store(GuestAddress{header+abi.convert_flags()},32,flags);
                            if(symbol=="acmStreamPrepareHeader"){
                                if(stream->headers.contains(header))co_return finish(0);
                                if((source_size && !source) || !destination || !destination_size ||
                                   source_size>64U*1024*1024 || destination_size>64U*1024*1024)co_return finish(11);
                                m.check(GuestAddress{source},source_size,Access::Read);
                                m.check(GuestAddress{destination},destination_size,Access::Write);
                                stream->headers.emplace(header,acm::DriverStream::Header{source,destination,source_size,destination_size});
                                const auto result=co_await stream_message(acm::stream_prepare,header);
                                if(result && result!=8){
                                    stream->headers.erase(header);
                                    co_return finish(result);
                                }
                                m.store(GuestAddress{header+4},32,(status&~done)|prepared);
                                co_return finish(0);
                            }
                            const auto expected=stream->headers.find(header);
                            if(symbol=="acmStreamUnprepareHeader"){
                                if(expected==stream->headers.end())co_return finish(0);
                                const auto result=co_await stream_message(acm::stream_unprepare,header);
                                if(result && result!=8)co_return finish(result);
                                stream->headers.erase(expected);
                                m.store(GuestAddress{header+4},32,status&~prepared);
                                co_return finish(0);
                            }
                            if(!convert)co_return finish(8);
                            if(expected==stream->headers.end())co_return finish(514);
                            if(source!=expected->second.source || destination!=expected->second.destination ||
                               source_size>expected->second.source_length || destination_size>expected->second.destination_length)
                                co_return finish(11);
                            m.check(GuestAddress{source},source_size,Access::Read);
                            m.check(GuestAddress{destination},destination_size,Access::Write);
                            m.store(GuestAddress{header+abi.source_length()+4},32,0);
                            m.store(GuestAddress{header+abi.destination_length()+4},32,0);
                            m.store(GuestAddress{header+4},32,status&~done);
                            const auto converted=co_await stream_message(acm::stream_convert,header);
                            if(converted)co_return finish(converted);
                            if(load32(header,abi.source_length()+4)>source_size ||
                               load32(header,abi.destination_length()+4)>destination_size){
                                m.store(GuestAddress{header+abi.source_length()+4},32,0);
                                m.store(GuestAddress{header+abi.destination_length()+4},32,0);
                                co_return finish(11);
                            }
                            status=load32(header,4);
                            m.store(GuestAddress{header+4},32,status|prepared|done);
                            co_return finish(0);
                        }
