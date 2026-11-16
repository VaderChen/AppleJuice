# 網域著色器（domain shader）讀取打包了多個語意的 patch 常數暫存器（例如古墓奇兵 2013 極高畫質的細分曲面：
# hull shader 把 TEXCOORD4／TEXCOORD5 打包在 o7.xy／o7.zw，domain shader 以 vpc7.xyzw 整個讀取）。
# 原版 OPERAND_TYPE_INPUT_PATCH_CONSTANT 直接以第一個語意命名，不看已宣告的合併變數，產生 float2 當 float4 用而 Metal 編譯失敗。

x64arm_hlslcc_patch(toMetalOperand.cpp
  [==[                default:
                    const std::string patchPrefix = "patch.";

                    if (psContext->psShader->eShaderType == DOMAIN_SHADER)
                        oss << psContext->inputPrefix << patchPrefix << psIn->semanticName << psIn->ui32SemanticIndex;]==]
  [==[                default:
                    const std::string patchPrefix = "patch.";

                    // x64arm：同一個 patch 常數暫存器打包多個語意（例如 TEXCOORD4 在 .xy、TEXCOORD5 在 .zw）而被跨語意讀取時，
                    // 宣告階段的 HandleInputRedirect 已組出 phaseN_Input1_R＝float4(各語意)，並把重導標記設為 0xfe；
                    // 讀取改用合併變數（與一般輸入的 GetDeclaredInputName 規則一致），否則只會取到第一個語意而型別不符。
                    if (psContext->psShader->eShaderType == DOMAIN_SHADER &&
                        psContext->psShader->asPhases[psContext->currentPhase].acPatchConstantsNeedsRedirect[psOperand->ui32RegisterNumber] == 0xfe)
                    {
                        oss << "phase" << psContext->currentPhase << "_Input1_" << psOperand->ui32RegisterNumber;
                        *piRebase = 0;
                        break;
                    }

                    if (psContext->psShader->eShaderType == DOMAIN_SHADER)
                        oss << psContext->inputPrefix << patchPrefix << psIn->semanticName << psIn->ui32SemanticIndex;]==])
