# 外殼著色器 fork／join 相位讀取的輸出控制點（vocp，UE4 patch constant 函式的 OutputPatch 會用到）。
# 原版 Metal 後端沒有處理：宣告時把 vocp 拿去查輸入簽章而解參考空指標（宿主 SIGSEGV），讀取時只產生空名稱。
# 控制點相位結束時把每個控制點的輸出寫入 threadgroup 陣列，屏障之後 fork／join 相位從陣列讀取同一 patch 的任意控制點。

x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [==[            // ...or control points
            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)
            {]==]
  [==[            // fork／join 相位讀取的輸出控制點（vocp）就是控制點相位的輸出結構，由 threadgroup 陣列提供，不另外宣告。
            if ((psOperand->eType == OPERAND_TYPE_OUTPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)
            {
                break;
            }
            // ...or control points
            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == HULL_SHADER)
            {]==])

x64arm_hlslcc_patch(toMetalOperand.cpp
  [==[        case OPERAND_TYPE_NULL:
        {
            // Null register, used to discard results of operations]==]
  [==[        case OPERAND_TYPE_OUTPUT_CONTROL_POINT:
        {
            // vocp[i][r]：同一 patch 第 i 個控制點相位的輸出；控制點相位結束時已複製到 x64arm_outputControlPoints。
            // 成員名稱與控制點相位宣告輸出（DeclareOutput）時相同：系統值用轉譯後名稱，其餘為語意名稱加索引。
            bool hasControlPointPhase = false;
            for (const auto& phase : psContext->psShader->asPhases)
                hasControlPointPhase = hasControlPointPhase || phase.ePhase == HS_CTRL_POINT_PHASE;
            if (psContext->psShader->eShaderType != HULL_SHADER || !hasControlPointPhase)
                throw std::runtime_error("Metal output control point read without control point phase");
            const ShaderInfo::InOutSignature* psOut = nullptr;
            psContext->psShader->sInfo.GetOutputSignatureFromRegister(psOperand->ui32RegisterNumber, psOperand->GetAccessMask(), 0, &psOut, true);
            if (!psOut)
                throw std::runtime_error("Metal missing output control point signature: register=" + std::to_string(psOperand->ui32RegisterNumber) + " mask=" + std::to_string(psOperand->GetAccessMask()));
            *piRebase = psOut->iRebase;
            std::string member;
            uint32_t ignoreSwizzle = 0;
            int ignoreRedirect = 1;
            if (!TranslateSystemValue(psOperand, psOut, member, &ignoreSwizzle, false, false, nullptr, &ignoreRedirect))
                member = psOut->semanticName + std::to_string(psOut->ui32SemanticIndex);
            oss << "x64arm_outputControlPoints";
            oss << TranslateOperandIndex(psOperand, 0);//Control point index
            oss << "." << member;
            if (ignoreSwizzle || (psContext->psShader->abScalarOutput[0][psOperand->ui32RegisterNumber] & psOperand->GetAccessMask()) != 0)
                *pui32IgnoreSwizzle = 1;
            break;
        }
        case OPERAND_TYPE_NULL:
        {
            // Null register, used to discard results of operations]==])

x64arm_hlslcc_patch(toMetal.cpp
  [==[bool ToMetal::Translate()
{]==]
  [==[// 外殼著色器的 fork／join 相位是否讀取輸出控制點（vocp，例如 UE4 patch constant 函式的 OutputPatch）。
static bool X64ArmHullReadsOutputControlPoints(Shader* psShader)
{
    bool found = false;
    for (auto& phase : psShader->asPhases)
    {
        if (phase.ePhase != HS_FORK_PHASE && phase.ePhase != HS_JOIN_PHASE)
            continue;
        for (const auto& decl : phase.psDecl)
            if (decl.asOperands[0].eType == OPERAND_TYPE_OUTPUT_CONTROL_POINT)
                found = true;
        HLSLcc::ForEachOperand(phase.psInst.begin(), phase.psInst.end(), FEO_FLAG_ALL,
            [&](std::vector<Instruction>::iterator &, Operand *o, uint32_t)
            {
                if (o->eType == OPERAND_TYPE_OUTPUT_CONTROL_POINT)
                    found = true;
            });
    }
    return found;
}

bool ToMetal::Translate()
{]==])

x64arm_hlslcc_patch(toMetal.cpp
  [==[            if (psShader->asPhases[ui32Phase].ePhase == HS_CTRL_POINT_PHASE)
                hasControlPointPhase = 1;
        }
    }
]==]
  [==[            if (psShader->asPhases[ui32Phase].ePhase == HS_CTRL_POINT_PHASE)
                hasControlPointPhase = 1;
        }
    }
    const bool readsOutputControlPoints = psShader->eShaderType == HULL_SHADER && hasControlPointPhase && X64ArmHullReadsOutputControlPoints(psShader);
]==])

x64arm_hlslcc_patch(toMetal.cpp
  [==[        bformata(bodyglsl, "threadgroup %s &input = inputGroup[patchIDInThreadGroup];\n", GetInputStructName().c_str());
]==]
  [==[        bformata(bodyglsl, "threadgroup %s &input = inputGroup[patchIDInThreadGroup];\n", GetInputStructName().c_str());

        if (readsOutputControlPoints)
        {
            psContext->AddIndentation();
            bformata(bodyglsl, "threadgroup Mtl_ControlPoint x64arm_outputControlPointGroup[numPatchesInThreadGroup][%u];\n", psShader->sInfo.ui32TessOutputControlPointCount);
            psContext->AddIndentation();
            bcatcstr(bodyglsl, "threadgroup Mtl_ControlPoint *x64arm_outputControlPoints = x64arm_outputControlPointGroup[patchIDInThreadGroup];\n");
        }
]==])

x64arm_hlslcc_patch(toMetal.cpp
  [==[                if (psShader->asPhases[ui32Phase].ePhase == HS_CTRL_POINT_PHASE)
                {
                    // We're done printing control point phase, run the rest only once per patch
                    psContext->AddIndentation();]==]
  [==[                if (psShader->asPhases[ui32Phase].ePhase == HS_CTRL_POINT_PHASE)
                {
                    if (readsOutputControlPoints)
                    {
                        // fork／join 相位要讀其他控制點的輸出：先寫入 threadgroup 陣列，下方的屏障確保可見。
                        psContext->AddIndentation();
                        bformata(bodyglsl, "if (patchValid && controlPointID < %u) x64arm_outputControlPoints[controlPointID] = output;\n", psShader->sInfo.ui32TessOutputControlPointCount);
                    }
                    // We're done printing control point phase, run the rest only once per patch
                    psContext->AddIndentation();]==])

# 找不到簽章時丟出可捕捉的例外（呼叫端以 try/catch 回報翻譯失敗），不再解參考空指標讓宿主崩潰。
x64arm_hlslcc_patch(HLSLCrossCompilerContext.cpp
  [==[#include "internal_includes/HLSLCrossCompilerContext.h"
#include "internal_includes/HLSLccToolkit.h"]==]
  [==[#include <stdexcept>
#include "internal_includes/HLSLCrossCompilerContext.h"
#include "internal_includes/HLSLccToolkit.h"]==])

x64arm_hlslcc_patch(HLSLCrossCompilerContext.cpp
  [==[    ASSERT(psIn != NULL);
    oss << inputPrefix]==]
  [==[    ASSERT(psIn != NULL);
    if (psIn == NULL)
        throw std::runtime_error("HLSLcc missing input signature: shader=" + std::to_string(psShader->eShaderType) + " phase=" + std::to_string(currentPhase) + " operand=" + std::to_string(psOperand->eType) + " register=" + std::to_string(psOperand->ui32RegisterNumber) + " mask=" + std::to_string(psOperand->GetAccessMask()));
    oss << inputPrefix]==])

x64arm_hlslcc_patch(HLSLCrossCompilerContext.cpp
  [==[    ASSERT(psOut != NULL);

    oss << outputPrefix]==]
  [==[    ASSERT(psOut != NULL);
    if (psOut == NULL)
        throw std::runtime_error("HLSLcc missing output signature: shader=" + std::to_string(psShader->eShaderType) + " phase=" + std::to_string(currentPhase) + " operand=" + std::to_string(psOperand->eType) + " register=" + std::to_string(psOperand->ui32RegisterNumber) + " mask=" + std::to_string(psOperand->GetAccessMask()));

    oss << outputPrefix]==])

# 外殼／域著色器以 v[n][r]（一般輸入型別、二維索引）宣告的控制點輸入：單分量時也要記下純量，
# 否則動態索引讀取（input.cp[r0.x].X）會帶 .x，Metal 不接受純量 swizzle 而編譯失敗。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [==[            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == DOMAIN_SHADER && psOperand->GetNumInputElements(psContext) == 1)
                psShader->abScalarInput[psOperand->GetRegisterSpace(psContext)][psOperand->ui32RegisterNumber] |= psOperand->GetAccessMask();
]==]
  [==[            if ((psOperand->eType == OPERAND_TYPE_INPUT_CONTROL_POINT) && psContext->psShader->eShaderType == DOMAIN_SHADER && psOperand->GetNumInputElements(psContext) == 1)
                psShader->abScalarInput[psOperand->GetRegisterSpace(psContext)][psOperand->ui32RegisterNumber] |= psOperand->GetAccessMask();
            if (psOperand->eType == OPERAND_TYPE_INPUT && psOperand->iIndexDims == INDEX_2D && (psContext->psShader->eShaderType == HULL_SHADER || psContext->psShader->eShaderType == DOMAIN_SHADER) && psOperand->GetNumInputElements(psContext) == 1)
                psShader->abScalarInput[psOperand->GetRegisterSpace(psContext)][psOperand->ui32RegisterNumber] |= psOperand->GetAccessMask();
]==])
