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

# 域著色器讀的控制點結構與外殼著色器相同：單一分量的成員宣告成純量，存取時不能再加 swizzle。
file(READ "${TESS_DECL}" TESS_TEXT)
set(TESS_OLD [=[            // ...or control points
            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)]=])
set(TESS_NEW [=[            // 域著色器的控制點同樣要記下純量，宣告流程照舊。
            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == DOMAIN_SHADER && psOperand->GetNumInputElements(psContext) == 1)
                psShader->abScalarInput[psOperand->GetRegisterSpace(psContext)][psOperand->ui32RegisterNumber] |= psOperand->GetAccessMask();
            // ...or control points
            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)]=])
if(NOT TESS_TEXT MATCHES "域著色器的控制點同樣要記下純量")
  string(REPLACE "${TESS_OLD}" "${TESS_NEW}" TESS_PATCHED "${TESS_TEXT}")
  if(TESS_TEXT STREQUAL TESS_PATCHED)
    message(FATAL_ERROR "找不到 Metal 控制點宣告區段")
  endif()
  file(WRITE "${TESS_DECL}" "${TESS_PATCHED}")
endif()
# DS 的 stage_in 同時含控制點與 patch 常數，attribute 編號要互不重複。編號依名稱共用配置，
# patch 常數若和控制點同名（例如都有 TEXCOORD3）就拿到同一個編號；patch 常數改用獨立的名稱空間配置。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[                out += "In";

                std::ostringstream oss;
                oss << type << " " << name;

                // VERTEX_SHADER hardcoded on purpose
                bool keepLocation = ((psContext->flags & HLSLCC_FLAG_KEEP_VARYING_LOCATIONS) != 0);
                uint32_t loc = psContext->psDependencies->GetVaryingLocation(name, VERTEX_SHADER, true, keepLocation, psContext->psShader->maxSemanticIndex);]=]
  [=[                out += "In";

                std::ostringstream oss;
                oss << type << " " << name;

                // VERTEX_SHADER hardcoded on purpose
                bool keepLocation = ((psContext->flags & HLSLCC_FLAG_KEEP_VARYING_LOCATIONS) != 0);
                const std::string x64arm_location_key = out.rfind("Mtl_PatchConstant", 0) == 0 ? "x64arm_patch_" + name : name;
                uint32_t loc = psContext->psDependencies->GetVaryingLocation(x64arm_location_key, VERTEX_SHADER, true, keepLocation, psContext->psShader->maxSemanticIndex);]=])

# PCSG 的 D3D_NAME 與指令 token 的 D3D_SB_NAME 編號不同。讀取細分係數要依簽章的語意索引，
# 與 HS 寫入同一個係數，並保留係數陣列的相對索引。
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[#include <cmath>
#include <sstream>]=]
  [=[#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>]=])
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[            const ShaderInfo::InOutSignature* psIn;
            psContext->psShader->sInfo.GetPatchConstantSignatureFromRegister(psOperand->ui32RegisterNumber, psOperand->GetAccessMask(), &psIn);
            *piRebase = psIn->iRebase;
            switch (psIn->eSystemValueType)]=]
  [=[            const ShaderInfo::InOutSignature* psIn = nullptr;
            psContext->psShader->sInfo.GetPatchConstantSignatureFromRegister(psOperand->ui32RegisterNumber, psOperand->GetAccessMask(), &psIn);
            if (!psIn) throw std::runtime_error("Metal missing patch constant signature");
            *piRebase = psIn->iRebase;
            const auto domain = psContext->psShader->sInfo.eTessDomain;
            if (psContext->psShader->eShaderType == DOMAIN_SHADER &&
                (domain == TESSELLATOR_DOMAIN_TRI || domain == TESSELLATOR_DOMAIN_QUAD) &&
                psIn->eSystemValueType >= NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR &&
                psIn->eSystemValueType <= NAME_FINAL_LINE_DENSITY_TESSFACTOR)
            {
                std::string semantic = psIn->semanticName;
                for (auto& ch : semantic)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (semantic == "sv_tessfactor" || semantic == "sv_insidetessfactor")
                {
                    const bool edge = semantic == "sv_tessfactor";
                    oss << (edge ? "tessFactor.edgeTessellationFactor" : "tessFactor.insideTessellationFactor");
                    if (edge || domain == TESSELLATOR_DOMAIN_QUAD)
                    {
                        oss << "[" << psIn->ui32SemanticIndex;
                        if (psOperand->m_SubOperands[0])
                            oss << " + " << TranslateOperand(psOperand->m_SubOperands[0].get(), TO_FLAG_INTEGER);
                        oss << "]";
                    }
                    *pui32IgnoreSwizzle = 1;
                    break;
                }
            }
            switch (psIn->eSystemValueType)]=])
