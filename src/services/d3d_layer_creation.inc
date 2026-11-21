    const bool dxgi_layer_entry=symbol=="DXGID3D10CreateDevice";
    const bool d3d10_layer_entry=symbol=="D3D10CoreCreateDevice";
    if(dxgi_layer_entry || ((d3d10_layer_entry || symbol=="D3D11CoreCreateDevice") && dxgi_guest_layers())){
        const WindowsCall call(state,memory,service);
        const auto module=dxgi_layer_entry?call(0):0;
        const bool builtin=!dxgi_layer_entry || runtime_.builtin_images.contains(module);
        const bool api10=d3d10_layer_entry || (dxgi_layer_entry && builtin &&
            (module==find_module(u"d3d10core.dll") || module==find_module(u"d3d10.dll") || module==find_module(u"d3d10_1.dll")));
        const auto registered=[this]{
            std::unique_lock<GraphicsMutex> lock(graphics_mutex(),std::defer_lock);lock_graphics(lock);
            if(d3d11_){
                const auto layer=d3d11_->device_layers.find(dxgi_primary_layer);
                if(layer!=d3d11_->device_layers.end())return layer->second;
            }
            return DxgiDeviceLayer{};
        };
        auto initial=registered();
        if((!dxgi_layer_entry || module) && (!builtin || initial.create))
            return nested_continuation(state,memory,"DXGI 裝置層",[this,service,builtin,api10,module,dxgi_layer_entry,d3d10_layer_entry,initial,registered]
                (GuestState& guest,GuestMemory& data)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall args(guest,data,service);
                const unsigned shift=dxgi_layer_entry?1:0;
                const auto output=args(d3d10_layer_entry?4:5+shift);
                const auto factory=args(shift),adapter=args(shift+1),flags=args(shift+2),levels=args(shift+3);
                const auto level_count=d3d10_layer_entry?1:args(shift+4);
                const std::uint8_t bits=x86_?32:64;
                const unsigned unit=bits/8;
                const auto finish=[&](std::uint32_t status){args.return_to(guest,status);return std::optional<std::uint32_t>{};};
                std::uint64_t device=0,storage=0,inner=0,inner_release=0;
                std::uint32_t status=0;
                struct Temporary {
                    WindowsServices& self;GuestMemory& data;GuestState& guest;std::uint64_t pointer{};
                    ~Temporary(){if(pointer)(void)self.free_heap(data,pointer,guest);}
                } scratch{*this,data,guest};
                try {
                    if(!output || !factory || !levels || !level_count)co_return finish(invalid);
                    data.check(GuestAddress{output},unit,Access::Write);
                    data.store(GuestAddress{output},bits,0);
                    if(!builtin && module!=runtime_.image.base && !runtime_.loaded_modules.contains(module))co_return finish(invalid);
                    auto layer=initial;
                    if(!builtin && !layer.create){
                        // 自帶的 D3D 核心可在它的 RegisterLayers 內登記客體回呼。
                        std::vector<std::uint64_t> loaded;
                        const auto registration=module_export(data,module,"D3D11CoreRegisterLayers",{},loaded);
                        const auto hr=static_cast<std::uint32_t>(co_await service_callback::call(registration,{}));
                        if(hr&0x80000000U)co_return finish(hr);
                        layer=registered();
                    }
                    if(!layer.initialize || !layer.size || !layer.create)co_return finish(failure);
                    for(auto endpoint:{layer.initialize,layer.size,layer.create})data.check(GuestAddress{endpoint},1,Access::Execute);
                    scratch.pointer=allocate_heap(data,96);
                    if(!scratch.pointer)co_return finish(out_of_memory);
                    const auto work=scratch.pointer;
                    const auto level_at=work+44,count=work+48,base=work+56,iid=work+64,object_at=work+80,device_at=work+88;
                    const std::array<std::uint8_t,96> zero{};
                    data.write(GuestAddress{work},zero);
                    const auto initialized=static_cast<std::uint32_t>(co_await service_callback::call(layer.initialize,{dxgi_primary_layer,count,0}));
                    if(initialized&0x80000000U)co_return finish(initialized);
                    data.store(GuestAddress{work+8+2*unit},bits,adapter);
                    const auto version=work+8+3*unit;
                    data.store(GuestAddress{version},16,10);data.store(GuestAddress{version+2},16,1);
                    data.store(GuestAddress{version+4},16,4);data.store(GuestAddress{version+6},16,6000);
                    const auto size=static_cast<std::uint32_t>(co_await service_callback::call(layer.size,{dxgi_primary_layer,work,0}));
                    if(!size)co_return finish(failure);
                    storage=allocate_heap(data,size);
                    if(!storage)co_return finish(out_of_memory);
                    data.fill(GuestAddress{storage},0,size);
                    if(d3d10_layer_entry)data.store(GuestAddress{level_at},32,levels);
                    status=static_cast<std::uint32_t>(nested_service(guest,data,"internal-dxgi-layers","DxgiCreateLayerBase",
                        {factory,adapter,flags,d3d10_layer_entry?level_at:levels,level_count,device_at,api10?1U:0U}));
                    if(!(status&0x80000000U)){
                        device=data.load(GuestAddress{device_at},bits);
                        data.store(GuestAddress{base},bits,storage);
                        constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                        data.write(GuestAddress{iid},unknown);
                        status=static_cast<std::uint32_t>(co_await service_callback::call(layer.create,{dxgi_primary_layer,base,0,device,iid,object_at}));
                        if(!(status&0x80000000U)){
                            inner=data.load(GuestAddress{object_at},bits);
                            if(!inner)throw std::invalid_argument("DXGI 裝置層沒有回傳 COM 物件");
                            const auto table=data.load(GuestAddress{inner},bits);
                            inner_release=data.load(GuestAddress{table+2*unit},bits);
                            data.check(GuestAddress{inner_release},1,Access::Execute);
                            status=static_cast<std::uint32_t>(nested_service(guest,data,"internal-dxgi-layers","DxgiAttachLayer",{device,inner,storage}));
                            if(!(status&0x80000000U)){
                                storage=inner_release=0; // 由共用裝置 COM 身分釋放。
                                const auto result=dxgi_layer_entry?device:nested_service(guest,data,"internal-dxgi-layers","DxgiCoreDevice",{device});
                                data.store(GuestAddress{output},bits,result);
                                co_return finish(0);
                            }
                        }
                    }
                }catch(const MemoryFault&){status=invalid;}
                 catch(const std::bad_alloc&){status=out_of_memory;}
                 catch(const std::exception&){status=failure;}
                if(inner_release)(void)co_await service_callback::call(inner_release,{inner});
                if(device)(void)co_await service_callback::call(resolve("internal-d3d11","Release").value,{device});
                if(storage)(void)free_heap(data,storage,guest);
                co_return finish(status);
            });
    }
