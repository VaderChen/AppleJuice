                    if(name=="ICInstall" || name=="ICRemove"){
                        const auto type=word_arg(0),handler=word_arg(1);
                        if(!vfw::read_fourcc(vfw::fcc_text(type)) || !vfw::read_fourcc(vfw::fcc_text(handler)))co_return finish(0);
                        const auto key=fold_key(type,handler);
                        const bool install=name=="ICInstall";
                        const auto flags=word_arg(install?4:2);
                        const auto basic=flags&0x7fffU;
                        if(flags&~0x8007U)co_return finish(0);
                        if(install && (basic==1 || basic==4)){
                            if(!args(2))co_return finish(0);
                            std::u16string description;
                            if(args(3))description=(flags&0x8000U)?read_utf16(m,GuestAddress{args(3)},256):
                                utf8_to_utf16_lossy(read_ansi(m,args(3),256));
                            if(basic==4 && !installable_drivers_.contains(args(2)))co_return finish(0);
                            vfw::Driver driver{type,handler,basic==1?Backend::Callback:Backend::InstallableDriver,
                                {},std::move(description),{},basic==1?args(2):0,basic==4?args(2):0};
                            if(store.codec_overrides.contains(key))co_return finish(0);
                            store.codec_overrides.emplace(key,std::move(driver));
                            co_return finish(1);
                        }
                        if(!install && (basic==0 || basic==1 || basic==4) && store.codec_overrides.erase(key))co_return finish(1);
                        if(basic!=2 && !(basic==0 && !install))co_return finish(0);
                        const auto machine=x86_?UINT64_C(0x80000002):UINT64_C(0xffffffff80000002);
                        const auto path=guest_text(vfw::drivers32);
                        const auto label=guest_text(vfw::driver_name(type,handler));
                        if(!install)
                            co_return finish(0==co_await api("advapi32.dll","RegDeleteKeyValueW",{machine,path,label}));
                        if(!args(2))co_return finish(0);
                        const auto file=(flags&0x8000U)?read_utf16(m,GuestAddress{args(2)},32767):
                            utf8_to_utf16_lossy(read_ansi(m,args(2),32767));
                        if(file.empty())co_return finish(0);
                        const auto data=utf16_bytes(file);
                        const auto status=co_await api("advapi32.dll","RegSetKeyValueW",
                            {machine,path,label,1,guest_bytes(data),data.size()});
                        co_return finish(status==0);
                    }
                    if(name=="ICGetDisplayFormat"){
                        const auto input=header_at(args(1));
                        const auto layout=avi::frame_layout(input);
                        if(!layout || !args(2))co_return finish(0);
                        auto output=avi::frame_output(input,*layout,{});
                        if(output.empty())co_return finish(0);
                        if(word_arg(3) || word_arg(4) || word_arg(5)){
                            const auto depth=word_arg(3)?word_arg(3):(avi::word(output,12)>>16);
                            if(depth!=16 && depth!=24 && depth!=32)co_return finish(0);
                            output.assign(40,0);
                            avi::put(output,0,40);
                            avi::put(output,4,word_arg(4)?word_arg(4):layout->width);
                            avi::put(output,8,word_arg(5)?word_arg(5):avi::word(input,8));
                            avi::put(output,12,(depth<<16)|1);
                            const auto new_layout=avi::frame_layout(output);
                            if(!new_layout)co_return finish(0);
                            avi::put(output,20,new_layout->image_size());
                        }
                        const auto proposed=guest_bytes(output);
                        m.check(GuestAddress{args(2)},output.size(),Access::Write);
                        auto handle=args(0);
                        if(handle && co_await send(handle,decompress_query,args(1),proposed))co_return finish(0);
                        if(!handle)handle=co_await open(vfw::video_type,0,2,args(1),proposed);
                        if(handle)m.write(GuestAddress{args(2)},output);
                        co_return finish(handle);
                    }
                    if(name=="ICImageDecompress"){
                        const auto input=header_at(args(2));
                        const auto layout=avi::frame_layout(input);
                        if(!layout || !args(3))co_return finish(0);
                        auto output=args(4)?header_at(args(4)):avi::frame_output(input,*layout,{});
                        const auto destination=avi::frame_layout(output);
                        if(!destination || !destination->raw())co_return finish(0);
                        avi::put(output,20,destination->image_size());
                        const auto proposed=guest_bytes(output);
                        auto handle=args(0);
                        const bool automatic=!handle;
                        if(automatic)handle=co_await open(vfw::video_type,0,2,args(2),proposed);
                        if(!handle)co_return finish(0);
                        std::uint64_t block=0;
                        bool success=false;
                        if(!(co_await send(handle,decompress_begin,args(2),proposed))){
                            block=co_await api("kernel32.dll","GlobalAlloc",{2,output.size()+destination->image_size()});
                            if(block){
                                const auto locked=co_await api("kernel32.dll","GlobalLock",{block});
                                if(locked){
                                    m.write(GuestAddress{locked},output);
                                    success=0==co_await decompress(handle,word_arg(1),args(2),args(3),
                                                                   locked,locked+output.size());
                                    (void)co_await api("kernel32.dll","GlobalUnlock",{block});
                                }
                            }
                            (void)co_await send(handle,decompress_end,0,0);
                        }
                        if(automatic)(void)co_await close(handle);
                        if(block && !success)(void)co_await api("kernel32.dll","GlobalFree",{block});
                        co_return finish(success?block:0);
                    }
