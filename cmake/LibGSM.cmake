include(FetchContent)
FetchContent_Declare(x64arm_gsm_source
  URL "${PROJECT_SOURCE_DIR}/third_party/gsm-1.0.24.tar.gz"
  URL_HASH SHA256=a3c40c6471928383f4abfcb2e8f24012a1f562be2f17b8d672145d5986681a92
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(x64arm_gsm_source)
set(gsm_source "${x64arm_gsm_source_SOURCE_DIR}")
if(NOT EXISTS "${gsm_source}/inc/gsm.h" AND EXISTS "${gsm_source}/gsm-1.0-pl24/inc/gsm.h")
  set(gsm_source "${gsm_source}/gsm-1.0-pl24")
endif()

# GSM 固定點運算使用 32 位元 longword。保留上游封存檔，在建置目錄產生 LP64 可用的內部標頭。
file(READ "${gsm_source}/inc/private.h" gsm_private)
if(NOT gsm_private MATCHES "typedef[ \t]+long[ \t]+longword;" OR
   NOT gsm_private MATCHES "typedef[ \t]+unsigned long[ \t]+ulongword;")
  message(FATAL_ERROR "無法核對 libgsm 的固定點型別")
endif()
string(REGEX REPLACE "typedef[ \t]+long[ \t]+longword;" "typedef int32_t longword;" gsm_private "${gsm_private}")
string(REGEX REPLACE "typedef[ \t]+unsigned long[ \t]+ulongword;" "typedef uint32_t ulongword;" gsm_private "${gsm_private}")
set(gsm_generated "${x64arm_gsm_source_BINARY_DIR}/fixed")
file(MAKE_DIRECTORY "${gsm_generated}")
file(CONFIGURE OUTPUT "${gsm_generated}/private.h" CONTENT "#include <stdint.h>\n${gsm_private}" @ONLY)

set(gsm_sources
  add.c code.c debug.c decode.c gsm_create.c gsm_decode.c gsm_destroy.c gsm_encode.c
  gsm_option.c long_term.c lpc.c preprocess.c rpe.c short_term.c table.c)
list(TRANSFORM gsm_sources PREPEND "${gsm_source}/src/")
add_library(x64arm_gsm STATIC ${gsm_sources})
target_include_directories(x64arm_gsm PRIVATE "${gsm_generated}" "${gsm_source}/inc")
target_compile_definitions(x64arm_gsm PRIVATE WAV49 SASR NeedFunctionPrototypes=1)
set_target_properties(x64arm_gsm PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
set_source_files_properties(src/services/windows_acm.cpp PROPERTIES INCLUDE_DIRECTORIES "${gsm_source}/inc")
target_link_libraries(x64arm_windows PRIVATE x64arm_gsm)
