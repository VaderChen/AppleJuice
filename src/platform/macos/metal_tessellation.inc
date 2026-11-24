// 只有讀 SV_TessFactor／SV_InsideTessFactor 的域著色器需要額外的 float 資料。
// 固定功能細分器仍收到 Metal 規定的 half，兩者使用同一個 patch 順序。
void metal_tessellation_factor_inputs(std::string& hull,std::string& domain,bool quad,unsigned kernel_slot,unsigned domain_slot){
    const std::string hardware=quad?"MTLQuadTessellationFactorsHalf":"MTLTriangleTessellationFactorsHalf";
    const auto kernel=hull.find("kernel void patchKernel(");
    const std::string publish="if (controlPointID == 0) tessFactors[internalPatchID] = tessFactor;";
    if(kernel==hull.npos || hull.find(hardware+" tessFactor;")==hull.npos || hull.find(publish)==hull.npos)
        throw std::invalid_argument("細分係數輸出的 Metal 版面不符");
    hull.insert(kernel,quad?
        "struct X64ARMTessFactors { float edgeTessellationFactor[4]; float insideTessellationFactor[2]; };\n":
        "struct X64ARMTessFactors { float edgeTessellationFactor[3]; float insideTessellationFactor; };\n");
    metal_replace(hull,hardware+" tessFactor;","X64ARMTessFactors tessFactor = {};");
    std::string stores="if (controlPointID == 0) {\n        "+hardware+" converted;\n";
    stores+="        for (uint i = 0; i < "+std::to_string(quad?4:3)+"; ++i) converted.edgeTessellationFactor[i] = half(tessFactor.edgeTessellationFactor[i]);\n";
    stores+=quad?"        for (uint i = 0; i < 2; ++i) converted.insideTessellationFactor[i] = half(tessFactor.insideTessellationFactor[i]);\n":
                 "        converted.insideTessellationFactor = half(tessFactor.insideTessellationFactor);\n";
    stores+="        tessFactors[internalPatchID] = converted;\n        x64armRawTessFactors[internalPatchID] = tessFactor;\n    }";
    metal_replace(hull,publish,stores);
    const auto hull_parameters=metal_function_text(hull,"patchKernel");
    hull.insert(hull_parameters.begin,"device X64ARMTessFactors* x64armRawTessFactors [[buffer("+std::to_string(kernel_slot)+")]],\n");

    const auto parameters=metal_function_text(domain,"xlatMtlMain");
    const bool has_id=std::ranges::find(parameters.names,"mtl_PrimitiveID")!=parameters.names.end();
    const auto body=domain.find('{',parameters.end);
    if(body==domain.npos)throw std::invalid_argument("域著色器缺少函式主體");
    domain.insert(body+1,std::string("\n    const auto tessFactor = x64armRawTessFactors[")+(has_id?"mtl_PrimitiveID":"x64armPatchID")+"];");
    auto arguments="device const X64ARMTessFactors* x64armRawTessFactors [[buffer("+std::to_string(domain_slot)+")]],\n";
    if(!has_id)arguments+="uint x64armPatchID [[patch_id]],\n";
    domain.insert(parameters.begin,arguments);
}

