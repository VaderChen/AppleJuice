option(X64ARM_METAL_OFFLINE "建置固定 Metal 著色器並內嵌 metallib" ON)
set(metal_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/x64arm")
file(MAKE_DIRECTORY "${metal_directory}")
set(metal_gl "${PROJECT_SOURCE_DIR}/src/platform/macos/fixed_gl.metal")
set(metal_raster "${PROJECT_SOURCE_DIR}/src/platform/macos/fixed_raster.metal")
set(metal_inputs "${metal_gl}" "${metal_raster}")
set(metal_library "")
if(X64ARM_METAL_OFFLINE)
  find_program(X64ARM_XCRUN xcrun REQUIRED)
  execute_process(COMMAND "${X64ARM_XCRUN}" --sdk macosx metal --version RESULT_VARIABLE metal_status OUTPUT_QUIET ERROR_QUIET)
  if(metal_status EQUAL 0)
    set(metal_air "")
    foreach(shader gl raster)
      add_custom_command(OUTPUT "${metal_directory}/fixed_${shader}.air"
        COMMAND "${X64ARM_XCRUN}" --sdk macosx metal -std=macos-metal2.3 -mmacosx-version-min=11.4 -ffast-math -c "${metal_${shader}}" -o "${metal_directory}/fixed_${shader}.air"
        DEPENDS "${metal_${shader}}" VERBATIM)
      list(APPEND metal_air "${metal_directory}/fixed_${shader}.air")
    endforeach()
    set(metal_library "${metal_directory}/fixed.metallib")
    add_custom_command(OUTPUT "${metal_library}"
      COMMAND "${X64ARM_XCRUN}" --sdk macosx metallib ${metal_air} -o "${metal_library}"
      DEPENDS ${metal_air} VERBATIM)
    list(APPEND metal_inputs "${metal_library}")
  else()
    message(WARNING "Metal toolchain 無法執行；固定 shader 改用同源的執行期編譯。")
  endif()
endif()
add_custom_command(OUTPUT "${metal_directory}/fixed_metal_data.hpp"
  COMMAND "${CMAKE_COMMAND}" "-DGL_SOURCE=${metal_gl}" "-DRASTER_SOURCE=${metal_raster}"
    "-DMETALLIB=${metal_library}" "-DOUTPUT=${metal_directory}/fixed_metal_data.hpp" -P "${PROJECT_SOURCE_DIR}/cmake/EmbedMetal.cmake"
  DEPENDS ${metal_inputs} "${PROJECT_SOURCE_DIR}/cmake/EmbedMetal.cmake" VERBATIM)
target_sources(x64arm_windows PRIVATE src/platform/macos/fixed_metal_library.mm "${metal_directory}/fixed_metal_data.hpp")
target_include_directories(x64arm_windows PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_source_files_properties(src/platform/macos/fixed_metal_library.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
