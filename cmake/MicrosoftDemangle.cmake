# Microsoft C++ 名稱解析共用固定 LLVM 來源；配置時僅解開本機封存檔。
include(FetchContent)
FetchContent_Declare(llvm_demangle
  URL "${PROJECT_SOURCE_DIR}/third_party/llvm-demangle-21.1.8.tar.gz"
  URL_HASH SHA256=2dc2b67eba2b32b17ebc4d05201c6a2165106d70cfc0c54aef0b5effce679d24
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(llvm_demangle)
if(NOT llvm_demangle_POPULATED)
  FetchContent_Populate(llvm_demangle)
endif()
set(X64ARM_LLVM_DEMANGLE_ROOT "${llvm_demangle_SOURCE_DIR}/llvm")
if(NOT EXISTS "${X64ARM_LLVM_DEMANGLE_ROOT}/lib/Demangle/MicrosoftDemangle.cpp")
  set(X64ARM_LLVM_DEMANGLE_ROOT "${llvm_demangle_SOURCE_DIR}/llvm-demangle-21.1.8/llvm")
endif()
add_library(x64arm_microsoft_demangle STATIC
  "${X64ARM_LLVM_DEMANGLE_ROOT}/lib/Demangle/MicrosoftDemangle.cpp"
  "${X64ARM_LLVM_DEMANGLE_ROOT}/lib/Demangle/MicrosoftDemangleNodes.cpp"
  "${X64ARM_LLVM_DEMANGLE_ROOT}/lib/Demangle/ItaniumDemangle.cpp")
target_include_directories(x64arm_microsoft_demangle SYSTEM PUBLIC
  "${X64ARM_LLVM_DEMANGLE_ROOT}/include")
set_target_properties(x64arm_microsoft_demangle PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION_RELEASE "${X64ARM_IPO_AVAILABLE}")
