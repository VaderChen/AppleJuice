# 合併取樣器的 DXBC 可在 texture 與 sampler 命名空間使用同名資源。
# 在共用 ResourceName 產生名稱，讓宣告與所有運算式保持一致。
file(READ "${HLSLCC_METAL_DECL}" X64ARM_SAMPLER_SOURCE)
set(X64ARM_SAMPLER_MARKER "std::string ToMetal::ResourceName(ResourceGroup group, const uint32_t ui32RegisterNumber)")
string(FIND "${X64ARM_SAMPLER_SOURCE}" "${X64ARM_SAMPLER_MARKER}" X64ARM_SAMPLER_START)
if(X64ARM_SAMPLER_START LESS 0)
  message(FATAL_ERROR "找不到 Metal ResourceName")
endif()
string(SUBSTRING "${X64ARM_SAMPLER_SOURCE}" 0 ${X64ARM_SAMPLER_START} X64ARM_SAMPLER_PREFIX)
string(SUBSTRING "${X64ARM_SAMPLER_SOURCE}" ${X64ARM_SAMPLER_START} -1 X64ARM_SAMPLER_TAIL)
string(FIND "${X64ARM_SAMPLER_TAIL}" "x64arm_sampler_" X64ARM_SAMPLER_PATCHED)
if(X64ARM_SAMPLER_PATCHED LESS 0)
  string(REPLACE "std::string name = psBinding->name;" [=[std::string name = psBinding->name;
        if (group == RGROUP_SAMPLER)
            for (const auto& resource : psContext->psShader->sInfo.psResourceBindings)
                if (ShaderInfo::ResourceTypeToResourceGroup(resource.eType) == RGROUP_TEXTURE && resource.name == psBinding->name)
                {
                    name = "x64arm_sampler_" + name;
                    break;
                }]=] X64ARM_SAMPLER_TAIL "${X64ARM_SAMPLER_TAIL}")
  file(WRITE "${HLSLCC_METAL_DECL}" "${X64ARM_SAMPLER_PREFIX}${X64ARM_SAMPLER_TAIL}")
endif()
