# 固定版本 HLSLcc 的 Metal 幾何階段擴充。指令運算沿用共用轉譯器，
# emit／cut 保存成每次 invocation 的串流，再由後端 compute 組裝 primitive。
function(x64arm_hlslcc_patch name before after)
  set(path "${hlslcc_SOURCE_DIR}/src/${name}")
  file(READ "${path}" source)
  string(FIND "${source}" "${after}" applied)
  string(FIND "${source}" "${before}" found)
  string(FIND "${before}" "${after}" deletion)
  # 刪除型修補的 after 也是 before 子字串；須先確認舊片段是否仍存在。
  if(NOT applied EQUAL -1 AND (deletion EQUAL -1 OR found EQUAL -1))
    return()
  endif()
  if(found EQUAL -1)
    message(FATAL_ERROR "HLSLcc Metal 修補位置不符：${name}")
  endif()
  string(REPLACE "${before}" "${after}" patched "${source}")
  file(WRITE "${path}" "${patched}")
endfunction()

x64arm_hlslcc_patch(HLSLcc.cpp
  [=[            // Geometry shader is not supported
            if (psShader->eShaderType == GEOMETRY_SHADER)
            {
                result->sourceCode = "";
                return 0;
            }]=]
  "            // x64arm：幾何階段轉成 Metal compute 使用的共用函式。")

x64arm_hlslcc_patch(toMetal.cpp
  "case VERTEX_SHADER:\n            return \"Mtl_VertexOut\";"
  "case GEOMETRY_SHADER:\n            return \"Mtl_GeometryOut\";\n        case VERTEX_SHADER:\n            return \"Mtl_VertexOut\";")
x64arm_hlslcc_patch(toMetal.cpp
  "case VERTEX_SHADER:\n            return \"Mtl_VertexIn\";"
  "case GEOMETRY_SHADER:\n            return \"Mtl_GeometryIn\";\n        case VERTEX_SHADER:\n            return \"Mtl_VertexIn\";")
x64arm_hlslcc_patch(toMetal.cpp
  "case COMPUTE_SHADER:\n                output = cbName.substr(1);"
  "case GEOMETRY_SHADER:\n                output[0] = 'G';\n                break;\n            case COMPUTE_SHADER:\n                output = cbName.substr(1);")
x64arm_hlslcc_patch(toMetal.cpp
  "case DOMAIN_SHADER:\n            psContext->inputPrefix = \"input.\";"
  "case DOMAIN_SHADER:\n        case GEOMETRY_SHADER:\n            psContext->inputPrefix = \"input.\";")
x64arm_hlslcc_patch(toMetal.cpp
  "case COMPUTE_SHADER:\n            bcatcstr(bodyglsl, \"kernel void computeMain(\\n\");"
  "case GEOMETRY_SHADER:\n            bcatcstr(bodyglsl, \"static void x64GeometryMain(\\n\");\n            break;\n        case COMPUTE_SHADER:\n            bcatcstr(bodyglsl, \"kernel void computeMain(\\n\");")
x64arm_hlslcc_patch(toMetal.cpp
  "    PrintStructDeclarations(m_StructDefinitions);"
  [=[    // x64arm：幾何著色器由 compute 執行，輸出保留 emit／cut 次序。
    if (psShader->eShaderType == GEOMETRY_SHADER)
        m_StructDefinitions[""].m_Members.push_back(std::make_pair("x64Emit", "thread Mtl_GeometryEmit& x64Emit"));
    PrintStructDeclarations(m_StructDefinitions);
    if (psShader->eShaderType == GEOMETRY_SHADER)
    {
        unsigned maximum = 0;
        for (const auto& phase : psShader->asPhases)
            for (const auto& decl : phase.psDecl)
                if (decl.eOpcode == OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT)
                    maximum = decl.value.ui32MaxOutputVertexCount;
        if (!maximum || maximum > 1024) return false;
        bformata(glsl, "struct Mtl_GeometryEmit { Mtl_GeometryOut vertices[%u]; uint strips[%u]; uint count; uint strip; };\n", maximum, maximum);
    }]=])
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  "                    result = \"mtl_Position\";\n                if (outSkipPrefix != NULL) *outSkipPrefix = true;"
  "                    result = \"mtl_Position\";\n                if (outSkipPrefix != NULL) *outSkipPrefix = psContext->psShader->eShaderType != GEOMETRY_SHADER;")
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  "        case VERTEX_SHADER:\n        case DOMAIN_SHADER:\n        case HULL_SHADER:\n        {\n            std::string out = GetOutputStructName();"
  "        case VERTEX_SHADER:\n        case DOMAIN_SHADER:\n        case HULL_SHADER:\n        case GEOMETRY_SHADER:\n        {\n            std::string out = GetOutputStructName();")
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  "        case GEOMETRY_SHADER:\n        default:\n            ASSERT(0);\n            break;\n    }\n    HandleOutputRedirect"
  "        default:\n            ASSERT(0);\n            break;\n    }\n    HandleOutputRedirect")
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  "if (psContext->psShader->eShaderType == PIXEL_SHADER  && !strncmp(psSig->semanticName.c_str(), \"PSIZE\", 5))"
  "if (psContext->psShader->eShaderType == PIXEL_SHADER && !strncmp(psSig->semanticName.c_str(), \"PSIZE\", 5))")
# 幾何輸入是 primitive 內的陣列，純量分量仍保留陣列索引。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  "psContext->psShader->eShaderType == COMPUTE_SHADER || (psContext->psShader->eShaderType == PIXEL_SHADER"
  "psContext->psShader->eShaderType == COMPUTE_SHADER || psContext->psShader->eShaderType == GEOMETRY_SHADER || (psContext->psShader->eShaderType == PIXEL_SHADER")
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        case OPCODE_CUT:
        case OPCODE_EMITTHENCUT_STREAM:
        case OPCODE_EMIT:
        case OPCODE_EMITTHENCUT:
        case OPCODE_CUT_STREAM:
        case OPCODE_EMIT_STREAM:
        {
            ASSERT(0); // Not on metal
        }]=]
  [=[        case OPCODE_CUT:
        case OPCODE_EMIT:
        case OPCODE_EMITTHENCUT:
        case OPCODE_EMITTHENCUT_STREAM:
        case OPCODE_CUT_STREAM:
        case OPCODE_EMIT_STREAM:
        {
            if ((psInst->eOpcode == OPCODE_EMIT_STREAM || psInst->eOpcode == OPCODE_CUT_STREAM || psInst->eOpcode == OPCODE_EMITTHENCUT_STREAM) && psInst->asOperands[0].ui32RegisterNumber != 0)
            {
                psContext->m_Reflection.OnDiagnostics("Metal geometry multi-stream output is not supported", 0, true);
                break;
            }
            if (psInst->eOpcode != OPCODE_CUT && psInst->eOpcode != OPCODE_CUT_STREAM)
            {
                bconcat(glsl, psContext->psShader->asPhases[psContext->currentPhase].postShaderCode);
                bcatcstr(glsl, "x64Emit.vertices[x64Emit.count] = output; x64Emit.strips[x64Emit.count++] = x64Emit.strip;\n");
            }
            if (psInst->eOpcode != OPCODE_EMIT && psInst->eOpcode != OPCODE_EMIT_STREAM)
                bcatcstr(glsl, "++x64Emit.strip;\n");
            break;
        }]=])
