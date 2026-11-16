# Metal 沒有 vertex cull_distance 內建欄位：先完整保留浮點 varying，
# 光柵化前由共用圖元處理路徑依所有頂點的符號剔除整個圖元。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[        case NAME_CLIP_DISTANCE:
            // it will be done separately in DeclareClipPlanes]=]
  [=[        case NAME_CULL_DISTANCE:
        {
            const ShaderInfo::InOutSignature* sig = NULL;
            psContext->psShader->sInfo.GetOutputSignatureFromRegister(psDecl->asOperands[0].ui32RegisterNumber, psDecl->asOperands[0].GetAccessMask(), 0, &sig);
            if (!sig) throw std::invalid_argument("CullDistance output signature missing");
            unsigned count = 0;
            for (unsigned c = 0; c < 4; ++c) count += (sig->ui32Mask >> c) & 1;
            const std::string name = sig->semanticName + std::to_string(sig->ui32SemanticIndex);
            const std::string type = count > 1 ? "float" + std::to_string(count) : "float";
            m_StructDefinitions[out].m_Members.push_back(std::make_pair(name, type + " " + name + " [[ user(" + name + ") ]]"));
            break;
        }
        case NAME_CLIP_DISTANCE:
            // it will be done separately in DeclareClipPlanes]=])
