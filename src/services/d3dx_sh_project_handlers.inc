        if(symbol=="D3DX10SHProjectCubeMap" || symbol=="D3DX11SHProjectCubeMap"){
            graphics_handled=true;
            const bool api10=symbol=="D3DX10SHProjectCubeMap";
            const unsigned offset=api10?0:1;
            const unsigned order=u32(offset);
            const auto found=bridge.objects.find_fast(arg(offset+1));
            require(order>=2 && order<=6 && found!=bridge.objects.end() && found->second.kind==Kind::Texture);
            const auto texture=found->second.identity;
            require(texture->api10==api10 && texture->resource && !texture->offered &&
                texture->subresource_maps.empty());
            const auto device=object(texture->device).identity;
            const auto ctx=bridge.objects.find_fast(api10?device->context10:arg(0));
            require(ctx!=bridge.objects.end() && ctx->second.kind==Kind::Context);
            const auto context=ctx->second.identity;
            require(context->device==texture->device && !context->context_type);
            const auto& desc=texture->resource->desc;
            require(desc.dimension==3 && desc.layers==6 && (desc.misc&4) && desc.width==desc.height && desc.samples==1);
            const std::size_t output_size=order*order*sizeof(float);
            const std::array outputs{arg(offset+2),arg(offset+3),arg(offset+4)};
            for(const auto address:outputs)if(address)memory.check(GuestAddress{address},output_size,Access::Write);

            auto& native=*context->native;
            auto accumulated=sh_project_cube_metal(native,texture->resource,order,device->sh_cube_shaders);
            if(accumulated)record_diagnostic("d3dx_sh_project_metal");
            else{
                ShCubeProjection projection(order,ShCubeProjection::Weighting::NormalizedDifferential);
                for(unsigned face=0;face<6;++face){
                    const auto pixels=native.graphics_read(subresource(texture,face*desc.levels));
                    projection.add_face(face,desc.width,desc.format,pixels);
                }
                accumulated=projection.coefficients();
                record_diagnostic("d3dx_sh_project_cpu");
            }
            for(unsigned channel=0;channel<3;++channel)if(outputs[channel])
                memory.write(GuestAddress{outputs[channel]},
                    {reinterpret_cast<const std::uint8_t*>((*accumulated)[channel].data()),output_size});
        }
