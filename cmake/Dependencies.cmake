include(FetchContent)
FetchContent_Declare(hlslcc
  URL "${PROJECT_SOURCE_DIR}/third_party/hlslcc-3ea1fcd.tar.gz"
  URL_HASH SHA256=f07b9c04c1090216b0d434d3b87ad986b2231257ecb76f42de9dd4c12224f2a0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(hlslcc)
# 外接磁碟的 AppleDouble 檔可能使 FetchContent 保留封存檔頂層目錄。
# 以真實原始碼辨識，避免先前失敗的修補所建立的空 src 目錄干擾。
if(NOT EXISTS "${hlslcc_SOURCE_DIR}/src/HLSLcc.cpp"
   AND EXISTS "${hlslcc_SOURCE_DIR}/HLSLcc-3ea1fcd6bd0ac445bc078de0bf32f0950188577b/src/HLSLcc.cpp")
  set(hlslcc_SOURCE_DIR "${hlslcc_SOURCE_DIR}/HLSLcc-3ea1fcd6bd0ac445bc078de0bf32f0950188577b")
endif()
# 修正上游 Metal 宣告自行加 sampler 前綴、運算式仍使用原名的通用命名問題。
set(HLSLCC_METAL_DECL "${hlslcc_SOURCE_DIR}/src/toMetalDeclaration.cpp")
file(READ "${HLSLCC_METAL_DECL}" HLSLCC_METAL_DECL_SOURCE)
string(REPLACE "samplerOss << (startsWithSampler ? \"\" : \"sampler\") << name;"
  "samplerOss << name; (void)startsWithSampler;" HLSLCC_METAL_DECL_PATCHED "${HLSLCC_METAL_DECL_SOURCE}")
if(NOT HLSLCC_METAL_DECL_SOURCE STREQUAL HLSLCC_METAL_DECL_PATCHED)
  file(WRITE "${HLSLCC_METAL_DECL}" "${HLSLCC_METAL_DECL_PATCHED}")
endif()
set(HLSLCC_METAL_INST "${hlslcc_SOURCE_DIR}/src/toMetalInstruction.cpp")
file(READ "${HLSLCC_METAL_INST}" HLSLCC_METAL_INST_SOURCE)
string(REPLACE "TranslateOperand(psDestAddr, TO_FLAG_INTEGER | TO_FLAG_UNSIGNED_INTEGER)"
  "TranslateOperand(psDestAddr, TO_FLAG_INTEGER | TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X)"
  HLSLCC_METAL_INST_PATCHED "${HLSLCC_METAL_INST_SOURCE}")
string(REPLACE "TranslateOperand(psDestByteOff, dstOffFlag)"
  "TranslateOperand(psDestByteOff, dstOffFlag, OPERAND_4_COMPONENT_MASK_X)"
  HLSLCC_METAL_INST_PATCHED "${HLSLCC_METAL_INST_PATCHED}")
# DXBC 的 IF／BREAKC／CONTINUEC／RETC／SWITCH／DISCARD 條件為 scalar。套用 swizzle 的第一個分量，
# 不讓 Metal 把 bool4／uint4 當成 C++ 的單一 bool；保留 zero／nonzero 判斷語義。
foreach(CONDITION_TYPE TO_FLAG_BOOL TO_FLAG_UNSIGNED_INTEGER TO_FLAG_INTEGER)
  string(REPLACE "TranslateOperand(&psInst->asOperands[0], ${CONDITION_TYPE})"
    "TranslateOperand(&psInst->asOperands[0], ${CONDITION_TYPE}, OPERAND_4_COMPONENT_MASK_X)"
    HLSLCC_METAL_INST_PATCHED "${HLSLCC_METAL_INST_PATCHED}")
endforeach()
if(NOT HLSLCC_METAL_INST_SOURCE STREQUAL HLSLCC_METAL_INST_PATCHED)
  file(WRITE "${HLSLCC_METAL_INST}" "${HLSLCC_METAL_INST_PATCHED}")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalGeometry.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalLayout.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccDeclarationReflection.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccTypedBuffers.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccTextureAtomics.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMaskedMoves.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccTempLaneTypes.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccUseDefineChains.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalTessellation.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccTextureScalarOperands.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalSamplerNames.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalClipDistance.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalCoverage.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccMetalPosition.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/HlslccCoalesceTemps.cmake")
file(GLOB HLSLCC_SOURCES CONFIGURE_DEPENDS "${hlslcc_SOURCE_DIR}/src/*.cpp" "${hlslcc_SOURCE_DIR}/src/cbstring/*.c")
# AppleDouble 是二進位中繼資料，副檔名相同也不能當成原始碼編譯。
list(FILTER HLSLCC_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")
add_library(x64arm_hlslcc STATIC ${HLSLCC_SOURCES})
target_include_directories(x64arm_hlslcc SYSTEM PUBLIC "${hlslcc_SOURCE_DIR}/include"
  PRIVATE "${hlslcc_SOURCE_DIR}" "${hlslcc_SOURCE_DIR}/src" "${hlslcc_SOURCE_DIR}/src/internal_includes" "${hlslcc_SOURCE_DIR}/src/cbstring")
set_target_properties(x64arm_hlslcc PROPERTIES CXX_STANDARD 11)
FetchContent_Declare(vkd3d_shader
  URL "${PROJECT_SOURCE_DIR}/third_party/vkd3d-2.1.tar.xz"
  URL_HASH SHA256=7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_Declare(vkd3d_parsers
  URL "${PROJECT_SOURCE_DIR}/third_party/vkd3d-2.1-parsers.tar.gz"
  URL_HASH SHA256=cecbb000a896fc910b320f5b62c7fd2e069aad7a42f4bce2a7d1fa691134a6c2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(vkd3d_shader vkd3d_parsers)
if(NOT EXISTS "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
   AND EXISTS "${vkd3d_shader_SOURCE_DIR}/vkd3d-2.1/libs/vkd3d-shader/hlsl_codegen.c")
  set(vkd3d_shader_SOURCE_DIR "${vkd3d_shader_SOURCE_DIR}/vkd3d-2.1")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dBufferDimensions.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dStaticInitializers.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dVectorCastTypes.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dFx2.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dPs1Sampling.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dLegacyPressure.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dLegacyScheduling.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dLegacyRematerialize.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dLegacyLighting.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dBoundedLoops.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dSubtractionConversion.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dDynamicLinkage.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dTextureBuffers.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dShaderIsolation.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dNativeLowPrecision.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/Vkd3dStorageBufferArrays.cmake")
# 舊版 shader model 的 PSIZE 使用特殊暫存器；SM4+ 已改為可與其他
# semantic 共用的輸出暫存器，必須保留真正的 component mask。
# 上游把 SM1 的全分量規則套到 SM4+，會覆蓋同一暫存器其他輸出的反射。
set(VKD3D_HLSL_CODEGEN "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c")
file(READ "${VKD3D_HLSL_CODEGEN}" VKD3D_HLSL_CODEGEN_SOURCE)
string(REPLACE
  "&& program->shader_version.type == VKD3D_SHADER_TYPE_VERTEX)\n        element->mask = VKD3DSP_WRITEMASK_ALL;"
  "&& program->shader_version.type == VKD3D_SHADER_TYPE_VERTEX\n            && program->shader_version.major < 4)\n        element->mask = VKD3DSP_WRITEMASK_ALL;"
  VKD3D_HLSL_CODEGEN_PATCHED "${VKD3D_HLSL_CODEGEN_SOURCE}")
if(NOT VKD3D_HLSL_CODEGEN_SOURCE STREQUAL VKD3D_HLSL_CODEGEN_PATCHED)
  file(WRITE "${VKD3D_HLSL_CODEGEN}" "${VKD3D_HLSL_CODEGEN_PATCHED}")
endif()
file(GLOB VKD3D_SHADER_SOURCES CONFIGURE_DEPENDS "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/*.c"
  "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-common/*.c" "${vkd3d_parsers_SOURCE_DIR}/*.c")
list(FILTER VKD3D_SHADER_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")
list(FILTER VKD3D_SHADER_SOURCES EXCLUDE REGEX "/blob\\.c$")
add_library(x64arm_shader_compiler STATIC ${VKD3D_SHADER_SOURCES})
file(MAKE_DIRECTORY "${vkd3d_shader_BINARY_DIR}/include")
configure_file("${PROJECT_SOURCE_DIR}/cmake/vkd3d_vkd3d_version.h.in"
  "${vkd3d_shader_BINARY_DIR}/include/vkd3d_version.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/cmake/vkd3d_config.h.in"
  "${vkd3d_shader_BINARY_DIR}/include/config.h" COPYONLY)
target_include_directories(x64arm_shader_compiler SYSTEM PUBLIC "${vkd3d_shader_SOURCE_DIR}/include"
  PRIVATE "${vkd3d_shader_SOURCE_DIR}/include/private" "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader"
  "${vkd3d_shader_BINARY_DIR}/include" "${vkd3d_parsers_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/third_party/spirv-headers")
target_compile_definitions(x64arm_shader_compiler PRIVATE LIBVKD3D_SHADER_SOURCE)
target_link_libraries(x64arm_shader_compiler PRIVATE ${CMAKE_DL_LIBS})

include("${PROJECT_SOURCE_DIR}/cmake/Glsl.cmake")

include("${PROJECT_SOURCE_DIR}/cmake/DirectXTex.cmake")
