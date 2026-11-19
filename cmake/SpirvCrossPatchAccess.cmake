# Patch 是介面變數的修飾。第二層 OpAccessChain 的基底是中間指標，
# 須從原始變數辨識，才能保留陣列索引與向量分量。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[patch = has_decoration(ops[2], DecorationPatch) || is_patch_block(type);]=]
  [=[patch = has_decoration(var->self, DecorationPatch) || is_patch_block(type);]=])

# DS 的逐頂點輸出由下一個階段依 location 與 component 配對，
# 沿用一般頂點輸出的 user varying 命名。patch 與控制點介面仍由細分路徑處理。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[if (is_tessellation_shader())
						SPIRV_CROSS_THROW("Component decoration is not supported in tessellation shaders.");]=]
  [=[if (is_tessellation_shader() && !(is_tese_shader() && storage == StorageClassOutput && !patch))
						SPIRV_CROSS_THROW("Component decoration is not supported in tessellation shaders.");]=])

# raw-buffer 的 DS 以 C 結構讀取整筆 patch。即使某成員沒有被讀取，
# 只要入口介面仍宣告它，就必須保留其大小與對齊，否則後續欄位與陣列步距會改變。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[	set_enabled_interface_variables(get_active_interface_variables());]=]
  [=[	auto active_variables = get_active_interface_variables();
	if (is_tese_shader() && msl_options.raw_buffer_tese_input)
	{
		for (auto id : get_entry_point().interface_variables)
		{
			auto *var = maybe_get<SPIRVariable>(id);
			if (var && var->storage == StorageClassInput && !is_builtin_variable(*var) &&
			    has_decoration(id, DecorationPatch) && has_decoration(id, DecorationLocation))
				active_variables.insert(id);
		}
	}
	set_enabled_interface_variables(std::move(active_variables));]=])
