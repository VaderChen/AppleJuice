#include <metal_stdlib>
using namespace metal;
struct V {float4 position,color,texcoord,normal,ambient,diffuse,specular,emission,shininess;};
struct Light {float4 ambient,diffuse,specular,position,direction,spot,attenuation;};
float3 unit_vector(float3 v){float n=length(v);return n>0?v/n:float3(0);}
float4 light_vertex(const device V& v,float4 eye,constant float4x4& normal,
                    constant Light* lights,constant float4& flags,thread float4& secondary){
 float3 n=(normal*float4(v.normal.xyz,0)).xyz;if(flags.y!=0)n=unit_vector(n);
 // lights[8] 為光照模型：ambient＝全域環境光，spot.x＝區域觀察者，spot.z＝分離鏡面色。
 constant Light& model=lights[8];
 float3 viewer=model.spot.x!=0 && eye.w!=0?unit_vector(-eye.xyz/eye.w):float3(0,0,1);
 float3 color=v.emission.rgb+model.ambient.rgb*v.ambient.rgb,specular_sum=float3(0);
 for(uint i=0;i<8;++i){if(!(uint(flags.z)&(1u<<i)))continue;
  constant Light& light=lights[i];float3 delta=light.position.w==0?light.position.xyz:light.position.xyz/light.position.w-eye.xyz/eye.w;
  float distance=length(delta);float3 direction=unit_vector(delta);float attenuation=1;
  if(light.position.w!=0)attenuation=1/(light.attenuation.x+light.attenuation.y*distance+light.attenuation.z*distance*distance);
  if(light.spot.z!=180){float cosine=dot(-direction,unit_vector(light.direction.xyz));
   attenuation*=cosine<light.spot.y?0:pow(max(cosine,0.0f),light.spot.x);}
  float diffuse=max(dot(n,direction),0.0f);
  float specular=diffuse>0?pow(max(dot(n,unit_vector(direction+viewer)),0.0f),v.shininess.x):0;
  color+=attenuation*(v.ambient.rgb*light.ambient.rgb+diffuse*v.diffuse.rgb*light.diffuse.rgb);
  specular_sum+=attenuation*specular*v.specular.rgb*light.specular.rgb;
 }
 if(model.spot.z!=0)secondary=float4(clamp(specular_sum,0.0f,1.0f),0);else color+=specular_sum;
 return clamp(float4(color,v.diffuse.a),0.0f,1.0f);
}
// stipple：點畫線的主軸像素計數（不做透視內插）。
struct O {float4 position [[position]];float4 color;float4 texcoord;float4 secondary;float eyeZ;float point [[point_size]];float stipple [[center_no_perspective]];};
// vertex_main 另帶 8 個使用者剪裁距離；片段函式沿用 O，多出的成員只供光柵化裁切。
struct ClipO {float4 position [[position]];float4 color;float4 texcoord;float4 secondary;float eyeZ;float point [[point_size]];float stipple [[center_no_perspective]];float clip [[clip_distance]] [8];};
vertex ClipO vertex_main(const device V* v [[buffer(0)]],constant float4x4& m [[buffer(1)]],constant float4x4& model [[buffer(2)]],constant float4x4& normal [[buffer(3)]],constant Light* lights [[buffer(4)]],constant float4& flags [[buffer(5)]],constant float& pointSize [[buffer(6)]],constant float4* clipPlanes [[buffer(7)]],uint id [[vertex_id]]){
 ClipO o;o.secondary=float4(0);
 // glClipPlane 的眼座標平面；未啟用的平面距離固定為 1。
 {float4 eye=model*v[id].position;uint mask=uint(clipPlanes[8].x);for(uint i=0;i<8;++i)o.clip[i]=(mask&(1u<<i))?dot(clipPlanes[i],eye):1.0f;}o.eyeZ=(model*v[id].position).z;o.position=m*v[id].position;o.position.z=(o.position.z+o.position.w)*0.5;o.color=flags.x!=0?light_vertex(v[id],model*v[id].position,normal,lights,flags,o.secondary):clamp(v[id].color,0.0f,1.0f);o.texcoord=v[id].texcoord;o.point=pointSize;o.stipple=v[id].shininess.y;return o;
}
float4 d3d_texture_argument(uint selector,float4 diffuse,float4 texture,float4 factor){float4 value=(selector&15)==2?texture:(selector&15)==3?factor:diffuse;if(selector&32)value=value.aaaa;if(selector&16)value=1-value;return value;}
// 固定管線片段處理；fragment_main 與 glDrawPixels／glCopyPixels 的影像變體共用。
static float4 fixed_fragment(O in,texture2d<float> tex,sampler sample,constant uint4& textureMode,constant float4* fog,constant float4* fixedState,constant uint* stipple){
 // 多邊形點畫以 GL 視窗座標（左下原點）查 32×32 表；點畫線以計數除以 factor 的低 4 位元查 16 位元樣式。
 if(stipple[0]&1u){uint x=uint(in.position.x)&31u,y=(stipple[1]-1u-uint(in.position.y))&31u;if(!((stipple[4+y]>>x)&1u))discard_fragment();}
 if(stipple[0]&2u){uint counter=uint(floor(in.stipple/float(stipple[3])))&15u;if(!((stipple[2]>>counter)&1u))discard_fragment();}
 float4 color=in.color;
 if(textureMode.x){float4 sampled=(textureMode.x&1u)?tex.sample(sample,in.texcoord.xy/in.texcoord.w):float4(1);
  if(textureMode.w&0x80000000u){float4 c1=d3d_texture_argument(textureMode.w&63,in.color,sampled,fixedState[0]),c2=d3d_texture_argument((textureMode.w>>6)&63,in.color,sampled,fixedState[0]),a1=d3d_texture_argument((textureMode.w>>12)&63,in.color,sampled,fixedState[0]),a2=d3d_texture_argument((textureMode.w>>18)&63,in.color,sampled,fixedState[0]);color=float4(textureMode.y==2?c1.rgb:textureMode.y==3?c2.rgb:c1.rgb*c2.rgb,textureMode.z==2?a1.a:textureMode.z==3?a2.a:a1.a*a2.a);}
  else {color.rgb=textureMode.y==2?sampled.rgb:textureMode.y==3?in.color.rgb:textureMode.y==5?mix(in.color.rgb,sampled.rgb,sampled.a):textureMode.y==6?mix(in.color.rgb,fixedState[0].rgb,sampled.rgb):textureMode.y==7?min(in.color.rgb+sampled.rgb,1.0f):in.color.rgb*sampled.rgb;
  color.a=textureMode.z==2?sampled.a:textureMode.z==3?in.color.a:in.color.a*sampled.a;}}
 color.rgb=clamp(color.rgb+in.secondary.rgb,0.0f,1.0f);
 // 與色彩／深度寫入共用 fragment；clear 不啟用 alpha test。
 uint function=uint(fixedState[1].x);float reference=fixedState[1].y;
 if(function){bool pass=function==0x207 || (function==0x201 && color.a<reference) || (function==0x202 && color.a==reference) || (function==0x203 && color.a<=reference) || (function==0x204 && color.a>reference) || (function==0x205 && color.a!=reference) || (function==0x206 && color.a>=reference);if(!pass)discard_fragment();}
 float mode=fog[1].w;
 if(mode!=0){float z=abs(in.eyeZ),d=fog[1].x*z;
  float f=mode==float(0x2601)?(fog[1].z-z)/(fog[1].z-fog[1].y):mode==float(0x800)?exp(-d):exp(-d*d);
  color.rgb=mix(fog[0].rgb,color.rgb,clamp(f,0.0f,1.0f));}
 return color;
}
fragment float4 fragment_main(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]],constant uint* stipple [[buffer(3)]]){
 return fixed_fragment(in,tex,sample,textureMode,fog,fixedState,stipple);
}
// glDrawPixels／glCopyPixels：pixels[0] 為光柵位置 x、y 與 zoom x、y，pixels[1] 為影像寬、高與繪製表面高度。
// 片段中心換成 GL 視窗座標（左下原點）後依 zoom 換算影像的行與列，超出影像的片段丟棄。
static uint2 pixel_image_texel(float4 position,constant float4* pixels){
 float x=position.x,y=pixels[1].z-position.y;float i=floor((x-pixels[0].x)/pixels[0].z),j=floor((y-pixels[0].y)/pixels[0].w);
 if(i<0 || j<0 || i>=pixels[1].x || j>=pixels[1].y)discard_fragment();
 return uint2(uint(i),uint(j));
}
// 色彩影像取代片段色彩，之後照常經過紋理環境、霧化與 alpha 測試。
fragment float4 fragment_pixels(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]],constant uint* stipple [[buffer(3)]],texture2d<float> image [[texture(1)]],constant float4* pixels [[buffer(4)]]){
 O shaded=in;shaded.color=image.read(pixel_image_texel(in.position,pixels));return fixed_fragment(shaded,tex,sample,textureMode,fog,fixedState,stipple);
}
// 深度影像輸出片段深度（視窗座標 [0,1]），色彩為光柵色彩。
struct PixelDepth {float4 color [[color(0)]];float depth [[depth(any)]];};
fragment PixelDepth fragment_pixels_depth(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]],constant uint* stipple [[buffer(3)]],texture2d<float> image [[texture(1)]],constant float4* pixels [[buffer(4)]]){
 PixelDepth out;out.depth=image.read(pixel_image_texel(in.position,pixels)).r;out.color=fixed_fragment(in,tex,sample,textureMode,fog,fixedState,stipple);return out;
}
// glLogicOp：來源與目前像素（framebuffer fetch）換成 8 位元整數，依 4 位元真值表（位元 0＝s&d、1＝s&~d、2＝~s&d、3＝~s&~d）組合。
static float4 logic_operation(float4 source,float4 destination,uint code){
 uint4 s=uint4(round(saturate(source)*255.0f)),d=uint4(round(saturate(destination)*255.0f)),r=uint4(0);
 if(code&1u)r|=s&d;if(code&2u)r|=s&~d;if(code&4u)r|=~s&d;if(code&8u)r|=~s&~d;
 return float4(r&255u)/255.0f;
}
fragment float4 fragment_main_logic(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]],constant uint* stipple [[buffer(3)]],float4 destination [[color(0)]],constant uint& logic [[buffer(5)]]){
 return logic_operation(fixed_fragment(in,tex,sample,textureMode,fog,fixedState,stipple),destination,logic);
}
fragment float4 fragment_pixels_logic(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]],constant uint* stipple [[buffer(3)]],texture2d<float> image [[texture(1)]],constant float4* pixels [[buffer(4)]],float4 destination [[color(0)]],constant uint& logic [[buffer(5)]]){
 O shaded=in;shaded.color=image.read(pixel_image_texel(in.position,pixels));return logic_operation(fixed_fragment(shaded,tex,sample,textureMode,fog,fixedState,stipple),destination,logic);
}
fragment PixelDepth fragment_pixels_depth_logic(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]],constant uint* stipple [[buffer(3)]],texture2d<float> image [[texture(1)]],constant float4* pixels [[buffer(4)]],float4 destination [[color(0)]],constant uint& logic [[buffer(5)]]){
 PixelDepth out;out.depth=image.read(pixel_image_texel(in.position,pixels)).r;out.color=logic_operation(fixed_fragment(in,tex,sample,textureMode,fog,fixedState,stipple),destination,logic);return out;
}
// glAccum(GL_RETURN)：累積緩衝與繪製表面同為 Metal 座標，依 value 倍數夾到 [0,1] 後寫回。
fragment float4 fragment_accum_return(O in [[stage_in]],texture2d<float> image [[texture(1)]],constant float4* pixels [[buffer(4)]]){
 return clamp(image.read(uint2(in.position.xy))*pixels[0].x,0.0f,1.0f);
}
// glAccum 其餘運算與 glClear(GL_ACCUM_BUFFER_BIT)：params[0] 為 value 與運算（0 為清除），params[1] 為清除值；region 為剪裁矩形。
kernel void accum_operation(texture2d<float,access::read_write> accum [[texture(0)]],texture2d<float,access::read> color [[texture(1)]],constant float4* params [[buffer(0)]],constant uint4& region [[buffer(1)]],uint2 id [[thread_position_in_grid]]){
 if(id.x>=region.z || id.y>=region.w)return;uint2 at=region.xy+id;uint op=uint(params[0].y);float value=params[0].x;float4 result;
 if(op==0x100u)result=accum.read(at)+value*color.read(at);else if(op==0x101u)result=value*color.read(at);else if(op==0x103u)result=accum.read(at)*value;else if(op==0x104u)result=accum.read(at)+value;else result=params[1];
 accum.write(result,at);
}
// 模板影像直接輸出模板值，色彩寫入遮罩關閉。
struct PixelStencil {float4 color [[color(0)]];uint stencil [[stencil]];};
fragment PixelStencil fragment_pixels_stencil(O in [[stage_in]],texture2d<float> image [[texture(1)]],constant float4* pixels [[buffer(4)]]){
 PixelStencil out;out.color=float4(0);out.stencil=uint(image.read(pixel_image_texel(in.position,pixels)).r);return out;
}

// D3D9 原始 XYZ 頂點由 GPU 依 world、view、projection 順序轉換。
struct FixedV {float4 position,color,texcoord,secondary;};
vertex O fixed_vertex_main(const device FixedV* vertices [[buffer(0)]],constant float4x4* matrices [[buffer(1)]],constant float2& viewport [[buffer(2)]],uint id [[vertex_id]]){
 // 視空間 z 供霧化使用（D3D9 的頂點霧以 world×view 後的 z 為距離）。
 float4 eye=matrices[1]*(matrices[0]*vertices[id].position);float4 position=matrices[2]*eye;
 position.x+=position.w/viewport.x;position.y-=position.w/viewport.y;
 // 沿用既有 D3D9 → GL → Metal 的半像素與深度轉換。
 position.z=2*position.z-position.w;position.z=(position.z+position.w)*0.5;
 O out;out.secondary=vertices[id].secondary;out.position=position;out.color=clamp(vertices[id].color,0.0f,1.0f);out.texcoord=vertices[id].texcoord;out.eyeZ=eye.z;out.point=1;out.stipple=0;return out;
}
