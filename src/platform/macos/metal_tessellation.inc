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
        metal_buffer_shader(*p->stages[2]);
        const auto reserve_slot=[](std::set<unsigned>& slots){
            unsigned at=30;while(slots.contains(at)){if(!at)throw std::invalid_argument("細分輸入 binding 配額不足");--at;}
            slots.insert(at);return at;
        };
        p->vertex_id_slot=reserve_slot(kernel_slots);
        std::set<unsigned> domain_slots;
        for(const auto& b:p->stages[2]->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9))domain_slots.insert(b.native_slot);
        if(p->stages[2]->buffer_views_slot!=UINT32_MAX)domain_slots.insert(p->stages[2]->buffer_views_slot);
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
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
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
        temporary(total*p->factor_stride,false),p->raw_factor_stride?temporary(total*p->raw_factor_stride,false):nil,patch_count,{}};
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
        (main_texture_writes_ && (samples_textures(d.stages[0]) || samples_textures(d.stages[3])))?3U:(encoder_chain_fence_ || dump_shaders || dump_buffers || metal_gpu_profile())?4U:6U;
    const bool side=refusal==6U;
    if(refusal<5U)++tess_main_[refusal];
    id<MTLComputeCommandEncoder> encoder=nil;
    if(side){if(!side_commands_)side_commands_=[queue_ commandBuffer];if(!side_commands_)throw std::runtime_error("Metal command buffer 建立失敗");encoder=[side_commands_ computeCommandEncoder];++side_computes_;}
    else{begin_commands(true,"tessellation");encoder=[commands_ computeCommandEncoder];}
    if(!encoder)throw std::runtime_error("Metal command encoder 建立失敗");
    MetalEncoderScope scope{encoder};[encoder setComputePipelineState:p->compute];
    std::set<unsigned> occupied{p->factor_slot,p->info_slot,p->vertex_id_slot};if(p->control_slot!=UINT32_MAX)occupied.insert(p->control_slot);if(p->patch_slot!=UINT32_MAX)occupied.insert(p->patch_slot);
    if(p->raw_factor_slot!=UINT32_MAX){occupied.insert(p->raw_factor_slot);[encoder setBuffer:result.raw_factors offset:0 atIndex:p->raw_factor_slot];}
    for(unsigned i=0;i<2;++i){auto stage=d.stages[i?3:0];stage.shader=p->stages[i];graphics_bind(nil,encoder,stage,false);for(const auto& b:stage.shader->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9))occupied.insert(b.native_slot);}
    for(unsigned i=0;i<2;++i)if(p->stages[i]->buffer_views_slot!=UINT32_MAX)occupied.insert(p->stages[i]->buffer_views_slot);
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
