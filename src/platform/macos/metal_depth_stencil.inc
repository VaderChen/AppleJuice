// DXGI 的交錯深度／模板資料透過 Metal 的兩個實際平面傳輸；不能直接
// 將 24 位元 UNORM 的客體資料當成 Metal 的 32 位元浮點深度。
bool metal_depth_format(MTLPixelFormat format){
    return format==MTLPixelFormatDepth32Float_Stencil8 || format==MTLPixelFormatDepth32Float || format==MTLPixelFormatDepth16Unorm;
}
void metal_depth_transfer(id<MTLDevice> device,id<MTLCommandQueue> queue,const GraphicsView& view,
    std::span<std::uint8_t> data,std::uint32_t row,std::array<std::uint32_t,4> region,bool upload){
    auto& resource=metal_resource(view.resource);const auto texture=resource.texture;
    if(!metal_depth_format(texture.pixelFormat) || resource.desc.samples!=1 || view.level>=resource.desc.levels || view.slice>=resource.desc.layers)
        throw std::invalid_argument("深度／模板平面傳輸的資源不合法");
    const auto [x,y,width,height]=region;const bool combined=texture.pixelFormat==MTLPixelFormatDepth32Float_Stencil8;
    const bool packed24=resource.desc.format>=44 && resource.desc.format<=47;
    const bool depth_plane=(view.flags&graphics_view_depth_plane)!=0,stencil_plane=(view.flags&graphics_view_stencil_plane)!=0;
    if(stencil_plane && !combined)throw std::invalid_argument("沒有 stencil 平面的深度資源不能傳輸 stencil 平面");
    if(depth_plane || stencil_plane){
        // D3D12 平面式子資源：只傳一個平面，客體資料每像素 4（深度）或 1（stencil）位元組。
        const unsigned bpp=stencil_plane?1U:4U;
        if(!width || !height || x+width>std::max(1U,resource.desc.width>>view.level) || y+height>std::max(1U,resource.desc.height>>view.level) ||
           row<std::uint64_t{width}*bpp || data.size()<std::uint64_t{row}*(height-1)+std::uint64_t{width}*bpp)
            throw std::invalid_argument("深度／模板平面傳輸越界");
        const unsigned host_bytes=stencil_plane?1U:texture.pixelFormat==MTLPixelFormatDepth16Unorm?2U:4U;
        const std::size_t pitch=(std::size_t{width}*host_bytes+255)&~std::size_t{255},plane=pitch*height;
        auto lease=reserve_gpu_memory(plane);
        auto buffer=[device newBufferWithLength:plane options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};
        auto* bytes=static_cast<std::uint8_t*>(buffer.contents);
        if(upload)for(unsigned iy=0;iy<height;++iy)for(unsigned ix=0;ix<width;++ix){
            const auto* src=data.data()+std::size_t{iy}*row+ix*bpp;auto* dst=bytes+iy*pitch+ix*host_bytes;
            if(stencil_plane)*dst=*src;
            else if(packed24){std::uint32_t value=0;std::memcpy(&value,src,4);const float depth=static_cast<float>(value&0xffffffU)/16777215.0F;std::memcpy(dst,&depth,4);}
            else std::memcpy(dst,src,host_bytes);
        }
        auto command=[queue commandBuffer];if(!command)throw std::runtime_error("深度／模板 command buffer 建立失敗");
        auto encoder=[command blitCommandEncoder];MetalEncoderScope scope{encoder};if(!encoder)throw std::runtime_error("深度／模板 blit encoder 建立失敗");
        const auto option=!combined?MTLBlitOptionNone:stencil_plane?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil;
        if(upload)[encoder copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:plane sourceSize:MTLSizeMake(width,height,1) toTexture:texture destinationSlice:view.slice destinationLevel:view.level destinationOrigin:MTLOriginMake(x,y,0) options:option];
        else [encoder copyFromTexture:texture sourceSlice:view.slice sourceLevel:view.level sourceOrigin:MTLOriginMake(x,y,0) sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:plane options:option];
        scope.end();track_gpu_commands(command);[command commit];[command waitUntilCompleted];
        if(command.status==MTLCommandBufferStatusError)throw std::runtime_error("深度／模板平面傳輸失敗："+std::string(command.error.localizedDescription.UTF8String));
        if(!upload)for(unsigned iy=0;iy<height;++iy)for(unsigned ix=0;ix<width;++ix){
            const auto* src=bytes+iy*pitch+ix*host_bytes;auto* dst=data.data()+std::size_t{iy}*row+ix*bpp;
            if(stencil_plane)*dst=*src;
            else if(packed24){float depth=0;std::memcpy(&depth,src,4);const auto value=static_cast<std::uint32_t>(std::llround(std::clamp(static_cast<double>(depth),0.0,1.0)*16777215.0));std::memcpy(dst,&value,4);}
            else{std::memset(dst,0,bpp);std::memcpy(dst,src,host_bytes);}
        }
        return;
    }
    const unsigned depth_bytes=texture.pixelFormat==MTLPixelFormatDepth16Unorm?2U:4U,bpp=graphics_format_bytes(resource.desc.format);
    if(!width || !height || x+width>std::max(1U,resource.desc.width>>view.level) || y+height>std::max(1U,resource.desc.height>>view.level) ||
       row<std::uint64_t{width}*bpp || data.size()<std::uint64_t{row}*(height-1)+std::uint64_t{width}*bpp)
        throw std::invalid_argument("深度／模板平面傳輸越界");
    const std::size_t pitch=(std::size_t{width}*depth_bytes+255)&~std::size_t{255},plane=pitch*height;
    auto lease=reserve_gpu_memory(plane*(combined?2U:1U));
    auto buffer=[device newBufferWithLength:plane*(combined?2U:1U) options:MTLResourceStorageModeShared];
    if(!buffer)throw std::bad_alloc{};auto* bytes=static_cast<std::uint8_t*>(buffer.contents);
    if(upload){
        std::memset(bytes,0,buffer.length);
        for(unsigned iy=0;iy<height;++iy)for(unsigned ix=0;ix<width;++ix){
            const auto* src=data.data()+std::size_t{iy}*row+ix*bpp;auto* dst=bytes+iy*pitch+ix*depth_bytes;
            if(packed24){std::uint32_t value=0;std::memcpy(&value,src,4);const float depth=static_cast<float>(value&0xffffffU)/16777215.0F;std::memcpy(dst,&depth,4);}
            else std::memcpy(dst,src,depth_bytes);
            if(combined)bytes[plane+iy*pitch+ix]=src[packed24?3:4];
        }
    }
    auto command=[queue commandBuffer];if(!command)throw std::runtime_error("深度／模板 command buffer 建立失敗");
    auto encoder=[command blitCommandEncoder];MetalEncoderScope scope{encoder};if(!encoder)throw std::runtime_error("深度／模板 blit encoder 建立失敗");
    for(unsigned aspect=0;aspect<(combined?2U:1U);++aspect){
        const auto option=!combined?MTLBlitOptionNone:aspect?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil;
        if(upload)[encoder copyFromBuffer:buffer sourceOffset:aspect*plane sourceBytesPerRow:pitch sourceBytesPerImage:plane sourceSize:MTLSizeMake(width,height,1) toTexture:texture destinationSlice:view.slice destinationLevel:view.level destinationOrigin:MTLOriginMake(x,y,0) options:option];
        else [encoder copyFromTexture:texture sourceSlice:view.slice sourceLevel:view.level sourceOrigin:MTLOriginMake(x,y,0) sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:aspect*plane destinationBytesPerRow:pitch destinationBytesPerImage:plane options:option];
    }
    scope.end();track_gpu_commands(command);[command commit];[command waitUntilCompleted];
    if(command.status==MTLCommandBufferStatusError)throw std::runtime_error("深度／模板平面傳輸失敗："+std::string(command.error.localizedDescription.UTF8String));
    if(!upload)for(unsigned iy=0;iy<height;++iy)for(unsigned ix=0;ix<width;++ix){
        const auto* src=bytes+iy*pitch+ix*depth_bytes;auto* dst=data.data()+std::size_t{iy}*row+ix*bpp;
        if(packed24){float depth=0;std::memcpy(&depth,src,4);const auto value=static_cast<std::uint32_t>(std::llround(std::clamp(static_cast<double>(depth),0.0,1.0)*16777215.0))|(std::uint32_t{bytes[plane+iy*pitch+ix]}<<24);std::memcpy(dst,&value,4);}
        else {std::memset(dst,0,bpp);std::memcpy(dst,src,depth_bytes);if(combined)dst[4]=bytes[plane+iy*pitch+ix];}
    }
}

