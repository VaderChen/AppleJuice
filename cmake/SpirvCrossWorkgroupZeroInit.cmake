# 零初始化 threadgroup 記憶體（VK_KHR_zero_initialize_workgroup_memory）時，SPIRV-Cross 產生的迴圈以
# gl_WorkGroupSize 計算步距，但只為 gl_LocalInvocationIndex 建立隱式宣告；workgroup size 常數只在模擬 subgroup 時建立。
# 零初始化也需要它，否則 Metal 編譯失敗（use of undeclared identifier 'gl_WorkGroupSize'）。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[	bool need_workgroup_size = msl_options.emulate_subgroups && active_input_builtins.get(BuiltInNumSubgroups);]=]
  [=[	bool need_workgroup_size = (msl_options.emulate_subgroups && active_input_builtins.get(BuiltInNumSubgroups)) ||
	                           needs_workgroup_zero_init;]=])
