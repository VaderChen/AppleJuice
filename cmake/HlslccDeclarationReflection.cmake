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
# 常數緩衝來源的負號依指令型別解讀：整數指令（iadd、ishl、ult、itof…）的 neg 是二補數取負，
# 只有無型別搬移（mov／movc）與浮點指令才翻轉 sign bit。翻轉 sign bit 用在整數上會得到 x+2^31，
# 例如 Niagara 產生間接參數的 CS 以 ThreadID - NumArgGenTasks 定位清零任務，錯成天文數字而永不清零計數。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[void ToMetal::TranslateInstruction(Instruction* psInst)
{
    bstring glsl = *psContext->currentGLSLString;]=]
  [=[thread_local const Instruction* x64armCurrentInstruction = nullptr;
void ToMetal::TranslateInstruction(Instruction* psInst)
{
    x64armCurrentInstruction = psInst;
    bstring glsl = *psContext->currentGLSLString;]=])
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[        if (psOperand->eModifier == OPERAND_MODIFIER_NEG || psOperand->eModifier == OPERAND_MODIFIER_ABSNEG)
            value = "(" + value + " ^ 0x80000000u)";]=]
  [=[        if (psOperand->eModifier == OPERAND_MODIFIER_NEG || psOperand->eModifier == OPERAND_MODIFIER_ABSNEG)
            value = x64armIntegerInstruction() ? "(0u - " + value + ")" : "(" + value + " ^ 0x80000000u)";]=])
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[std::string ToMetal::TranslateOperand(const Operand* psOperand, uint32_t ui32TOFlag, uint32_t ui32ComponentMask)
{
    const auto rawType = TypeFlagsToSVTType(ui32TOFlag);]=]
  [=[extern thread_local const Instruction* x64armCurrentInstruction;
// DXBC 整數指令：來源 neg 修飾為二補數取負（D3D10 規格），其餘指令沿用浮點 sign bit 語義。
static bool x64armIntegerInstruction()
{
    if (!x64armCurrentInstruction) return false;
    switch (x64armCurrentInstruction->eOpcode)
    {
        case OPCODE_IADD: case OPCODE_IEQ: case OPCODE_IGE: case OPCODE_ILT: case OPCODE_IMAD: case OPCODE_IMAX:
        case OPCODE_IMIN: case OPCODE_IMUL: case OPCODE_INE: case OPCODE_INEG: case OPCODE_ISHL: case OPCODE_ISHR:
        case OPCODE_ITOF: case OPCODE_UDIV: case OPCODE_ULT: case OPCODE_UGE: case OPCODE_UMUL: case OPCODE_UMAD:
        case OPCODE_UMAX: case OPCODE_UMIN: case OPCODE_USHR: case OPCODE_UTOF: case OPCODE_AND: case OPCODE_OR:
        case OPCODE_XOR: case OPCODE_NOT: case OPCODE_UADDC: case OPCODE_USUBB: case OPCODE_BFI: case OPCODE_UBFE:
        case OPCODE_IBFE: case OPCODE_BFREV: case OPCODE_COUNTBITS: case OPCODE_FIRSTBIT_HI: case OPCODE_FIRSTBIT_LO:
        case OPCODE_FIRSTBIT_SHI: case OPCODE_SWITCH: case OPCODE_CASE:
            return true;
        default:
            return false;
    }
}
std::string ToMetal::TranslateOperand(const Operand* psOperand, uint32_t ui32TOFlag, uint32_t ui32ComponentMask)
{
    const auto rawType = TypeFlagsToSVTType(ui32TOFlag);]=])
