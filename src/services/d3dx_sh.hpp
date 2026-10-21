#pragma once
namespace x64arm {
// 隔離 DirectXMath 的 ARM NEON 型別，避免與 SoftFloat 的 float64_t 衝突。
void sh_evaluate(float* output,unsigned order,const float* direction);
void sh_rotate(float* output,unsigned order,const float* matrix,const float* input);
void sh_scale(float* output,unsigned order,const float* input,float scale);
void sh_add(float* output,unsigned order,const float* a,const float* b);
float sh_dot(unsigned order,const float* a,const float* b);
bool sh_cone(unsigned order,const float* direction,float radius,const float* color,float* r,float* g,float* b);
void d3dx_unproject(float* output,const float* input,const float* viewport,const float* projection,const float* view,const float* world);
}
