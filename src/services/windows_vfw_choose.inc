                    if(name=="ICCompressorChoose"){
                        const auto vars=args(4),input=args(2);
                        const auto options=word_arg(1);
                        const vfw::CompVarsAbi abi{unit};
                        if(!vars || options&~15U || m.load(GuestAddress{vars},32)!=abi.size() ||
                           store.compression_sequences.contains(vars) || store.compressor_dialogs.contains(vars))
                            co_return finish(0);
                        m.check(GuestAddress{vars},abi.size(),Access::Write);
                        const auto u32=[&](unsigned offset){return static_cast<std::uint32_t>(m.load(GuestAddress{vars+offset},32));};
                        const bool defaults=(u32(4)&1)!=0;
                        const auto wanted_type=defaults?u32(8+unit):0;
                        if(wanted_type && !vfw::matches(wanted_type,vfw::video_type))co_return finish(0);
                        const auto wanted_codec=defaults?u32(12+unit):vfw::fourcc('D','I','B',' ');
                        const auto input_format=header_at(input);
                        if(input && input_format.empty())co_return finish(0);
                        const auto prior=defaults?m.load(GuestAddress{vars+8},ptr_bits):0;
                        const auto title=args(5)?utf8_to_utf16_lossy(read_ansi(m,args(5),1024)):u"視訊壓縮";
                        const auto dialog=std::make_shared<vfw::CompressorChoice>();
                        dialog->stream=(options&4)?args(3):0;
                        dialog->quality=defaults?u32(abi.quality()):UINT32_MAX;
                        dialog->key=defaults?std::min<std::uint32_t>(u32(abi.key()),INT32_MAX):0;
                        dialog->rate=defaults?std::min<std::uint32_t>(u32(abi.rate()),INT32_MAX):0;
                        store.compressor_dialogs.emplace(vars,dialog);
                        struct RemoveDialog {
                            decltype(store.compressor_dialogs)& dialogs;
                            std::uint64_t vars;
                            ~RemoveDialog(){dialogs.erase(vars);}
                        } remove{store.compressor_dialogs,vars};
                        std::uint64_t chosen=0,owned_format=0;
                        unsigned chosen_handler=0;
                        avi::Bytes data(abi.size());
                        const auto choose=[&]()->Task<bool>{
                            const auto drivers=catalog();
                            dialog->choices.reserve(drivers.size());
                            for(const auto& driver:drivers){
                                if(!vfw::matches(driver.type,vfw::video_type))continue;
                                auto label=driver.description.empty()?driver.file:driver.description;
                                const auto code=vfw::fcc_text(driver.handler);
                                label=label.empty()?code:label+u" ("+code+u")";
                                const auto hic=co_await allocation(driver,1);
                                if(!hic)continue;
                                auto& choice=dialog->choices.emplace_back(vfw::CompressorChoice::Entry{hic,driver.handler,std::move(label)});
                                if(input && !(options&8) && (co_await send(choice.hic,vfw::compress_query,input,0))){
                                    (void)co_await close(choice.hic);
                                    dialog->choices.pop_back();
                                    continue;
                                }
                                if(vfw::matches(driver.handler,wanted_codec))
                                    dialog->selected=static_cast<unsigned>(dialog->choices.size()-1);
                                if(dialog->choices.size()==4096)break;
                            }
                            if(dialog->choices.empty())co_return false;
                            const auto decimal=[](unsigned value){return utf8_to_utf16(std::to_string(value));};
                            DialogTemplate items;
                            items.item(0,8,8,54,12,-1,0x82,u"",u"編碼器");
                            items.item(0x10203,65,7,177,92,vfw::choice_codec,0x85,u"",u"");
                            items.item(0,8,27,54,12,-1,0x82,u"",u"品質");
                            items.item(0x810080,65,25,52,14,vfw::choice_quality,0x81,u"",
                                dialog->quality<=10000?decimal(dialog->quality):u"");
                            items.item(0x10000,126,24,53,16,vfw::choice_configure,0x80,u"",u"設定");
                            items.item(0x10000,186,24,56,16,vfw::choice_about,0x80,u"",u"關於");
                            int y=46;
                            const auto numeric=[&](int check,int edit,std::u16string label,unsigned value){
                                items.item(0x10003,8,y,137,14,check,0x80,u"",label);
                                items.item(0x810080,149,y,93,14,edit,0x81,u"",value?decimal(value):u"");
                                y+=20;
                            };
                            if(options&1)numeric(vfw::choice_key_enable,vfw::choice_key,u"關鍵影格間隔",dialog->key);
                            if(options&2)numeric(vfw::choice_rate_enable,vfw::choice_rate,u"資料率（KB／秒）",dialog->rate);
                            if(options&4)items.item(0x10000,8,y,77,16,vfw::choice_preview,0x80,u"",u"預覽");
                            items.item(0x10001,108,y,62,16,1,0x80,u"",u"確定");
                            items.item(0x10000,180,y,62,16,2,0x80,u"",u"取消");
                            if(options&4)items.item(0x800000,8,y+22,234,112,vfw::choice_view,0x82,u"",u"");
                            DialogTemplate window;
                            window.dword(0x90c80040);window.dword(0);window.word(items.count);
                            window.word(10);window.word(10);window.word(250);window.word(static_cast<unsigned>(y+23));
                            window.word(0);window.word(0);window.text(title);window.word(9);window.text(u"Segoe UI");window.align();
                            window.bytes.insert(window.bytes.end(),items.bytes.begin(),items.bytes.end());
                            const auto source=guest_bytes(window.bytes);
                            const auto status=co_await api("user32.dll","DialogBoxIndirectParamW",
                                {0,source,args(0),resolve("internal-avi-vfw-choice","DialogProc").value,vars});
                            if(status!=1 || dialog->selected>=dialog->choices.size())co_return false;
                            const auto& choice=dialog->choices[dialog->selected];
                            if(input && !(co_await send(choice.hic,vfw::compress_query,input,0))){
                                const auto size=co_await send(choice.hic,vfw::compress_format,input,0);
                                if(size>=40 && size<=8192){
                                    const auto proposed=guest_bytes(avi::Bytes(size));
                                    if(!(co_await send(choice.hic,vfw::compress_format,input,proposed))){
                                        avi::Bytes data(size);
                                        m.read(GuestAddress{proposed},data);
                                        owned_format=allocate_heap(m,data.size());
                                        if(!owned_format)co_return false;
                                        m.write(GuestAddress{owned_format},data);
                                    }
                                }
                            }
                            chosen=choice.hic;
                            chosen_handler=choice.handler;
                            co_return true;
                        };
                        bool confirmed=false;
                        try{
                            confirmed=co_await choose();
                            if(confirmed){
                                m.check(GuestAddress{vars},abi.size(),Access::Write);
                                if(owned_format)store.compressor_outputs.try_emplace(vars,0);
                            }
                        }catch(const MemoryFault&){confirmed=false;}
                        catch(const std::bad_alloc&){confirmed=false;}
                        for(const auto& item:dialog->choices)
                            if(!confirmed || item.hic!=chosen)(void)co_await close(item.hic);
                        if(!confirmed){
                            if(owned_format)(void)free_heap(m,owned_format,s);
                            co_return finish(0);
                        }
                        avi::put(data,0,abi.size());
                        avi::put(data,4,1);
                        vfw::put_pointer(data,8,unit,chosen);
                        avi::put(data,8+unit,vfw::video_type);
                        avi::put(data,12+unit,chosen_handler);
                        vfw::put_pointer(data,abi.output(),unit,owned_format);
                        avi::put(data,abi.quality(),dialog->quality);
                        avi::put(data,abi.key(),dialog->key);
                        avi::put(data,abi.rate(),dialog->rate);
                        m.write(GuestAddress{vars},data);
                        if(const auto ownership=store.compressor_outputs.find(vars);
                           ownership!=store.compressor_outputs.end()){
                            if(ownership->second)(void)free_heap(m,ownership->second,s);
                            if(owned_format)ownership->second=owned_format;
                            else store.compressor_outputs.erase(ownership);
                        }
                        if(prior && prior!=chosen)(void)co_await close(prior);
                        co_return finish(1);
                    }
