// DXBC 幾何階段共用運算轉譯；Metal 依序執行 VS、GS compute 與 raster。
// 中介資料以 float4 保存原始位元，整數 varying 不經浮點數值轉換。
void metal_replace(std::string& text,const std::string& from,const std::string& to){
    for(std::size_t at=0;(at=text.find(from,at))!=text.npos;at+=to.size())text.replace(at,from.size(),to);
}
void metal_shader_varyings(MetalShader& shader,const ShaderInfo& info){
    const auto convert=[](const auto& signatures,bool output){
        std::vector<MetalVarying> out;
        for(const auto& s:signatures){
            std::string name=s.semanticName+std::to_string(s.ui32SemanticIndex);
            if(s.eSystemValueType==NAME_POSITION)name="mtl_Position";
            else if(s.eSystemValueType==NAME_RENDER_TARGET_ARRAY_INDEX)name="mtl_Layer";
            else if(s.eSystemValueType==NAME_VIEWPORT_ARRAY_INDEX)name="mtl_ViewPortIndex";
            else if(!output && s.eSystemValueType==NAME_CLIP_DISTANCE)name="x64ClipDistance"+std::to_string(s.ui32SemanticIndex);
            out.push_back({name,s.semanticName,s.ui32SemanticIndex,static_cast<unsigned>(std::popcount(s.ui32Mask)),static_cast<unsigned>(s.eComponentType),static_cast<unsigned>(s.eSystemValueType)});
            if(output && s.eSystemValueType==NAME_CLIP_DISTANCE){
                auto& varying=out.back();varying.name="mtl_ClipDistance";
                for(const auto& other:signatures)if(other.eSystemValueType==NAME_CLIP_DISTANCE){const auto count=static_cast<unsigned>(std::popcount(other.ui32Mask));varying.clip_count+=count;if(other.ui32SemanticIndex<s.ui32SemanticIndex)varying.clip_offset+=count;}
            }
        }
        return out;
    };
    shader.input_varyings=convert(info.psInputSignatures,false);shader.output_varyings=convert(info.psOutputSignatures,true);
}
void metal_geometry_metadata(MetalShader& shader,std::span<const std::uint8_t> bytes){
    for(unsigned i=0;i<metal_word(bytes,28);++i){const auto off=metal_word(bytes,32+4*i),tag=metal_word(bytes,off);
        if(tag!=0x52444853 && tag!=0x58454853)continue;
        const auto n=metal_word(bytes,off+4)/4;
        for(unsigned at=2;at<n;){const auto t=metal_word(bytes,off+8+4*at),op=t&0x7ff,len=op==53?metal_word(bytes,off+12+4*at):(t>>24)&127;
            if(op==92)shader.geometry_output=(t>>11)&0x7f;
            else if(op==93)shader.geometry_input=(t>>11)&0x3f;
            else if(op==94)shader.geometry_max=metal_word(bytes,off+12+4*at);
            else if(op==206)shader.geometry_instances=metal_word(bytes,off+12+4*at);
            at+=len;
        }
    }
    if(!shader.geometry_max || shader.geometry_max>1024 || shader.geometry_instances!=1 ||
       (shader.geometry_input!=1 && shader.geometry_input!=2 && shader.geometry_input!=3 && shader.geometry_input!=6 && shader.geometry_input!=7) ||
       (shader.geometry_output!=1 && shader.geometry_output!=3 && shader.geometry_output!=5))
        throw std::invalid_argument("幾何階段圖元／頂點配額不支援");
    for(const auto& v:shader.output_varyings)if(!v.components || v.components>4 || (v.system!=0 && v.system!=NAME_POSITION && v.system!=NAME_RENDER_TARGET_ARRAY_INDEX && v.system!=NAME_VIEWPORT_ARRAY_INDEX && v.system!=NAME_CLIP_DISTANCE))
        throw std::invalid_argument("幾何輸出 semantic 尚未映射："+v.semantic);
}
struct MetalFunctionText {std::size_t begin{},end{};std::vector<std::string> parameters,names;};
MetalFunctionText metal_function_text(const std::string& program_source,const std::string& name){
    MetalFunctionText result;result.begin=program_source.find(name+"(");
    if(result.begin==program_source.npos)throw std::invalid_argument("Metal 著色器函式不存在："+name);
    result.begin+=name.size()+1;int depth=1,angle=0,square=0;auto start=result.begin;
    for(auto at=start;at<program_source.size();++at){const char c=program_source[at];
        if(c=='(')++depth;else if(c==')')--depth;else if(c=='<')++angle;else if(c=='>')--angle;else if(c=='[')++square;else if(c==']')--square;
        if(!depth || (c==',' && depth==1 && !angle && !square)){
            auto p=program_source.substr(start,at-start);if(p.find_first_not_of(" \t\r\n")!=p.npos){
                std::smatch match;const auto clean=std::regex_replace(p,std::regex(R"(\[\[.*?\]\])"),"");
                if(!std::regex_search(clean,match,std::regex(R"(([A-Za-z_]\w*)\s*$)")))throw std::invalid_argument("Metal 函式參數無法解析");
                result.parameters.push_back(p);result.names.push_back(match[1]);
            }
            start=at+1;if(!depth){result.end=at;return result;}
        }
    }
    throw std::invalid_argument("Metal 函式參數截斷");
}
std::string metal_geometry_clean(std::string program_source){
    const auto first=program_source.find("struct Mtl_GeometryIn"),end=program_source.find("};",first);
    if(first!=program_source.npos && end!=program_source.npos){auto part=program_source.substr(first,end-first);part=std::regex_replace(part,std::regex(R"(\[\[.*?\]\])"),"");program_source.replace(first,end-first,part);}
    const auto fn=metal_function_text(program_source,"x64GeometryMain");
    const auto parameters=std::regex_replace(program_source.substr(fn.begin,fn.end-fn.begin),std::regex(R"(\[\[.*?\]\])"),"");program_source.replace(fn.begin,fn.end-fn.begin,parameters);
    // 超出 DXBC 宣告上限的惡意 emit 不得越界寫入 thread 記憶體。
    metal_replace(program_source,"x64Emit.vertices[x64Emit.count] = output; x64Emit.strips[x64Emit.count++] = x64Emit.strip;",
        "if(x64Emit.count < sizeof(x64Emit.vertices)/sizeof(x64Emit.vertices[0])) { x64Emit.vertices[x64Emit.count] = output; x64Emit.strips[x64Emit.count++] = x64Emit.strip; }");
    return program_source;
}
std::string metal_pack_varying(const MetalVarying& v,const std::string& field){
    if(v.clip_count){std::string value="float4(";for(unsigned i=0;i<4;++i){if(i)value+=",";value+=i<v.components?field+(v.clip_count>1?"["+std::to_string(v.clip_offset+i)+"]":""):"0";}return value+")";}
    const auto type=v.type==INOUT_COMPONENT_UINT32?"uint":v.type==INOUT_COMPONENT_SINT32?"int":"float";
    std::string value=std::string(type)+"4("+field;
    for(unsigned i=v.components;i<4;++i)value+=",0";
    value+=")";return v.type==INOUT_COMPONENT_FLOAT32?value:"as_type<float4>("+value+")";
}
std::string metal_unpack_varying(const MetalVarying& v,const std::string& field){
    auto value=v.type==INOUT_COMPONENT_UINT32?"as_type<uint4>("+field+")":v.type==INOUT_COMPONENT_SINT32?"as_type<int4>("+field+")":field;
    return "("+value+")."+std::string("xyzw").substr(0,v.components);
}
std::string metal_assign_varying(const MetalVarying& v,const std::string& destination,const std::string& packed){
    if(v.clip_count){std::string code;for(unsigned i=0;i<v.components;++i)code+=destination+(v.clip_count>1?"["+std::to_string(v.clip_offset+i)+"]":"")+"=("+packed+")."+std::string(1,"xyzw"[i])+";\n";return code;}
    return destination+"="+metal_unpack_varying(v,packed)+";\n";
}
std::vector<unsigned> metal_free_buffers(const MetalShader& shader,const std::array<int,32>* slots,unsigned count){
    std::set<unsigned> occupied;
    if(shader.buffer_views_slot!=UINT32_MAX)occupied.insert(shader.buffer_views_slot);
    for(const auto& b:shader.bindings)if(b.type==0 || b.type==3 || b.type==4){occupied.insert(b.native_slot);if(b.counter_slot!=UINT32_MAX)occupied.insert(b.counter_slot);}
    if(slots)for(int slot:*slots)if(slot>=0)occupied.insert(static_cast<unsigned>(slot));
    std::vector<unsigned> result;for(unsigned i=0;i<31 && result.size()<count;++i)if(!occupied.contains(i))result.push_back(i);
    if(result.size()!=count)throw std::invalid_argument("Metal 幾何階段 buffer binding 配額不足");return result;
}

