# Coverage 是獨立的純量輸出，不得與一般 o0 的宣告遮罩／簽章混用。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[void ToMetal::DeclareOutput(const Declaration *psDecl)
{
    Shader* psShader = psContext->psShader;
]=]
  [=[void ToMetal::DeclareOutput(const Declaration *psDecl)
{
    Shader* psShader = psContext->psShader;
    if (psDecl->asOperands[0].eType == OPERAND_TYPE_OUTPUT_COVERAGE_MASK)
    {
        auto& members = m_StructDefinitions[GetOutputStructName()].m_Members;
        for (const auto& member : members)
            if (member.first == "mtl_CoverageMask") return;
        members.push_back(std::make_pair("mtl_CoverageMask", "uint mtl_CoverageMask [[ sample_mask ]]"));
        return;
    }
]=])
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[case OPERAND_TYPE_OUTPUT_COVERAGE_MASK:
        {
            oss << "mtl_CoverageMask";]=]
  [=[case OPERAND_TYPE_OUTPUT_COVERAGE_MASK:
        {
            oss << "output.mtl_CoverageMask";]=])
