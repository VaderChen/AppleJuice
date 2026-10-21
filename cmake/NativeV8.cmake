# 後端只為實驗模組 ABI 建置；不宣告與 NW.js 6.5 的 C++／物件布局相容。
target_sources(x64arm_windows PRIVATE src/services/native_v8.cpp src/services/windows_native_v8.cpp)
option(X64ARM_NATIVE_V8_PROTOTYPE "建置 ARM64 V8 整體模組原型" OFF)
set(X64ARM_V8_NODE_ROOT "" CACHE PATH "提供 V8 標頭與 libnode 的相同 Node 安裝根目錄")
if(X64ARM_NATIVE_V8_PROTOTYPE)
  if(NOT APPLE OR NOT X64ARM_TARGET_ARM64)
    message(FATAL_ERROR "V8 原型目前要求 macOS ARM64")
  endif()
  find_path(X64ARM_V8_INCLUDE v8.h HINTS "${X64ARM_V8_NODE_ROOT}/include/node" REQUIRED NO_DEFAULT_PATH)
  file(GLOB X64ARM_NODE_LIBRARIES "${X64ARM_V8_NODE_ROOT}/lib/libnode.*.dylib")
  list(LENGTH X64ARM_NODE_LIBRARIES X64ARM_NODE_COUNT)
  if(NOT X64ARM_NODE_COUNT EQUAL 1)
    message(FATAL_ERROR "請指定只含一份 libnode 的 X64ARM_V8_NODE_ROOT")
  endif()
  # config.gypi 是安裝標頭的建置契約，禁止猜測壓縮指標／sandbox 巨集。
  file(READ "${X64ARM_V8_NODE_ROOT}/include/node/config.gypi" X64ARM_NODE_CONFIG)
  if(NOT X64ARM_NODE_CONFIG MATCHES "\"target_arch\": \"arm64\"" OR
     NOT X64ARM_NODE_CONFIG MATCHES "\"v8_enable_pointer_compression\": 0" OR
     NOT X64ARM_NODE_CONFIG MATCHES "\"v8_enable_sandbox\": 0")
    message(FATAL_ERROR "此原型需 ARM64、未啟用 pointer compression/sandbox 的一致 V8 建置")
  endif()
  add_executable(x64arm_v8_host src/native/v8_host.cpp)
  target_include_directories(x64arm_v8_host PRIVATE include)
  target_include_directories(x64arm_v8_host SYSTEM PRIVATE "${X64ARM_V8_INCLUDE}")
  target_compile_options(x64arm_v8_host PRIVATE -fno-rtti)
  target_link_libraries(x64arm_v8_host PRIVATE x64arm_options "${X64ARM_NODE_LIBRARIES}")
  set_target_properties(x64arm_v8_host PROPERTIES OSX_DEPLOYMENT_TARGET "13.5")
  add_custom_command(TARGET x64arm_v8_host POST_BUILD
    COMMAND "${X64ARM_CODESIGN}" --force --sign - --options runtime
      --entitlements "${PROJECT_SOURCE_DIR}/cmake/native-v8.entitlements.plist" "$<TARGET_FILE:x64arm_v8_host>"
    VERBATIM COMMENT "為獨立 V8 原型套用 JIT 與開發用 libnode 簽章")
endif()
