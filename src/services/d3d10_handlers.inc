        if(symbol=="D3DX10CreateDevice" || symbol=="D3DX10CreateDeviceAndSwapChain"){
            // D3DX10CreateDevice(Adapter, DriverType, Software, Flags, ppDevice)／AndSwapChain(..., pSwapChainDesc, ppSwapChain, ppDevice)：
            // 優先建立 10.1 等級的裝置（回傳 ID3D10Device，可再取得 ID3D10Device1），與 D3D10CreateDevice1 共用裝置與交換鏈建立。
            graphics_handled=true;const bool swap=symbol.ends_with("AndSwapChain");const unsigned device_output=swap?6:4,swap_output=5;
            if(arg(device_output)){output(device_output);write_pointer(arg(device_output),0);}
            if(swap && arg(swap_output)){output(swap_output);write_pointer(arg(swap_output),0);}
            require(!arg(2) && u32(1)==0 && !(u32(3)&~0x23fU));
            auto device=make_hardware_device(true,u32(3),0xa100,arg(0));
            try{
                if(swap && arg(swap_output)){require(arg(4)!=0);write_pointer(arg(swap_output),make_swap(device,0,arg(4),0,false)->root);}
                if(arg(device_output))write_pointer(arg(device_output),device->root);else release(device->root);
            }catch(...){release(device->root);throw;}
        }else if(symbol=="D3DX10GetFeatureLevel1"){
            // (ID3D10Device*, ID3D10Device1**)：引擎的 D3D10 裝置都支援 ID3D10Device1，回傳同一個物件並增加參考。
            graphics_handled=true;output(1);write_pointer(arg(1),0);auto device=typed(arg(0),Kind::Device);hold(device->root);write_pointer(arg(1),device->root);
        }else if(symbol=="D3DX10CheckVersion" || symbol=="D3DX11CheckVersion"){
            graphics_handled=true;
            const bool eleven=symbol=="D3DX11CheckVersion";
            const auto version=static_cast<unsigned>(service.family[7]-'0')*10U+static_cast<unsigned>(service.family[8]-'0');
            result=eleven?(u32(0)==7 && u32(1)==version):d3dx10_check_version(u32(0),u32(1),version);
        }else if(symbol=="D3D10CreateDevice" || symbol=="D3D10CreateDevice1" || symbol=="D3D10CreateDeviceAndSwapChain" || symbol=="D3D10CreateDeviceAndSwapChain1"){
            graphics_handled=true;const bool extended=symbol.ends_with("1"),swap=symbol.find("AndSwapChain")!=symbol.npos;
            const unsigned sdk=extended?5:4,device_output=sdk+(swap?3:1),swap_output=sdk+2;
            if(arg(device_output)){output(device_output);write_pointer(arg(device_output),0);}
            if(swap && arg(swap_output)){output(swap_output);write_pointer(arg(swap_output),0);}
            const auto level=extended?u32(4):0xa000U;
            require(u32(sdk)==(extended?32U:29U) && !arg(2) && u32(1)==0 && (level==0xa000 || level==0xa100) && !(u32(3)&~0x23fU));
            auto device=make_hardware_device(true,u32(3),level,arg(0));
            try{
                if(swap && arg(swap_output)){require(arg(sdk+1)!=0);write_pointer(arg(swap_output),make_swap(device,0,arg(sdk+1),0,false)->root);}
                if(arg(device_output))write_pointer(arg(device_output),device->root);else release(device->root);
            }catch(...){release(device->root);throw;}
        }else if(service.family=="internal-d3d10" && (symbol=="SetTextFilterSize" || symbol=="GetTextFilterSize")){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);
            if(symbol=="SetTextFilterSize"){require(u32(1)>0 && u32(2)>0);device->text_filter_size={u32(1),u32(2)};}
            else {if(arg(1))memory.store(GuestAddress{arg(1)},32,device->text_filter_size[0]);if(arg(2))memory.store(GuestAddress{arg(2)},32,device->text_filter_size[1]);}
        }
