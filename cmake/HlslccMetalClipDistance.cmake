# 更新本工作先前的前綴修補，保留呼叫端的宣告／運算式區分。
file(READ "${HLSLCC_METAL_DECL}" X64ARM_CLIP_INPUT_SOURCE)
string(REPLACE [=[result = "x64ClipDistance" + std::to_string(sig->ui32SemanticIndex);
                    if (outSkipPrefix) *outSkipPrefix = false;
                    if (iIgnoreRedirect) *iIgnoreRedirect = 1;]=]
  [=[result = "x64ClipDistance" + std::to_string(sig->ui32SemanticIndex);
                    if (outSkipPrefix) *outSkipPrefix = false;]=]
  X64ARM_CLIP_INPUT_PATCHED "${X64ARM_CLIP_INPUT_SOURCE}")
if(NOT X64ARM_CLIP_INPUT_SOURCE STREQUAL X64ARM_CLIP_INPUT_PATCHED)
  file(WRITE "${HLSLCC_METAL_DECL}" "${X64ARM_CLIP_INPUT_PATCHED}")
endif()
# GS 的裁切距離輸入保留 primitive 索引，不能與輸出暫存器共用名稱。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[            case NAME_CLIP_DISTANCE:
            {
                // this is temp variable]=]
  [=[            case NAME_CLIP_DISTANCE:
            {
                if (isInput && psContext->psShader->eShaderType == GEOMETRY_SHADER)
                {
                    result = "x64ClipDistance" + std::to_string(sig->ui32SemanticIndex);
                    if (outSkipPrefix) *outSkipPrefix = false;
                    return true;
                }
                // this is temp variable]=])
# 各簽章可具有不同分量數；按 semantic index 緊密排列原生裁切平面。
file(READ "${HLSLCC_METAL_DECL}" X64ARM_CLIP_SOURCE)
string(FIND "${X64ARM_CLIP_SOURCE}" "void ToMetal::DeclareClipPlanes(" X64ARM_CLIP_START)
string(FIND "${X64ARM_CLIP_SOURCE}" "void ToMetal::GenerateTexturesReflection(" X64ARM_CLIP_END)
if(X64ARM_CLIP_START LESS 0 OR X64ARM_CLIP_END LESS X64ARM_CLIP_START)
  message(FATAL_ERROR "找不到 Metal 裁切平面宣告範圍")
endif()
set(X64ARM_CLIP_BODY [=[void ToMetal::DeclareClipPlanes(const Declaration*, unsigned)
{
    const auto& signatures = psContext->psShader->sInfo.psOutputSignatures;
    unsigned count = 0;
    for (const auto& sig : signatures)
        if (sig.eSystemValueType == NAME_CLIP_DISTANCE)
            for (unsigned c = 0; c < 4; ++c) count += (sig.ui32Mask >> c) & 1;
    if (!count) return;
    std::ostringstream member;
    member << "float mtl_ClipDistance [[ clip_distance ]]";
    if (count > 1) member << "[" << count << "]";
    m_StructDefinitions[GetOutputStructName()].m_Members.push_back(std::make_pair("mtl_ClipDistance", member.str()));
    for (const auto& sig : signatures)
    {
        if (sig.eSystemValueType != NAME_CLIP_DISTANCE) continue;
        unsigned offset = 0;
        for (const auto& previous : signatures)
            if (previous.eSystemValueType == NAME_CLIP_DISTANCE && previous.ui32SemanticIndex < sig.ui32SemanticIndex)
                for (unsigned c = 0; c < 4; ++c) offset += (previous.ui32Mask >> c) & 1;
        const unsigned phase = psContext->currentPhase;
        bformata(GetEarlyMain(psContext), "float4 phase%u_ClipDistance%u;\n", phase, sig.ui32SemanticIndex);
        for (unsigned c = 0; c < 4; ++c)
        {
            if (!(sig.ui32Mask & (1u << c))) continue;
            if (count > 1)
                bformata(GetPostShaderCode(psContext), "output.mtl_ClipDistance[%u] = phase%u_ClipDistance%u.%c;\n", offset++, phase, sig.ui32SemanticIndex, "xyzw"[c]);
            else
                bformata(GetPostShaderCode(psContext), "output.mtl_ClipDistance = phase%u_ClipDistance%u.%c;\n", phase, sig.ui32SemanticIndex, "xyzw"[c]);
        }
    }
}

]=])
string(SUBSTRING "${X64ARM_CLIP_SOURCE}" 0 ${X64ARM_CLIP_START} X64ARM_CLIP_PREFIX)
string(SUBSTRING "${X64ARM_CLIP_SOURCE}" ${X64ARM_CLIP_END} -1 X64ARM_CLIP_SUFFIX)
set(X64ARM_CLIP_PATCHED "${X64ARM_CLIP_PREFIX}${X64ARM_CLIP_BODY}${X64ARM_CLIP_SUFFIX}")
if(NOT X64ARM_CLIP_SOURCE STREQUAL X64ARM_CLIP_PATCHED)
  file(WRITE "${HLSLCC_METAL_DECL}" "${X64ARM_CLIP_PATCHED}")
endif()