// GPU 專用顏色紋理（渲染目標／UAV，Private 儲存以啟用無損壓縮）的 CPU 傳輸：經 Shared staging 緩衝與 blit，
// 與深度平面傳輸相同在呼叫端 wait_resource 之後以同一佇列提交並等待完成。row／image 為客體資料的列與平面間距。
bool metal_private_targets() noexcept {
    // 渲染目標與 UAV 紋理預設 Shared（Apple GPU 不能壓縮）。2026-11-09《暗影》1080p 4 vCPU 開關各兩輪 55.3 → 55.75 FPS，
    // 在雜訊內，且 CPU 寫入這類紋理改為同步 blit，預設關閉；X64ARM_METAL_PRIVATE_TARGETS=1 開啟。
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_METAL_PRIVATE_TARGETS");return value && std::string_view(value)=="1";}();
    return enabled;
}
void metal_private_color_transfer(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLTexture> texture,std::uint32_t level,std::uint32_t slice,
    std::span<std::uint8_t> data,std::uint32_t row,std::uint32_t image,MTLRegion region,bool upload){
    const auto planes=static_cast<std::size_t>(region.size.depth),rows=static_cast<std::size_t>(region.size.height);
    if(!image)image=static_cast<std::uint32_t>(std::size_t{row}*rows);
    const std::size_t length=std::size_t{image}*(planes-1)+std::size_t{row}*rows;
    if(!length || data.size()<length)throw std::invalid_argument("Private 紋理傳輸資料不足");
    auto lease=reserve_gpu_memory(length);
    auto buffer=[device newBufferWithLength:length options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};
    if(upload)std::memcpy(buffer.contents,data.data(),length);
    auto command=[queue commandBuffer];if(!command)throw std::runtime_error("Private 紋理 command buffer 建立失敗");
    auto encoder=[command blitCommandEncoder];MetalEncoderScope scope{encoder};if(!encoder)throw std::runtime_error("Private 紋理 blit encoder 建立失敗");
    const NSUInteger per_image=texture.textureType==MTLTextureType3D?image:0;
    if(upload)[encoder copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:per_image sourceSize:region.size
                            toTexture:texture destinationSlice:slice destinationLevel:level destinationOrigin:region.origin];
    else [encoder copyFromTexture:texture sourceSlice:slice sourceLevel:level sourceOrigin:region.origin sourceSize:region.size
                         toBuffer:buffer destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:per_image];
    scope.end();track_gpu_commands(command);[command commit];[command waitUntilCompleted];
    if(command.status==MTLCommandBufferStatusError)throw std::runtime_error("Private 紋理傳輸失敗："+std::string(command.error.localizedDescription.UTF8String));
    if(!upload)std::memcpy(data.data(),buffer.contents,length);
}

