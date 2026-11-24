#pragma once
// D3D9 固定管線的像素階段（D3DTSS_*）：8 個貼圖階段的合成以 HLSL（ps_2_0）產生，供「有頂點著色器、無像素著色器」與
// 「純固定管線改走可程式化繪製」兩條路徑共用；語義依 D3D9 文件〈Texture Blending〉與 D3DTEXTUREOP。
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace x64arm::d3d9_fixed {

// 階段狀態以 key+stage*stage_stride 存放；D3DTSS_CONSTANT 為 32，間距須大於它。
inline constexpr unsigned stage_stride=64;
inline constexpr unsigned stage_count=8;

inline bool valid_stage_key(unsigned key){return (key>=1 && key<=11) || (key>=22 && key<=24) || (key>=26 && key<=28) || key==32;}

// D3D9 預設值：第 0 階段 COLOROP=MODULATE、ALPHAOP=SELECTARG1，其餘階段 DISABLE；ARG1=TEXTURE、ARG2／ARG0／RESULTARG=CURRENT；
// TEXCOORDINDEX 為階段序號；BUMPENVMAT／LSCALE／LOFFSET 為 0.0、TEXTURETRANSFORMFLAGS 為 DISABLE、CONSTANT 為 0。
inline std::map<std::uint32_t,std::uint32_t> default_stage_states(){
    std::map<std::uint32_t,std::uint32_t> result;
    for(unsigned stage=0;stage<stage_count;++stage){
        const auto base=stage*stage_stride;
        result[base+1]=stage?1U:4U;result[base+2]=2;result[base+3]=1;result[base+4]=stage?1U:2U;result[base+5]=2;result[base+6]=1;
        for(unsigned key=7;key<=10;++key)result[base+key]=0;
        result[base+11]=stage;result[base+22]=0;result[base+23]=0;result[base+24]=0;result[base+26]=1;result[base+27]=1;result[base+28]=1;result[base+32]=0;
    }
    return result;
}

struct PixelInputs {
    bool diffuse{},specular{},specular_enable{};
    std::array<bool,8> texcoord{};
    // 各階段綁定的紋理種類：0 無、1 二維、2 立方體、3 體積。
    std::array<unsigned,8> texture{};
    // 頂點端為固定管線時，PROJECTED 依 TEXTURETRANSFORMFLAGS 的座標數取除數；有頂點著色器時固定以 w 為除數。
    bool fixed_vertex{};
    // 霧化：0 無；1 FOG 輸入即霧因子（頂點霧、vs 的 oFog、已轉換頂點的鏡面 alpha）；2 FOG 輸入為視空間深度，依 table_mode
    // （1 EXP、2 EXP2、3 LINEAR）逐像素計算（表格霧）。有 FOG 輸入時以 ps_3_0 編譯。
    unsigned fog{},table_mode{};
};

inline const char* pixel_profile(const PixelInputs& inputs){return inputs.fog?"ps_3_0":"ps_2_0";}

inline unsigned state(const std::map<std::uint32_t,std::uint32_t>& states,unsigned stage,unsigned key){
    const auto found=states.find(key+stage*stage_stride);return found==states.end()?0U:found->second;
}

inline bool op_reads(unsigned op,unsigned argument){
    // argument：0＝ARG0、1＝ARG1、2＝ARG2。
    if(op<=1 || op==22 || op==23)return false;
    if(op==2)return argument==1;
    if(op==3)return argument==2;
    if(op==25 || op==26)return true;
    return argument!=0;
}

// 啟用中的階段數：遇到 COLOROP=DISABLE 即停止，後續階段全部忽略。
inline unsigned active_stages(const std::map<std::uint32_t,std::uint32_t>& states){
    unsigned count=0;while(count<stage_count && state(states,count,1)!=1)++count;return count;
}

inline bool stage_reads_texture(const std::map<std::uint32_t,std::uint32_t>& states,unsigned stage){
    const auto color=state(states,stage,1),alpha=state(states,stage,4);
    const auto reads=[&](unsigned op,unsigned arg0,unsigned arg1,unsigned arg2){
        return (op_reads(op,0) && (arg0&15U)==2) || (op_reads(op,1) && (arg1&15U)==2) || (op_reads(op,2) && (arg2&15U)==2) || op==13 || op==15 || op==22 || op==23;};
    return reads(color,state(states,stage,26),state(states,stage,2),state(states,stage,3)) || (alpha!=1 && reads(alpha,state(states,stage,27),state(states,stage,5),state(states,stage,6)));
}

// 既有 GL 固定管線（單一紋理）可表達的設定：只有第 0 階段，運算為 DISABLE／SELECTARG1／SELECTARG2／MODULATE，
// 參數為 DIFFUSE／CURRENT／TEXTURE／TFACTOR（含修飾），結果寫入 CURRENT。
inline bool single_stage_basic(const std::map<std::uint32_t,std::uint32_t>& states){
    if(active_stages(states)>1)return false;
    if(state(states,0,28)!=1)return false;
    // GL 路徑只帶一組紋理座標（第 0 組）；TEXCOORDINDEX 指向其他組時改走產生器。
    if((state(states,0,11)&0xffffU)!=0)return false;
    for(const auto key:{1U,4U})if(state(states,0,key)>4)return false;
    for(const auto key:{2U,3U,5U,6U}){const auto value=state(states,0,key);if((value&15U)>3 || (value&~0x3fU))return false;}
    return true;
}

// 階段使用 PROJECTED 或座標產生（TCI 高位）時需要頂點端支援；回傳純固定頂點產生器能否處理。
inline bool vertex_side_basic(const std::map<std::uint32_t,std::uint32_t>& states){
    const auto count=active_stages(states);
    for(unsigned stage=0;stage<count;++stage){if(state(states,stage,11)>>16)return false;if(state(states,stage,24)&0xffU)return false;}
    return true;
}

// 像素常數：c0＝TFACTOR，c1～c8＝各階段 CONSTANT，c9～c16＝BUMPENVMAT（00、01、10、11），c17～c24＝（LSCALE、LOFFSET），
// c25＝FOGCOLOR，c26＝（FOGSTART、FOGEND、FOGDENSITY）。
inline std::vector<std::uint8_t> pixel_constants(const std::map<std::uint32_t,std::uint32_t>& states,std::uint32_t texture_factor,std::uint32_t fog_color=0,
    std::uint32_t fog_start=0,std::uint32_t fog_end=0x3f800000U,std::uint32_t fog_density=0x3f800000U){
    std::vector<std::uint8_t> bytes(224*16);
    const auto put=[&](unsigned reg,std::array<float,4> value){std::memcpy(bytes.data()+reg*16,value.data(),16);};
    const auto argb=[](std::uint32_t value){return std::array<float,4>{float((value>>16)&255)/255,float((value>>8)&255)/255,float(value&255)/255,float(value>>24)/255};};
    const auto real=[](std::uint32_t value){return std::bit_cast<float>(value);};
    put(0,argb(texture_factor));
    for(unsigned stage=0;stage<stage_count;++stage){
        put(1+stage,argb(state(states,stage,32)));
        put(9+stage,{real(state(states,stage,7)),real(state(states,stage,8)),real(state(states,stage,9)),real(state(states,stage,10))});
        put(17+stage,{real(state(states,stage,22)),real(state(states,stage,23)),0,0});
    }
    put(25,argb(fog_color));put(26,{real(fog_start),real(fog_end),real(fog_density),0});
    return bytes;
}

// pixel_source 讀取的全部狀態：啟用階段數、各階段的運算／參數／RESULTARG／TEXCOORDINDEX 組別／TEXTURETRANSFORMFLAGS 與紋理種類，
// 以及輸入、鏡面色與霧化設定。相同鍵產生相同 HLSL。
inline std::vector<std::uint32_t> pixel_key(const std::map<std::uint32_t,std::uint32_t>& states,const PixelInputs& inputs){
    const auto count=active_stages(states);std::vector<std::uint32_t> key;key.reserve(8+count*12);
    std::uint32_t flags=(inputs.diffuse?1U:0U)|(inputs.specular?2U:0U)|(inputs.specular_enable?4U:0U)|(inputs.fixed_vertex?8U:0U);
    for(unsigned set=0;set<8;++set)if(inputs.texcoord[set])flags|=16U<<set;
    key.push_back(flags);key.push_back(inputs.fog|(inputs.table_mode<<8)|(count<<16));
    for(unsigned stage=0;stage<count;++stage){
        for(const auto state_key:{1U,2U,3U,4U,5U,6U,26U,27U,28U,24U})key.push_back(state(states,stage,state_key));
        key.push_back((state(states,stage,11)&7U)|(inputs.texture[stage]<<8));
    }
    return key;
}

inline std::string pixel_source(const std::map<std::uint32_t,std::uint32_t>& states,const PixelInputs& inputs){
    const auto count=active_stages(states);
    std::string head,body;
    const auto n=[](unsigned value){return std::to_string(value);};
    std::array<bool,8> sampled{};for(unsigned stage=0;stage<count;++stage)sampled[stage]=inputs.texture[stage]!=0 && stage_reads_texture(states,stage);
    // BUMPENVMAP 讀下一階段的紋理座標偏移；下一階段即使不讀紋理，偏移仍不影響結果。
    for(unsigned stage=0;stage<count;++stage)if(sampled[stage]){
        const char* type=inputs.texture[stage]==2?"samplerCUBE":inputs.texture[stage]==3?"sampler3D":"sampler2D";
        head+=std::string(type)+" s"+n(stage)+":register(s"+n(stage)+");";
    }
    head+="float4 tf:register(c0);";
    for(unsigned stage=0;stage<count;++stage)head+="float4 k"+n(stage)+":register(c"+n(1+stage)+");float4 m"+n(stage)+":register(c"+n(9+stage)+");float4 l"+n(stage)+":register(c"+n(17+stage)+");";
    head+="struct I{";
    if(inputs.diffuse)head+="float4 d:COLOR0;";
    if(inputs.specular)head+="float4 s:COLOR1;";
    for(unsigned set=0;set<8;++set)if(inputs.texcoord[set])head+="float4 t"+n(set)+":TEXCOORD"+n(set)+";";
    if(inputs.fog)head+="float f:FOG;";
    head+="};";
    if(inputs.fog)head+="float4 fogc:register(c25);float4 fogp:register(c26);";
    body+="float4 main(I i):COLOR0{";
    // 固定管線的漫射／鏡面色在內插前即夾在 [0,1]（vs 1.x／2.x 的 oD0、oD1 亦同）。
    body+=inputs.diffuse?"float4 dif=saturate(i.d);":"float4 dif=float4(1,1,1,1);";
    body+=inputs.specular?"float4 spe=saturate(i.s);":"float4 spe=float4(0,0,0,0);";
    body+="float4 cur=dif,tmp=float4(0,0,0,0);float2 bump=float2(0,0);float lum=1;";
    for(unsigned stage=0;stage<count;++stage){
        const auto s=n(stage);const auto color_op=state(states,stage,1),alpha_op=state(states,stage,4);
        const auto set=state(states,stage,11)&7U;const bool previous_bump=stage && (state(states,stage-1,1)==22 || state(states,stage-1,1)==23);
        if(sampled[stage]){
            body+="float4 x"+s+";{float4 uv="+(inputs.texcoord[set]?"i.t"+n(set):std::string("float4(0,0,0,1)"))+";";
            if(previous_bump)body+="uv.xy+=bump;";
            const auto flags=state(states,stage,24);
            if(flags&256U){const auto components=flags&0xffU;const char* divisor=inputs.fixed_vertex && components>=2 && components<=4?(components==2?"uv.y":components==3?"uv.z":"uv.w"):"uv.w";
                body+=std::string("uv.xyz/=")+divisor+";";}
            const char* fetch=inputs.texture[stage]==2?"texCUBE":inputs.texture[stage]==3?"tex3D":"tex2D";const char* swizzle=inputs.texture[stage]==1?"uv.xy":"uv.xyz";
            body+="x"+s+"="+fetch+"(s"+s+","+swizzle+");";
            if(previous_bump && state(states,stage-1,1)==23)body+="x"+s+".rgb*=lum;";
            body+="}";
        }
        const auto argument=[&](unsigned selector)->std::string{
            std::string value;switch(selector&15U){case 0:value="dif";break;case 1:value="cur";break;case 2:value="x"+s;break;case 3:value="tf";break;case 4:value="spe";break;case 5:value="tmp";break;case 6:value="k"+s;break;default:value="float4(0,0,0,0)";break;}
            if(selector&32U)value="("+value+").aaaa";if(selector&16U)value="(1-"+value+")";return value;};
        const auto reads_missing_texture=[&](unsigned op,unsigned arg0,unsigned arg1,unsigned arg2){
            if(sampled[stage])return false;
            return (op_reads(op,0) && (arg0&15U)==2) || (op_reads(op,1) && (arg1&15U)==2) || (op_reads(op,2) && (arg2&15U)==2) || op==13 || op==15;};
        const auto expression=[&](unsigned op,const std::string& a0,const std::string& a1,const std::string& a2,const char* m)->std::string{
            const std::string x0="("+a0+")"+m,x1="("+a1+")"+m,x2="("+a2+")"+m,texture_alpha="x"+s+".a";
            switch(op){
            case 2:return x1;case 3:return x2;case 4:return x1+"*"+x2;case 5:return "2*"+x1+"*"+x2;case 6:return "4*"+x1+"*"+x2;case 7:return x1+"+"+x2;
            case 8:return x1+"+"+x2+"-0.5";case 9:return "2*("+x1+"+"+x2+")-1";case 10:return x1+"-"+x2;case 11:return x1+"+"+x2+"-"+x1+"*"+x2;
            case 12:return "lerp("+x2+","+x1+",dif.a)";case 13:return "lerp("+x2+","+x1+","+texture_alpha+")";case 14:return "lerp("+x2+","+x1+",tf.a)";
            case 15:return x1+"+"+x2+"*(1-"+texture_alpha+")";case 16:return "lerp("+x2+","+x1+",cur.a)";case 17:return x1;
            case 18:return x1+"+("+a1+").a*"+x2;case 19:return x1+"*"+x2+"+("+a1+").a";case 20:return "(1-("+a1+").a)*"+x2+"+"+x1;case 21:return "(1-"+x1+")*"+x2+"+("+a1+").a";
            case 25:return x0+"+"+x1+"*"+x2;case 26:return "lerp("+x2+","+x1+","+x0+")";
            default:return "";}
        };
        const bool to_temp=state(states,stage,28)==5;
        body+="{float4 r="+std::string(to_temp?"tmp":"cur")+";";
        if(color_op==24){
            const auto a1=argument(state(states,stage,2)),a2=argument(state(states,stage,3));
            if(!reads_missing_texture(color_op,0,state(states,stage,2),state(states,stage,3)))body+="r=saturate(4*dot(("+a1+").rgb-0.5,("+a2+").rgb-0.5)).xxxx;";
        }else{
            if(color_op==22 || color_op==23){
                // 本階段紋理為（du,dv,l）：以 BUMPENVMAT 轉換後偏移下一階段座標；色彩輸出沿用 CURRENT。
                if(sampled[stage]){body+="bump=float2(m"+s+".x*x"+s+".r+m"+s+".z*x"+s+".g,m"+s+".y*x"+s+".r+m"+s+".w*x"+s+".g);";
                    if(color_op==23)body+="lum=saturate(x"+s+".b*l"+s+".x+l"+s+".y);";}
            }else if(!reads_missing_texture(color_op,state(states,stage,26),state(states,stage,2),state(states,stage,3))){
                const auto value=expression(color_op,argument(state(states,stage,26)),argument(state(states,stage,2)),argument(state(states,stage,3)),".rgb");
                if(!value.empty())body+="r.rgb=saturate("+value+");";
            }
            if(alpha_op!=1 && alpha_op!=22 && alpha_op!=23 && !reads_missing_texture(alpha_op,state(states,stage,27),state(states,stage,5),state(states,stage,6))){
                const auto value=alpha_op==24?"saturate(4*dot(("+argument(state(states,stage,5))+").rgb-0.5,("+argument(state(states,stage,6))+").rgb-0.5))":
                    expression(alpha_op,argument(state(states,stage,27)),argument(state(states,stage,5)),argument(state(states,stage,6)),".a");
                if(!value.empty())body+="r.a=saturate("+value+");";
            }
        }
        body+=std::string(to_temp?"tmp":"cur")+"=r;}";
    }
    if(inputs.specular_enable)body+="cur.rgb=saturate(cur.rgb+spe.rgb);";
    // 霧化在貼圖合成與鏡面色之後：f＝1 無霧、0 全霧，色彩＝lerp(FOGCOLOR, 色彩, f)；alpha 不變。
    if(inputs.fog==1)body+="cur.rgb=lerp(fogc.rgb,cur.rgb,saturate(i.f));";
    else if(inputs.fog==2){
        if(inputs.table_mode==3)body+="cur.rgb=lerp(fogc.rgb,cur.rgb,saturate((fogp.y-i.f)/(fogp.y-fogp.x)));";
        else if(inputs.table_mode==1)body+="cur.rgb=lerp(fogc.rgb,cur.rgb,saturate(exp(-fogp.z*i.f)));";
        else if(inputs.table_mode==2)body+="{float d=fogp.z*i.f;cur.rgb=lerp(fogc.rgb,cur.rgb,saturate(exp(-d*d)));}";
    }
    body+="return cur;}";
    return head+body;
}

}
