        bool imm_memory_handled=false;
        const auto make_imm_buffer=[&](std::uint32_t size,std::uint64_t owner=0,unsigned kind=0){
            if(size>64U*1024U*1024U || gui_.input_buffers.size()>=4096)throw std::length_error("IME 記憶體超過上限");
            GuiImmBuffer buffer;buffer.storage=allocate_heap(memory,std::max(size,1U));if(!buffer.storage)throw std::bad_alloc{};
            buffer.size=size;buffer.context=owner;buffer.kind=kind;memory.write(GuestAddress{buffer.storage},std::vector<std::uint8_t>(size));
            const auto handle=gui_.next_object++;try{gui_.input_buffers.emplace(handle,std::move(buffer));}catch(...){free_heap(memory,buffer.storage,state);throw;}return handle;
        };
        const auto resize_imm_buffer=[&](GuiImmBuffer& buffer,std::uint32_t size){
            if(size>64U*1024U*1024U)throw std::length_error("IME 記憶體超過上限");
            if(size==buffer.size)return;
            if(buffer.locks)throw std::domain_error("IME 記憶體使用中");
            const auto storage=allocate_heap(memory,std::max(size,1U));if(!storage)throw std::bad_alloc{};
            std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{buffer.storage},std::span(bytes).first(std::min(size,buffer.size)));memory.write(GuestAddress{storage},bytes);
            free_heap(memory,buffer.storage,state);buffer.storage=storage;buffer.size=size;buffer.snapshot.clear();
        };
        if(symbol=="ImmCreateIMCC"){imm_memory_handled=true;result=make_imm_buffer(d(0));}
        else if(symbol=="ImmDestroyIMCC" || symbol=="ImmLockIMCC" || symbol=="ImmUnlockIMCC" || symbol=="ImmReSizeIMCC" || symbol=="ImmGetIMCCSize" || symbol=="ImmGetIMCCLockCount"){
            imm_memory_handled=true;const auto found=gui_.input_buffers.find(arg(0));
            if(found==gui_.input_buffers.end()){fail(6);if(symbol=="ImmDestroyIMCC")result=arg(0);}
            else {auto& buffer=found->second;
                if(symbol=="ImmGetIMCCSize")result=buffer.size;
                else if(symbol=="ImmGetIMCCLockCount")result=buffer.locks;
                else if(symbol=="ImmLockIMCC"){
                    if(buffer.locks==UINT32_MAX)fail(8);else {++buffer.locks;result=buffer.storage;}
                }else if(symbol=="ImmUnlockIMCC"){
                    if(!buffer.locks)fail(158);else {
                        if(buffer.locks==1 && buffer.context && gui_.input_contexts.contains(buffer.context))imm_commit_buffer(memory,buffer,gui_.input_contexts.at(buffer.context).data);
                        result=--buffer.locks!=0;last_error_=0;
                    }
                }else if(symbol=="ImmReSizeIMCC"){resize_imm_buffer(buffer,d(1));result=arg(0);}
                else {
                    if(buffer.context && gui_.input_contexts.contains(buffer.context)){
                        auto& context=gui_.input_contexts.at(buffer.context);for(auto& handle:context.buffers)if(handle==arg(0))handle=0;
                    }
                    free_heap(memory,buffer.storage,state);gui_.input_buffers.erase(found);last_error_=0;
                }
            }
        }else if(symbol=="ImmLockIMC" || symbol=="ImmUnlockIMC" || symbol=="ImmGetIMCLockCount"){
            imm_memory_handled=true;const auto found=gui_.input_contexts.find(arg(0));
            if(found==gui_.input_contexts.end() || found->second.thread!=threads_.current)fail(6);
            else {auto& context=found->second;const unsigned unit=x86_?4:8,base=unit,components=x86_?280:288;
                if(symbol=="ImmGetIMCLockCount")result=context.locks;
                else if(symbol=="ImmLockIMC"){
                    if(context.locks==UINT32_MAX)fail(8);
                    else {
                        if(!context.storage){context.storage=allocate_heap(memory,x86_?320:352);if(!context.storage)throw std::bad_alloc{};memory.write(GuestAddress{context.storage},std::vector<std::uint8_t>(x86_?320:352));}
                        if(!context.locks){
                            const auto publish=[&](unsigned kind,const std::vector<std::uint8_t>& bytes){
                                auto& handle=context.buffers[kind];if(!handle)handle=make_imm_buffer(static_cast<std::uint32_t>(bytes.size()),arg(0),kind);
                                auto& buffer=gui_.input_buffers.at(handle);if(buffer.locks)return;
                                resize_imm_buffer(buffer,static_cast<std::uint32_t>(bytes.size()));memory.write(GuestAddress{buffer.storage},bytes);buffer.snapshot=bytes;
                            };
                            publish(0,imm_composition_bytes(context.data));publish(1,imm_candidate_info(context.data));std::vector<std::uint8_t> guide(28);guide[0]=28;publish(2,guide);
                            std::uint64_t hwnd=0;if(gui_.windows.contains(gui_.focus) && imm_window_context(gui_.focus)==arg(0))hwnd=gui_.focus;
                            if(!hwnd)for(const auto& [handle,window]:gui_.windows)if(window.thread==context.thread && gui_control_class(window)!=u"ime" && imm_window_context(handle)==arg(0)){hwnd=handle;break;}
                            memory.store(GuestAddress{context.storage},static_cast<std::uint8_t>(unit*8),hwnd);
                            memory.store(GuestAddress{context.storage+base},32,context.open);memory.store(GuestAddress{context.storage+base+20},32,context.conversion);memory.store(GuestAddress{context.storage+base+24},32,context.sentence);
                            memory.write(GuestAddress{context.storage+base+28},context.font);memory.write(GuestAddress{context.storage+base+120},context.composition);
                            for(unsigned i=0;i<4;++i){std::array<std::uint8_t,32> bytes{};if(const auto candidate=context.candidates.find(i);candidate!=context.candidates.end())bytes=candidate->second;else bytes[0]=static_cast<std::uint8_t>(i);memory.write(GuestAddress{context.storage+base+148+i*32},bytes);}
                            for(unsigned i=0;i<4;++i)memory.store(GuestAddress{context.storage+components+i*unit},static_cast<std::uint8_t>(unit*8),context.buffers[i]);
                            memory.store(GuestAddress{context.storage+components+4*unit},32,0);memory.store(GuestAddress{context.storage+(x86_?300U:328U)},static_cast<std::uint8_t>(unit*8),context.buffers[4]);
                            context.snapshot.resize(x86_?320:352);memory.read(GuestAddress{context.storage},context.snapshot);
                        }
                        ++context.locks;result=context.storage;
                    }
                }else {
                    if(context.locks==1){
                        // INPUTCONTEXT 的公開欄位與既有 IMM／WM_IME_CONTROL 共用同一份狀態。
                        std::vector<std::uint8_t> changed(context.snapshot.size());memory.read(GuestAddress{context.storage},changed);
                        const auto written=[&](unsigned offset,unsigned size){return !std::equal(changed.begin()+offset,changed.begin()+offset+size,context.snapshot.begin()+offset);};
                        if(written(base,4))context.open=memory.load(GuestAddress{context.storage+base},32)!=0;
                        if(written(base+20,4))context.conversion=static_cast<std::uint32_t>(memory.load(GuestAddress{context.storage+base+20},32));
                        if(written(base+24,4))context.sentence=static_cast<std::uint32_t>(memory.load(GuestAddress{context.storage+base+24},32));
                        if(written(base+28,92))memory.read(GuestAddress{context.storage+base+28},context.font);
                        if(written(base+120,28))memory.read(GuestAddress{context.storage+base+120},context.composition);
                        for(unsigned i=0;i<4;++i)if(written(base+148+i*32,32))memory.read(GuestAddress{context.storage+base+148+i*32},context.candidates[i]);
                    }
                    if(context.locks)--context.locks;result=1;
                }
            }
        }
