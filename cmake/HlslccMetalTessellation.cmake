# 細分階段可能包含沒有一般 signature 的系統暫存器；不讓空反射造成宿主崩潰。
set(TESS_DECL "${hlslcc_SOURCE_DIR}/src/toMetalDeclaration.cpp")
file(READ "${TESS_DECL}" TESS_TEXT)
set(TESS_OLD [=[    switch (psOperand->eType)
    {
        case OPERAND_TYPE_INPUT_COVERAGE_MASK:]=])
set(TESS_NEW [=[    if (!sig && (psOperand->eType == OPERAND_TYPE_INPUT || psOperand->eType == OPERAND_TYPE_OUTPUT || psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT || psOperand->eType == OPERAND_TYPE_INPUT_PATCH_CONSTANT))
        throw std::runtime_error("Metal missing signature: stage=" + std::to_string(psContext->psShader->eShaderType) + " phase=" + std::to_string(psContext->currentPhase) + " operand=" + std::to_string(psOperand->eType) + " register=" + std::to_string(psOperand->ui32RegisterNumber) + " mask=" + std::to_string(psOperand->GetAccessMask()));
    switch (psOperand->eType)
    {
        case OPERAND_TYPE_INPUT_COVERAGE_MASK:]=])
if(NOT TESS_TEXT MATCHES "Metal missing signature:")
  string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
  if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
    file(WRITE "${TESS_DECL}" "#include <stdexcept>\n${TESS_PATCHED}")
  endif()
endif()

file(READ "${TESS_DECL}" TESS_TEXT)
set(TESS_OLD [=[        std::string name = psBinding->name;
        uint32_t ui32ArrayOffset = ui32RegisterNumber - psBinding->ui32BindPoint;]=])
set(TESS_NEW [=[        std::string name = psBinding->name;
        if ((psContext->flags & HLSLCC_FLAG_METAL_TESSELLATION) != 0)
            name = "x64s" + std::to_string(psContext->psShader->eShaderType) + "_" + name;
        uint32_t ui32ArrayOffset = ui32RegisterNumber - psBinding->ui32BindPoint;]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_DECL}" "${TESS_PATCHED}")
endif()

# Register 號與 stage namespace 分開編碼，避免稀疏 b/t 暫存器碰撞。
set(TESS_HEADER "${hlslcc_SOURCE_DIR}/include/hlslcc.h")
file(READ "${TESS_HEADER}" TESS_TEXT)
string(REPLACE "uint32_t key = (m_ShaderStageAllocations + regNo) | (uint32_t(type) << 16);"
  "uint32_t key = regNo | (m_ShaderStageAllocations << 16) | (uint32_t(type) << 24);" TESS_PATCHED "${TESS_TEXT}")
string(REPLACE "uint32_t key = regNo | (uint32_t(type) << 16);"
  "uint32_t key = regNo | (uint32_t(type) << 24);" TESS_PATCHED "${TESS_PATCHED}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_HEADER}" "${TESS_PATCHED}")
endif()

# Domain point 等系統輸入沒有一般暫存器；不能被控制點的 register 0 去重規則略過。
file(READ "${TESS_DECL}" TESS_TEXT)
string(REPLACE
  "if (psDecl->eOpcode == OPCODE_DCL_INPUT && psShader->aIndexedInput[psOperand->GetRegisterSpace(psContext)][psDecl->asOperands[0].ui32RegisterNumber] == -1)"
  "if (psDecl->eOpcode == OPCODE_DCL_INPUT && (psOperand->eType == OPERAND_TYPE_INPUT || psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT || psOperand->eType == OPERAND_TYPE_INPUT_PATCH_CONSTANT) && psShader->aIndexedInput[psOperand->GetRegisterSpace(psContext)][psDecl->asOperands[0].ui32RegisterNumber] == -1)"
  TESS_PATCHED "${TESS_TEXT}")
string(REPLACE
  "if ((ui32CompMask != 0) && ((ui32CompMask & ~psShader->acInputDeclared[0][ui32Reg]) == 0))"
  "if ((psOperand->eType == OPERAND_TYPE_INPUT || psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT || psOperand->eType == OPERAND_TYPE_INPUT_PATCH_CONSTANT) && (ui32CompMask != 0) && ((ui32CompMask & ~psShader->acInputDeclared[psOperand->GetRegisterSpace(psContext)][ui32Reg]) == 0))"
  TESS_PATCHED "${TESS_PATCHED}")
string(REPLACE "psShader->acInputDeclared[0][ui32Reg] = (char)psSig->ui32Mask;"
  "psShader->acInputDeclared[regSpace][ui32Reg] = (char)psSig->ui32Mask;" TESS_PATCHED "${TESS_PATCHED}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_DECL}" "${TESS_PATCHED}")
endif()

# D3D 的各 shader stage 有獨立的 cbuffer 綁定；名稱相同不代表同一份資源。
set(TESS_METAL "${hlslcc_SOURCE_DIR}/src/toMetal.cpp")
file(READ "${TESS_METAL}" TESS_TEXT)
set(TESS_OLD [=[std::string ToMetal::GetCBName(const std::string& cbName) const
{
    std::string output = cbName;]=])
set(TESS_NEW [=[std::string ToMetal::GetCBName(const std::string& cbName) const
{
    if ((psContext->flags & HLSLCC_FLAG_METAL_TESSELLATION) != 0)
        return "x64s" + std::to_string(psContext->psShader->eShaderType) + "_" + (cbName[0] == '$' ? cbName.substr(1) : cbName);
    std::string output = cbName;]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_METAL}" "${TESS_PATCHED}")
endif()

file(READ "${TESS_METAL}" TESS_TEXT)
set(TESS_OLD [=[    if (psShader->eShaderType == HULL_SHADER)
    {
        psContext->currentPhase = MAIN_PHASE;]=])
set(TESS_NEW [=[    if (psShader->eShaderType == HULL_SHADER)
    {
        GenerateTexturesReflection(&psContext->m_Reflection);
        psContext->currentPhase = MAIN_PHASE;]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_METAL}" "${TESS_PATCHED}")
endif()

# HS 的 primitive id 是輸入 patch 編號；DS 由 Metal patch_id 系統值提供。
file(READ "${TESS_DECL}" TESS_TEXT)
set(TESS_OLD [=[        case OPERAND_TYPE_INPUT_DOMAIN_POINT:
            result = "mtl_TessCoord";]=])
set(TESS_NEW [=[        case OPERAND_TYPE_INPUT_PRIMITIVEID:
            result = psContext->psShader->eShaderType == HULL_SHADER ? "patchID" : "mtl_PrimitiveID";
            if (outSkipPrefix) *outSkipPrefix = true;
            if (pui32IgnoreSwizzle) *pui32IgnoreSwizzle = 1;
            return true;
        case OPERAND_TYPE_INPUT_DOMAIN_POINT:
            result = "mtl_TessCoord";]=])
if(NOT TESS_TEXT MATCHES "result = psContext->psShader->eShaderType == HULL_SHADER")
  string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_TEXT "${TESS_TEXT}")
endif()
set(TESS_OLD [=[            // No need to declare patch constants read again by the hull shader.]=])
set(TESS_NEW [=[            if (psOperand->eType == OPERAND_TYPE_INPUT_PRIMITIVEID)
            {
                if (psShader->eShaderType == DOMAIN_SHADER)
                    m_StructDefinitions[""].m_Members.push_back(std::make_pair("mtl_PrimitiveID", "uint mtl_PrimitiveID [[ patch_id ]]"));
                break;
            }
            // No need to declare patch constants read again by the hull shader.]=])
if(NOT TESS_TEXT MATCHES "mtl_PrimitiveID.*patch_id")
  string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_TEXT "${TESS_TEXT}")
endif()
file(READ "${TESS_DECL}" TESS_ORIGINAL)
if(NOT TESS_ORIGINAL STREQUAL TESS_TEXT)
  file(WRITE "${TESS_DECL}" "${TESS_TEXT}")
endif()
set(TESS_OPERAND "${hlslcc_SOURCE_DIR}/src/toMetalOperand.cpp")
file(READ "${TESS_OPERAND}" TESS_TEXT)
set(TESS_OLD [=[        case OPERAND_TYPE_INPUT_PRIMITIVEID:
        {
            // Not supported on Metal
            ASSERT(0);
            break;
        }]=])
set(TESS_NEW [=[        case OPERAND_TYPE_INPUT_PRIMITIVEID:
        {
            if (psContext->psShader->eShaderType == HULL_SHADER) oss << "patchID";
            else if (psContext->psShader->eShaderType == DOMAIN_SHADER) oss << "mtl_PrimitiveID";
            else { ASSERT(0); }
            *pui32IgnoreSwizzle = 1;
            break;
        }]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_OPERAND}" "${TESS_PATCHED}")
endif()

# 相對索引的子 operand 不一定有兩個 token；SV_OutputControlPointID 只有一個。
# 必須使用遞迴解碼回傳的長度，否則會將下一個索引或指令當成暫存器編號。
set(TESS_DECODE "${hlslcc_SOURCE_DIR}/src/decode.cpp")
file(READ "${TESS_DECODE}" TESS_TEXT)
set(TESS_OLD [=[                DecodeOperand(pui32Tokens + ui32NumTokens, psOperand->m_SubOperands[i].get());

                ui32NumTokens++;]=])
set(TESS_NEW [=[                ui32NumTokens += DecodeOperand(pui32Tokens + ui32NumTokens, psOperand->m_SubOperands[i].get()) - 1;]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_DECODE}" "${TESS_PATCHED}")
endif()

file(READ "${TESS_METAL}" TESS_TEXT)
set(TESS_PATCHED "${TESS_TEXT}")
string(REPLACE "bformata(vertexOut, \"Mtl_VertexOut cp[%d]\", psShader->sInfo.ui32TessOutputControlPointCount);"
  "bformata(vertexOut, \"Mtl_VertexOut cp[%d]\", psShader->sInfo.ui32TessInputControlPointCount);" TESS_PATCHED "${TESS_PATCHED}")
set(TESS_OLD [=[                    if (psContext->currentPhase == HS_CTRL_POINT_PHASE && hasControlPointPhase == 1)
                    {
                        psContext->AddIndentation();
                        bcatcstr(bodyglsl, "if (patchValid) {\n");
                        psContext->indent++;

                        psContext->AddIndentation();
                        bformata(bodyglsl, "input.cp[controlPointID] = vertexFunction(%s);\n", tessVertexFunctionArguments.c_str());
                    }]=])
set(TESS_NEW [=[                    if (psPhase->ePhase == HS_CTRL_POINT_PHASE && hasControlPointPhase == 1)
                    {
                        psContext->AddIndentation();
                        bformata(bodyglsl, "if (patchValid && controlPointID < %u) input.cp[controlPointID] = vertexFunction(%s);\n", psShader->sInfo.ui32TessInputControlPointCount, tessVertexFunctionArguments.c_str());
                        psContext->AddIndentation();
                        bcatcstr(bodyglsl, "threadgroup_barrier(mem_flags::mem_threadgroup);\n");
                        psContext->AddIndentation();
                        bformata(bodyglsl, "if (patchValid && controlPointID < %u) {\n", psShader->sInfo.ui32TessOutputControlPointCount);
                        psContext->indent++;
                    }]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_PATCHED}")
string(REPLACE "bcatcstr(bodyglsl, \"controlPoints[mtl_VertexID] = output;\\n\");"
  "bformata(bodyglsl, \"if (controlPointID < %u) controlPoints[internalPatchID * %u + controlPointID] = output;\\n\", psShader->sInfo.ui32TessOutputControlPointCount, psShader->sInfo.ui32TessOutputControlPointCount);" TESS_PATCHED "${TESS_PATCHED}")
string(REPLACE "bcatcstr(bodyglsl, \"tessFactors[internalPatchID] = tessFactor;\\n\");"
  "bcatcstr(bodyglsl, \"if (controlPointID == 0) tessFactors[internalPatchID] = tessFactor;\\n\");" TESS_PATCHED "${TESS_PATCHED}")
string(REPLACE "bcatcstr(bodyglsl, \"patchConstants[internalPatchID] = patch;\\n\");"
  "bcatcstr(bodyglsl, \"if (controlPointID == 0) patchConstants[internalPatchID] = patch;\\n\");" TESS_PATCHED "${TESS_PATCHED}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_METAL}" "${TESS_PATCHED}")
endif()

# HS 沿用 VS 結構而略過宣告時，仍需保存 scalar 型別，避免產生 float.x。
file(READ "${TESS_DECL}" TESS_TEXT)
set(TESS_OLD [=[            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)
            {
                break;
            }]=])
set(TESS_NEW [=[            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)
            {
                if (psOperand->GetNumInputElements(psContext) == 1)
                    psShader->abScalarInput[psOperand->GetRegisterSpace(psContext)][psOperand->ui32RegisterNumber] |= psOperand->GetAccessMask();
                break;
            }]=])
string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
if(NOT TESS_TEXT STREQUAL TESS_PATCHED)
  file(WRITE "${TESS_DECL}" "${TESS_PATCHED}")
endif()
