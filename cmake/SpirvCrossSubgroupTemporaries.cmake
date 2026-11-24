# subgroup（D3D wave）運算的結果一律落成暫存變數。SPIRV-Cross 會把只用一次的運算式內嵌到使用處；
# 使用處若是 OpSelect 產生的 ?:（或 && ||），MSL 只在選中的分支求值，wave 運算就只在部分 lane 執行，
# ballot／broadcast／prefix 等結果因而錯誤（HLSL 的 cond ? WaveActiveSum(x) : y 即屬此類）。
# SPIR-V 的 OpSelect 兩邊都要求值，故在指令位置先求值、保留原語義。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[void CompilerMSL::emit_subgroup_op(const Instruction &i)
{
	const uint32_t *ops = stream(i);
	auto op = static_cast<Op>(i.op);
]=]
  [=[void CompilerMSL::emit_subgroup_op(const Instruction &i)
{
	const uint32_t *ops = stream(i);
	auto op = static_cast<Op>(i.op);

	// x64arm: evaluate at the instruction, never inside a short-circuiting use (see SpirvCrossSubgroupTemporaries.cmake).
	if (i.length >= 2)
		forced_temporaries.insert(ops[1]);
]=])
