# SM5.1 DXBC 的 range ID／register space 轉成共用 HLSLcc 資源 IR。
configure_file("${PROJECT_SOURCE_DIR}/src/platform/dxbc_sm51_resources.inc"
  "${hlslcc_SOURCE_DIR}/src/x64arm_sm51_resources.inc" COPYONLY)
x64arm_hlslcc_patch(../include/ShaderInfo.h
  "    uint32_t ui32RangeID;"
  "    uint32_t ui32RangeID;\n    uint32_t ui32SourceBindPoint = 0;")
# SM5.1 的大暫存器編號與不同 space 先由正規化器處理，避免反射階段以原始編號配置巨型索引表。
# 判斷必須與正規化器相同（只有 5.1）：舊版寫成「次版本為 0」，SM4.1（vs_4_1／ps_4_1）因此沒有繫結點→常數緩衝對應，
# cb1 以上全對到索引 0 的緩衝，Metal 端重複宣告 CB0、找不到 CB1，CreateVertexShader 失敗（伊蘇 VIII 新遊戲時崩潰）。
# 已修補過的樹先把舊條件換成新條件，下方修補才會判定為已套用。
file(READ "${hlslcc_SOURCE_DIR}/src/reflect.cpp" x64arm_reflect_source)
string(REPLACE "if (psShaderInfo->ui32MinorVersion == 0) "
  "if (psShaderInfo->ui32MajorVersion != 5 || psShaderInfo->ui32MinorVersion != 1) "
  x64arm_reflect_migrated "${x64arm_reflect_source}")
if(NOT x64arm_reflect_migrated STREQUAL x64arm_reflect_source)
  file(WRITE "${hlslcc_SOURCE_DIR}/src/reflect.cpp" "${x64arm_reflect_migrated}")
endif()
x64arm_hlslcc_patch(reflect.cpp
  "        ASSERT(psResBindings[i].ui32BindPoint < MAX_RESOURCE_BINDINGS);"
  "        if (psShaderInfo->ui32MajorVersion != 5 || psShaderInfo->ui32MinorVersion != 1) ASSERT(psResBindings[i].ui32BindPoint < MAX_RESOURCE_BINDINGS);")
x64arm_hlslcc_patch(reflect.cpp
  "                    psShaderInfo->aui32ResourceMap[eRGroup][psResBindings[i].ui32BindPoint] = cbufIndex;"
  "                    if (psShaderInfo->ui32MajorVersion != 5 || psShaderInfo->ui32MinorVersion != 1) psShaderInfo->aui32ResourceMap[eRGroup][psResBindings[i].ui32BindPoint] = cbufIndex;")
x64arm_hlslcc_patch(decode.cpp
  "uint32_t DecodeOperand(const uint32_t *pui32Tokens, Operand* psOperand)"
  "#include \"x64arm_sm51_resources.inc\"\n\nuint32_t DecodeOperand(const uint32_t *pui32Tokens, Operand* psOperand)")
x64arm_hlslcc_patch(decode.cpp
  "    psOperand->specialName = \"\";\n\n    return ui32NumTokens;"
  "    psOperand->specialName = \"\";\n    if (x64armSm51Resources) x64armSm51Resources->operand(*psOperand);\n\n    return ui32NumTokens;")
x64arm_hlslcc_patch(decode.cpp
  "    psDecl->eOpcode = eOpcode;"
  "    X64armSm51Declaration sm51Declaration(pui32Token, ui32TokenLength);\n    psDecl->eOpcode = eOpcode;")
# 移除早期配置曾放在 DecodeDXBC 的外層 scope；正規化只能在 Decode 進行一次。
x64arm_hlslcc_patch(decode.cpp
  [=[            X64armSm51Resources sm51Resources(psShader->sInfo, shaderChunk[0]);
            Decode(shaderChunk, psShader);
            x64armSm51ExpandDeclarations(psShader);]=]
  "            Decode(shaderChunk, psShader);")
x64arm_hlslcc_patch(decode.cpp
  [=[void Decode(const uint32_t* pui32Tokens, Shader* psShader)
{]=]
  [=[void Decode(const uint32_t* pui32Tokens, Shader* psShader)
{
    X64armSm51Resources sm51Resources(psShader->sInfo, pui32Tokens[0]);]=])
x64arm_hlslcc_patch(decode.cpp
  "    DecodeShaderPhase(pui32CurrentToken, psShader, MAIN_PHASE, &psShader->asPhases[0]);"
  "    DecodeShaderPhase(pui32CurrentToken, psShader, MAIN_PHASE, &psShader->asPhases[0]);\n    x64armSm51ExpandDeclarations(psShader);")
