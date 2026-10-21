# 固定版本的 Microsoft 網格運算，沿用專案的 DirectXMath／Headers，停用 OpenMP。
FetchContent_Declare(directxmesh
  URL "${PROJECT_SOURCE_DIR}/third_party/directxmesh-27e180b.tar.gz"
  URL_HASH SHA256=4d7e949e56d34ec499e2aaf95dfd06479d113a63b2309b14df28e99ec5291d87
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR x64arm-do-not-add)
FetchContent_MakeAvailable(directxmesh)
set(X64ARM_MESH_SOURCES)
foreach(unit Adjacency Clean Concat GSAdjacency Normals Optimize OptimizeLRU OptimizeTVC Remap TangentFrame Util Validate VBReader VBWriter WeldVertices)
  list(APPEND X64ARM_MESH_SOURCES "${directxmesh_SOURCE_DIR}/DirectXMesh/DirectXMesh${unit}.cpp")
endforeach()
add_library(x64arm_mesh STATIC ${X64ARM_MESH_SOURCES} "${PROJECT_SOURCE_DIR}/src/services/d3dx_mesh.cpp")
target_link_libraries(x64arm_mesh PRIVATE x64arm_directxtex)
target_include_directories(x64arm_mesh SYSTEM PRIVATE "${directxmesh_SOURCE_DIR}/DirectXMesh")
target_compile_features(x64arm_mesh PRIVATE cxx_std_20)
