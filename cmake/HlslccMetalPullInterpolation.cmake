# SM5 的三種 pull-model 插值共用 Metal interpolant，保留一般輸入的取樣模式。
# EvaluateAttributeSnapped 的低四位為有號 1/16 像素偏移；Metal 使用像素左上角座標。
x64arm_hlslcc_patch(internal_includes/toMetal.h
  "    bool m_ShadowSamplerDeclared;"
  [=[    bool m_ShadowSamplerDeclared;
    std::map<std::string, INTERPOLATION_MODE> m_PullModelInputs;
    std::string GetInputValueName(const Operand*, int*, int, uint32_t*);
]=])
x64arm_hlslcc_patch(toMetal.cpp
  "#include <metal_texture>\\nusing namespace metal;"
  "#include <metal_texture>\\n#include <metal_interpolate>\\nusing namespace metal;")
x64arm_hlslcc_patch(toMetal.cpp
  "    psShader->PruneTempRegisters();"
  [=[    // x64arm: establish all pull attributes before packed-register redirects.
    m_PullModelInputs.clear();
    if (psShader->eShaderType == PIXEL_SHADER)
        for (const auto& phase : psShader->asPhases)
            for (const auto& instruction : phase.psInst)
            {
                if (instruction.eOpcode != OPCODE_EVAL_CENTROID && instruction.eOpcode != OPCODE_EVAL_SAMPLE_INDEX &&
                    instruction.eOpcode != OPCODE_EVAL_SNAPPED) continue;
                const auto& source = instruction.asOperands[1];
                for (unsigned lane = 0; lane < 4; ++lane)
                {
                    if (!(source.GetAccessMask() & (1u << lane))) continue;
                    const ShaderInfo::InOutSignature* signature = nullptr;
                    psShader->sInfo.GetInputSignatureFromRegister(source.ui32RegisterNumber, 1u << lane, &signature, true);
                    if (!signature || signature->eComponentType != INOUT_COMPONENT_FLOAT32) continue;
                    for (const auto& declaration : phase.psDecl)
                        if (declaration.eOpcode == OPCODE_DCL_INPUT_PS &&
                            declaration.asOperands[0].ui32RegisterNumber == source.ui32RegisterNumber &&
                            (declaration.asOperands[0].GetAccessMask() & (1u << lane)) &&
                            declaration.value.eInterpolation != INTERPOLATION_CONSTANT)
                            m_PullModelInputs[signature->semanticName + std::to_string(signature->ui32SemanticIndex)] = declaration.value.eInterpolation;
                }
            }
    psShader->PruneTempRegisters();]=])
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[            std::string declString;
            if ((OPERAND_INDEX_DIMENSION)psOperand->iIndexDims == INDEX_2D]=]
  [=[            // x64arm: promote only attributes used by pull-model instructions.
            if (m_PullModelInputs.count(psSig->semanticName + std::to_string(psSig->ui32SemanticIndex)))
            {
                    const auto mode = psDecl->value.eInterpolation;
                    const bool linear = mode == INTERPOLATION_LINEAR_NOPERSPECTIVE ||
                        mode == INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID || mode == INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE;
                    typeName = "interpolant<" + typeName + ", interpolation::" + (linear ? "no_perspective>" : "perspective>");
                    interpolation.clear();
            }
            std::string declString;
            if ((OPERAND_INDEX_DIMENSION)psOperand->iIndexDims == INDEX_2D]=])
