# 現代路徑的細分管線：HS 的控制點輸出與 patch 常數分屬兩個結構，依連結指定的 location 補位時要依速率分開，
# 否則 patch 常數的 location 會被當成控制點欄位補進每個控制點，兩端的結構步距不同。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[			if (location_outputs_in_use.count(output.first.location) != 0)
				continue;]=]
  [=[			if (location_outputs_in_use.count(output.first.location) != 0)
				continue;
			if (is_tesc_shader() && patch != (output.second.rate == MSL_SHADER_VARIABLE_RATE_PER_PATCH))
				continue;]=])

# vkd3d 的 patch 常數是個別變數而不是 block；HS 的 fork phase 在輔助函式中寫入時，
# 也要像 block 一樣改傳 patchOut（DS 的 patchIn 同理），否則函式內會出現未宣告的欄位名稱。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[			bool is_patch_block_storage = is_patch && is_block && var.storage == StorageClassOutput;]=]
  [=[			bool is_patch_block_storage = is_patch && var.storage == StorageClassOutput &&
			                              (is_block || (is_tesc_shader() && !is_builtin_variable(var)));
			bool is_patch_input_storage = is_patch && var.storage == StorageClassInput && is_tese_shader() &&
			                              !is_builtin_variable(var);]=])
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[			bool is_redirected_to_global_stage_io = (is_control_point_storage || is_patch_block_storage) &&]=]
  [=[			bool is_redirected_to_global_stage_io = (is_control_point_storage || is_patch_block_storage || is_patch_input_storage) &&]=])

# vkd3d 以 OpCopyMemory 寫入整個介面變數（例如 HS fork phase 的 patch 常數）；
# 全域變數轉函式參數的分析原本只看 OpStore／OpLoad，漏掉這種寫法時函式內會引用未傳入的變數。只對階段介面變數補上。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[			case OpStore:
			{
				uint32_t base_id = ops[0];
				if (global_var_ids.find(base_id) != global_var_ids.end())
				{
					added_arg_ids.insert(base_id);]=]
  [=[			case OpCopyMemory:
			{
				// 只補階段介面變數；描述元陣列等資源維持原本的處理。
				for (uint32_t operand = 0; operand < 2; operand++)
				{
					auto *copied = maybe_get<SPIRVariable>(ops[operand]);
					if (copied && (copied->storage == StorageClassInput || copied->storage == StorageClassOutput) &&
					    global_var_ids.find(ops[operand]) != global_var_ids.end())
						added_arg_ids.insert(ops[operand]);
				}
				break;
			}

			case OpStore:
			{
				uint32_t base_id = ops[0];
				if (global_var_ids.find(base_id) != global_var_ids.end())
				{
					added_arg_ids.insert(base_id);]=])

# 改傳 patchOut／patchIn 後，輔助函式內的個別 patch 變數也要使用限定名稱（patchOut.vpc2），
# 原本只在入口函式套用限定別名。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[	if (current_function && (current_function->self == ir.default_entry_point))
	{
		auto *m = ir.find_meta(id);]=]
  [=[	const SPIRVariable *patch_var = is_tessellation_shader() && has_decoration(id, DecorationPatch) ? maybe_get<SPIRVariable>(id) : nullptr;
	if (current_function && (current_function->self == ir.default_entry_point ||
	                         (patch_var && !is_builtin_variable(*patch_var) &&
	                          (patch_var->storage == StorageClassOutput || patch_var->storage == StorageClassInput))))
	{
		auto *m = ir.find_meta(id);]=])
