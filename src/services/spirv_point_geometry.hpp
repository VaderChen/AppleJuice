#pragma once

#include <spirv.hpp>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm {
// 將單一點輸入、單一點輸出改寫為頂點運算，供 SPIRV-Cross 的 compute 頂點擷取使用。
// 每條正常返回路徑都必須在尾端送出唯一頂點，才能直接以 invocation 編號定位輸出。
inline std::vector<std::uint32_t> spirv_point_geometry(std::span<const std::uint32_t> module){
    if(module.size()<5 || module.front()!=spv::MagicNumber)
        throw std::invalid_argument("幾何著色器的 SPIR-V 標頭不合法");
    std::vector<std::uint32_t> output(module.begin(),module.begin()+5);
    unsigned entries=0,functions=0,emissions=0,returns=0;
    bool input_points=false,output_points=false,single_vertex=false,awaiting_return=false;
    for(std::size_t offset=5;offset<module.size();){
        const auto length=module[offset]>>16;
        if(!length || length>module.size()-offset)
            throw std::invalid_argument("幾何著色器的 SPIR-V 指令越界");
        const auto instruction=module.subspan(offset,length);
        offset+=length;
        const auto op=static_cast<spv::Op>(instruction[0]&0xffff);
        if(awaiting_return && op!=spv::OpLine && op!=spv::OpNoLine && op!=spv::OpNop && op!=spv::OpReturn)
            throw std::invalid_argument("送出頂點後仍有幾何運算");
        if(op==spv::OpCapability && length==2 && instruction[1]==spv::CapabilityGeometry){
            output.insert(output.end(),{instruction[0],spv::CapabilityShader});
            continue;
        }
        if(op==spv::OpEntryPoint){
            if(length<4 || instruction[1]!=spv::ExecutionModelGeometry || ++entries!=1)
                throw std::invalid_argument("需要單一幾何入口");
            output.insert(output.end(),{instruction[0],spv::ExecutionModelVertex});
            output.insert(output.end(),instruction.begin()+2,instruction.end());
            continue;
        }
        if(op==spv::OpExecutionMode){
            if(length==3 && instruction[2]==spv::ExecutionModeInputPoints)input_points=true;
            else if(length==3 && instruction[2]==spv::ExecutionModeOutputPoints)output_points=true;
            else if(length==4 && instruction[2]==spv::ExecutionModeOutputVertices && instruction[3]==1)single_vertex=true;
            else if(!(length==4 && instruction[2]==spv::ExecutionModeInvocations && instruction[3]==1))
                throw std::invalid_argument("尚未支援此幾何執行模式");
            continue;
        }
        // 點資料擷取沒有光柵插值。vkd3d 會為整數幾何輸入加上 Flat。
        if(op==spv::OpDecorate && length==3 && instruction[2]==spv::DecorationFlat)continue;
        if(op==spv::OpDecorate && length==4 && instruction[2]==spv::DecorationBuiltIn &&
           instruction[3]!=spv::BuiltInPointSize && instruction[3]!=spv::BuiltInPosition)
            throw std::invalid_argument("此幾何系統值需要獨立映射");
        if(op==spv::OpFunction)++functions;
        if(op==spv::OpEmitVertex){++emissions;awaiting_return=true;continue;}
        if(op==spv::OpReturn){
            if(!awaiting_return)throw std::invalid_argument("幾何入口存在未送出頂點的返回路徑");
            ++returns;awaiting_return=false;
        }
        if(op==spv::OpEmitStreamVertex || op==spv::OpEndStreamPrimitive || op==spv::OpEndPrimitive ||
           op==spv::OpKill || op==spv::OpUnreachable || op==spv::OpReturnValue)
            throw std::invalid_argument("點擷取無法保留此幾何控制流程");
        output.insert(output.end(),instruction.begin(),instruction.end());
    }
    if(entries!=1 || functions!=1 || emissions!=1 || returns!=1 || !input_points || !output_points || !single_vertex)
        throw std::invalid_argument("需要單次呼叫且固定送出一點的幾何入口");
    return output;
}
}
