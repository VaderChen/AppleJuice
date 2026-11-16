#pragma once
namespace x64arm::d3dx_math {
// D3DX9 數學的 DirectXMath 實作（ARM NEON 原生路徑）。只用 float 陣列交換資料，隔離 DirectXMath 型別：
// 矩陣為 16 個 float（D3DX 列主序、列向量）、四元數為 x,y,z,w；指標可為 NULL 的參數依 D3DX 採預設值。
void perspective(float* m,float a,float b,float zn,float zf,bool fov,bool right);
void perspective_off_center(float* m,float l,float r,float b,float t,float zn,float zf,bool right);
void ortho_off_center_rh(float* m,float l,float r,float b,float t,float zn,float zf);
void look_at_rh(float* m,const float* eye,const float* at,const float* up);
float determinant(const float* m);
bool decompose(float* scale,float* rotation,float* translation,const float* m);
void transformation(float* m,const float* scaling_center,const float* scaling_rotation,const float* scaling,const float* rotation_center,const float* rotation,const float* translation);
void transformation_2d(float* m,const float* scaling_center,float scaling_rotation,const float* scaling,const float* rotation_center,float rotation,const float* translation);
void affine(float* m,float scaling,const float* rotation_center,const float* rotation,const float* translation);
void affine_2d(float* m,float scaling,const float* rotation_center,float rotation,const float* translation);
void quaternion_rotation_axis(float* q,const float* axis,float angle);
void quaternion_slerp(float* q,const float* a,const float* b,float t);
void quaternion_squad(float* q,const float* q1,const float* a,const float* b,const float* c,float t);
void quaternion_squad_setup(float* a,float* b,float* c,const float* q0,const float* q1,const float* q2,const float* q3);
void quaternion_barycentric(float* q,const float* q1,const float* q2,const float* q3,float f,float g);
void quaternion_exp(float* q,const float* input);
void quaternion_ln(float* q,const float* input);
// 直線與平面平行時回傳 false（D3DX 回 NULL）。
bool plane_intersect_line(float* out,const float* plane,const float* a,const float* b);
void color_contrast(float* out,const float* color,float contrast);
void color_saturation(float* out,const float* color,float saturation);
float fresnel(float cosine,float index);
void vec4_cross(float* out,const float* a,const float* b,const float* c);
// viewport＝{X, Y, Width, Height, MinZ, MaxZ}；矩陣為 NULL 時視為單位矩陣。
void project(float* out,const float* v,const float* viewport,const float* projection,const float* view,const float* world);
}
