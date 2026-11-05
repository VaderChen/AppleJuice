        // 檔案、記憶體與 PE 資源共用相同影像解碼及沙箱路徑規則。
        const auto load_d3dx_image=[&](unsigned index,bool resource,bool from_memory){
            std::vector<std::uint8_t> bytes;
            if(resource){
                require(arg(index+1));
                auto found=gui_resource(memory,arg(index),arg(index+1),10,symbol.ends_with('W'));
                if(!found)found=gui_resource(memory,arg(index),arg(index+1),2,symbol.ends_with('W'));
                require(found!=0,0x80070716U);
                const auto size=memory.load(GuestAddress{found+4},32);require(size && size<=512U*1024U*1024U);
                const auto* owner=resource_image(found,true);require(owner!=nullptr);
                bytes=read(owner->base+memory.load(GuestAddress{found},32),size);
            }else if(from_memory){
                require(arg(index) && u(index+1) && u(index+1)<=512U*1024U*1024U);
                bytes=read(arg(index),u(index+1));
            }else{
                require(arg(index));
                const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(index)})):read_ansi(memory,arg(index));
                const auto opened=filesystem_.open_reader(d3dx_path(path));
                if(opened.error && std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3dx9 texture] open error=%u path=%s\n",opened.error,path.c_str());
                require(!opened.error,0x80070000U|opened.error);const auto meta=opened.file->information();
                require(!meta.error,0x80070000U|meta.error);require(meta.metadata.size && meta.metadata.size<=512U*1024U*1024U);
                bytes.resize(meta.metadata.size);const auto io=opened.file->read(bytes,0);
                require(!io.error && io.bytes==bytes.size(),0x88760b59U);
            }
            return texture_image::decode(bytes);
        };
        const auto d3dx_image_info=[&](const texture_image::Decoded& image,std::uint64_t address){
            if(!address)return;
            const auto& d=image.desc;
            const std::array<unsigned,7> info{d.width,d.height,d.depth,d.levels,legacy_format(d.format),d.dimension==4?4U:(d.misc&4)?5U:3U,image.file_format};
            memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(info.data()),sizeof(info)));
        };
