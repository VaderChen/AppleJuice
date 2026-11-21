        if(symbol=="DXGID3D10RegisterLayers"){
            graphics_handled=true;
            const auto count=u32(1);
            const auto stride=4U*pointer_size;
            if(count){
                require(arg(0) && arg(0)<=UINT64_MAX-std::uint64_t{count}*stride);
                memory.check(GuestAddress{arg(0)},static_cast<std::size_t>(count)*stride,Access::Read);
                auto layers=bridge.device_layers;
                for(std::uint32_t i=0;i<count;++i){
                    const auto address=arg(0)+std::uint64_t{i}*stride;
                    const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{address},32));
                    const auto callback=[&](unsigned slot){
                        const auto target=memory.load(GuestAddress{address+slot*pointer_size},pointer_bits);
                        require(target!=0);
                        memory.check(GuestAddress{target},1,Access::Execute);
                        return target;
                    };
                    // 多個模組可以重複登記同一層；保留第一個供後續裝置查詢。
                    layers.try_emplace(id,DxgiDeviceLayer{callback(1),callback(2),callback(3)});
                }
                bridge.device_layers.swap(layers);
                if(const auto primary=bridge.device_layers.find(dxgi_primary_layer);
                   primary!=bridge.device_layers.end() && primary->second.create)
                    bridge.guest_device_layer.store(true,std::memory_order_relaxed);
                for(const auto& [id,layer]:bridge.device_layers){
                    (void)id;
                    for(const auto target:{layer.initialize,layer.size,layer.create})
                        if(const auto* image=module_image(target,true))
                            if(auto module=runtime_.loaded_modules.find(image->base);module!=runtime_.loaded_modules.end())
                                module->second.pinned=true;
                }
            }
        }else if(symbol=="DxgiAttachLayer"){
            graphics_handled=true;
            auto device=typed(arg(0),Kind::DxgiDevice);
            require(!device->dxgi_layer && arg(1));
            device->dxgi_layer=arg(1);
            device->dxgi_layer_storage=arg(2);
        }else if(symbol=="DxgiFinishLayerRelease"){
            graphics_handled=true;
            release_object(release_object,arg(0),u32(1)!=0);
        }else if(symbol=="DxgiCoreDevice"){
            graphics_handled=true;
            // 將原有的裝置引用交給同一 COM 物件的 API 根介面。
            result=typed(arg(0),Kind::DxgiDevice)->root;
        }else if(symbol=="D3D10RegisterLayers" || symbol=="D3D10CoreRegisterLayers" || symbol=="D3D11CoreRegisterLayers"){
            graphics_handled=true;
            // 以共用表登記裝置底層。內建層由原生 Metal 建立器提供；外部層保有各自的客體回呼。
            bridge.device_layers.try_emplace(dxgi_primary_layer,DxgiDeviceLayer{});
        }else if(symbol=="D3D10CoreCreateDevice" || symbol=="D3D11CoreCreateDevice" ||
                 symbol=="DXGID3D10CreateDevice" || symbol=="DxgiCreateLayerBase"){
            graphics_handled=true;
            const bool dxgi=symbol=="DXGID3D10CreateDevice";
            const bool base=symbol=="DxgiCreateLayerBase";
            const unsigned shift=dxgi?1:0;
            const auto module=dxgi?arg(0):0;
            const bool api10=symbol=="D3D10CoreCreateDevice" || (base && u32(6)) || (dxgi && module &&
                (module==find_module(u"d3d10core.dll") || module==find_module(u"d3d10.dll") || module==find_module(u"d3d10_1.dll")));
            const bool single=symbol=="D3D10CoreCreateDevice";
            const unsigned out=single?4:5+shift;
            output(out);write_pointer(arg(out),0);
            require(arg(shift)!=0);
            const auto flags=u32(2+shift);
            require(!(flags&~(api10?0x23fU:0x8e3U)));
            if(dxgi)require(module && runtime_.builtin_images.contains(module) && (api10 || module==find_module(u"d3d11.dll")));
            const auto level=single?u32(3):feature_level(arg(3+shift),u32(4+shift),api10);
            if(!level || (single && level!=0xa000 && level!=0xa100))result=not_supported;
            else{
                bridge.device_layers.try_emplace(dxgi_primary_layer,DxgiDeviceLayer{});
                auto device=make_hardware_device(api10,flags,level,arg(1+shift),arg(shift));
                try{write_pointer(arg(out),dxgi || base?interface(device,Kind::DxgiDevice):device->root);}
                catch(...){release(device->root);throw;}
            }
        }
