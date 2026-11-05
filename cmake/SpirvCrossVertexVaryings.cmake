# 對齊 add_msl_shader_output 的 vecsize 契約；共用既有輸出補齊與 swizzle 流程。
# D3D9 的 VS／PS 依四分量暫存器連接，實際讀寫遮罩可以不同。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[	if (pad_fragment_output)
	{
		uint32_t locn = get_decoration(var.self, DecorationLocation);
		target_components = get_target_components_for_fragment_location(locn);]=]
  [=[	auto requested_output = outputs_by_location.find({get_decoration(var.self, DecorationLocation),
	                                                    get_decoration(var.self, DecorationComponent)});
	bool pad_vertex_output = get_entry_point().model == ExecutionModelVertex && storage == StorageClassOutput &&
	                         has_decoration(var.self, DecorationLocation) && requested_output != outputs_by_location.end() &&
	                         requested_output->second.vecsize > type_components;
	if (pad_fragment_output || pad_vertex_output)
	{
		uint32_t locn = get_decoration(var.self, DecorationLocation);
		target_components = pad_vertex_output ? requested_output->second.vecsize : get_target_components_for_fragment_location(locn);]=])

# 明確要求完整介面向量時，非零 Component 仍須保留原始分量位置。
# 例如 PS 只讀 zw，應由同一 float4 介面的 zw 取值，不能另建 locn_N_2。
# 重用 SPIRV-Cross 的分量聚合與進出 swizzle，無明確要求的介面維持原行為。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[				uint32_t component = get_decoration(var_id, DecorationComponent);
				if (component != 0)]=]
  [=[				uint32_t component = get_decoration(var_id, DecorationComponent);
				const auto &requested_varyings = storage == StorageClassInput ? inputs_by_location : outputs_by_location;
				auto requested_varying = requested_varyings.find({location, 0});
				bool pack_requested_varying =
				    ((storage == StorageClassInput && get_execution_model() == ExecutionModelFragment) ||
				     (storage == StorageClassOutput && get_execution_model() == ExecutionModelVertex)) &&
				    requested_varying != requested_varyings.end() &&
				    requested_varying->second.vecsize >= component + type.vecsize;
				if (component != 0)]=])
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[					else if (pack_components)]=]
  [=[					else if (pack_components || pack_requested_varying)]=])
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[							location_meta.num_components = max<uint32_t>(location_meta.num_components, component + type.vecsize);]=]
  [=[							location_meta.num_components = max<uint32_t>(location_meta.num_components,
							    pack_requested_varying ? requested_varying->second.vecsize : component + type.vecsize);]=])
