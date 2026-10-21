// RDEF 是可移除的編譯器反射資料；實際資源契約仍保存在 SM4／SM5 宣告。
// 此檔由固定版本 HLSLcc 的 decode.cpp 納入，保留既有反射，只補缺少的資源描述。
#include <stdexcept>

static void x64armDeclaredResources(const uint32_t* words, ShaderInfo& info)
{
    struct Index { uint32_t type, count, value[3]; };
    const auto operand = [](const uint32_t* instruction, uint32_t length, uint32_t& cursor) -> Index {
        if (cursor >= length) throw std::invalid_argument("DXBC 資源 operand 截斷");
        const uint32_t token = instruction[cursor++];
        uint32_t extension = token;
        while (extension & 0x80000000U) {
            if (cursor >= length) throw std::invalid_argument("DXBC 資源 operand extension 截斷");
            extension = instruction[cursor++];
        }
        Index result = { (token >> 12) & 255U, (token >> 20) & 3U, {0,0,0} };
        for (uint32_t i = 0; i < result.count; ++i) {
            if (((token >> (22 + 3*i)) & 7U) || cursor >= length)
                throw std::invalid_argument("DXBC 資源宣告必須使用有效的直接索引");
            result.value[i] = instruction[cursor++];
        }
        return result;
    };
    std::vector<uint32_t> counters;
    const uint32_t count = words[1];
    for (uint32_t at = 2; at < count;) {
        const auto* instruction = words + at;
        const uint32_t opcode = instruction[0] & 0x7ffU;
        const uint32_t length = opcode == OPCODE_CUSTOMDATA && count-at >= 2 ? instruction[1] : (instruction[0] >> 24) & 127U;
        if (!length || length > count-at) throw std::invalid_argument("DXBC 資源宣告長度不合法");
        uint32_t cursor = 1, extension = instruction[0];
        while (extension & 0x80000000U) {
            if (cursor >= length) throw std::invalid_argument("DXBC 資源指令 extension 截斷");
            extension = instruction[cursor++];
        }
        if (opcode == OPCODE_IMM_ATOMIC_ALLOC || opcode == OPCODE_IMM_ATOMIC_CONSUME) {
            (void)operand(instruction,length,cursor);
            const auto resource = operand(instruction,length,cursor);
            if (resource.type != OPERAND_TYPE_UNORDERED_ACCESS_VIEW || resource.count != 1)
                throw std::invalid_argument("DXBC 計數器資源索引不合法");
            counters.push_back(resource.value[0]);
        }
        ResourceType type = RTYPE_COUNT;
        switch (opcode) {
            case OPCODE_DCL_CONSTANT_BUFFER: type=RTYPE_CBUFFER; break;
            case OPCODE_DCL_SAMPLER: type=RTYPE_SAMPLER; break;
            case OPCODE_DCL_RESOURCE: type=RTYPE_TEXTURE; break;
            case OPCODE_DCL_RESOURCE_RAW: type=RTYPE_BYTEADDRESS; break;
            case OPCODE_DCL_RESOURCE_STRUCTURED: type=RTYPE_STRUCTURED; break;
            case OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED: type=RTYPE_UAV_RWTYPED; break;
            case OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW: type=RTYPE_UAV_RWBYTEADDRESS; break;
            case OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED:
                type=(instruction[0] & 0x00800000U)?RTYPE_UAV_RWSTRUCTURED_WITH_COUNTER:RTYPE_UAV_RWSTRUCTURED; break;
            default: break;
        }
        if (type != RTYPE_COUNT) {
            const auto index = operand(instruction,length,cursor);
            const auto group = ShaderInfo::ResourceTypeToResourceGroup(type);
            const ResourceBinding* existing = NULL;
            const uint32_t expected=group==RGROUP_CBUFFER?OPERAND_TYPE_CONSTANT_BUFFER:group==RGROUP_TEXTURE?OPERAND_TYPE_RESOURCE:group==RGROUP_UAV?OPERAND_TYPE_UNORDERED_ACCESS_VIEW:OPERAND_TYPE_SAMPLER;
            if (index.type != expected || index.count != (type==RTYPE_CBUFFER?2U:1U) || index.value[0] >= 128)
                throw std::invalid_argument("DXBC 資源宣告索引不在 SM4／SM5.0 範圍");
            if (!info.GetResourceFromBindingPoint(group,index.value[0],&existing)) {
                ResourceBinding binding = {};
                binding.eType=type;binding.ui32BindPoint=index.value[0];binding.ui32BindCount=1;
                binding.ePrecision=REFLECT_RESOURCE_PRECISION_HIGHP;binding.ui32NumSamples=0xffffffffU;
                // 不同階段的同槽位採獨立型別與名稱，保留管線組合時的識別。
                binding.name="x64stage"+std::to_string(words[0]>>16)+std::string(group==RGROUP_CBUFFER?"cb":group==RGROUP_TEXTURE?"srv":group==RGROUP_UAV?"uav":"sampler")+std::to_string(index.value[0]);
                binding.ui32ReturnType=RETURN_TYPE_UINT;
                binding.eDimension=type==RTYPE_CBUFFER || type==RTYPE_SAMPLER?REFLECT_RESOURCE_DIMENSION_UNKNOWN:REFLECT_RESOURCE_DIMENSION_BUFFER;
                if (type==RTYPE_TEXTURE || type==RTYPE_UAV_RWTYPED) {
                    const REFLECT_RESOURCE_DIMENSION dimensions[] = {
                        REFLECT_RESOURCE_DIMENSION_UNKNOWN,REFLECT_RESOURCE_DIMENSION_BUFFER,REFLECT_RESOURCE_DIMENSION_TEXTURE1D,
                        REFLECT_RESOURCE_DIMENSION_TEXTURE2D,REFLECT_RESOURCE_DIMENSION_TEXTURE2DMS,REFLECT_RESOURCE_DIMENSION_TEXTURE3D,
                        REFLECT_RESOURCE_DIMENSION_TEXTURECUBE,REFLECT_RESOURCE_DIMENSION_TEXTURE1DARRAY,REFLECT_RESOURCE_DIMENSION_TEXTURE2DARRAY,
                        REFLECT_RESOURCE_DIMENSION_TEXTURE2DMSARRAY,REFLECT_RESOURCE_DIMENSION_TEXTURECUBEARRAY
                    };
                    const uint32_t dimension=(instruction[0]>>11)&31U;
                    if (!dimension || dimension>=sizeof(dimensions)/sizeof(dimensions[0]) || cursor>=length)
                        throw std::invalid_argument("DXBC typed resource 宣告截斷或維度不合法");
                    binding.eDimension=dimensions[dimension];binding.ui32ReturnType=static_cast<RESOURCE_RETURN_TYPE>(instruction[cursor]&15U);
                    if (dimension==4 || dimension==9) binding.ui32NumSamples=(instruction[0]>>16)&127U;
                }
                if (type==RTYPE_SAMPLER) {
                    binding.m_SamplerMode=(instruction[0]>>11)&15U;
                    if (binding.m_SamplerMode==1) binding.ui32Flags=2;
                }
                uint32_t size=0;
                if (type==RTYPE_CBUFFER) {
                    if (!index.value[1] || index.value[1]>4096) throw std::invalid_argument("DXBC 常數緩衝區長度不合法");
                    size=index.value[1]*16;
                } else if (type==RTYPE_STRUCTURED || type==RTYPE_UAV_RWSTRUCTURED || type==RTYPE_UAV_RWSTRUCTURED_WITH_COUNTER) {
                    if (cursor>=length || !instruction[cursor] || instruction[cursor]%4 || instruction[cursor]>2048)
                        throw std::invalid_argument("DXBC structured buffer stride 不合法");
                    size=instruction[cursor];
                }
                if (size) {
                    ConstantBuffer buffer = {};buffer.name=binding.name;buffer.ui32TotalSizeInBytes=size;
                    ShaderVar variable = {};variable.name="x64arm_data";variable.ui32Size=size;
                    variable.sType.name=variable.name;variable.sType.fullName=variable.name;
                    variable.sType.Class=type==RTYPE_CBUFFER?SVC_VECTOR:SVC_SCALAR;variable.sType.Type=SVT_UINT;
                    variable.sType.Rows=1;variable.sType.Columns=type==RTYPE_CBUFFER?4:1;
                    variable.sType.Elements=size/(type==RTYPE_CBUFFER?16:4);variable.sType.m_IsUsed=true;
                    buffer.asVars.push_back(variable);
                    info.aui32ResourceMap[group][binding.ui32BindPoint]=static_cast<uint32_t>(info.psConstantBuffers.size());
                    info.psConstantBuffers.push_back(buffer);
                }
                info.psResourceBindings.push_back(binding);
            }
        }
        at += length;
    }
    // APPEND／CONSUME 沒有 _opc 旗標，是否需要計數器由實際原子指令決定。
    for (auto& binding : info.psResourceBindings)
        if (binding.eType==RTYPE_UAV_RWSTRUCTURED)
            for (const auto slot : counters) if (slot==binding.ui32BindPoint) binding.eType=RTYPE_UAV_RWSTRUCTURED_WITH_COUNTER;
}
