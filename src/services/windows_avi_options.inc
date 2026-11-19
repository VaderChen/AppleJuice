        if(symbol=="AVISaveOptions"){
            const auto count=static_cast<std::int32_t>(args(2));
            if(count<=0 || !args(3) || !args(4))co_return finish(bad_parameter);
            if(count>100)co_return finish(0);
            const auto unit=bits/8U;
            const unsigned opt_size=x86_?44U:56U,interval_at=x86_?40U:52U;
            struct Draft {
                std::uint64_t target{},new_format{},old_format{};
                std::vector<std::uint8_t> options,source_format;
                bool use_source{};
            };
            std::vector<Draft> drafts;
            std::set<std::uint64_t> seen;
            bool interleaved=true;unsigned interval=0;
            const auto decimal=[](unsigned value){return utf8_to_utf16(std::to_string(value));};
            for(int i=0;i<count;++i){
                const auto offset=static_cast<unsigned>(i)*unit;
                const auto stream=ptr(args(3)+offset),target=ptr(args(4)+offset);
                if(!stream)co_return finish(0);
                if(!target || !seen.insert(target).second)continue;
                m.check(GuestAddress{target},opt_size,Access::Write);
                auto hr=co_await service_callback::call(method(stream,4),{stream,temp,204});
                if(failed(hr))co_return finish(0);
                const auto type=static_cast<unsigned>(m.load(GuestAddress{temp},32));
                const bool video=type==0x73646976U,audio=type==0x73647561U;
                if(count==1 && !video && !audio)co_return finish(0);
                const auto start=m.load(GuestAddress{temp+28},32);
                auto label=read_utf16_counted(m,GuestAddress{temp+76},64);
                label.resize(label.find(u'\0')==label.npos?label.size():label.find(u'\0'));
                auto& draft=drafts.emplace_back();draft.target=target;draft.options.resize(opt_size);
                m.read(GuestAddress{target},draft.options);
                const auto field=[&](unsigned n){return static_cast<unsigned>(m.load(GuestAddress{target+n},32));};
                unsigned flags=field(20),handler=field(4);
                if(audio){
                    m.store(GuestAddress{temp},32,0);
                    hr=co_await service_callback::call(method(stream,6),{stream,start,0,temp});
                    const auto size=m.load(GuestAddress{temp},32);
                    if(failed(hr) || size<16 || size>65536)co_return finish(0);
                    const auto format=scratch.allocate(size);
                    if(!format)co_return finish(0);
                    hr=co_await service_callback::call(method(stream,6),{stream,start,format,temp});
                    const auto actual=m.load(GuestAddress{temp},32);
                    if(failed(hr) || actual<16 || actual>size)co_return finish(0);
                    draft.source_format.resize(actual);m.read(GuestAddress{format},draft.source_format);
                }
                DialogTemplate items;
                int y=8;
                auto row=[&](unsigned id,const std::u16string& caption,const std::u16string& value){
                    items.item(0,8,y,118,14,-1,0x82,u"",caption);
                    items.item(0x00810080,130,y,84,14,static_cast<int>(id),0x81,u"",value);y+=19;
                };
                items.item(0,8,y,206,16,-1,0x82,u"",decimal(static_cast<unsigned>(i+1))+u" / "+decimal(static_cast<unsigned>(count))+u"   "+label);y+=21;
                if(video){
                    std::u16string codec;
                    if(handler)for(unsigned n=0;n<4;++n)codec+=static_cast<char16_t>((handler>>(8*n))&255);
                    row(101,u"壓縮器 FOURCC（空白為複製）",codec);
                    row(102,u"品質（0–10000）",(flags&8) && field(12)!=UINT32_MAX?decimal(field(12)):u"");
                    if(args(1)&1)row(103,u"關鍵影格間隔",decimal(flags&4?field(8):0));
                    if(args(1)&2)row(104,u"每秒位元組數",decimal(flags&2?field(16):0));
                }
                bool pcm_choice=false;
                if(audio){
                    const auto registered=class_registry_value(u"AVIFile\\Compressors\\auds",u"");
                    pcm_choice=registered && parse_windows_guid(*registered,true) && draft.source_format[0]==1 && !draft.source_format[1];
                    items.item(0x10003,8,y,204,15,107,0x80,u"",pcm_choice?u"指定 PCM 音訊格式":u"使用此串流的音訊格式");y+=20;
                    if(pcm_choice){
                        const auto prior=ptr(target+24);
                        const bool prior_pcm=prior && field(24+unit)>=16 && m.load(GuestAddress{prior},16)==1;
                        auto pcm_field=[&](unsigned at,unsigned n){
                            if(prior_pcm)return static_cast<unsigned>(m.load(GuestAddress{prior+at},static_cast<std::uint8_t>(n*8)));
                            unsigned value=0;
                            for(unsigned j=0;j<n;++j)value|=static_cast<unsigned>(draft.source_format[at+j])<<(8*j);
                            return value;
                        };
                        row(108,u"取樣頻率（Hz）",decimal(pcm_field(4,4)));
                        row(109,u"聲道數",decimal(pcm_field(2,2)));row(110,u"每樣本位元數",decimal(pcm_field(14,2)));
                    }
                }
                const bool first=drafts.size()==1;
                if(first && count>1){
                    interleaved=!(flags&8) || (flags&1);interval=(flags&8)?field(interval_at):0;
                    items.item(0x10003,8,y,204,15,106,0x80,u"",u"交錯寫入音訊與視訊");y+=20;
                    row(105,u"交錯間隔（影格）",decimal(interval));
                }
                items.item(0x10001,82,y,62,16,1,0x80,u"",i+1<count?u"下一個":u"確定");
                items.item(0x10000,151,y,62,16,2,0x80,u"",u"取消");
                DialogTemplate dlg;
                dlg.dword(0x90c80040);dlg.dword(0);dlg.word(items.count);
                dlg.word(10);dlg.word(10);dlg.word(222);dlg.word(static_cast<unsigned>(y+23));
                dlg.word(0);dlg.word(0);dlg.text(u"AVI 儲存選項");dlg.word(9);dlg.text(u"Segoe UI");dlg.align();
                dlg.bytes.insert(dlg.bytes.end(),items.bytes.begin(),items.bytes.end());
                const auto address=scratch.allocate(dlg.bytes.size());
                if(!address)co_return finish(0);
                m.write(GuestAddress{address},dlg.bytes);m.fill(GuestAddress{temp},0,28);
                m.store(GuestAddress{temp+20},32,interleaved);m.store(GuestAddress{temp+24},32,!ptr(target+24));
                const auto ok=co_await service_callback::call(resolve("user32.dll","DialogBoxIndirectParamW").value,
                    {0,address,args(0),resolve("internal-avifil32-options","DialogProc").value,temp});
                if(ok!=1)co_return finish(0);
                auto value=[&](unsigned at){return static_cast<unsigned>(m.load(GuestAddress{temp+at},32));};
                auto set=[&](unsigned at,unsigned v){for(unsigned n=0;n<4;++n)draft.options[at+n]=static_cast<std::uint8_t>(v>>(8*n));};
                if(video){
                    set(4,value(0));set(12,value(4));
                    if(args(1)&1){set(8,value(8));flags=value(8)?flags|4U:flags&~4U;}
                    if(args(1)&2){set(16,value(12));flags=value(12)?flags|2U:flags&~2U;}
                }
                if(first && count>1){interval=value(16);interleaved=value(20)!=0;}
                if(count>1){flags=interleaved?flags|1U:flags&~1U;if(interleaved)set(interval_at,interval);}
                if(video || audio){set(0,type);flags|=8U;}
                set(20,flags);
                draft.use_source=audio && value(24);
                if(draft.use_source && pcm_choice){
                    const auto hz=value(28),channels=value(32),depth=value(36);
                    const auto alignment=channels*depth/8;
                    draft.source_format.assign(18,0);
                    auto write=[&](unsigned at,unsigned n,unsigned v){
                        for(unsigned k=0;k<n;++k)draft.source_format[at+k]=static_cast<std::uint8_t>(v>>(8*k));
                    };
                    write(0,2,1);write(2,2,channels);write(4,4,hz);write(8,4,hz*alignment);
                    write(12,2,alignment);write(14,2,depth);
                }
                draft.old_format=ptr(target+24);
            }
            if(drafts.empty())co_return finish(0);
            // 所有對話框確認後才配置並提交，取消保留呼叫端的指標與資料。
            for(auto& draft:drafts)if(draft.use_source){
                draft.new_format=allocate_heap(m,draft.source_format.size());
                if(!draft.new_format){
                    for(const auto& prior:drafts)if(prior.new_format)free_heap(m,prior.new_format,s);
                    co_return finish(0);
                }
                m.write(GuestAddress{draft.new_format},draft.source_format);
            }
            for(const auto& draft:drafts){
                m.write(GuestAddress{draft.target},draft.options);
                if(draft.new_format){
                    m.store(GuestAddress{draft.target+24},bits,draft.new_format);
                    m.store(GuestAddress{draft.target+24+unit},32,draft.source_format.size());
                    if(draft.old_format)free_heap(m,draft.old_format,s);
                }
            }
            co_return finish(1);
        }
