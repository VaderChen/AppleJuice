        if(symbol=="D3DX10ComputeNormalMap" || symbol=="D3DX11ComputeNormalMap"){
            graphics_handled=true;
            const bool api10=symbol=="D3DX10ComputeNormalMap";
            const unsigned offset=api10?0:1;
            // Win64 的 D3DX10 第四個參數在 XMM3，D3DX11 第五個參數在堆疊。
            auto amplitude_bits=u32(offset+3);
            if(api10 && !x86_)std::memcpy(&amplitude_bits,state.xmm[3].data(),sizeof(amplitude_bits));
            const auto amplitude=std::bit_cast<float>(amplitude_bits);
            const auto flags=u32(offset+1),channel=u32(offset+2);
            const auto lookup=[&](std::uint64_t address,Kind kind)->std::shared_ptr<Identity>{
                const auto found=bridge.objects.find_fast(address);
                if(found==bridge.objects.end() || found->second.kind!=kind)return {};
                return found->second.identity;
            };
            const auto validate=[](bool valid){
                if(!valid)throw texture_image::Error(0x8876086cU,"法線貼圖參數或資源無效");
            };
            const auto source=lookup(arg(offset),Kind::Texture);
            const auto destination=lookup(arg(offset+4),Kind::Texture);
            validate(source && destination && source->resource && destination->resource &&
                source->device==destination->device && source->api10==api10 && destination->api10==api10);
            const auto device=object(source->device).identity;
            const auto context=lookup(api10?device->context10:arg(0),Kind::Context);
            validate(context && !context->context_type && context->device==source->device &&
                !source->offered && !destination->offered && source->subresource_maps.empty() &&
                destination->subresource_maps.empty() && destination->texture_desc[7]!=1);
            const auto& sd=source->resource->desc;
            const auto& dd=destination->resource->desc;
            validate(sd.dimension==3 && dd.dimension==3 && sd.samples==1 && dd.samples==1 &&
                sd.width==dd.width && sd.height==dd.height &&
                sd.layers==dd.layers && sd.levels>=dd.levels && !(flags&~0x1b0000U) &&
                channel && channel<=16 && std::has_single_bit(channel) && std::isfinite(amplitude));

            auto& native=*context->native;
            static const bool metal_enabled=[] {
                const auto* value=std::getenv("X64ARM_METAL_D3DX_NORMAL_MAP");
                return !value || std::string_view(value)!="0";
            }();
            // 支援直接讀取的 UNORM 來源與 RGBA8 輸出。暫存紋理讓原地轉換及未設 UAV 的目的紋理共用同一路徑。
            std::shared_ptr<GraphicsResource> scratch,constants;
            if(metal_enabled && (sd.bind&8) && (sd.format==28 || sd.format==87 || sd.format==61) &&
                dd.format==28 && std::abs(amplitude)<=1.0e8f){
                try{
                    if(!device->normal_map_shader){
                        GraphicsModernShader shader;
                        shader.entry="normal_map";
                        shader.threads={8,8,1};
                        shader.bindings={{0,0,0},{1,0,0},{5,0,1}};
                        shader.source=
#include "d3dx_normal_map_metal.inc"
                        ;
                        device->normal_map_shader=native.graphics_compute_source(shader);
                    }
                    auto desc=dd;
                    desc.layers=1;
                    desc.misc=0;
                    desc.bind=0x80;
                    scratch=native.graphics_resource(desc);
                    const std::array parameters{flags,channel,amplitude_bits,sd.format==61?1U:0U};
                    GraphicsResourceDesc buffer;
                    buffer.width=sizeof(parameters);
                    buffer.bind=4;
                    buffer.initialized=buffer.width;
                    constants=native.graphics_resource(buffer);
                    native.graphics_write(GraphicsView{constants},
                        {reinterpret_cast<const std::uint8_t*>(parameters.data()),sizeof(parameters)},
                        buffer.width,buffer.width);
                }catch(const std::runtime_error&){
                    scratch.reset();
                }catch(const std::invalid_argument&){
                    scratch.reset();
                }
            }
            const auto dimensions=[&](unsigned mip){
                return std::array{std::max(1U,dd.width>>mip),std::max(1U,dd.height>>mip)};
            };
            if(scratch){
                for(unsigned layer=0;layer<dd.layers;++layer)for(unsigned mip=0;mip<dd.levels;++mip){
                    const auto [width,height]=dimensions(mip);
                    auto stage=std::make_shared<GraphicsStage>();
                    stage->shader=device->normal_map_shader;
                    stage->constants[0]=constants;
                    stage->resources[0]=subresource(source,layer*sd.levels+mip);
                    stage->resources[0].dimension=4;
                    stage->unordered[0]=GraphicsView{scratch,dd.format,mip};
                    native.graphics_dispatch_shared(stage,{(width+7)/8,(height+7)/8,1});
                    native.graphics_copy(subresource(destination,layer*dd.levels+mip),
                        stage->unordered[0],{0,0,0},{0,0,0,width,height,1});
                }
                record_diagnostic("d3dx_normal_map_metal");
            }else{
                // 先完成所有層的格式轉換，再寫回，避免不支援的轉換留下部分結果。
                std::vector<std::vector<std::uint8_t>> converted_subresources;
                converted_subresources.reserve(dd.layers*dd.levels);
                for(unsigned layer=0;layer<dd.layers;++layer)for(unsigned mip=0;mip<dd.levels;++mip){
                    const auto [width,height]=dimensions(mip);
                    const auto input=native.graphics_read(subresource(source,layer*sd.levels+mip));
                    converted_subresources.push_back(
                        texture_image::normal_map(input,sd.format,width,height,dd.format,flags,channel,amplitude));
                }
                for(unsigned index=0;index<converted_subresources.size();++index){
                    const auto [width,height]=dimensions(index%dd.levels);
                    const auto pitch=graphics_format_pitch(dd.format,width,height);
                    native.graphics_write(subresource(destination,index),converted_subresources[index],pitch[0],pitch[0]*pitch[1]);
                }
                record_diagnostic("d3dx_normal_map_cpu");
            }
            destination->pristine=false;
            if(destination->texture_desc[7]==3)destination->gpu_fence=native.graphics_fence_pending();
        }
