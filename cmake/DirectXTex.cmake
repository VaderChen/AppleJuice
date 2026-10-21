# Microsoft DirectXTex 的可攜 CPU 紋理運算；不連結 Windows／Direct3D 系統 DLL。
FetchContent_Declare(directxtex
  URL "${PROJECT_SOURCE_DIR}/third_party/directxtex-1acf4eb.tar.gz"
  URL_HASH SHA256=06a5b5fe61fe1a7dc1b515009f36c338d5d07afd10448c03806b75837f4b73c4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(directxtex)
FetchContent_Declare(directxmath
  URL "${PROJECT_SOURCE_DIR}/third_party/directxmath-5bbe2ce.tar.gz"
  URL_HASH SHA256=25107d9c57d19efe9cae7eda502166e154ab9ae0d634be0edbff9ef7e14758a0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(directxmath)
FetchContent_Declare(directx_headers
  URL "${PROJECT_SOURCE_DIR}/third_party/directx-headers-adbd6f3.tar.gz"
  URL_HASH SHA256=38aedf08f2a6f775cc619639d30b109bd684742fda506ab510fcae1517b70adf
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(directx_headers)
set(X64ARM_DIRECTXTEX_SOURCES)
foreach(unit BC BC4BC5 BC6HBC7 DirectXTexCompress DirectXTexConvert DirectXTexDDS DirectXTexImage DirectXTexMipmaps DirectXTexMisc DirectXTexNormalMaps DirectXTexPMAlpha DirectXTexResize DirectXTexUtil)
  list(APPEND X64ARM_DIRECTXTEX_SOURCES "${directxtex_SOURCE_DIR}/DirectXTex/${unit}.cpp")
endforeach()
add_library(x64arm_directxtex STATIC ${X64ARM_DIRECTXTEX_SOURCES})
target_sources(x64arm_directxtex PRIVATE "${directxmath_SOURCE_DIR}/SHMath/DirectXSH.cpp")
target_sources(x64arm_directxtex PRIVATE "${PROJECT_SOURCE_DIR}/src/services/d3dx_sh.cpp")
target_compile_features(x64arm_directxtex PUBLIC cxx_std_17)
target_compile_definitions(x64arm_directxtex PUBLIC USING_DIRECTX_HEADERS)
target_include_directories(x64arm_directxtex SYSTEM PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/directx-sal"
  "${directxtex_SOURCE_DIR}/DirectXTex" "${directxmath_SOURCE_DIR}/Inc"
  "${directxmath_SOURCE_DIR}/SHMath"
  "${directx_headers_SOURCE_DIR}/include" "${directx_headers_SOURCE_DIR}/include/wsl/stubs")
