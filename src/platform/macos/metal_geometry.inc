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
    for(const auto& b:shader.bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9)){occupied.insert(b.native_slot);if(b.counter_slot!=UINT32_MAX)occupied.insert(b.counter_slot);}
    if(slots)for(int slot:*slots)if(slot>=0)occupied.insert(static_cast<unsigned>(slot));
    std::vector<unsigned> result;for(unsigned i=0;i<31 && result.size()<count;++i)if(!occupied.contains(i))result.push_back(i);
    if(result.size()!=count)throw std::invalid_argument("Metal 幾何階段 buffer binding 配額不足");return result;
}

#include "metal_stream_output.inc"
#include "metal_stream_output_gpu.inc"
std::pair<unsigned,unsigned> metal_input_primitives(unsigned topology,unsigned count){
    switch(topology){case 1:return {count,1};case 2:return {count/2,2};case 3:return {count>1?count-1:0,2};case 4:return {count/3,3};case 5:return {count>2?count-2:0,3};case 10:return {count/4,4};case 11:return {count>3?count-3:0,4};case 12:return {count/6,6};default:throw std::invalid_argument("幾何輸入 topology 尚未映射");}
}
// 幾何與細分的暫存 buffer 每次繪製都新建的成本很高（AGXBuffer／IOGPUResourceCreate）。
// 依儲存模式分池、長度捨入到 2 的冪後重用；持有者掛在批次的待釋放清單，批次完成才歸還池，在途期間不重用。
// 內容不需清零：capture 只寫被引用的頂點、GS 只讀被引用的頂點，counts 與 results 每格都由 compute 寫入；細分輸出同樣每格由 kernel 寫入。X64ARM_METAL_GEOMETRY_POOL=0 關閉。
std::shared_ptr<MetalPooledBuffer> MetalGl::graphics_scratch_buffer(std::uint64_t bytes,bool shared){
    static const bool geometry_pool=[] {const auto* value=std::getenv("X64ARM_METAL_GEOMETRY_POOL");return !value || std::string_view(value)!="0";}();
    const auto options=shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate;
    auto holder=std::make_shared<MetalPooledBuffer>();
    if(!geometry_pool){holder->buffer=[device_ newBufferWithLength:std::max<std::uint64_t>(bytes,1) options:options];return holder->buffer?holder:nullptr;}
    const auto length=std::max<std::uint64_t>(4096,std::bit_ceil(std::max<std::uint64_t>(bytes,1)));
    const auto& pool=shared?geometry_shared_pool_:geometry_private_pool_;
    holder->buffer=pool->take(static_cast<std::size_t>(length));
    if(!holder->buffer)holder->buffer=[device_ newBufferWithLength:length options:options];
    if(!holder->buffer)return nullptr;
    holder->pool=pool;return holder;
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
    // VS 可以完全沒有輸出（GS 只靠 SV_PrimitiveID 等系統值讀緩衝，如 3DMark Fire Strike 的粒子）：這時不擷取 VS 輸出，只跑展開 kernel；
    // GS 若仍宣告了輸入，下方串接時會因找不到對應的 VS 輸出而拒絕。
    if(gs.output_varyings.empty())throw std::invalid_argument("幾何階段沒有輸出反射");
    const bool capture_vs=!vs.output_varyings.empty();
    const auto key=layout_key+":"+std::to_string(gs.serial);
    std::shared_ptr<MetalGeometryPipeline> p;
    if(const auto found=geometry_pipelines_.find(key);found!=geometry_pipelines_.end())p=found->second;
    else {
        p=std::make_shared<MetalGeometryPipeline>();p->vertex=d.stages[0].shader;p->geometry=d.stages[2].shader;
        auto free=metal_free_buffers(vs,&slots,2);p->capture_buffer=free[0];p->capture_params=free[1];
        free=metal_free_buffers(gs,nullptr,5);p->input_buffer=free[0];p->index_buffer=free[1];p->output_buffer=free[2];p->params_buffer=free[3];p->counts_buffer=free[4];
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;id<MTLLibrary> library=nil;
        if(capture_vs){
        auto capture=vs.source;const auto fn=metal_function_text(capture,"xlatMtlMain");
        std::string vertex_id,instance_id,extra;
        for(unsigned i=0;i<fn.parameters.size();++i){if(fn.parameters[i].find("vertex_id")!=std::string::npos)vertex_id=fn.names[i];if(fn.parameters[i].find("instance_id")!=std::string::npos && fn.parameters[i].find("base_instance")==std::string::npos)instance_id=fn.names[i];}
        if(vertex_id.empty()){vertex_id="x64VertexID";extra+=",uint x64VertexID [[vertex_id]]";}
        if(instance_id.empty()){instance_id="x64InstanceID";extra+=",uint x64InstanceID [[instance_id]]";}
        extra+=",device float4* x64Captured [[buffer("+std::to_string(p->capture_buffer)+")]],constant uint2& x64CaptureParams [[buffer("+std::to_string(p->capture_params)+")]]";
        // 沒有任何參數的 VS（只回傳常數、輸入全被最佳化掉）：參數列是空的，附加的參數不可帶前導逗號。
        if(fn.parameters.empty())extra.erase(0,1);
        capture.insert(fn.end,extra);
        // VS 本體依 D3D 語義把 mtl_VertexID／mtl_InstanceID 減去基底；擷取位置以含基底的原值計算，先保存。
        if(vertex_id=="mtl_VertexID"){metal_replace(capture,"mtl_VertexID = mtl_VertexID - mtl_BaseVertex;","const uint x64RawVertexID = mtl_VertexID;\nmtl_VertexID = mtl_VertexID - mtl_BaseVertex;");vertex_id="x64RawVertexID";}
        if(instance_id=="mtl_InstanceID"){metal_replace(capture,"mtl_InstanceID = mtl_InstanceID - mtl_BaseInstance;","const uint x64RawInstanceID = mtl_InstanceID;\nmtl_InstanceID = mtl_InstanceID - mtl_BaseInstance;");instance_id="x64RawInstanceID";}
        std::string writes="if("+vertex_id+" < x64CaptureParams.x) { uint x64At=(("+instance_id+"-x64CaptureParams.y)*x64CaptureParams.x+"+vertex_id+")*"+std::to_string(vs.output_varyings.size())+";\n";
        for(unsigned i=0;i<vs.output_varyings.size();++i)writes+="x64Captured[x64At+"+std::to_string(i)+"]="+metal_pack_varying(vs.output_varyings[i],"output."+vs.output_varyings[i].name)+";\n";
        writes+="} return;";metal_replace(capture,"return output;",writes);
        metal_replace(capture,"vertex Mtl_VertexOut xlatMtlMain(","vertex void xlatMtlMain(");
        library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:capture.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("幾何 VS capture 編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+capture);
        auto pipeline=[MTLRenderPipelineDescriptor new];pipeline.vertexFunction=[library newFunctionWithName:@"xlatMtlMain"];pipeline.vertexDescriptor=descriptor;pipeline.rasterizationEnabled=NO;pipeline.inputPrimitiveTopology=MTLPrimitiveTopologyClassPoint;
        p->capture=[device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];if(!p->capture)throw std::invalid_argument("幾何 VS capture pipeline："+std::string(error.localizedDescription.UTF8String));
        }

        auto text=gs.source;const auto original=metal_function_text(gs.geometry_original,"x64GeometryMain");
        std::ostringstream code;code<<text<<"\nstruct X64GeometryArgs {uint vertices,primitives,instances,start,indexOffset,indexBytes;int baseVertex;uint topology;};\n";
        code<<"kernel void x64GeometryExpand(";
        std::string arguments;bool comma=false;
        for(unsigned i=0;i<original.parameters.size();++i){const auto& name=original.names[i];if(i)arguments+=",";
            // SV_PrimitiveID：同一個實例內的 primitive 序號（D3D 每個實例重新從 0 起算）。
            if(name=="input")arguments+="input";else if(name=="x64Emit")arguments+="emitted";else if(name=="mtl_PrimitiveID")arguments+="primitive";
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
            // VS 沒有寫的欄位：D3D11 是未定義值，維持 input{} 的零值（VS 完全沒有輸出時也不擷取）。型別或分量數不符仍拒絕。
            if(linked==vs.output_varyings.end())continue;
            if(linked->type!=input.type || linked->components<input.components)throw std::invalid_argument("VS／GS semantic 不相容："+input.semantic);
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
    if(d.indexed){auto& resource=metal_resource(d.indices);
        // 要在 CPU 掃索引範圍：只有索引緩衝可能有未完成的 GPU 寫入時才等全部 GPU 工作，否則每個幾何繪製都會把 GPU 排空。
        // CPU 寫入已在呼叫端完成；待完成的上傳與複製由 gpu_written 記錄，UAV／串流輸出目標一律等待。X64ARM_METAL_GS_INDEX_NOWAIT=0 恢復每次等待。
        static const bool index_nowait=[] {const auto* value=std::getenv("X64ARM_METAL_GS_INDEX_NOWAIT");return !value || std::string_view(value)!="0";}();
        if(!index_nowait || resource.gpu_written || (resource.desc.bind&0x90) || !resource.buffer.contents){flush(true);resource.gpu_written=false;}
        indices=resource.buffer;const auto bytes=d.index_format==57?2U:4U;
        if(std::uint64_t{d.index_offset}+(std::uint64_t{d.start}+d.count)*bytes>resource.desc.width)throw std::invalid_argument("幾何索引範圍越界");vertices=0;
        auto data=static_cast<const std::uint8_t*>(indices.contents)+d.index_offset+std::uint64_t{d.start}*bytes;
        if(d.count){const auto range=graphics_index_range(std::span<const std::uint8_t>(data,std::size_t(d.count)*bytes),bytes);if(std::int64_t{range.minimum}+d.base_vertex<0)throw std::invalid_argument("幾何索引 base vertex 越界");vertices=static_cast<std::uint64_t>(std::int64_t{range.maximum}+d.base_vertex)+1;}
        graphics_pending_.push_back(d.indices);
    }
    if(vertices>UINT32_MAX || (capture_vs && vertices>UINT64_C(512)*1024*1024/(std::uint64_t{d.instances}*vs.output_varyings.size()*16)) || std::uint64_t{primitives}*d.instances>UINT32_MAX/output_capacity)throw std::bad_alloc{};
    const auto output_vertices=std::uint64_t{primitives}*d.instances*output_capacity;
    const auto input_bytes=vertices*d.instances*vs.output_varyings.size()*16,output_bytes=output_vertices*gs.output_varyings.size()*16;
    if(vertices>UINT32_MAX || output_vertices>UINT32_MAX || input_bytes>512*1024*1024 || output_bytes>512*1024*1024)throw std::bad_alloc{};
    const auto count_bytes=std::uint64_t{primitives}*d.instances*4;auto lease=reserve_gpu_memory(input_bytes+output_bytes+count_bytes);
    const auto scratch=[&](std::uint64_t bytes,bool shared)->id<MTLBuffer> {auto holder=graphics_scratch_buffer(bytes,shared);if(!holder)return nil;graphics_pending_.push_back(holder);return holder->buffer;};
    auto inputs=scratch(input_bytes,false);auto outputs=scratch(output_bytes,!gs.stream_output.empty());auto counts=scratch(count_bytes,true);
    if(!inputs || !outputs || !counts)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));
    begin_commands(true,"geometry");
    if(capture_vs){
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.renderTargetWidth=1;pass.renderTargetHeight=1;pass.defaultRasterSampleCount=1;pass.renderTargetArrayLength=1;
    auto capture=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope capture_scope{capture};if(!capture)throw std::runtime_error("幾何 capture encoder 建立失敗");[capture setRenderPipelineState:p->capture];
    graphics_bind(capture,nil,d.stages[0],true);
    for(unsigned i=0;i<32;++i)if(slots[i]>=0){const auto& v=d.vertices[i];[capture setVertexBuffer:metal_resource(v.resource).buffer offset:v.offset atIndex:static_cast<NSUInteger>(slots[i])];graphics_pending_.push_back(v.resource);}
    for(const auto& b:shifted){const auto& v=d.vertices[b[1]];[capture setVertexBuffer:metal_resource(v.resource).buffer offset:std::uint64_t{v.offset}+b[2] atIndex:b[0]];}
    const std::array<std::uint32_t,2> capture_params{static_cast<std::uint32_t>(vertices),d.start_instance};
    [capture setVertexBuffer:inputs offset:0 atIndex:p->capture_buffer];[capture setVertexBytes:capture_params.data() length:sizeof(capture_params) atIndex:p->capture_params];
    if(d.indexed)[capture drawIndexedPrimitives:MTLPrimitiveTypePoint indexCount:d.count indexType:d.index_format==57?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:d.index_offset+std::uint64_t{d.start}*(d.index_format==57?2:4) instanceCount:d.instances baseVertex:d.base_vertex baseInstance:d.start_instance];
    else [capture drawPrimitives:MTLPrimitiveTypePoint vertexStart:d.start vertexCount:d.count instanceCount:d.instances baseInstance:d.start_instance];capture_scope.end();
    }
    auto compute=[commands_ computeCommandEncoder];MetalEncoderScope compute_scope{compute};[compute setComputePipelineState:p->expand];graphics_bind(nil,compute,d.stages[2],false);
    const std::array<std::uint32_t,8> args{static_cast<std::uint32_t>(vertices),primitives,d.instances,d.start,d.index_offset,d.indexed?(d.index_format==57?2U:4U):0U,static_cast<std::uint32_t>(d.base_vertex),d.topology};
    [compute setBuffer:inputs offset:0 atIndex:p->input_buffer];[compute setBuffer:indices?indices:inputs offset:0 atIndex:p->index_buffer];[compute setBuffer:outputs offset:0 atIndex:p->output_buffer];[compute setBuffer:counts offset:0 atIndex:p->counts_buffer];[compute setBytes:args.data() length:sizeof(args) atIndex:p->params_buffer];
    [compute dispatchThreads:MTLSizeMake(std::uint64_t{primitives}*d.instances,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(64,p->expand.maxTotalThreadsPerThreadgroup),1,1)];compute_scope.end();
    if(!gs.stream_output.empty() && !graphics_stream_output(d,gs,outputs,primitives*d.instances,output_capacity,output_width)){
        flush(true);metal_stream_output(d,gs,static_cast<const std::uint8_t*>(outputs.contents),static_cast<const std::uint32_t*>(counts.contents),primitives*d.instances,output_capacity,output_width);
    }
    return {p->draw,outputs,static_cast<unsigned>(output_vertices),output_width==1?MTLPrimitiveTypePoint:output_width==2?MTLPrimitiveTypeLine:MTLPrimitiveTypeTriangle};
}}
// GS 的 mesh 路徑（X64ARM_METAL_GEOMETRY_MESH=0 關閉）：GS 繪製（粒子等）改由 object／mesh 階段執行。VS 沒有輸出時不執行 VS；
// VS 有輸出但沒有頂點緩衝輸入（只用 SV_VertexID／SV_InstanceID 讀資源）且不帶索引時，VS 以命名空間 x64vs 併入 mesh 函式，
// 每個 GS 輸入頂點呼叫一次（D3D 允許同一頂點多次呼叫 VS），資源槽位排在 GS 之後。
// object 階段讀繪製參數（間接繪製直接讀參數緩衝，不讀回 CPU）、計算 primitive 數並展開 mesh 執行緒群組；每個 mesh 執行緒跑一次 GS，
// 輸出的條帶直接轉成圖元送光柵化：不需要暫存緩衝與 compute encoder，繪製可留在開啟中的 render pass。
// 頂點放在每個執行緒的固定槽位；沒用到的圖元槽指向位於裁剪範圍外的頂點，由裁剪丟棄。layer／viewport 在 Metal mesh 是圖元屬性，取自圖元的首頂點。
// 不適用（VS 有頂點輸入、帶索引且 VS 有輸出、細分、串流輸出、點輸出、輸出或槽位超過 mesh 上限、原始碼不是 DXBC 轉譯格式）時回傳 nullptr，改走 compute 展開。
std::shared_ptr<const MetalGeometryMesh> MetalGl::graphics_geometry_mesh(const GraphicsDraw& d){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_METAL_GEOMETRY_MESH");return !value || std::string_view(value)!="0";}();
    if(!enabled || !d.stages[0].shader || !d.stages[2].shader || d.stages[3].shader || d.stages[4].shader)return nullptr;
    if(std::any_of(d.stream_targets.begin(),d.stream_targets.end(),[](const auto& target){return bool(target);}))return nullptr;
    const auto* vs=dynamic_cast<const MetalShader*>(d.stages[0].shader.get());const auto* gs=dynamic_cast<const MetalShader*>(d.stages[2].shader.get());
    if(!vs || !gs || gs->stage!=2 || !gs->stream_output.empty())return nullptr;
    const bool inline_vertex=!vs->output_varyings.empty();if(inline_vertex && d.indexed)return nullptr;
    unsigned expected=0;switch(d.topology){case 1:expected=1;break;case 2:case 3:expected=2;break;case 4:case 5:expected=3;break;case 10:case 11:expected=4;break;case 12:expected=6;break;default:return nullptr;}
    if(expected!=(gs->geometry_input==6?4U:gs->geometry_input==7?6U:gs->geometry_input))return nullptr;
    const std::pair<std::uint64_t,std::uint64_t> key{inline_vertex?vs->serial:0,gs->serial};
    if(const auto found=geometry_meshes_.find(key);found!=geometry_meshes_.end())return found->second;
    if(mesh_supported_<0){mesh_supported_=0;if(@available(macOS 13.0,*))mesh_supported_=[device_ supportsFamily:MTLGPUFamilyMetal3]?1:0;}
    if(geometry_meshes_.size()>=256)geometry_meshes_.clear();
    auto& entry=geometry_meshes_[key];
    if(!mesh_supported_)return nullptr;
    try{@autoreleasepool{
        const unsigned width=gs->geometry_output==1?1:gs->geometry_output==3?2:3,limit_vertices=gs->geometry_max;
        if(width==1 || limit_vertices<width || limit_vertices>255)return nullptr;
        const unsigned primitives=limit_vertices-width+1;
        auto text=metal_nonpoint_source(gs->source);
        // 輸出結構：layer／viewport 欄位移到圖元結構，其餘欄位組成 mesh 頂點結構。
        const auto struct_begin=text.find("struct Mtl_GeometryOut");const auto open=text.find('{',struct_begin),close=text.find("};",open);
        if(struct_begin==text.npos || open==text.npos || close==text.npos)return nullptr;
        std::vector<std::string> kept,fields,primitive_lines,primitive_fields;std::string position_field;bool arrays=false;std::size_t vertex_bytes=0;
        {std::istringstream body(text.substr(open+1,close-open-1));std::string line;const std::regex field(R"((\w+)\s*(\[\s*\d*\s*\])?\s*\[\[)");
            while(std::getline(body,line)){
                std::smatch match;if(line.find("[[")==line.npos || !std::regex_search(line,match,field))continue;
                if(line.find("render_target_array_index")!=line.npos || line.find("viewport_array_index")!=line.npos){primitive_lines.push_back(line);primitive_fields.push_back(match[1]);continue;}
                const auto attribute_end=line.find("]]",line.find("[["));
                if(match[2].matched || (attribute_end!=line.npos && line.find('[',attribute_end)!=line.npos))arrays=true;
                if(line.find("position")!=line.npos && position_field.empty())position_field=match[1];
                kept.push_back(line);fields.push_back(match[1]);vertex_bytes+=line.find("clip_distance")!=line.npos?32:16;
            }}
        if(position_field.empty() || (arrays && !primitive_fields.empty()))return nullptr;
        // mesh 輸出上限 16 KB（頂點、索引與圖元資料合計，這裡保守取 12 KB）、256 個頂點（含一個裁剪用頂點）、512 個圖元。
        const std::size_t per_invocation=limit_vertices*vertex_bytes+primitives*(width+4U*primitive_fields.size());
        const unsigned per_mesh=static_cast<unsigned>(std::min<std::size_t>({32U,255U/limit_vertices,512U/primitives,12288U/per_invocation}));
        if(!per_mesh)return nullptr;
        const bool split=!primitive_fields.empty();const std::string vertex_type=split?"X64MeshVertex":"Mtl_GeometryOut";
        const auto original=metal_function_text(gs->geometry_original,"x64GeometryMain");
        const bool has_input=std::find(original.names.begin(),original.names.end(),"input")!=original.names.end();
        const unsigned input_count=gs->geometry_input==6?4U:gs->geometry_input==7?6U:gs->geometry_input;
        // 併入的 VS：入口改成一般函式、放進命名空間；資源參數改名（x64vs_）、型別加命名空間、槽位排在 GS 已用槽位之後。
        std::string vertex_source,vertex_parameters,vertex_arguments,vertex_inputs;std::array<unsigned,3> offsets{};
        const bool run_vertex=inline_vertex && has_input;
        if(run_vertex){
            auto source=vs->source;
            if(source.find("stage_in")!=source.npos)return nullptr;
            const auto fn=metal_function_text(source,"xlatMtlMain");const auto declaration=source.rfind("vertex",fn.begin);
            if(declaration==source.npos || source.substr(declaration,fn.begin-declaration).find("xlatMtlMain")==std::string::npos)return nullptr;
            const auto used=[](const MetalShader& shader){std::array<unsigned,3> value{};
                for(const auto& b:shader.bindings){if(b.type==2)value[2]=std::max(value[2],b.native_slot+1);else if(b.type==1 || b.type==5)value[1]=std::max(value[1],b.native_slot+1);
                    else{value[0]=std::max(value[0],b.native_slot+1);if(b.counter_slot!=UINT32_MAX)value[0]=std::max(value[0],b.counter_slot+1);}}
                if(shader.buffer_views_slot!=UINT32_MAX)value[0]=std::max(value[0],shader.buffer_views_slot+1);return value;};
            offsets=used(*gs);const auto vertex_used=used(*vs);
            if(offsets[0]+vertex_used[0]>31 || offsets[1]+vertex_used[1]>128 || offsets[2]+vertex_used[2]>16)return nullptr;
            std::vector<std::string> types;{const std::regex declared(R"(\bstruct\s+(\w+))");for(std::sregex_iterator it(source.begin(),source.end(),declared),end;it!=end;++it)types.push_back((*it)[1]);}
            const std::regex resource(R"(\[\[\s*(buffer|texture|sampler)\s*\(\s*(\d+)\s*\)\s*\]\])");
            for(unsigned i=0;i<fn.parameters.size();++i){const auto& parameter=fn.parameters[i];const auto& name=fn.names[i];std::string argument;
                if(parameter.find("vertex_id")!=parameter.npos)argument="x64VertexID";
                else if(parameter.find("base_instance")!=parameter.npos)argument="x64BaseInstance";
                else if(parameter.find("instance_id")!=parameter.npos)argument="x64InstanceID";
                else if(parameter.find("base_vertex")!=parameter.npos)argument="0u";
                else{
                    std::smatch match;if(!std::regex_search(parameter,match,resource))return nullptr;
                    const unsigned kind=match[1]=="buffer"?0U:match[1]=="texture"?1U:2U;
                    auto head=parameter.substr(0,static_cast<std::size_t>(match.position(0)));const auto at=head.rfind(name);if(at==head.npos)return nullptr;
                    auto type=head.substr(0,at);for(const auto& t:types)type=std::regex_replace(type,std::regex("\\b"+t+"\\b"),"x64vs::"+t);
                    vertex_parameters+=","+type+"x64vs_"+name+head.substr(at+name.size())+"[["+match[1].str()+"("+std::to_string(std::stoul(match[2].str())+offsets[kind])+")]]";
                    argument="x64vs_"+name;
                }
                if(i)vertex_arguments+=",";
                vertex_arguments+=argument;
            }
            source=source.substr(0,declaration)+"static Mtl_VertexOut x64VertexMain("+metal_strip_attributes(source.substr(fn.begin,fn.end-fn.begin))+source.substr(fn.end);
            std::istringstream lines(source);std::string line;vertex_source="namespace x64vs {\n";
            while(std::getline(lines,line))if(line.rfind("#include",0)!=0 && line.find("using namespace metal")==line.npos)vertex_source+=line+"\n";
            vertex_source+="}\n";
            // GS 輸入依 semantic 串接 VS 輸出（規則同 compute 展開的擷取）。
            const auto input_begin=text.find("struct Mtl_GeometryIn"),input_end=text.find("};",input_begin);
            if(input_begin==text.npos || input_end==text.npos)return nullptr;
            const auto members=text.substr(input_begin,input_end-input_begin);
            for(const auto& input:gs->input_varyings){
                if(!std::regex_search(members,std::regex("\\b"+input.name+"\\s*\\[")))continue;
                const auto linked=std::find_if(vs->output_varyings.begin(),vs->output_varyings.end(),[&](const auto& v){return v.semantic==input.semantic && v.index==input.index;});
                if(linked==vs->output_varyings.end())continue;
                if(linked->type!=input.type || linked->components<input.components)return nullptr;
                vertex_inputs+="input."+input.name+"[i]="+metal_unpack_varying(input,metal_pack_varying(*linked,"x64V."+linked->name))+";\n";
            }
        }
        std::ostringstream code;code<<text<<"\n"<<vertex_source<<"struct X64MeshPayload {uint base,primitives,total,start,startInstance,topology,pad0,pad1;};\nstruct X64MeshArgs {uint indirect,count,instances,topology,perObject,perMesh,limit,start,startInstance,indexed,pad0,pad1;};\n";
        if(split){
            code<<"struct X64MeshVertex {\n";for(const auto& line:kept)code<<line<<"\n";code<<"};\nstruct X64MeshPrimitive {\n";for(const auto& line:primitive_lines)code<<line<<"\n";code<<"};\n";
        }
        code<<"[[object]] void x64GeometryObject(object_data X64MeshPayload& x64Payload [[payload]],mesh_grid_properties x64Grid,device const uint* x64Indirect [[buffer(0)]],constant X64MeshArgs& x64Args [[buffer(1)]],uint x64Group [[threadgroup_position_in_grid]]){\n"
              "uint count=x64Args.indirect?x64Indirect[0]:x64Args.count,instances=x64Args.indirect?x64Indirect[1]:x64Args.instances,t=x64Args.topology;\n"
              "uint primitives=t==1u?count:t==2u?count/2u:t==3u?(count>1u?count-1u:0u):t==4u?count/3u:t==5u?(count>2u?count-2u:0u):t==10u?count/4u:t==11u?(count>3u?count-3u:0u):t==12u?count/6u:0u;\n"
              "ulong total=min(ulong(primitives)*ulong(instances),ulong(x64Args.limit)),base=ulong(x64Group)*ulong(x64Args.perObject);\n"
              "uint remaining=total>base?uint(min(total-base,ulong(x64Args.perObject))):0u;\n"
              "x64Payload.base=uint(base);x64Payload.primitives=primitives;x64Payload.total=uint(total);x64Payload.topology=t;x64Payload.pad0=0u;x64Payload.pad1=0u;\n"
              "x64Payload.start=x64Args.indirect?x64Indirect[2]:x64Args.start;x64Payload.startInstance=x64Args.indirect?x64Indirect[x64Args.indexed?4:3]:x64Args.startInstance;\n"
              "x64Grid.set_threadgroups_per_grid(uint3((remaining+x64Args.perMesh-1u)/x64Args.perMesh,1u,1u));}\n";
        const unsigned dead=per_mesh*limit_vertices,primitive_capacity=per_mesh*primitives;
        code<<"using X64MeshOutput=metal::mesh<"<<vertex_type<<","<<(split?"X64MeshPrimitive":"void")<<","<<dead+1<<","<<primitive_capacity<<",metal::topology::"<<(width==2?"line":"triangle")<<">;\n";
        code<<"[[mesh]] void x64GeometryMesh(X64MeshOutput x64Mesh,const object_data X64MeshPayload& x64Payload [[payload]]";
        std::string arguments;
        for(unsigned i=0;i<original.parameters.size();++i){const auto& name=original.names[i];if(i)arguments+=",";
            if(name=="input")arguments+="input";else if(name=="x64Emit")arguments+="emitted";else if(name=="mtl_PrimitiveID")arguments+="primitive";
            else {code<<","<<original.parameters[i];arguments+=name;}
        }
        code<<vertex_parameters<<",uint x64Group [[threadgroup_position_in_grid]],uint x64Lane [[thread_index_in_threadgroup]]){\n";
        code<<"const uint x64Dead="<<dead<<"u;if(x64Lane==0u){"<<vertex_type<<" x64DeadVertex{};x64DeadVertex."<<position_field<<"=float4(2,2,2,1);x64Mesh.set_vertex(x64Dead,x64DeadVertex);x64Mesh.set_primitive_count("<<primitive_capacity<<"u);}\n";
        code<<"uint x64Written=0;const uint x64Invocation=x64Payload.base+x64Group*"<<per_mesh<<"u+x64Lane;\n";
        code<<"if(x64Invocation<x64Payload.total){uint primitive=x64Invocation%x64Payload.primitives;\n";
        if(has_input)code<<"Mtl_GeometryIn input{};\n";
        if(run_vertex){
            code<<"const uint x64Instance=x64Invocation/x64Payload.primitives;const uint x64Topology=x64Payload.topology;\n";
            code<<"for(uint i=0;i<"<<input_count<<"u;++i){uint ordinal=x64Topology==1u?primitive:x64Topology==2u?primitive*2u+i:x64Topology==3u?primitive+i:x64Topology==4u?primitive*3u+i:x64Topology==10u?primitive*4u+i:x64Topology==11u?primitive+i:x64Topology==12u?primitive*6u+i:primitive+((primitive&1u) && i<2u?1u-i:i);\n";
            code<<"const uint x64VertexID=x64Payload.start+ordinal,x64InstanceID=x64Payload.startInstance+x64Instance,x64BaseInstance=x64Payload.startInstance;\n";
            code<<"x64vs::Mtl_VertexOut x64V=x64vs::x64VertexMain("<<vertex_arguments<<");\n"<<vertex_inputs<<"}\n";
        }
        code<<"Mtl_GeometryEmit emitted{};x64GeometryMain("<<arguments<<");uint x64StripStart=0;const uint x64Count=min(emitted.count,"<<limit_vertices<<"u);\n";
        code<<"for(uint i=0;i<x64Count;++i){";
        if(split){code<<"X64MeshVertex x64Vertex;";for(const auto& f:fields)code<<"x64Vertex."<<f<<"=emitted.vertices[i]."<<f<<";";code<<"x64Mesh.set_vertex(x64Lane*"<<limit_vertices<<"u+i,x64Vertex);}\n";}
        else code<<"x64Mesh.set_vertex(x64Lane*"<<limit_vertices<<"u+i,emitted.vertices[i]);}\n";
        code<<"for(uint i=0;i<x64Count && x64Written<"<<primitives<<"u;++i){if(i==0 || emitted.strips[i]!=emitted.strips[i-1])x64StripStart=i;if(i-x64StripStart+1<"<<width<<"u)continue;\n";
        code<<"const uint x64Slot=x64Lane*"<<primitives<<"u+x64Written;for(uint k=0;k<"<<width<<"u;++k){uint j=i;";
        if(width==2)code<<"j=i-1+k;";
        if(width==3)code<<"j=i-2+k;if(((i-x64StripStart)&1u) && k<2)j=i-1-k;";
        code<<"x64Mesh.set_index(x64Slot*"<<width<<"u+k,uchar(x64Lane*"<<limit_vertices<<"u+j));}\n";
        if(split){code<<"X64MeshPrimitive x64Primitive;";for(const auto& f:primitive_fields)code<<"x64Primitive."<<f<<"=emitted.vertices[i+1-"<<width<<"u]."<<f<<";";code<<"x64Mesh.set_primitive(x64Slot,x64Primitive);";}
        code<<"++x64Written;}}\n";
        code<<"for(uint p=x64Written;p<"<<primitives<<"u;++p){const uint x64Slot=x64Lane*"<<primitives<<"u+p;for(uint k=0;k<"<<width<<"u;++k)x64Mesh.set_index(x64Slot*"<<width<<"u+k,uchar(x64Dead));";
        if(split)code<<"x64Mesh.set_primitive(x64Slot,X64MeshPrimitive{});";
        code<<"}}\n";
        const auto program=code.str();
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:program.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("幾何 mesh 編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+program);
        auto result=std::make_shared<MetalGeometryMesh>();
        result->object=[library newFunctionWithName:@"x64GeometryObject"];result->mesh=[library newFunctionWithName:@"x64GeometryMesh"];
        if(!result->object || !result->mesh)throw std::invalid_argument("幾何 mesh 缺少入口");
        result->per_mesh=per_mesh;result->per_object=per_mesh*1024U;result->vertex=run_vertex;result->vertex_offsets=offsets;
        // 上限不低於 compute 展開路徑（輸出暫存 512 MiB、32 位元頂點數）可處理的 GS 呼叫數，取整到整個 object 群組。
        const std::uint64_t capacity=std::uint64_t{primitives}*width,compute_limit=std::min<std::uint64_t>(UINT32_MAX/capacity,(UINT64_C(512)<<20)/(capacity*std::max<std::size_t>(1,gs->output_varyings.size())*16));
        result->limit=static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX-result->per_object,(std::max<std::uint64_t>(compute_limit,1)+result->per_object-1)/result->per_object*result->per_object));
        entry=result;
    }}catch(const std::exception& error){
        // 不適用時改走 compute 展開；每個 GS 只記錄一次原因。
        std::fprintf(stderr,"[metal_geometry_mesh] gs=%llu 改走 compute 展開：%.400s\n",static_cast<unsigned long long>(gs->serial),error.what());entry=nullptr;
    }
    return entry;
}
