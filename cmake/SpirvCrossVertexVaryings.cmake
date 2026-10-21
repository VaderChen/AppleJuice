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
