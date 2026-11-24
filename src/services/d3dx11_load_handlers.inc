        if(const D3dxTextureEntry texture_entry(symbol);texture_entry.valid){
            graphics_handled=true;
            require(arg(texture_entry.out_arg) && !arg(texture_entry.pump_arg));
            if(texture_entry.info)memory.check(GuestAddress{arg(texture_entry.out_arg)},36,Access::Write);
            else{
                output(texture_entry.out_arg);
                write_pointer(arg(texture_entry.out_arg),0);
            }
            if(arg(texture_entry.status_arg))memory.check(GuestAddress{arg(texture_entry.status_arg)},4,Access::Write);
            std::vector<std::uint8_t> source;
            if(texture_entry.file){
                const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(texture_entry.source_arg)})):ansi_to_utf8(read_ansi(memory,arg(texture_entry.source_arg)));
                const auto opened=filesystem_.open_reader(path);if(opened.error)throw texture_image::Error(0x80070000U|opened.error,"無法讀取紋理檔案");
                const auto metadata=opened.file->information();if(metadata.error)throw texture_image::Error(0x80070000U|metadata.error,"無法取得紋理檔案大小");require(metadata.metadata.size<=512U*1024U*1024U);
                source.resize(static_cast<std::size_t>(metadata.metadata.size));const auto io=opened.file->read(source,0);if(io.error || io.bytes!=source.size())throw texture_image::Error(0x80070000U|(io.error?io.error:38U),"紋理檔案讀取未完成");
            }else if(texture_entry.resource){
                const auto found=gui_resource(memory,arg(texture_entry.source_arg),arg(texture_entry.source_arg+1),10,symbol.ends_with('W'));if(!found)throw texture_image::Error(0x80070716U,"找不到紋理 RCDATA 資源");
                const auto size=memory.load(GuestAddress{found+4},32);require(size<=512U*1024U*1024U);source=read_bytes(memory,resource_image(found,true)->base+memory.load(GuestAddress{found},32),size);
            }else{require(arg(texture_entry.source_arg+1)<=512U*1024U*1024U);source=read_bytes(memory,arg(texture_entry.source_arg),static_cast<std::size_t>(arg(texture_entry.source_arg+1)));}
            auto decoded=texture_image::decode(source);
            if(texture_entry.info)emit_d3dx_image_info(decoded,arg(texture_entry.out_arg));
            else{
                const auto device=typed(arg(0),Kind::Device);
                const auto options=arg(texture_entry.load_arg)?read_bytes(memory,arg(texture_entry.load_arg),48+pointer_size):std::vector<std::uint8_t>{};
                const auto prepared=prepare_d3dx_texture(decoded,options,texture_entry.view);
                write_pointer(arg(texture_entry.out_arg),create_d3dx_texture(device,decoded,prepared,texture_entry.view));
            }
        }