x64arm_hlslcc_patch(toMetal.cpp
  "    // x64arm：幾何著色器由 compute 執行，輸出保留 emit／cut 次序。"
  [=[    // x64arm: original sample-qualified reads still require per-sample invocation.
    for (const auto& input : m_PullModelInputs)
        if (input.second == INTERPOLATION_LINEAR_SAMPLE || input.second == INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE)
        {
            auto& parameters = m_StructDefinitions[""].m_Members;
            if (std::none_of(parameters.begin(), parameters.end(), [](const MemberDefinitions::value_type& member) { return member.first == "mtl_SampleID"; }))
                parameters.push_back(std::make_pair("mtl_SampleID", "uint mtl_SampleID [[ sample_id ]]"));
            break;
        }
    // x64arm：幾何著色器由 compute 執行，輸出保留 emit／cut 次序。]=])

# 一般讀取與 packed-register redirect 都在選取分量前取得原模式的值。
x64arm_hlslcc_patch(toMetalOperand.cpp
  "psContext->GetDeclaredInputName(psOperand, piRebase, 0, pui32IgnoreSwizzle)"
  "GetInputValueName(psOperand, piRebase, 0, pui32IgnoreSwizzle)")
x64arm_hlslcc_patch(toMetalOperand.cpp
  "using namespace HLSLcc;"
  [=[using namespace HLSLcc;

std::string ToMetal::GetInputValueName(const Operand* operand, int* rebase, int ignoreRedirect, uint32_t* ignoreSwizzle)
{
    std::string result = psContext->GetDeclaredInputName(operand, rebase, ignoreRedirect, ignoreSwizzle);
    const std::string prefix = psContext->inputPrefix;
    const auto found = m_PullModelInputs.find(result.compare(0, prefix.size(), prefix) == 0 ? result.substr(prefix.size()) : result);
    if (found == m_PullModelInputs.end()) return result;
    switch (found->second)
    {
        case INTERPOLATION_LINEAR_CENTROID:
        case INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID: return result + ".interpolate_at_centroid()";
        case INTERPOLATION_LINEAR_SAMPLE:
        case INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE: return result + ".interpolate_at_sample(mtl_SampleID)";
        default: return result + ".interpolate_at_center()";
    }
}
]=])

# 每個目的分量獨立解析來源簽章，涵蓋 scalar、swizzle 及同暫存器內的多個屬性。
set(pull_path "${hlslcc_SOURCE_DIR}/src/toMetalInstruction.cpp")
file(READ "${pull_path}" pull_source)
if(TRUE)
  string(FIND "${pull_source}" "        case OPCODE_EVAL_CENTROID:" pull_begin)
  if(pull_begin EQUAL -1)
    message(FATAL_ERROR "HLSLcc pull-model 插值修補位置不符")
  endif()
  string(SUBSTRING "${pull_source}" ${pull_begin} -1 pull_remainder)
  string(FIND "${pull_remainder}" "        case OPCODE_LD_STRUCTURED:" pull_relative_end)
  if(pull_relative_end EQUAL -1)
    message(FATAL_ERROR "HLSLcc pull-model 插值結束位置不符")
  endif()
  math(EXPR pull_end "${pull_begin} + ${pull_relative_end}")
  string(SUBSTRING "${pull_source}" 0 ${pull_begin} pull_head)
  string(SUBSTRING "${pull_source}" ${pull_end} -1 pull_tail)
  set(pull_body [=[        case OPCODE_EVAL_CENTROID:
        case OPCODE_EVAL_SAMPLE_INDEX:
        case OPCODE_EVAL_SNAPPED:
        {
            // x64arm: native pull-model interpolation
            const Operand& destination = psInst->asOperands[0];
            const Operand& source = psInst->asOperands[1];
            const unsigned mask = destination.GetAccessMask();
            const unsigned count = HLSLcc::GetNumberBitsSet(mask);
            psContext->AddIndentation();
            glsl << TranslateOperand(&destination, TO_FLAG_DESTINATION);
            bcatcstr(glsl, " = ");
            if (count > 1) bformata(glsl, "float%u(", count);
            bool first = true;
            for (unsigned lane = 0; lane < 4; ++lane)
            {
                if (!(mask & (1u << lane))) continue;
                if (!first) bcatcstr(glsl, ", ");
                first = false;
                Operand scalar = source;
                scalar.eSelMode = OPERAND_4_COMPONENT_SELECT_1_MODE;
                const unsigned component = source.eSelMode == OPERAND_4_COMPONENT_SELECT_1_MODE ? source.aui32Swizzle[0] :
                    source.eSelMode == OPERAND_4_COMPONENT_SWIZZLE_MODE ? source.aui32Swizzle[lane] : lane;
                scalar.aui32Swizzle[0] = component;
                scalar.ui32Swizzle = component;
                scalar.eModifier = OPERAND_MODIFIER_NONE;
                int rebase = 0;
                uint32_t ignoreSwizzle = 0;
                auto name = psContext->GetDeclaredInputName(&scalar, &rebase, 1, &ignoreSwizzle);
                const std::string prefix = psContext->inputPrefix;
                const ShaderInfo::InOutSignature* signature = nullptr;
                psContext->psShader->sInfo.GetInputSignatureFromRegister(source.ui32RegisterNumber, 1u << component, &signature, true);
                if (signature && signature->eSystemValueType == NAME_UNDEFINED)
                    name = prefix + name;
                const auto found = m_PullModelInputs.find(name.compare(0, prefix.size(), prefix) == 0 ? name.substr(prefix.size()) : name);
                glsl << name;
                if (found != m_PullModelInputs.end())
                {
                    if (psInst->eOpcode == OPCODE_EVAL_CENTROID)
                        bcatcstr(glsl, ".interpolate_at_centroid()");
                    else if (psInst->eOpcode == OPCODE_EVAL_SAMPLE_INDEX)
                    {
                        bcatcstr(glsl, ".interpolate_at_sample(uint(");
                        glsl << TranslateOperand(&psInst->asOperands[2], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                        bcatcstr(glsl, "))");
                    }
                    else
                    {
                        bcatcstr(glsl, ".interpolate_at_offset(float2(((");
                        glsl << TranslateOperand(&psInst->asOperands[2], TO_FLAG_INTEGER | TO_AUTO_EXPAND_TO_VEC2,
                            OPERAND_4_COMPONENT_MASK_X | OPERAND_4_COMPONENT_MASK_Y);
                        bcatcstr(glsl, " & int2(15)) ^ int2(8)) - int2(8)) * 0.0625 + float2(0.5))");
                    }
                }
                if (!ignoreSwizzle && signature && HLSLcc::GetNumberBitsSet(signature->ui32Mask) > 1)
                    glsl << TranslateOperandSwizzle(&scalar, OPERAND_4_COMPONENT_MASK_ALL, rebase);
            }
            if (count > 1) bcatcstr(glsl, ")");
            bcatcstr(glsl, ";\n");
            break;
        }
]=])
  if(NOT pull_source STREQUAL "${pull_head}${pull_body}${pull_tail}")
    file(WRITE "${pull_path}" "${pull_head}${pull_body}${pull_tail}")
  endif()
endif()
