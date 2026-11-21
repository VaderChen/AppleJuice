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
bool sh_multiply(float* output,unsigned order,const float* a,const float* b){
    switch(order){case 2:DirectX::XMSHMultiply2(output,a,b);return true;case 3:DirectX::XMSHMultiply3(output,a,b);return true;
        case 4:DirectX::XMSHMultiply4(output,a,b);return true;case 5:DirectX::XMSHMultiply5(output,a,b);return true;case 6:DirectX::XMSHMultiply6(output,a,b);return true;default:return false;}
}
void d3dx_unproject(float* output,const float* input,const float* viewport,const float* projection,const float* view,const float* world){
    DirectX::XMFLOAT4X4 p,v,w;std::memcpy(&p,projection,sizeof(p));std::memcpy(&v,view,sizeof(v));std::memcpy(&w,world,sizeof(w));
    DirectX::XMFLOAT3 out;
    DirectX::XMStoreFloat3(&out,DirectX::XMVector3Unproject(DirectX::XMVectorSet(input[0],input[1],input[2],1),viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5],DirectX::XMLoadFloat4x4(&p),DirectX::XMLoadFloat4x4(&v),DirectX::XMLoadFloat4x4(&w)));
    std::memcpy(output,&out,sizeof(out));
}

}
