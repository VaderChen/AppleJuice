        if(symbol=="D3DX10CreateDevice" || symbol=="D3DX10CreateDeviceAndSwapChain"){
            // D3DX10CreateDevice(Adapter, DriverType, Software, Flags, ppDevice)／AndSwapChain(..., pSwapChainDesc, ppSwapChain, ppDevice)：
            // 優先建立 10.1 等級的裝置（回傳 ID3D10Device，可再取得 ID3D10Device1），與 D3D10CreateDevice1 共用裝置與交換鏈建立。
            graphics_handled=true;const bool swap=symbol.ends_with("AndSwapChain");const unsigned device_output=swap?6:4,swap_output=5;
            if(arg(device_output)){output(device_output);write_pointer(arg(device_output),0);}
            if(swap && arg(swap_output)){output(swap_output);write_pointer(arg(swap_output),0);}
            require(!arg(2) && u32(1)==0 && !(u32(3)&~0x23fU));if(arg(0))(void)typed(arg(0),Kind::Adapter);
            auto device=create(Kind::Device,0,true);device->feature_level=0xa100;device->creation_flags=u32(3);
            try{
                if(arg(0)){hold(arg(0));device->adapter=arg(0);}
                auto context=create(Kind::Context,device->root);context->weak_bindings=true;device->context10=context->root;
                release(device->root);
                if(swap && arg(swap_output)){require(arg(4)!=0);write_pointer(arg(swap_output),make_swap(device,0,arg(4),0,false)->root);}
                if(arg(device_output))write_pointer(arg(device_output),device->root);else release(device->root);
            }catch(...){release(device->root);throw;}
        }else if(symbol=="D3DX10GetFeatureLevel1"){
            // (ID3D10Device*, ID3D10Device1**)：引擎的 D3D10 裝置都支援 ID3D10Device1，回傳同一個物件並增加參考。
            graphics_handled=true;output(1);write_pointer(arg(1),0);auto device=typed(arg(0),Kind::Device);hold(device->root);write_pointer(arg(1),device->root);
        }else if(symbol=="D3DX10CheckVersion" || symbol=="D3DX11CheckVersion"){
            // D3DX10：D3D10_SDK_VERSION 29 與 D3DX10_SDK_VERSION 43；D3DX11：D3D11_SDK_VERSION 7 與該 DLL 的 D3DX11_SDK_VERSION（42 或 43）。
            graphics_handled=true;const bool eleven=symbol=="D3DX11CheckVersion";
            result=eleven?(u32(0)==7 && u32(1)==(service.family=="d3dx11_42.dll"?42U:43U)):(u32(0)==29 && u32(1)==43);
        }else if(symbol=="D3D10CreateDevice" || symbol=="D3D10CreateDevice1" || symbol=="D3D10CreateDeviceAndSwapChain" || symbol=="D3D10CreateDeviceAndSwapChain1"){
            graphics_handled=true;const bool extended=symbol.ends_with("1"),swap=symbol.find("AndSwapChain")!=symbol.npos;
            const unsigned sdk=extended?5:4,device_output=sdk+(swap?3:1),swap_output=sdk+2;
            if(arg(device_output)){output(device_output);write_pointer(arg(device_output),0);}
            if(swap && arg(swap_output)){output(swap_output);write_pointer(arg(swap_output),0);}
            const auto level=extended?u32(4):0xa000U;
            require(u32(sdk)==(extended?32U:29U) && !arg(2) && u32(1)==0 && (level==0xa000 || level==0xa100) && !(u32(3)&~0x23fU));
            if(arg(0))(void)typed(arg(0),Kind::Adapter);
            auto device=create(Kind::Device,0,true);device->feature_level=level;device->creation_flags=u32(3);
            try{
                if(arg(0)){hold(arg(0));device->adapter=arg(0);}
                auto context=create(Kind::Context,device->root);context->weak_bindings=true;device->context10=context->root;
                // 私有 context 由 device 擁有；不得形成 context → device 強參照循環。
                release(device->root);
                if(swap && arg(swap_output)){require(arg(sdk+1)!=0);write_pointer(arg(swap_output),make_swap(device,0,arg(sdk+1),0,false)->root);}
                if(arg(device_output))write_pointer(arg(device_output),device->root);else release(device->root);
            }catch(...){release(device->root);throw;}
        }else if(service.family=="internal-d3d10" && (symbol=="SetTextFilterSize" || symbol=="GetTextFilterSize")){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);
            if(symbol=="SetTextFilterSize"){require(u32(1)>0 && u32(2)>0);device->text_filter_size={u32(1),u32(2)};}
            else {if(arg(1))memory.store(GuestAddress{arg(1)},32,device->text_filter_size[0]);if(arg(2))memory.store(GuestAddress{arg(2)},32,device->text_filter_size[1]);}
        }
