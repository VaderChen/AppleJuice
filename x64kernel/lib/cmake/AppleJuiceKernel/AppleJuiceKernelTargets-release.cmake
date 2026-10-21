#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "AppleJuiceKernel::Core" for configuration "Release"
set_property(TARGET AppleJuiceKernel::Core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AppleJuiceKernel::Core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libx64arm_core.a"
  )

list(APPEND _cmake_import_check_targets AppleJuiceKernel::Core )
list(APPEND _cmake_import_check_files_for_AppleJuiceKernel::Core "${_IMPORT_PREFIX}/lib/libx64arm_core.a" )

# Import target "AppleJuiceKernel::Platform" for configuration "Release"
set_property(TARGET AppleJuiceKernel::Platform APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AppleJuiceKernel::Platform PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libx64arm_platform.a"
  )

list(APPEND _cmake_import_check_targets AppleJuiceKernel::Platform )
list(APPEND _cmake_import_check_files_for_AppleJuiceKernel::Platform "${_IMPORT_PREFIX}/lib/libx64arm_platform.a" )

# Import target "AppleJuiceKernel::JIT" for configuration "Release"
set_property(TARGET AppleJuiceKernel::JIT APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AppleJuiceKernel::JIT PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "ASM;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libx64arm_jit.a"
  )

list(APPEND _cmake_import_check_targets AppleJuiceKernel::JIT )
list(APPEND _cmake_import_check_files_for_AppleJuiceKernel::JIT "${_IMPORT_PREFIX}/lib/libx64arm_jit.a" )

# Import target "AppleJuiceKernel::SoftFloat" for configuration "Release"
set_property(TARGET AppleJuiceKernel::SoftFloat APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(AppleJuiceKernel::SoftFloat PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libx64arm_softfloat.a"
  )

list(APPEND _cmake_import_check_targets AppleJuiceKernel::SoftFloat )
list(APPEND _cmake_import_check_files_for_AppleJuiceKernel::SoftFloat "${_IMPORT_PREFIX}/lib/libx64arm_softfloat.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