id<MTLTexture> metal_shader_texture(const GraphicsView& view){
    auto& resource=metal_resource(view.resource);auto texture=resource.texture;if(!texture)return nil;
    const auto format=view.format?view.format:resource.desc.format;
    const bool rgb32=format>=5 && format<=8;const bool stencil=format==22 || format==47;
    const bool replicate_red=(view.flags&graphics_view_replicate_red)!=0,red_only=(view.flags&graphics_view_red_only)!=0;auto type=texture.textureType;
    // 單層資源也能建立 array view；著色器取樣維度由 view 決定，不能依 layer 數推斷。
    switch(view.dimension){case 2:type=MTLTextureType2D;break;case 3:type=MTLTextureType2DArray;break;
        case 4:type=MTLTextureType2D;break;case 5:type=MTLTextureType2DArray;break;
        case 6:type=MTLTextureType2DMultisample;break;case 7:type=MTLTextureType2DMultisampleArray;break;
        case 8:type=MTLTextureType3D;break;case 9:type=MTLTextureTypeCube;break;case 10:type=MTLTextureTypeCubeArray;break;default:break;}
    auto pixel_format=metal_format(format);
    if(metal_depth_format(texture.pixelFormat))pixel_format=stencil?MTLPixelFormatX32_Stencil8:texture.pixelFormat;
    // Metal view 的範圍須符合類型：3D 只能涵蓋單一 slice（整個體積；D3D 的 FirstWSlice／WSize 以整個體積表示），
    // 2D／MSAA 為 1、Cube 為 6；0 表示其餘全部（D3D 的 -1／未指定）。
    const bool volume=type==MTLTextureType3D || texture.textureType==MTLTextureType3D;
    const std::uint32_t layers=std::max(1U,resource.desc.layers),slice=volume?0:std::min(view.slice,layers-1);
    std::uint32_t slices=view.slices && view.slices<=layers-slice?view.slices:layers-slice;
    if(volume || type==MTLTextureType2D || type==MTLTextureType2DMultisample)slices=1;else if(type==MTLTextureTypeCube)slices=6;
    else if(type==MTLTextureTypeCubeArray)slices=std::max(6U,slices/6*6); // Metal 要求 Cube 陣列的 slice 數是 6 的倍數。
    const std::uint32_t mips=std::max(1U,resource.desc.levels),level=std::min(view.level,mips-1),levels=view.levels && view.levels<=mips-level?view.levels:mips-level;
    const std::uint32_t mapping=view.swizzle&0x1fffU;
    if(level || levels!=resource.desc.levels || slice || (!volume && slices!=resource.desc.layers) || pixel_format!=texture.pixelFormat || stencil || rgb32 || replicate_red || red_only || mapping || type!=texture.textureType)
    return resource.shader_view((__bridge const void*)texture,{2,static_cast<std::uint32_t>(pixel_format),static_cast<std::uint32_t>(type),(rgb32?1U:0U)|(stencil?2U:0U)|(replicate_red?4U:0U)|(red_only?8U:0U)|(mapping<<4),level|(levels<<16),slice|(slices<<16)},[&]()->id<MTLTexture>{
        id<MTLTexture> made=nil;
        if(mapping){
            // 先依格式得到 D3D 看到的四個分量（RGB32 的 A 為 1、stencil 在 G 等），再套用 D3D12 的 Shader4ComponentMapping。
            std::array<MTLTextureSwizzle,4> base{MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha};
            if(replicate_red)base={MTLTextureSwizzleRed,MTLTextureSwizzleRed,MTLTextureSwizzleRed,MTLTextureSwizzleRed};
            else if(red_only)base={MTLTextureSwizzleRed,MTLTextureSwizzleZero,MTLTextureSwizzleZero,MTLTextureSwizzleOne};
            else if(rgb32)base[3]=MTLTextureSwizzleOne;
            else if(stencil)base={MTLTextureSwizzleZero,MTLTextureSwizzleRed,MTLTextureSwizzleZero,MTLTextureSwizzleOne};
            std::array<MTLTextureSwizzle,4> channel{};
            for(unsigned c=0;c<4;++c){const auto source=(mapping>>(3*c))&7U;channel[c]=source<4?base[source]:source==4?MTLTextureSwizzleZero:MTLTextureSwizzleOne;}
            const auto swizzle=MTLTextureSwizzleChannelsMake(channel[0],channel[1],channel[2],channel[3]);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(level,levels) slices:NSMakeRange(slice,slices) swizzle:swizzle];
        }else if(replicate_red){
            const auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleRed,MTLTextureSwizzleRed,MTLTextureSwizzleRed);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(level,levels) slices:NSMakeRange(slice,slices) swizzle:swizzle];
        }else if(red_only){
            const auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleZero,MTLTextureSwizzleZero,MTLTextureSwizzleOne);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(level,levels) slices:NSMakeRange(slice,slices) swizzle:swizzle];
        }else if(rgb32){
            const auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleOne);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(level,levels) slices:NSMakeRange(slice,slices) swizzle:swizzle];
        }else if(stencil){
            // DXGI 的 stencil 位於 G 通道；Metal 的 X32_Stencil8 讀值在 R。
            const auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero,MTLTextureSwizzleRed,MTLTextureSwizzleZero,MTLTextureSwizzleOne);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(level,levels) slices:NSMakeRange(slice,slices) swizzle:swizzle];
        }else made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(level,levels) slices:NSMakeRange(slice,slices)];
        if(!made)throw std::invalid_argument("Metal 紋理 view 格式或範圍不合法");
        return made;
    });
    return texture;
}
