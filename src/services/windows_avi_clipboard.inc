    if(name.ends_with("Clipboard")){
        const auto st=table(Kind::stream),ft=table(Kind::file),pt=table(Kind::persist);
        return nested_continuation(state,memory,"AVI 剪貼簿",[this,service,st,ft,pt](GuestState& s,GuestMemory& m)
            ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall args(s,m,service);
            const auto op=service.symbol;
            const unsigned width=x86_?4:8;
            const auto bits=static_cast<std::uint8_t>(width*8);
            auto ptr=[&](std::uint64_t p){return m.load(GuestAddress{p},bits);};
            auto store_ptr=[&](std::uint64_t p,std::uint64_t v){m.store(GuestAddress{p},bits,v);};
            auto get=[&](std::uint64_t p){return static_cast<unsigned>(m.load(GuestAddress{p},32));};
            auto set=[&](std::uint64_t p,unsigned v){m.store(GuestAddress{p},32,v);};
            auto slot=[&](std::uint64_t p,unsigned n){return ptr(ptr(p)+width*n);};
            auto api=[&](std::string_view dll,std::string_view sym,std::vector<std::uint64_t> argv){
                return service_callback::call_value(resolve(dll,sym).value,std::move(argv));
            };
            auto finish=[&](unsigned hr){args.return_to(s,hr);return std::optional<std::uint32_t>{};};
            GuestScratch scratch(*this,m,s);
            const auto tmp=scratch.allocate(220);
            if(!tmp)co_return finish(no_memory);
            auto& objects=avi_disk_->objects;
            auto publish_object=[&](avi::Movie movie,bool file){
                const auto p=allocate_heap(m,file?2*width:width);if(!p)return std::uint64_t{};
                auto disk=std::make_shared<DiskMovie>();disk->movie=std::move(movie);disk->memory_backed=true;
                store_ptr(p,file?ft:st);if(file)store_ptr(p+width,pt);
                objects.emplace(p,DiskObject{file?Kind::file:Kind::stream,std::move(disk),0});
                return p;
            };
            auto read_block=[&](std::uint64_t handle,Bytes& bytes){
                const auto it=gui_.global_memory.find(handle);
                if(it==gui_.global_memory.end() || !it->second.address)return bad_handle;
                if(!it->second.size || it->second.size>memory_limit)return bad_size;
                bytes.resize(it->second.size);m.read(GuestAddress{it->second.address},bytes);return 0U;
            };
            const auto format_name=scratch.text(u"AVIFile");
            if(!format_name)co_return finish(no_memory);
            const auto avi_format=static_cast<unsigned>(co_await api("user32.dll","RegisterClipboardFormatW",{format_name}));
            if(!avi_format)co_return finish(no_memory);
            const bool make=op=="AVIMakeStreamFromClipboard",take=op=="AVIGetFromClipboard";
            if(make || take || op=="AVIClearClipboard"){
                const auto output=make?args(2):take?args(0):0;
                if((make || take) && !output)co_return finish(bad_handle);
                if(output)store_ptr(output,0);
                if(!make && !(co_await api("user32.dll","OpenClipboard",{0})))co_return finish(open_error);
                unsigned hr=0;
                avi::Movie movie;
                if(op=="AVIClearClipboard"){
                    if((co_await api("user32.dll","IsClipboardFormatAvailable",{avi_format})) &&
                       !(co_await api("user32.dll","EmptyClipboard",{})))hr=write_error;
                }else{
                    std::vector<unsigned> formats;
                    if(make)formats.push_back(static_cast<unsigned>(args(0)));
                    else formats={avi_format,11,8,12};
                    for(auto fmt:formats){
                        const auto handle=make?args(1):co_await api("user32.dll","GetClipboardData",{fmt});
                        if(!handle){if(make)hr=bad_handle;continue;}
                        if(fmt!=avi_format && fmt!=11 && fmt!=8 && fmt!=17 && fmt!=12){hr=unsupported;break;}
                        Bytes bytes;hr=read_block(handle,bytes);
                        if(hr)break;
                        try{
                            if((fmt==avi_format || fmt==11) && bytes.size()>=12 && word(bytes,8)==avi::fourcc('A','V','I',' ')){
                                movie=avi::decode(bytes);break;
                            }
                            if(fmt==11 && !make)continue;
                            if(!avi::append_clipboard_stream(movie,fmt,bytes)){hr=bad_format;break;}
                        }catch(const std::invalid_argument&){hr=bad_format;break;}
                    }
                    if(!hr && movie.streams.empty())hr=no_data;
                }
                if(!make && !(co_await api("user32.dll","CloseClipboard",{})) && !hr)hr=open_error;
                if(hr || !output)co_return finish(hr);
                update_main(movie);
                const auto obj=publish_object(std::move(movie),take);if(!obj)co_return finish(no_memory);
                if(take){store_ptr(output,obj);co_return finish(0);}
                hr=static_cast<unsigned>(co_await api("avifil32.dll","CreateEditableStream",{output,obj}));
                (void)co_await service_callback::call(slot(obj,2),{obj});co_return finish(hr);
            }
            const auto file=args(0);
            if(!file)co_return finish(bad_handle);
            avi::Movie movie;
            const auto own=objects.find(file);
            const bool native=own!=objects.end() && own->second.kind==Kind::file;
            if(native)movie=own->second.disk->movie;
            auto hr=co_await service_callback::call(slot(file,3),{file,tmp,172});
            if(hr)co_return finish(hr);
            const auto count=get(tmp+12);
            if(count>100)co_return finish(bad_size);
            Bytes dib;std::vector<unsigned> colors;
            bool saw_video=false;
            std::size_t total=0;
            // 自訂 COM 串流使用相同的 Info、ReadFormat、Read 與影格解碼入口。
            auto capture=[&](std::uint64_t stream,unsigned index)->service_callback::Task<unsigned>{
                auto result=co_await service_callback::call(slot(stream,4),{stream,tmp,204});
                if(result)co_return result;
                Bytes info(204);m.read(GuestAddress{tmp},info);
                auto source=unpack_stream(info);
                const auto type=word(info,0),first=word(info,28),length=word(info,32),fixed=word(info,48);
                if(!native){
                    set(tmp,0);result=co_await service_callback::call(slot(stream,6),{stream,first,0,tmp});
                    if(result)co_return result;
                    const auto format_size=get(tmp);
                    if(format_size>memory_limit-total)co_return no_memory;
                    GuestScratch buffers(*this,m,s);
                    const auto format=buffers.allocate(std::max(1U,format_size));if(!format)co_return no_memory;
                    result=co_await service_callback::call(slot(stream,6),{stream,first,format,tmp});
                    if(result || get(tmp)>format_size)co_return result?result:bad_format;
                    source.format.resize(get(tmp));m.read(GuestAddress{format},source.format);total+=source.format.size();
                    auto suffix=type==avi::audio?0x6277U:source.format.size()>=40 && !word(source.format,16)?0x6264U:0x6364U;
                    movie.streams.push_back(std::move(source));
                    for(unsigned pos=0;pos<length;){
                        const unsigned wanted=fixed?std::min(length-pos,std::max(1U,65536U/fixed)):1;
                        result=co_await service_callback::call(slot(stream,8),{stream,first+pos,wanted,0,0,tmp,tmp+4});
                        if(result)co_return result;
                        const auto size=get(tmp);
                        if(size>memory_limit-total || memory_limit-total-size<sizeof(avi::Packet))co_return no_memory;
                        GuestScratch packet(*this,m,s);const auto data=packet.allocate(std::max(1U,size));if(!data)co_return no_memory;
                        result=co_await service_callback::call(slot(stream,8),{stream,first+pos,wanted,data,size,tmp,tmp+4});
                        const auto used=get(tmp),samples=get(tmp+4);
                        if(result || used>size || !samples || samples>wanted)co_return result?result:read_error;
                        const auto key=type==avi::video?co_await service_callback::call(slot(stream,5),{stream,first+pos,0x14}):first+pos;
                        Bytes payload(used);m.read(GuestAddress{data},payload);
                        movie.packets.push_back({index,('0'+index/10)|(('0'+index%10)<<8)|(suffix<<16),
                                                 key==first+pos?0x10U:0U,std::move(payload)});
                        pos+=samples;total+=used+sizeof(avi::Packet);
                    }
                }
                if(type==avi::video && !saw_video && length){
                    saw_video=true;
                    const auto getter=co_await api("avifil32.dll","AVIStreamGetFrameOpen",{stream,0});
                    if(!getter)co_return bad_format;
                    const auto frame=co_await service_callback::call_value(slot(getter,3),{getter,first});
                    if(!frame)result=read_error;
                    else{
                        Bytes header(40);m.read(GuestAddress{frame},header);
                        const auto size=avi::frame_header_size(header);
                        if(!size)result=bad_format;
                        else{
                            header.resize(size);m.read(GuestAddress{frame},header);
                            const auto layout=avi::frame_layout(header);
                            if(!layout || !layout->raw())result=bad_format;
                            else{colors=layout->palette;dib.resize(size+layout->image_size());m.read(GuestAddress{frame},dib);}
                        }
                    }
                    (void)co_await service_callback::call(slot(getter,2),{getter});
                }
                co_return result;
            };
            for(unsigned i=0;i<count;++i){
                store_ptr(tmp,0);
                hr=co_await service_callback::call(slot(file,4),{file,tmp,0,i});
                if(hr)co_return finish(hr);
                const auto stream=ptr(tmp);if(!stream)co_return finish(bad_handle);
                hr=co_await capture(stream,i);
                (void)co_await service_callback::call(slot(stream,2),{stream});
                if(hr)co_return finish(hr);
            }
            if(!native)update_main(movie);
            std::map<unsigned,std::uint64_t> handles;
            auto block=[&](unsigned fmt,const Bytes& data){
                const auto h=allocate_gui_global(m,data.size(),2);if(!h)return false;
                handles.emplace(fmt,h);m.write(GuestAddress{gui_.global_memory.at(h).address},data);return true;
            };
            if(!block(avi_format,avi::encode(movie)))hr=no_memory;
            if(!hr && !dib.empty()){
                if(!block(8,dib))hr=no_memory;
                else if(!colors.empty()){
                    GuiState::Palette palette;palette.entries=std::move(colors);
                    const auto h=gui_.next_object++;gui_.palettes.emplace(h,std::move(palette));handles.emplace(9,h);
                }else{
                    const auto h=co_await api("gdi32.dll","CreateHalftonePalette",{0});
                    if(h)handles.emplace(9,h);else hr=no_memory;
                }
            }
            for(unsigned i=0;!hr && i<movie.streams.size();++i)if(word(movie.streams[i].header,0)==avi::audio){
                if(!block(12,avi::clipboard_wave(movie,i)))hr=no_memory;
                break;
            }
            if(!hr){
                const auto station=gui_.user_handles.find(gui_.station_handle);
                const auto key=std::make_pair(threads_.current,station==gui_.user_handles.end()?1ULL:station->second.object);
                const auto known=gui_.clipboard_windows.find(key);
                std::uint64_t owner=known==gui_.clipboard_windows.end()?0:known->second;
                if(!gui_.windows.contains(owner)){
                    const auto cls=scratch.text(u"Static");
                    if(cls)owner=co_await api("user32.dll","CreateWindowExW",{0,cls,0,0x80000000U,0,0,0,0,UINT64_MAX-2,0,0,0});
                    if(owner)gui_.clipboard_windows[key]=owner;
                }
                if(!owner || !(co_await api("user32.dll","OpenClipboard",{owner})))hr=open_error;
                else{
                    if(!(co_await api("user32.dll","EmptyClipboard",{})))hr=write_error;
                    for(auto& [fmt,h]:handles){
                        if(hr)break;
                        if(co_await api("user32.dll","SetClipboardData",{fmt,h}))h=0;
                        else hr=write_error;
                    }
                    if(hr)(void)co_await api("user32.dll","EmptyClipboard",{});
                    if(!(co_await api("user32.dll","CloseClipboard",{})) && !hr)hr=open_error;
                }
            }
            for(const auto& [fmt,h]:handles)if(h){
                if(fmt==9)gui_.palettes.erase(h);
                else{auto block_it=gui_.global_memory.find(h);if(block_it!=gui_.global_memory.end()){
                    free_heap(m,block_it->second.address,s);gui_.global_memory.erase(block_it);}}
            }
            co_return finish(hr);
        });
    }
