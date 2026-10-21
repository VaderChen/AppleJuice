#include <metal_stdlib>
using namespace metal;
struct V {float4 position,color,texcoord,normal,ambient,diffuse,specular,emission,shininess;};
struct Light {float4 ambient,diffuse,specular,position,direction,spot,attenuation;};
float3 unit_vector(float3 v){float n=length(v);return n>0?v/n:float3(0);}
float4 light_vertex(const device V& v,float4 eye,constant float4x4& normal,
                    constant Light* lights,constant float4& flags){
 float3 n=(normal*float4(v.normal.xyz,0)).xyz;if(flags.y!=0)n=unit_vector(n);
 float3 color=v.emission.rgb+0.2f*v.ambient.rgb;
 for(uint i=0;i<8;++i){if(!(uint(flags.z)&(1u<<i)))continue;
  constant Light& light=lights[i];float3 delta=light.position.w==0?light.position.xyz:light.position.xyz/light.position.w-eye.xyz/eye.w;
  float distance=length(delta);float3 direction=unit_vector(delta);float attenuation=1;
  if(light.position.w!=0)attenuation=1/(light.attenuation.x+light.attenuation.y*distance+light.attenuation.z*distance*distance);
  if(light.spot.z!=180){float cosine=dot(-direction,unit_vector(light.direction.xyz));
   attenuation*=cosine<light.spot.y?0:pow(max(cosine,0.0f),light.spot.x);}
  float diffuse=max(dot(n,direction),0.0f);
  float specular=diffuse>0?pow(max(dot(n,unit_vector(direction+float3(0,0,1))),0.0f),v.shininess.x):0;
  color+=attenuation*(v.ambient.rgb*light.ambient.rgb+diffuse*v.diffuse.rgb*light.diffuse.rgb+specular*v.specular.rgb*light.specular.rgb);
 }
 return clamp(float4(color,v.diffuse.a),0.0f,1.0f);
}
struct O {float4 position [[position]];float4 color;float4 texcoord;float4 secondary;float eyeZ;float point [[point_size]];};
vertex O vertex_main(const device V* v [[buffer(0)]],constant float4x4& m [[buffer(1)]],constant float4x4& model [[buffer(2)]],constant float4x4& normal [[buffer(3)]],constant Light* lights [[buffer(4)]],constant float4& flags [[buffer(5)]],uint id [[vertex_id]]){
 O o;o.secondary=float4(0);o.eyeZ=(model*v[id].position).z;o.position=m*v[id].position;o.position.z=(o.position.z+o.position.w)*0.5;o.color=flags.x!=0?light_vertex(v[id],model*v[id].position,normal,lights,flags):clamp(v[id].color,0.0f,1.0f);o.texcoord=v[id].texcoord;o.point=1;return o;
}
float4 d3d_texture_argument(uint selector,float4 diffuse,float4 texture,float4 factor){float4 value=(selector&15)==2?texture:(selector&15)==3?factor:diffuse;if(selector&32)value=value.aaaa;if(selector&16)value=1-value;return value;}
fragment float4 fragment_main(O in [[stage_in]],texture2d<float> tex [[texture(0)]],sampler sample [[sampler(0)]],constant uint4& textureMode [[buffer(0)]],constant float4* fog [[buffer(1)]],constant float4* fixedState [[buffer(2)]]){
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

// D3D9 原始 XYZ 頂點由 GPU 依 world、view、projection 順序轉換。
struct FixedV {float4 position,color,texcoord,secondary;};
vertex O fixed_vertex_main(const device FixedV* vertices [[buffer(0)]],constant float4x4* matrices [[buffer(1)]],constant float2& viewport [[buffer(2)]],uint id [[vertex_id]]){
 float4 position=matrices[0]*vertices[id].position;
 position=matrices[1]*position;position=matrices[2]*position;
 position.x+=position.w/viewport.x;position.y-=position.w/viewport.y;
 // 沿用既有 D3D9 → GL → Metal 的半像素與深度轉換。
 position.z=2*position.z-position.w;position.z=(position.z+position.w)*0.5;
 O out;out.secondary=vertices[id].secondary;out.position=position;out.color=clamp(vertices[id].color,0.0f,1.0f);out.texcoord=vertices[id].texcoord;out.eyeZ=0;out.point=1;return out;
}
