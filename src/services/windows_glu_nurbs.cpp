#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "glu_tessellation.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>

// GLU NURBS：宿主端以 de Boor 演算法求值（VERTEX_4 於齊次座標求值後再除以 w），依 GLU 取樣方法決定分段數，
// 修剪區域在參數空間以網格處理：完全在區域內的格子直接輸出，跨越修剪邊界的格子由共用 tessellator 求與區域的交集。
// GLU_NURBS_RENDERER 模式直接送出 GL 基本圖元；GLU_NURBS_TESSELLATOR 模式改以 GLU_NURBS_* 回呼回傳。
namespace x64arm {
namespace {
constexpr std::uint32_t glu_error=100103,nurbs_begin=100164,invalid_enum=100900,invalid_value=100901;
constexpr std::uint32_t fill=100012,outline_polygon=100240,outline_patch=100241,renderer=100162,tessellator=100161;
constexpr std::uint32_t path_length=100215,parametric_error=100216,domain_distance=100217,object_path_length=100209,object_parametric_error=100208;
constexpr std::uint32_t trim2=100210,trim3=100211;
// GLU_NURBS_ERRORn＝100250+n。
constexpr std::uint32_t nurbs_error(unsigned n){return 100250+n;}
using Vec4=std::array<double,4>;
// GL map 類型沿用 opengl32 求值器的分量表；修剪曲線另外是 2（TRIM_2）或 3（TRIM_3，含 w）分量。
unsigned components(std::uint32_t type){return type==trim2?2U:type==trim3?3U:gl_map_components(type);}
// de Boor：order 階、knots 長度＝控制點數＋order。
Vec4 deboor(const std::vector<float>& knots,const std::vector<Vec4>& points,unsigned order,double u){
    const auto n=points.size();const unsigned p=order-1;std::size_t k=p;
    while(k+1<n && !(u<knots[k+1]))++k;
    while(k>p && !(knots[k]<knots[k+1]))--k;
    std::vector<Vec4> d(order);for(unsigned j=0;j<=p;++j)d[j]=points[k+j-p];
    for(unsigned r=1;r<=p;++r)for(unsigned j=p;j>=r;--j){
        const double left=knots[k+j-p],right=knots[k+1+j-r];const double alpha=right>left?(u-left)/(right-left):0;
        for(unsigned c=0;c<4;++c)d[j][c]=(1-alpha)*d[j-1][c]+alpha*d[j][c];
    }
    return d[p];
}
struct Vertex {Vec4 position{};std::optional<std::array<double,3>> normal;std::optional<Vec4> color;std::optional<Vec4> texcoord;unsigned texcomponents{};};
struct Primitive {std::uint32_t mode{};std::vector<Vertex> vertices;};
struct Matrices {std::array<double,16> model{},projection{};std::array<double,4> viewport{};bool valid{};};
std::array<double,3> project(const Matrices& m,const Vec4& p){
    Vec4 eye{},clip{};for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)eye[r]+=m.model[c*4+r]*p[c];}for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)clip[r]+=m.projection[c*4+r]*eye[c];}
    const double w=std::fabs(clip[3])>1e-12?clip[3]:1e-12;return {m.viewport[0]+(clip[0]/w+1)*m.viewport[2]/2,m.viewport[1]+(clip[1]/w+1)*m.viewport[3]/2,clip[2]/w};
}
std::array<double,3> euclid(const Vec4& p){const double w=std::fabs(p[3])>1e-12?p[3]:1;return {p[0]/w,p[1]/w,p[2]/w};}
// 一段控制多邊形的長度（螢幕像素或物件空間）。
double polygon_length(const GluNurbs& nurbs,const Matrices& m,const std::vector<Vec4>& points){
    const bool screen=(nurbs.sampling==path_length || nurbs.sampling==parametric_error) && m.valid;double length=0;
    for(std::size_t i=1;i<points.size();++i){const auto a=screen?project(m,points[i-1]):euclid(points[i-1]),b=screen?project(m,points[i]):euclid(points[i]);
        length+=std::sqrt((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+(screen?0:(a[2]-b[2])*(a[2]-b[2])));}
    return length;
}
unsigned segments(const GluNurbs& nurbs,double length,double range,float step){
    double count=0;
    if(nurbs.sampling==domain_distance)count=range*step;
    else count=length/std::max(1e-3,static_cast<double>(nurbs.sampling==parametric_error || nurbs.sampling==object_parametric_error?nurbs.parametric*8:nurbs.tolerance));
    return static_cast<unsigned>(std::clamp(std::ceil(count),1.0,256.0));
}
struct Map {std::uint32_t type{};unsigned comps{};std::vector<Vec4> points;std::vector<float> knots,sknots,tknots;unsigned order{},sorder{},torder{},scount{},tcount{};};
Vec4 to_vec(const std::array<float,4>& p,unsigned comps){Vec4 v{0,0,0,1};for(unsigned c=0;c<comps;++c)v[c]=p[c];if(comps<4 && comps>=1){}return v;}
// 張量積曲面求值：先沿 s 對每列控制點求值，再沿 t 求值。
Vec4 surface_at(const Map& map,double s,double t){
    std::vector<Vec4> column(map.tcount);std::vector<Vec4> row(map.scount);
    for(unsigned j=0;j<map.tcount;++j){for(unsigned i=0;i<map.scount;++i)row[i]=map.points[i*map.tcount+j];column[j]=deboor(map.sknots,row,map.sorder,s);}
    return deboor(map.tknots,column,map.torder,t);
}
// 修剪迴圈的有號面積（逆時針為正）與繞數。
double signed_area(const std::vector<std::array<double,2>>& loop){double area=0;for(std::size_t i=0;i<loop.size();++i){const auto& a=loop[i];const auto& b=loop[(i+1)%loop.size()];area+=a[0]*b[1]-b[0]*a[1];}return area/2;}
int winding(const std::vector<std::vector<std::array<double,2>>>& loops,double x,double y){
    int total=0;for(const auto& loop:loops)for(std::size_t i=0;i<loop.size();++i){const auto& a=loop[i];const auto& b=loop[(i+1)%loop.size()];
        if(a[1]<=y){if(b[1]>y && (b[0]-a[0])*(y-a[1])-(x-a[0])*(b[1]-a[1])>0)++total;}else if(b[1]<=y && (b[0]-a[0])*(y-a[1])-(x-a[0])*(b[1]-a[1])<0)--total;}
    return total;
}
bool segment_hits_box(std::array<double,2> a,std::array<double,2> b,double x0,double y0,double x1,double y1){
    double t0=0,t1=1;const double dx=b[0]-a[0],dy=b[1]-a[1];
    const auto clip=[&](double p,double q){if(p==0)return q>=0;const double r=q/p;if(p<0){if(r>t1)return false;if(r>t0)t0=r;}else{if(r<t0)return false;if(r<t1)t1=r;}return true;};
    return clip(-dx,a[0]-x0) && clip(dx,x1-a[0]) && clip(-dy,a[1]-y0) && clip(dy,y1-a[1]);
}
}

