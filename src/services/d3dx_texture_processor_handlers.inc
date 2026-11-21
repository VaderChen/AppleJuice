        if(symbol=="D3DX11CreateAsyncTextureInfoProcessor" || symbol=="D3DX11CreateAsyncTextureProcessor" ||
            symbol=="D3DX11CreateAsyncShaderResourceViewProcessor"){
            graphics_handled=true;
            const bool info=symbol=="D3DX11CreateAsyncTextureInfoProcessor";
            const bool view=symbol=="D3DX11CreateAsyncShaderResourceViewProcessor";
            const unsigned out=info?1:2;
            require(arg(out)!=0);
            output(out);
            write_pointer(arg(out),0);
            std::vector<std::uint8_t> options;
            std::uint64_t info_pointer=0,device_pointer=0;
            if(info){
                info_pointer=arg(0);
                require(info_pointer!=0);
                memory.check(GuestAddress{info_pointer},36,Access::Write);
            }else{
                require(arg(0)!=0);
                device_pointer=typed(arg(0),Kind::Device)->root;
                if(arg(1))options=read_bytes(memory,arg(1),48+pointer_size);
            }
            auto processor=std::make_unique<Identity::DataProcessor>();
            processor->info_only=info;
            processor->view=view;
            processor->info_pointer=info_pointer;
            processor->options=std::move(options);
            const auto id=create(Kind::DataProcessor,device_pointer);
            id->data_processor=std::move(processor);
            write_pointer(arg(out),id->root);
        }else if(symbol.starts_with("DataProcessor")){
            graphics_handled=true;
            const auto id=typed(arg(0),Kind::DataProcessor);
            auto& processor=*id->data_processor;
            if(symbol=="DataProcessorProcess"){
                processor.decoded.reset();
                require(arg(1) && arg(2)<=512U*1024U*1024U);
                auto decoded=texture_image::decode(read_bytes(memory,arg(1),static_cast<std::size_t>(arg(2))));
                if(processor.info_only)emit_d3dx_image_info(decoded,processor.info_pointer);
                else{
                    processor.prepared=prepare_d3dx_texture(decoded,processor.options,processor.view);
                    processor.decoded.emplace(std::move(decoded));
                }
            }else if(symbol=="DataProcessorCreateDeviceObject"){
                if(!processor.info_only){
                    require(arg(1)!=0);
                    output(1);
                    write_pointer(arg(1),0);
                    if(!processor.decoded)result=failure;
                    else{
                        const auto device=typed(id->device,Kind::Device);
                        write_pointer(arg(1),create_d3dx_texture(device,*processor.decoded,processor.prepared,processor.view));
                    }
                }
            }else if(symbol=="DataProcessorDestroy")release(id->root);
        }
