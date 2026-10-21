# 以 DXBC 宣告還原可省略的 RDEF 綁定，不修改原始 guest bytecode。
configure_file("${PROJECT_SOURCE_DIR}/src/platform/dxbc_declaration_reflection.inc"
  "${hlslcc_SOURCE_DIR}/src/x64arm_declaration_reflection.inc" COPYONLY)
x64arm_hlslcc_patch(decode.cpp
  "Shader* DecodeDXBC(uint32_t* data, uint32_t decodeFlags)"
  "#include \"x64arm_declaration_reflection.inc\"\n\nShader* DecodeDXBC(uint32_t* data, uint32_t decodeFlags)")
x64arm_hlslcc_patch(decode.cpp
  "        Decode(shaderChunk, psShader);"
  [=[        try {
            if (!refChunks.pui32Resources) x64armDeclaredResources(shaderChunk, psShader->sInfo);
            Decode(shaderChunk, psShader);
        } catch (...) {
            delete psShader;
            throw;
        }]=])
x64arm_hlslcc_patch(ShaderInfo.cpp
  "    const ResourceBinding* psBindings = &psResourceBindings[0];"
  "    const ResourceBinding* psBindings = psResourceBindings.data();")
# RDEF 省略時 MOV 以原始 uint 位元搬移；浮點來源修飾仍須操作 sign bit，不能做整數負值運算。
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[std::string ToMetal::TranslateOperand(const Operand* psOperand, uint32_t ui32TOFlag, uint32_t ui32ComponentMask)
{
    std::ostringstream oss;]=]
  [=[std::string ToMetal::TranslateOperand(const Operand* psOperand, uint32_t ui32TOFlag, uint32_t ui32ComponentMask)
{
    const auto rawType = TypeFlagsToSVTType(ui32TOFlag);
    if (psOperand->eType == OPERAND_TYPE_CONSTANT_BUFFER && psOperand->eModifier != OPERAND_MODIFIER_NONE &&
        !(ui32TOFlag & (TO_FLAG_NAME_ONLY | TO_FLAG_DESTINATION | TO_FLAG_DECLARATION_NAME)) &&
        (rawType == SVT_UINT || rawType == SVT_INT))
    {
        Operand plain = *psOperand; plain.eModifier = OPERAND_MODIFIER_NONE;
        std::string value = TranslateOperand(&plain, TO_FLAG_UNSIGNED_INTEGER, ui32ComponentMask);
        if (psOperand->eModifier == OPERAND_MODIFIER_ABS || psOperand->eModifier == OPERAND_MODIFIER_ABSNEG)
            value = "(" + value + " & 0x7fffffffu)";
        if (psOperand->eModifier == OPERAND_MODIFIER_NEG || psOperand->eModifier == OPERAND_MODIFIER_ABSNEG)
            value = "(" + value + " ^ 0x80000000u)";
        if (rawType == SVT_INT)
            value = "as_type<" + std::string(GetConstructorForTypeMetal(SVT_INT, psOperand->GetNumSwizzleElements(ui32ComponentMask))) + ">(" + value + ")";
        return value;
    }
    std::ostringstream oss;]=])
