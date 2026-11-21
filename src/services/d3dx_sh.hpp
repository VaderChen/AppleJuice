#pragma once
namespace x64arm {
// 隔離 DirectXMath 的 ARM NEON 型別，避免與 SoftFloat 的 float64_t 衝突。
void sh_evaluate(float* output,unsigned order,const float* direction);
void sh_rotate(float* output,unsigned order,const float* matrix,const float* input);
// D3DXSHMultiply2～6 的截斷乘積投影（各回傳 false 表示參數不合法）。
bool sh_multiply(float* output,unsigned order,const float* a,const float* b);
void d3dx_unproject(float* output,const float* input,const float* viewport,const float* projection,const float* view,const float* world);
}
