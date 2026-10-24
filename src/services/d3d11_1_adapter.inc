        // D3D11.1～11.4（ID3D11Device1～5、ID3D11DeviceContext1～4）：與 D3D10 轉接相同只轉換 ABI，
        // 驗證、資源生命週期與 Metal 命令沿用 11.0 的共用處理器。只支援的能力（不含邏輯運算混合、
        // ForcedSampleCount、保守光柵化、非 UNDEFINED 紋理配置、平面 view）由 CheckFeatureSupport 一致回報。
        bool partial_constant_update=false;
        if(service.family=="internal-d3d11"){
            const auto plane_free=[&](std::span<const std::uint8_t> bytes,unsigned plane){if(word(bytes,plane))throw std::invalid_argument("不支援平面格式的 view PlaneSlice");};
            if(symbol=="GetImmediateContext1" || symbol=="GetImmediateContext2" || symbol=="GetImmediateContext3")symbol="GetImmediateContext";
            else if(symbol=="CreateDeferredContext1" || symbol=="CreateDeferredContext2" || symbol=="CreateDeferredContext3")symbol="CreateDeferredContext";
            else if(symbol=="OpenSharedResource1")symbol="OpenSharedResource";
            else if(symbol=="CreateBlendState1"){
                // D3D11_RENDER_TARGET_BLEND_DESC1 多 LogicOpEnable／LogicOp（40 位元組），轉成 32 位元組的 11.0 描述。
                if(arg(1)){
                    const auto desc1=read_bytes(memory,arg(1),328);std::vector<std::uint8_t> desc(264);put_word(desc,0,word(desc1,0));put_word(desc,4,word(desc1,4));
                    for(unsigned i=0;i<8;++i){
                        const auto in=8+i*40,out=8+i*32;
                        if(word(desc1,in+4))throw std::invalid_argument("不支援 D3D11.1 邏輯運算混合");
                        put_word(desc,out,word(desc1,in));for(unsigned j=0;j<6;++j)put_word(desc,out+4+j*4,word(desc1,in+8+j*4));desc[out+28]=desc1[in+36];
                    }
                    adapt(1,temporary(desc));
                }
                symbol="CreateBlendState";
            }else if(symbol=="CreateRasterizerState1" || symbol=="CreateRasterizerState2"){
                // DESC1 多 ForcedSampleCount、DESC2 再多 ConservativeRaster；兩者只接受 0（未啟用）。
                if(arg(1)){
                    const bool second=symbol=="CreateRasterizerState2";const auto desc=read_bytes(memory,arg(1),second?48:44);
                    if(word(desc,40))throw std::invalid_argument("不支援 ForcedSampleCount");
                    if(second && word(desc,44))throw std::invalid_argument("不支援保守光柵化");
                    adapt(1,temporary(std::span(desc).first(40)));
                }
                symbol="CreateRasterizerState";
            }else if(symbol=="CreateTexture2D1" || symbol=="CreateTexture3D1"){
                const bool planar=symbol=="CreateTexture2D1";const unsigned size=planar?44:36;
                if(arg(1)){const auto desc=read_bytes(memory,arg(1),size+4);if(word(desc,size))throw std::invalid_argument("只支援 D3D11_TEXTURE_LAYOUT_UNDEFINED");adapt(1,temporary(std::span(desc).first(size)));}
                symbol=planar?"CreateTexture2D":"CreateTexture3D";
            }else if(symbol=="CreateShaderResourceView1" || symbol=="CreateRenderTargetView1" || symbol=="CreateUnorderedAccessView1"){
                // *_DESC1 只在 TEXTURE2D／TEXTURE2DARRAY 多 PlaneSlice；只接受 0，其餘維度與 11.0 位置相同。
                const bool srv=symbol=="CreateShaderResourceView1";const unsigned size=srv?24:20;
                if(arg(2)){
                    auto desc=read_bytes(memory,arg(2),srv?28:24);const auto dimension=word(desc,4);
                    if(dimension==4){const unsigned plane=srv?16:12;plane_free(desc,plane);std::fill(desc.begin()+plane,desc.end(),0);}
                    else if(dimension==5)plane_free(desc,srv?24:20);
                    desc.resize(size);adapt(2,temporary(desc));
                }
                symbol=srv?"CreateShaderResourceView":symbol=="CreateRenderTargetView1"?"CreateRenderTargetView":"CreateUnorderedAccessView";
            }else if(symbol=="CreateQuery1"){
                // D3D11_QUERY_DESC1 多 ContextType（ALL、3D、COMPUTE、COPY、VIDEO）；單一命令佇列，類型只保存供 GetDesc1。
                std::uint32_t context_type=0;
                if(arg(1)){const auto desc=read_bytes(memory,arg(1),12);context_type=word(desc,8);if(context_type>4)throw std::invalid_argument("無效的查詢 ContextType");adapt(1,temporary(std::span(desc).first(8)));}
                const auto output_address=original_arg(2);
                finish_adapter=[&,context_type,output_address]{if(output_address)if(const auto created=memory.load(GuestAddress{output_address},pointer_bits))object(created).identity->query_context=context_type;};
                symbol="CreateQuery";
            }else if(symbol=="CheckMultisampleQualityLevels1"){
                // 只有 TILED_RESOURCE 旗標；不支援 tiled resources，因此回 0 個品質等級。
                if(u32(3)&~1U)throw std::invalid_argument("無效的多重取樣查詢旗標");
                if(!u32(3)){adapt(3,original_arg(4));symbol="CheckMultisampleQualityLevels";}
            }else if(symbol=="CopySubresourceRegion1" || symbol=="UpdateSubresource1"){
                // D3D11_COPY_NO_OVERWRITE／DISCARD 是驅動提示；共用路徑本來就保留其餘內容。
                const bool copy=symbol=="CopySubresourceRegion1";if(u32(copy?9:7)&~3U)throw std::invalid_argument("無效的複製旗標");
                if(!copy)partial_constant_update=true;
                symbol=copy?"CopySubresourceRegion":"UpdateSubresource";
            }else if(symbol.size()==21 && symbol.ends_with("SetConstantBuffers1")){
                // FirstConstant／NumConstants 以 16 個常數為單位對齊（256 位元組），每個範圍最多 4096 個常數。
                const std::uint32_t start=u32(1),count=u32(2);const auto first=arg(4),number=arg(5);
                if(!first!=!number)throw std::invalid_argument("FirstConstant 與 NumConstants 必須同時提供");
                std::vector<std::uint32_t> firsts(count),numbers(count);
                if(first && count<=14){for(unsigned i=0;i<count;++i){firsts[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{first+i*4},32));numbers[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{number+i*4},32));
                    if((firsts[i]&15U) || (numbers[i]&15U) || numbers[i]>4096)throw std::invalid_argument("常數緩衝範圍未對齊或超過 4096 個常數");}}
                const auto context_address=original_arg(0);const auto stage=stage_index(symbol);
                finish_adapter=[&,context_address,stage,start,count,firsts,numbers]{
                    auto context=typed(context_address,Kind::Context);
                    for(unsigned i=0;i<count && start+i<14;++i){context->constant_first[stage][start+i]=firsts[i];context->constant_count[stage][start+i]=numbers[i];}
                };
                adapted_symbol=std::string(symbol.substr(0,20));symbol=adapted_symbol;
            }else if(symbol.size()==21 && symbol.ends_with("GetConstantBuffers1")){
                // 緩衝區沿用 11.0 取得（含 AddRef）；範圍未以 *SetConstantBuffers1 指定時回報整個緩衝（最多 4096 個常數）。
                const auto context_address=original_arg(0);const auto stage=stage_index(symbol);const std::uint32_t start=u32(1),count=u32(2);const auto first=arg(4),number=arg(5);
                if(first)memory.check(GuestAddress{first},std::size_t{count}*4,Access::Write);
                if(number)memory.check(GuestAddress{number},std::size_t{count}*4,Access::Write);
                finish_adapter=[&,context_address,stage,start,count,first,number]{
                    auto context=typed(context_address,Kind::Context);const auto found=context->bindings.find(stage_names[stage]+"ConstantBuffers");
                    for(unsigned i=0;i<count;++i){
                        const auto slot=start+i;const auto pointer=slot<14 && found!=context->bindings.end() && slot<found->second.size()?found->second[slot]:0;
                        std::uint32_t begin=0,constants=0;
                        if(pointer){begin=context->constant_first[stage][slot];constants=context->constant_count[stage][slot];
                            if(!constants)if(const auto& resource=object(pointer).identity->resource)constants=std::min<std::uint32_t>(4096,resource->desc.width/16);}
                        if(first)memory.store(GuestAddress{first+i*4},32,begin);
                        if(number)memory.store(GuestAddress{number+i*4},32,constants);
                    }
                };
                adapted_symbol=std::string(symbol.substr(0,20));symbol=adapted_symbol;
            }
        }