std::optional<std::uint32_t> WindowsServices::invoke_glu_nurbs(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};const auto n=[&](unsigned i){return std::bit_cast<std::int32_t>(d(i));};
    const bool x86=memory.execution_mode()==ExecutionMode::X86;const unsigned pointer=x86?4U:8U;
    const auto real32=[&](unsigned i)->float{if(x86 || i>=4)return std::bit_cast<float>(d(i));std::uint32_t value=0;for(unsigned k=0;k<4;++k)value|=std::uint32_t(state.xmm[i][k])<<(k*8);return std::bit_cast<float>(value);};
    const auto floats=[&](std::uint64_t address,std::size_t count){std::vector<float> values(count);for(std::size_t i=0;i<count;++i)values[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{address+i*4},32)));return values;};
    OpenGlContext* context=nullptr;
    if(const auto current=opengl_.current.find(threads_.current);current!=opengl_.current.end()){const auto found=opengl_.contexts.find(current->second);if(found!=opengl_.contexts.end() && found->second.native)context=&found->second;}
    if(name=="gluNewNurbsRenderer"){const auto block=allocate_heap(memory,16);if(!block)return finish(0);opengl_.glu_nurbs[block]=GluNurbs{};return finish(block);}
    const auto found=opengl_.glu_nurbs.find(arg(0));if(found==opengl_.glu_nurbs.end())return finish(0);
    auto& nurbs=found->second;
    if(name=="gluDeleteNurbsRenderer"){opengl_.glu_nurbs.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
    std::vector<std::uint32_t> errors;std::vector<Primitive> output;
    const auto fail=[&](std::uint32_t code){errors.push_back(code);};
    const auto valid_knots=[&](const std::vector<float>& knots,unsigned order,std::size_t count)->bool{
        if(order<1 || order>24){fail(nurbs_error(1));return false;}if(knots.size()<2*static_cast<std::size_t>(order) || count<order){fail(nurbs_error(2));return false;}
        for(std::size_t i=1;i<knots.size();++i)if(knots[i]<knots[i-1]){fail(nurbs_error(4));return false;}
        if(!(knots[order-1]<knots[count])){fail(nurbs_error(3));return false;}return true;
    };
    const auto matrices=[&]{
        Matrices m;
        if(nurbs.auto_load && context){std::array<std::uint8_t,64> buffer{};
            for(const auto [pname,target]:{std::pair{0x0BA6U,&m.model},std::pair{0x0BA7U,&m.projection}}){context->native->execute(GlOperation::GetFloatv,std::array<std::uint64_t,2>{pname,0},{},buffer);for(unsigned i=0;i<16;++i){float v;std::memcpy(&v,buffer.data()+i*4,4);(*target)[i]=v;}}
            std::array<std::uint8_t,16> view{};context->native->execute(GlOperation::GetIntegerv,std::array<std::uint64_t,2>{0x0BA2,0},{},view);for(unsigned i=0;i<4;++i){std::int32_t v;std::memcpy(&v,view.data()+i*4,4);m.viewport[i]=v;}
            m.valid=m.viewport[2]>0 && m.viewport[3]>0;
        }else if(nurbs.loaded){for(unsigned i=0;i<16;++i){m.model[i]=nurbs.model[i];m.projection[i]=nurbs.projection[i];}for(unsigned i=0;i<4;++i)m.viewport[i]=nurbs.viewport[i];m.valid=m.viewport[2]>0 && m.viewport[3]>0;}
        return m;
    };
    // 曲線：GL_LINE_STRIP；VERTEX_3／4 必須提供，其餘 map 依參數一起求值。
    const auto render_curves=[&]{
        const GluNurbsCurveData* geometry=nullptr;for(const auto& c:nurbs.curves)if(c.type==0x0D97 || c.type==0x0D98)geometry=&c;
        if(!geometry){fail(nurbs_error(8));return;}
        const auto m=matrices();std::vector<Map> maps;
        for(const auto& c:nurbs.curves){Map map;map.type=c.type;map.comps=components(c.type);map.knots=c.knots;map.order=c.order;for(const auto& p:c.points)map.points.push_back(to_vec(p,map.comps));maps.push_back(std::move(map));}
        const auto& g=*std::find_if(maps.begin(),maps.end(),[](const Map& map){return map.type==0x0D97 || map.type==0x0D98;});
        const double u0=g.knots[g.order-1],u1=g.knots[g.points.size()];const auto steps=segments(nurbs,polygon_length(nurbs,m,g.points),u1-u0,nurbs.ustep);
        Primitive line;line.mode=3;
        for(unsigned i=0;i<=steps;++i){
            Vertex v;
            for(const auto& map:maps){const double a=map.knots[map.order-1],b=map.knots[map.points.size()];const double u=a+(b-a)*i/steps;auto value=deboor(map.knots,map.points,map.order,u);
                if(map.type==0x0D97 || map.type==0x0D98){if(map.type==0x0D97)value[3]=1;v.position=value;}
                else if(map.type==0x0D92)v.normal=std::array<double,3>{value[0],value[1],value[2]};
                else if(map.type==0x0D90)v.color=value;
                else if(map.type>=0x0D93 && map.type<=0x0D96){v.texcoord=value;v.texcomponents=map.comps;}}
            line.vertices.push_back(v);
        }
        output.push_back(std::move(line));
    };
    const auto render_surface=[&]{
        const GluNurbsSurfaceData* geometry=nullptr;for(const auto& s:nurbs.surfaces)if(s.type==0x0DB7 || s.type==0x0DB8)geometry=&s;
        if(!geometry){fail(nurbs_error(8));return;}
        std::vector<Map> maps;
        for(const auto& s:nurbs.surfaces){Map map;map.type=s.type;map.comps=components(s.type);map.sknots=s.sknots;map.tknots=s.tknots;map.sorder=s.sorder;map.torder=s.torder;map.scount=s.scount;map.tcount=s.tcount;for(const auto& p:s.points)map.points.push_back(to_vec(p,map.comps));maps.push_back(std::move(map));}
        const auto g=*std::find_if(maps.begin(),maps.end(),[](const Map& map){return map.type==0x0DB7 || map.type==0x0DB8;});
        const auto m=matrices();
        if(nurbs.culling && m.valid){
            // 所有控制點都落在同一個視埠外側時略過。
            bool left=true,right=true,below=true,above=true;for(const auto& p:g.points){const auto s=project(m,p);left=left && s[0]<m.viewport[0];right=right && s[0]>m.viewport[0]+m.viewport[2];below=below && s[1]<m.viewport[1];above=above && s[1]>m.viewport[1]+m.viewport[3];}
            if(left || right || below || above)return;
        }
        const double s0=g.sknots[g.sorder-1],s1=g.sknots[g.scount],t0=g.tknots[g.torder-1],t1=g.tknots[g.tcount];
        double slength=0,tlength=0;
        for(unsigned j=0;j<g.tcount;++j){std::vector<Vec4> row;for(unsigned i=0;i<g.scount;++i)row.push_back(g.points[i*g.tcount+j]);slength=std::max(slength,polygon_length(nurbs,m,row));}
        for(unsigned i=0;i<g.scount;++i){std::vector<Vec4> column(g.points.begin()+i*g.tcount,g.points.begin()+(i+1)*g.tcount);tlength=std::max(tlength,polygon_length(nurbs,m,column));}
        const auto ns=segments(nurbs,slength,s1-s0,nurbs.ustep),nt=segments(nurbs,tlength,t1-t0,nurbs.vstep);
        bool auto_normal=false;if(context)auto_normal=context->native->execute(GlOperation::IsEnabled,std::array<std::uint64_t,1>{0x0D80})!=0;
        const bool has_normal_map=std::any_of(maps.begin(),maps.end(),[](const Map& map){return map.type==0x0DB2;});
        const auto at=[&](double s,double t){
            Vertex v;
            for(const auto& map:maps){const auto u=map.sknots[map.sorder-1]+(map.sknots[map.scount]-map.sknots[map.sorder-1])*(s-s0)/std::max(1e-12,s1-s0);
                const auto w=map.tknots[map.torder-1]+(map.tknots[map.tcount]-map.tknots[map.torder-1])*(t-t0)/std::max(1e-12,t1-t0);auto value=surface_at(map,u,w);
                if(map.type==0x0DB7 || map.type==0x0DB8){if(map.type==0x0DB7)value[3]=1;v.position=value;}
                else if(map.type==0x0DB2)v.normal=std::array<double,3>{value[0],value[1],value[2]};
                else if(map.type==0x0DB0)v.color=value;
                else if(map.type>=0x0DB3 && map.type<=0x0DB6){v.texcoord=value;v.texcomponents=map.comps;}}
            if(!has_normal_map && auto_normal){
                // 以參數空間中央差分求偏導數的外積作為法線。
                const double hs=(s1-s0)*1e-4,ht=(t1-t0)*1e-4;const auto e=[&](double a,double b){auto p=surface_at(g,std::clamp(a,s0,s1),std::clamp(b,t0,t1));if(g.type==0x0DB7)p[3]=1;return euclid(p);};
                const auto ds0=e(s-hs,t),ds1=e(s+hs,t),dt0=e(s,t-ht),dt1=e(s,t+ht);const std::array<double,3> a{ds1[0]-ds0[0],ds1[1]-ds0[1],ds1[2]-ds0[2]},b{dt1[0]-dt0[0],dt1[1]-dt0[1],dt1[2]-dt0[2]};
                std::array<double,3> normal{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};const auto length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
                if(length>0)for(auto& c:normal)c/=length;v.normal=normal;
            }
            return v;
        };
        // 修剪迴圈：分段線性與 NURBS 修剪曲線依序串接成參數空間多邊形。
        std::vector<std::vector<std::array<double,2>>> loops;
        for(const auto& trim:nurbs.trims){
            std::vector<std::array<double,2>> loop;
            for(const auto& piece:trim){
                if(piece.pwl){for(const auto& p:piece.points){const double w=p[2]!=0?p[2]:1;loop.push_back({p[0]/w,p[1]/w});}continue;}
                const auto comps=components(piece.curve.type);std::vector<Vec4> points;for(const auto& p:piece.curve.points){Vec4 v{p[0],p[1],0,comps==3?p[2]:1};points.push_back(v);}
                const double a=piece.curve.knots[piece.curve.order-1],b=piece.curve.knots[points.size()];const unsigned steps=std::max(16U,static_cast<unsigned>(points.size())*8);
                for(unsigned i=0;i<=steps;++i){const auto value=deboor(piece.curve.knots,points,piece.curve.order,a+(b-a)*i/steps);const double w=value[3]!=0?value[3]:1;loop.push_back({value[0]/w,value[1]/w});}
            }
            // 首尾重合點與重複點去除。
            std::vector<std::array<double,2>> cleaned;for(const auto& p:loop)if(cleaned.empty() || std::fabs(cleaned.back()[0]-p[0])>1e-9 || std::fabs(cleaned.back()[1]-p[1])>1e-9)cleaned.push_back(p);
            if(cleaned.size()>1 && std::fabs(cleaned.front()[0]-cleaned.back()[0])<1e-9 && std::fabs(cleaned.front()[1]-cleaned.back()[1])<1e-9)cleaned.pop_back();
            if(cleaned.size()<3){fail(nurbs_error(18));return;}loops.push_back(std::move(cleaned));
        }
        if(!loops.empty()){double total=0;for(const auto& loop:loops)total+=signed_area(loop);if(total<0)loops.insert(loops.begin(),{{s0,t0},{s1,t0},{s1,t1},{s0,t1}});}
        std::vector<std::array<std::array<double,2>,3>> triangles;
        const double ds=(s1-s0)/ns,dt=(t1-t0)/nt;
        for(unsigned j=0;j<nt;++j)for(unsigned i=0;i<ns;++i){
            const double x0=s0+i*ds,x1=i+1==ns?s1:s0+(i+1)*ds,y0=t0+j*dt,y1=j+1==nt?t1:t0+(j+1)*dt;
            bool crossing=false;
            for(const auto& loop:loops){for(std::size_t k=0;k<loop.size() && !crossing;++k)crossing=segment_hits_box(loop[k],loop[(k+1)%loop.size()],x0,y0,x1,y1);if(crossing)break;}
            if(!crossing){if(loops.empty() || winding(loops,(x0+x1)/2,(y0+y1)/2)>0){triangles.push_back({{{x0,y0},{x1,y0},{x1,y1}}});triangles.push_back({{{x0,y0},{x1,y1},{x0,y1}}});}continue;}
            // 格子與修剪區域的交集：格子（+1）與修剪迴圈一起以 ABS_GEQ_TWO 求交。
            GluTessellator tess;tess.winding=100134;tess.normal={0,0,1};
            tess.contours.push_back({{{x0,y0,0},0},{{x1,y0,0},0},{{x1,y1,0},0},{{x0,y1,0},0}});
            for(const auto& loop:loops){std::vector<GluTessVertex> contour;for(const auto& p:loop)contour.push_back({{p[0],p[1],0},0});tess.contours.push_back(std::move(contour));}
            if(const auto result=glu_triangulate(tess))for(const auto& tri:result->triangles)triangles.push_back({{{result->positions[tri[0]][0],result->positions[tri[0]][1]},{result->positions[tri[1]][0],result->positions[tri[1]][1]},{result->positions[tri[2]][0],result->positions[tri[2]][1]}}});
        }
        if(nurbs.display==outline_patch){
            // 邊界：修剪迴圈或整個定義域的外框。
            const auto edges=loops.empty()?std::vector<std::vector<std::array<double,2>>>{{{s0,t0},{s1,t0},{s1,t1},{s0,t1}}}:loops;
            for(const auto& loop:edges){Primitive line;line.mode=2;for(std::size_t k=0;k<loop.size();++k){const auto& a=loop[k];const auto& b=loop[(k+1)%loop.size()];for(unsigned q=0;q<8;++q)line.vertices.push_back(at(a[0]+(b[0]-a[0])*q/8,a[1]+(b[1]-a[1])*q/8));}output.push_back(std::move(line));}
            return;
        }
        if(nurbs.display==outline_polygon){for(const auto& tri:triangles){Primitive loop;loop.mode=2;for(const auto& p:tri)loop.vertices.push_back(at(p[0],p[1]));output.push_back(std::move(loop));}return;}
        Primitive filled;filled.mode=4;for(const auto& tri:triangles)for(const auto& p:tri)filled.vertices.push_back(at(p[0],p[1]));if(!filled.vertices.empty())output.push_back(std::move(filled));
    };
    // 讀取控制點：每點依 stride 取 comps 個 float。
    const auto read_points=[&](std::uint64_t address,std::size_t count,std::uint32_t stride,unsigned comps){std::vector<std::array<float,4>> points;for(std::size_t i=0;i<count;++i){std::array<float,4> p{0,0,0,1};const auto values=floats(address+i*stride*4ULL,comps);std::copy(values.begin(),values.end(),p.begin());points.push_back(p);}return points;};
    if(name=="gluNurbsProperty" || name=="gluGetNurbsProperty"){
        // GLU_AUTO_LOAD_MATRIX 100200、CULLING 100201、PARAMETRIC_TOLERANCE 100202、SAMPLING_TOLERANCE 100203、DISPLAY_MODE 100204、SAMPLING_METHOD 100205、U_STEP 100206、V_STEP 100207、NURBS_MODE 100160。
        const bool get=name=="gluGetNurbsProperty";const auto property=d(1);float value=get?0.F:real32(2);bool ok=true;
        switch(property){
        case 100200:if(get)value=nurbs.auto_load?1.F:0.F;else nurbs.auto_load=value!=0;break;
        case 100201:if(get)value=nurbs.culling?1.F:0.F;else nurbs.culling=value!=0;break;
        case 100202:if(get)value=nurbs.parametric;else if(value>0)nurbs.parametric=value;else fail(invalid_value);break;
        case 100203:if(get)value=nurbs.tolerance;else if(value>0)nurbs.tolerance=value;else fail(invalid_value);break;
        case 100204:if(get)value=static_cast<float>(nurbs.display);else {const auto mode=static_cast<std::uint32_t>(value);if(mode==fill || mode==outline_polygon || mode==outline_patch)nurbs.display=mode;else fail(invalid_value);}break;
        case 100205:if(get)value=static_cast<float>(nurbs.sampling);else {const auto method=static_cast<std::uint32_t>(value);if(method==path_length || method==parametric_error || method==domain_distance || method==object_path_length || method==object_parametric_error)nurbs.sampling=method;else fail(invalid_value);}break;
        case 100206:if(get)value=nurbs.ustep;else if(value>0)nurbs.ustep=value;else fail(invalid_value);break;
        case 100207:if(get)value=nurbs.vstep;else if(value>0)nurbs.vstep=value;else fail(invalid_value);break;
        case 100160:if(get)value=static_cast<float>(nurbs.mode);else {const auto mode=static_cast<std::uint32_t>(value);if(mode==renderer || mode==tessellator)nurbs.mode=mode;else fail(invalid_value);}break;
        default:ok=false;fail(invalid_enum);break;
        }
        if(get && ok && arg(2))memory.store(GuestAddress{arg(2)},32,std::bit_cast<std::uint32_t>(value));
    }else if(name=="gluLoadSamplingMatrices"){
        if(arg(1) && arg(2) && arg(3)){const auto model=floats(arg(1),16),projection=floats(arg(2),16);std::copy(model.begin(),model.end(),nurbs.model.begin());std::copy(projection.begin(),projection.end(),nurbs.projection.begin());
            for(unsigned i=0;i<4;++i)nurbs.viewport[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(3)+i*4},32));nurbs.loaded=true;}
    }else if(name=="gluNurbsCallback"){
        const auto which=d(1);if(which==glu_error)nurbs.callbacks[0]=arg(2);else if(which>=nurbs_begin && which<=nurbs_begin+11)nurbs.callbacks[which-nurbs_begin+1]=arg(2);else fail(invalid_enum);
    }else if(name=="gluBeginCurve"){
        // 狀態：0 無、1 曲線、2 曲面、3 修剪。重複開始時先回報，再把未結束的區塊收尾後重新開始。
        if(nurbs.state==1){fail(nurbs_error(6));render_curves();nurbs.curves.clear();}
        else if(nurbs.state)fail(nurbs_error(6));else {nurbs.state=1;nurbs.curves.clear();}
    }else if(name=="gluEndCurve"){if(nurbs.state!=1)fail(nurbs_error(7));else {nurbs.state=0;render_curves();nurbs.curves.clear();}}
    else if(name=="gluBeginSurface"){
        if(nurbs.state>=2){fail(nurbs_error(27));nurbs.state=2;render_surface();}
        if(nurbs.state==1)fail(nurbs_error(27));else {nurbs.state=2;nurbs.surfaces.clear();nurbs.trims.clear();}
    }else if(name=="gluEndSurface"){
        if(nurbs.state==3){fail(nurbs_error(12));nurbs.state=2;if(nurbs.trims.back().empty()){nurbs.trims.pop_back();fail(nurbs_error(18));}}
        if(nurbs.state!=2)fail(nurbs_error(13));else {nurbs.state=0;render_surface();nurbs.surfaces.clear();nurbs.trims.clear();}
    }else if(name=="gluBeginTrim"){
        // 曲面外開始修剪時隱含開始曲面；修剪中再開始則先結束前一個修剪迴圈。
        if(nurbs.state==0){fail(nurbs_error(15));nurbs.state=2;nurbs.surfaces.clear();nurbs.trims.clear();}
        if(nurbs.state==3){fail(nurbs_error(16));nurbs.state=2;if(nurbs.trims.back().empty()){nurbs.trims.pop_back();fail(nurbs_error(18));}}
        if(nurbs.state!=2)fail(nurbs_error(15));else {nurbs.state=3;nurbs.trims.emplace_back();}
    }else if(name=="gluEndTrim"){if(nurbs.state!=3)fail(nurbs_error(17));else {nurbs.state=2;if(nurbs.trims.back().empty()){nurbs.trims.pop_back();fail(nurbs_error(18));}}
    }else if(name=="gluPwlCurve"){
        // (nobj, count, array, stride, type)：GLU_MAP1_TRIM_2／TRIM_3，只能在修剪區塊中。
        const auto count=n(1),stride=n(3);const auto type=d(4);
        if(nurbs.state!=3)fail(nurbs_error(19));else if(count<0)fail(nurbs_error(33));else if(stride<0)fail(nurbs_error(34));else if(type!=trim2 && type!=trim3)fail(nurbs_error(35));else if(!arg(2))fail(nurbs_error(36));
        else {GluNurbsTrimPiece piece;piece.pwl=true;const unsigned comps=type==trim2?2:3;for(std::int32_t i=0;i<std::min(count,1<<16);++i){const auto values=floats(arg(2)+static_cast<std::uint64_t>(i)*static_cast<std::uint64_t>(stride)*4,comps);piece.points.push_back({values[0],values[1],comps==3?values[2]:1.F});}nurbs.trims.back().push_back(std::move(piece));}
    }else if(name=="gluNurbsCurve"){
        // (nobj, nknots, knot, stride, ctlarray, order, type)：曲線區塊中的幾何／屬性 map，或修剪區塊中的 TRIM_2／TRIM_3 曲線。
        const auto knots_count=n(1),stride=n(3),order=n(5);const auto type=d(6);const auto comps=components(type);
        const bool trimming=nurbs.state==3,standalone=nurbs.state==0 || nurbs.state==2;
        if(knots_count<0)fail(nurbs_error(33));else if(stride<0)fail(nurbs_error(34));else if(!comps || (trimming!=(type==trim2 || type==trim3)) || (!trimming && type>=0x0DB0))fail(nurbs_error(trimming?14:35));
        else if(!arg(2) || !arg(4))fail(nurbs_error(36));
        else {
            GluNurbsCurveData curve;curve.knots=floats(arg(2),static_cast<std::size_t>(std::min(knots_count,1<<16)));curve.order=static_cast<std::uint32_t>(order);curve.type=type;const auto count=knots_count>order?static_cast<std::size_t>(knots_count-order):0U;
            if(order>=1 && stride<static_cast<std::int32_t>(comps))fail(nurbs_error(34));
            else if(valid_knots(curve.knots,curve.order,count)){curve.points=read_points(arg(4),count,static_cast<std::uint32_t>(stride),comps);
                if(trimming){GluNurbsTrimPiece piece;piece.curve=std::move(curve);nurbs.trims.back().push_back(std::move(piece));}
                else if(standalone){nurbs.curves.assign(1,std::move(curve));render_curves();nurbs.curves.clear();}else nurbs.curves.push_back(std::move(curve));}
        }
    }else if(name=="gluNurbsSurface"){
        // (nobj, sknot_count, sknot, tknot_count, tknot, s_stride, t_stride, ctlarray, sorder, torder, type)
        const auto sk=n(1),tk=n(3),ss=n(5),ts=n(6),so=n(8),to=n(9);const auto type=d(10);const auto comps=components(type);
        // 曲面外的 gluNurbsSurface 隱含一個只含這個 map 的曲面並立即繪製。
        const bool standalone=nurbs.state==0;
        if(nurbs.state==1 || nurbs.state==3)fail(nurbs_error(22));else if(sk<0 || tk<0)fail(nurbs_error(33));else if(ss<0 || ts<0)fail(nurbs_error(34));else if(!comps || type<0x0DB0 || type>0x0DB8)fail(nurbs_error(35));else if(!arg(2) || !arg(4) || !arg(7))fail(nurbs_error(36));
        else {
            GluNurbsSurfaceData surface;surface.sknots=floats(arg(2),static_cast<std::size_t>(std::min(sk,1<<12)));surface.tknots=floats(arg(4),static_cast<std::size_t>(std::min(tk,1<<12)));
            surface.sorder=static_cast<std::uint32_t>(so);surface.torder=static_cast<std::uint32_t>(to);surface.type=type;
            const auto scount=sk>so?static_cast<std::size_t>(sk-so):0U,tcount=tk>to?static_cast<std::size_t>(tk-to):0U;
            if(valid_knots(surface.sknots,surface.sorder,scount) && valid_knots(surface.tknots,surface.torder,tcount)){
                if(scount*tcount>(1U<<16))fail(nurbs_error(8));
                else {surface.scount=static_cast<std::uint32_t>(scount);surface.tcount=static_cast<std::uint32_t>(tcount);
                    for(std::size_t i=0;i<scount;++i)for(std::size_t j=0;j<tcount;++j){std::array<float,4> p{0,0,0,1};const auto values=floats(arg(7)+(i*static_cast<std::uint64_t>(ss)+j*static_cast<std::uint64_t>(ts))*4,comps);std::copy(values.begin(),values.end(),p.begin());surface.points.push_back(p);}
                    if(standalone){nurbs.surfaces.assign(1,std::move(surface));nurbs.trims.clear();render_surface();nurbs.surfaces.clear();}else nurbs.surfaces.push_back(std::move(surface));}
            }
        }
    }
    const bool callbacks_mode=nurbs.mode==tessellator;
    if(!callbacks_mode && context && !output.empty()){
        auto& gl=*context->native;++context->calls;
        const auto f=[](double v){return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(static_cast<float>(v)));};
        for(const auto& primitive:output){
            gl.execute(GlOperation::Begin,std::array<std::uint64_t,1>{primitive.mode});
            for(const auto& v:primitive.vertices){
                if(v.color)gl.execute(GlOperation::Color4f,std::array<std::uint64_t,4>{f((*v.color)[0]),f((*v.color)[1]),f((*v.color)[2]),f((*v.color)[3])});
                if(v.texcoord){if(v.texcomponents==2)gl.execute(GlOperation::TexCoord2f,std::array<std::uint64_t,2>{f((*v.texcoord)[0]),f((*v.texcoord)[1])});
                    else {std::array<float,4> t{static_cast<float>((*v.texcoord)[0]),v.texcomponents>1?static_cast<float>((*v.texcoord)[1]):0.F,v.texcomponents>2?static_cast<float>((*v.texcoord)[2]):0.F,v.texcomponents>3?static_cast<float>((*v.texcoord)[3]):1.F};gl.execute(GlOperation::TexCoord4fv,{},std::span(reinterpret_cast<const std::uint8_t*>(t.data()),16));}}
                if(v.normal)gl.execute(GlOperation::Normal3f,std::array<std::uint64_t,3>{f((*v.normal)[0]),f((*v.normal)[1]),f((*v.normal)[2])});
                const auto p=euclid(v.position);gl.execute(GlOperation::Vertex3f,std::array<std::uint64_t,3>{f(p[0]),f(p[1]),f(p[2])});
            }
            gl.execute(GlOperation::End);
        }
        output.clear();
    }
    if(errors.empty() && (!callbacks_mode || output.empty()))return finish(0);
    const auto table=nurbs.callbacks;
    return nested_continuation(state,memory,"GLU NURBS",[this,service,errors,output,table,pointer](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        // 捕獲值複製到協程框架；頂點資料以暫存區塊傳給回呼，回呼結束後釋放。
        const auto descriptor=service;const auto pending=errors;const auto primitives=output;const auto callbacks=table;(void)pointer;
        for(const auto code:pending)if(callbacks[0])(void)co_await service_callback::call_value(callbacks[0],{code});
        if(!primitives.empty()){
            const auto block=allocate_heap(m,16);if(!block)throw std::bad_alloc{};
            struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t block;~Release(){(void)self.free_heap(memory,block,state);}} release{*this,m,s,block};
            // 一般版與 _DATA 版：_DATA 版多傳一個使用者資料（此實作沒有 gluNurbsCallbackData，傳 NULL）。
            const auto pick=[&](unsigned offset){const auto data=callbacks[1+offset+6];return std::pair<std::uint64_t,bool>{data?data:callbacks[1+offset],data!=0};};
            const auto send=[&](const std::array<double,4>& values,unsigned count){for(unsigned i=0;i<count;++i)m.store(GuestAddress{block+i*4},32,std::bit_cast<std::uint32_t>(static_cast<float>(values[i])));};
            const auto begin=pick(0),vertex=pick(1),normal=pick(2),color=pick(3),texture=pick(4),end=pick(5);
            for(const auto& primitive:primitives){
                if(begin.first)(void)co_await service_callback::call_value(begin.first,begin.second?std::vector<std::uint64_t>{primitive.mode,0}:std::vector<std::uint64_t>{primitive.mode});
                for(const auto& v:primitive.vertices){
                    if(v.normal && normal.first){send({(*v.normal)[0],(*v.normal)[1],(*v.normal)[2],0},3);(void)co_await service_callback::call_value(normal.first,normal.second?std::vector<std::uint64_t>{block,0}:std::vector<std::uint64_t>{block});}
                    if(v.color && color.first){send(*v.color,4);(void)co_await service_callback::call_value(color.first,color.second?std::vector<std::uint64_t>{block,0}:std::vector<std::uint64_t>{block});}
                    if(v.texcoord && texture.first){send(*v.texcoord,v.texcomponents);(void)co_await service_callback::call_value(texture.first,texture.second?std::vector<std::uint64_t>{block,0}:std::vector<std::uint64_t>{block});}
                    if(vertex.first){const auto p=euclid(v.position);send({p[0],p[1],p[2],1},3);(void)co_await service_callback::call_value(vertex.first,vertex.second?std::vector<std::uint64_t>{block,0}:std::vector<std::uint64_t>{block});}
                }
                if(end.first)(void)co_await service_callback::call_value(end.first,end.second?std::vector<std::uint64_t>{0}:std::vector<std::uint64_t>{});
            }
        }
        const WindowsCall call(s,m,descriptor);call.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
}
}
