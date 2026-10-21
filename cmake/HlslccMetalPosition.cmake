# SM4 以後 POS 是一般使用者語意；只有簽章中的 NAME_POSITION 才是系統位置。
# 保留 SM3 解碼器的舊式 POS 別名，避免把 float3 世界座標誤認成裁切座標。
foreach(signature IN ITEMS sig psSig psSignature)
  x64arm_hlslcc_patch(toMetalDeclaration.cpp
    "${signature}->eSystemValueType == NAME_POSITION || ${signature}->semanticName == \"POS\""
    "${signature}->eSystemValueType == NAME_POSITION || (psContext->psShader->ui32MajorVersion < 4 && ${signature}->semanticName == \"POS\")")
endforeach()
x64arm_hlslcc_patch(Shader.cpp
  "sig->eSystemValueType == NAME_POSITION || sig->semanticName == \"POS\""
  "sig->eSystemValueType == NAME_POSITION || (ui32MajorVersion < 4 && sig->semanticName == \"POS\")")
