#pragma once
#include <algorithm>
#include <cmath>
#include <utility>
namespace x64arm::audio::spatial {
struct Vector {float x{},y{},z{};};
inline Vector sub(Vector a,Vector b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline float dot(Vector a,Vector b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline float length(Vector v){return std::sqrt(dot(v,v));}
inline Vector normalized(Vector v){const auto n=length(v);return n!=0?Vector{v.x/n,v.y/n,v.z/n}:Vector{};}
inline Vector cross(Vector a,Vector b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline std::pair<float,float> stereo(float pan,float attenuation){
    pan=std::clamp(pan,-1.0F,1.0F);
    return {attenuation*std::sqrt(1-std::max(pan,0.0F)),attenuation*std::sqrt(1+std::min(pan,0.0F))};
}
inline float doppler(float sound,float listener,float source,float speed_limit,float minimum,float maximum){
    return std::clamp((sound+std::clamp(listener,-speed_limit,speed_limit))/(sound+std::clamp(source,-speed_limit,speed_limit)),minimum,maximum);
}
}
