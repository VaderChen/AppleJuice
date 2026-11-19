        if(symbol=="AVIMakeCompressedStream"){
            if(!output)co_return finish(bad_parameter);
            m.store(GuestAddress{output},bits,0);
            if(!args(1))co_return finish(bad_handle);
            auto cls=args(3);
            if(!cls){
                const auto info=co_await service_callback::call(method(args(1),4),{args(1),temp,204});
                if(failed(info))co_return finish(info);
                std::u16string key=u"AVIFile\\Compressors\\";
                for(unsigned i=0;i<4;++i)key+=static_cast<char16_t>(m.load(GuestAddress{temp+i},8));
                const auto value=class_registry_value(key,u"");
                const auto id=value?parse_windows_guid(*value,true):std::nullopt;
                if(!id)co_return finish(unsupported);
                cls=temp+32;m.write(GuestAddress{cls},*id);
            }
            m.write(GuestAddress{temp+16},ole_standard_guid(0x20021));
            auto hr=co_await service_callback::call(resolve("ole32.dll","CoCreateInstance").value,{cls,0,3,temp+16,temp});
            if(failed(hr))co_return finish(hr);
            const auto stream=ptr(temp);
            if(!stream)co_return finish(bad_handle);
            hr=co_await service_callback::call(method(stream,3),{stream,args(1),args(2)});
            if(failed(hr))(void)co_await service_callback::call(method(stream,2),{stream});
            else m.store(GuestAddress{output},bits,stream);
            co_return finish(hr);
        }
        if(symbol.starts_with("AVISave")){
            const bool vector_form=symbol.starts_with("AVISaveV");
            const auto count=static_cast<std::int32_t>(args(3));
            if(!output || count<=0 || !args(4) || (vector_form && !args(5)))co_return finish(bad_parameter);
            if(count>100)co_return finish(0x80044068U);
            const auto unit=bits/8U;
            const WindowsCall parameters(s,m,vector_form?6U:4U+2U*static_cast<unsigned>(count),vector_form);
            struct Track {
                std::uint64_t source{},options{},dest{};
                std::uint32_t type{},start{},length{},used{},fixed{},scale{},rate{},interval{},flags{};
                bool retained{};
                std::array<std::uint8_t,204> info{};
            };
            std::vector<Track> tracks(static_cast<unsigned>(count));
            for(unsigned i=0;i<tracks.size();++i){
                tracks[i].source=vector_form?ptr(args(4)+i*unit):parameters(4+2*i);
                tracks[i].options=vector_form?ptr(args(5)+i*unit):parameters(5+2*i);
                if(!tracks[i].source)co_return finish(bad_handle);
            }
            const auto callback=args(2);
            std::uint64_t file=0;
            constexpr std::uint32_t read_error=0x8004406d,write_error=0x8004406e,no_memory=0x80044067,user_abort=0x800440c6;
            auto save=[&]()->service_callback::Task<std::uint32_t>{
                const auto open=resolve("avifil32.dll",symbol.ends_with('W')?"AVIFileOpenW":"AVIFileOpenA");
                auto hr=static_cast<std::uint32_t>(co_await service_callback::call(open.value,{temp,output,0x1011,args(1)}));
                if(failed(hr))co_return hr;
                file=ptr(temp);if(!file)co_return bad_handle;
                std::uint64_t total=0;
                unsigned interval=0;
                auto field=[&](unsigned at){return static_cast<unsigned>(m.load(GuestAddress{temp+at},32));};
                for(auto& track:tracks){
                    if(track.options){
                        if(m.load(GuestAddress{track.options+20},32)&1)
                            track.interval=static_cast<unsigned>(m.load(GuestAddress{track.options+(x86_?40:52)},32));
                        interval=std::max(interval,track.interval);
                        const auto handler=m.load(GuestAddress{track.options+4},32);
                        const auto format=ptr(track.options+24);
                        bool convert=handler || format;
                        // 選項對話框可以交回來源格式的獨立副本。逐位元組相同時沿用原封包。
                        if(!handler && format && !ptr(track.options+(x86_?32:40))){
                            const auto requested=m.load(GuestAddress{track.options+24+unit},32);
                            if(requested && requested<=65536){
                                const auto info=co_await service_callback::call(method(track.source,4),{track.source,temp,204});
                                if(failed(info))co_return static_cast<unsigned>(info);
                                const auto start=field(28);
                                m.store(GuestAddress{temp},32,0);
                                auto status=co_await service_callback::call(method(track.source,6),{track.source,start,0,temp});
                                if(!failed(status) && field(0)==requested){
                                    GuestScratch block(*this,m,s);
                                    const auto source=block.allocate(requested);if(!source)co_return no_memory;
                                    status=co_await service_callback::call(method(track.source,6),{track.source,start,source,temp});
                                    if(!failed(status) && field(0)==requested){
                                        std::vector<std::uint8_t> a(requested),b(requested);
                                        m.read(GuestAddress{source},a);m.read(GuestAddress{format},b);convert=a!=b;
                                    }
                                }
                            }
                        }
                        if(convert){
                            auto address=resolve("avifil32.dll","AVIMakeCompressedStream");
                            hr=static_cast<unsigned>(co_await service_callback::call(address.value,{temp,track.source,track.options,0}));
                            if(failed(hr))co_return hr;
                            track.source=ptr(temp);
                            if(!track.source)co_return bad_handle;
                            track.retained=true;
                        }
                    }
                    if(!track.retained){
                        (void)co_await service_callback::call(method(track.source,1),{track.source});
                        track.retained=true;
                    }
                    hr=static_cast<unsigned>(co_await service_callback::call(method(track.source,4),{track.source,temp,204}));
                    if(failed(hr))co_return hr;
                    track.type=field(0);track.flags=field(8);track.start=field(28);track.length=field(32);
                    track.fixed=field(48);track.scale=field(20);track.rate=field(24);
                    if(!track.scale || !track.rate || std::uint64_t(track.start)+track.length>INT32_MAX)co_return bad_parameter;
                    m.store(GuestAddress{temp+36},32,0);
                    m.read(GuestAddress{temp},track.info);total+=track.length;
                }
                const auto video=std::find_if(tracks.begin(),tracks.end(),[](const auto& t){return t.type==0x73646976U;});
                if(video!=tracks.end())std::iter_swap(tracks.begin(),video);
                auto transfer_metadata=[&](Track& t,bool format,unsigned pos)->service_callback::Task<unsigned>{
                    constexpr unsigned handler_data=0x64727473U;
                    const auto query=format?6U:11U,set=format?7U:12U;
                    const auto id=format?pos:handler_data;
                    m.store(GuestAddress{temp},32,0);
                    auto status=static_cast<unsigned>(co_await service_callback::call(method(t.source,query),{t.source,id,0,temp}));
                    if(failed(status))co_return format?status:0U;
                    const auto size=field(0);
                    if(!size)co_return format?0x80044066U:0U;
                    if(size>16U*1024*1024)co_return no_memory;
                    GuestScratch block(*this,m,s);const auto bytes=block.allocate(size);
                    if(!bytes)co_return no_memory;
                    status=static_cast<unsigned>(co_await service_callback::call(method(t.source,query),{t.source,id,bytes,temp}));
                    if(failed(status))co_return status;
                    const auto actual=field(0);
                    if(actual>size)co_return read_error;
                    co_return static_cast<unsigned>(co_await service_callback::call(method(t.dest,set),{t.dest,id,bytes,actual}));
                };
                for(auto& t:tracks){
                    m.write(GuestAddress{temp},t.info);
                    const auto out_stream=scratch.allocate(unit);
                    if(!out_stream)co_return no_memory;
                    m.store(GuestAddress{out_stream},bits,0);
                    hr=static_cast<unsigned>(co_await service_callback::call(method(file,5),{file,out_stream,temp}));
                    if(failed(hr))co_return hr;
                    t.dest=ptr(out_stream);if(!t.dest)co_return bad_handle;
                    hr=co_await transfer_metadata(t,true,t.start);if(failed(hr))co_return hr;
                    hr=co_await transfer_metadata(t,false,0);if(failed(hr))co_return hr;
                }
                std::uint64_t done=0;unsigned previous=UINT32_MAX;
                std::uint64_t record_number=UINT64_MAX;
                while(done<total){
                    // 要求交錯時，按各串流的有理數時間排列封包，保留不同的音訊與視訊取樣率。
                    auto next=tracks.end();
                    for(auto it=tracks.begin();it!=tracks.end();++it){
                        if(it->used==it->length)continue;
                        if(next==tracks.end() || static_cast<unsigned __int128>(it->start+it->used)*it->scale*next->rate <
                            static_cast<unsigned __int128>(next->start+next->used)*next->scale*it->rate)next=it;
                        if(!interval)break;
                    }
                    auto& t=*next;const auto pos=t.start+t.used;
                    if(interval){
                        const auto& first=tracks.front();
                        const auto window=static_cast<std::uint64_t>(static_cast<unsigned __int128>(pos)*t.scale*first.rate/
                            (static_cast<unsigned __int128>(t.rate)*first.scale*interval));
                        if(window!=record_number){
                            hr=static_cast<unsigned>(co_await service_callback::call(method(file,8),{file}));
                            if(failed(hr))co_return hr;
                            record_number=window;
                        }
                    }
                    if((t.flags&0x10000) && t.used){
                        hr=co_await transfer_metadata(t,true,pos);if(failed(hr))co_return hr;
                    }
                    unsigned batch=t.fixed?std::max(1U,65536U/t.fixed):1U;
                    if(interval && t.fixed){
                        const auto& clock=tracks.front();
                        const auto samples=static_cast<unsigned __int128>(interval)*clock.scale*t.rate/(static_cast<std::uint64_t>(clock.rate)*t.scale);
                        batch=std::min(batch,static_cast<unsigned>(std::clamp<unsigned __int128>(samples,1,UINT32_MAX)));
                    }
                    batch=std::min(batch,t.length-t.used);
                    hr=static_cast<unsigned>(co_await service_callback::call(method(t.source,8),{t.source,pos,batch,0,0,temp,temp+4}));
                    if(failed(hr))co_return hr;
                    const auto size=field(0);
                    if(size>128U*1024*1024)co_return no_memory;
                    GuestScratch block(*this,m,s);const auto data=block.allocate(size);
                    if(!data)co_return no_memory;
                    hr=static_cast<unsigned>(co_await service_callback::call(method(t.source,8),{t.source,pos,batch,data,size,temp,temp+4}));
                    if(failed(hr))co_return hr;
                    const auto bytes=field(0),samples=field(4);
                    if(bytes>size || !samples || samples>batch)co_return read_error;
                    const auto key=co_await service_callback::call(method(t.source,5),{t.source,pos,0x14});
                    hr=static_cast<unsigned>(co_await service_callback::call(method(t.dest,9),{t.dest,UINT32_MAX,samples,data,bytes,
                        static_cast<unsigned>(key)==pos?0x10U:0U,temp,temp+4}));
                    if(failed(hr))co_return hr;
                    if(field(0)!=samples || field(4)!=bytes)co_return write_error;
                    t.used+=samples;done+=samples;
                    const auto percent=static_cast<unsigned>(done*100/total);
                    if(callback && percent!=previous){
                        previous=percent;
                        if(static_cast<unsigned>(co_await service_callback::call(callback,{percent})))co_return user_abort;
                    }
                }
                if(interval)co_return static_cast<unsigned>(co_await service_callback::call(method(file,8),{file}));
                co_return 0U;
            };
            const auto status=co_await save();
            for(const auto& t:tracks){
                if(t.dest)(void)co_await service_callback::call(method(t.dest,2),{t.dest});
                if(t.retained)(void)co_await service_callback::call(method(t.source,2),{t.source});
            }
            if(file)(void)co_await service_callback::call(method(file,2),{file});
            co_return finish(status);
        }
