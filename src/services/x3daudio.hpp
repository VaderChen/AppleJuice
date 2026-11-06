#pragma once
#include "x64arm/windows_abi.hpp"
#include "audio_spatial.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>
namespace x64arm::x3daudio {
inline constexpr float pi=3.141592654F,two_pi=6.283185307F;
struct Speaker {float azimuth{};unsigned row{};};
struct SpeakerLayout {std::array<Speaker,11> ring{};unsigned speakers{},rows{};int lfe{-1};};
// 矩陣列依遮罩位元順序（同 WAVEFORMATEXTENSIBLE）；LFE 不參與水平環。方位依文件為順時針（前 0、右 π/2）。
// 官方未列各喇叭方位，表值取自 FAudio F3DAudio 對照真實 X3DAudio 的紀錄，包含立體聲左右實際位於 ±π/2。
inline SpeakerLayout speaker_layout(std::uint32_t mask,bool zero_center){
    static constexpr float azimuths[11]{pi*7/4,pi/4,0,0,pi*5/4,pi*3/4,pi*15/8,pi/8,pi,pi*3/2,pi/2};
    SpeakerLayout layout;const bool side_pair=(mask&0x7f7U)==3U;
    for(unsigned bit=0;bit<11;++bit){
        if(!((mask>>bit)&1U))continue;
        const auto row=layout.rows++;
        if(bit==3){layout.lfe=static_cast<int>(row);continue;}
        // ZEROCENTER 讓中置靜音並改由相鄰喇叭承接；只有中置一顆時沒有其他喇叭可承接。
        if(bit==2 && zero_center && (mask&0x7f3U))continue;
        layout.ring[layout.speakers++]={side_pair?(bit?pi/2:pi*3/2):azimuths[bit],row};
    }
    std::sort(layout.ring.begin(),layout.ring.begin()+layout.speakers,[](const Speaker& a,const Speaker& b){return a.azimuth<b.azimuth;});
    return layout;
}
// 官方示意圖的內半徑區域：聽者水平面上半徑 InnerRadius 的圓，往上下以 InnerRadiusAngle（與垂直軸夾角）張開的雙錐台。
// 回傳聲源相對區域邊界的距離；大於 1 表示在區域外，內半徑與內半徑角依文件沒有作用。文件限定角度上限為 π/4。
inline float inner_region_distance(float radial,float height,float radius,float angle){
    return radial/std::max(std::max(radius,0.0F)+std::abs(height)*std::tan(std::clamp(angle,0.0F,pi/4)),4e-7F);
}
struct Diffusion {float all{},matching{},opposite{};};
// 能量分給全部喇叭、最接近的兩顆與對向兩顆的比例，總和為 1。文件定義區域內逐漸滲入對向喇叭，到聽者位置或正上下方時
// 所有喇叭相等；未公布曲線，0.707107、1e-7、4e-7 與分段方式取自 FAudio 對真實 X3DAudio 的實驗（中段為線性近似）。
inline Diffusion diffuse(float radial,float region){
    constexpr float midpoint=0.707107F;
    if(radial<=1e-7F)return {1,0,0};
    if(region<=0.5F){const auto all=1-2*region,matching=2*region*midpoint;return {all,matching,1-all-matching};}
    if(region<=1){const auto matching=midpoint+(1-midpoint)*(2*region-1);return {0,matching,1-matching};}
    return {0,1,0};
}
// 文件：聲音由最接近的兩顆喇叭發出，正對某喇叭時只有該喇叭出聲。依方位線性分配取自 FAudio 對照紀錄。
template<class Add> void pan(const SpeakerLayout& layout,float azimuth,float energy,Add&& add){
    if(layout.speakers<2){if(layout.speakers)add(layout.ring[0].row,energy);return;}
    const auto first=layout.ring[0].azimuth;auto at=std::fmod(azimuth-first,two_pi);if(at<0)at+=two_pi;at+=first;
    for(unsigned i=0;i<layout.speakers;++i){
        const bool last=i+1==layout.speakers;const auto current=layout.ring[i].azimuth,next=last?first+two_pi:layout.ring[i+1].azimuth;
        if(at>=next && !last)continue;
        const auto share=std::clamp((at-current)/(next-current),0.0F,1.0F);
        add(layout.ring[i].row,(1-share)*energy);add(layout.ring[last?0:i+1].row,share*energy);return;
    }
}
// X64ARM_X3DAUDIO_LINEAR_PAN=0 對照舊的立體聲增益律（聲像置中時兩聲道皆為全音量），只影響立體聲矩陣。
inline bool linear_pan(){static const bool enabled=[] {const auto* value=std::getenv("X64ARM_X3DAUDIO_LINEAR_PAN");return !value || std::string_view(value)!="0";}();return enabled;}
inline std::uint64_t invoke(GuestMemory& memory,const WindowsCall& arg,const GuestState& state,bool initialize,bool x86){
    using namespace audio::spatial;
    const unsigned pointer=x86?4:8;const std::uint8_t bits=x86?32:64;
    const auto u32=[&](std::uint64_t p){return static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));};
    // 文件：NaN／無限值會讓音訊輸出靜音，不是呼叫失敗。只讀本次計算用到的欄位，未用到的欄位可為任意值。
    // real() 先換成 fallback 並標記 invalid：矩陣計算結束前讀到非有限值時，矩陣與延遲歸零；之後才讀的 LPF／殘響曲線與都卜勒欄位只影響各自輸出。
    // 之後的合法性檢查看的是換過的值，所以距離曲線第 2 個以後節點的 Distance（換成 0 後不再遞增）與 InnerAngle>0 時音錐的 OuterAngle
    //（換成 0 後小於 InnerAngle）仍視為非法參數而致命，不靜音。
    bool invalid=false;
    const auto real=[&](std::uint64_t p,float fallback=0.0F){const auto v=std::bit_cast<float>(u32(p));if(std::isfinite(v))return v;invalid=true;return fallback;};
    const auto ptr=[&](std::uint64_t p){return memory.load(GuestAddress{p},bits);};
    const auto vector=[&](std::uint64_t p){return Vector{real(p),real(p+4),real(p+8)};};
    const auto put=[&](std::uint64_t p,float v){memory.store(GuestAddress{p},32,std::bit_cast<std::uint32_t>(std::isfinite(v)?v:0.0F));};
    constexpr std::uint32_t signature=0x58443341;
    if(initialize){
        const auto mask=static_cast<std::uint32_t>(arg(0));auto raw=static_cast<std::uint32_t>(arg(1));
        if(!x86){raw=0;for(unsigned i=0;i<4;++i)raw|=std::uint32_t{state.xmm[1][i]}<<(8*i);}
        const auto sound=std::bit_cast<float>(raw);
        if(!mask || (mask&~0x3ffffU) || !std::isfinite(sound) || sound<std::numeric_limits<float>::min() || !arg(2))return 0x80070057U;
        memory.check(GuestAddress{arg(2)},20,Access::Write);memory.fill(GuestAddress{arg(2)},0,20);
        memory.store(GuestAddress{arg(2)},32,signature);memory.store(GuestAddress{arg(2)+4},32,mask);put(arg(2)+8,sound);return 0;
    }
    constexpr std::uint32_t flag_matrix=1,flag_delay=2,flag_lpf_direct=4,flag_lpf_reverb=8,flag_reverb=0x10,flag_doppler=0x20,flag_angle=0x40,flag_zero_center=0x10000,flag_redirect_lfe=0x20000;
    const auto listener=arg(1),emitter=arg(2),dsp=arg(4);const auto flags=static_cast<std::uint32_t>(arg(3));
    if(!arg(0) || !listener || !emitter || !dsp || u32(arg(0))!=signature)throw HostCallFault("X3DAudio 無效 instance／結構");
    const auto mask=u32(arg(0)+4);
    // 未定義旗標與高度喇叭遮罩沒有文件語義，維持明確失敗；其餘旗標與水平喇叭配置都實際計算。
    if((flags&~0x3007fU) || (mask&~0x7ffU))throw HostCallFault("X3DAudio 未定義旗標或不支援的高度喇叭配置");
    const auto channels=u32(emitter+pointer+56);const auto curves=emitter+pointer+64+pointer,output=dsp+2*pointer+8;
    const bool uses_distance=flags&(flag_matrix|flag_lpf_direct|flag_lpf_reverb|flag_reverb),oriented=flags&(flag_matrix|flag_delay),multi_matrix=(flags&flag_matrix) && channels>1;
    // 文件：聽者朝向只用於矩陣與延遲，有音錐時另用於 LPF／殘響；發射端朝向只用於夾角、單聲道音錐與多聲道矩陣。
    // 全零朝向在未使用時合法；使用時不合法但真實 API 沒有失敗回報，依實際值計算，零向量視為沒有方向。
    const auto listener_cone=uses_distance?ptr(listener+48):UINT64_C(0),emitter_cone=uses_distance && channels==1?ptr(emitter):UINT64_C(0);
    const auto front=oriented || listener_cone?vector(listener):Vector{},top=oriented?vector(listener+12):Vector{};
    const auto ef=(flags&flag_angle) || emitter_cone || multi_matrix?vector(emitter+pointer):Vector{},et=multi_matrix?vector(emitter+pointer+12):Vector{};
    const auto ep=vector(emitter+pointer+24),position=vector(listener+24);
    const auto scale=uses_distance?real(curves+5*pointer,1.0F):1.0F;
    if(scale<=0)throw HostCallFault("X3DAudio CurveDistanceScaler 無效");
    const auto delta=sub(ep,position),direction=normalized(delta);const auto distance=length(delta);
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
    const auto cones=[&](unsigned pair){return cone(listener_cone,listener_angle,pair)*cone(emitter_cone,angle,pair);};
    const auto normalized_distance=distance/scale;
    std::vector<float> matrix;
    if(flags&flag_matrix){
        const auto layout=speaker_layout(mask,flags&flag_zero_center);
        const auto source_count=u32(dsp+2*pointer),destination_count=u32(dsp+2*pointer+4);
        const auto region_radius=real(emitter+pointer+48),region_angle=real(emitter+pointer+52);
        const auto radius=channels>1?real(emitter+pointer+60):0.0F;const auto azimuths=channels>1?ptr(emitter+pointer+64):UINT64_C(0);
        if(!ptr(dsp))throw HostCallFault("X3DAudio 缺少矩陣輸出");
        if(!channels || channels>64 || channels!=source_count || destination_count!=layout.rows || radius<0 || (channels>1 && !azimuths))throw HostCallFault("X3DAudio 聲道或距離參數無效");
        matrix.resize(std::size_t{source_count}*destination_count);
        const auto up=normalized(top),forward=normalized(front),right=normalized(cross(top,front)),er=normalized(cross(et,ef));
        const auto listener_volume=cone(listener_cone,listener_angle,8),emitter_volume=cone(emitter_cone,angle,8);
        const auto mix=[&](unsigned source,Vector v,float attenuation){
            const auto add=[&](unsigned row,float value){matrix[row*source_count+source]+=value;};
            const auto height=dot(up,v);const Vector plane{v.x-up.x*height,v.y-up.y*height,v.z-up.z*height};const auto radial=length(plane);
            const auto region=inner_region_distance(radial,height,region_radius,region_angle);
            if(mask==3 && !linear_pan()){const auto gains=stereo(dot(normalized(v),right)*std::min(region,1.0F),attenuation);add(0,gains.first);add(1,gains.second);return;}
            const auto shares=diffuse(radial,region);const auto azimuth=std::atan2(dot(right,plane),dot(forward,plane));
            if(shares.all>0)for(unsigned i=0;i<layout.speakers;++i)add(layout.ring[i].row,shares.all*attenuation/static_cast<float>(layout.speakers));
            if(shares.matching>0)pan(layout,azimuth,shares.matching*attenuation,add);
            if(shares.opposite>0)pan(layout,azimuth+pi,shares.opposite*attenuation,add);
        };
        // 文件：方位 X3DAUDIO_2PI 是位於發射端基準點的 LFE 聲道，只依 pLFECurve 衰減；目的沒有 LFE 喇叭時靜音。
        // 音錐只有聽者端作用於 LFE（取自 FAudio 對照紀錄），預設曲線與音量曲線同為距離反比。
        const auto lfe_gain=layout.lfe>=0?curve(1,normalized_distance,normalized_distance>1?1/normalized_distance:1.0F)*listener_volume:0.0F;
        const auto lfe_row=layout.lfe>=0?static_cast<unsigned>(layout.lfe):0U;bool source_lfe=false;
        for(unsigned source=0;source<channels;++source){
            auto v=delta;
            if(channels>1){const auto azimuth=real(azimuths+source*4);
                if(azimuth>=two_pi){source_lfe=true;if(layout.lfe>=0)matrix[lfe_row*source_count+source]=lfe_gain;continue;}
                const auto x=std::sin(azimuth)*radius,z=std::cos(azimuth)*radius;v={v.x+er.x*x+ef.x*z,v.y+er.y*x+ef.y*z,v.z+er.z*x+ef.z*z};}
            const auto d=length(v);mix(source,v,curve(0,d/scale,d>scale?scale/d:1.0F)*listener_volume*emitter_volume);
        }
        // 文件：REDIRECT_TO_LFE 只在來源沒有 LFE、目的有 LFE 時，把所有來源聲道等量混入 LFE。
        if((flags&flag_redirect_lfe) && layout.lfe>=0 && !source_lfe)for(unsigned source=0;source<channels;++source)matrix[lfe_row*source_count+source]+=lfe_gain/static_cast<float>(channels);
        if(invalid){std::fill(matrix.begin(),matrix.end(),0.0F);static const bool trace=std::getenv("X64ARM_TRACE_AUDIO")!=nullptr;if(trace)std::fputs("[x3daudio] 非有限輸入，本次矩陣靜音\n",stderr);}
        memory.check(GuestAddress{ptr(dsp)},matrix.size()*4,Access::Write);
    }
    // 文件只保證立體聲時回傳各目的聲道的毫秒延遲，其他配置不回傳；SpeedOfSound 僅供都卜勒，公式未公布。
    // 此處以 Woodworth 球頭模型（頭半徑 8.75 cm、聲速 343 m/s）估算兩耳時間差：近耳 0、遠耳延遲。數值不是官方公式。
    std::array<float,2> delays{};const auto delay_count=(flags&flag_delay) && mask==3?std::min(u32(dsp+2*pointer+4),2U):0U;
    if(delay_count){
        if(!ptr(dsp+pointer))throw HostCallFault("X3DAudio 缺少延遲輸出");
        const auto lateral=dot(direction,normalized(cross(top,front)));const auto theta=std::asin(std::min(std::abs(lateral),1.0F));
        if(!invalid)delays[lateral>0?0:1]=0.0875F/343.0F*1000.0F*(theta+std::sin(theta));
        memory.check(GuestAddress{ptr(dsp+pointer)},delay_count*std::size_t{4},Access::Write);
    }
    memory.check(GuestAddress{output},32,Access::Write);
    if(flags&flag_lpf_direct)put(output,curve(2,normalized_distance,1.0F-0.25F*std::min(normalized_distance,1.0F))*cones(16));
    if(flags&flag_lpf_reverb)put(output+4,curve(3,normalized_distance,0.75F)*cones(16));
    if(flags&flag_reverb)put(output+8,curve(4,normalized_distance,1.0F-std::min(normalized_distance,1.0F))*cones(24));
    if(flags&flag_doppler){
        const auto sound=real(arg(0)+8),doppler_scale=real(curves+5*pointer+4);if(sound<=0 || doppler_scale<0)throw HostCallFault("X3DAudio 都卜勒參數無效");
        const auto a=dot(vector(listener+36),direction),b=dot(vector(emitter+pointer+36),direction);
        put(output+12,doppler(sound,a*doppler_scale,b*doppler_scale,sound*0.999F,0.5F,4.0F));put(output+24,-b);put(output+28,-a);}
    if(flags&flag_angle)put(output+16,angle);
    put(output+20,distance);
    for(std::size_t i=0;i<matrix.size();++i)put(ptr(dsp)+i*4,matrix[i]);
    for(unsigned i=0;i<delay_count;++i)put(ptr(dsp+pointer)+i*4,delays[i]);
    return 0;
}
}