#include "metal_stream_output.inc"
#include "metal_stream_output_gpu.inc"
std::pair<unsigned,unsigned> metal_input_primitives(unsigned topology,unsigned count){
    switch(topology){case 1:return {count,1};case 2:return {count/2,2};case 3:return {count>1?count-1:0,2};case 4:return {count/3,3};case 5:return {count>2?count-2:0,3};case 10:return {count/4,4};case 11:return {count>3?count-3:0,4};case 12:return {count/6,6};default:throw std::invalid_argument("幾何輸入 topology 尚未映射");}
}
MetalGeometryDraw MetalGl::graphics_geometry(const GraphicsDraw& d,MTLVertexDescriptor* descriptor,const std::array<int,32>& slots,const std::vector<std::array<std::uint32_t,3>>& shifted,const std::string& layout_key){@autoreleasepool{
    auto& vs=dynamic_cast<MetalShader&>(*d.stages[0].shader);auto& original_gs=dynamic_cast<MetalShader&>(*d.stages[2].shader);
    MetalShader passthrough;auto* selected=&original_gs;
    const auto [primitives,input_count]=metal_input_primitives(d.topology,d.count);
    if(original_gs.stage==0){
        // 以 VS 輸出簽章建立的 SO 物件只轉送目前 VS 的輸出，不重跑宣告時的 VS。
        passthrough=original_gs;passthrough.input_varyings=vs.output_varyings;passthrough.output_varyings=vs.output_varyings;
        for(auto& v:passthrough.input_varyings)if(v.clip_count){v.name="x64ClipDistance"+std::to_string(v.index);v.clip_count=0;v.clip_offset=0;}
        const auto emitted_count=input_count==4?2U:input_count==6?3U:input_count;
        passthrough.geometry_input=input_count==4?6U:input_count==6?7U:input_count;passthrough.geometry_output=emitted_count==1?1:emitted_count==2?3:5;passthrough.geometry_max=emitted_count;
        std::ostringstream source;source<<"#include <metal_stdlib>\nusing namespace metal;\nstruct Mtl_GeometryIn {\n";
        const auto type=[](const MetalVarying& v){return std::string(v.type==INOUT_COMPONENT_UINT32?"uint":v.type==INOUT_COMPONENT_SINT32?"int":"float")+(v.components==1?"":std::to_string(v.components));};
        for(const auto& v:passthrough.input_varyings)source<<type(v)<<" "<<v.name<<"["<<input_count<<"];\n";
        source<<"};\nstruct Mtl_GeometryOut {\n";
        for(const auto& v:vs.output_varyings){if(v.clip_count){if(!v.clip_offset)source<<"float "<<v.name<<" [[clip_distance]]"<<(v.clip_count>1?"["+std::to_string(v.clip_count)+"]":"")<<";\n";continue;}source<<type(v)<<" "<<v.name;const auto attribute=v.system==NAME_POSITION?"position":v.system==NAME_RENDER_TARGET_ARRAY_INDEX?"render_target_array_index":v.system==NAME_VIEWPORT_ARRAY_INDEX?"viewport_array_index":"";source<<" [["<<(attribute[0]?std::string(attribute):"user("+v.name+")")<<"]];\n";}
        source<<"};\nstruct Mtl_GeometryEmit {Mtl_GeometryOut vertices["<<emitted_count<<"];uint strips["<<emitted_count<<"];uint count,strip;};\nvoid x64GeometryMain(Mtl_GeometryIn input,thread Mtl_GeometryEmit& x64Emit){for(uint i=0;i<"<<emitted_count<<";++i){\n";
        for(unsigned j=0;j<vs.output_varyings.size();++j){const auto& v=vs.output_varyings[j];const auto& in=passthrough.input_varyings[j];source<<metal_assign_varying(v,"x64Emit.vertices[i]."+v.name,metal_pack_varying(in,"input."+in.name+"["+(input_count==4?"i+1":input_count==6?"i*2":"i")+"]"));}
        source<<"x64Emit.strips[i]=0;}x64Emit.count="<<emitted_count<<";}\n";
        passthrough.source=source.str();passthrough.geometry_original=passthrough.source;selected=&passthrough;
    }
    auto& gs=*selected;
    if(input_count!=(gs.geometry_input==6?4U:gs.geometry_input==7?6U:gs.geometry_input))throw std::invalid_argument("幾何輸入 primitive 與 IA topology 不符");
    const unsigned output_width=gs.geometry_output==1?1:gs.geometry_output==3?2:3;
    const unsigned output_capacity=gs.geometry_max>=output_width?(gs.geometry_max-output_width+1)*output_width:0;
    if(!primitives || !output_capacity)return {};
    if(vs.output_varyings.empty() || gs.output_varyings.empty())throw std::invalid_argument("幾何階段沒有輸出反射");
    const auto key=layout_key+":"+std::to_string(gs.serial);
    std::shared_ptr<MetalGeometryPipeline> p;
    if(const auto found=geometry_pipelines_.find(key);found!=geometry_pipelines_.end())p=found->second;
    else {
        p=std::make_shared<MetalGeometryPipeline>();p->vertex=d.stages[0].shader;p->geometry=d.stages[2].shader;
        auto free=metal_free_buffers(vs,&slots,2);p->capture_buffer=free[0];p->capture_params=free[1];
        free=metal_free_buffers(gs,nullptr,5);p->input_buffer=free[0];p->index_buffer=free[1];p->output_buffer=free[2];p->params_buffer=free[3];p->counts_buffer=free[4];
        auto capture=vs.source;const auto fn=metal_function_text(capture,"xlatMtlMain");
        std::string vertex_id,instance_id,extra;
        for(unsigned i=0;i<fn.parameters.size();++i){if(fn.parameters[i].find("vertex_id")!=std::string::npos)vertex_id=fn.names[i];if(fn.parameters[i].find("instance_id")!=std::string::npos && fn.parameters[i].find("base_instance")==std::string::npos)instance_id=fn.names[i];}
        if(vertex_id.empty()){vertex_id="x64VertexID";extra+=",uint x64VertexID [[vertex_id]]";}
        if(instance_id.empty()){instance_id="x64InstanceID";extra+=",uint x64InstanceID [[instance_id]]";}
        extra+=",device float4* x64Captured [[buffer("+std::to_string(p->capture_buffer)+")]],constant uint2& x64CaptureParams [[buffer("+std::to_string(p->capture_params)+")]]";
        capture.insert(fn.end,extra);
        std::string writes="if("+vertex_id+" < x64CaptureParams.x) { uint x64At=(("+instance_id+"-x64CaptureParams.y)*x64CaptureParams.x+"+vertex_id+")*"+std::to_string(vs.output_varyings.size())+";\n";
        for(unsigned i=0;i<vs.output_varyings.size();++i)writes+="x64Captured[x64At+"+std::to_string(i)+"]="+metal_pack_varying(vs.output_varyings[i],"output."+vs.output_varyings[i].name)+";\n";
        writes+="} return;";metal_replace(capture,"return output;",writes);
        metal_replace(capture,"vertex Mtl_VertexOut xlatMtlMain(","vertex void xlatMtlMain(");
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:capture.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("幾何 VS capture 編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+capture);
        auto pipeline=[MTLRenderPipelineDescriptor new];pipeline.vertexFunction=[library newFunctionWithName:@"xlatMtlMain"];pipeline.vertexDescriptor=descriptor;pipeline.rasterizationEnabled=NO;pipeline.inputPrimitiveTopology=MTLPrimitiveTopologyClassPoint;
        p->capture=[device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];if(!p->capture)throw std::invalid_argument("幾何 VS capture pipeline："+std::string(error.localizedDescription.UTF8String));

        auto text=gs.source;const auto original=metal_function_text(gs.geometry_original,"x64GeometryMain");
        std::ostringstream code;code<<text<<"\nstruct X64GeometryArgs {uint vertices,primitives,instances,start,indexOffset,indexBytes;int baseVertex;uint topology;};\n";
        code<<"kernel void x64GeometryExpand(";
        std::string arguments;bool comma=false;
        for(unsigned i=0;i<original.parameters.size();++i){const auto& name=original.names[i];if(i)arguments+=",";
            if(name=="input")arguments+="input";else if(name=="x64Emit")arguments+="emitted";
            else {if(comma)code<<",";comma=true;code<<original.parameters[i];arguments+=name;}
        }
        if(comma)code<<",";
        code<<"device const float4* x64Inputs [[buffer("<<p->input_buffer<<")]],device const uchar* x64Indices [[buffer("<<p->index_buffer<<")]],device float4* x64Results [[buffer("<<p->output_buffer<<")]],device uint* x64Counts [[buffer("<<p->counts_buffer<<")]],constant X64GeometryArgs& args [[buffer("<<p->params_buffer<<")]],uint invocation [[thread_position_in_grid]]) {\n";
        code<<"if(invocation>=args.primitives*args.instances)return;x64Counts[invocation]=0;uint primitive=invocation%args.primitives,instance=invocation/args.primitives;\n";
        // GS 可以完全不讀取 VS 輸出；最佳化後沒有輸入結構時，不建立或串接它。
        const bool has_input=std::find(original.names.begin(),original.names.end(),"input")!=original.names.end();
        if(has_input){
        code<<"Mtl_GeometryIn input{};\n";
        code<<"for(uint i=0;i<"<<input_count<<";++i){uint ordinal=args.topology==1?primitive:args.topology==2?primitive*2+i:args.topology==3?primitive+i:args.topology==4?primitive*3+i:args.topology==10?primitive*4+i:args.topology==11?primitive+i:args.topology==12?primitive*6+i:primitive+((primitive&1u) && i<2?1u-i:i); uint vertexIndex=args.start+ordinal; if(args.indexBytes==2)vertexIndex=uint(*reinterpret_cast<device const ushort*>(x64Indices+args.indexOffset+vertexIndex*2))+args.baseVertex;else if(args.indexBytes==4)vertexIndex=(*reinterpret_cast<device const uint*>(x64Indices+args.indexOffset+vertexIndex*4))+args.baseVertex;if(vertexIndex>=args.vertices)return;uint at=(instance*args.vertices+vertexIndex)*"<<vs.output_varyings.size()<<";\n";
        const auto input_begin=text.find("struct Mtl_GeometryIn"),input_end=text.find("};",input_begin);
        if(input_begin==text.npos || input_end==text.npos)throw std::invalid_argument("Metal 幾何輸入宣告不存在");
        const auto input_members=text.substr(input_begin,input_end-input_begin);
        for(const auto& input:gs.input_varyings){
            // 簽章可保留未使用的欄位；只串接 DXBC 宣告所產生的 Metal 成員。
            if(!std::regex_search(input_members,std::regex("\\b"+input.name+"\\s*\\[")))continue;
            const auto linked=std::find_if(vs.output_varyings.begin(),vs.output_varyings.end(),[&](const auto& v){return v.semantic==input.semantic && v.index==input.index;});
            if(linked==vs.output_varyings.end() || linked->type!=input.type || linked->components<input.components)throw std::invalid_argument("VS／GS semantic 不相容："+input.semantic);
            code<<"input."<<input.name<<"[i]="<<metal_unpack_varying(input,"x64Inputs[at+"+std::to_string(linked-vs.output_varyings.begin())+"]")<<";\n";
        }
        code<<"}\n";
        }
        code<<"Mtl_GeometryEmit emitted{};x64GeometryMain("<<arguments<<");uint written=0,stripStart=0;\n";
        code<<"for(uint i=0;i<emitted.count;++i){if(i==0 || emitted.strips[i]!=emitted.strips[i-1])stripStart=i;if(i-stripStart+1<"<<output_width<<")continue;for(uint k=0;k<"<<output_width<<";++k){uint j=i;";
        if(output_width==2)code<<"j=i-1+k;";
        if(output_width==3)code<<"j=i-2+k;if(((i-stripStart)&1u) && k<2)j=i-1-k;";
        code<<"uint at=(invocation*"<<output_capacity<<"+written++)*"<<gs.output_varyings.size()<<";\n";
        for(unsigned i=0;i<gs.output_varyings.size();++i)code<<"x64Results[at+"<<i<<"]="<<metal_pack_varying(gs.output_varyings[i],"emitted.vertices[j]."+gs.output_varyings[i].name)<<";\n";
        code<<"}}x64Counts[invocation]=written;for(;written<"<<output_capacity<<";++written){uint at=(invocation*"<<output_capacity<<"+written)*"<<gs.output_varyings.size()<<";\n";
        for(unsigned i=0;i<gs.output_varyings.size();++i)code<<"x64Results[at+"<<i<<"]="<<(gs.output_varyings[i].system==NAME_POSITION?"float4(2,2,2,1)":"float4(0)")<<";\n";
        code<<"}}\n";const bool position=std::any_of(gs.output_varyings.begin(),gs.output_varyings.end(),[](const auto& v){return v.system==NAME_POSITION;});if(position){code<<"vertex Mtl_GeometryOut x64GeometryDraw(device const float4* data [[buffer(0)]],uint x64Index [[vertex_id]]){Mtl_GeometryOut output{};uint at=x64Index*"<<gs.output_varyings.size()<<";\n";
        for(unsigned i=0;i<gs.output_varyings.size();++i)code<<metal_assign_varying(gs.output_varyings[i],"output."+gs.output_varyings[i].name,"data[at+"+std::to_string(i)+"]");
        code<<"return output;}\n";}auto program=gs.geometry_output==1?code.str():metal_nonpoint_source(code.str());
        library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:program.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("幾何 compute 編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+program);
        p->expand=[device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"x64GeometryExpand"] error:&error];
        p->draw=[library newFunctionWithName:@"x64GeometryDraw"];if(!p->expand || (position && !p->draw))throw std::invalid_argument("幾何 compute pipeline："+std::string(error.localizedDescription.UTF8String));
        if(geometry_pipelines_.size()>=128)geometry_pipelines_.clear();geometry_pipelines_[key]=p;
    }
    std::uint64_t vertices=std::uint64_t{d.start}+d.count;
    id<MTLBuffer> indices=nil;
    if(d.indexed){flush(true);auto& resource=metal_resource(d.indices);indices=resource.buffer;const auto bytes=d.index_format==57?2U:4U;
        if(std::uint64_t{d.index_offset}+(std::uint64_t{d.start}+d.count)*bytes>resource.desc.width)throw std::invalid_argument("幾何索引範圍越界");vertices=0;
        auto data=static_cast<const std::uint8_t*>(indices.contents)+d.index_offset+std::uint64_t{d.start}*bytes;
        if(d.count){const auto range=graphics_index_range(std::span<const std::uint8_t>(data,std::size_t(d.count)*bytes),bytes);if(std::int64_t{range.minimum}+d.base_vertex<0)throw std::invalid_argument("幾何索引 base vertex 越界");vertices=static_cast<std::uint64_t>(std::int64_t{range.maximum}+d.base_vertex)+1;}
        graphics_pending_.push_back(d.indices);
    }
    if(vertices>UINT32_MAX || vertices>UINT64_C(512)*1024*1024/(std::uint64_t{d.instances}*vs.output_varyings.size()*16) || std::uint64_t{primitives}*d.instances>UINT32_MAX/output_capacity)throw std::bad_alloc{};
    const auto output_vertices=std::uint64_t{primitives}*d.instances*output_capacity;
    const auto input_bytes=vertices*d.instances*vs.output_varyings.size()*16,output_bytes=output_vertices*gs.output_varyings.size()*16;
    if(vertices>UINT32_MAX || output_vertices>UINT32_MAX || input_bytes>512*1024*1024 || output_bytes>512*1024*1024)throw std::bad_alloc{};
    const auto count_bytes=std::uint64_t{primitives}*d.instances*4;auto lease=reserve_gpu_memory(input_bytes+output_bytes+count_bytes);
    auto inputs=[device_ newBufferWithLength:input_bytes options:MTLResourceStorageModePrivate];auto outputs=[device_ newBufferWithLength:output_bytes options:gs.stream_output.empty()?MTLResourceStorageModePrivate:MTLResourceStorageModeShared];auto counts=[device_ newBufferWithLength:count_bytes options:MTLResourceStorageModeShared];
    if(!inputs || !outputs || !counts)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));
    begin_commands(true,"geometry");auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.renderTargetWidth=1;pass.renderTargetHeight=1;pass.defaultRasterSampleCount=1;pass.renderTargetArrayLength=1;
    auto capture=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope capture_scope{capture};if(!capture)throw std::runtime_error("幾何 capture encoder 建立失敗");[capture setRenderPipelineState:p->capture];
    graphics_bind(capture,nil,d.stages[0],true);
    for(unsigned i=0;i<32;++i)if(slots[i]>=0){const auto& v=d.vertices[i];[capture setVertexBuffer:metal_resource(v.resource).buffer offset:v.offset atIndex:static_cast<NSUInteger>(slots[i])];graphics_pending_.push_back(v.resource);}
    for(const auto& b:shifted){const auto& v=d.vertices[b[1]];[capture setVertexBuffer:metal_resource(v.resource).buffer offset:std::uint64_t{v.offset}+b[2] atIndex:b[0]];}
    const std::array<std::uint32_t,2> capture_params{static_cast<std::uint32_t>(vertices),d.start_instance};
    [capture setVertexBuffer:inputs offset:0 atIndex:p->capture_buffer];[capture setVertexBytes:capture_params.data() length:sizeof(capture_params) atIndex:p->capture_params];
    if(d.indexed)[capture drawIndexedPrimitives:MTLPrimitiveTypePoint indexCount:d.count indexType:d.index_format==57?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:d.index_offset+std::uint64_t{d.start}*(d.index_format==57?2:4) instanceCount:d.instances baseVertex:d.base_vertex baseInstance:d.start_instance];
    else [capture drawPrimitives:MTLPrimitiveTypePoint vertexStart:d.start vertexCount:d.count instanceCount:d.instances baseInstance:d.start_instance];capture_scope.end();
    auto compute=[commands_ computeCommandEncoder];MetalEncoderScope compute_scope{compute};[compute setComputePipelineState:p->expand];graphics_bind(nil,compute,d.stages[2],false);
    const std::array<std::uint32_t,8> args{static_cast<std::uint32_t>(vertices),primitives,d.instances,d.start,d.index_offset,d.indexed?(d.index_format==57?2U:4U):0U,static_cast<std::uint32_t>(d.base_vertex),d.topology};
    [compute setBuffer:inputs offset:0 atIndex:p->input_buffer];[compute setBuffer:indices?indices:inputs offset:0 atIndex:p->index_buffer];[compute setBuffer:outputs offset:0 atIndex:p->output_buffer];[compute setBuffer:counts offset:0 atIndex:p->counts_buffer];[compute setBytes:args.data() length:sizeof(args) atIndex:p->params_buffer];
    [compute dispatchThreads:MTLSizeMake(std::uint64_t{primitives}*d.instances,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(64,p->expand.maxTotalThreadsPerThreadgroup),1,1)];compute_scope.end();
    if(!gs.stream_output.empty() && !graphics_stream_output(d,gs,outputs,primitives*d.instances,output_capacity,output_width)){
        flush(true);metal_stream_output(d,gs,static_cast<const std::uint8_t*>(outputs.contents),static_cast<const std::uint32_t*>(counts.contents),primitives*d.instances,output_capacity,output_width);
    }
    return {p->draw,outputs,static_cast<unsigned>(output_vertices),output_width==1?MTLPrimitiveTypePoint:output_width==2?MTLPrimitiveTypeLine:MTLPrimitiveTypeTriangle};
}}
