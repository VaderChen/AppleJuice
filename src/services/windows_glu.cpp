// GLU 1.2（glu32）宿主端實作：直接驅動目前 context 的 OpenGL 後端（NativeOpenGlContext::execute），與客體 gl* 呼叫走同一條路徑，
// display list 編譯期間同樣被記錄。gluPerspective 屬 OpenGL 指令表（windows_opengl.cpp）。
// - 字串：gluErrorString（GL 與 GLU、tessellator、NURBS 錯誤碼；未知為 NULL）、gluErrorUnicodeStringEXT（UTF-16）、
//   gluGetString（GLU_VERSION "1.2.2.0"、GLU_EXTENSIONS "GL_EXT_bgra"，其他為 NULL）、gluCheckExtension（以空白分隔的完整名稱比對）。
// - 矩陣：gluOrtho2D（glOrtho，近遠 -1／1）、gluLookAt、gluPickMatrix（寬或高 ≤0 不變更）乘到目前矩陣；
//   gluProject／gluUnProject 依傳入的欄優先雙精度矩陣與視埠計算，w 為 0 或矩陣不可逆時回 GL_FALSE。
// - 影像：gluScaleImage（輸入依 GL_UNPACK_*、輸出依 GL_PACK_* 像素儲存設定，面積加權平均，輸出列間留白保留原內容）；
//   gluBuild2DMipmaps／gluBuild1DMipmaps（先縮到不大於原尺寸的 2 的冪且不超過 GL_MAX_TEXTURE_SIZE，再逐層 2×2（1D 為 2:1）平均到 1×1，上傳時暫用緊密排列並還原像素儲存設定）。
// - NURBS：windows_glu_nurbs.cpp（曲線／曲面／修剪，渲染與 tessellator 回呼兩種模式）。
//   格式：索引、深度、單一色版、RGB／RGBA、BGR／BGRA、亮度（含 alpha）；型別：8／16／32 位元整數、浮點、打包型別與 GL_BITMAP（只限索引格式）。
//   錯誤回 GLU_INVALID_VALUE／GLU_INVALID_ENUM／GLU_INVALID_OPERATION（打包型別與色版數不符、沒有 context）。
// - 二次曲面：gluNewQuadric／gluDeleteQuadric（客體堆積物件）、gluQuadricDrawStyle／Normals／Orientation／Texture／Callback（GLU_ERROR）、
//   gluSphere、gluCylinder、gluDisk、gluPartialDisk：依 GLU 規格的座標、法線與貼圖座標（方位角從 +y 軸起往 +x），
//   點／線／輪廓／填滿四種樣式；GLU_FLAT 為每面一個法線、GLU_INSIDE 反轉法線與正面方向。無效參數經錯誤回呼回報。
// tessellator 實作見 windows_glu_tess.cpp。gluBuild1DMipmaps 需要 opengl32 的 1D 貼圖，尚未提供；NURBS 另列後續家族。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace x64arm {
namespace {
constexpr std::uint32_t glu_invalid_enum=100900,glu_invalid_value=100901,glu_out_of_memory=100902,glu_invalid_operation=100904;
constexpr std::uint32_t glu_smooth=100000,glu_flat=100001,glu_none=100002,glu_point=100010,glu_fill=100012,glu_silhouette=100013,glu_outside=100020,glu_inside=100021;
constexpr double pi=3.14159265358979323846;
using Matrix=std::array<double,16>; // 欄優先
Matrix multiply(const Matrix& a,const Matrix& b){Matrix r{};for(std::size_t c=0;c<4;++c)for(std::size_t row=0;row<4;++row){double s=0;for(std::size_t k=0;k<4;++k)s+=a[k*4+row]*b[c*4+k];r[c*4+row]=s;}return r;}
std::optional<Matrix> invert(const Matrix& m){
    std::array<std::array<double,8>,4> t{};
    for(std::size_t r=0;r<4;++r)for(std::size_t c=0;c<4;++c){t[r][c]=m[c*4+r];t[r][c+4]=r==c?1:0;}
    for(std::size_t c=0;c<4;++c){
        std::size_t p=c;for(std::size_t r=c+1;r<4;++r)if(std::fabs(t[r][c])>std::fabs(t[p][c]))p=r;
        if(t[p][c]==0)return std::nullopt;std::swap(t[p],t[c]);const auto pivot=t[c][c];for(auto& v:t[c])v/=pivot;
        for(std::size_t r=0;r<4;++r)if(r!=c){const auto f=t[r][c];if(f!=0)for(std::size_t k=0;k<8;++k)t[r][k]-=f*t[c][k];}
    }
    Matrix r{};for(std::size_t row=0;row<4;++row)for(std::size_t c=0;c<4;++c)r[c*4+row]=t[row][c+4];return r;
}
std::array<double,4> transform(const Matrix& m,const std::array<double,4>& v){std::array<double,4> r{};for(std::size_t row=0;row<4;++row)r[row]=m[row]*v[0]+m[4+row]*v[1]+m[8+row]*v[2]+m[12+row]*v[3];return r;}
// 自行撰寫的錯誤說明；名稱對應 GL／GLU 規格的錯誤碼。
std::optional<std::string_view> error_text(std::uint32_t code){
    switch(code){
    case 0:return "no error";case 0x500:return "invalid enumerant";case 0x501:return "invalid value";case 0x502:return "invalid operation";
    case 0x503:return "stack overflow";case 0x504:return "stack underflow";case 0x505:return "out of memory";case 0x506:return "invalid framebuffer operation";
    case glu_invalid_enum:return "invalid enumerant";case glu_invalid_value:return "invalid value";case glu_out_of_memory:return "out of memory";
    case 100903:return "incompatible gl version";case glu_invalid_operation:return "invalid operation";
    case 100151:return "gluTessBeginPolygon is missing";case 100152:return "gluTessBeginContour is missing";case 100153:return "gluTessEndPolygon is missing";
    case 100154:return "gluTessEndContour is missing";case 100155:return "tessellation coordinate is too large";case 100156:return "a combine callback is required";
    default:break;
    }
    static constexpr std::array<std::string_view,37> nurbs{
        "unsupported spline order","not enough knots","the valid knot range is empty","the knot sequence decreases","a knot multiplicity exceeds the spline order",
        "gluEndCurve without gluBeginCurve","gluBeginCurve without gluEndCurve","missing or extra geometric data","piecewise linear curves cannot be drawn",
        "missing or extra domain data","missing or extra domain data","gluEndTrim must come before gluEndSurface","gluBeginSurface must come before gluEndSurface",
        "a curve of the wrong type was used as a trim curve","gluBeginSurface must come before gluBeginTrim","gluEndTrim without gluBeginTrim","gluBeginTrim without gluEndTrim",
        "invalid or missing trim curve","gluBeginTrim must come before gluPwlCurve","a piecewise linear curve is referenced twice","piecewise linear and NURBS curves are mixed",
        "trim data used incorrectly","a NURBS curve is referenced twice","NURBS and piecewise linear curves are mixed","a NURBS surface is referenced twice","invalid property",
        "gluEndSurface without gluBeginSurface","trim curves intersect or are misoriented","trim curves intersect","unused error code","trim curves are not connected",
        "unknown knot error","negative vertex count","negative byte stride","unknown type descriptor","null control point reference","duplicate point on a piecewise linear curve"};
    if(code>=100251 && code<100251+nurbs.size())return nurbs[code-100251];
    return std::nullopt;
}
// ---- 影像 ----
struct PixelStore {std::int32_t swap{},lsb{},row{},skip_rows{},skip_pixels{},alignment{4};};
PixelStore pixel_store(const OpenGlContext* c,bool pack){
    PixelStore s;if(!c)return s;const auto base=pack?0xd00U:0xcf0U;
    const auto value=[&](unsigned i){const auto it=c->client.pixel_store.find(base+i);return it==c->client.pixel_store.end()?(i==5?4:0):it->second;};
    s.swap=value(0);s.lsb=value(1);s.row=value(2);s.skip_rows=value(3);s.skip_pixels=value(4);s.alignment=value(5);return s;
}
unsigned channels(std::uint32_t format){
    switch(format){case 0x1900:case 0x1901:case 0x1902:case 0x1903:case 0x1904:case 0x1905:case 0x1906:case 0x1909:return 1;case 0x190a:return 2;case 0x1907:case 0x80e0:return 3;case 0x1908:case 0x80e1:return 4;default:return 0;}
}
bool index_format(std::uint32_t format){return format==0x1900 || format==0x1901;}
unsigned component_bytes(std::uint32_t type){switch(type){case 0x1400:case 0x1401:return 1;case 0x1402:case 0x1403:return 2;case 0x1404:case 0x1405:case 0x1406:return 4;default:return 0;}}
// 打包型別：bits 依色版順序；reversed（_REV）表示第一個色版在最低位元。
struct Packed {unsigned bytes,components;std::array<unsigned,4> bits;bool reversed;};
std::optional<Packed> packed(std::uint32_t type){
    switch(type){
    case 0x8032:return Packed{1,3,{3,3,2,0},false};case 0x8362:return Packed{1,3,{3,3,2,0},true};
    case 0x8363:return Packed{2,3,{5,6,5,0},false};case 0x8364:return Packed{2,3,{5,6,5,0},true};
    case 0x8033:return Packed{2,4,{4,4,4,4},false};case 0x8365:return Packed{2,4,{4,4,4,4},true};
    case 0x8034:return Packed{2,4,{5,5,5,1},false};case 0x8366:return Packed{2,4,{5,5,5,1},true};
    case 0x8035:return Packed{4,4,{8,8,8,8},false};case 0x8367:return Packed{4,4,{8,8,8,8},true};
    case 0x8036:return Packed{4,4,{10,10,10,2},false};case 0x8368:return Packed{4,4,{10,10,10,2},true};
    default:return std::nullopt;}
}
// 0 或 GLU 錯誤碼。
std::uint32_t validate(std::uint32_t format,std::uint32_t type){
    const auto n=channels(format);if(!n)return glu_invalid_enum;
    if(type==0x1a00)return index_format(format)?0:glu_invalid_enum;
    if(component_bytes(type))return 0;
    const auto p=packed(type);if(!p)return glu_invalid_enum;
    if(p->components!=n || (n==3 && format!=0x1907) || (n==4 && format!=0x1908 && format!=0x80e1))return glu_invalid_operation;
    return 0;
}
struct Layout {std::uint64_t stride{},offset{},total{};unsigned group{};bool bitmap{};};
std::optional<Layout> layout(std::int32_t width,std::int32_t height,std::uint32_t format,std::uint32_t type,const PixelStore& s){
    Layout l;l.bitmap=type==0x1a00;const auto p=packed(type);l.group=l.bitmap?0:p?p->bytes:component_bytes(type)*channels(format);
    const auto row=static_cast<std::uint64_t>(s.row>0?s.row:width),align=static_cast<std::uint64_t>(s.alignment>0?s.alignment:1);
    const auto raw=l.bitmap?(row+7)/8:row*l.group;l.stride=(raw+align-1)/align*align;
    const auto skip_rows=static_cast<std::uint64_t>(std::max(0,s.skip_rows)),skip_pixels=static_cast<std::uint64_t>(std::max(0,s.skip_pixels));
    l.offset=skip_rows*l.stride+(l.bitmap?0:skip_pixels*l.group);
    const auto last=l.bitmap?(skip_pixels+static_cast<std::uint64_t>(width)+7)/8:static_cast<std::uint64_t>(width)*l.group;
    l.total=l.offset+(static_cast<std::uint64_t>(height)-1)*l.stride+last;
    if(l.total>256U*1024U*1024U)return std::nullopt;
    return l;
}
std::uint64_t read_unit(const std::uint8_t* at,unsigned bytes,bool swap){std::uint64_t v=0;for(unsigned i=0;i<bytes;++i)v|=std::uint64_t(at[swap?bytes-1-i:i])<<(8*i);return v;}
void write_unit(std::uint8_t* at,unsigned bytes,bool swap,std::uint64_t v){for(unsigned i=0;i<bytes;++i)at[swap?bytes-1-i:i]=static_cast<std::uint8_t>(v>>(8*i));}
double component_value(std::uint32_t type,std::uint64_t raw,bool index){
    switch(type){
    case 0x1401:return index?double(raw):double(raw)/255.0;
    case 0x1400:{const auto v=double(static_cast<std::int8_t>(raw));return index?v:std::max(v/127.0,-1.0);}
    case 0x1403:return index?double(raw):double(raw)/65535.0;
    case 0x1402:{const auto v=double(static_cast<std::int16_t>(raw));return index?v:std::max(v/32767.0,-1.0);}
    case 0x1405:return index?double(raw):double(raw)/4294967295.0;
    case 0x1404:{const auto v=double(static_cast<std::int32_t>(raw));return index?v:std::max(v/2147483647.0,-1.0);}
    default:return double(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
    }
}
std::uint64_t component_raw(std::uint32_t type,double v,bool index){
    const auto unsigned_value=[&](double maximum){return static_cast<std::uint64_t>(std::llround(index?std::clamp(v,0.0,maximum):std::clamp(v,0.0,1.0)*maximum));};
    const auto signed_value=[&](double maximum,unsigned bits){const auto r=std::llround(index?std::clamp(v,-maximum-1,maximum):std::clamp(v,-1.0,1.0)*maximum);return static_cast<std::uint64_t>(r)&((bits==64?~0ULL:(1ULL<<bits)-1));};
    switch(type){
    case 0x1401:return unsigned_value(255);case 0x1400:return signed_value(127,8);
    case 0x1403:return unsigned_value(65535);case 0x1402:return signed_value(32767,16);
    case 0x1405:return unsigned_value(4294967295.0);case 0x1404:return signed_value(2147483647.0,32);
    default:return std::bit_cast<std::uint32_t>(static_cast<float>(v));
    }
}
std::vector<double> decode(std::span<const std::uint8_t> bytes,std::int32_t width,std::int32_t height,std::uint32_t format,std::uint32_t type,const PixelStore& s,const Layout& l){
    const auto n=channels(format);const bool index=index_format(format);const auto p=packed(type);const auto unit=component_bytes(type);
    std::vector<double> out(static_cast<std::size_t>(width)*static_cast<std::size_t>(height)*n);
    for(std::int32_t y=0;y<height;++y){
        const auto row=l.offset+static_cast<std::uint64_t>(y)*l.stride;
        for(std::int32_t x=0;x<width;++x){
            auto* target=out.data()+(static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x))*n;
            if(l.bitmap){const auto bit=static_cast<std::uint64_t>(std::max(0,s.skip_pixels))+static_cast<std::uint64_t>(x);const auto byte=bytes[row+bit/8];target[0]=(byte>>(s.lsb?bit%8:7-bit%8))&1;continue;}
            const auto* at=bytes.data()+row+static_cast<std::uint64_t>(x)*l.group;
            if(p){
                const auto value=read_unit(at,p->bytes,s.swap && p->bytes>1);unsigned shift=p->reversed?0:p->bytes*8;
                for(unsigned c=0;c<n;++c){const auto bits=p->bits[c];if(!p->reversed)shift-=bits;const auto mask=(1ULL<<bits)-1;target[c]=double((value>>shift)&mask)/double(mask);if(p->reversed)shift+=bits;}
            }else for(unsigned c=0;c<n;++c)target[c]=component_value(type,read_unit(at+c*unit,unit,s.swap && unit>1),index);
        }
    }
    return out;
}
// 寫入 bytes 中屬於像素的部分；列間留白與 skip 區域保留呼叫端原內容。
void encode(std::span<std::uint8_t> bytes,const std::vector<double>& pixels,std::int32_t width,std::int32_t height,std::uint32_t format,std::uint32_t type,const PixelStore& s,const Layout& l){
    const auto n=channels(format);const bool index=index_format(format);const auto p=packed(type);const auto unit=component_bytes(type);
    for(std::int32_t y=0;y<height;++y){
        const auto row=l.offset+static_cast<std::uint64_t>(y)*l.stride;
        for(std::int32_t x=0;x<width;++x){
            const auto* source=pixels.data()+(static_cast<std::size_t>(y)*static_cast<std::size_t>(width)+static_cast<std::size_t>(x))*n;
            if(l.bitmap){
                const auto bit=static_cast<std::uint64_t>(std::max(0,s.skip_pixels))+static_cast<std::uint64_t>(x);auto& byte=bytes[row+bit/8];const auto mask=static_cast<std::uint8_t>(1U<<(s.lsb?bit%8:7-bit%8));
                if(std::llround(source[0])&1)byte|=mask;else byte&=static_cast<std::uint8_t>(~mask);continue;
            }
            auto* at=bytes.data()+row+static_cast<std::uint64_t>(x)*l.group;
            if(p){
                std::uint64_t value=0;unsigned shift=p->reversed?0:p->bytes*8;
                for(unsigned c=0;c<n;++c){const auto bits=p->bits[c];if(!p->reversed)shift-=bits;const auto mask=(1ULL<<bits)-1;value|=(static_cast<std::uint64_t>(std::llround(std::clamp(source[c],0.0,1.0)*double(mask)))&mask)<<shift;if(p->reversed)shift+=bits;}
                write_unit(at,p->bytes,s.swap && p->bytes>1,value);
            }else for(unsigned c=0;c<n;++c)write_unit(at+c*unit,unit,s.swap && unit>1,component_raw(type,source[c],index));
        }
    }
}
// 面積加權平均：每個輸出像素取其覆蓋的來源範圍（含部分覆蓋）。
std::vector<double> scale(const std::vector<double>& in,std::int32_t wi,std::int32_t hi,unsigned n,std::int32_t wo,std::int32_t ho){
    std::vector<double> out(static_cast<std::size_t>(wo)*static_cast<std::size_t>(ho)*n);const double sx=double(wi)/wo,sy=double(hi)/ho;
    for(std::int32_t y=0;y<ho;++y){
        const double y0=y*sy,y1=(y+1)*sy;
        for(std::int32_t x=0;x<wo;++x){
            const double x0=x*sx,x1=(x+1)*sx;std::array<double,4> sum{};double area=0;
            for(auto iy=static_cast<std::int32_t>(std::floor(y0));iy<hi && iy<y1;++iy){
                const double wy=std::min(y1,iy+1.0)-std::max(y0,double(iy));if(wy<=0)continue;
                for(auto ix=static_cast<std::int32_t>(std::floor(x0));ix<wi && ix<x1;++ix){
                    const double wx=std::min(x1,ix+1.0)-std::max(x0,double(ix));if(wx<=0)continue;const double weight=wx*wy;area+=weight;
                    const auto* source=in.data()+(static_cast<std::size_t>(iy)*static_cast<std::size_t>(wi)+static_cast<std::size_t>(ix))*n;for(unsigned c=0;c<n;++c)sum[c]+=source[c]*weight;
                }
            }
            auto* target=out.data()+(static_cast<std::size_t>(y)*static_cast<std::size_t>(wo)+static_cast<std::size_t>(x))*n;for(unsigned c=0;c<n;++c)target[c]=area>0?sum[c]/area:0;
        }
    }
    return out;
}
// ---- 二次曲面 ----（GLU_LINE 100011 為線框；不是點、填滿、輪廓時都依線框處理）
struct QuadricVertex {std::array<double,3> p,n;std::array<double,2> t;};
struct Draw {
    NativeOpenGlContext& gl;const GluQuadric& q;
    void op(GlOperation operation,std::initializer_list<double> values){std::array<std::uint64_t,4> a{};unsigned i=0;for(const double v:values)a[i++]=std::bit_cast<std::uint32_t>(static_cast<float>(v));gl.execute(operation,std::span(a).first(i));}
    void begin(std::uint32_t mode){gl.execute(GlOperation::Begin,std::array<std::uint64_t,1>{mode});}
    void end(){gl.execute(GlOperation::End);}
    void vertex(const QuadricVertex& v,const std::array<double,3>* normal=nullptr){
        if(q.normals!=glu_none){const auto& n=normal?*normal:v.n;op(GlOperation::Normal3f,{n[0],n[1],n[2]});}
        if(q.texture)op(GlOperation::TexCoord2f,{v.t[0],v.t[1]});
        op(GlOperation::Vertex3f,{v.p[0],v.p[1],v.p[2]});
    }
};
std::array<double,3> sub(const std::array<double,3>& a,const std::array<double,3>& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
std::array<double,3> cross(const std::array<double,3>& a,const std::array<double,3>& b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double dot(const std::array<double,3>& a,const std::array<double,3>& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
std::array<double,3> normalize(std::array<double,3> v,const std::array<double,3>& fallback){const auto length=std::sqrt(dot(v,v));if(length<=0)return fallback;for(auto& c:v)c/=length;return v;}
// 網格 grid[row*(columns+1)+column]；每格依「法線方向為正面」決定頂點順序（GLU_INSIDE 時法線已反轉）。
// lines 為 GLU_LINE／GLU_SILHOUETTE 要畫的列與欄。
void emit(Draw& draw,const std::vector<QuadricVertex>& grid,int columns,int rows,const std::vector<int>& line_rows,const std::vector<int>& line_columns,bool close_rows){
    const auto at=[&](int column,int row)->const QuadricVertex&{return grid[static_cast<std::size_t>(row)*static_cast<std::size_t>(columns+1)+static_cast<std::size_t>(column)];};
    if(draw.q.style==glu_fill){
        draw.begin(7); // GL_QUADS
        for(int row=0;row<rows;++row)for(int column=0;column<columns;++column){
            std::array<const QuadricVertex*,4> corner{&at(column,row),&at(column+1,row),&at(column+1,row+1),&at(column,row+1)};
            std::array<double,3> wanted{};for(const auto* v:corner)for(std::size_t k=0;k<3;++k)wanted[k]+=v->n[k];
            const auto geometric=cross(sub(corner[2]->p,corner[0]->p),sub(corner[3]->p,corner[1]->p));
            if(dot(geometric,wanted)<0)std::swap(corner[1],corner[3]);
            const auto facet=normalize(dot(geometric,wanted)<0?std::array<double,3>{-geometric[0],-geometric[1],-geometric[2]}:geometric,normalize(wanted,{0,0,1}));
            for(const auto* v:corner)draw.vertex(*v,draw.q.normals==glu_flat?&facet:nullptr);
        }
        draw.end();return;
    }
    if(draw.q.style==glu_point){draw.begin(0);for(int row=0;row<=rows;++row)for(int column=0;column<=columns;++column)draw.vertex(at(column,row));draw.end();return;}
    for(const int row:line_rows){draw.begin(close_rows?2:3);for(int column=0;column<(close_rows?columns:columns+1);++column)draw.vertex(at(column,row));draw.end();} // GL_LINE_LOOP／GL_LINE_STRIP
    for(const int column:line_columns){draw.begin(3);for(int row=0;row<=rows;++row)draw.vertex(at(column,row));draw.end();}
}
std::vector<std::uint8_t> guest_bytes(GuestMemory& memory,std::uint64_t address,std::uint64_t length){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{address},bytes);return bytes;}
std::vector<int> range(int count){std::vector<int> r(static_cast<std::size_t>(count));for(int i=0;i<count;++i)r[static_cast<std::size_t>(i)]=i;return r;}
}
std::span<const WindowsServiceDescriptor> glu_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"glu32.dll","gluErrorString",1},{"glu32.dll","gluErrorUnicodeStringEXT",1},{"glu32.dll","gluGetString",1},{"glu32.dll","gluCheckExtension",2},
        {"glu32.dll","gluOrtho2D",4,0xf},{"glu32.dll","gluLookAt",9,0x1ff},{"glu32.dll","gluPickMatrix",5,0xf},
        {"glu32.dll","gluProject",9,0x7},{"glu32.dll","gluUnProject",9,0x7},
        {"glu32.dll","gluScaleImage",9,0,0,0xef},{"glu32.dll","gluBuild2DMipmaps",7,0,0,0x3f},{"glu32.dll","gluBuild1DMipmaps",6,0,0,0x1f},
        {"glu32.dll","gluNewQuadric",0},{"glu32.dll","gluDeleteQuadric",1},{"glu32.dll","gluQuadricCallback",3,0,0,0x2},
        {"glu32.dll","gluQuadricDrawStyle",2,0,0,0x2},{"glu32.dll","gluQuadricNormals",2,0,0,0x2},{"glu32.dll","gluQuadricOrientation",2,0,0,0x2},{"glu32.dll","gluQuadricTexture",2,0,0,0x2},
        {"glu32.dll","gluSphere",4,0x2,0,0xc},{"glu32.dll","gluCylinder",6,0xe,0,0x30},{"glu32.dll","gluDisk",5,0x6,0,0x18},{"glu32.dll","gluPartialDisk",7,0x66,0,0x18},
        // tessellator（windows_glu_tess.cpp）
        {"glu32.dll","gluNewTess",0},{"glu32.dll","gluDeleteTess",1},{"glu32.dll","gluTessCallback",3,0,0,0x2},{"glu32.dll","gluTessProperty",3,0x4,0,0x2},
        {"glu32.dll","gluGetTessProperty",3,0,0,0x2},{"glu32.dll","gluTessNormal",4,0xe},{"glu32.dll","gluTessBeginPolygon",2},{"glu32.dll","gluTessBeginContour",1},
        {"glu32.dll","gluTessVertex",3},{"glu32.dll","gluTessEndContour",1},{"glu32.dll","gluTessEndPolygon",1},
        {"glu32.dll","gluBeginPolygon",1},{"glu32.dll","gluNextContour",2,0,0,0x2},{"glu32.dll","gluEndPolygon",1},
        // NURBS（windows_glu_nurbs.cpp）；gluNurbsProperty 的第三個參數是 float，x64 取自 XMM2、x86 為 4 位元組堆疊值。
        {"glu32.dll","gluNewNurbsRenderer",0},{"glu32.dll","gluDeleteNurbsRenderer",1},{"glu32.dll","gluNurbsProperty",3,0,0,0x2},{"glu32.dll","gluGetNurbsProperty",3,0,0,0x2},
        {"glu32.dll","gluLoadSamplingMatrices",4},{"glu32.dll","gluNurbsCallback",3,0,0,0x2},{"glu32.dll","gluBeginCurve",1},{"glu32.dll","gluEndCurve",1},
        {"glu32.dll","gluNurbsCurve",7,0,0,0x6a},{"glu32.dll","gluBeginSurface",1},{"glu32.dll","gluEndSurface",1},{"glu32.dll","gluNurbsSurface",11,0,0,0x76a},
        {"glu32.dll","gluBeginTrim",1},{"glu32.dll","gluEndTrim",1},{"glu32.dll","gluPwlCurve",5,0,0,0x1a}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_glu(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.find("Tess")!=std::string_view::npos || service.symbol=="gluBeginPolygon" || service.symbol=="gluNextContour" || service.symbol=="gluEndPolygon")return invoke_glu_tess(state,memory,service);
    if(service.symbol.find("Nurbs")!=std::string_view::npos || service.symbol.ends_with("Curve") || service.symbol.ends_with("Surface") || service.symbol.ends_with("Trim") || service.symbol=="gluLoadSamplingMatrices")return invoke_glu_nurbs(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};const auto n=[&](unsigned i){return std::bit_cast<std::int32_t>(d(i));};
    // 雙精度參數：x64 前四個在 XMM，其餘與 x86 都在堆疊。
    const auto real=[&](unsigned i)->double{
        if(memory.execution_mode()==ExecutionMode::X86 || i>=4)return std::bit_cast<double>(arg(i));
        std::uint64_t value=0;for(unsigned k=0;k<8;++k)value|=std::uint64_t(state.xmm[i][k])<<(k*8);return std::bit_cast<double>(value);
    };
    OpenGlContext* context=nullptr;
    if(const auto current=opengl_.current.find(threads_.current);current!=opengl_.current.end()){const auto found=opengl_.contexts.find(current->second);if(found!=opengl_.contexts.end() && found->second.native){context=&found->second;++context->calls;}}
    const auto text=[&](std::uint64_t key,std::string_view value,bool wide){
        auto& slot=opengl_.glu_strings[key<<1|(wide?1U:0U)];
        if(!slot){std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(utf8_to_utf16(value));else{bytes.assign(value.begin(),value.end());bytes.push_back(0);}slot=runtime_allocate(memory,bytes);}
        return slot;
    };
    const auto multiply_current=[&](const Matrix& m){
        if(!context)return;std::array<float,16> values{};for(unsigned i=0;i<16;++i)values[i]=static_cast<float>(m[i]);
        context->native->execute(GlOperation::MultMatrixf,{},std::span(reinterpret_cast<const std::uint8_t*>(values.data()),sizeof(values)));
    };
    const auto read_matrix=[&](std::uint64_t at){Matrix m{};for(unsigned i=0;i<16;++i)m[i]=std::bit_cast<double>(memory.load(GuestAddress{at+i*8ULL},64));return m;};
    const auto read_viewport=[&](std::uint64_t at){std::array<double,4> v{};for(unsigned i=0;i<4;++i)v[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4ULL},32)));return v;};
    // GLU_ERROR 回呼（stdcall，參數為錯誤碼）；沒有回呼時只結束呼叫。
    const auto report=[&](const GluQuadric& quadric,std::uint32_t code)->std::optional<std::uint32_t>{
        const auto callback=quadric.callback;if(!callback)return finish(0);
        return nested_continuation(state,memory,"GLU 錯誤回呼",[service,callback,code](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            (void)co_await service_callback::call_value(callback,{code});const WindowsCall call(s,m,service);call.return_to(s,0);co_return std::optional<std::uint32_t>{};});
    };
    if(name=="gluErrorString" || name=="gluErrorUnicodeStringEXT"){const auto value=error_text(d(0));return finish(value?text(d(0),*value,name=="gluErrorUnicodeStringEXT"):0);}
    if(name=="gluGetString"){
        if(d(0)==100800)return finish(text(UINT64_C(0x100000000)|100800,"1.2.2.0",false));
        if(d(0)==100801)return finish(text(UINT64_C(0x100000000)|100801,"GL_EXT_bgra",false));
        return finish(0);
    }
    if(name=="gluCheckExtension"){
        if(!arg(0) || !arg(1))return finish(0);const auto wanted=read_ansi(memory,arg(0),4096),list=read_ansi(memory,arg(1),65536);if(wanted.empty())return finish(0);
        for(std::size_t at=0;at<list.size();){const auto end=std::min(list.find(' ',at),list.size());if(list.compare(at,end-at,wanted)==0 && end-at==wanted.size())return finish(1);at=end+1;}
        return finish(0);
    }
    if(name=="gluOrtho2D"){
        if(context)context->native->execute(GlOperation::Ortho,std::array<std::uint64_t,6>{std::bit_cast<std::uint64_t>(real(0)),std::bit_cast<std::uint64_t>(real(1)),std::bit_cast<std::uint64_t>(real(2)),std::bit_cast<std::uint64_t>(real(3)),std::bit_cast<std::uint64_t>(-1.0),std::bit_cast<std::uint64_t>(1.0)});
        return finish(0);
    }
    if(name=="gluLookAt"){
        const std::array<double,3> eye{real(0),real(1),real(2)},center{real(3),real(4),real(5)},up{real(6),real(7),real(8)};
        const auto forward=normalize(sub(center,eye),{0,0,-1});const auto side=normalize(cross(forward,up),{1,0,0});const auto upward=cross(side,forward);
        Matrix m{side[0],upward[0],-forward[0],0,side[1],upward[1],-forward[1],0,side[2],upward[2],-forward[2],0,0,0,0,1};
        Matrix t{1,0,0,0,0,1,0,0,0,0,1,0,-eye[0],-eye[1],-eye[2],1};
        multiply_current(multiply(m,t));return finish(0);
    }
    try{
        if(name=="gluPickMatrix"){
            const double x=real(0),y=real(1),width=real(2),height=real(3);if(width<=0 || height<=0 || !arg(4))return finish(0);
            const auto v=read_viewport(arg(4));
            Matrix m{v[2]/width,0,0,0,0,v[3]/height,0,0,0,0,1,0,(v[2]-2*(x-v[0]))/width,(v[3]-2*(y-v[1]))/height,0,1};
            multiply_current(m);return finish(0);
        }
        if(name=="gluProject" || name=="gluUnProject"){
            if(!arg(3) || !arg(4) || !arg(5) || !arg(6) || !arg(7) || !arg(8))return finish(0);
            const auto model=read_matrix(arg(3)),projection=read_matrix(arg(4));const auto view=read_viewport(arg(5));const auto combined=multiply(projection,model);
            std::array<double,4> result{};
            if(name=="gluProject"){
                auto v=transform(combined,{real(0),real(1),real(2),1});if(v[3]==0)return finish(0);for(std::size_t i=0;i<3;++i)v[i]/=v[3];
                result={view[0]+view[2]*(v[0]+1)/2,view[1]+view[3]*(v[1]+1)/2,(v[2]+1)/2,1};
            }else{
                const auto inverse=invert(combined);if(!inverse || view[2]==0 || view[3]==0)return finish(0);
                result=transform(*inverse,{(real(0)-view[0])*2/view[2]-1,(real(1)-view[1])*2/view[3]-1,real(2)*2-1,1});if(result[3]==0)return finish(0);
                for(std::size_t i=0;i<3;++i)result[i]/=result[3];
            }
            for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(6+i)},64,std::bit_cast<std::uint64_t>(result[i]));
            return finish(1);
        }
        if(name=="gluScaleImage"){
            const auto format=d(0),type_in=d(3),type_out=d(7);const auto wi=n(1),hi=n(2),wo=n(5),ho=n(6);
            if(wi<0 || hi<0 || wo<0 || ho<0)return finish(glu_invalid_value);
            if(const auto error=validate(format,type_in))return finish(error);if(const auto error=validate(format,type_out))return finish(error);
            if(!wi || !hi || !wo || !ho)return finish(0);
            const auto unpack=pixel_store(context,false),pack=pixel_store(context,true);
            const auto source=layout(wi,hi,format,type_in,unpack),target=layout(wo,ho,format,type_out,pack);if(!source || !target)return finish(glu_out_of_memory);
            const auto input=guest_bytes(memory,arg(4),source->total);auto pixels=decode(input,wi,hi,format,type_in,unpack,*source);
            if(wi!=wo || hi!=ho)pixels=scale(pixels,wi,hi,channels(format),wo,ho);
            auto output=guest_bytes(memory,arg(8),target->total);encode(output,pixels,wo,ho,format,type_out,pack,*target);memory.write(GuestAddress{arg(8)},output);
            return finish(0);
        }
        if(name=="gluBuild2DMipmaps" || name=="gluBuild1DMipmaps"){
            // 1D 版本 (target,components,width,format,type,data) 與 2D 共用縮放與逐層平均，高度固定 1。
            const bool one_d=name=="gluBuild1DMipmaps";
            const auto target=d(0),components=d(1),format=d(one_d?3:4),type=d(one_d?4:5);const auto width=n(2),height=one_d?1:n(3);
            if(width<1 || height<1)return finish(glu_invalid_value);if(const auto error=validate(format,type))return finish(error);
            if(!context)return finish(glu_invalid_operation);
            auto& gl=*context->native;std::array<std::uint8_t,4> limit{};gl.execute(GlOperation::GetIntegerv,std::array<std::uint64_t,2>{0xd33,0},{},limit);
            std::int32_t maximum=0;std::memcpy(&maximum,limit.data(),4);if(maximum<1)maximum=2048;
            auto w=static_cast<std::int32_t>(std::bit_floor(static_cast<std::uint32_t>(width))),h=static_cast<std::int32_t>(std::bit_floor(static_cast<std::uint32_t>(height)));
            while(w>maximum || h>maximum){w=std::max(1,w/2);h=std::max(1,h/2);}
            const auto unpack=pixel_store(context,false);const auto source=layout(width,height,format,type,unpack);if(!source)return finish(glu_out_of_memory);
            const auto input=guest_bytes(memory,arg(one_d?5:6),source->total);auto pixels=decode(input,width,height,format,type,unpack,*source);
            if(w!=width || h!=height)pixels=scale(pixels,width,height,channels(format),w,h);
            // 上傳時暫用緊密排列（對齊 1、無 skip／row length／byte swap），結束後還原 context 的解包設定。
            const auto set_unpack=[&](const PixelStore& s){const std::array<std::int32_t,6> values{s.swap,s.lsb,s.row,s.skip_rows,s.skip_pixels,s.alignment};for(unsigned i=0;i<6;++i)gl.execute(GlOperation::PixelStorei,std::array<std::uint64_t,2>{0xcf0U+i,static_cast<std::uint32_t>(values[i])});};
            PixelStore tight;tight.alignment=1;set_unpack(tight);
            struct Restore {const decltype(set_unpack)& apply;PixelStore saved;~Restore(){try{apply(saved);}catch(...){}}} restore{set_unpack,unpack};
            for(std::int32_t level=0;;++level){
                const auto packed_layout=layout(w,h,format,type,tight);if(!packed_layout)return finish(glu_out_of_memory);
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(packed_layout->total));encode(bytes,pixels,w,h,format,type,tight,*packed_layout);
                gl.execute(GlOperation::TexImage2D,std::array<std::uint64_t,9>{target,static_cast<std::uint32_t>(level),components,static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h),0,format,type,1},bytes);
                if(w==1 && h==1)break;
                const auto next_w=std::max(1,w/2),next_h=std::max(1,h/2);pixels=scale(pixels,w,h,channels(format),next_w,next_h);w=next_w;h=next_h;
            }
            return finish(0);
        }
    }catch(const MemoryFault&){return finish(name=="gluScaleImage" || name.ends_with("Mipmaps")?glu_invalid_value:0);}
    catch(const std::bad_alloc&){return finish(name=="gluScaleImage" || name.ends_with("Mipmaps")?glu_out_of_memory:0);}
    if(name=="gluNewQuadric"){
        const auto block=allocate_heap(memory,32);if(!block)return finish(0);
        opengl_.glu_quadrics[block]=GluQuadric{};return finish(block);
    }
    const auto quadric=opengl_.glu_quadrics.find(arg(0));if(quadric==opengl_.glu_quadrics.end())return finish(0);auto& q=quadric->second;
    if(name=="gluDeleteQuadric"){opengl_.glu_quadrics.erase(quadric);(void)free_heap(memory,arg(0),state);return finish(0);}
    if(name=="gluQuadricCallback"){if(d(1)!=100103)return report(q,glu_invalid_enum);q.callback=arg(2);return finish(0);}
    if(name=="gluQuadricDrawStyle"){if(d(1)<glu_point || d(1)>glu_silhouette)return report(q,glu_invalid_enum);q.style=d(1);return finish(0);}
    if(name=="gluQuadricNormals"){if(d(1)<glu_smooth || d(1)>glu_none)return report(q,glu_invalid_enum);q.normals=d(1);return finish(0);}
    if(name=="gluQuadricOrientation"){if(d(1)!=glu_outside && d(1)!=glu_inside)return report(q,glu_invalid_enum);q.orientation=d(1);return finish(0);}
    if(name=="gluQuadricTexture"){q.texture=(d(1)&0xff)!=0;return finish(0);}
    const double sign=q.orientation==glu_inside?-1:1;std::vector<QuadricVertex> grid;
    if(name=="gluSphere"){
        const double radius=real(1);const auto slices=n(2),stacks=n(3);if(slices<2 || stacks<1 || radius<0)return report(q,glu_invalid_value);if(!context)return finish(0);
        for(int j=0;j<=stacks;++j){const double theta=pi*j/stacks,z=-std::cos(theta),ring=std::sin(theta);
            for(int i=0;i<=slices;++i){const double alpha=2*pi*i/slices,x=std::sin(alpha)*ring,y=std::cos(alpha)*ring;grid.push_back({{x*radius,y*radius,z*radius},{x*sign,y*sign,z*sign},{double(i)/slices,double(j)/stacks}});}}
        Draw draw{*context->native,q};emit(draw,grid,slices,stacks,range(stacks+1),range(slices),true);return finish(0);
    }
    if(name=="gluCylinder"){
        const double base=real(1),top=real(2),height=real(3);const auto slices=n(4),stacks=n(5);
        if(slices<2 || stacks<1 || base<0 || top<0 || height<0)return report(q,glu_invalid_value);if(!context)return finish(0);
        for(int j=0;j<=stacks;++j){const double z=height*j/stacks,r=base+(top-base)*j/stacks;
            for(int i=0;i<=slices;++i){const double alpha=2*pi*i/slices,x=std::sin(alpha),y=std::cos(alpha);const auto normal=normalize({x*height,y*height,base-top},{x,y,0});
                grid.push_back({{x*r,y*r,z},{normal[0]*sign,normal[1]*sign,normal[2]*sign},{double(i)/slices,double(j)/stacks}});}}
        Draw draw{*context->native,q};
        emit(draw,grid,slices,stacks,q.style==glu_silhouette?std::vector<int>{0,stacks}:range(stacks+1),range(slices),true);return finish(0);
    }
    if(name=="gluDisk" || name=="gluPartialDisk"){
        const double inner=real(1),outer=real(2);const auto slices=n(3),loops=n(4);const bool partial=name=="gluPartialDisk";
        const double start=partial?real(5):0,sweep=partial?std::clamp(real(6),-360.0,360.0):360;
        if(slices<2 || loops<1 || outer<=0 || inner<0 || inner>outer)return report(q,glu_invalid_value);if(!context)return finish(0);
        for(int k=0;k<=loops;++k){const double r=inner+(outer-inner)*k/loops;
            for(int i=0;i<=slices;++i){const double angle=(start+sweep*i/slices)*pi/180,x=r*std::sin(angle),y=r*std::cos(angle);grid.push_back({{x,y,0},{0,0,sign},{0.5+x/(2*outer),0.5+y/(2*outer)}});}}
        const bool full=std::fabs(sweep)>=360;Draw draw{*context->native,q};
        std::vector<int> rings=q.style==glu_silhouette?std::vector<int>{0,loops}:range(loops+1);if(inner==0)std::erase(rings,0);
        std::vector<int> spokes=q.style==glu_silhouette?(full?std::vector<int>{}:std::vector<int>{0,slices}):(full?range(slices):range(slices+1));
        emit(draw,grid,slices,loops,rings,spokes,full);return finish(0);
    }
    return finish(0);
}
}
