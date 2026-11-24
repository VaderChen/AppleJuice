        // 檔案、記憶體與 PE 資源共用相同影像解碼及沙箱路徑規則。
        const auto load_d3dx_image=[&](unsigned index,bool resource,bool from_memory){
            std::vector<std::uint8_t> bytes;
            if(resource)bytes=d3dx_resource_bytes(arg(index),arg(index+1),0,symbol.ends_with('W'));
            else if(from_memory){
                // 官方 D3DX 依影像內容本身的長度解析（PNG 的 IEND 等）：傳入超過可讀範圍的長度（常見 0xFFFFFFFF）時只讀到所在映射的結尾。
                require(arg(index) && u(index+1));std::uint64_t size=u(index+1);
                if(const auto mapping=memory.query(GuestAddress{arg(index)});mapping && mapping->base.value+mapping->size>arg(index))size=std::min<std::uint64_t>(size,mapping->base.value+mapping->size-arg(index));
                require(size<=512U*1024U*1024U);bytes=read(arg(index),static_cast<std::size_t>(size));
            }else{
                require(arg(index));
                const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(index)})):ansi_to_utf8(read_ansi(memory,arg(index)));
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
