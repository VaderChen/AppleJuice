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
inline std::array<Light,8> default_lights(){std::array<Light,8> lights;lights[0].diffuse=lights[0].specular={1,1,1,1};return lights;}
inline bool lighting_cap(unsigned cap){return cap==0xb50 || cap==0xb57 || cap==0xba1 || (cap>=0x4000 && cap<=0x4007);}
static_assert(sizeof(Light)==7*16);
}
