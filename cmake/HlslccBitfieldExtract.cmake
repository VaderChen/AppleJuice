# HLSLcc 的 IBFE 純量輔助函式多了 template 關鍵字，Metal 會當成顯式實例化而拒絕編譯（UBFE 與向量版本沒有這個問題）。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[template int bitFieldExtractI(uint width, uint offset, int src)]=]
  [=[int bitFieldExtractI(uint width, uint offset, int src)]=])
# 輸出重導向的 as_type 沒有算入分量數；簽章是多分量整數輸出時（例如 uint2）大小不符而編譯失敗。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[                bformata(str, "as_type<int>(");]=]
  [=[                bformata(str, numComps > 1 ? "as_type<int%d>(" : "as_type<int>(", numComps);]=])
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[                bformata(str, "as_type<uint>(");]=]
  [=[                bformata(str, numComps > 1 ? "as_type<uint%d>(" : "as_type<uint>(", numComps);]=])
# D3D 的 firstbit_hi／firstbit_lo 對 0 回傳 0xffffffff，Metal 的 clz／ctz 回傳 32（上游留有 TODO）；依 -1 結束的位元迴圈會跑不完。
# firstbit_shi 另須處理負值：回傳第一個與符號不同的位元，0 與 -1 回傳 -1。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[{ UVecType res = clz(input); return res; };]=]
  [=[{ return select(clz(input), UVecType(0xffffffffu), input == UVecType(0u)); };]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[{ UVecType res = ctz(input); return res; };]=]
  [=[{ return select(ctz(input), UVecType(0xffffffffu), input == UVecType(0u)); };]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[{ IVecType res = clz(input); return res; };]=]
  [=[{ IVecType magnitude = select(input, ~input, input < IVecType(0)); return select(IVecType(clz(magnitude)), IVecType(-1), magnitude == IVecType(0)); };]=])
