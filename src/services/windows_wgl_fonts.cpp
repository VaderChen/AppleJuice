// wglUseFontBitmapsA／W 與 wglUseFontOutlinesA／W：以 DC 目前選入的字型，為 first～first+count-1 的字元建立 listBase 起的顯示清單。
// 字形透過 gdi32 的 GetGlyphOutlineW（與客體呼叫同一份實作）取得；清單內容直接送入目前 context 的 OpenGL 後端編譯。
// - Bitmaps：GGO_BITMAP 的黑框（由上而下、DWORD 對齊）轉成由下而上的 glBitmap，xorig＝-原點 x、yorig＝高度-原點 y，前進量為字元格增量；
//   空白字元只前進。編譯期間暫用緊密解包設定並於結束後還原。
// - Outlines：GGO_NATIVE 輪廓依 deviation（em 單位的最大弦偏差，0 表示約一個設計單位）把二次曲線切成折線，座標以 em＝1 正規化；
//   WGL_FONT_POLYGONS 以 GLU tessellator（非零環繞、法線 +z）三角化正面，extrusion>0 時加上 z＝-extrusion 的背面（反向）與側面（法線朝外），
//   WGL_FONT_LINES 為各輪廓的線框（擠出時另加背面線框與連接線）；清單最後以 glTranslatef 前進字元格增量，lpgmf 取得 GLYPHMETRICSFLOAT。
// - ANSI 版本：引擎 ACP 為 UTF-8，0x80～0xFF 的單一位元組沒有對應字元，依 Latin-1 視為 U+0080～U+00FF。
// - 沒有目前 context 時回 FALSE（ERROR_INVALID_HANDLE）；取不到字形的字元仍建立空清單（點陣時只前進）。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "glu_tessellation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace x64arm {
namespace {
using Contour=std::vector<std::array<double,2>>;
std::int32_t i32(std::span<const std::uint8_t> b,std::size_t at){std::int32_t v;std::memcpy(&v,b.data()+at,4);return v;}
std::uint16_t u16(std::span<const std::uint8_t> b,std::size_t at){return static_cast<std::uint16_t>(b[at]|b[at+1]<<8);}
std::vector<std::uint8_t> guest_bytes(GuestMemory& memory,std::uint64_t address,std::uint64_t length){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{address},bytes);return bytes;}
// GGO_NATIVE：TTPOLYGONHEADER（cb、dwType、pfxStart）後接 TTPOLYCURVE（wType、cpfx、apfx）；TT_PRIM_LINE 1、TT_PRIM_QSPLINE 2、TT_PRIM_CSPLINE 3。
std::vector<Contour> outline(std::span<const std::uint8_t> bytes,double tolerance){
    std::vector<Contour> contours;
    const auto point=[&](std::size_t at){return std::array<double,2>{i32(bytes,at)/65536.0,i32(bytes,at+4)/65536.0};};
    for(std::size_t header=0;header+16<=bytes.size();){
        const auto size=static_cast<std::size_t>(static_cast<std::uint32_t>(i32(bytes,header)));if(size<16 || header+size>bytes.size())break;
        Contour contour{point(header+8)};auto current=contour.front();
        for(std::size_t at=header+16;at+4<=header+size;){
            const auto type=u16(bytes,at),count=u16(bytes,at+2);if(at+4+count*8ULL>header+size)break;
            std::vector<std::array<double,2>> p;for(unsigned i=0;i<count;++i)p.push_back(point(at+4+i*8ULL));
            if(type==1){for(const auto& q:p){contour.push_back(q);current=q;}}
            else if(type==2){
                for(std::size_t i=0;i+1<p.size();++i){
                    const auto control=p[i];const auto end=i+2==p.size()?p[i+1]:std::array<double,2>{(p[i][0]+p[i+1][0])/2,(p[i][1]+p[i+1][1])/2};
                    const double dx=current[0]-2*control[0]+end[0],dy=current[1]-2*control[1]+end[1];
                    const auto steps=std::clamp(static_cast<int>(std::ceil(std::sqrt(std::sqrt(dx*dx+dy*dy)/(8*tolerance)))),1,32);
                    for(int k=1;k<=steps;++k){const double t=double(k)/steps,u=1-t;contour.push_back({u*u*current[0]+2*u*t*control[0]+t*t*end[0],u*u*current[1]+2*u*t*control[1]+t*t*end[1]});}
                    current=end;
                }
            }else if(type==3){
                for(std::size_t i=0;i+2<p.size();i+=3){
                    const auto c1=p[i],c2=p[i+1],end=p[i+2];
                    const double d=std::max(std::hypot(current[0]-2*c1[0]+c2[0],current[1]-2*c1[1]+c2[1]),std::hypot(c1[0]-2*c2[0]+end[0],c1[1]-2*c2[1]+end[1]));
                    const auto steps=std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75*d/tolerance))),1,32);
                    for(int k=1;k<=steps;++k){const double t=double(k)/steps,u=1-t;contour.push_back({u*u*u*current[0]+3*u*u*t*c1[0]+3*u*t*t*c2[0]+t*t*t*end[0],u*u*u*current[1]+3*u*u*t*c1[1]+3*u*t*t*c2[1]+t*t*t*end[1]});}
                    current=end;
                }
            }
            at+=4+count*8ULL;
        }
        while(contour.size()>1 && contour.back()==contour.front())contour.pop_back();
        if(contour.size()>=2)contours.push_back(std::move(contour));
        header+=size;
    }
    return contours;
}
// 非零環繞：點是否落在字形填滿區域內。
bool filled(const std::vector<Contour>& contours,const std::array<double,2>& p){
    long winding=0;
    for(const auto& c:contours)for(std::size_t i=0;i<c.size();++i){
        const auto& a=c[i];const auto& b=c[(i+1)%c.size()];const double side=(b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0]);
        if(a[1]<=p[1]){if(b[1]>p[1] && side>0)++winding;}else if(b[1]<=p[1] && side<0)--winding;
    }
    return winding!=0;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_wgl_fonts(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool ansi=name.ends_with('A');
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    OpenGlContext* context=nullptr;
    if(const auto current=opengl_.current.find(threads_.current);current!=opengl_.current.end()){const auto found=opengl_.contexts.find(current->second);if(found!=opengl_.contexts.end() && found->second.native)context=&found->second;}
    if(!context){last_error_=6;return finish(0);}
    const bool outlines=name.starts_with("wglUseFontOutlines");const std::uint64_t dc=arg(0);const std::uint32_t first=d(1),count=d(2),base=d(3);
    const float deviation=outlines?std::bit_cast<float>(d(4)):0,extrusion=outlines?std::bit_cast<float>(d(5)):0;const auto format=outlines?d(6):0;const auto metrics_out=outlines?arg(7):0;
    if(outlines && (deviation<0 || extrusion<0 || format>1)){last_error_=87;return finish(0);}
    if(count>65536 || (count && static_cast<std::uint64_t>(base)+count>UINT32_MAX)){last_error_=87;return finish(0);}
    if(!count)return finish(1);
    auto& gl=*context->native;
    std::vector<std::uint64_t> blocks;struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto b:blocks)(void)self.free_heap(memory,b,state);}} release{*this,memory,state,blocks};
    const auto scratch=[&](std::uint64_t size)->std::uint64_t{const auto b=allocate_heap(memory,std::max<std::uint64_t>(size,1));if(!b)throw std::bad_alloc{};blocks.push_back(b);return b;};
    try{
        const auto metrics=scratch(20),matrix=scratch(16),text=scratch(64);
        memory.write(GuestAddress{matrix},std::array<std::uint8_t,16>{0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0}); // MAT2 單位矩陣
        double em=1;
        if(outlines){if(!nested_service(state,memory,"gdi32.dll","GetTextMetricsW",{dc,text})){last_error_=6;return finish(0);}const auto bytes=guest_bytes(memory,text,16);const auto height=i32(bytes,0),leading=i32(bytes,12);em=height-leading>0?height-leading:std::max(1,height);}
        const auto glyph=[&](std::uint32_t code,std::uint32_t mode,std::vector<std::uint8_t>& data)->std::optional<std::array<std::int32_t,6>>{
            const std::uint64_t character=ansi && code>255?UINT32_MAX:code;if(character>0xffff)return std::nullopt;
            const auto size=static_cast<std::uint32_t>(nested_service(state,memory,"gdi32.dll","GetGlyphOutlineW",{dc,character,mode,metrics,0,0,matrix}));if(size==UINT32_MAX)return std::nullopt;
            data.clear();
            if(mode && size){if(size>16U*1024U*1024U)return std::nullopt;const auto buffer=scratch(size);if(nested_service(state,memory,"gdi32.dll","GetGlyphOutlineW",{dc,character,mode,metrics,size,buffer,matrix})==UINT32_MAX)return std::nullopt;data=guest_bytes(memory,buffer,size);(void)free_heap(memory,buffer,state);blocks.pop_back();}
            const auto m=guest_bytes(memory,metrics,20);
            return std::array<std::int32_t,6>{i32(m,0),i32(m,4),i32(m,8),i32(m,12),static_cast<std::int16_t>(u16(m,16)),static_cast<std::int16_t>(u16(m,18))};
        };
        const auto op=[&](GlOperation operation,std::initializer_list<double> values){std::array<std::uint64_t,4> a{};unsigned i=0;for(const double v:values)a[i++]=std::bit_cast<std::uint32_t>(static_cast<float>(v));gl.execute(operation,std::span(a).first(i));};
        // 點陣：編譯期間暫用緊密解包設定，結束後依 context 的客戶端狀態還原。
        const auto unpack=[&](bool tight){for(unsigned i=0;i<6;++i){const auto pname=0xcf0U+i;const auto it=context->client.pixel_store.find(pname);const auto value=tight?(i==5?1:0):(it==context->client.pixel_store.end()?(i==5?4:0):it->second);gl.execute(GlOperation::PixelStorei,std::array<std::uint64_t,2>{pname,static_cast<std::uint32_t>(value)});}};
        if(!outlines)unpack(true);
        struct Restore {const decltype(unpack)& apply;bool active;~Restore(){if(active)try{apply(false);}catch(...){}}} restore{unpack,!outlines};
        std::vector<std::uint8_t> data;
        for(std::uint32_t i=0;i<count;++i){
            const auto list=base+i;gl.execute(GlOperation::NewList,std::array<std::uint64_t,2>{list,0x1300});
            if(!outlines){
                const auto m=glyph(first+i,1,data); // GGO_BITMAP
                if(m){
                    const auto width=static_cast<unsigned>(std::max(0,(*m)[0])),height=static_cast<unsigned>(std::max(0,(*m)[1]));const auto source=((width+31)/32)*4,target=(width+7)/8;
                    std::vector<std::uint8_t> bits;const bool ink=!data.empty() && width && height && data.size()>=static_cast<std::size_t>(source)*height;
                    if(ink){bits.resize(static_cast<std::size_t>(target)*height);for(unsigned y=0;y<height;++y)std::memcpy(bits.data()+static_cast<std::size_t>(y)*target,data.data()+static_cast<std::size_t>(height-1-y)*source,target);}
                    const auto f=[](double v){return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(static_cast<float>(v)));};
                    gl.execute(GlOperation::Bitmap,std::array<std::uint64_t,10>{ink?width:0U,ink?height:0U,f(-(*m)[2]),f(static_cast<double>(height)-(*m)[3]),f((*m)[4]),f((*m)[5]),ink?1U:0U,0,0,0},bits);
                }
            }else{
                const auto m=glyph(first+i,2,data); // GGO_NATIVE
                std::array<float,6> gm{};
                if(m){
                    gm={static_cast<float>((*m)[0]/em),static_cast<float>((*m)[1]/em),static_cast<float>((*m)[2]/em),static_cast<float>((*m)[3]/em),static_cast<float>((*m)[4]/em),static_cast<float>((*m)[5]/em)};
                    auto contours=outline(data,deviation>0?deviation*em:em/2048.0);for(auto& c:contours)for(auto& p:c){p[0]/=em;p[1]/=em;}
                    const double depth=extrusion;
                    if(format==0){ // WGL_FONT_LINES
                        for(const double z:depth>0?std::vector<double>{0,-depth}:std::vector<double>{0}){for(const auto& c:contours){gl.execute(GlOperation::Begin,std::array<std::uint64_t,1>{2});for(const auto& p:c)op(GlOperation::Vertex3f,{p[0],p[1],z});gl.execute(GlOperation::End);}}
                        if(depth>0){gl.execute(GlOperation::Begin,std::array<std::uint64_t,1>{1});for(const auto& c:contours)for(const auto& p:c){op(GlOperation::Vertex3f,{p[0],p[1],0});op(GlOperation::Vertex3f,{p[0],p[1],-depth});}gl.execute(GlOperation::End);}
                    }else if(!contours.empty()){
                        GluTessellator tess;tess.winding=100131;tess.normal={0,0,1};tess.state=1;
                        for(const auto& c:contours){auto& out=tess.contours.emplace_back();for(const auto& p:c)out.push_back({{p[0],p[1],0},0});}
                        if(const auto mesh=glu_triangulate(tess)){
                            const auto face=[&](double z,bool back){
                                op(GlOperation::Normal3f,{0,0,back?-1.0:1.0});gl.execute(GlOperation::Begin,std::array<std::uint64_t,1>{4});
                                for(const auto& t:mesh->triangles)for(std::size_t k=0;k<3;++k){const auto& p=mesh->positions[t[back?2-k:k]];op(GlOperation::Vertex3f,{p[0],p[1],z});}
                                gl.execute(GlOperation::End);
                            };
                            face(0,false);
                            if(depth>0){
                                face(-depth,true);
                                gl.execute(GlOperation::Begin,std::array<std::uint64_t,1>{7}); // GL_QUADS：側面法線指向填滿區域外側
                                for(const auto& c:contours)for(std::size_t k=0;k<c.size();++k){
                                    const auto& a=c[k];const auto& b=c[(k+1)%c.size()];const double dx=b[0]-a[0],dy=b[1]-a[1],length=std::hypot(dx,dy);if(length<=0)continue;
                                    std::array<double,2> normal{-dy/length,dx/length};const std::array<double,2> mid{(a[0]+b[0])/2+normal[0]*1e-4,(a[1]+b[1])/2+normal[1]*1e-4};
                                    if(filled(contours,mid))normal={-normal[0],-normal[1]};
                                    // 由外側看為逆時針：沿邊方向與法線決定頂點順序。
                                    const bool forward=(dx*normal[1]-dy*normal[0])>0;
                                    const std::array<std::array<double,3>,4> quad{{{a[0],a[1],0},{b[0],b[1],0},{b[0],b[1],-depth},{a[0],a[1],-depth}}};
                                    op(GlOperation::Normal3f,{normal[0],normal[1],0});
                                    for(std::size_t q=0;q<4;++q){const auto& v=quad[forward?q:3-q];op(GlOperation::Vertex3f,{v[0],v[1],v[2]});}
                                }
                                gl.execute(GlOperation::End);
                            }
                        }
                    }
                    op(GlOperation::Translatef,{gm[4],gm[5],0});
                }
                if(metrics_out)for(unsigned k=0;k<6;++k)memory.store(GuestAddress{metrics_out+i*24ULL+k*4},32,std::bit_cast<std::uint32_t>(gm[k]));
            }
            gl.execute(GlOperation::EndList);
        }
    }catch(const MemoryFault&){last_error_=998;return finish(0);}catch(const std::bad_alloc&){last_error_=8;return finish(0);}catch(const std::invalid_argument&){last_error_=87;return finish(0);}
    return finish(1);
}
}
