#pragma once
#include "x64arm/windows_abi.hpp"
#include "audio_spatial.hpp"
#include <limits>
namespace x64arm::x3daudio {
inline std::uint64_t invoke(GuestMemory& memory,const WindowsCall& arg,const GuestState& state,bool initialize,bool x86){
    using namespace audio::spatial;
    const unsigned pointer=x86?4:8;const std::uint8_t bits=x86?32:64;
    const auto u32=[&](std::uint64_t p){return static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));};
    const auto real=[&](std::uint64_t p){const auto v=std::bit_cast<float>(u32(p));if(!std::isfinite(v))throw HostCallFault("X3DAudio 非有限浮點參數");return v;};
    const auto ptr=[&](std::uint64_t p){return memory.load(GuestAddress{p},bits);};
    const auto vector=[&](std::uint64_t p){return Vector{real(p),real(p+4),real(p+8)};};
    const auto put=[&](std::uint64_t p,float v){memory.store(GuestAddress{p},32,std::bit_cast<std::uint32_t>(v));};
    constexpr std::uint32_t signature=0x58443341;
    if(initialize){
        const auto mask=static_cast<std::uint32_t>(arg(0));auto raw=static_cast<std::uint32_t>(arg(1));
        if(!x86){raw=0;for(unsigned i=0;i<4;++i)raw|=std::uint32_t{state.xmm[1][i]}<<(8*i);}
        const auto sound=std::bit_cast<float>(raw);
        if(!mask || (mask&~0x3ffffU) || !std::isfinite(sound) || sound<std::numeric_limits<float>::min() || !arg(2))return 0x80070057U;
        memory.check(GuestAddress{arg(2)},20,Access::Write);memory.fill(GuestAddress{arg(2)},0,20);
        memory.store(GuestAddress{arg(2)},32,signature);memory.store(GuestAddress{arg(2)+4},32,mask);put(arg(2)+8,sound);return 0;
    }
    const auto listener=arg(1),emitter=arg(2),dsp=arg(4);const auto flags=static_cast<std::uint32_t>(arg(3));
    if(!arg(0) || !listener || !emitter || !dsp || u32(arg(0))!=signature)throw HostCallFault("X3DAudio 無效 instance／結構");
    const auto mask=u32(arg(0)+4);const auto sound=real(arg(0)+8);
    // 目前原生立體聲後端；延遲、環繞聲與內半徑擴散尚未實作，不能回傳空白成功。
    if((flags&~0x7dU) || (mask!=3 && mask!=4))throw HostCallFault("X3DAudio 尚未支援此旗標或喇叭配置");
    const auto front=vector(listener),top=vector(listener+12),position=vector(listener+24),velocity=vector(listener+36);
    const auto ef=vector(emitter+pointer),et=vector(emitter+pointer+12),ep=vector(emitter+pointer+24),ev=vector(emitter+pointer+36);
    const auto inner=real(emitter+pointer+48),inner_angle=real(emitter+pointer+52);const auto channels=u32(emitter+pointer+56);const auto radius=real(emitter+pointer+60);
    const auto azimuths=ptr(emitter+pointer+64),curves=emitter+pointer+64+pointer;
    const auto scale=real(curves+5*pointer),doppler_scale=real(curves+5*pointer+4);
    const auto source_count=u32(dsp+2*pointer),destination_count=u32(dsp+2*pointer+4);const auto output=dsp+2*pointer+8;
    if(inner!=0 || inner_angle!=0)throw HostCallFault("X3DAudio 內半徑擴散尚未實作");
    if(!channels || channels>64 || channels!=source_count || destination_count!=(mask==3?2U:1U) || scale<=0 || doppler_scale<0 || radius<0 || sound<=0 || (channels>1 && !azimuths))throw HostCallFault("X3DAudio 聲道或距離參數無效");
    const auto delta=sub(ep,position),direction=normalized(delta),right=normalized(cross(top,front));const auto distance=length(delta);
    if(length(right)<0.5F || length(front)<0.5F || length(ef)<0.5F || length(cross(et,ef))<0.5F)throw HostCallFault("X3DAudio 朝向無效");
    const auto angle=distance>0?std::acos(std::clamp(-dot(normalized(ef),direction),-1.0F,1.0F)):0.0F;
    const auto curve=[&](unsigned index,float normalized_distance,float fallback){
        const auto p=ptr(curves+index*pointer);if(!p)return fallback;const auto points=ptr(p);const auto count=u32(p+pointer);
        if(!points || count<2 || count>4096)throw HostCallFault("X3DAudio 距離曲線無效");
        float previous=-1,value=0,result=0;bool selected=false;
        for(unsigned i=0;i<count;++i){const auto d=real(points+i*8),v=real(points+i*8+4);
            if(d<=previous || d<0 || d>1 || v<0 || (!i && d!=0) || (i==count-1 && d!=1))throw HostCallFault("X3DAudio 距離曲線節點無效");
            if(!selected && normalized_distance<=d){result=i?value+(v-value)*(normalized_distance-previous)/(d-previous):v;selected=true;}previous=d;value=v;
        }return selected?result:value;
    };
    const auto cone=[&](std::uint64_t p,float a,unsigned pair){
        if(!p)return 1.0F;const auto inner_angle=real(p),outer_angle=real(p+4),inside=real(p+pair),outside=real(p+pair+4);
        if(inner_angle<0 || outer_angle<inner_angle || outer_angle>6.2831854F || inside<0 || outside<0)throw HostCallFault("X3DAudio 音錐參數無效");
        const auto fraction=outer_angle>inner_angle?std::clamp((2*a-inner_angle)/(outer_angle-inner_angle),0.0F,1.0F):(2*a>inner_angle?1.0F:0.0F);
        return inside+(outside-inside)*fraction;
    };
    const auto listener_angle=distance>0?std::acos(std::clamp(dot(normalized(front),direction),-1.0F,1.0F)):0.0F;
    const auto cones=[&](unsigned pair){return cone(ptr(listener+48),listener_angle,pair)*cone(channels==1?ptr(emitter):0,angle,pair);};
    const auto normalized_distance=distance/scale;
    std::vector<float> matrix;
    if(flags&1){
        if(!ptr(dsp))throw HostCallFault("X3DAudio 缺少矩陣輸出");matrix.resize(source_count*destination_count);
        const auto er=normalized(cross(et,ef));
        for(unsigned source=0;source<channels;++source){
            auto v=delta;
            if(channels>1){const auto azimuth=real(azimuths+source*4);if(azimuth<0 || azimuth>=6.2831854F)throw HostCallFault("X3DAudio LFE 或聲道方位尚未支援");
                const auto x=std::sin(azimuth)*radius,z=std::cos(azimuth)*radius;v={v.x+er.x*x+ef.x*z,v.y+er.y*x+ef.y*z,v.z+er.z*x+ef.z*z};}
            const auto d=length(v),attenuation=curve(0,d/scale,d>scale?scale/d:1.0F)*cones(8);
            const auto gains=stereo(dot(normalized(v),right),attenuation);matrix[source]=mask==4?attenuation:gains.first;
            if(mask==3)matrix[source_count+source]=gains.second;
        }
        memory.check(GuestAddress{ptr(dsp)},matrix.size()*4,Access::Write);
    }
    memory.check(GuestAddress{output},32,Access::Write);
    if(flags&4)put(output,curve(2,normalized_distance,1.0F-0.25F*std::min(normalized_distance,1.0F))*cones(16));
    if(flags&8)put(output+4,curve(3,normalized_distance,0.75F)*cones(16));
    if(flags&16)put(output+8,curve(4,normalized_distance,1.0F-std::min(normalized_distance,1.0F))*cones(24));
    if(flags&32){const auto a=dot(velocity,direction),b=dot(ev,direction);
        put(output+12,doppler(sound,a*doppler_scale,b*doppler_scale,sound*0.999F,0.5F,4.0F));put(output+24,-b);put(output+28,-a);}
    if(flags&64)put(output+16,angle);put(output+20,distance);
    for(unsigned i=0;i<matrix.size();++i)put(ptr(dsp)+i*4,matrix[i]);return 0;
}
}
