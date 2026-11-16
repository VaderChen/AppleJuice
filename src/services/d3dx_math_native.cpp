#include "d3dx_math_native.hpp"
#include <DirectXMath.h>
#include <cmath>
#include <cstring>

namespace x64arm::d3dx_math {
namespace {
using namespace DirectX;
void put(float* m,const XMMATRIX& value){XMFLOAT4X4 out;XMStoreFloat4x4(&out,value);std::memcpy(m,&out,sizeof(out));}
XMMATRIX get(const float* m){if(!m)return XMMatrixIdentity();XMFLOAT4X4 in;std::memcpy(&in,m,sizeof(in));return XMLoadFloat4x4(&in);}
XMVECTOR v2(const float* p,float fallback=0){return p?XMVectorSet(p[0],p[1],0,0):XMVectorSet(fallback,fallback,0,0);}
XMVECTOR v3(const float* p,float fallback=0){return p?XMVectorSet(p[0],p[1],p[2],0):XMVectorSet(fallback,fallback,fallback,0);}
XMVECTOR v4(const float* p){return XMVectorSet(p[0],p[1],p[2],p[3]);}
XMVECTOR quat(const float* p){return p?v4(p):XMQuaternionIdentity();}
void store4(float* out,XMVECTOR value){XMFLOAT4 f;XMStoreFloat4(&f,value);out[0]=f.x;out[1]=f.y;out[2]=f.z;out[3]=f.w;}
void store3(float* out,XMVECTOR value){XMFLOAT3 f;XMStoreFloat3(&f,value);out[0]=f.x;out[1]=f.y;out[2]=f.z;}
}
void perspective(float* m,float a,float b,float zn,float zf,bool fov,bool right){
    put(m,fov?(right?XMMatrixPerspectiveFovRH(a,b,zn,zf):XMMatrixPerspectiveFovLH(a,b,zn,zf)):(right?XMMatrixPerspectiveRH(a,b,zn,zf):XMMatrixPerspectiveLH(a,b,zn,zf)));
}
void perspective_off_center(float* m,float l,float r,float b,float t,float zn,float zf,bool right){
    put(m,right?XMMatrixPerspectiveOffCenterRH(l,r,b,t,zn,zf):XMMatrixPerspectiveOffCenterLH(l,r,b,t,zn,zf));
}
void ortho_off_center_rh(float* m,float l,float r,float b,float t,float zn,float zf){put(m,XMMatrixOrthographicOffCenterRH(l,r,b,t,zn,zf));}
void look_at_rh(float* m,const float* eye,const float* at,const float* up){put(m,XMMatrixLookAtRH(v3(eye),v3(at),v3(up)));}
float determinant(const float* m){return XMVectorGetX(XMMatrixDeterminant(get(m)));}
bool decompose(float* scale,float* rotation,float* translation,const float* m){
    XMVECTOR s,r,t;if(!XMMatrixDecompose(&s,&r,&t,get(m)))return false;
    store3(scale,s);store4(rotation,r);store3(translation,t);return true;
}
void transformation(float* m,const float* scaling_center,const float* scaling_rotation,const float* scaling,const float* rotation_center,const float* rotation,const float* translation){
    put(m,XMMatrixTransformation(v3(scaling_center),quat(scaling_rotation),v3(scaling,1),v3(rotation_center),quat(rotation),v3(translation)));
}
void transformation_2d(float* m,const float* scaling_center,float scaling_rotation,const float* scaling,const float* rotation_center,float rotation,const float* translation){
    // D3DX 的 2D 版本 z 縮放為 1；DirectXMath 2D 版以 xy 平面運算，z 維持單位。
    put(m,XMMatrixTransformation2D(v2(scaling_center),scaling_rotation,v2(scaling,1),v2(rotation_center),rotation,v2(translation)));
}
void affine(float* m,float scaling,const float* rotation_center,const float* rotation,const float* translation){
    put(m,XMMatrixAffineTransformation(XMVectorReplicate(scaling),v3(rotation_center),quat(rotation),v3(translation)));
}
void affine_2d(float* m,float scaling,const float* rotation_center,float rotation,const float* translation){
    put(m,XMMatrixAffineTransformation2D(XMVectorSet(scaling,scaling,0,0),v2(rotation_center),rotation,v2(translation)));
}
void quaternion_rotation_axis(float* q,const float* axis,float angle){store4(q,XMQuaternionRotationAxis(v3(axis),angle));}
void quaternion_slerp(float* q,const float* a,const float* b,float t){store4(q,XMQuaternionSlerp(v4(a),v4(b),t));}
void quaternion_squad(float* q,const float* q1,const float* a,const float* b,const float* c,float t){store4(q,XMQuaternionSquad(v4(q1),v4(a),v4(b),v4(c),t));}
void quaternion_squad_setup(float* a,float* b,float* c,const float* q0,const float* q1,const float* q2,const float* q3){
    XMVECTOR ra,rb,rc;XMQuaternionSquadSetup(&ra,&rb,&rc,v4(q0),v4(q1),v4(q2),v4(q3));store4(a,ra);store4(b,rb);store4(c,rc);
}
void quaternion_barycentric(float* q,const float* q1,const float* q2,const float* q3,float f,float g){store4(q,XMQuaternionBaryCentric(v4(q1),v4(q2),v4(q3),f,g));}
void quaternion_exp(float* q,const float* input){store4(q,XMQuaternionExp(v4(input)));}
void quaternion_ln(float* q,const float* input){store4(q,XMQuaternionLn(v4(input)));}
bool plane_intersect_line(float* out,const float* plane,const float* a,const float* b){
    const auto point=XMPlaneIntersectLine(v4(plane),v3(a),v3(b));
    if(XMVector3IsNaN(point))return false;store3(out,point);return true;
}
void color_contrast(float* out,const float* color,float contrast){store4(out,XMColorAdjustContrast(v4(color),contrast));}
void color_saturation(float* out,const float* color,float saturation){store4(out,XMColorAdjustSaturation(v4(color),saturation));}
float fresnel(float cosine,float index){return XMVectorGetX(XMFresnelTerm(XMVectorReplicate(cosine),XMVectorReplicate(index)));}
void vec4_cross(float* out,const float* a,const float* b,const float* c){store4(out,XMVector4Cross(v4(a),v4(b),v4(c)));}
void project(float* out,const float* v,const float* viewport,const float* projection,const float* view,const float* world){
    store3(out,XMVector3Project(v3(v),viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5],get(projection),get(view),get(world)));
}
}
