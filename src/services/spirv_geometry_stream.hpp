#pragma once

#include <spirv_cross.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace x64arm {
// 每個 compute invocation 執行一個幾何圖元。輸入按「頂點、簽章暫存器、分量」排列。
// 每個圖元的輸出為頂點數、斷帶次數，接著是各頂點的帶號與暫存器。
// 最後一筆是越額寫入的隔離區，不計入送出的頂點數。
struct GeometryStreamModule {
    struct Interface {uint32_t reg,component,width;bool input,position;};
    std::vector<Interface> interface;
    std::vector<uint32_t> words;
    uint32_t input_vertices{},max_vertices{},input_registers{},output_registers{};
    spv::ExecutionMode output_topology{};
    uint32_t output_vertex_words()const{return 1+4*output_registers;}
    uint32_t output_primitive_words()const{return 2+(max_vertices+1)*output_vertex_words();}
};

inline GeometryStreamModule spirv_geometry_stream(const std::vector<uint32_t>& source){
    using namespace spv;
    if(source.size()<5 || source[0]!=MagicNumber || source[1]>0x00010300)
        throw std::invalid_argument("幾何運算需要 SPIR-V 1.0 至 1.3 模組");
    spirv_cross::Compiler reflect(source);
    const auto entries=reflect.get_entry_points_and_stages();
    if(entries.size()!=1 || entries[0].execution_model!=ExecutionModelGeometry)
        throw std::invalid_argument("幾何運算需要單一幾何入口");
    const auto& modes=reflect.get_execution_mode_bitset();
    GeometryStreamModule result;
    for(const auto& [mode,count]:std::array<std::pair<ExecutionMode,uint32_t>,5>{{
        {ExecutionModeInputPoints,1},{ExecutionModeInputLines,2},{ExecutionModeTriangles,3},
        {ExecutionModeInputLinesAdjacency,4},{ExecutionModeInputTrianglesAdjacency,6}}})
        if(modes.get(mode))result.input_vertices=count;
    for(auto mode:{ExecutionModeOutputPoints,ExecutionModeOutputLineStrip,ExecutionModeOutputTriangleStrip})
        if(modes.get(mode))result.output_topology=mode;
    result.max_vertices=reflect.get_execution_mode_argument(ExecutionModeOutputVertices);
    if(!result.input_vertices || !result.output_topology || !result.max_vertices || result.max_vertices>1024 ||
       (modes.get(ExecutionModeInvocations) && reflect.get_execution_mode_argument(ExecutionModeInvocations)!=1))
        throw std::invalid_argument("幾何拓撲或呼叫數尚未映射");

    using Words=std::vector<uint32_t>;
    Words annotations,globals,prologue,body;
    auto next=source[3];
    auto put=[](Words& dst,Op op,std::initializer_list<uint32_t> operands){
        dst.push_back((static_cast<uint32_t>(operands.size()+1)<<16)|op);
        dst.insert(dst.end(),operands);
    };
    uint32_t uint_type=0,bool_type=0,uint3=0,entry=0;
    std::map<uint32_t,uint32_t> constants;
    struct Field {uint32_t id,type,scalar,width,reg,component;bool input,position;};
    std::vector<Field> fields;
    // 輸入與輸出依 location 和 component 分開保存，支援多個語意共用暫存器。
    for(size_t at=5;at<source.size();){
        const auto* p=source.data()+at;const auto size=p[0]>>16;const auto op=Op(p[0]&0xffff);
        if(!size || size>source.size()-at)throw std::invalid_argument("幾何指令越界");
        if(op==OpTypeInt && p[2]==32 && !p[3])uint_type=p[1];
        if(op==OpTypeBool)bool_type=p[1];
        if(op==OpTypeVector && uint_type && p[2]==uint_type && p[3]==3)uint3=p[1];
        if(op==OpConstant && p[1]==uint_type && size==4)constants[p[3]]=p[2];
        if(op==OpEntryPoint)entry=p[2];
        if(op==OpVariable && (p[3]==StorageClassInput || p[3]==StorageClassOutput)){
            const auto id=p[2];const auto& t=reflect.get_type_from_variable(id);
            const bool input=p[3]==StorageClassInput;
            if(t.columns!=1 || t.width!=32 || t.vecsize>4 ||
               (input?(t.array.size()!=1 || !t.array_size_literal[0] || t.array[0]!=result.input_vertices):!t.array.empty()))
                throw std::invalid_argument("幾何介面需要 32 位元純量或向量");
            bool position=false;
            if(reflect.has_decoration(id,DecorationBuiltIn)){
                const auto builtin=reflect.get_decoration(id,DecorationBuiltIn);
                if(builtin==BuiltInPointSize && !input) {at+=size;continue;}
                if(builtin!=BuiltInPosition)throw std::invalid_argument("幾何系統值未映射");
                position=true;
            }else if(!reflect.has_decoration(id,DecorationLocation))throw std::invalid_argument("幾何介面缺少 location");
            auto reg=reflect.get_decoration(id,DecorationLocation),component=reflect.get_decoration(id,DecorationComponent);
            if(reg>32 || component+t.vecsize>4)throw std::invalid_argument("幾何暫存器範圍不合法");
            uint32_t type=reflect.get_type(p[1]).parent_type;
            if(input)type=reflect.get_type(type).parent_type;
            const auto scalar=t.vecsize==1?type:uint32_t(reflect.get_type(type).parent_type);
            if(!position){auto& count=input?result.input_registers:result.output_registers;count=std::max(count,reg+1);}
            fields.push_back({id,type,scalar,t.vecsize,reg,component,input,position});
        }
        at+=size;
    }
    for(auto& f:fields)if(f.position)f.reg=(f.input?result.input_registers:result.output_registers)++;
    for(const auto& f:fields)result.interface.push_back({f.reg,f.component,f.width,f.input,f.position});
    if(!uint_type){uint_type=next++;put(globals,OpTypeInt,{uint_type,32,0});}
    if(!bool_type){bool_type=next++;put(globals,OpTypeBool,{bool_type});}
    if(!uint3){uint3=next++;put(globals,OpTypeVector,{uint3,uint_type,3});}
    auto constant=[&](uint32_t n){
        auto [it,added]=constants.emplace(n,next);
        if(added){++next;put(globals,OpConstant,{uint_type,it->second,n});}
        return it->second;
    };
    auto pointer=[&](StorageClass storage,uint32_t type){
        const auto id=next++;put(globals,OpTypePointer,{id,storage,type});return id;
    };
    auto value=[&](Words& dst,Op op,uint32_t type,std::initializer_list<uint32_t> args){
        const auto id=next++;dst.push_back((uint32_t(args.size()+3)<<16)|op);
        dst.push_back(type);dst.push_back(id);dst.insert(dst.end(),args);return id;
    };
    const auto zero=constant(0),one=constant(1),array=next++,block=next++;
    put(globals,OpTypeRuntimeArray,{array,uint_type});put(globals,OpTypeStruct,{block,array});
    put(annotations,OpDecorate,{array,DecorationArrayStride,4});
    put(annotations,OpDecorate,{block,DecorationBufferBlock});put(annotations,OpMemberDecorate,{block,0,DecorationOffset,0});
    const auto buffer_ptr=pointer(StorageClassUniform,block),word_ptr=pointer(StorageClassUniform,uint_type);
    const auto inputs=next++,outputs=next++,thread_ptr=pointer(StorageClassInput,uint3),thread=next++;
    for(const auto id:{inputs,outputs}){
        put(globals,OpVariable,{buffer_ptr,id,StorageClassUniform});
        put(annotations,OpDecorate,{id,DecorationDescriptorSet,1});
        put(annotations,OpDecorate,{id,DecorationBinding,id==inputs?0U:1U});
    }
    put(globals,OpVariable,{thread_ptr,thread,StorageClassInput});
    put(annotations,OpDecorate,{thread,DecorationBuiltIn,BuiltInGlobalInvocationId});
    const auto tid3=value(prologue,OpLoad,uint3,{thread});
    const auto tid=value(prologue,OpCompositeExtract,uint_type,{tid3,0});
    const auto in_base=value(prologue,OpIMul,uint_type,{tid,constant(result.input_vertices*result.input_registers*4)});
    const auto out_base=value(prologue,OpIMul,uint_type,{tid,constant(result.output_primitive_words())});
    auto address=[&](Words& dst,uint32_t object,uint32_t base,uint32_t offset){
        const auto index=value(dst,OpIAdd,uint_type,{base,constant(offset)});
        return value(dst,OpAccessChain,word_ptr,{object,zero,index});
    };
    const auto counter=address(prologue,outputs,out_base,0),strip=address(prologue,outputs,out_base,1);
    put(prologue,OpStore,{counter,zero});put(prologue,OpStore,{strip,zero});
    std::map<uint32_t,uint32_t> private_ptrs;
    for(const auto& f:fields)if(f.input){
        auto [it,added]=private_ptrs.emplace(f.type,0);
        if(added)it->second=pointer(StorageClassPrivate,f.type);
        for(uint32_t v=0;v<result.input_vertices;++v){
            Words components;
            for(uint32_t c=0;c<f.width;++c){
                const auto ptr=address(prologue,inputs,in_base,(v*result.input_registers+f.reg)*4+f.component+c);
                const auto bits=value(prologue,OpLoad,uint_type,{ptr});
                components.push_back(f.scalar==uint_type?bits:value(prologue,OpBitcast,f.scalar,{bits}));
            }
            auto packed=components.front();
            if(f.width>1){
                packed=next++;
                prologue.insert(prologue.end(),{((3+f.width)<<16)|OpCompositeConstruct,f.type,packed});
                prologue.insert(prologue.end(),components.begin(),components.end());
            }
            const auto dest=value(prologue,OpAccessChain,it->second,{f.id,constant(v)});
            put(prologue,OpStore,{dest,packed});
        }
    }
    auto emit=[&]{
        const auto n=value(body,OpLoad,uint_type,{counter});
        const auto offset=value(body,OpIMul,uint_type,{n,constant(result.output_vertex_words())});
        const auto base=value(body,OpIAdd,uint_type,{out_base,offset});
        const auto strip_id=value(body,OpLoad,uint_type,{strip});
        put(body,OpStore,{address(body,outputs,base,2),strip_id});
        for(const auto& f:fields)if(!f.input){
            const auto data=value(body,OpLoad,f.type,{f.id});
            for(uint32_t c=0;c<f.width;++c){
                const auto component=f.width==1?data:value(body,OpCompositeExtract,f.scalar,{data,c});
                const auto bits=f.scalar==uint_type?component:value(body,OpBitcast,uint_type,{component});
                put(body,OpStore,{address(body,outputs,base,3+f.reg*4+f.component+c),bits});
            }
        }
        const auto room=value(body,OpULessThan,bool_type,{n,constant(result.max_vertices)});
        const auto after=value(body,OpIAdd,uint_type,{n,one});
        put(body,OpStore,{counter,value(body,OpSelect,uint_type,{room,after,n})});
    };
    bool first_label=false,need_prologue=false;size_t annotations_at=0,globals_at=0;
    for(size_t at=5;at<source.size();){
        auto p=source.data()+at;const auto size=p[0]>>16;const auto op=Op(p[0]&0xffff);at+=size;
        if(!annotations_at && op>=OpTypeVoid && op<=OpTypeFunction)annotations_at=body.size();
        if(!globals_at && op==OpFunction)globals_at=body.size();
        if(need_prologue && op!=OpVariable && op!=OpLine && op!=OpNoLine){body.insert(body.end(),prologue.begin(),prologue.end());need_prologue=false;}
        if(op==OpCapability && p[1]==CapabilityGeometry){put(body,OpCapability,{CapabilityShader});continue;}
        if(op==OpEntryPoint){
            // 名稱是 NUL 結尾，SPIR-V 1.3 的 compute 介面只列出 dispatch ID。
            size_t end=3;while(end<size && (p[end++]&0xff000000U)){}
            body.push_back((uint32_t(end+1)<<16)|op);body.push_back(ExecutionModelGLCompute);
            body.insert(body.end(),p+2,p+end);body.push_back(thread);continue;
        }
        if(op==OpExecutionMode){if(p[2]==ExecutionModeOutputVertices)put(body,op,{entry,ExecutionModeLocalSize,1,1,1});continue;}
        if(op==OpDecorate && (p[2]==DecorationLocation || p[2]==DecorationComponent || p[2]==DecorationBuiltIn || p[2]==DecorationFlat || p[2]==DecorationNoPerspective || p[2]==DecorationCentroid || p[2]==DecorationSample))continue;
        if(op==OpEmitVertex){emit();continue;}
        if(op==OpEndPrimitive){
            const auto n=value(body,OpLoad,uint_type,{strip});put(body,OpStore,{strip,value(body,OpIAdd,uint_type,{n,one})});continue;
        }
        if(op==OpEmitStreamVertex || op==OpEndStreamPrimitive)throw std::invalid_argument("具名幾何輸出串流尚未映射");
        if(op==OpFunction)first_label=p[2]==entry;
        if(op==OpLabel && first_label){first_label=false;need_prologue=true;}
        const auto dest=body.size();body.insert(body.end(),p,p+size);
        if(op==OpTypePointer && (p[2]==StorageClassInput || p[2]==StorageClassOutput))body[dest+2]=StorageClassPrivate;
        if(op==OpVariable && (p[3]==StorageClassInput || p[3]==StorageClassOutput))body[dest+3]=StorageClassPrivate;
    }
    result.words.assign(source.begin(),source.begin()+5);result.words[3]=next;
    auto& dst=result.words;dst.insert(dst.end(),body.begin(),body.begin()+annotations_at);
    dst.insert(dst.end(),annotations.begin(),annotations.end());dst.insert(dst.end(),body.begin()+annotations_at,body.begin()+globals_at);
    dst.insert(dst.end(),globals.begin(),globals.end());dst.insert(dst.end(),body.begin()+globals_at,body.end());
    return result;
}
}
