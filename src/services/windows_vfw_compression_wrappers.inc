                    if(name=="ICCompress" || name=="ICImageCompress" || name.starts_with("ICSeqCompress") ||
                       name=="ICCompressorFree"){
                        const auto submit=[&](std::uint64_t hic,unsigned flags,
                            const std::array<std::uint64_t,8>& pointers,const std::array<std::uint32_t,3>& numbers)
                            ->Task<std::uint64_t>{
                            avi::Bytes data(vfw::compress_wire_size(unit));
                            avi::put(data,0,flags);
                            for(unsigned n=0;n<6;++n)vfw::put_pointer(data,(n+1)*unit,unit,pointers[n]);
                            for(unsigned n=0;n<numbers.size();++n)avi::put(data,7*unit+n*4,numbers[n]);
                            for(unsigned n=0;n<2;++n)vfw::put_pointer(data,vfw::compress_previous(unit)+n*unit,unit,pointers[6+n]);
                            co_return co_await send(hic,vfw::compress_frame,guest_bytes(data),data.size());
                        };
                        if(name=="ICCompress"){
                            const auto status=co_await submit(args(0),word_arg(1),
                                {args(2),args(3),args(4),args(5),args(6),args(7),args(11),args(12)},
                                {word_arg(8),word_arg(9),word_arg(10)});
                            co_return finish(static_cast<std::uint32_t>(status));
                        }
                        const auto result_header=[&](std::uint64_t hic,std::uint64_t input,std::uint64_t specified)
                            ->Task<avi::Bytes>{
                            if(specified)co_return header_at(specified);
                            const auto capacity=co_await send(hic,vfw::compress_format,input,0);
                            if(capacity<40 || capacity>8192)co_return avi::Bytes{};
                            avi::Bytes output(capacity);
                            const auto address=guest_bytes(output);
                            if(co_await send(hic,vfw::compress_format,input,address))co_return avi::Bytes{};
                            m.read(GuestAddress{address},output);
                            const auto size=avi::frame_header_size(output);
                            if(!size || size>output.size())co_return avi::Bytes{};
                            output.resize(size);
                            co_return output;
                        };
                        const auto image_bound=[&](std::uint64_t hic,std::uint64_t input,std::uint64_t output,
                                                  const avi::Bytes& proposed)->Task<std::size_t>{
                            const auto suggested=co_await send(hic,vfw::compress_size,input,output);
                            if(suggested && suggested<=memory_limit)co_return suggested;
                            const auto estimate=avi::word(proposed,20);
                            co_return estimate<=memory_limit?estimate:0;
                        };
                        if(name=="ICImageCompress"){
                            const auto input=args(2),pixels=args(3),size_at=args(6);
                            const auto hint=static_cast<std::uint32_t>(size_at?m.load(GuestAddress{size_at},32):0);
                            if(size_at)m.check(GuestAddress{size_at},4,Access::Write);
                            if(word_arg(1) || !input || !pixels ||
                               (word_arg(5)>10000 && word_arg(5)!=UINT32_MAX))co_return finish(0);
                            auto hic=args(0);
                            const bool auto_open=!hic;
                            if(auto_open){
                                const auto requested=header_at(args(4));
                                if(args(4) && requested.empty())co_return finish(0);
                                auto format=requested.empty()?0:avi::word(requested,16);
                                if(format==1 || format==2)format=vfw::fourcc('M','R','L','E');
                                else if(format==4)format=vfw::fourcc('M','J','P','G');
                                else if(format==5)format=vfw::fourcc('P','N','G',' ');
                                else if(format==0 || format==3 || format==6)format=vfw::fourcc('D','I','B',' ');
                                hic=co_await open(vfw::video_type,format,1,input,args(4));
                            }
                            if(!hic)co_return finish(0);
                            std::uint64_t block=0,locked=0;
                            bool begun=false;
                            std::uint32_t used=0;
                            const auto compress_one=[&]()->Task<bool>{
                                auto output=co_await result_header(hic,input,args(4));
                                if(output.empty())co_return false;
                                const auto format=guest_bytes(output);
                                const auto capacity=co_await image_bound(hic,input,format,output);
                                if(!capacity)co_return false;
                                if(co_await send(hic,vfw::compress_begin,input,format))co_return false;
                                begun=true;
                                block=co_await api("kernel32.dll","GlobalAlloc",{2,output.size()+capacity});
                                if(!block)co_return false;
                                locked=co_await api("kernel32.dll","GlobalLock",{block});
                                if(!locked)co_return false;
                                m.write(GuestAddress{locked},output);
                                if(co_await submit(hic,1,{locked,locked+output.size(),input,pixels,0,0,0,0},
                                                   {0,hint>INT32_MAX?0:hint,word_arg(5)}))co_return false;
                                used=static_cast<std::uint32_t>(m.load(GuestAddress{locked+20},32));
                                co_return used && used<=capacity;
                            };
                            bool success=false;
                            try{success=co_await compress_one();}
                            catch(const MemoryFault&){}
                            catch(const std::bad_alloc&){}
                            if(locked)(void)co_await api("kernel32.dll","GlobalUnlock",{block});
                            if(begun)(void)co_await send(hic,vfw::compress_end,0,0);
                            if(auto_open)(void)co_await close(hic);
                            if(!success && block)(void)co_await api("kernel32.dll","GlobalFree",{block});
                            if(success && size_at)m.store(GuestAddress{size_at},32,used);
                            co_return finish(success?block:0);
                        }
                        const auto vars=args(0);
                        const vfw::CompVarsAbi abi{unit};
                        if(!vars || m.load(GuestAddress{vars},32)!=abi.size() ||
                           store.compressor_dialogs.contains(vars))co_return finish(0);
                        const auto field=[&](unsigned offset){return static_cast<std::uint32_t>(m.load(GuestAddress{vars+offset},32));};
                        const auto ptr=[&](unsigned offset){return m.load(GuestAddress{vars+offset},ptr_bits);};
                        const auto put=[&](unsigned offset,std::uint64_t value){m.store(GuestAddress{vars+offset},ptr_bits,value);};
                        const auto number=[&](unsigned offset,std::uint32_t value){m.store(GuestAddress{vars+offset},32,value);};
                        auto found=store.compression_sequences.find(vars);
                        if(found!=store.compression_sequences.end() && found->second->busy)co_return finish(0);
                        const auto release_buffers=[&](vfw::CompressionSequence& sequence){
                            if(sequence.input)(void)free_heap(m,std::exchange(sequence.input,0),s);
                            if(sequence.output_storage)(void)free_heap(m,std::exchange(sequence.output_storage,0),s);
                        };
                        if(name=="ICSeqCompressFrameEnd" || name=="ICCompressorFree"){
                            const bool free=name=="ICCompressorFree";
                            m.check(GuestAddress{vars},abi.size(),Access::Write);
                            const auto hic=ptr(8);
                            if(found!=store.compression_sequences.end()){
                                const auto sequence=found->second;
                                sequence->busy=true;
                                (void)co_await send(sequence->hic,vfw::compress_end,0,0);
                                put(abi.input(),0);
                                put(abi.output(),sequence->caller_output);
                                put(abi.bits(),0);
                                put(abi.previous(),0);
                                release_buffers(*sequence);
                                store.compression_sequences.erase(vars);
                            }
                            if(free){
                                if(const auto format=store.compressor_outputs.find(vars);format!=store.compressor_outputs.end()){
                                    if(ptr(abi.output())==format->second)put(abi.output(),0);
                                    (void)free_heap(m,format->second,s);
                                    store.compressor_outputs.erase(format);
                                }
                                put(8,0);
                                put(abi.input(),0);
                                put(abi.bits(),0);
                                put(abi.previous(),0);
                                put(abi.state(),0);
                                number(abi.state()+unit,0);
                                number(4,0);
                                if(hic)(void)co_await close(hic);
                            }
                            co_return finish(0);
                        }
                        if(name=="ICSeqCompressFrameStart"){
                            if(found!=store.compression_sequences.end() || !(field(4)&1) || !args(1))co_return finish(0);
                            const auto hic=ptr(8);
                            const auto input=header_at(args(1));
                            const auto layout=avi::frame_layout(input);
                            if(!hic || !layout || !layout->raw())co_return finish(0);
                            if(std::any_of(store.compression_sequences.begin(),store.compression_sequences.end(),
                                [&](const auto& entry){return entry.second->hic==hic;}))co_return finish(0);
                            m.check(GuestAddress{vars},abi.size(),Access::Write);
                            const auto entry=std::make_shared<vfw::CompressionSequence>();
                            auto& sequence=*entry;
                            sequence.hic=hic;
                            sequence.input_size=layout->image_size();
                            sequence.caller_output=ptr(abi.output());
                            sequence.quality=field(abi.quality());
                            sequence.busy=true;
                            store.compression_sequences.emplace(vars,entry);
                            const auto configure=[&]()->Task<bool>{
                                auto output=co_await result_header(hic,args(1),sequence.caller_output);
                                if(output.empty())co_return false;
                                const auto proposed=guest_bytes(output);
                                sequence.output_size=co_await image_bound(hic,args(1),proposed,output);
                                if(!sequence.output_size)co_return false;
                                sequence.input=allocate_heap(m,input.size()+sequence.input_size);
                                if(!sequence.input)co_return false;
                                const auto header_size=sequence.caller_output?0:output.size();
                                sequence.output_storage=allocate_heap(m,header_size+sequence.output_size);
                                if(!sequence.output_storage)co_return false;
                                sequence.output=sequence.caller_output?sequence.caller_output:sequence.output_storage;
                                sequence.previous=sequence.input+input.size();
                                sequence.bits=sequence.output_storage+header_size;
                                m.write(GuestAddress{sequence.input},input);
                                if(!sequence.caller_output)m.write(GuestAddress{sequence.output},output);
                                const auto status=co_await send(hic,vfw::compress_begin,sequence.input,sequence.output);
                                co_return status==0;
                            };
                            bool ready=false;
                            try{ready=co_await configure();}
                            catch(const MemoryFault&){}
                            catch(const std::bad_alloc&){}
                            if(!ready){
                                release_buffers(sequence);
                                store.compression_sequences.erase(vars);
                                co_return finish(0);
                            }
                            put(abi.input(),sequence.input);
                            put(abi.output(),sequence.output);
                            put(abi.bits(),sequence.bits);
                            put(abi.previous(),sequence.previous);
                            number(abi.frame(),0);
                            number(abi.key_count(),0);
                            sequence.busy=false;
                            co_return finish(1);
                        }
                        if(found==store.compression_sequences.end() || word_arg(1) || !args(2))co_return finish(0);
                        const auto entry=found->second;
                        auto& sequence=*entry;
                        if(sequence.frame==INT32_MAX)co_return finish(0);
                        const auto size=args(4),key=args(3),current=args(2);
                        const auto target=static_cast<std::uint32_t>(size?m.load(GuestAddress{size},32):0);
                        if(size)m.check(GuestAddress{size},4,Access::Write);
                        if(key)m.check(GuestAddress{key},4,Access::Write);
                        m.check(GuestAddress{current},sequence.input_size,Access::Read);
                        m.check(GuestAddress{vars},abi.size(),Access::Write);
                        const auto interval=static_cast<std::int32_t>(field(abi.key()));
                        const bool want_key=!sequence.frame || (interval>0 && sequence.frame-sequence.last_key>=static_cast<unsigned>(interval));
                        const auto outflags=guest_bytes(avi::Bytes(4));
                        sequence.busy=true;
                        struct ClearSequence {bool& busy;~ClearSequence(){busy=false;}} clear{sequence.busy};
                        const auto status=co_await submit(sequence.hic,want_key?1U:0U,
                            {sequence.output,sequence.bits,sequence.input,current,0,outflags,
                             sequence.has_previous?sequence.input:0,sequence.has_previous?sequence.previous:0},
                            {sequence.frame,target>INT32_MAX?0:target,sequence.quality});
                        if(status)co_return finish(0);
                        const auto actual=m.load(GuestAddress{sequence.output+20},32);
                        if(!actual || actual>sequence.output_size)co_return finish(0);
                        const bool keyframe=(m.load(GuestAddress{outflags},32)&0x10)!=0;
                        avi::Bytes previous(sequence.input_size);
                        m.read(GuestAddress{current},previous);
                        m.write(GuestAddress{sequence.previous},previous);
                        sequence.has_previous=true;
                        if(keyframe)sequence.last_key=sequence.frame;
                        ++sequence.frame;
                        number(abi.frame(),sequence.frame);
                        number(abi.key_count(),sequence.frame-sequence.last_key);
                        if(size)m.store(GuestAddress{size},32,actual);
                        if(key)m.store(GuestAddress{key},32,keyframe);
                        co_return finish(sequence.bits);
                    }
