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

id<MTLTexture> metal_shader_texture(const GraphicsView& view){
    auto& resource=metal_resource(view.resource);auto texture=resource.texture;if(!texture)return nil;
    const auto format=view.format?view.format:resource.desc.format;
    const bool rgb32=format>=5 && format<=8;const bool stencil=format==22 || format==47;auto type=texture.textureType;
    // 單層資源也能建立 array view；著色器取樣維度由 view 決定，不能依 layer 數推斷。
    switch(view.dimension){case 2:type=MTLTextureType2D;break;case 3:type=MTLTextureType2DArray;break;
        case 4:type=MTLTextureType2D;break;case 5:type=MTLTextureType2DArray;break;
        case 6:type=MTLTextureType2DMultisample;break;case 7:type=MTLTextureType2DMultisampleArray;break;
        case 8:type=MTLTextureType3D;break;case 9:type=MTLTextureTypeCube;break;case 10:type=MTLTextureTypeCubeArray;break;default:break;}
    auto pixel_format=metal_format(format);
    if(metal_depth_format(texture.pixelFormat))pixel_format=stencil?MTLPixelFormatX32_Stencil8:texture.pixelFormat;
    if(view.level || view.levels!=resource.desc.levels || view.slice || view.slices!=resource.desc.layers || pixel_format!=texture.pixelFormat || stencil || rgb32 || type!=texture.textureType)
    return resource.shader_view((__bridge const void*)texture,{2,static_cast<std::uint32_t>(pixel_format),static_cast<std::uint32_t>(type),(rgb32?1U:0U)|(stencil?2U:0U),view.level|(view.levels<<16),view.slice|(view.slices<<16)},[&]()->id<MTLTexture>{
        id<MTLTexture> made=nil;
        if(rgb32){
            const auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleOne);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(view.level,view.levels) slices:NSMakeRange(view.slice,view.slices) swizzle:swizzle];
        }else if(stencil){
            // DXGI 的 stencil 位於 G 通道；Metal 的 X32_Stencil8 讀值在 R。
            const auto swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero,MTLTextureSwizzleRed,MTLTextureSwizzleZero,MTLTextureSwizzleOne);
            made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(view.level,view.levels) slices:NSMakeRange(view.slice,view.slices) swizzle:swizzle];
        }else made=[texture newTextureViewWithPixelFormat:pixel_format textureType:type levels:NSMakeRange(view.level,view.levels) slices:NSMakeRange(view.slice,view.slices)];
        if(!made)throw std::invalid_argument("Metal 紋理 view 格式或範圍不合法");
        return made;
    });
    return texture;
}
