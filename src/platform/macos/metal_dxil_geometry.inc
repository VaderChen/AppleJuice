// 將已驗證的幾何 compute 核心內嵌在共用展開函式。合成緩衝改為本圖元的 thread 暫存，
// D3D 資源保留原 address space 與綁定位址；外層沿用 VS 擷取、索引、實例、SO 與光柵化。
void metal_dxil_geometry(MetalShader& shader,const GraphicsGeometryStream& layout){
    shader.geometry_input=layout.input_vertices==4?6:layout.input_vertices==6?7:layout.input_vertices;
    shader.geometry_output=layout.output_width==1?1:layout.output_width==2?3:5;
    shader.geometry_max=layout.max_vertices;
    auto source=shader.source;
    const auto fn=metal_function_text(source,"xlatMtlMain");
    std::string helper_params,resources,arguments;
    for(size_t i=0;i<fn.parameters.size();++i){
        const auto& p=fn.parameters[i];const auto& name=fn.names[i];
        if(i){helper_params+=",";arguments+=",";}
        if(name=="ajGeometryInput" || name=="ajGeometryOutput"){
            helper_params+="thread AJGeometryStorage& "+name;
            arguments+=name=="ajGeometryInput"?"packedInput":"packedOutput";
        }else if(p.find("[[thread_position_in_grid]]")!=p.npos){
            helper_params+="uint3 "+name;arguments+="uint3(0)";
        }else{
            if(p.find("[[buffer(")==p.npos && p.find("[[texture(")==p.npos && p.find("[[sampler(")==p.npos)
                throw std::invalid_argument("幾何運算資源參數未映射："+name);
            helper_params+=metal_strip_attributes(p);resources+=","+p;arguments+=name;
        }
    }
    source.replace(fn.begin,fn.end-fn.begin,helper_params);
    metal_replace(source,"kernel void xlatMtlMain(","inline void xlatMtlMain(");
    const auto storage=source.find("struct AJGeometryStorage");
    const auto field=source.find("words[1]",storage);
    if(storage==source.npos || field==source.npos || field>=source.find("};",storage))
        throw std::invalid_argument("幾何暫存結構宣告無效");
    const auto scratch=std::max(layout.input_vertices*layout.input_registers*4,layout.output_primitive_words);
    source.replace(field,8,"words["+std::to_string(scratch)+"]");
    std::ostringstream inputs,outputs,pack,unpack;
    for(const auto& f:layout.interface){
        const auto base=f.component_type==INOUT_COMPONENT_UINT32?"uint":f.component_type==INOUT_COMPONENT_SINT32?"int":"float";
        const auto type=std::string(base)+(f.width>1?std::to_string(f.width):"");
        const auto name=f.position?"mtl_Position":std::string(f.input?"ajIn":"ajOut")+std::to_string(f.reg)+"_"+std::to_string(f.component);
        auto& varyings=f.input?shader.input_varyings:shader.output_varyings;
        varyings.push_back({name,f.semantic,f.index,f.width,f.component_type,f.position?unsigned(NAME_POSITION):0U});
        if(f.input)inputs<<type<<" "<<name<<"["<<layout.input_vertices<<"];\n";
        else outputs<<type<<" "<<name<<(f.position?" [[position]]":" [[user("+name+")]]")<<";\n";
        for(uint32_t c=0;c<f.width;c++){
            auto suffix=f.width==1?std::string{}:"."+std::string(1,"xyzw"[c]);
            const auto index=f.reg*4+f.component+c;
            if(f.input)pack<<"packedInput.words[i*"<<layout.input_registers*4<<"+"<<index<<"]=as_type<uint>(input."<<name<<"[i]"<<suffix<<");\n";
            else unpack<<"x64Emit.vertices[i]."<<name<<suffix<<"=as_type<"<<base<<">(packedOutput.words[base+"<<1+index<<"]);\n";
        }
    }
    std::ostringstream wrapper;
    wrapper<<"\nstruct Mtl_GeometryIn {\n"<<inputs.str()<<"};\nstruct Mtl_GeometryOut {\n"<<outputs.str()<<"};\n"
           <<"struct Mtl_GeometryEmit {Mtl_GeometryOut vertices["<<layout.max_vertices<<"]; uint strips["<<layout.max_vertices<<"],count,strip;};\n"
           <<"void x64GeometryMain(Mtl_GeometryIn input,thread Mtl_GeometryEmit& x64Emit"<<resources<<"){\n"
           <<"AJGeometryStorage packedInput{},packedOutput{};for(uint i=0;i<"<<layout.input_vertices<<";++i){\n"<<pack.str()<<"}\n"
           <<"xlatMtlMain("<<arguments<<");x64Emit.count=packedOutput.words[0];\n"
           <<"for(uint i=0;i<x64Emit.count;++i){uint base=2+i*"<<1+layout.output_registers*4<<";"
           <<"x64Emit.strips[i]=packedOutput.words[base];\n"<<unpack.str()<<"}}\n";
    shader.geometry_original=source+wrapper.str();shader.source=metal_geometry_clean(shader.geometry_original);
}
