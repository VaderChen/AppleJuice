# 固定來源、靜態資料與同一 deployment target；執行時不依賴 Homebrew dylib。
include(FetchContent)
include(ExternalProject)
FetchContent_Declare(x64arm_icu_source
  URL "${PROJECT_SOURCE_DIR}/third_party/icu4c-78.3-sources.tgz"
  URL_HASH SHA256=3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(x64arm_icu_source)
if(NOT EXISTS "${x64arm_icu_source_SOURCE_DIR}/source/configure"
   AND EXISTS "${x64arm_icu_source_SOURCE_DIR}/icu/source/configure")
  set(x64arm_icu_source_SOURCE_DIR "${x64arm_icu_source_SOURCE_DIR}/icu")
endif()
set(X64ARM_ICU_BUILD "${CMAKE_CURRENT_BINARY_DIR}/icu")
set(X64ARM_ICU_FLAGS "")
if(APPLE)
  string(APPEND X64ARM_ICU_FLAGS " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(X64ARM_TARGET_ARM64)
    string(APPEND X64ARM_ICU_FLAGS " -arch arm64")
  else()
    string(APPEND X64ARM_ICU_FLAGS " -arch x86_64")
  endif()
endif()
if(X64ARM_SANITIZERS)
  string(APPEND X64ARM_ICU_FLAGS " -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1")
endif()
ExternalProject_Add(x64arm_icu_build
  SOURCE_DIR "${x64arm_icu_source_SOURCE_DIR}/source"
  BINARY_DIR "${X64ARM_ICU_BUILD}"
  DOWNLOAD_COMMAND ""
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
    "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
    "CFLAGS=${X64ARM_ICU_FLAGS}" "CXXFLAGS=${X64ARM_ICU_FLAGS}" "LDFLAGS=${X64ARM_ICU_FLAGS}"
    "${x64arm_icu_source_SOURCE_DIR}/source/configure"
    --enable-static --disable-shared --with-data-packaging=static
    --disable-tests --disable-samples --disable-extras
  BUILD_COMMAND "${X64ARM_MAKE}" "-j${X64ARM_EXTERNAL_BUILD_JOBS}"
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${X64ARM_ICU_BUILD}/lib/libicuuc.a" "${X64ARM_ICU_BUILD}/lib/libicui18n.a" "${X64ARM_ICU_BUILD}/lib/libicudata.a"
  LOG_CONFIGURE TRUE LOG_BUILD TRUE)
foreach(component uc i18n data)
  add_library(ICU::${component} STATIC IMPORTED GLOBAL)
  set_target_properties(ICU::${component} PROPERTIES
    IMPORTED_LOCATION "${X64ARM_ICU_BUILD}/lib/libicu${component}.a"
    INTERFACE_INCLUDE_DIRECTORIES "${x64arm_icu_source_SOURCE_DIR}/source/common;${x64arm_icu_source_SOURCE_DIR}/source/i18n"
    INTERFACE_COMPILE_DEFINITIONS U_STATIC_IMPLEMENTATION)
  add_dependencies(ICU::${component} x64arm_icu_build)
endforeach()
set_property(TARGET ICU::i18n PROPERTY INTERFACE_LINK_LIBRARIES ICU::uc)
set_property(TARGET ICU::uc PROPERTY INTERFACE_LINK_LIBRARIES "ICU::data;${CMAKE_DL_LIBS}")
target_link_libraries(x64arm_windows PRIVATE ICU::i18n)
