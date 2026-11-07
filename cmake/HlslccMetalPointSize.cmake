# PSIZE 只有 SM3 以前是點大小。SM4 起沒有這個系統值，它只是一般的 varying（遊戲常拿來傳實例編號等整數），
# 當成 [[ point_size ]] 會讓 Metal 拒絕非浮點型別；像素著色器讀它時也不能略過宣告。
set(point_size_decl "${hlslcc_SOURCE_DIR}/src/toMetalDeclaration.cpp")
file(READ "${point_size_decl}" point_size_source)
string(REPLACE "psSignature->semanticName == \"PSIZE\" && psSignature->ui32SemanticIndex == 0)"
  "psSignature->semanticName == \"PSIZE\" && psSignature->ui32SemanticIndex == 0 && psContext->psShader->ui32MajorVersion < 4)"
  point_size_patched "${point_size_source}")
string(REPLACE "psContext->psShader->eShaderType == PIXEL_SHADER && !strncmp(psSig->semanticName.c_str(), \"PSIZE\", 5))"
  "psContext->psShader->eShaderType == PIXEL_SHADER && !strncmp(psSig->semanticName.c_str(), \"PSIZE\", 5) && psContext->psShader->ui32MajorVersion < 4)"
  point_size_patched "${point_size_patched}")
if(NOT point_size_source STREQUAL point_size_patched)
  file(WRITE "${point_size_decl}" "${point_size_patched}")
endif()
