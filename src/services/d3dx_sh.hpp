#pragma once
namespace x64arm {
// 隔離 DirectXMath 的 ARM NEON 型別，避免與 SoftFloat 的 float64_t 衝突。
void sh_evaluate(float* output,unsigned order,const float* direction);
void sh_rotate(float* output,unsigned order,const float* matrix,const float* input);
void sh_scale(float* output,unsigned order,const float* input,float scale);
void sh_add(float* output,unsigned order,const float* a,const float* b);
float sh_dot(unsigned order,const float* a,const float* b);
bool sh_cone(unsigned order,const float* direction,float radius,const float* color,float* r,float* g,float* b);
// D3DXSHMultiply2～4 的截斷乘積投影、繞 Z 軸旋轉，以及方向光／球形光／半球光的投影（各回傳 false 表示參數不合法）。
bool sh_multiply(float* output,unsigned order,const float* a,const float* b);
void sh_rotate_z(float* output,unsigned order,float angle,const float* input);
bool sh_directional(unsigned order,const float* direction,const float* color,float* r,float* g,float* b);
bool sh_spherical(unsigned order,const float* position,float radius,const float* color,float* r,float* g,float* b);
bool sh_hemisphere(unsigned order,const float* direction,const float* top,const float* bottom,float* r,float* g,float* b);
void d3dx_unproject(float* output,const float* input,const float* viewport,const float* projection,const float* view,const float* world);
}
