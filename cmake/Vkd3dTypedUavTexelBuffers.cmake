# 型別化 UAV 緩衝（RWBuffer<T>）維持 texel buffer，由 view 的 DXGI 格式轉換（R16_UINT、R8_UINT、R16G16B16A16_FLOAT 等）；
# 以 storage buffer 存取會把每個元素當成一個 32 位元分量，格式與多分量都會錯。只有用到原子運算的型別化 UAV
# （D3D 規定只能是 R32 格式）與 raw／structured 緩衝才宣告為 storage buffer（見 Vkd3dStorageBufferArrays.cmake）。
# 資源符號記住宣告方式，讀寫、原子與 bufinfo 都依它選擇路徑。
set(X64ARM_VKD3D_SPIRV "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c")

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    bool raw;
    unsigned int binding_base_idx;
    uint32_t uav_counter_id;]=]
  [=[    bool raw;
    bool x64arm_ssbo;
    unsigned int binding_base_idx;
    uint32_t uav_counter_id;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    return compiler->compile_info.ssbo_uavs
            && resource->resource_type_info->resource_type == VKD3D_SHADER_RESOURCE_BUFFER;]=]
  [=[    return compiler->compile_info.ssbo_uavs
            && resource->resource_type_info->resource_type == VKD3D_SHADER_RESOURCE_BUFFER && resource->x64arm_ssbo;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    sampled_type = descriptor->resource_data_type;
]=]
  [=[    sampled_type = descriptor->resource_data_type;
    bool x64arm_ssbo = false;
]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    resource_symbol.info.resource.raw = raw;
]=]
  [=[    resource_symbol.info.resource.raw = raw;
    resource_symbol.info.resource.x64arm_ssbo = x64arm_ssbo;
]=])

# UAV 緩衝陣列可能同時有 storage buffer 與 texel buffer 兩種宣告。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[            if (resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
                vkd3d_spirv_enable_capability(builder, compiler->compile_info.ssbo_uavs
                        ? SpvCapabilityStorageBufferArrayDynamicIndexing
                        : SpvCapabilityStorageTexelBufferArrayDynamicIndexingEXT);]=]
  [=[            if (resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
            {
                if (compiler->compile_info.ssbo_uavs)
                    vkd3d_spirv_enable_capability(builder, SpvCapabilityStorageBufferArrayDynamicIndexing);
                vkd3d_spirv_enable_capability(builder, SpvCapabilityStorageTexelBufferArrayDynamicIndexingEXT);
            }]=])

# 掃描資訊：公開欄位分不出 structured 與 uint 型別化緩衝（資料型別同為 UINT），structured 加私有旗標 0x40000000，
# 讓 d3d12_shader.cpp 判斷哪些 UAV 緩衝是 storage buffer、哪些是 texel buffer。
set(X64ARM_VKD3D_MAIN "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/vkd3d_shader_main.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_MAIN}"
  [=[    d->structure_stride = structure_stride;
    if (raw)]=]
  [=[    d->structure_stride = structure_stride;
    if (structure_stride)
        d->flags |= 0x40000000u; /* x64arm: structured buffer */
    if (raw)]=])
