#pragma once
#include <array>
#include <algorithm>
#include <cmath>

namespace x64arm::metal_gl {
using Vec=std::array<float,4>;
using Matrix=std::array<float,16>;
inline Vec transform(const Matrix& m,const Vec& v){
    Vec result{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)result[r]+=m[c*4+r]*v[c];
    return result;
}
// Inverse transpose, including nonuniform scales and shears. A singular
// modelview has undefined GL normal results; return zero instead of NaNs.
inline Matrix normal_matrix(const Matrix& m){
    double rows[4][8]{};
    for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)rows[r][c]=m[c*4+r];rows[r][r+4]=1;}
    for(unsigned c=0;c<4;++c){
        unsigned pivot=c;
        for(unsigned r=c+1;r<4;++r)if(std::abs(rows[r][c])>std::abs(rows[pivot][c]))pivot=r;
        if(rows[pivot][c]==0)return {};
        for(unsigned k=0;k<8;++k)std::swap(rows[c][k],rows[pivot][k]);
        const auto divisor=rows[c][c];for(auto& value:rows[c])value/=divisor;
        for(unsigned r=0;r<4;++r)if(r!=c){const auto factor=rows[r][c];for(unsigned k=0;k<8;++k)rows[r][k]-=factor*rows[c][k];}
    }
    Matrix result{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)result[c*4+r]=static_cast<float>(rows[c][r+4]);
    return result;
}
struct Material {
    Vec ambient{.2F,.2F,.2F,1},diffuse{.8F,.8F,.8F,1},specular{0,0,0,1},emission{0,0,0,1},shininess{};
    Vec indexes{0,1,1,0};
};
// All GPU fields are float4 to keep C++/MSL layout identical.
struct Light {
    Vec ambient{0,0,0,1},diffuse{0,0,0,1},specular{0,0,0,1},position{0,0,1,0},direction{0,0,-1,0};
    Vec spot{0,-1,180,0},attenuation{1,0,0,0};
};
// fixed_gl.metal 的 light_vertex 在 CPU 端的同式實作（回饋模式要回傳光照後的頂點顏色）：lights[8] 為光照模型
// （ambient＝全域環境光、spot.x＝區域觀察者、spot.z＝分離鏡面色），mask 為啟用的光源位元；回傳主色，分離鏡面時鏡面項不計入。
inline Vec light_vertex(const Vec& vertex_normal,const Vec& ambient,const Vec& diffuse_color,const Vec& specular_color,const Vec& emission,float shininess,
                        const Vec& eye,const Matrix& normal,const std::array<Light,9>& lights,bool normalize,unsigned mask){
    const auto unit=[](std::array<float,3> v){const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n>0)for(auto& c:v)c/=n;else v={0,0,0};return v;};
    const auto dot=[](const std::array<float,3>& a,const std::array<float,3>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto transformed=transform(normal,{vertex_normal[0],vertex_normal[1],vertex_normal[2],0});std::array<float,3> n{transformed[0],transformed[1],transformed[2]};if(normalize)n=unit(n);
    const auto& model=lights[8];
    const std::array<float,3> viewer=model.spot[0]!=0 && eye[3]!=0?unit({-eye[0]/eye[3],-eye[1]/eye[3],-eye[2]/eye[3]}):std::array<float,3>{0,0,1};
    std::array<float,3> color{},specular_sum{};for(unsigned c=0;c<3;++c)color[c]=emission[c]+model.ambient[c]*ambient[c];
    for(unsigned i=0;i<8;++i){
        if(!(mask&(1U<<i)))continue;const auto& light=lights[i];
        std::array<float,3> delta{};for(unsigned c=0;c<3;++c)delta[c]=light.position[3]==0?light.position[c]:light.position[c]/light.position[3]-eye[c]/eye[3];
        const float distance=std::sqrt(dot(delta,delta));const auto direction=unit(delta);float attenuation=1;
        if(light.position[3]!=0)attenuation=1/(light.attenuation[0]+light.attenuation[1]*distance+light.attenuation[2]*distance*distance);
        if(light.spot[2]!=180){const float cosine=-dot(direction,unit({light.direction[0],light.direction[1],light.direction[2]}));
            attenuation*=cosine<light.spot[1]?0:std::pow(std::max(cosine,0.F),light.spot[0]);}
        const float diffuse=std::max(dot(n,direction),0.F);
        const float specular=diffuse>0?std::pow(std::max(dot(n,unit({direction[0]+viewer[0],direction[1]+viewer[1],direction[2]+viewer[2]})),0.F),shininess):0;
        for(unsigned c=0;c<3;++c){color[c]+=attenuation*(ambient[c]*light.ambient[c]+diffuse*diffuse_color[c]*light.diffuse[c]);specular_sum[c]+=attenuation*specular*specular_color[c]*light.specular[c];}
    }
    if(model.spot[2]==0)for(unsigned c=0;c<3;++c)color[c]+=specular_sum[c];
    return {std::clamp(color[0],0.F,1.F),std::clamp(color[1],0.F,1.F),std::clamp(color[2],0.F,1.F),std::clamp(diffuse_color[3],0.F,1.F)};
}
inline std::array<Light,8> default_lights(){std::array<Light,8> lights;lights[0].diffuse=lights[0].specular={1,1,1,1};return lights;}
inline bool lighting_cap(unsigned cap){return cap==0xb50 || cap==0xb57 || cap==0xba1 || (cap>=0x4000 && cap<=0x4007);}
static_assert(sizeof(Light)==7*16);
}
