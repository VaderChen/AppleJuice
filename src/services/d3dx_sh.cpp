#include "d3dx_sh.hpp"
#include <DirectXSH.h>
#include <cstring>
namespace x64arm {
void sh_evaluate(float* output,unsigned order,const float* direction){
    DirectX::XMSHEvalDirection(output,order,DirectX::XMVectorSet(direction[0],direction[1],direction[2],0));
}
void sh_rotate(float* output,unsigned order,const float* matrix,const float* input){
    DirectX::XMFLOAT4X4 native;std::memcpy(&native,matrix,sizeof(native));
    DirectX::XMSHRotate(output,order,DirectX::XMLoadFloat4x4(&native),input);
}
void sh_scale(float* output,unsigned order,const float* input,float scale){DirectX::XMSHScale(output,order,input,scale);}
void sh_add(float* output,unsigned order,const float* a,const float* b){DirectX::XMSHAdd(output,order,a,b);}
float sh_dot(unsigned order,const float* a,const float* b){return DirectX::XMSHDot(order,a,b);}
bool sh_cone(unsigned order,const float* direction,float radius,const float* color,float* r,float* g,float* b){
    return DirectX::XMSHEvalConeLight(order,DirectX::XMVectorSet(direction[0],direction[1],direction[2],0),radius,DirectX::XMVectorSet(color[0],color[1],color[2],0),r,g,b);
}
void d3dx_unproject(float* output,const float* input,const float* viewport,const float* projection,const float* view,const float* world){
    DirectX::XMFLOAT4X4 p,v,w;std::memcpy(&p,projection,sizeof(p));std::memcpy(&v,view,sizeof(v));std::memcpy(&w,world,sizeof(w));
    DirectX::XMFLOAT3 out;
    DirectX::XMStoreFloat3(&out,DirectX::XMVector3Unproject(DirectX::XMVectorSet(input[0],input[1],input[2],1),viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5],DirectX::XMLoadFloat4x4(&p),DirectX::XMLoadFloat4x4(&v),DirectX::XMLoadFloat4x4(&w)));
    std::memcpy(output,&out,sizeof(out));
}

}