// VS／HS 由同一個 compute kernel 執行；DS 接到 Metal 固定功能細分器。
// 管線以實際 shader 與頂點 layout 建立，不依 EXE 或呼叫位址選擇行為。
MetalTessellationDraw MetalGl::graphics_tessellation(const GraphicsDraw& d,const std::string& base_key){
    if(!d.stages[3].shader || !d.stages[4].shader || d.stages[2].shader || d.topology<33 || d.topology>64)
        throw std::invalid_argument("細分繪製需要 patch topology、HS 與 DS");
    const unsigned input_count=d.topology-32,patch_count=d.count/input_count;
    if(!patch_count)return {};
    // 逐頂點輸入優先讓 GPU 直接讀原始緩衝：索引走 stage-in 的索引步進，base vertex／起始頂點併入 binding offset，不經 CPU 收集與暫存複製。
    // stride 不符 Metal 要求（0、非 4 倍數、元素超出 stride）或 offset 為負時才退回 CPU 收集。X64ARM_METAL_TESS_DIRECT=0 關閉。
    static const bool direct_enabled=[] {const auto* value=std::getenv("X64ARM_METAL_TESS_DIRECT");return !value || std::string_view(value)!="0";}();
    const auto element_of=[&](const auto& input){return std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});};
    const auto direct_offset=[&](const GraphicsVertexBuffer& v){return std::int64_t{v.offset}+std::int64_t{v.stride}*(d.indexed?std::int64_t{d.base_vertex}:std::int64_t{d.start});};
    // 逐實例輸入同樣由 GPU 直接讀：stage-in 以執行緒格子 Y（實例）步進，步進率即 InstanceDataStepRate，0 為常數；起始實例併入 binding offset。
    const auto instance_offset=[&](const GraphicsVertexBuffer& v){return std::uint64_t{v.offset}+std::uint64_t{v.stride}*d.start_instance;};
    bool direct=direct_enabled;std::string steps;
    for(const auto& input:d.stages[0].shader->inputs){const auto it=element_of(input);if(it==d.layout.end() || it->slot>=32)continue;const auto& v=d.vertices[it->slot];
        if(it->classification){
            if(it->step && (!v.stride || v.stride%4 || it->offset+graphics_format_bytes(it->format)>v.stride))direct=false;
            if(instance_offset(v)%4 || it->offset%4)direct=false;
            steps+=":i"+std::to_string(it->step);continue;
        }
        if(!v.stride || v.stride%4 || it->offset+graphics_format_bytes(it->format)>v.stride || direct_offset(v)<0 || direct_offset(v)%4)direct=false;}
    // 細分管線（VS／HS 合成的 compute kernel 與 DS 頂點函式）只取決於三個著色器、patch topology、頂點 layout 與 stride、
    // 讀取方式；PS、渲染目標、混色等渲染管線狀態不影響它（那些另由渲染管線快取區分）。鍵只放這些，避免同一組細分程式重複編譯。
    (void)base_key;
    std::ostringstream tess_key;tess_key<<dynamic_cast<MetalShader&>(*d.stages[0].shader).serial<<':'<<dynamic_cast<MetalShader&>(*d.stages[3].shader).serial<<':'<<dynamic_cast<MetalShader&>(*d.stages[4].shader).serial<<':'<<d.topology;
    for(const auto& input:d.stages[0].shader->inputs){const auto it=element_of(input);if(it==d.layout.end())continue;
        tess_key<<'|'<<it->semantic<<it->index<<':'<<it->format<<':'<<it->offset<<':'<<it->slot<<':'<<it->classification<<':'<<it->step<<':'<<(it->slot<32?d.vertices[it->slot].stride:0U);}
    const std::string key=tess_key.str()+(!direct?":gather":(!d.indexed?":direct":d.index_format==57?":direct16":":direct32")+steps);
    auto found=tessellation_pipelines_.find(key);
    // 診斷：X64ARM_TRACE_TESS=1 時列出每次建立細分管線的鍵與結果（上限 200 筆）。
    static const bool trace_tess=std::getenv("X64ARM_TRACE_TESS")!=nullptr;static std::atomic<unsigned> traced{0};
    if(found==tessellation_pipelines_.end() && trace_tess && traced.fetch_add(1)<200)std::fprintf(stderr,"[tess] 建立 key=%s 快取=%zu\n",key.c_str(),tessellation_pipelines_.size());
    // 建立失敗的管線記住錯誤：之後同一組合直接回報，不在每次繪製重新翻譯與編譯。
    if(const auto failed=tessellation_failures_.find(key);failed!=tessellation_failures_.end())throw std::invalid_argument(failed->second);
    if(found==tessellation_pipelines_.end())try{
        auto p=std::make_shared<MetalTessellationPipeline>();p->input_count=input_count;
        GLSLCrossDependencyData dependencies;
        std::string source;
        for(unsigned i=0;i<3;++i){const unsigned stage=i?i+2:0;
            p->stages[i]=std::dynamic_pointer_cast<MetalShader>(graphics_translate_shader(stage,dynamic_cast<MetalShader&>(*d.stages[stage].shader).bytecode,&dependencies));
        }
        // patch 常數中的細分係數是純量成員，DXBC 運算元仍帶 .x／.xx 等 swizzle（MSL 不允許）：只有 x 的 swizzle 改成純量或建構向量。
        {std::set<std::string> scalars;const std::regex member(R"(\b(?:float|half|int|uint)\s+(\w+)\s*\[\[\s*attribute\(\d+\)\s*\]\])");
            for(const auto& stage:p->stages){const auto at=stage->source.find("struct Mtl_PatchConstantIn");if(at==std::string::npos)continue;const auto end=stage->source.find('}',at);
                const auto body=stage->source.substr(at,end==std::string::npos?std::string::npos:end-at);for(auto it=std::sregex_iterator(body.begin(),body.end(),member);it!=std::sregex_iterator();++it)scalars.insert((*it)[1].str());}
            for(auto& stage:p->stages)for(const auto& name:scalars){
                const std::regex use("((?:[A-Za-z_][A-Za-z0-9_]*\\.)*patch\\."+name+")\\.(x{1,4})\\b");std::string out;std::size_t last=0;
                for(auto it=std::sregex_iterator(stage->source.begin(),stage->source.end(),use);it!=std::sregex_iterator();++it){
                    const auto& m=*it;out.append(stage->source,last,static_cast<std::size_t>(m.position())-last);const auto count=m[2].length();
                    out+=count==1?m[1].str():"float"+std::to_string(count)+"("+m[1].str()+")";last=static_cast<std::size_t>(m.position()+m.length());}
                if(last){out.append(stage->source,last,std::string::npos);stage->source=std::move(out);}
            }
        }
        for(const auto& stage:p->stages)source+=stage->source;
        p->settings=p->stages[2]->tessellation;p->group_patches=p->settings[3];
        std::smatch match;
        if(!std::regex_search(source,match,std::regex(R"(\[\[patch\((quad|triangle),\s*(\d+)\)\]\])")))throw std::invalid_argument("DS 缺少 patch 宣告");
        p->control_count=static_cast<unsigned>(std::stoul(match[2]));p->factor_stride=match[1]=="quad"?12:8;
        if(!p->control_count || p->control_count>32 || !p->group_patches)throw std::invalid_argument("HS 控制點數量不合法");
        // 控制點多於輸入點時，執行緒格子每個 patch 有補位，不能直接對應索引位置。
        p->direct=direct && p->control_count<=input_count;p->uses_vertex_id=p->stages[0]->source.find("mtl_VertexID")!=std::string::npos;
        const auto slot=[&](const char* name){std::smatch m;if(!std::regex_search(source,m,std::regex(std::string(name)+R"(\s*\[\[\s*buffer\((\d+)\)\s*\]\])")))throw std::invalid_argument(std::string("細分 kernel 缺少參數：")+name);return static_cast<unsigned>(std::stoul(m[1]));};
        p->control_slot=dependencies.hasControlPoint?slot("controlPoints"):UINT32_MAX;
        p->patch_slot=dependencies.hasPatchConstant?slot("patchConstants"):UINT32_MAX;
        p->factor_slot=slot("tessFactors");p->info_slot=slot("patchInfo");
        std::set<unsigned> kernel_slots{p->factor_slot,p->info_slot};if(p->control_slot!=UINT32_MAX)kernel_slots.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)kernel_slots.insert(p->patch_slot);
        for(unsigned i=0;i<2;++i)for(const auto& b:p->stages[i]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=14))kernel_slots.insert(b.native_slot);
        for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_sizes_slot!=UINT32_MAX)kernel_slots.insert(p->stages[i]->buffer_sizes_slot);
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
        metal_buffer_shader(*p->stages[2]);
        const auto reserve_slot=[](std::set<unsigned>& slots){
            unsigned at=30;while(slots.contains(at)){if(!at)throw std::invalid_argument("細分輸入 binding 配額不足");--at;}
            slots.insert(at);return at;
        };
        p->vertex_id_slot=reserve_slot(kernel_slots);
        std::set<unsigned> domain_slots;
        for(const auto& b:p->stages[2]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=14))domain_slots.insert(b.native_slot);
        if(p->stages[2]->buffer_views_slot!=UINT32_MAX)domain_slots.insert(p->stages[2]->buffer_views_slot);
        if(p->stages[2]->buffer_sizes_slot!=UINT32_MAX)domain_slots.insert(p->stages[2]->buffer_sizes_slot);
        p->domain_control_slot=reserve_slot(domain_slots);p->domain_patch_slot=reserve_slot(domain_slots);
        auto& domain_source=p->stages[2]->source;
        if(domain_source.find("tessFactor.")!=domain_source.npos){
            const bool quad=p->factor_stride==12;
            p->raw_factor_slot=reserve_slot(kernel_slots);p->domain_raw_factor_slot=reserve_slot(domain_slots);
            p->raw_factor_stride=quad?24:16;
            metal_tessellation_factor_inputs(p->stages[1]->source,domain_source,quad,p->raw_factor_slot,p->domain_raw_factor_slot);
        }
        source.clear();for(const auto& stage:p->stages)source+=stage->source;
        const auto kernel_function=metal_function_text(source,"patchKernel");
        source.insert(kernel_function.begin,"device const uint* x64VertexIds [[buffer("+std::to_string(p->vertex_id_slot)+")]],\n");
        metal_replace(source,"ushort numControlPointsPerPatch;","ushort numControlPointsPerPatch;\n    uint baseInstance;");
        metal_replace(source,"ushort2 groupID","uint2 groupID");
        metal_replace(source,"const uint mtl_InstanceID = groupID.y - mtl_BaseInstance;","const uint mtl_InstanceID = uint(groupID.y);"); // SV_InstanceID 不含 StartInstanceLocation。
        // 內部 buffer 的索引不包含 DrawInstanced 的 base instance。
        metal_replace(source,"const uint internalPatchID = mtl_InstanceID * patchInfo.numPatches + patchID;","const uint internalPatchID = uint(groupID.y) * patchInfo.numPatches + patchID;");
        source=std::regex_replace(source,std::regex(R"(const uint mtl_VertexID = [^;]+;)"),p->uses_vertex_id?"const uint mtl_VertexID = patchValid && controlPointID < "+std::to_string(input_count)+" ? x64VertexIds[tID.x] : 0u;":std::string("const uint mtl_VertexID = 0u;"));
        // 保存完整生成碼，只有明確要求診斷時才寫入檔案。
        if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){std::ofstream out(std::filesystem::path(folder)/("tess-"+std::to_string(p->stages[0]->serial)+".metal"));out<<source;}
        NSError* error=nil;auto options=metal_d3d_compile_options();
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("Metal 細分著色器編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+source);
        p->domain=[library newFunctionWithName:@"xlatMtlMain"];
        auto descriptor=[MTLComputePipelineDescriptor new];descriptor.computeFunction=[library newFunctionWithName:@"patchKernel"];
        if(!descriptor.computeFunction || !p->domain)throw std::invalid_argument("Metal 細分管線缺少入口");
        auto inputs=[MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        std::set<unsigned> occupied{p->factor_slot,p->info_slot,p->vertex_id_slot};if(p->control_slot!=UINT32_MAX)occupied.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)occupied.insert(p->patch_slot);
        if(p->raw_factor_slot!=UINT32_MAX)occupied.insert(p->raw_factor_slot);
        for(unsigned i=0;i<2;++i)for(const auto& binding:p->stages[i]->bindings)if(binding.type==0 || binding.type==3 || binding.type==4)occupied.insert(binding.native_slot);
        for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_views_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_views_slot);
        for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_sizes_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_sizes_slot);
        unsigned next=30;
        for(const auto& input:p->stages[0]->inputs){auto element=std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});
            if(element==d.layout.end() || element->slot>=32 || !d.vertices[element->slot].resource)throw std::invalid_argument("細分頂點 layout 缺少 semantic");
            while(occupied.contains(next)){if(!next)throw std::invalid_argument("細分頂點 binding 配額不足");--next;}
            const auto& e=*element;auto attribute=inputs.attributes[input.attribute];attribute.format=static_cast<MTLAttributeFormat>(metal_vertex_format(e.format));attribute.offset=e.offset;attribute.bufferIndex=next;
            auto layout=inputs.layouts[next];const bool direct_input=p->direct;const bool constant=direct_input && e.classification && !e.step;
            layout.stride=constant?(e.offset+graphics_format_bytes(e.format)+3U)/4U*4U:direct_input?d.vertices[e.slot].stride:std::max(d.vertices[e.slot].stride,e.offset+graphics_format_bytes(e.format));
            layout.stepFunction=constant?MTLStepFunctionConstant:e.classification?MTLStepFunctionThreadPositionInGridY:direct_input && d.indexed?MTLStepFunctionThreadPositionInGridXIndexed:MTLStepFunctionThreadPositionInGridX;
            layout.stepRate=constant?0:direct_input && e.classification?e.step:1;
            occupied.insert(next);--next;
        }
        if(p->direct && d.indexed){
            while(occupied.contains(next)){if(!next)throw std::invalid_argument("細分索引 binding 配額不足");--next;}
            p->index_slot=next;inputs.indexBufferIndex=next;inputs.indexType=d.index_format==57?MTLIndexTypeUInt16:MTLIndexTypeUInt32;
        }
        descriptor.stageInputDescriptor=inputs;
        MTLComputePipelineReflection* reflection=nil;
        p->compute=[device_ newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
        if(!p->compute)throw std::invalid_argument("Metal 細分 kernel 建立失敗："+std::string(error.localizedDescription.UTF8String));
        p->descriptor=[MTLVertexDescriptor vertexDescriptor];
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
        // 上限內不淘汰；超過時只移除一筆（不整批清空，否則所有細分程式都要重新編譯）。
        if(tessellation_pipelines_.size()>=1024)tessellation_pipelines_.erase(tessellation_pipelines_.begin());found=tessellation_pipelines_.emplace(key,std::move(p)).first;
    }catch(const std::exception& error){if(tessellation_failures_.size()<1024)tessellation_failures_.emplace(key,error.what());throw;}
    auto p=found->second;
    // CPU 只讀必要的資料（逐實例輸入、SV_VertexID 用的索引、退回收集時的逐頂點輸入）；這些緩衝可能有未完成的 GPU 寫入時才等全部 GPU 工作，
    // 否則每個細分繪製都會把 GPU 排空（規則同幾何路徑的索引掃描）。X64ARM_METAL_TESS_NOWAIT=0 恢復每次等待。
    {static const bool nowait=[] {const auto* value=std::getenv("X64ARM_METAL_TESS_NOWAIT");return !value || std::string_view(value)!="0";}();
        const auto written=[](const MetalResource& resource){return resource.gpu_written || (resource.desc.bind&0x90) || !resource.buffer.contents;};
        const bool read_indices=d.indexed && (p->uses_vertex_id || !p->direct);bool wait=!nowait || (read_indices && written(metal_resource(d.indices)));
        for(const auto& input:p->stages[0]->inputs){const auto it=element_of(input);if(it!=d.layout.end() && it->slot<32 && d.vertices[it->slot].resource && !p->direct && written(metal_resource(d.vertices[it->slot].resource)))wait=true;}
        if(wait){flush(true);if(read_indices)metal_resource(d.indices).gpu_written=false;for(const auto& input:p->stages[0]->inputs){const auto it=element_of(input);if(it!=d.layout.end() && it->slot<32 && d.vertices[it->slot].resource && !p->direct)metal_resource(d.vertices[it->slot].resource).gpu_written=false;}}
    }
    // 每次繪製獨立的輸出與暫存：來自暫存池，持有者掛到批次的待釋放清單，在途期間不重用。診斷傾印要讀回內容，改用共享且不回池的緩衝。
    static const bool dump_buffers=[] {const auto* folder=std::getenv("X64ARM_DUMP_DXBC");return folder && *folder;}();
    std::vector<std::shared_ptr<GraphicsObject>> holders;
    const auto temporary=[&](std::uint64_t size,bool shared)->id<MTLBuffer> {
        if(size>512U*1024U*1024U)throw std::invalid_argument("細分輸出超出限制");
        pending_leases_.push_back(reserve_gpu_memory(std::max<std::uint64_t>(4096,size)));
        std::shared_ptr<MetalPooledBuffer> holder;
        if(dump_buffers){holder=std::make_shared<MetalPooledBuffer>();holder->buffer=[device_ newBufferWithLength:std::max<std::uint64_t>(256,size) options:MTLResourceStorageModeShared];}
        else holder=graphics_scratch_buffer(size,shared);
        if(!holder || !holder->buffer)throw std::bad_alloc{};
        holders.push_back(holder);graphics_pending_.push_back(holder);return holder->buffer;
    };
    const std::uint64_t total=std::uint64_t{patch_count}*d.instances;
    MetalTessellationDraw result{p,temporary(total*p->control_count*p->control_stride,false),temporary(total*p->patch_stride,false),
        temporary(total*p->factor_stride,false),p->raw_factor_stride?temporary(total*p->raw_factor_stride,false):nil,patch_count,nil,nil,nil,{}};
    // CPU 收集只重排輸入資料；所有 VS／HS／DS 運算仍由 GPU 執行。保留索引、base vertex、instance step rate 與零 stride。
    const unsigned points=std::max(p->input_count,p->control_count),fetch_count=patch_count*points;
    auto vertex_ids=temporary(p->uses_vertex_id?std::uint64_t{fetch_count}*4:256,true);auto* ids=p->uses_vertex_id?static_cast<std::uint32_t*>(vertex_ids.contents):nullptr;
    std::vector<std::uint32_t> fetch;if(!p->direct)fetch.resize(fetch_count);
    if(ids || !p->direct){
        const std::uint8_t* index_bytes=nullptr;const unsigned index_size=d.index_format==57?2:4;
        if(d.indexed){auto& indices=metal_resource(d.indices);index_bytes=static_cast<const std::uint8_t*>(indices.buffer.contents);if(std::uint64_t{d.index_offset}+(std::uint64_t{d.start}+d.count)*index_size>indices.desc.width)throw std::invalid_argument("細分索引 buffer 越界");index_bytes+=std::uint64_t{d.index_offset}+std::uint64_t{d.start}*index_size;}
        for(unsigned i=0;i<fetch_count;++i){const unsigned point=i%points;if(point>=p->input_count)continue;
            const std::uint64_t sequence=std::uint64_t{i/points}*p->input_count+point;
            // SV_VertexID 不含 base vertex 與起始頂點；讀取位置要含。
            std::uint64_t vertex_id=sequence,position=sequence+d.start;
            if(d.indexed){std::uint32_t value=0;std::memcpy(&value,index_bytes+sequence*index_size,index_size);vertex_id=value;const auto adjusted=std::int64_t{value}+d.base_vertex;if(adjusted<0 || adjusted>UINT32_MAX)throw std::invalid_argument("細分頂點索引超出範圍");position=static_cast<std::uint64_t>(adjusted);}
            if(position>UINT32_MAX)throw std::invalid_argument("細分頂點索引超出範圍");
            if(ids)ids[i]=static_cast<std::uint32_t>(vertex_id);if(!p->direct)fetch[i]=static_cast<std::uint32_t>(position);
        }
    }
    std::vector<std::pair<id<MTLBuffer>,std::uint64_t>> gathered;
    for(const auto& input:p->stages[0]->inputs){const auto it=element_of(input);const auto& e=*it;const auto& v=d.vertices[e.slot];auto& resource=metal_resource(v.resource);
        if(p->direct){gathered.emplace_back(resource.buffer,e.classification?instance_offset(v):static_cast<std::uint64_t>(direct_offset(v)));graphics_pending_.push_back(v.resource);continue;}
        const auto size=graphics_format_bytes(e.format);const auto stride=std::max(v.stride,e.offset+size);const unsigned count=e.classification?d.instances:fetch_count;
        auto destination=temporary(std::uint64_t{stride}*count,true);auto* out=static_cast<std::uint8_t*>(destination.contents);const auto* in=static_cast<const std::uint8_t*>(resource.buffer.contents);const auto width=resource.desc.width;
        for(unsigned i=0;i<count;++i){
            if(!e.classification && i%points>=p->input_count)continue;
            const std::uint64_t index=e.classification?std::uint64_t{d.start_instance}+(e.step?i/e.step:0U):fetch[i]; // 步進率 0：所有實例共用同一筆。
            const auto at=std::uint64_t{v.offset}+index*v.stride+e.offset;if(at+size>width)throw std::invalid_argument("細分頂點 buffer 越界");std::memcpy(out+std::uint64_t{i}*stride+e.offset,in+at,size);
        }
        gathered.emplace_back(destination,0);
    }
    // render encoder 開著、主命令緩衝還沒有繪製以外的 GPU 寫入，而且 VS／HS 沒有綁貼圖（貼圖可能是主緩衝稍早的繪製目標）時，
    // compute 編進側邊命令緩衝（在主緩衝之前提交），不結束 render encoder；其餘情況照舊先結束 encoder 再編在主緩衝。
    const auto samples_textures=[&](const GraphicsStage& stage){return std::any_of(stage.resources.begin(),stage.resources.end(),[](const auto& view){return bool(view.resource);});};
    static const bool dump_shaders=[] {const char* folder=std::getenv("X64ARM_DUMP_DXBC");return folder && *folder;}();
    const auto inputs_written=[&]{
        for(const unsigned stage:{0U,3U}){
            for(const auto& constant:d.stages[stage].constants)if(written_in_main(constant))return true;
            for(const auto& view:d.stages[stage].resources)if(view.resource && written_in_main(view.resource))return true;
        }
        for(const auto& vertex:d.vertices)if(written_in_main(vertex.resource))return true;
        return d.indexed && written_in_main(d.indices);
    };
    const unsigned refusal=!tessellation_coalesce()?5U:!(open_render_ && commands_)?0U:main_gpu_writes_?1U:inputs_written()?2U:
        (main_texture_writes_ && (samples_textures(d.stages[0]) || samples_textures(d.stages[3])))?3U:(encoder_chain_fence_ || dump_shaders || dump_buffers || (metal_gpu_profile() && !metal_gpu_profile_async()))?4U:6U;
    const bool side=refusal==6U;
    if(refusal<5U)++tess_main_[refusal];
    id<MTLComputeCommandEncoder> encoder=nil;
    if(side){if(!side_commands_)side_commands_=[queue_ commandBuffer];if(!side_commands_)throw std::runtime_error("Metal command buffer 建立失敗");encoder=[side_commands_ computeCommandEncoder];++side_computes_;}
    else{begin_commands(true,"tessellation");encoder=[commands_ computeCommandEncoder];}
    if(!encoder)throw std::runtime_error("Metal command encoder 建立失敗");
    MetalEncoderScope scope{encoder};[encoder setComputePipelineState:p->compute];
    std::set<unsigned> occupied{p->factor_slot,p->info_slot,p->vertex_id_slot};if(p->control_slot!=UINT32_MAX)occupied.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)occupied.insert(p->patch_slot);
    if(p->raw_factor_slot!=UINT32_MAX){occupied.insert(p->raw_factor_slot);[encoder setBuffer:result.raw_factors offset:0 atIndex:p->raw_factor_slot];}
    for(unsigned i=0;i<2;++i){auto stage=d.stages[i?3:0];stage.shader=p->stages[i];graphics_bind(nil,encoder,stage,false);for(const auto& b:stage.shader->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=14))occupied.insert(b.native_slot);}
    for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_views_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_views_slot);
        for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_sizes_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_sizes_slot);
    unsigned next=30;for(const auto& [buffer,offset]:gathered){while(occupied.contains(next))--next;[encoder setBuffer:buffer offset:offset atIndex:next];occupied.insert(next);--next;}
    if(p->direct && d.indexed){[encoder setBuffer:metal_resource(d.indices).buffer offset:std::uint64_t{d.index_offset}+std::uint64_t{d.start}*(d.index_format==57?2U:4U) atIndex:p->index_slot];graphics_pending_.push_back(d.indices);}
    if(p->control_slot!=UINT32_MAX)[encoder setBuffer:result.controls offset:0 atIndex:p->control_slot];if(p->patch_slot!=UINT32_MAX)[encoder setBuffer:result.patches offset:0 atIndex:p->patch_slot];[encoder setBuffer:result.factors offset:0 atIndex:p->factor_slot];
    [encoder setBuffer:vertex_ids offset:0 atIndex:p->vertex_id_slot];
    const std::array<std::uint32_t,3> info{patch_count,std::max(p->input_count,p->control_count),d.start_instance};[encoder setBytes:info.data() length:sizeof(info) atIndex:p->info_slot];
    const auto threads=p->group_patches*std::max(p->input_count,p->control_count);
    [encoder setStageInRegion:MTLRegionMake2D(0,0,patch_count*std::max(p->input_count,p->control_count),d.instances)];
    [encoder dispatchThreadgroups:MTLSizeMake((patch_count+p->group_patches-1)/p->group_patches,d.instances,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];scope.end();
    if(!p->captured){if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){
        flush(true);const auto base=std::filesystem::path(folder)/("tess-"+std::to_string(p->stages[0]->serial));
        const auto save=[&](const char* suffix,id<MTLBuffer> buffer,std::uint64_t size){std::ofstream file(base.string()+suffix,std::ios::binary);file.write(static_cast<const char*>(buffer.contents),static_cast<std::streamsize>(size));if(!file)throw std::runtime_error("無法保存細分診斷 buffer");};
        save("-control.bin",result.controls,total*p->control_count*p->control_stride);save("-patch.bin",result.patches,total*p->patch_stride);save("-factor.bin",result.factors,total*p->factor_stride);
        for(unsigned i=0;i<gathered.size();++i)save(("-input"+std::to_string(i)+".bin").c_str(),gathered[i].first,gathered[i].first.length);
        for(unsigned stage:{0U,3U,4U})for(unsigned i=0;i<14;++i)if(d.stages[stage].constants[i]){auto& r=metal_resource(d.stages[stage].constants[i]);save(("-stage"+std::to_string(stage)+"-cb"+std::to_string(i)+".bin").c_str(),r.buffer,r.desc.width);}
        for(unsigned stage:{0U,3U,4U})for(unsigned i=0;i<d.stages[stage].resources.size();++i){const auto& view=d.stages[stage].resources[i];if(view.resource && view.resource->desc.dimension==1){auto& r=metal_resource(view.resource);save(("-stage"+std::to_string(stage)+"-srv"+std::to_string(i)+".bin").c_str(),r.buffer,r.desc.width);}}
        std::ofstream meta(base.string()+".json");meta<<"{\"patch_count\":"<<patch_count<<",\"instances\":"<<d.instances<<",\"control_count\":"<<p->control_count<<",\"control_stride\":"<<p->control_stride<<",\"patch_stride\":"<<p->patch_stride<<",\"factor_stride\":"<<p->factor_stride;
        const auto fields=[&](const char* name,const auto& entries){meta<<",\""<<name<<"\":{";bool first=true;for(const auto& [field,layout_info]:entries){if(!first)meta<<',';first=false;meta<<'"'<<field<<"\":["<<layout_info[0]<<','<<layout_info[1]<<']';}meta<<'}';};
        fields("control_fields",p->control_fields);fields("patch_fields",p->patch_fields);meta<<"}\n";p->captured=true;
    }}
    result.holders=std::move(holders);return result;
}
// SPIRV-Cross 的 HS kernel 與 DS 頂點函式是否只以 gl_PrimitiveID 定址：去掉宣告與定址用法後若仍出現，表示著色器讀取 SV_PrimitiveID 的值
// （每個實例從 0 起算），不能把實例攤平成全域 patch 編號。
inline bool metal_tessellation_primitive_semantic(const std::string& source){
    static const std::regex addressing(R"(uint gl_PrimitiveID = min\(|uint gl_PrimitiveID \[\[\s*patch_id\s*\]\]|spvTessLevel\[gl_PrimitiveID\]|spvPatchIn\[gl_PrimitiveID\]|spvIn\[gl_PrimitiveID \* \d+\])");
    static const std::regex any(R"(\bgl_PrimitiveID\b)");
    const auto rest=std::regex_replace(source,addressing,"");
    return std::regex_search(rest,any);
}
// 現代路徑（SM5.1 DXBC 與 DXIL）的細分：三個階段依 graphics_modern_tessellation_link 的介面連結轉譯。
// VS 是擷取輸出的 compute kernel（頂點屬性走 stage-in，索引由 kernel 讀取），HS 是 tessellation control kernel（逐 patch 寫控制點、
// patch 常數與 half 細分係數），DS 是 post-tessellation vertex 函式，以 raw buffer 讀這三份資料。輸出排列與 HLSLcc 路徑相同
// （實例為主、每實例 count 個 patch），繪製端的 drawPatches 與綁定沿用同一套程式。
namespace {
// 等值線展開（D3D 的 isoline：SV_TessFactor[0] 為線數、[1] 為每條線的段數，第 i 條線 v=i/線數、u=j/段數）：
// 每個 patch 一條執行緒，以原子計數取得輸出位置，寫出線段兩端的 (patch, u, v)。段數依分割模式取整（integer／pow2 向上取整數或 2 的冪，
// fractional_odd／even 向上取奇數／偶數，近似為均分）；係數不大於 0 或 NaN 的 patch 依 D3D 規則剔除。超出容量的 patch 不輸出。
constexpr const char* metal_isoline_kernels=R"(#include <metal_stdlib>
using namespace metal;
struct AJIsolineParams {uint patches; uint capacity; uint spacing; uint pad;};
kernel void ajIsolineReset(device atomic_uint* counter [[buffer(0)]]) {atomic_store_explicit(counter, 0u, memory_order_relaxed);}
kernel void ajIsolineExpand(const device MTLQuadTessellationFactorsHalf* factors [[buffer(0)]], device uint4* vertices [[buffer(1)]],
                            device atomic_uint* counter [[buffer(2)]], constant AJIsolineParams& p [[buffer(3)]], uint patch [[thread_position_in_grid]])
{
    if (patch >= p.patches) return;
    const float lines_factor = float(factors[patch].edgeTessellationFactor[0]), detail_factor = float(factors[patch].edgeTessellationFactor[1]);
    if (!(lines_factor > 0.0f) || !(detail_factor > 0.0f)) return;
    const uint lines = clamp(uint(ceil(min(lines_factor, 64.0f))), 1u, 64u);
    uint detail = clamp(uint(ceil(min(detail_factor, 64.0f))), 1u, 64u);
    if (p.spacing == 3u) detail = min(64u, 1u << (32u - clz(max(detail, 1u) - 1u)));
    else if (p.spacing == 2u && (detail & 1u) == 0u) detail = min(63u, detail + 1u);
    else if (p.spacing == 1u && (detail & 1u) != 0u) detail = min(64u, detail + 1u);
    const uint count = lines * detail * 2u;
    const uint base = atomic_fetch_add_explicit(counter, count, memory_order_relaxed);
    if (base > p.capacity || count > p.capacity - base) return;
    uint at = base;
    for (uint line = 0; line < lines; ++line) {
        const uint v = as_type<uint>(float(line) / float(lines));
        for (uint segment = 0; segment < detail; ++segment) {
            vertices[at++] = uint4(patch, as_type<uint>(float(segment) / float(detail)), v, 0u);
            vertices[at++] = uint4(patch, as_type<uint>(float(segment + 1u) / float(detail)), v, 0u);
        }
    }
}
kernel void ajIsolineArguments(device atomic_uint* counter [[buffer(0)]], device MTLDrawPrimitivesIndirectArguments* arguments [[buffer(1)]], constant uint& capacity [[buffer(2)]])
{
    const uint total = atomic_load_explicit(counter, memory_order_relaxed);
    arguments->vertexCount = total > capacity ? capacity : total; arguments->instanceCount = 1u; arguments->vertexStart = 0u; arguments->baseInstance = 0u;
}
)";
// GPU 驅動的間接細分（ExecuteIndirect 的 DrawInstanced 參數由 GPU 寫入，常見於 GPU 剔除後的實例數）：不讀回參數，
// 由這個 kernel 轉成 VS／HS 的間接派送、VS 的 stage-in 區域（起點＝起始頂點／起始實例，與 CPU 路徑的緩衝位移等價）、
// HS 的 spvIndirectParams 與 drawPatches 間接參數。實例攤平成全域 patch（見 MetalTessellationPipeline::flat_instances）；
// 超過暫存容量的實例本次略過，需求量以原子最大值回饋，CPU 下次繪製前依此擴充容量。
// 輸出位移（位元組）：VS 群組 0、stage-in 區域 16、HS 群組 48、drawPatches 80、HS 參數 256、VS 參數 512。
constexpr const char* metal_tessellation_indirect_kernels=R"(#include <metal_stdlib>
using namespace metal;
struct AJTessIndirectParams {uint input_count; uint capacity; uint vs_group; uint hs_group; uint control_count; uint pad[3];};
kernel void ajTessIndirectArguments(const device uint* source [[buffer(0)]], device uint* out [[buffer(1)]], constant AJTessIndirectParams& p [[buffer(2)]],
                                    device atomic_uint* feedback [[buffer(3)]])
{
    const uint count = source[0], instances = source[1], start = source[2], start_instance = source[3];
    const uint patches = count / p.input_count;
    const ulong requested = ulong(patches) * ulong(instances);
    atomic_fetch_max_explicit(feedback, uint(min(requested, ulong(0xffffffffu))), memory_order_relaxed);
    const uint pc = min(patches, p.capacity);
    const uint inst = pc ? min(instances, p.capacity / pc) : 0u;
    const uint total = pc * inst, points = pc * p.input_count, threads = total * p.control_count;
    out[0] = total ? (points + p.vs_group - 1u) / p.vs_group : 0u; out[1] = total ? inst : 0u; out[2] = 1u;
    out[4] = start; out[5] = start_instance; out[6] = 0u; out[7] = points; out[8] = inst; out[9] = 1u;
    out[12] = (threads + p.hs_group - 1u) / p.hs_group; out[13] = 1u; out[14] = 1u;
    out[20] = total; out[21] = total ? 1u : 0u; out[22] = 0u; out[23] = start_instance;
    out[64] = p.input_count; out[65] = total;
    out[128] = points; out[129] = inst;
}
)";
// DS（post-tessellation vertex）改成一般頂點函式：domain 座標與 patch 編號改由展開後的頂點表依 vertex_id 讀取，其餘程式不變。
std::string metal_isoline_domain_source(std::string source,unsigned slot){
    static const std::regex patch_attribute(R"(\[\[\s*patch\s*\(\s*quad\s*,\s*\d+\s*\)\s*\]\]\s*)");
    if(!std::regex_search(source,patch_attribute))throw std::invalid_argument("等值線 DS 缺少 patch 宣告");
    source=std::regex_replace(source,patch_attribute,"");
    // SPIRV-Cross 依 DS 宣告的分量數，參數可能是 gl_TessCoord 或 gl_TessCoordIn（後者在本體內再擴成 float3）。
    static const std::regex coordinate(R"(float2\s+(gl_TessCoord\w*)\s*\[\[\s*position_in_patch\s*\]\])");
    std::smatch found;if(!std::regex_search(source,found,coordinate))throw std::invalid_argument("等值線 DS 缺少 domain 座標參數");
    const auto coordinate_name=found[1].str();
    source=std::regex_replace(source,coordinate,"uint ajIsolineVertexId [[vertex_id]], const device uint4* ajIsolineVertices [[buffer("+std::to_string(slot)+")]]");
    source=std::regex_replace(source,std::regex(R"(,\s*uint\s+gl_PrimitiveID\s*\[\[\s*patch_id\s*\]\])"),"");
    source=std::regex_replace(source,std::regex(R"(uint\s+gl_PrimitiveID\s*\[\[\s*patch_id\s*\]\]\s*,\s*)"),"");
    const auto entry=source.find(" xlatMtlMain(");if(entry==std::string::npos)throw std::invalid_argument("等值線 DS 缺少入口");
    const auto body=source.find("\n{",entry);if(body==std::string::npos)throw std::invalid_argument("等值線 DS 缺少函式本體");
    source.insert(body+2,"\n    const uint4 ajIsoline = ajIsolineVertices[ajIsolineVertexId];\n    const float2 "+coordinate_name+" = as_type<float2>(ajIsoline.yz);\n    const uint gl_PrimitiveID = ajIsoline.x;\n    (void)gl_PrimitiveID;\n");
    return source;
}
}
std::shared_ptr<MetalTessellationPipeline> MetalGl::graphics_modern_tessellation_pipeline(const GraphicsDraw& d){
    if(!d.stages[3].shader || !d.stages[4].shader || d.stages[2].shader || d.topology<33 || d.topology>64)
        throw std::invalid_argument("細分繪製需要 patch topology、HS 與 DS");
    const unsigned input_count=d.topology-32;
    auto& vertex_shader=dynamic_cast<MetalShader&>(*d.stages[0].shader);auto& hull_shader=dynamic_cast<MetalShader&>(*d.stages[3].shader);auto& domain_shader=dynamic_cast<MetalShader&>(*d.stages[4].shader);
    const unsigned index_type=d.indexed?(d.index_format==57?1U:2U):0U;
    const auto element_of=[&](const GraphicsInput& input){return std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});};
    // 管線取決於三個著色器、patch topology、索引型別與頂點 layout（stage-in 描述含步距）。
    std::ostringstream tess_key;tess_key<<"modern:"<<vertex_shader.serial<<':'<<hull_shader.serial<<':'<<domain_shader.serial<<':'<<d.topology<<':'<<index_type;
    for(const auto& input:vertex_shader.inputs){const auto it=element_of(input);if(it==d.layout.end())continue;
        tess_key<<'|'<<it->semantic<<it->index<<':'<<it->format<<':'<<it->offset<<':'<<it->slot<<':'<<it->classification<<':'<<it->step<<':'<<(it->slot<32?d.vertices[it->slot].stride:0U);}
    const auto key=tess_key.str();
    if(const auto failed=tessellation_failures_.find(key);failed!=tessellation_failures_.end())throw std::invalid_argument(failed->second);
    auto found=tessellation_pipelines_.find(key);
    if(found==tessellation_pipelines_.end())try{
        auto p=std::make_shared<MetalTessellationPipeline>();p->modern=true;p->input_count=input_count;
        auto link=graphics_modern_tessellation_link(vertex_shader.bytecode,hull_shader.bytecode,domain_shader.bytecode);link.index_type=index_type;
        p->isoline=link.info[graphics_tess_domain]==2;
        const std::array<std::uint32_t,3> ids{0,3,4};const std::array<const MetalShader*,3> originals{&vertex_shader,&hull_shader,&domain_shader};
        std::array<GraphicsModernShader,3> translated;
        for(unsigned i=0;i<3;++i){
            translated[i]=graphics_modern_tessellation_stage(ids[i],originals[i]->bytecode,link);
            auto shader=std::make_shared<MetalShader>();shader->stage=ids[i];shader->serial=++shader_serial_;shader->bytecode=originals[i]->bytecode;
            shader->reduction_marker=translated[i].source.find(metal_reduction_marker)!=std::string::npos;shader->source=metal_reduction_source(translated[i].source);
            shader->reduction_enabled=shader->reduction_marker && metal_reduction_flag().load(std::memory_order_acquire);
            shader->modern_math=true;
            shader->bindings=translated[i].bindings;shader->inputs=translated[i].inputs;shader->buffer_sizes_slot=translated[i].buffer_sizes_slot;
            shader->signature_inputs=translated[i].signature_inputs;shader->signature_outputs=translated[i].signature_outputs;p->stages[i]=std::move(shader);
        }
        p->vertex_slots=translated[0].tess_slots;p->hull_slots=translated[1].tess_slots;const auto& domain_slots=translated[2].tess_slots;
        p->output_points=p->control_count=link.info[graphics_tess_output_points];
        if(p->hull_slots[graphics_tess_input]==UINT32_MAX || p->hull_slots[graphics_tess_output]==UINT32_MAX || p->hull_slots[graphics_tess_factor]==UINT32_MAX || p->vertex_slots[graphics_tess_output]==UINT32_MAX)
            throw std::invalid_argument("現代細分 kernel 缺少內部緩衝");
        // Metal 分割模式：pow2=0、integer=1、fractional_odd=2、fractional_even=3。
        const auto spacing=link.info[graphics_tess_spacing];
        // 繞序：vkd3d 以 Vulkan 慣例（domain 原點在左上）宣告 VertexOrderCw／Ccw，Metal 的 domain 原點在左下，Metal 端要反轉
        // （D3D 的 triangle_cw 經背面剔除後與 HLSLcc 路徑一致，見 d3d12_modern_tessellation 測項）。
        p->settings={spacing==1?2U:spacing==2?3U:1U,link.info[graphics_tess_winding]?0U:1U,64U,1U};
        p->factor_stride=link.info[graphics_tess_domain]==0?8:12; // 四邊形與等值線（以四邊形轉譯）都是 MTLQuadTessellationFactorsHalf。
        NSError* error=nil;auto options=metal_d3d_compile_options(true);
        const auto library=[&](const MetalShader& shader,const char* what){
            auto value=[device_ newLibraryWithSource:[NSString stringWithUTF8String:shader.source.c_str()] options:options error:&error];
            if(!value)throw std::invalid_argument(std::string("Metal 現代細分")+what+"編譯失敗："+error.localizedDescription.UTF8String+"\n"+shader.source);
            auto function=[value newFunctionWithName:@"xlatMtlMain"];if(!function)throw std::invalid_argument(std::string("Metal 現代細分")+what+"缺少入口");return function;
        };
        const auto stride_of=[](MTLComputePipelineReflection* reflection,unsigned slot)->unsigned{
            if(slot==UINT32_MAX)return 0;
            for(MTLArgument* a in reflection.arguments)if(a.type==MTLArgumentTypeBuffer && a.index==slot && a.bufferDataSize)return static_cast<unsigned>(a.bufferDataSize);
            throw std::invalid_argument("Metal 現代細分輸出缺少結構反射");
        };
        // VS kernel：頂點屬性走 stage-in（逐頂點 X、逐實例 Y），起始頂點／base vertex／起始實例併入綁定位移；索引由 kernel 讀取。
        auto vertex_descriptor=[MTLComputePipelineDescriptor new];vertex_descriptor.computeFunction=library(*p->stages[0]," VS ");
        auto inputs=[MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        std::set<unsigned> occupied;for(const auto& b:p->stages[0]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=14))occupied.insert(b.native_slot);
        for(const auto slot:p->vertex_slots)if(slot!=UINT32_MAX)occupied.insert(slot);if(p->stages[0]->buffer_sizes_slot!=UINT32_MAX)occupied.insert(p->stages[0]->buffer_sizes_slot);
        unsigned next=30;std::map<unsigned,unsigned> buffer_for_slot;
        for(const auto& input:p->stages[0]->inputs){const auto it=element_of(input);
            if(it==d.layout.end() || it->slot>=32 || !d.vertices[it->slot].resource)throw std::invalid_argument("細分頂點 layout 缺少 semantic");
            // 步距 0（D3D 允許，所有頂點讀同一筆）與步進率 0 的逐實例資料都對應 Metal 的常數步進。
            const auto& e=*it;const auto& v=d.vertices[e.slot];const bool constant=(e.classification && !e.step) || !v.stride;
            if(!constant && (v.stride%4 || e.offset+graphics_format_bytes(e.format)>v.stride))throw std::invalid_argument("細分頂點緩衝步距不符 Metal stage-in 要求");
            if(e.offset%4)throw std::invalid_argument("細分頂點屬性位移未對齊");
            auto at=buffer_for_slot.find(e.slot);
            if(at==buffer_for_slot.end()){while(occupied.contains(next)){if(!next)throw std::invalid_argument("細分頂點 binding 配額不足");--next;}
                at=buffer_for_slot.emplace(e.slot,next).first;occupied.insert(next);p->stage_in.emplace_back(e.slot,next);
                auto layout=inputs.layouts[next];layout.stride=constant?(e.offset+graphics_format_bytes(e.format)+3U)/4U*4U:v.stride;
                layout.stepFunction=constant?MTLStepFunctionConstant:e.classification?MTLStepFunctionThreadPositionInGridY:d.indexed?MTLStepFunctionThreadPositionInGridXIndexed:MTLStepFunctionThreadPositionInGridX;
                layout.stepRate=constant?0:e.classification?e.step:1;}
            auto attribute=inputs.attributes[input.attribute];attribute.format=static_cast<MTLAttributeFormat>(metal_vertex_format(e.format));attribute.offset=e.offset;attribute.bufferIndex=at->second;
        }
        if(d.indexed){
            // VS 沒讀 SV_VertexID 時 SPIRV-Cross 不宣告 spvIndices，但逐頂點 stage-in 仍要由 Metal 依索引取值：
            // 另配一個未占用槽位給 stage-in 的索引緩衝（繪製時綁在同一槽位）。
            if(p->vertex_slots[graphics_tess_index]==UINT32_MAX){
                while(occupied.contains(next)){if(!next)throw std::invalid_argument("細分頂點 binding 配額不足");--next;}
                p->vertex_slots[graphics_tess_index]=next;occupied.insert(next);
            }
            inputs.indexBufferIndex=p->vertex_slots[graphics_tess_index];inputs.indexType=d.index_format==57?MTLIndexTypeUInt16:MTLIndexTypeUInt32;
        }
        vertex_descriptor.stageInputDescriptor=inputs;
        MTLComputePipelineReflection* reflection=nil;
        p->vertex_compute=[device_ newComputePipelineStateWithDescriptor:vertex_descriptor options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
        if(!p->vertex_compute)throw std::invalid_argument("Metal 現代細分 VS kernel 建立失敗："+std::string(error.localizedDescription.UTF8String));
        p->vertex_stride=stride_of(reflection,p->vertex_slots[graphics_tess_output]);
        auto hull_descriptor=[MTLComputePipelineDescriptor new];hull_descriptor.computeFunction=library(*p->stages[1]," HS ");
        p->compute=[device_ newComputePipelineStateWithDescriptor:hull_descriptor options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
        if(!p->compute)throw std::invalid_argument("Metal 現代細分 HS kernel 建立失敗："+std::string(error.localizedDescription.UTF8String));
        p->control_stride=stride_of(reflection,p->hull_slots[graphics_tess_output]);p->patch_stride=stride_of(reflection,p->hull_slots[graphics_tess_patch_output]);
        // DS 沒讀的內部緩衝仍由繪製端綁定：放在 DS 未占用的槽位。
        std::set<unsigned> domain_used;for(const auto& b:p->stages[2]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=14))domain_used.insert(b.native_slot);
        for(const auto slot:domain_slots)if(slot!=UINT32_MAX)domain_used.insert(slot);if(p->stages[2]->buffer_sizes_slot!=UINT32_MAX)domain_used.insert(p->stages[2]->buffer_sizes_slot);
        const auto spare=[&]{unsigned at=30;while(domain_used.contains(at)){if(!at)throw std::invalid_argument("DS 內部緩衝配額不足");--at;}domain_used.insert(at);return at;};
        if(p->isoline){
            p->domain_isoline_slot=spare();p->stages[2]->source=metal_isoline_domain_source(p->stages[2]->source,p->domain_isoline_slot);
            auto kernels=[device_ newLibraryWithSource:[NSString stringWithUTF8String:metal_isoline_kernels] options:options error:&error];
            if(!kernels)throw std::invalid_argument("Metal 等值線展開 kernel 編譯失敗："+std::string(error.localizedDescription.UTF8String));
            const auto pipeline=[&](NSString* name){auto function=[kernels newFunctionWithName:name];auto state=function?[device_ newComputePipelineStateWithFunction:function error:&error]:nil;
                if(!state)throw std::invalid_argument("Metal 等值線展開 kernel 建立失敗");return state;};
            p->isoline_reset=pipeline(@"ajIsolineReset");p->isoline_expand=pipeline(@"ajIsolineExpand");p->isoline_args=pipeline(@"ajIsolineArguments");
        }
        p->flat_instances=!p->isoline && !metal_tessellation_primitive_semantic(p->stages[1]->source) && !metal_tessellation_primitive_semantic(p->stages[2]->source);
        p->domain=library(*p->stages[2]," DS ");p->descriptor=[MTLVertexDescriptor vertexDescriptor];
        p->domain_control_slot=domain_slots[graphics_tess_input]!=UINT32_MAX?domain_slots[graphics_tess_input]:spare();
        p->domain_patch_slot=domain_slots[graphics_tess_patch_input]!=UINT32_MAX?domain_slots[graphics_tess_patch_input]:spare();
        if(domain_slots[graphics_tess_factor]!=UINT32_MAX){p->domain_raw_factor_slot=domain_slots[graphics_tess_factor];p->raw_factor_stride=p->factor_stride;}
        if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder)for(unsigned i=0;i<3;++i)
            std::ofstream(std::filesystem::path(folder)/("tess-modern-"+std::to_string(p->stages[0]->serial)+"-"+std::to_string(ids[i])+".metal"))<<p->stages[i]->source;
        // 診斷：X64ARM_TRACE_TESS=1 列出現代路徑建立的細分管線（domain 0 三角形／1 四邊形／2 等值線、spacing、輸出控制點），上限 200 筆。
        if(static const bool trace=std::getenv("X64ARM_TRACE_TESS")!=nullptr;trace){static std::atomic<unsigned> traced{0};
            if(traced.fetch_add(1)<200)std::fprintf(stderr,"[tess] modern 建立 vs=%llu hs=%llu ds=%llu domain=%u spacing=%u points=%u flat=%d isoline=%d\n",
                static_cast<unsigned long long>(vertex_shader.serial),static_cast<unsigned long long>(hull_shader.serial),static_cast<unsigned long long>(domain_shader.serial),
                link.info[graphics_tess_domain],link.info[graphics_tess_spacing],link.info[graphics_tess_output_points],p->flat_instances?1:0,p->isoline?1:0);}
        if(tessellation_pipelines_.size()>=1024)tessellation_pipelines_.erase(tessellation_pipelines_.begin());found=tessellation_pipelines_.emplace(key,std::move(p)).first;
    }catch(const std::exception& error){
        if(static const bool trace=std::getenv("X64ARM_TRACE_TESS")!=nullptr;trace)std::fprintf(stderr,"[tess] modern 失敗 key=%s：%s\n",key.c_str(),error.what());
        if(tessellation_failures_.size()<1024)tessellation_failures_.emplace(key,error.what());throw;}
    return found->second;
}
MetalTessellationDraw MetalGl::graphics_modern_tessellation(const GraphicsDraw& d){
    if(!d.stages[3].shader || !d.stages[4].shader || d.stages[2].shader || d.topology<33 || d.topology>64)
        throw std::invalid_argument("細分繪製需要 patch topology、HS 與 DS");
    const unsigned input_count=d.topology-32,patch_count=d.indirect?0U:d.count/input_count;
    if(!d.indirect && (!patch_count || !d.instances))return {};
    auto& hull_shader=dynamic_cast<MetalShader&>(*d.stages[3].shader);
    auto p=graphics_modern_tessellation_pipeline(d);
    if(d.indirect)return graphics_modern_tessellation_indirect(d,p);
    std::vector<std::shared_ptr<GraphicsObject>> holders;
    // 診斷（X64ARM_DUMP_DXBC）：中間緩衝改用共享記憶體，首次繪製後保存內容。
    static const bool dump_buffers=[] {const auto* folder=std::getenv("X64ARM_DUMP_DXBC");return folder && *folder;}();
    const auto temporary=[&](std::uint64_t size)->id<MTLBuffer> {
        size=std::max<std::uint64_t>(size,16);if(size>512U*1024U*1024U)throw std::invalid_argument("細分輸出超出限制");
        pending_leases_.push_back(reserve_gpu_memory(std::max<std::uint64_t>(4096,size)));
        std::shared_ptr<MetalPooledBuffer> holder;
        if(dump_buffers){holder=std::make_shared<MetalPooledBuffer>();holder->buffer=[device_ newBufferWithLength:size options:MTLResourceStorageModeShared];}
        else holder=graphics_scratch_buffer(size,false);
        if(!holder || !holder->buffer)throw std::bad_alloc{};
        holders.push_back(holder);graphics_pending_.push_back(holder);return holder->buffer;
    };
    const unsigned points=patch_count*input_count;const std::uint64_t total=std::uint64_t{patch_count}*d.instances;
    auto vertices=temporary(std::uint64_t{points}*d.instances*p->vertex_stride);
    MetalTessellationDraw result{p,temporary(total*p->control_count*p->control_stride),temporary(total*p->patch_stride),temporary(total*p->factor_stride),nil,patch_count,nil,nil,nil,{}};
    if(p->raw_factor_stride)result.raw_factors=result.factors;
    begin_commands(true,"tessellation");
    // 非同步 GPU 剖析時 VS／HS 前置運算也取樣並標記（patch 數、實例數），否則其時間落在命令緩衝的未歸屬區段。
    id<MTLComputeCommandEncoder> encoder=nil;
    if(const auto base=profile_reserve(true)){
        auto descriptor=[MTLComputePassDescriptor computePassDescriptor];descriptor.dispatchType=MTLDispatchTypeSerial;
        auto attachment=descriptor.sampleBufferAttachments[0];attachment.sampleBuffer=profile_buffer_;attachment.startOfEncoderSampleIndex=*base;attachment.endOfEncoderSampleIndex=*base+1;
        encoder=[commands_ computeCommandEncoderWithDescriptor:descriptor];
    }else encoder=[commands_ computeCommandEncoder];
    if(!encoder)throw std::runtime_error("Metal command encoder 建立失敗");
    if(metal_labels()){encoder.label=[NSString stringWithFormat:@"cs tess %016llx p%u i%u c%u",static_cast<unsigned long long>(hull_shader.content_key),patch_count,d.instances,p->control_count];hang_note(encoder.label.UTF8String);}
    MetalEncoderScope scope{encoder};
    // VS：每個實例 points 個頂點，輸出依 (實例, 頂點) 連續排列。
    [encoder setComputePipelineState:p->vertex_compute];{auto stage=d.stages[0];stage.shader=p->stages[0];graphics_bind(nil,encoder,stage,false);}
    for(const auto& [slot,index]:p->stage_in){const auto& v=d.vertices[slot];auto& resource=metal_resource(v.resource);
        const auto element=std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.slot==slot;});
        const std::int64_t offset=element!=d.layout.end() && element->classification?std::int64_t{v.offset}+std::int64_t{v.stride}*d.start_instance:
            std::int64_t{v.offset}+std::int64_t{v.stride}*(d.indexed?std::int64_t{d.base_vertex}:std::int64_t{d.start});
        if(offset<0)throw std::invalid_argument("細分頂點緩衝位移為負");
        [encoder setBuffer:resource.buffer offset:static_cast<NSUInteger>(offset) atIndex:index];graphics_pending_.push_back(v.resource);}
    if(d.indexed){[encoder setBuffer:metal_resource(d.indices).buffer offset:std::uint64_t{d.index_offset}+std::uint64_t{d.start}*(d.index_format==57?2U:4U) atIndex:p->vertex_slots[graphics_tess_index]];graphics_pending_.push_back(d.indices);}
    [encoder setBuffer:vertices offset:0 atIndex:p->vertex_slots[graphics_tess_output]];
    if(p->vertex_slots[graphics_tess_params]!=UINT32_MAX){const std::array<std::uint32_t,2> params{points,d.instances};[encoder setBytes:params.data() length:sizeof params atIndex:p->vertex_slots[graphics_tess_params]];}
    if(!p->stage_in.empty() || d.indexed)[encoder setStageInRegion:MTLRegionMake2D(0,0,points,d.instances)];
    [encoder dispatchThreads:MTLSizeMake(points,d.instances,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(points,p->vertex_compute.maxTotalThreadsPerThreadgroup),1,1)];
    // HS：每個實例一次派送（SV_PrimitiveID 每實例從 0 起算），執行緒數＝patch 數 × 輸出控制點數。
    [encoder setComputePipelineState:p->compute];{auto stage=d.stages[3];stage.shader=p->stages[1];graphics_bind(nil,encoder,stage,false);}
    // 攤平時整個繪製視為 total 個 patch（全域編號＝實例 × patch 數＋patch），一次派送；否則每個實例各派送一次。
    const bool flat=p->flat_instances && d.instances>1;
    if(flat && total*p->control_count>UINT32_MAX)throw std::invalid_argument("細分 patch 總數超出限制");
    const std::array<std::uint32_t,2> hull_params{input_count,flat?static_cast<std::uint32_t>(total):patch_count};
    const unsigned dispatches=flat?1U:d.instances,dispatch_patches=flat?static_cast<unsigned>(total):patch_count;
    for(unsigned instance=0;instance<dispatches;++instance){
        const std::uint64_t first=std::uint64_t{instance}*patch_count;
        [encoder setBuffer:vertices offset:std::uint64_t{instance}*points*p->vertex_stride atIndex:p->hull_slots[graphics_tess_input]];
        [encoder setBuffer:result.controls offset:first*p->control_count*p->control_stride atIndex:p->hull_slots[graphics_tess_output]];
        if(p->hull_slots[graphics_tess_patch_output]!=UINT32_MAX)[encoder setBuffer:result.patches offset:first*p->patch_stride atIndex:p->hull_slots[graphics_tess_patch_output]];
        [encoder setBuffer:result.factors offset:first*p->factor_stride atIndex:p->hull_slots[graphics_tess_factor]];
        if(p->hull_slots[graphics_tess_params]!=UINT32_MAX)[encoder setBytes:hull_params.data() length:sizeof hull_params atIndex:p->hull_slots[graphics_tess_params]];
        // 同一個 patch 的控制點執行緒必須落在同一個執行緒群組（HS 的 patch 常數階段讀其他控制點的輸出時要靠群組屏障）。
        const NSUInteger threads=std::uint64_t{dispatch_patches}*p->control_count;
        const NSUInteger group=std::max<NSUInteger>(p->control_count,p->compute.maxTotalThreadsPerThreadgroup/p->control_count*p->control_count);
        [encoder dispatchThreads:MTLSizeMake(threads,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(threads,group),1,1)];
    }
    if(p->isoline){
        // 線段頂點容量：每個 patch 最多 64 條線 × 64 段 × 2 個頂點，總量上限 400 萬個頂點（64 MiB）。
        const std::uint64_t capacity=std::min<std::uint64_t>(total*64U*64U*2U,4U*1024U*1024U);
        result.isoline_vertices=temporary(capacity*16);result.isoline_arguments=temporary(16);auto counter=temporary(16);
        [encoder setComputePipelineState:p->isoline_reset];[encoder setBuffer:counter offset:0 atIndex:0];[encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        const std::array<std::uint32_t,4> params{static_cast<std::uint32_t>(total),static_cast<std::uint32_t>(capacity),p->settings[0],0};
        [encoder setComputePipelineState:p->isoline_expand];[encoder setBuffer:result.factors offset:0 atIndex:0];[encoder setBuffer:result.isoline_vertices offset:0 atIndex:1];
        [encoder setBuffer:counter offset:0 atIndex:2];[encoder setBytes:params.data() length:sizeof params atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(total,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(total,p->isoline_expand.maxTotalThreadsPerThreadgroup),1,1)];
        const std::uint32_t limit=static_cast<std::uint32_t>(capacity);
        [encoder setComputePipelineState:p->isoline_args];[encoder setBuffer:counter offset:0 atIndex:0];[encoder setBuffer:result.isoline_arguments offset:0 atIndex:1];
        [encoder setBytes:&limit length:sizeof limit atIndex:2];[encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    }
    scope.end();
    if(dump_buffers && !p->captured){
        flush(true);const auto base=std::filesystem::path(std::getenv("X64ARM_DUMP_DXBC"))/("tess-modern-"+std::to_string(p->stages[0]->serial));
        const auto save=[&](const char* suffix,id<MTLBuffer> buffer){std::ofstream(base.string()+suffix,std::ios::binary).write(static_cast<const char*>(buffer.contents),static_cast<std::streamsize>(buffer.length));};
        save("-vertices.bin",vertices);save("-control.bin",result.controls);save("-patch.bin",result.patches);save("-factor.bin",result.factors);
        std::ofstream(base.string()+".json")<<"{\"vertex_stride\":"<<p->vertex_stride<<",\"control_stride\":"<<p->control_stride<<",\"patch_stride\":"<<p->patch_stride<<",\"factor_stride\":"<<p->factor_stride<<",\"points\":"<<points<<",\"patches\":"<<patch_count<<"}\n";
        p->captured=true;
    }
    result.holders=std::move(holders);return result;
}
// 間接細分是否可走 GPU 驅動路徑：現代 HS／DS、非索引、攤平實例的管線、沒有步進率大於 1 的逐實例屬性（stage-in 區域的起始實例
// 加在步進前，與 D3D 的「起始實例＋實例/步進率」在起點不整除時不同）。其餘情形仍讀回參數走 CPU 路徑。
bool MetalGl::graphics_modern_tessellation_indirect_ready(const GraphicsDraw& d){
    static const bool enabled=[] {const auto* text=std::getenv("X64ARM_METAL_TESS_GPU_INDIRECT");return !text || std::string_view(text)!="0";}();
    if(!enabled || !d.indirect || d.indexed || !d.stages[3].shader || !d.stages[4].shader || d.stages[2].shader || d.topology<33 || d.topology>64 ||
       std::any_of(d.stream_targets.begin(),d.stream_targets.end(),[](const auto& target){return bool(target);}) ||
       !graphics_modern_shader_required(dynamic_cast<MetalShader&>(*d.stages[3].shader).bytecode))return false;
    for(const auto& input:dynamic_cast<MetalShader&>(*d.stages[0].shader).inputs){
        const auto it=std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});
        if(it!=d.layout.end() && it->classification && it->step>1)return false;
    }
    try{const auto p=graphics_modern_tessellation_pipeline(d);return p->flat_instances && !p->isoline;}catch(const std::exception&){return false;}
}
MetalTessellationDraw MetalGl::graphics_modern_tessellation_indirect(const GraphicsDraw& d,const std::shared_ptr<MetalTessellationPipeline>& p){
    if(d.indexed || p->isoline || !p->flat_instances)throw std::invalid_argument("間接細分不符 GPU 驅動條件");
    auto& arguments=metal_resource(d.indirect);
    if(!arguments.buffer || (d.indirect_offset&3U) || std::uint64_t{d.indirect_offset}+16>arguments.desc.width)throw std::invalid_argument("間接細分參數越界或未對齊");
    if(!tessellation_indirect_args_){
        NSError* error=nil;auto options=[MTLCompileOptions new];
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:metal_tessellation_indirect_kernels] options:options error:&error];
        auto function=library?[library newFunctionWithName:@"ajTessIndirectArguments"]:nil;
        tessellation_indirect_args_=function?[device_ newComputePipelineStateWithFunction:function error:&error]:nil;
        if(!tessellation_indirect_args_)throw std::invalid_argument("Metal 間接細分參數 kernel 建立失敗");
    }
    // 容量依上次回饋的需求量倍增（上限約 100 萬個 patch）；回饋緩衝為共享記憶體，讀取不等待 GPU。
    if(!p->indirect_feedback){p->indirect_feedback=[device_ newBufferWithLength:16 options:MTLResourceStorageModeShared];if(!p->indirect_feedback)throw std::bad_alloc{};std::memset(p->indirect_feedback.contents,0,16);}
    const auto requested=static_cast<const volatile std::uint32_t*>(p->indirect_feedback.contents)[0];
    constexpr unsigned capacity_limit=1U<<20;
    while(p->indirect_capacity<requested && p->indirect_capacity<capacity_limit)p->indirect_capacity*=2;
    const unsigned capacity=p->indirect_capacity,input_count=p->input_count;
    std::vector<std::shared_ptr<GraphicsObject>> holders;
    const auto temporary=[&](std::uint64_t size)->id<MTLBuffer> {
        size=std::max<std::uint64_t>(size,16);if(size>512U*1024U*1024U)throw std::invalid_argument("細分輸出超出限制");
        pending_leases_.push_back(reserve_gpu_memory(std::max<std::uint64_t>(4096,size)));
        auto holder=graphics_scratch_buffer(size,false);if(!holder || !holder->buffer)throw std::bad_alloc{};
        holders.push_back(holder);graphics_pending_.push_back(holder);return holder->buffer;
    };
    const unsigned vs_group=static_cast<unsigned>(std::min<NSUInteger>(64,p->vertex_compute.maxTotalThreadsPerThreadgroup));
    const unsigned hs_group=static_cast<unsigned>(std::max<NSUInteger>(p->control_count,p->compute.maxTotalThreadsPerThreadgroup/p->control_count*p->control_count));
    // HS 以整群派送：最後一群多出的執行緒會寫到總量之後，輸出緩衝多留一群的空間（讀取由 kernel 的 min 夾住）。
    const std::uint64_t padded=std::uint64_t{capacity}+hs_group;
    auto vertices=temporary(std::uint64_t{capacity}*input_count*p->vertex_stride);
    MetalTessellationDraw result{p,temporary(padded*p->control_count*p->control_stride),temporary(padded*p->patch_stride),temporary(padded*p->factor_stride),nil,1,nil,nil,nil,{}};
    if(p->raw_factor_stride)result.raw_factors=result.factors;
    auto args=temporary(1024);result.indirect_args=args;
    begin_commands(true,"tessellation");
    id<MTLComputeCommandEncoder> encoder=nil;
    if(const auto base=profile_reserve(true)){
        auto descriptor=[MTLComputePassDescriptor computePassDescriptor];descriptor.dispatchType=MTLDispatchTypeSerial;
        auto attachment=descriptor.sampleBufferAttachments[0];attachment.sampleBuffer=profile_buffer_;attachment.startOfEncoderSampleIndex=*base;attachment.endOfEncoderSampleIndex=*base+1;
        encoder=[commands_ computeCommandEncoderWithDescriptor:descriptor];
    }else encoder=[commands_ computeCommandEncoder];
    if(!encoder)throw std::runtime_error("Metal command encoder 建立失敗");
    if(metal_labels()){encoder.label=[NSString stringWithFormat:@"cs tess-indirect %016llx cap%u c%u",static_cast<unsigned long long>(dynamic_cast<MetalShader&>(*d.stages[3].shader).content_key),capacity,p->control_count];hang_note(encoder.label.UTF8String);}
    MetalEncoderScope scope{encoder};
    const std::array<std::uint32_t,8> params{input_count,capacity,vs_group,hs_group,p->control_count,0,0,0};
    [encoder setComputePipelineState:tessellation_indirect_args_];[encoder setBuffer:arguments.buffer offset:d.indirect_offset atIndex:0];[encoder setBuffer:args offset:0 atIndex:1];
    [encoder setBytes:params.data() length:sizeof params atIndex:2];[encoder setBuffer:p->indirect_feedback offset:0 atIndex:3];
    [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    graphics_pending_.push_back(d.indirect);
    // VS：stage-in 區域與群組數由參數 kernel 決定；頂點緩衝只綁原始位移（起始頂點／實例在區域起點）。
    [encoder setComputePipelineState:p->vertex_compute];{auto stage=d.stages[0];stage.shader=p->stages[0];graphics_bind(nil,encoder,stage,false);}
    for(const auto& [slot,index]:p->stage_in){const auto& v=d.vertices[slot];auto& resource=metal_resource(v.resource);
        [encoder setBuffer:resource.buffer offset:v.offset atIndex:index];graphics_pending_.push_back(v.resource);}
    [encoder setBuffer:vertices offset:0 atIndex:p->vertex_slots[graphics_tess_output]];
    if(p->vertex_slots[graphics_tess_params]!=UINT32_MAX)[encoder setBuffer:args offset:512 atIndex:p->vertex_slots[graphics_tess_params]];
    [encoder setStageInRegionWithIndirectBuffer:args indirectBufferOffset:16];
    [encoder dispatchThreadgroupsWithIndirectBuffer:args indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(vs_group,1,1)];
    // HS：全域 patch 一次派送。
    [encoder setComputePipelineState:p->compute];{auto stage=d.stages[3];stage.shader=p->stages[1];graphics_bind(nil,encoder,stage,false);}
    [encoder setBuffer:vertices offset:0 atIndex:p->hull_slots[graphics_tess_input]];
    [encoder setBuffer:result.controls offset:0 atIndex:p->hull_slots[graphics_tess_output]];
    if(p->hull_slots[graphics_tess_patch_output]!=UINT32_MAX)[encoder setBuffer:result.patches offset:0 atIndex:p->hull_slots[graphics_tess_patch_output]];
    [encoder setBuffer:result.factors offset:0 atIndex:p->hull_slots[graphics_tess_factor]];
    if(p->hull_slots[graphics_tess_params]!=UINT32_MAX)[encoder setBuffer:args offset:256 atIndex:p->hull_slots[graphics_tess_params]];
    [encoder dispatchThreadgroupsWithIndirectBuffer:args indirectBufferOffset:48 threadsPerThreadgroup:MTLSizeMake(hs_group,1,1)];
    scope.end();
    result.holders=std::move(holders);
    return result;
}
