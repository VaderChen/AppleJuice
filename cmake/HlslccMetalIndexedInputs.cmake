# 輸入索引範圍（dcl_indexrange）內的暫存器：GLSL 路徑以一個輸入陣列宣告整段，所以略過個別宣告；
# Metal 路徑的索引範圍初始化卻是逐一讀取 input.<名稱>，頂點屬性與片段輸入也必須各自是輸入結構的成員。
# 原本只宣告範圍第一個暫存器，其餘成員不存在而編譯失敗（例如蒙皮著色器的 BLENDWEIGHT1～5）。
# 頂點與像素著色器的一般輸入改為照常宣告；控制點與修補常數輸入維持原行為。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[psShader->aIndexedInput[psOperand->GetRegisterSpace(psContext)][psDecl->asOperands[0].ui32RegisterNumber] == -1)
            {
                break;
            }]=]
  [=[psShader->aIndexedInput[psOperand->GetRegisterSpace(psContext)][psDecl->asOperands[0].ui32RegisterNumber] == -1 &&
                !(psOperand->eType == OPERAND_TYPE_INPUT && (psShader->eShaderType == VERTEX_SHADER || psShader->eShaderType == PIXEL_SHADER)))
            {
                break;
            }]=])
