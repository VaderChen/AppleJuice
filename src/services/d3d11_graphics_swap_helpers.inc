        const auto swap_buffers=[&](const std::shared_ptr<Identity>& swap,std::uint32_t width,std::uint32_t height,std::uint32_t format,std::uint32_t count,std::uint32_t usage,std::uint32_t samples){
            require(width && height && width<=16384 && height<=16384 && (format==24 || format==87 || format==28 || format==29 || format==91) && count>=1 && count<=4);
            require(swap->native->graphics_samples_supported(format,samples) && (samples==1 || !(usage&0x400)));
            std::vector<std::uint64_t> buffers,resolved;const auto device=object(swap->device).identity;
            // GetBuffer 傳回真正的 MSAA 資源；呈現前才解析成單一 sample。
            const auto make_buffer=[&](unsigned sample_count){GraphicsResourceDesc d;d.dimension=3;d.width=width;d.height=height;d.format=format;d.samples=sample_count;d.bind=0x28|((usage&0x400)?0x80U:0U);auto native=swap->native->graphics_resource(d);auto texture=create(Kind::Texture,device->root);texture->resource=std::move(native);texture->texture_desc={width,height,1,1,format,sample_count,0,0,d.bind,0,0};texture->desc.resize(44);for(unsigned j=0;j<11;++j)put_word(texture->desc,4*j,texture->texture_desc[j]);return texture->root;};
            try{for(unsigned i=0;i<count;++i)buffers.push_back(make_buffer(samples));if(samples>1)resolved.push_back(make_buffer(1));if(gui_.windows.contains(swap->window))swap->native->drawable(native_gui_?native_gui_->graphics_view(swap->window):nullptr,width,height);}
            catch(...){for(auto pointer:buffers)release(pointer);for(auto pointer:resolved)release(pointer);throw;}
            auto previous=std::move(swap->bindings["Backbuffers"]);swap->bindings["Backbuffers"]=std::move(buffers);for(auto pointer:previous)release(pointer);
            previous=std::move(swap->bindings["PresentResolve"]);swap->bindings["PresentResolve"]=std::move(resolved);for(auto pointer:previous)release(pointer);
            swap->present_frame.reset();swap->present_spare.reset();
        };
        const auto make_swap=[&](const std::shared_ptr<Identity>& device,std::uint64_t factory,std::uint64_t address,std::uint64_t window,bool desc1,std::uint64_t fullscreen_desc=0,std::uint64_t restricted=0){
            auto desc=read_bytes(memory,address,desc1?48:x86_?60:72);std::uint32_t width=word(desc,0),height=word(desc,4),format=word(desc,desc1?8:16),count=word(desc,desc1?28:40);
            if(!desc1)window=memory.load(GuestAddress{address+(x86_?44:48)},pointer_bits);const auto local=gui_.windows.find(window);const auto remote=local==gui_.windows.end()?shared_gui_window(window):std::nullopt;require(local!=gui_.windows.end() || remote.has_value());if(!width)width=remote?remote->width:local->second.width;if(!height)height=remote?remote->height:local->second.height;if(!format)format=87;
            const auto samples=word(desc,desc1?16:28),effect=word(desc,desc1?36:x86_?52:60);
            require(samples && (samples==1 || effect==0) && !word(desc,desc1?20:32) && (word(desc,desc1?24:36)&0x20) && (!desc1 || !word(desc,12)));
            if(restricted)(void)typed(restricted,Kind::Output);
            const auto fs=fullscreen_desc?read_bytes(memory,fullscreen_desc,20):std::vector<std::uint8_t>{};
            const bool fullscreen=desc1?(!fs.empty() && !word(fs,16)):!word(desc,x86_?48:56);
            if(!factory)factory=object(device_adapter(device)).identity->device;
            auto swap=create(Kind::SwapChain,device->root);swap->window=window;swap->factory=factory;if(factory)hold(factory);
            // device 的繪製 queue 共用；HWND／尺寸／drawable 則由各交換鏈獨立持有。
            try{swap->native=make_metal_opengl(nullptr,1,1);swap_buffers(swap,width,height,format,count,word(desc,desc1?24:36),samples);swap->desc.resize(x86_?60:72);if(!desc1)std::copy_n(desc.begin(),28,swap->desc.begin());else if(!fs.empty()){std::copy_n(fs.begin(),8,swap->desc.begin()+8);put_word(swap->desc,20,word(fs,8));put_word(swap->desc,24,word(fs,12));}put_word(swap->desc,0,width);put_word(swap->desc,4,height);put_word(swap->desc,16,format);put_word(swap->desc,28,samples);put_word(swap->desc,36,word(desc,desc1?24:36));put_word(swap->desc,40,count);std::memcpy(swap->desc.data()+(x86_?44:48),&window,pointer_size);put_word(swap->desc,x86_?48:56,1);put_word(swap->desc,x86_?52:60,word(desc,desc1?36:x86_?52:60));put_word(swap->desc,x86_?56:64,word(desc,desc1?44:x86_?56:64));
                swap->target_mode.assign(swap->desc.begin(),swap->desc.begin()+28);if(restricted){hold(restricted);swap->bindings["RestrictOutput"]={restricted};}
                if(fullscreen){const auto status=set_fullscreen(swap,true,restricted);if(status)throw WindowPresentationFault("建立全螢幕交換鏈失敗");}
                if(std::getenv("X64ARM_TRACE_DXGI_STATE"))std::fprintf(stderr,"[dxgi] create swap=%llx window=%llx size=%ux%u fullscreen=%u buffer=%p\n",static_cast<unsigned long long>(swap->root),static_cast<unsigned long long>(window),width,height,swap->fullscreen,object(swap->bindings.at("Backbuffers")[0]).identity->resource.get());
            }
            catch(...){release(swap->root);throw;}return swap;
        };
