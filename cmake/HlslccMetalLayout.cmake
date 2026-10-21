# DXBC 以 16-byte 暫存器存取常數緩衝區；直接保留原始位元與 swizzle，
# 避免 Metal struct alignment、陣列尾端共用暫存器及顯式 packoffset 改變位置。
x64arm_hlslcc_patch(DataTypeAnalysis.cpp
  [=[    if (psOperand->eType != OPERAND_TYPE_CONSTANT_BUFFER)
        return;]=]
  [=[    if (psOperand->eType != OPERAND_TYPE_CONSTANT_BUFFER || psContext->psShader->eTargetLanguage == LANG_METAL)
        return;]=])
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[    DeclareStructType(cbname + "_Type", psCBuf->asVars, true, 0, stripUnused);]=]
  [=[    m_StructDefinitions[cbname + "_Type"].m_Members.push_back(std::make_pair("x64arm_data", "uint4 x64arm_data[" + std::to_string(std::max(1U, (psCBuf->ui32TotalSizeInBytes + 15U) / 16U)) + "]"));]=])
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[    if (HLSLcc::IsUnityFlexibleInstancingBuffer(psCBuf))
        oss << "const constant " << psCBuf->asVars[0].name << "_Type* ";
    else
        oss << "constant " << cbname << "_Type& ";]=]
  [=[    oss << "constant " << cbname << "_Type& ";]=])
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[    SHADER_VARIABLE_TYPE eType = psOperand->GetDataType(psContext, requestedType);
    int numComponents = psOperand->GetNumSwizzleElements(ui32CompMask);]=]
  [=[    SHADER_VARIABLE_TYPE eType = psOperand->eType == OPERAND_TYPE_CONSTANT_BUFFER ? SVT_UINT : psOperand->GetDataType(psContext, requestedType);
    int numComponents = psOperand->GetNumSwizzleElements(ui32CompMask);]=])
# 完整取代常數讀取分支，其餘 operand conversion、modifier、swizzle 沿用共用實作。
set(layout_operand "${hlslcc_SOURCE_DIR}/src/toMetalOperand.cpp")
file(READ "${layout_operand}" layout_source)
set(layout_marker "// x64arm: raw DXBC constant registers")
string(FIND "${layout_source}" "${layout_marker}" layout_applied)
if(layout_applied EQUAL -1)
  string(FIND "${layout_source}" "        case OPERAND_TYPE_CONSTANT_BUFFER:" layout_begin)
  string(FIND "${layout_source}" "        case OPERAND_TYPE_RESOURCE:" layout_end)
  if(layout_begin EQUAL -1 OR layout_end LESS layout_begin)
    message(FATAL_ERROR "HLSLcc 常數 operand 修補位置不符")
  endif()
  string(SUBSTRING "${layout_source}" 0 ${layout_begin} layout_head)
  string(SUBSTRING "${layout_source}" ${layout_end} -1 layout_tail)
  set(layout_body [=[        case OPERAND_TYPE_CONSTANT_BUFFER:
        {
            // x64arm: raw DXBC constant registers
            const ConstantBuffer *buffer = nullptr;
            psContext->psShader->sInfo.GetConstantBufferFromBindingPoint(RGROUP_CBUFFER, psOperand->aui32ArraySizes[0], &buffer);
            if (!buffer)
            {
                psContext->m_Reflection.OnDiagnostics("DXBC constant buffer reflection is missing", 0, true);
                oss << "uint4(0)";
                break;
            }
            const auto name = GetCBName(buffer->name);
            if (ui32TOFlag & TO_FLAG_DECLARATION_NAME)
            {
                *pui32IgnoreSwizzle = 1;
                oss << name;
                break;
            }
            oss << name << ".x64arm_data[" << psOperand->aui32ArraySizes[1];
            if (psOperand->m_SubOperands[1])
                oss << "+" << TranslateOperand(psOperand->m_SubOperands[1].get(), TO_FLAG_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            oss << "]";
            break;
        }
]=])
  file(WRITE "${layout_operand}" "${layout_head}${layout_body}${layout_tail}")
endif()
