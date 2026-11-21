#include "d3dx_light_basis.h"

typedef struct { float value[4]; } ShColor;

__declspec(dllexport) float *CALL D3DXSHEvalDirection(float *out, unsigned order, const float *dir)
{
    if (order >= 2 && order <= 6)
        sh_light_basis(out, order, dir[0], dir[1], dir[2]);
    return out;
}

EXPORT D3DXSHEvalDirectionalLight(unsigned order, const float *dir, float r, float g, float b,
                                 float *red, float *green, float *blue)
{
    if (order < 2 || order > 6) return 0;
    if (!dir || !red) return 0x8876086cU;
    sh_light_basis(red, order, dir[0], dir[1], dir[2]);
    const float norm = sh_light_cosine_norm(order);
    for (unsigned i = 0; i < order*order; ++i) {
        const float value = red[i] / norm;
        red[i] = value*r;
        if (green) green[i] = value*g;
        if (blue) blue[i] = value*b;
    }
    return 0;
}

EXPORT D3DXSHEvalHemisphereLight(unsigned order, const float *dir, ShColor top, ShColor bottom,
                                float *red, float *green, float *blue)
{
    if (!order) return 0;
    if (!dir || !red) return 0x8876086cU;
    float basis[4], *outputs[3] = {red,green,blue};
    sh_light_basis(basis, 2, dir[0], dir[1], dir[2]);
    for (unsigned c = 0; c < 3; ++c) {
        if (!outputs[c]) continue;
        for (unsigned long long i = 0; i < (unsigned long long)order*order; ++i)
            outputs[c][i] = sh_light_hemisphere(i,basis,top.value[c],bottom.value[c]);
    }
    return 0;
}

/* 大角度的三角函式交給引擎已有的 CRT 實作，映射後的入口表為唯讀。 */
__declspec(dllexport) __UINTPTR_TYPE__ sh_light_trig[2]={1,1};
typedef double (__attribute__((cdecl)) *ShTrig)(double);

static unsigned sh_light_project(unsigned order,const float *dir,const float *weights,
                                 float r,float g,float b,float *red,float *green,float *blue)
{
    sh_light_basis(red,order,dir[0],dir[1],dir[2]);
    for(unsigned l=0;l<order;++l)for(unsigned i=l*l;i<(l+1)*(l+1);++i){
        const float value=red[i]*weights[l];
        red[i]=value*r;
        if(green)green[i]=value*g;
        if(blue)blue[i]=value*b;
    }
    return 0;
}

EXPORT D3DXSHEvalConeLight(unsigned order,const float *dir,float angle,float r,float g,float b,
                          float *red,float *green,float *blue)
{
    if(order<2 || order>6)return 0;
    if(!dir || !red)return 0x8876086cU;
    if(angle<=0)return D3DXSHEvalDirectionalLight(order,dir,r,g,b,red,green,blue);
    const ShTrig sine=(ShTrig)sh_light_trig[0], cosine=(ShTrig)sh_light_trig[1];
    const double s=sine(angle), h=sine(0.5*(double)angle);
    float weights[6];
    sh_light_cap(weights,order,cosine(angle),s*s,2*h*h,angle<=SH_LIGHT_PI/2?s*s:1);
    return sh_light_project(order,dir,weights,r,g,b,red,green,blue);
}

EXPORT D3DXSHEvalSphericalLight(unsigned order,const float *position,float radius,float r,float g,float b,
                               float *red,float *green,float *blue)
{
    if(order<2 || order>6)return 0;
    if(!position || !red)return 0x8876086cU;
    float dir[3]={position[0],position[1],position[2]}, weights[6];
    const float distance=__builtin_sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    sh_light_sphere(weights,order,distance,__builtin_fabsf(radius));
    if(distance)for(unsigned i=0;i<3;++i)dir[i]/=distance;
    return sh_light_project(order,dir,weights,r,g,b,red,green,blue);
}
