#include <metal_stdlib>
using namespace metal;
struct Params { int4 rect, source, clip; uint kind, color, fill, width; int hatch; uint opaque, background, blend, excludes; int2 brush_origin; uint stretch, bitmap_pattern, pattern_color, dst_dib; uint operation, transparent; float4 inverse_linear; float2 inverse_offset; uint transformed, rop2; };
float4 mix_rop2(float4 paint,float4 dest,uint mode){
 uint3 P=uint3(round(paint.rgb*255.0)),D=uint3(round(dest.rgb*255.0)),R=0;uint table=mode-1;
 for(uint bit=0;bit<4;++bit)if(table&(1u<<bit))R|=((bit&2)?P:~P)&((bit&1)?D:~D);
 return float4(float3(R&255u)/255.0,paint.a);
}
float4 color(uint c) {return float4(c&255,(c>>8)&255,(c>>16)&255,255)/255.0;}
bool inside(int2 p,int4 r){return p.x>=r.x && p.y>=r.y && p.x<r.z && p.y<r.w;}
bool pattern(int2 p,int h){int x=p.x&7,y=p.y&7;return h<0 || (h==0 && y==0) || (h==1 && x==0) || (h==2 && ((x+y)&7)==0) || (h==3 && x==y) || (h==4 && (x==0 || y==0)) || (h==5 && (x==y || ((x+y)&7)==0));}
kernel void raster(texture2d<float,access::read_write> dst [[texture(0)]],texture2d<float,access::read> src [[texture(1)]],texture2d<float,access::read> brush [[texture(2)]],constant Params& a [[buffer(0)]],constant int4* excluded [[buffer(1)]],constant int2& origin [[buffer(2)]],uint2 tid [[thread_position_in_grid]]) {
 int2 p=int2(tid)+origin;if(p.x<0 || p.y<0 || p.x>=int(dst.get_width()) || p.y>=int(dst.get_height()) || !inside(p,a.clip))return;
 for(uint i=0;i<a.excludes;++i)if(inside(p,excluded[i]))return;
 int2 device_point=p;float2 sample=float2(p)+0.5;if(a.transformed){float2 original=sample;sample=float2(original.x*a.inverse_linear.x+original.y*a.inverse_linear.z,original.x*a.inverse_linear.y+original.y*a.inverse_linear.w)+a.inverse_offset;p=int2(floor(sample));}
 float4 c=color(a.color);bool covered=false;
 if(a.kind==0) {covered=inside(p,a.rect);if(!pattern(device_point-a.brush_origin,a.hatch)){if(!a.opaque)return;c=color(a.background);}}
 else if(a.kind==1 || a.kind==2){
  if(!inside(p,a.rect))return;
  bool edge;
  if(a.kind==1)edge=p.x<a.rect.x+int(a.width) || p.y<a.rect.y+int(a.width) || p.x>=a.rect.z-int(a.width) || p.y>=a.rect.w-int(a.width);
  else {float2 center=float2(a.rect.xy+a.rect.zw)*0.5,r=float2(a.rect.zw-a.rect.xy)*0.5,q=sample-center;if(any(r<=0.0) || dot(q/r,q/r)>1.0)return;float2 inner=max(r-float(a.width),float2(0.01));edge=dot(q/inner,q/inner)>=1.0;}
  if(edge && a.width)covered=true;
  else if(a.fill!=0xffffffffu){covered=true;c=color(a.fill);if(!pattern(device_point-a.brush_origin,a.hatch)){if(!a.opaque)return;c=color(a.background);} }
  if(edge && a.width){if(covered){c=mix_rop2(c,dst.read(uint2(device_point)),a.rop2);c.a=a.dst_dib?0.0:1.0;dst.write(c,uint2(device_point));}return;}
 }
 else if(a.kind==3){if(!a.width)return;float2 d=float2(a.rect.zw-a.rect.xy),q=sample-0.5-float2(a.rect.xy);float l=dot(d,d);float t=l>0?dot(q,d)/l:0;if(t<0 || t>=1)return;float2 e=q-t*d;covered=dot(e,e)<=max(0.5,float(a.width)*0.5)*max(0.5,float(a.width)*0.5);}
 else if(a.kind==4 || a.kind==5){
  if(!inside(p,int4(min(a.rect.xy,a.rect.zw),max(a.rect.xy,a.rect.zw))))return;
  float2 t=(sample-float2(a.rect.xy))/float2(a.rect.zw-a.rect.xy);int2 q=int2(floor(float2(a.source.xy)+t*float2(a.source.zw-a.source.xy)));
  bool reads_source=a.kind==5 || ((a.operation^(a.operation>>2))&0x33u)!=0;
  if(reads_source && (any(q<0) || q.x>=int(src.get_width()) || q.y>=int(src.get_height())))return;
  c=reads_source?src.read(uint2(q)):float4(0);
  if(a.transparent!=0xffffffffu && all(uint3(round(c.rgb*255.0))==uint3(a.transparent&255,(a.transparent>>8)&255,(a.transparent>>16)&255)))return;
  if(a.kind==4 && reads_source && a.stretch!=3 && any(abs(a.source.zw-a.source.xy)>abs(a.rect.zw-a.rect.xy))){
   float2 scale=float2(a.source.zw-a.source.xy)/float2(a.rect.zw-a.rect.xy);
   float2 begin=float2(a.source.xy)+(sample-0.5-float2(a.rect.xy))*scale,end=begin+scale;
   float2 low=max(min(begin,end),float2(0)),high=min(max(begin,end),float2(src.get_width(),src.get_height()));
   float4 sum=0;float weight=0;uint4 bits=a.stretch==1?uint4(255):uint4(0);
   for(int y=int(floor(low.y));y<int(ceil(high.y));++y)for(int x=int(floor(low.x));x<int(ceil(high.x));++x){
    float4 sample=src.read(uint2(x,y));float cell=max(0.0,min(high.x,float(x+1))-max(low.x,float(x)))*max(0.0,min(high.y,float(y+1))-max(low.y,float(y)));
    if(a.stretch==4){sum+=sample*cell;weight+=cell;}
    else {uint4 value=uint4(round(sample*255.0));bits=a.stretch==1?(bits&value):(bits|value);}
   }
   if(a.stretch==4){if(weight>0)c=sum/weight;}else c=float4(bits)/255.0;
  }
  if(a.kind==4 && a.blend==0xffffffffu && a.operation!=0xccu){
   float4 paint=color(a.color);bool reads_pattern=((a.operation^(a.operation>>4))&15u)!=0;
   if(reads_pattern){
    if(a.bitmap_pattern){int2 size=int2(brush.get_width(),brush.get_height()),q=((device_point-a.brush_origin)%size+size)%size;paint=brush.read(uint2(q));if(a.bitmap_pattern==2)paint=color(paint.r>0.5?a.background:a.pattern_color);}
    else if(!pattern(device_point-a.brush_origin,a.hatch)){if(!a.opaque)return;paint=color(a.background);}
   }
   uint3 P=uint3(round(paint.rgb*255.0)),S=uint3(round(c.rgb*255.0)),D=uint3(round(dst.read(uint2(device_point)).rgb*255.0)),R=0;
   for(uint bit=0;bit<8;++bit)if(a.operation&(1u<<bit))R|=((bit&4)?P:~P)&((bit&2)?S:~S)&((bit&1)?D:~D);
   c=float4(float3(R&255u)/255.0,0);
  }
  if(a.kind==5)c.rgb=color(a.color).rgb*c.a;covered=true;
  if(a.blend!=0xffffffffu){float alpha=float((a.blend>>16)&255)/255.0;float sa=(a.blend&0x01000000u)?c.a:1.0;float4 old=dst.read(uint2(device_point));c.rgb=c.rgb*alpha+old.rgb*(1.0-alpha*sa);c.a=alpha*sa+old.a*(1.0-alpha*sa);}
 }
 if(covered){if(a.bitmap_pattern && a.kind!=4){int2 size=int2(src.get_width(),src.get_height()),q=((device_point-a.brush_origin)%size+size)%size;c=src.read(uint2(q));if(a.bitmap_pattern==2)c=color(c.r>0.5?a.background:a.pattern_color);}if(a.kind<=3)c=mix_rop2(c,dst.read(uint2(device_point)),a.rop2);c.a=a.dst_dib?(a.kind==4?c.a:0.0):1.0;dst.write(c,uint2(device_point));}
}
