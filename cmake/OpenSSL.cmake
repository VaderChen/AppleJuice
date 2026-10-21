# 固定來源與 deployment target，避免系統 dylib 的 JIT library-validation 限制。
include(FetchContent)
include(ExternalProject)
find_package(Perl REQUIRED)
find_program(X64ARM_MAKE make REQUIRED)
FetchContent_Declare(x64arm_openssl_source
  URL "${PROJECT_SOURCE_DIR}/third_party/openssl-3.5.9.tar.gz"
  URL_HASH SHA256=4584983142262c52497752ed70d6bc02c70e3d91b66914072a6cd4588f591101
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(x64arm_openssl_source)
# macOS 外接磁碟的 AppleDouble 檔可能使解壓縮保留頂層目錄。
if(NOT EXISTS "${x64arm_openssl_source_SOURCE_DIR}/Configure"
   AND EXISTS "${x64arm_openssl_source_SOURCE_DIR}/openssl-3.5.9/Configure")
  set(x64arm_openssl_source_SOURCE_DIR "${x64arm_openssl_source_SOURCE_DIR}/openssl-3.5.9")
endif()
set(X64ARM_OPENSSL_BUILD "${CMAKE_CURRENT_BINARY_DIR}/openssl")
file(MAKE_DIRECTORY "${X64ARM_OPENSSL_BUILD}/include")
set(X64ARM_OPENSSL_FLAGS no-shared no-tests no-module no-apps --openssldir=/etc/ssl)
if(X64ARM_SANITIZERS)
  list(APPEND X64ARM_OPENSSL_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
endif()
if(APPLE)
  if(X64ARM_TARGET_ARM64)
    list(PREPEND X64ARM_OPENSSL_FLAGS darwin64-arm64-cc)
  else()
    list(PREPEND X64ARM_OPENSSL_FLAGS darwin64-x86_64-cc)
  endif()
  list(APPEND X64ARM_OPENSSL_FLAGS "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
ExternalProject_Add(x64arm_openssl_build
  SOURCE_DIR "${x64arm_openssl_source_SOURCE_DIR}"
  BINARY_DIR "${X64ARM_OPENSSL_BUILD}"
  DOWNLOAD_COMMAND ""
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
    "${PERL_EXECUTABLE}" "${x64arm_openssl_source_SOURCE_DIR}/Configure" ${X64ARM_OPENSSL_FLAGS}
  BUILD_COMMAND "${X64ARM_MAKE}" "-j${X64ARM_EXTERNAL_BUILD_JOBS}" build_libs
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${X64ARM_OPENSSL_BUILD}/libssl.a" "${X64ARM_OPENSSL_BUILD}/libcrypto.a"
  LOG_CONFIGURE TRUE LOG_BUILD TRUE)
foreach(component Crypto SSL)
  string(TOLOWER "${component}" lower)
  add_library(OpenSSL::${component} STATIC IMPORTED GLOBAL)
  set_target_properties(OpenSSL::${component} PROPERTIES
    IMPORTED_LOCATION "${X64ARM_OPENSSL_BUILD}/lib${lower}.a"
    INTERFACE_INCLUDE_DIRECTORIES "${X64ARM_OPENSSL_BUILD}/include;${x64arm_openssl_source_SOURCE_DIR}/include")
  add_dependencies(OpenSSL::${component} x64arm_openssl_build)
endforeach()
set_property(TARGET OpenSSL::SSL PROPERTY INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
if(NOT APPLE)
  find_package(Threads REQUIRED)
  set_property(TARGET OpenSSL::Crypto PROPERTY INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
endif()
