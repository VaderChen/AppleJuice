
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was AppleJuiceKernelConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################
include(CMakeFindDependencyMacro)
set(Zycore_DIR "${CMAKE_CURRENT_LIST_DIR}/../zycore")
set(Zydis_DIR "${CMAKE_CURRENT_LIST_DIR}/../zydis")
find_dependency(Zycore CONFIG PATHS "${CMAKE_CURRENT_LIST_DIR}/../zycore" NO_DEFAULT_PATH)
find_dependency(Zydis CONFIG PATHS "${CMAKE_CURRENT_LIST_DIR}/../zydis" NO_DEFAULT_PATH)
set(AppleJuiceKernel_NATIVE_JIT "ON")
set(AppleJuiceKernel_LLVM_REGIONS "OFF")
set(AppleJuiceKernel_SANITIZERS "OFF")
set(AppleJuiceKernel_SYSTEM "Darwin")
set(AppleJuiceKernel_ARCHITECTURES "arm64")
set(AppleJuiceKernel_DISTRIBUTION "ON")
set(AppleJuiceKernel_CONFIGURATION "Release")
if(AppleJuiceKernel_LLVM_REGIONS)
  find_dependency(LLVM 21.1 CONFIG)
  if(TARGET zstd::libzstd_static)
    get_target_property(_applejuice_llvm_links LLVMSupport INTERFACE_LINK_LIBRARIES)
    list(TRANSFORM _applejuice_llvm_links REPLACE "^zstd::libzstd_shared$" "zstd::libzstd_static")
    set_property(TARGET LLVMSupport PROPERTY INTERFACE_LINK_LIBRARIES "${_applejuice_llvm_links}")
  endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/AppleJuiceKernelTargets.cmake")
if(NOT TARGET AppleJuiceKernel::Kernel)
  add_library(AppleJuiceKernel::Kernel INTERFACE IMPORTED)
  set_target_properties(AppleJuiceKernel::Kernel PROPERTIES
    INTERFACE_LINK_LIBRARIES AppleJuiceKernel::JIT)
endif()
check_required_components(AppleJuiceKernel)
