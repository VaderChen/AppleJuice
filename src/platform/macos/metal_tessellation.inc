// VS／HS 由同一個 compute kernel 執行；DS 接到 Metal 固定功能細分器。
// 管線以實際 shader 與頂點 layout 建立，不依 EXE 或呼叫位址選擇行為。
MetalTessellationDraw MetalGl::graphics_tessellation(const GraphicsDraw& d,const std::string& key){
    if(!d.stages[3].shader || !d.stages[4].shader || d.stages[2].shader || d.topology<33 || d.topology>64)
        throw std::invalid_argument("細分繪製需要 patch topology、HS 與 DS");
    const unsigned input_count=d.topology-32,patch_count=d.count/input_count;
    if(!patch_count)return {};
    auto found=tessellation_pipelines_.find(key);
    if(found==tessellation_pipelines_.end()){
        auto p=std::make_shared<MetalTessellationPipeline>();p->input_count=input_count;
        GLSLCrossDependencyData dependencies;
        std::string source;
        for(unsigned i=0;i<3;++i){const unsigned stage=i?i+2:0;
            p->stages[i]=std::dynamic_pointer_cast<MetalShader>(graphics_translate_shader(stage,dynamic_cast<MetalShader&>(*d.stages[stage].shader).bytecode,&dependencies));
            source+=p->stages[i]->source;
        }
        p->settings=p->stages[2]->tessellation;p->group_patches=p->settings[3];
        std::smatch match;
        if(!std::regex_search(source,match,std::regex(R"(\[\[patch\((quad|triangle),\s*(\d+)\)\]\])")))throw std::invalid_argument("DS 缺少 patch 宣告");
        p->control_count=static_cast<unsigned>(std::stoul(match[2]));p->factor_stride=match[1]=="quad"?12:8;
        if(!p->control_count || p->control_count>32 || !p->group_patches)throw std::invalid_argument("HS 控制點數量不合法");
        const auto slot=[&](const char* name){std::smatch m;if(!std::regex_search(source,m,std::regex(std::string(name)+R"(\s*\[\[\s*buffer\((\d+)\)\s*\]\])")))throw std::invalid_argument(std::string("細分 kernel 缺少參數：")+name);return static_cast<unsigned>(std::stoul(m[1]));};
        p->control_slot=dependencies.hasControlPoint?slot("controlPoints"):UINT32_MAX;
        p->patch_slot=dependencies.hasPatchConstant?slot("patchConstants"):UINT32_MAX;
        p->factor_slot=slot("tessFactors");p->info_slot=slot("patchInfo");
        std::set<unsigned> kernel_slots{p->factor_slot,p->info_slot};if(p->control_slot!=UINT32_MAX)kernel_slots.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)kernel_slots.insert(p->patch_slot);
        for(unsigned i=0;i<2;++i)for(const auto& b:p->stages[i]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9))kernel_slots.insert(b.native_slot);
        // VS／HS 共用 kernel，但 typed buffer 的 view 範圍各自獨立。
        for(unsigned i=0;i<2;++i)if(p->stages[i]->source.find("x64arm_buffer_views[")!=std::string::npos){
            unsigned at=30;while(kernel_slots.contains(at)){if(!at)throw std::invalid_argument("細分 buffer view 配額不足");--at;}
            kernel_slots.insert(at);p->stages[i]->buffer_views_slot=at;metal_buffer_shader(*p->stages[i],i?"patchKernel":"vertexFunction");
        }
        if(p->stages[0]->buffer_views_slot!=UINT32_MAX){
            metal_replace(p->stages[0]->source,"x64arm_buffer_views","x64arm_vertex_buffer_views");
            metal_replace(p->stages[1]->source,"vertexFunction(","vertexFunction(x64arm_vertex_buffer_views, ");
            const auto function=metal_function_text(p->stages[1]->source,"patchKernel");
            p->stages[1]->source.insert(function.begin,"constant uint4* x64arm_vertex_buffer_views [[buffer("+std::to_string(p->stages[0]->buffer_views_slot)+")]],\n");
        }
        metal_buffer_shader(*p->stages[2]);source.clear();for(const auto& stage:p->stages)source+=stage->source;
        p->vertex_id_slot=30;while(kernel_slots.contains(p->vertex_id_slot)){if(!p->vertex_id_slot)throw std::invalid_argument("細分系統輸入 binding 配額不足");--p->vertex_id_slot;}
        const auto kernel_function=metal_function_text(source,"patchKernel");
        source.insert(kernel_function.begin,"device const uint* x64VertexIds [[buffer("+std::to_string(p->vertex_id_slot)+")]],\n");
        metal_replace(source,"ushort numControlPointsPerPatch;","ushort numControlPointsPerPatch;\n    uint baseInstance;");
        metal_replace(source,"ushort2 groupID","uint2 groupID");
        metal_replace(source,"const uint mtl_InstanceID = groupID.y - mtl_BaseInstance;","const uint mtl_InstanceID = uint(groupID.y) + patchInfo.baseInstance;");
        // 內部 buffer 的索引不包含 DrawInstanced 的 base instance。
        metal_replace(source,"const uint internalPatchID = mtl_InstanceID * patchInfo.numPatches + patchID;","const uint internalPatchID = uint(groupID.y) * patchInfo.numPatches + patchID;");
        source=std::regex_replace(source,std::regex(R"(const uint mtl_VertexID = [^;]+;)"),"const uint mtl_VertexID = patchValid && controlPointID < "+std::to_string(input_count)+" ? x64VertexIds[tID.x] : 0u;");
        // 保存完整生成碼，只有明確要求診斷時才寫入檔案。
        if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){std::ofstream out(std::filesystem::path(folder)/("tess-"+std::to_string(p->stages[0]->serial)+".metal"));out<<source;}
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("Metal 細分著色器編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+source);
        p->domain=[library newFunctionWithName:@"xlatMtlMain"];
        auto descriptor=[MTLComputePipelineDescriptor new];descriptor.computeFunction=[library newFunctionWithName:@"patchKernel"];
        if(!descriptor.computeFunction || !p->domain)throw std::invalid_argument("Metal 細分管線缺少入口");
        auto inputs=[MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        std::set<unsigned> occupied{p->factor_slot,p->info_slot,p->vertex_id_slot};if(p->control_slot!=UINT32_MAX)occupied.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)occupied.insert(p->patch_slot);
        for(unsigned i=0;i<2;++i)for(const auto& binding:p->stages[i]->bindings)if(binding.type==0 || binding.type==3 || binding.type==4)occupied.insert(binding.native_slot);
        for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_views_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_views_slot);
        unsigned next=30;
        for(const auto& input:p->stages[0]->inputs){auto element=std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});
            if(element==d.layout.end() || element->slot>=32 || !d.vertices[element->slot].resource)throw std::invalid_argument("細分頂點 layout 缺少 semantic");
            while(occupied.contains(next)){if(!next)throw std::invalid_argument("細分頂點 binding 配額不足");--next;}
            const auto& e=*element;auto attribute=inputs.attributes[input.attribute];attribute.format=static_cast<MTLAttributeFormat>(metal_vertex_format(e.format));attribute.offset=e.offset;attribute.bufferIndex=next;
            auto layout=inputs.layouts[next];layout.stride=std::max(d.vertices[e.slot].stride,e.offset+graphics_format_bytes(e.format));layout.stepFunction=e.classification?MTLStepFunctionThreadPositionInGridY:MTLStepFunctionThreadPositionInGridX;layout.stepRate=1;
            occupied.insert(next);--next;
        }
        descriptor.stageInputDescriptor=inputs;
        MTLComputePipelineReflection* reflection=nil;
        p->compute=[device_ newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
        if(!p->compute)throw std::invalid_argument("Metal 細分 kernel 建立失敗："+std::string(error.localizedDescription.UTF8String));
        p->descriptor=[MTLVertexDescriptor vertexDescriptor];
        std::set<unsigned> domain_slots;for(const auto& b:p->stages[2]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9))domain_slots.insert(b.native_slot);
        if(p->stages[2]->buffer_views_slot!=UINT32_MAX)domain_slots.insert(p->stages[2]->buffer_views_slot);
        const auto domain_slot=[&](){unsigned at=30;while(domain_slots.contains(at)){if(!at)throw std::invalid_argument("DS 輸入 binding 配額不足");--at;}domain_slots.insert(at);return at;};
        p->domain_control_slot=domain_slot();p->domain_patch_slot=domain_slot();
        const auto output=[&](unsigned kernel_slot,const char* struct_name,unsigned vertex_slot,MTLVertexStepFunction step,unsigned& stride){
            if(kernel_slot==UINT32_MAX)return;
            MTLArgument* argument=nil;for(MTLArgument* a in reflection.arguments)if(a.type==MTLArgumentTypeBuffer && a.index==kernel_slot){argument=a;break;}
            if(!argument || !argument.bufferStructType || !argument.bufferDataSize)throw std::invalid_argument("Metal 細分輸出缺少結構反射");
            stride=static_cast<unsigned>(argument.bufferDataSize);
            std::smatch structure;if(!std::regex_search(source,structure,std::regex(std::string("struct ")+struct_name+R"(\s*\{([^}]+)\})")))throw std::invalid_argument("DS 缺少輸入結構");
            const auto body=structure[1].str();const std::regex field(R"((float|uint|int|half)([1-4]?)\s+(\w+)\s*\[\[\s*attribute\((\d+)\)\s*\]\])");
            for(auto it=std::sregex_iterator(body.begin(),body.end(),field);it!=std::sregex_iterator();++it){const auto& f=*it;auto member=[argument.bufferStructType memberByName:[NSString stringWithUTF8String:f[3].str().c_str()]];if(!member)throw std::invalid_argument("HS／DS 欄位反射不相符："+f[3].str());
                const unsigned count=f[2].str().empty()?1:static_cast<unsigned>(std::stoul(f[2]));const unsigned index=static_cast<unsigned>(std::stoul(f[4]));
                (step==MTLVertexStepFunctionPerPatchControlPoint?p->control_fields:p->patch_fields)[f[3].str()]={static_cast<unsigned>(member.offset),count};
                const auto base=f[1]=="float"?MTLVertexFormatFloat:f[1]=="uint"?MTLVertexFormatUInt:f[1]=="int"?MTLVertexFormatInt:MTLVertexFormatHalf;
                // scalar 與 vector 格式在 Metal enum 中不連續。
                const auto format=f[1]=="float"?(count==1?MTLVertexFormatFloat:static_cast<MTLVertexFormat>(MTLVertexFormatFloat2+count-2)):f[1]=="uint"?(count==1?MTLVertexFormatUInt:static_cast<MTLVertexFormat>(MTLVertexFormatUInt2+count-2)):f[1]=="int"?(count==1?MTLVertexFormatInt:static_cast<MTLVertexFormat>(MTLVertexFormatInt2+count-2)):(count==1?base:static_cast<MTLVertexFormat>(MTLVertexFormatHalf2+count-2));
                p->descriptor.attributes[index].format=format;p->descriptor.attributes[index].offset=member.offset;p->descriptor.attributes[index].bufferIndex=vertex_slot;
            }
            p->descriptor.layouts[vertex_slot].stride=stride;p->descriptor.layouts[vertex_slot].stepFunction=step;p->descriptor.layouts[vertex_slot].stepRate=1;
        };
        output(p->control_slot,"Mtl_ControlPointIn",p->domain_control_slot,MTLVertexStepFunctionPerPatchControlPoint,p->control_stride);
        output(p->patch_slot,"Mtl_PatchConstantIn",p->domain_patch_slot,MTLVertexStepFunctionPerPatch,p->patch_stride);
        if(tessellation_pipelines_.size()>128)tessellation_pipelines_.clear();found=tessellation_pipelines_.emplace(key,std::move(p)).first;
    }
    auto p=found->second;
    flush(true);
    // 每次繪製獨立配置輸出；command buffer 持有 Metal buffer，避免下一次繪製覆寫。
    std::vector<std::shared_ptr<GraphicsResource>> storage;
    const auto allocate=[&](std::uint64_t size){if(size>512U*1024U*1024U)throw std::invalid_argument("細分輸出超出限制");GraphicsResourceDesc desc;desc.width=static_cast<unsigned>(std::max<std::uint64_t>(256,size));auto resource=graphics_resource(desc);auto b=metal_resource(resource).buffer;storage.push_back(resource);graphics_pending_.push_back(std::move(resource));return b;};
    const std::uint64_t total=std::uint64_t{patch_count}*d.instances;
    MetalTessellationDraw result{p,allocate(total*p->control_count*p->control_stride),allocate(total*p->patch_stride),allocate(total*p->factor_stride),patch_count};
    // CPU gather 只重排輸入資料；所有 VS／HS／DS 運算仍由 GPU 執行。
    // 讀取前等待先前 GPU 寫入，保留索引、base vertex、instance step rate 與零 stride。
    const unsigned fetch_count=patch_count*std::max(p->input_count,p->control_count);
    auto vertex_ids=allocate(std::uint64_t{fetch_count}*4);auto* ids=static_cast<std::uint32_t*>(vertex_ids.contents);
    for(unsigned i=0;i<fetch_count;++i){if(i%std::max(p->input_count,p->control_count)>=p->input_count)continue;
        std::uint64_t index=std::uint64_t{i/std::max(p->input_count,p->control_count)}*p->input_count+i%std::max(p->input_count,p->control_count)+d.start;
        if(d.indexed){const unsigned size=d.index_format==57?2:4;auto& indices=metal_resource(d.indices);const auto at=std::uint64_t{d.index_offset}+index*size;if(at+size>indices.desc.width)throw std::invalid_argument("細分索引 buffer 越界");std::uint32_t value=0;std::memcpy(&value,static_cast<const char*>(indices.buffer.contents)+at,size);const auto adjusted=std::int64_t{value}+d.base_vertex;if(adjusted<0 || adjusted>UINT32_MAX)throw std::invalid_argument("細分頂點索引超出範圍");index=static_cast<std::uint64_t>(adjusted);}
        if(index>UINT32_MAX)throw std::invalid_argument("細分頂點索引超出範圍");ids[i]=static_cast<std::uint32_t>(index);
    }
    std::vector<id<MTLBuffer>> gathered;
    for(const auto& input:p->stages[0]->inputs){const auto it=std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});const auto& e=*it;const auto& v=d.vertices[e.slot];auto& resource=metal_resource(v.resource);
        const auto stride=std::max(v.stride,e.offset+graphics_format_bytes(e.format));const unsigned count=e.classification?d.instances:fetch_count;
        auto destination=allocate(std::uint64_t{stride}*count);
        for(unsigned i=0;i<count;++i){const std::uint64_t index=e.classification?std::uint64_t{d.start_instance}+i/std::max(1U,e.step):ids[i];
            if(!e.classification && i%std::max(p->input_count,p->control_count)>=p->input_count)continue;
            const auto at=std::uint64_t{v.offset}+index*v.stride+e.offset;const auto size=graphics_format_bytes(e.format);if(at+size>resource.desc.width)throw std::invalid_argument("細分頂點 buffer 越界");std::memcpy(static_cast<char*>(destination.contents)+std::uint64_t{i}*stride+e.offset,static_cast<const char*>(resource.buffer.contents)+at,size);
        }
        gathered.push_back(destination);
    }
    begin_commands();auto encoder=[commands_ computeCommandEncoder];MetalEncoderScope scope{encoder};[encoder setComputePipelineState:p->compute];
    std::set<unsigned> occupied{p->factor_slot,p->info_slot,p->vertex_id_slot};if(p->control_slot!=UINT32_MAX)occupied.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)occupied.insert(p->patch_slot);
    for(unsigned i=0;i<2;++i){auto stage=d.stages[i?3:0];stage.shader=p->stages[i];graphics_bind(nil,encoder,stage,false);for(const auto& b:stage.shader->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9))occupied.insert(b.native_slot);}
    for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_views_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_views_slot);
    unsigned next=30;for(auto buffer:gathered){while(occupied.contains(next))--next;[encoder setBuffer:buffer offset:0 atIndex:next];occupied.insert(next);--next;}
    if(p->control_slot!=UINT32_MAX)[encoder setBuffer:result.controls offset:0 atIndex:p->control_slot];if(p->patch_slot!=UINT32_MAX)[encoder setBuffer:result.patches offset:0 atIndex:p->patch_slot];[encoder setBuffer:result.factors offset:0 atIndex:p->factor_slot];
    [encoder setBuffer:vertex_ids offset:0 atIndex:p->vertex_id_slot];
    const std::array<std::uint32_t,3> info{patch_count,std::max(p->input_count,p->control_count),d.start_instance};[encoder setBytes:info.data() length:sizeof(info) atIndex:p->info_slot];
    const auto threads=p->group_patches*std::max(p->input_count,p->control_count);
    [encoder setStageInRegion:MTLRegionMake2D(0,0,patch_count*std::max(p->input_count,p->control_count),d.instances)];
    [encoder dispatchThreadgroups:MTLSizeMake((patch_count+p->group_patches-1)/p->group_patches,d.instances,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];scope.end();
    if(!p->captured){if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){
        flush(true);graphics_pending_.insert(graphics_pending_.end(),storage.begin(),storage.end());const auto base=std::filesystem::path(folder)/("tess-"+std::to_string(p->stages[0]->serial));
        const auto save=[&](const char* suffix,id<MTLBuffer> buffer,std::uint64_t size){std::ofstream file(base.string()+suffix,std::ios::binary);file.write(static_cast<const char*>(buffer.contents),static_cast<std::streamsize>(size));if(!file)throw std::runtime_error("無法保存細分診斷 buffer");};
        save("-control.bin",result.controls,total*p->control_count*p->control_stride);save("-patch.bin",result.patches,total*p->patch_stride);save("-factor.bin",result.factors,total*p->factor_stride);
        for(unsigned i=0;i<gathered.size();++i)save(("-input"+std::to_string(i)+".bin").c_str(),gathered[i],gathered[i].length);
        for(unsigned stage:{0U,3U,4U})for(unsigned i=0;i<14;++i)if(d.stages[stage].constants[i]){auto& r=metal_resource(d.stages[stage].constants[i]);save(("-stage"+std::to_string(stage)+"-cb"+std::to_string(i)+".bin").c_str(),r.buffer,r.desc.width);}
        for(unsigned stage:{0U,3U,4U})for(unsigned i=0;i<d.stages[stage].resources.size();++i){const auto& view=d.stages[stage].resources[i];if(view.resource && view.resource->desc.dimension==1){auto& r=metal_resource(view.resource);save(("-stage"+std::to_string(stage)+"-srv"+std::to_string(i)+".bin").c_str(),r.buffer,r.desc.width);}}
        std::ofstream meta(base.string()+".json");meta<<"{\"patch_count\":"<<patch_count<<",\"instances\":"<<d.instances<<",\"control_count\":"<<p->control_count<<",\"control_stride\":"<<p->control_stride<<",\"patch_stride\":"<<p->patch_stride<<",\"factor_stride\":"<<p->factor_stride;
        const auto fields=[&](const char* name,const auto& entries){meta<<",\""<<name<<"\":{";bool first=true;for(const auto& [field,layout_info]:entries){if(!first)meta<<',';first=false;meta<<'"'<<field<<"\":["<<layout_info[0]<<','<<layout_info[1]<<']';}meta<<'}';};
        fields("control_fields",p->control_fields);fields("patch_fields",p->patch_fields);meta<<"}\n";p->captured=true;
    }}
    return result;
}
