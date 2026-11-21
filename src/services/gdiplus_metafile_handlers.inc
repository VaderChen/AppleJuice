// 讀取類 GpMetafile 與 GDI handle 分別管理，GetHemf 成功後由客體負責 DeleteEnhMetaFile。
if(const auto entry=g.metafiles.find(arg(0));entry!=g.metafiles.end()){
    auto& image=entry->second;
    const auto& h=image.header;
    if(symbol=="GdipDisposeImage"){
        if(image.own_wmf)gui_.windows_metafiles.erase(image.wmf);
        if(image.own_emf)gui_.metafiles.erase(image.emf);
        g.metafiles.erase(entry);release_handle(arg(0));return finish(ok);
    }
    if(symbol=="GdipGetHemfFromMetafile"){
        if(!arg(1) || !image.emf)return finish(invalid_parameter);
        memory.store(GuestAddress{arg(1)},pointer_bits,image.emf);
        image.emf=0;image.own_emf=false;return finish(ok);
    }
    if(symbol=="GdipGetMetafileHeaderFromMetafile"){
        if(!arg(1))return finish(invalid_parameter);
        memory.write(GuestAddress{arg(1)},std::bit_cast<std::array<std::uint8_t,sizeof(h)>>(h));return finish(ok);
    }
    if(symbol=="GdipCloneImage"){
        if(!arg(1))return finish(invalid_parameter);
        memory.store(GuestAddress{arg(1)},pointer_bits,0);
        const auto original=gui_.metafiles.find(image.emf);
        if(original==gui_.metafiles.end())return finish(invalid_parameter);
        auto copy=image;copy.own_emf=true;copy.wmf=0;copy.own_wmf=false;
        const auto handle=new_handle();if(!handle)return finish(out_of_memory);
        copy.emf=0;
        try{
            copy.emf=import_enhanced_metafile(original->second.bytes).value_or(0);
            if(!copy.emf){release_handle(handle);return finish(generic_error);}
            g.metafiles.emplace(handle,copy);memory.store(GuestAddress{arg(1)},pointer_bits,handle);
        }catch(...){gui_.metafiles.erase(copy.emf);g.metafiles.erase(handle);release_handle(handle);throw;}
        return finish(ok);
    }
    if(symbol=="GdipGetImageBounds" || symbol=="GdipGetImageDimension"){
        if(!arg(1) || !arg(2))return finish(invalid_parameter);
        if(symbol=="GdipGetImageBounds"){
            memory.write(GuestAddress{arg(1)},std::bit_cast<std::array<std::uint8_t,16>>(image.bounds));
            memory.store(GuestAddress{arg(2)},32,2);
        }else{
            memory.store(GuestAddress{arg(1)},32,std::bit_cast<std::uint32_t>(image.bounds[2]));
            memory.store(GuestAddress{arg(2)},32,std::bit_cast<std::uint32_t>(image.bounds[3]));
        }
        return finish(ok);
    }
    if(symbol=="GdipGetImageRawFormat"){
        if(!arg(1))return finish(invalid_parameter);
        const std::array<std::uint8_t,16> format{image.format,
            0x3c,0x6b,0xb9,0x28,0x07,0xd3,0x11,0x9d,0x7b,0x00,0x00,0xf8,0x1e,0xf3,0x2e};
        memory.write(GuestAddress{arg(1)},format);return finish(ok);
    }
    if(symbol=="GdipGetImageType" || symbol=="GdipGetImageWidth" || symbol=="GdipGetImageHeight" ||
       symbol=="GdipGetImagePixelFormat" || symbol=="GdipGetImageHorizontalResolution" || symbol=="GdipGetImageVerticalResolution"){
        if(!arg(1) || (!image.emf && (symbol=="GdipGetImageWidth" || symbol=="GdipGetImageHeight")))return finish(invalid_parameter);
        std::uint32_t result=2;
        if(symbol=="GdipGetImageWidth")result=static_cast<std::uint32_t>(static_cast<std::int64_t>(image.bounds[2]));
        else if(symbol=="GdipGetImageHeight")result=static_cast<std::uint32_t>(static_cast<std::int64_t>(image.bounds[3]));
        else if(symbol=="GdipGetImagePixelFormat")result=rgb24;
        else if(symbol=="GdipGetImageHorizontalResolution")result=std::bit_cast<std::uint32_t>(h.dpi_x);
        else if(symbol=="GdipGetImageVerticalResolution")result=std::bit_cast<std::uint32_t>(h.dpi_y);
        memory.store(GuestAddress{arg(1)},32,result);return finish(ok);
    }
}
if(symbol=="GdipGetMetafileHeaderFromMetafile" || symbol=="GdipGetHemfFromMetafile")return finish(invalid_parameter);
