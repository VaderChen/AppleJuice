    if(name=="CreateEditableStream" || name.starts_with("EditStream") ||
       service.family=="internal-avi-editable" || service.family=="internal-avi-edit"){
        const auto stream_table=table(Kind::editable),edit_table=table(Kind::edit);
        return nested_continuation(state,memory,"AVI 串流編輯",[this,service,stream_table,edit_table](GuestState& s,GuestMemory& m)
            ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall args(s,m,service);
            const auto op=service.symbol;
            const unsigned width=x86_?4U:8U;
            const auto ptrbits=static_cast<std::uint8_t>(width*8);
            auto ptr=[&](std::uint64_t p){return m.load(GuestAddress{p},ptrbits);};
            auto storeptr=[&](std::uint64_t p,std::uint64_t v){m.store(GuestAddress{p},ptrbits,v);};
            auto slot=[&](std::uint64_t p,unsigned n){return ptr(ptr(p)+width*n);};
            auto get=[&](std::uint64_t p){return static_cast<unsigned>(m.load(GuestAddress{p},32));};
            auto set=[&](std::uint64_t p,unsigned v){if(p)m.store(GuestAddress{p},32,v);};
            auto finish=[&](std::uint64_t v){args.return_to(s,v);return std::optional<std::uint32_t>{};};
            auto failed=[](std::uint64_t hr){return static_cast<std::int32_t>(hr)<0;};
            const auto owner=args(0);
            GuestScratch scratch(*this,m,s);
            const auto tmp=scratch.allocate(244);
            if(!tmp)co_return finish(no_memory);
            auto& objects=avi_disk_->edits;
            auto make=[&](const avi::EditList& list,std::uint64_t out)->service_callback::Task<unsigned>{
                const auto base=allocate_heap(m,width*2);
                if(!base)co_return no_memory;
                auto copy=std::make_shared<avi::EditList>(list);
                copy->refs=1;
                for(auto leaf:copy->sources())(void)co_await service_callback::call(slot(leaf,1),{leaf});
                storeptr(base,stream_table);storeptr(base+width,edit_table);
                objects.emplace(base,copy);storeptr(out,base);co_return 0U;
            };
            auto load_list=[&](std::uint64_t src,avi::EditList& list)->service_callback::Task<unsigned>{
                const auto found=objects.find(src);
                if(found!=objects.end()){list=*found->second;co_return 0U;}
                const auto hr=co_await service_callback::call(slot(src,4),{src,tmp,204});
                if(failed(hr))co_return static_cast<unsigned>(hr);
                m.read(GuestAddress{tmp},list.info);
                list.append({src,word(list.info,28),word(list.info,32)});
                co_return 0U;
            };
            if(op=="CreateEditableStream" || op.starts_with("EditStream")){
                const bool create=op=="CreateEditableStream",clone=op=="EditStreamClone";
                const bool cut=op=="EditStreamCut",copy=op=="EditStreamCopy",paste=op=="EditStreamPaste";
                const auto src=create?args(1):owner;
                const auto out=create?owner:clone?args(1):(cut||copy)?args(3):0;
                if(out)storeptr(out,0);
                if(!src && !create)co_return finish(bad_handle);
                if(((create||clone||copy) && !out) || (!create && !args(1)) ||
                   ((cut||copy||paste) && !args(2)))co_return finish(bad_parameter);
                if(paste && !args(3))co_return finish(bad_handle);
                if(paste && failed(args(4)))co_return finish(bad_parameter);
                if(!src)co_return finish(co_await make({},out));
                auto data=args(1),size=std::uint64_t{204};
                if(op.starts_with("EditStreamSet")){
                    const bool wide=op.ends_with('W');
                    const bool rename=op.starts_with("EditStreamSetName");
                    if(rename){
                        const auto hr=co_await service_callback::call(slot(src,4),{src,tmp,204});
                        if(failed(hr))co_return finish(hr);
                    }else{
                        const auto n=static_cast<std::int32_t>(args(2));
                        if(n>=0 && n<(wide?204:140))co_return finish(bad_size);
                        if(!wide){std::array<std::uint8_t,76> fields;m.read(GuestAddress{data},fields);m.write(GuestAddress{tmp},fields);}
                    }
                    if(rename || !wide){
                        const auto text=wide?read_utf16(m,GuestAddress{data}):utf8_to_utf16_lossy(read_ansi(m,data+(rename?0:76),rename?32767:64));
                        m.fill(GuestAddress{tmp+76},0,128);
                        m.write(GuestAddress{tmp+76},utf16_bytes(text.substr(0,63)));data=tmp;
                    }else size=args(2);
                }
                m.write(GuestAddress{tmp+208},ole_standard_guid(0x20024));storeptr(tmp+224,0);
                const auto qi=co_await service_callback::call(slot(src,0),{src,tmp+208,tmp+224});
                const auto edit=ptr(tmp+224);
                if(failed(qi) || !edit){
                    if(!create)co_return finish(unsupported);
                    avi::EditList list;
                    const auto hr=co_await load_list(src,list);
                    if(hr)co_return finish(hr);
                    co_return finish(co_await make(list,out));
                }
                std::uint64_t hr;
                if(create||clone)hr=co_await service_callback::call(slot(edit,6),{edit,out});
                else if(cut||copy)hr=co_await service_callback::call(slot(edit,cut?3:4),{edit,args(1),args(2),out});
                else if(paste)hr=co_await service_callback::call(slot(edit,5),{edit,args(1),args(2),args(3),args(4),args(5)});
                else hr=co_await service_callback::call(slot(edit,7),{edit,data,size});
                (void)co_await service_callback::call(slot(edit,2),{edit});co_return finish(hr);
            }
            const auto base=service.family=="internal-avi-edit"?owner-width:owner;
            const auto found=objects.find(base);
            if(found==objects.end())co_return finish(bad_handle);
            const auto list=found->second;
            if(op=="AddRef")co_return finish(++list->refs);
            if(op=="Release"){
                if(--list->refs)co_return finish(list->refs);
                objects.erase(base);
                for(auto leaf:list->sources())(void)co_await service_callback::call(slot(leaf,2),{leaf});
                free_heap(m,base,s);co_return finish(0);
            }
            if(op=="QueryInterface"){
                if(!args(1)||!args(2))co_return finish(pointer_error);
                WindowsGuid id{};m.read(GuestAddress{args(1)},id);storeptr(args(2),0);
                const bool edit=id==ole_standard_guid(0x20024);
                if(!edit && id!=ole_standard_guid(0) && id!=ole_standard_guid(0x20021))co_return finish(no_interface);
                ++list->refs;storeptr(args(2),edit?base+width:base);co_return finish(0);
            }
            const unsigned start=word(list->info,28),length=word(list->info,32);
            if(op=="Info"){
                if(!args(1))co_return finish(bad_parameter);
                if(failed(args(2)))co_return finish(bad_size);
                const auto n=std::min<std::size_t>(args(2),204);
                m.write(GuestAddress{args(1)},avi::View(list->info).first(n));co_return finish(n==204?0:too_small);
            }
            if(op=="SetInfo"){
                if(!args(1))co_return finish(bad_parameter);
                if(!failed(args(2)) && args(2)<204)co_return finish(bad_size);
                std::array<std::uint8_t,204> info;m.read(GuestAddress{args(1)},info);
                for(auto offset:{16U,20U,24U,28U,44U,52U,56U,60U,64U})put(list->info,offset,word(info,offset));
                std::copy(info.begin()+76,info.end(),list->info.begin()+76);
                put(list->info,68,word(list->info,68)+1);co_return finish(0);
            }
            if(op=="Clone"){
                if(!args(1))co_return finish(bad_parameter);
                storeptr(args(1),0);co_return finish(co_await make(*list,args(1)));
            }
            if(op=="Cut" || op=="Copy" || op=="Paste" || op=="Delete"){
                const bool paste=op=="Paste",copy=op=="Copy",del=op=="Delete";
                if(!del && (!args(1)||!args(2)||(copy&&!args(3))))co_return finish(bad_parameter);
                unsigned pos=del?static_cast<unsigned>(args(1)):get(args(1));
                const unsigned requested=del?static_cast<unsigned>(args(2)):paste?0:get(args(2));
                if(pos>INT32_MAX || requested>INT32_MAX)co_return finish(bad_parameter);
                if(!length && paste && pos!=start)co_return finish(bad_parameter);
                const auto origin=!length && paste?pos:start;
                if(pos<origin || pos-origin>length)co_return finish(bad_parameter);
                const unsigned offset=pos-origin;
                unsigned n=std::min(requested,length-offset);
                auto next=*list;
                next.parts.clear();next.append_slice(*list,0,offset);
                if(paste){
                    if(!args(3))co_return finish(bad_handle);
                    avi::EditList src;
                    auto hr=co_await load_list(args(3),src);
                    if(hr)co_return finish(hr);
                    const unsigned first=static_cast<unsigned>(args(4)),src_start=word(src.info,28),src_len=word(src.info,32);
                    const auto wanted=static_cast<std::int32_t>(args(5));
                    if(first<src_start || first-src_start>src_len || wanted< -1)co_return finish(bad_parameter);
                    n=src_len-(first-src_start);
                    if(wanted>=0)n=std::min(n,static_cast<unsigned>(wanted));
                    if(std::uint64_t(origin)+length+n>INT32_MAX)co_return finish(bad_size);
                    if(length && n){
                        if(word(src.info,0)!=word(list->info,0))co_return finish(unsupported);
                        if(word(src.info,0)==avi::video){
                            for(const auto axis:{52U,56U})
                                if(word(src.info,axis+8)-word(src.info,axis)!=word(list->info,axis+8)-word(list->info,axis))
                                    co_return finish(unsupported);
                        }
                        if(word(src.info,0)==avi::audio){
                            Bytes formats[2];
                            auto read_format=[&](const avi::EditPart& part,Bytes& bytes)->service_callback::Task<unsigned>{
                                set(tmp,0);
                                auto r=co_await service_callback::call(slot(part.stream,6),{part.stream,part.first,0,tmp});
                                const unsigned size=get(tmp);
                                if(failed(r)||!size||size>65536)co_return bad_format;
                                const auto buf=scratch.allocate(size);if(!buf)co_return no_memory;
                                r=co_await service_callback::call(slot(part.stream,6),{part.stream,part.first,buf,tmp});
                                if(failed(r)||get(tmp)!=size)co_return bad_format;
                                bytes.resize(size);m.read(GuestAddress{buf},bytes);co_return 0U;
                            };
                            hr=co_await read_format(list->parts.front(),formats[0]);
                            if(!hr)hr=co_await read_format(src.parts.front(),formats[1]);
                            if(hr || formats[0]!=formats[1])co_return finish(hr?hr:bad_format);
                        }
                    }
                    if(!length){next.info=src.info;put(next.info,28,origin);}
                    next.append_slice(src,first-src_start,n);next.append_slice(*list,offset,length-offset);
                    put(next.info,32,length+n);
                }else{
                    if(!del && args(3)){
                        avi::EditList selection;selection.info=list->info;selection.append_slice(*list,offset,n);
                        put(selection.info,28,0);put(selection.info,32,n);
                        selection.decode_video=word(selection.info,0)==avi::video;
                        if(selection.decode_video){put(selection.info,4,0);put(selection.info,48,0);}
                        const auto hr=co_await make(selection,args(3));if(hr)co_return finish(hr);
                    }
                    next.append_slice(*list,offset+n,length-offset-n);put(next.info,32,length-n);
                }
                if(!copy && n){
                    const auto old=list->sources(),added=next.sources();
                    for(auto leaf:added)if(!old.contains(leaf))(void)co_await service_callback::call(slot(leaf,1),{leaf});
                    next.decode_video=word(next.info,0)==avi::video;
                    if(next.decode_video){put(next.info,4,0);put(next.info,48,0);}
                    put(next.info,68,word(list->info,68)+1);
                    list->parts=std::move(next.parts);list->info=next.info;list->decode_video=next.decode_video;
                    for(auto leaf:old)if(!added.contains(leaf))(void)co_await service_callback::call(slot(leaf,2),{leaf});
                }
                if(!del){set(args(1),pos);set(args(2),n);}co_return finish(0);
            }
            if(op=="ReadData"){
                for(const auto leaf:list->sources()){
                    const auto hr=co_await service_callback::call(slot(leaf,11),{leaf,args(1),args(2),args(3)});
                    if(static_cast<unsigned>(hr)!=no_data)co_return finish(hr);
                }
                co_return finish(no_data);
            }
            if(op=="FindSample"){
                const unsigned flags=static_cast<unsigned>(args(2));
                const bool previous=(flags&4)!=0,from_start=(flags&8)!=0;
                const auto target=static_cast<std::int32_t>(args(1));
                std::uint64_t answer=UINT32_MAX;
                unsigned cursor=start;
                for(const auto part:list->parts){
                    const auto end=cursor+part.count;
                    if(from_start || (previous?target>=static_cast<std::int64_t>(cursor):target<static_cast<std::int64_t>(end))){
                        const auto at=from_start?cursor:static_cast<unsigned>(std::clamp<std::int64_t>(target,cursor,end-1));
                        const auto mapped=part.first+at-cursor;
                        const auto f=list->decode_video?(flags&~0x10U):flags;
                        auto v=co_await service_callback::call(slot(part.stream,5),{part.stream,mapped,f&0xffU&~8U});
                        if(!failed(v) && v>=part.first && v-part.first<part.count){
                            answer=v-part.first+cursor;
                            if(flags&0xf000U)answer=co_await service_callback::call(slot(part.stream,5),{part.stream,v,f});
                            if(!previous || from_start)break;
                        }
                    }
                    cursor=end;
                }
                co_return finish(answer);
            }
            if(op=="Read" || op=="ReadFormat"){
                const bool format=op=="ReadFormat";
                const unsigned pos=static_cast<unsigned>(args(1));
                if(!format){set(args(5),0);set(args(6),0);}
                if(format && !args(3))co_return finish(bad_parameter);
                if(pos<start || pos-start>length || (format && pos-start==length))co_return finish(no_data);
                const auto requested=static_cast<std::int32_t>(args(2));
                const unsigned capacity=format?get(args(3)):static_cast<unsigned>(args(4));
                if(!format && (requested< -1 || capacity>INT32_MAX))co_return finish(bad_parameter);
                const auto fixed=word(list->info,48);
                unsigned skip=pos-start,bytes=0,samples=0;
                unsigned remaining=format?1U:requested<0?(fixed?capacity/fixed:1U):static_cast<unsigned>(requested);
                if(!fixed)remaining=std::min(remaining,1U);
                for(const auto part:list->parts){
                    if(!remaining)break;
                    if(skip>=part.count){skip-=part.count;continue;}
                    const auto at=part.first+skip,n=std::min(remaining,part.count-skip);
                    if(list->decode_video){
                        m.fill(GuestAddress{tmp},0,40);set(tmp,40);set(tmp+12,(24U<<16)|1);
                        const auto getter=co_await service_callback::call_value(resolve("avifil32.dll","AVIStreamGetFrameOpen").value,{part.stream,tmp});
                        if(!getter)co_return finish(bad_format);
                        const auto dib=co_await service_callback::call_value(slot(getter,3),{getter,at});
                        unsigned hr=0;
                        if(!dib)hr=read_error;
                        else{
                            const unsigned header=get(dib),size=format?header:get(dib+20);
                            const auto dest=format?args(2):args(3);
                            if(format)set(args(3),size);
                            if(dest && size>capacity)hr=too_small;
                            else if(size>memory_limit || header!=40)hr=bad_format;
                            else{
                                if(dest){Bytes data(size);m.read(GuestAddress{dib+(format?0:header)},data);m.write(GuestAddress{dest},data);}
                                if(!format){set(args(5),size);set(args(6),1);}
                            }
                        }
                        (void)co_await service_callback::call(slot(getter,2),{getter});co_return finish(hr);
                    }
                    if(format)co_return finish(co_await service_callback::call(slot(part.stream,6),{part.stream,at,args(2),args(3)}));
                    set(tmp,0);set(tmp+4,0);
                    const auto hr=co_await service_callback::call(slot(part.stream,8),{part.stream,at,n,args(3)?args(3)+bytes:0,capacity-bytes,tmp,tmp+4});
                    if(failed(hr))co_return finish(hr);
                    const auto got=get(tmp),read=get(tmp+4);
                    if(read>n || got>UINT32_MAX-bytes || (args(3)&&got>capacity-bytes))co_return finish(read_error);
                    bytes+=got;samples+=read;remaining-=read;skip=0;
                    set(args(5),bytes);set(args(6),samples);
                    if(read<n)break;
                }
                co_return finish(0);
            }
            if(op=="Write"){set(args(6),0);set(args(7),0);}
            co_return finish(unsupported);
        });
    }
