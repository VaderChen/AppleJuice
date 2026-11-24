# 常數緩衝的執行期描述元陣列（SM6.6 的 ConstantBuffer<T> 由 ResourceDescriptorHeap 取得、CBV 描述元表）：
# 原本只有 device T* 使用 16 位元組的 spvBufferDescriptor（位址＋長度），constant T* 落到 8 位元組的 spvDescriptor。
# D3D12 的緩衝鏡像每項 16 位元組並同時存放 CBV、SRV、UAV 緩衝，常數緩衝陣列也改用相同的記錄格式。
x64arm_vkd3d_patch("${spirv_cross_SOURCE_DIR}/spirv_msl.cpp"
  [=[				statement("int length(int i) const { return ptr[i].length; }");
				statement("const device spvBufferDescriptor<device T*>* ptr;");
				end_scope_decl();
				statement("");
			}
			break;]=]
  [=[				statement("int length(int i) const { return ptr[i].length; }");
				statement("const device spvBufferDescriptor<device T*>* ptr;");
				end_scope_decl();
				statement("");
			}
			if (msl_options.runtime_array_rich_descriptor)
			{
				if (spv_function_implementations.count(SPVFuncImplVariableSizedDescriptor) == 0)
				{
					statement("template<typename T>");
					statement("struct spvBufferDescriptor;");
					statement("");
				}
				statement("template<typename T>");
				statement("struct spvBufferDescriptor<constant T*>");
				begin_scope();
				statement("constant T* value;");
				statement("int length;");
				statement("int padding;");
				end_scope_decl();
				statement("");
				statement("template<typename T>");
				statement("struct spvDescriptorArray<constant T*>");
				begin_scope();
				statement("spvDescriptorArray(const device spvBufferDescriptor<constant T*>* ptr_) : ptr(ptr_) {}");
				statement("spvDescriptorArray(const device void *ptr_) : spvDescriptorArray(static_cast<const device spvBufferDescriptor<constant T*>*>(ptr_)) {}");
				statement("constant T* operator [] (size_t i) const { return ptr[i].value; }");
				statement("int length(int i) const { return ptr[i].length; }");
				statement("const device spvBufferDescriptor<constant T*>* ptr;");
				end_scope_decl();
				statement("");
			}
			break;]=])
