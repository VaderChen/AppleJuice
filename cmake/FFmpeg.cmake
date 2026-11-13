# FFmpeg 只用來解碼 Windows Media 系列內容（ASF 內的 VC-1／WMV 視訊與 WMA 音訊），不編入編碼器、容器、濾鏡、裝置與網路。
# 從本機固定的官方原始碼建置靜態 libavcodec／libavutil（LGPL 2.1+，不啟用 GPL／nonfree 元件），
# 使用與引擎相同的編譯器與 macOS deployment target；不自動偵測宿主程式庫，執行時不依賴 Homebrew 或系統 FFmpeg。
include(FetchContent)
include(ExternalProject)
find_program(X64ARM_MAKE make REQUIRED)
FetchContent_Declare(x64arm_ffmpeg_source
  URL "${PROJECT_SOURCE_DIR}/third_party/ffmpeg-8.1.2.tar.xz"
  URL_HASH SHA256=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(x64arm_ffmpeg_source)
# macOS 外接磁碟的 AppleDouble 檔可能使解壓縮保留頂層目錄。
if(NOT EXISTS "${x64arm_ffmpeg_source_SOURCE_DIR}/configure"
   AND EXISTS "${x64arm_ffmpeg_source_SOURCE_DIR}/ffmpeg-8.1.2/configure")
  set(x64arm_ffmpeg_source_SOURCE_DIR "${x64arm_ffmpeg_source_SOURCE_DIR}/ffmpeg-8.1.2")
endif()
set(X64ARM_FFMPEG_BUILD "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")
file(MAKE_DIRECTORY "${X64ARM_FFMPEG_BUILD}")
# 解碼器清單即支援範圍：VC-1（WVC1／WMVA／WMV3）、WMV7／WMV8，以及 WMA 1／2／Pro／Lossless／Voice。
set(X64ARM_FFMPEG_DECODERS vc1 wmv1 wmv2 wmv3 wmav1 wmav2 wmapro wmalossless wmavoice)
list(JOIN X64ARM_FFMPEG_DECODERS "," X64ARM_FFMPEG_DECODER_LIST)
set(X64ARM_FFMPEG_CFLAGS "")
set(X64ARM_FFMPEG_LDFLAGS "")
if(APPLE)
  string(APPEND X64ARM_FFMPEG_CFLAGS " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  string(APPEND X64ARM_FFMPEG_LDFLAGS " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if(X64ARM_SANITIZERS)
  string(APPEND X64ARM_FFMPEG_CFLAGS " -fsanitize=address,undefined -fno-omit-frame-pointer")
  string(APPEND X64ARM_FFMPEG_LDFLAGS " -fsanitize=address,undefined")
endif()
string(STRIP "${X64ARM_FFMPEG_CFLAGS}" X64ARM_FFMPEG_CFLAGS)
string(STRIP "${X64ARM_FFMPEG_LDFLAGS}" X64ARM_FFMPEG_LDFLAGS)
set(X64ARM_FFMPEG_FLAGS
  --prefix=${X64ARM_FFMPEG_BUILD}/install
  --cc=${CMAKE_C_COMPILER} --pkg-config=false
  --enable-static --disable-shared --enable-pic
  --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample
  --disable-debug
  --enable-decoder=${X64ARM_FFMPEG_DECODER_LIST}
  --enable-parser=vc1)
if(X64ARM_FFMPEG_CFLAGS)
  list(APPEND X64ARM_FFMPEG_FLAGS "--extra-cflags=${X64ARM_FFMPEG_CFLAGS}" "--extra-ldflags=${X64ARM_FFMPEG_LDFLAGS}")
endif()
ExternalProject_Add(x64arm_ffmpeg_build
  SOURCE_DIR "${x64arm_ffmpeg_source_SOURCE_DIR}"
  BINARY_DIR "${X64ARM_FFMPEG_BUILD}"
  DOWNLOAD_COMMAND ""
  CONFIGURE_COMMAND "${x64arm_ffmpeg_source_SOURCE_DIR}/configure" ${X64ARM_FFMPEG_FLAGS}
  BUILD_COMMAND "${X64ARM_MAKE}" "-j${X64ARM_EXTERNAL_BUILD_JOBS}" libavcodec/libavcodec.a libavutil/libavutil.a
  # 只安裝公開標頭（含 configure 產生的 avconfig.h），不把建置目錄的 config.h 暴露給引擎來源。
  INSTALL_COMMAND "${X64ARM_MAKE}" install-headers
  BUILD_BYPRODUCTS "${X64ARM_FFMPEG_BUILD}/libavcodec/libavcodec.a" "${X64ARM_FFMPEG_BUILD}/libavutil/libavutil.a"
  LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE)
file(MAKE_DIRECTORY "${X64ARM_FFMPEG_BUILD}/install/include")
foreach(component avutil avcodec)
  add_library(X64ArmFFmpeg::${component} STATIC IMPORTED GLOBAL)
  set_target_properties(X64ArmFFmpeg::${component} PROPERTIES
    IMPORTED_LOCATION "${X64ARM_FFMPEG_BUILD}/lib${component}/lib${component}.a"
    INTERFACE_INCLUDE_DIRECTORIES "${X64ARM_FFMPEG_BUILD}/install/include")
  add_dependencies(X64ArmFFmpeg::${component} x64arm_ffmpeg_build)
endforeach()
set_property(TARGET X64ArmFFmpeg::avcodec PROPERTY INTERFACE_LINK_LIBRARIES X64ArmFFmpeg::avutil)
