// GLU tessellator（glu32）宿主端實作：gluNewTess／gluDeleteTess（客體堆積物件）、gluTessCallback（BEGIN～COMBINE 與 _DATA 版本，_DATA 優先）、
// gluTessProperty／gluGetTessProperty（WINDING_RULE 的 ODD／NONZERO／POSITIVE／NEGATIVE／ABS_GEQ_TWO、BOUNDARY_ONLY、TOLERANCE 0～1）、gluTessNormal、
// gluTessBeginPolygon／BeginContour／Vertex／EndContour／EndPolygon，以及舊版 gluBeginPolygon／gluNextContour／gluEndPolygon。
// - 狀態機：呼叫順序不符時依 GLU 規格先回報 GLU_TESS_MISSING_BEGIN／END_POLYGON／CONTOUR，再自動補上或捨棄；座標絕對值超過 1e150 時夾住並回報 GLU_TESS_COORD_TOO_LARGE。
// - 幾何：法線未指定時以 Newell 法計算（退化時改用最大三角形），並使整體面積為正；投影到法線主軸平面後，在所有邊的交點與接觸點切開邊、
//   相同座標的頂點合併（沿用第一個頂點的資料），以邊的方向累計各面的環繞數，再依環繞規則取「內部」的面。
//   內部面以耳切法（含洞）三角化，三角形相對法線為逆時針；BOUNDARY_ONLY 時輸出內部區域的邊界（GL_LINE_LOOP，內部在左側）。
// - 回呼：交點經 COMBINE 取得新頂點資料（座標、四個來源頂點與權重，各邊權重和為 0.5）；沒有 COMBINE 回呼或回傳 NULL 時回報
//   GLU_TESS_NEED_COMBINE_CALLBACK 且不輸出。三角形以 GL_TRIANGLES 輸出；設定 EDGE_FLAG 回呼時在旗標改變前呼叫，只有區域邊界邊為 TRUE。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "service_callback_task.hpp"
#include "glu_tessellation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace x64arm {
namespace {
constexpr std::uint32_t tess_begin=100100,tess_vertex=100101,tess_end=100102,tess_error=100103,tess_edge_flag=100104,tess_combine=100105;
constexpr std::uint32_t glu_invalid_enum=100900,glu_invalid_value=100901;
constexpr std::size_t none=std::numeric_limits<std::size_t>::max();
struct Point {double x,y;bool operator==(const Point&)const=default;};
double orient(const Point& a,const Point& b,const Point& c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
bool inside_rule(std::uint32_t rule,long winding){
    switch(rule){case 100130:return (winding%2)!=0;case 100131:return winding!=0;case 100132:return winding>0;case 100133:return winding<0;default:return winding>=2 || winding<=-2;}
}
struct Vertex {Point p;std::array<double,3> xyz;std::uint64_t data{};bool intersection{};std::array<std::uint64_t,4> sources{};std::array<float,4> weights{};};
struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::array<std::size_t,3>> triangles;
    std::vector<std::array<bool,3>> boundary; // 三角形第 k 邊（頂點 k → k+1）是否為區域邊界
    std::vector<std::vector<std::size_t>> loops;
};
// 耳切法：outer 為逆時針、holes 為順時針的頂點編號（座標在 vertices）。
std::vector<std::array<std::size_t,3>> triangulate(const std::vector<Vertex>& vertices,const std::vector<std::size_t>& outer,const std::vector<std::vector<std::size_t>>& holes){
    struct Node {std::size_t id;Point p;std::size_t prev{},next{};};
    std::vector<Node> nodes;std::vector<std::array<std::size_t,3>> result;
    const auto ring=[&](const std::vector<std::size_t>& ids){const auto first=nodes.size();for(std::size_t k=0;k<ids.size();++k)nodes.push_back({ids[k],vertices[ids[k]].p,first+(k+ids.size()-1)%ids.size(),first+(k+1)%ids.size()});return first;};
    if(outer.size()<3)return result;
    const auto start=ring(outer);
    const auto P=[&](std::size_t n)->const Point&{return nodes[n].p;};
    const auto locally_inside=[&](std::size_t a,std::size_t b){
        const auto& prev=P(nodes[a].prev);const auto& next=P(nodes[a].next);
        return orient(prev,P(a),next)>0?orient(P(a),P(b),next)<=0 && orient(P(a),prev,P(b))<=0:orient(P(a),P(b),prev)>0 || orient(P(a),next,P(b))>0;
    };
    const auto split=[&](std::size_t a,std::size_t b){ // 以 a↔b 連接（a 在外圈、b 在洞上）
        const auto a2=nodes.size();nodes.push_back(nodes[a]);const auto b2=nodes.size();nodes.push_back(nodes[b]);
        const auto an=nodes[a].next,bp=nodes[b].prev;
        nodes[a].next=b;nodes[b].prev=a;nodes[a2].next=an;nodes[an].prev=a2;nodes[b2].next=a2;nodes[a2].prev=b2;nodes[bp].next=b2;nodes[b2].prev=bp;
    };
    // 洞依最右點由右到左接到外圈：自最右點往 +x 方向找最近的邊，取該邊 x 較大的端點，再檢查三角形內是否有更合適的頂點。
    std::vector<std::size_t> anchors;
    for(const auto& hole:holes){if(hole.size()<3)continue;const auto first=ring(hole);std::size_t best=first;for(std::size_t k=0;k<hole.size();++k){const auto n=first+k;if(P(n).x>P(best).x || (P(n).x==P(best).x && P(n).y<P(best).y))best=n;}anchors.push_back(best);}
    std::sort(anchors.begin(),anchors.end(),[&](std::size_t a,std::size_t b){return P(a).x>P(b).x;});
    for(const auto h:anchors){
        const auto& hp=P(h);double nearest=std::numeric_limits<double>::infinity();std::size_t m=none;std::size_t n=start;
        do{const auto q=nodes[n].next;const auto& a=P(n);const auto& b=P(q);
            if((a.y<=hp.y && b.y>hp.y) || (b.y<=hp.y && a.y>hp.y)){const double x=a.x+(hp.y-a.y)*(b.x-a.x)/(b.y-a.y);if(x>=hp.x && x<nearest){nearest=x;m=a.x>b.x?n:q;}}
            else if(a.y==hp.y && a.x>=hp.x && a.x<nearest){nearest=a.x;m=n;}
            n=q;}while(n!=start);
        if(m==none)continue;
        const Point hit{nearest,hp.y};const auto mp=P(m);double best_tan=std::numeric_limits<double>::infinity();std::size_t chosen=m;
        n=start;do{
            const auto& p=P(n);
            if(n!=m && p.x>=hp.x && p.x<=std::max(mp.x,hit.x) && !(p==hp)){
                const bool in=hp.y<mp.y?orient(hp,hit,p)>=0 && orient(hit,mp,p)>=0 && orient(mp,hp,p)>=0:orient(hp,mp,p)>=0 && orient(mp,hit,p)>=0 && orient(hit,hp,p)>=0;
                if(in && p.x>hp.x && locally_inside(n,h)){const double tan=std::fabs(hp.y-p.y)/(p.x-hp.x);if(tan<best_tan || (tan==best_tan && p.x<P(chosen).x)){best_tan=tan;chosen=n;}}
            }
            n=nodes[n].next;}while(n!=start);
        split(chosen,h);
    }
    // 耳切：凸頂點且三角形內沒有其他凹（或共線）頂點時切下；整圈無法切下時先移除重複／共線點，再強制切凸頂點。
    std::size_t ear=start,stop=start;unsigned pass=0;std::size_t remaining=0;{std::size_t n=start;do{++remaining;n=nodes[n].next;}while(n!=start);}
    const auto remove=[&](std::size_t n){nodes[nodes[n].prev].next=nodes[n].next;nodes[nodes[n].next].prev=nodes[n].prev;--remaining;};
    const auto is_ear=[&](std::size_t b,bool strict){
        const auto a=nodes[b].prev,c=nodes[b].next;if(orient(P(a),P(b),P(c))<=0)return false;if(!strict)return true;
        for(auto p=nodes[c].next;p!=a;p=nodes[p].next){
            const auto& q=P(p);if(q==P(a) || q==P(b) || q==P(c))continue;
            if(orient(P(a),P(b),q)>=0 && orient(P(b),P(c),q)>=0 && orient(P(c),P(a),q)>=0 && orient(P(nodes[p].prev),q,P(nodes[p].next))<=0)return false;
        }
        return true;
    };
    std::size_t guard=0;
    while(remaining>=3 && ++guard<4*nodes.size()*nodes.size()+16){
        const auto prev=nodes[ear].prev,next=nodes[ear].next;
        if(is_ear(ear,pass<2)){result.push_back({nodes[prev].id,nodes[ear].id,nodes[next].id});remove(ear);ear=nodes[next].next;stop=ear;pass=pass==2?1:pass;continue;}
        ear=next;
        if(ear!=stop)continue;
        if(pass==0){
            // 移除重複與共線點
            std::size_t n=ear;std::size_t visited=0;
            while(remaining>=3 && visited<=remaining){const auto& a=P(nodes[n].prev);const auto& b=P(n);const auto& c=P(nodes[n].next);
                if(b==c || orient(a,b,c)==0){const auto keep=nodes[n].prev;remove(n);n=keep;visited=0;continue;}n=nodes[n].next;++visited;}
            ear=stop=n;pass=1;
        }else if(pass==1){pass=2;}
        else break;
    }
    return result;
}
std::optional<Geometry> tessellate(const GluTessellator& t){
    Geometry g;
    // 法線
    auto normal=t.normal;const bool supplied=normal[0]!=0 || normal[1]!=0 || normal[2]!=0;
    if(!supplied){
        for(const auto& contour:t.contours)for(std::size_t i=0;i<contour.size();++i){
            const auto& a=contour[i].coords;const auto& b=contour[(i+1)%contour.size()].coords;
            normal[0]+=(a[1]-b[1])*(a[2]+b[2]);normal[1]+=(a[2]-b[2])*(a[0]+b[0]);normal[2]+=(a[0]-b[0])*(a[1]+b[1]);
        }
        if(normal==std::array<double,3>{}){
            // 淨面積為 0（如 8 字形）：取距第一點最遠的點，再取與兩者形成最大三角形的點。
            std::vector<std::array<double,3>> all;for(const auto& contour:t.contours)for(const auto& v:contour)all.push_back(v.coords);if(all.size()<3)return std::nullopt;
            const auto distance=[](const std::array<double,3>& a,const std::array<double,3>& b){double s=0;for(std::size_t k=0;k<3;++k)s+=(a[k]-b[k])*(a[k]-b[k]);return s;};
            std::size_t far=0;for(std::size_t i=1;i<all.size();++i)if(distance(all[i],all[0])>distance(all[far],all[0]))far=i;
            double best=0;for(const auto& c:all){const std::array<double,3> u{all[far][0]-all[0][0],all[far][1]-all[0][1],all[far][2]-all[0][2]},v{c[0]-all[0][0],c[1]-all[0][1],c[2]-all[0][2]};
                const std::array<double,3> n{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};const double s=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];if(s>best){best=s;normal=n;}}
            if(best==0)return std::nullopt;
        }
    }
    std::size_t axis=2;for(std::size_t k=0;k<3;++k)if(std::fabs(normal[k])>std::fabs(normal[axis]))axis=k;
    bool positive=normal[axis]>0;
    const auto project=[&](const std::array<double,3>& v)->Point{
        switch(axis){case 0:return positive?Point{v[1],v[2]}:Point{v[2],v[1]};case 1:return positive?Point{v[2],v[0]}:Point{v[0],v[2]};default:return positive?Point{v[0],v[1]}:Point{v[1],v[0]};}
    };
    if(!supplied){
        double area=0;for(const auto& contour:t.contours)for(std::size_t i=0;i<contour.size();++i){const auto a=project(contour[i].coords),b=project(contour[(i+1)%contour.size()].coords);area+=a.x*b.y-b.x*a.y;}
        if(area<0)positive=!positive;
    }
    // 頂點（相同投影座標合併）與邊
    std::map<std::pair<double,double>,std::size_t> index;
    const auto add=[&](const Point& p,const std::array<double,3>& xyz,std::uint64_t data){const auto [it,inserted]=index.try_emplace({p.x,p.y},g.vertices.size());if(inserted)g.vertices.push_back({p,xyz,data,false,{},{}});return it->second;};
    struct Segment {std::size_t a,b;std::vector<std::pair<double,std::size_t>> splits;};
    std::vector<Segment> segments;double scale=0;
    for(const auto& contour:t.contours){
        std::vector<std::size_t> ids;
        for(const auto& v:contour){const auto p=project(v.coords);scale=std::max({scale,std::fabs(p.x),std::fabs(p.y)});const auto id=add(p,v.coords,v.data);if(ids.empty() || ids.back()!=id)ids.push_back(id);}
        while(ids.size()>1 && ids.front()==ids.back())ids.pop_back();
        if(ids.size()<2)continue;
        for(std::size_t i=0;i<ids.size();++i)segments.push_back({ids[i],ids[(i+1)%ids.size()],{}});
    }
    if(segments.empty())return std::nullopt;
    // 交點與接觸點：依最小 x 排序後只比較 x 範圍重疊的邊。
    const double snap=scale*1e-12;
    std::vector<std::size_t> order(segments.size());std::iota(order.begin(),order.end(),0);
    const auto min_x=[&](const Segment& s){return std::min(g.vertices[s.a].p.x,g.vertices[s.b].p.x);};const auto max_x=[&](const Segment& s){return std::max(g.vertices[s.a].p.x,g.vertices[s.b].p.x);};
    std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return min_x(segments[a])<min_x(segments[b]);});
    const auto split_at=[&](Segment& s,std::size_t v){
        if(v==s.a || v==s.b)return;const auto& a=g.vertices[s.a].p;const auto& b=g.vertices[s.b].p;const auto& p=g.vertices[v].p;
        const double dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;if(length==0)return;const double u=((p.x-a.x)*dx+(p.y-a.y)*dy)/length;if(u>0 && u<1)s.splits.push_back({u,v});
    };
    for(std::size_t oi=0;oi<order.size();++oi){
        const auto i=order[oi];const auto xi=max_x(segments[i]);
        for(std::size_t oj=oi+1;oj<order.size() && min_x(segments[order[oj]])<=xi;++oj){
            const auto j=order[oj];auto& A=segments[i];auto& B=segments[j];
            const auto pa=g.vertices[A.a].p,pb=g.vertices[A.b].p,qa=g.vertices[B.a].p,qb=g.vertices[B.b].p;
            if(std::max(pa.y,pb.y)<std::min(qa.y,qb.y) || std::max(qa.y,qb.y)<std::min(pa.y,pb.y))continue;
            if((A.a==B.a && A.b==B.b) || (A.a==B.b && A.b==B.a))continue;
            const double d1=orient(pa,pb,qa),d2=orient(pa,pb,qb),d3=orient(qa,qb,pa),d4=orient(qa,qb,pb);
            if(d1==0)split_at(A,B.a);if(d2==0)split_at(A,B.b);if(d3==0)split_at(B,A.a);if(d4==0)split_at(B,A.b);
            if(!(((d1>0 && d2<0) || (d1<0 && d2>0)) && ((d3>0 && d4<0) || (d3<0 && d4>0))))continue;
            const double ta=d3/(d3-d4),tb=d1/(d1-d2);const Point p{pa.x+ta*(pb.x-pa.x),pa.y+ta*(pb.y-pa.y)};
            // 交點與端點幾乎重合時視為接觸，避免為浮點誤差產生新頂點。
            std::size_t endpoint=none;for(const auto e:{A.a,A.b,B.a,B.b}){const auto& q=g.vertices[e].p;if(std::fabs(q.x-p.x)<=snap && std::fabs(q.y-p.y)<=snap){endpoint=e;break;}}
            if(endpoint!=none){split_at(A,endpoint);split_at(B,endpoint);continue;}
            const auto& va=g.vertices[A.a];const auto& vb=g.vertices[A.b];const auto& vc=g.vertices[B.a];const auto& vd=g.vertices[B.b];
            std::array<double,3> xyz{};for(std::size_t k=0;k<3;++k)xyz[k]=((va.xyz[k]+ta*(vb.xyz[k]-va.xyz[k]))+(vc.xyz[k]+tb*(vd.xyz[k]-vc.xyz[k])))/2;
            const auto sources=std::array<std::uint64_t,4>{va.data,vb.data,vc.data,vd.data};
            const auto weights=std::array<float,4>{static_cast<float>(0.5*(1-ta)),static_cast<float>(0.5*ta),static_cast<float>(0.5*(1-tb)),static_cast<float>(0.5*tb)};
            const auto [it,inserted]=index.try_emplace({p.x,p.y},g.vertices.size());
            if(inserted)g.vertices.push_back({p,xyz,0,true,sources,weights});
            A.splits.push_back({ta,it->second});B.splits.push_back({tb,it->second});
        }
    }
    // 切開後的邊依方向累計環繞增量；增量為 0 的邊不影響任何面而移除。
    std::map<std::pair<std::size_t,std::size_t>,long> edges;
    const auto add_edge=[&](std::size_t u,std::size_t v){if(u<v)++edges[{u,v}];else --edges[{v,u}];};
    for(auto& s:segments){
        std::sort(s.splits.begin(),s.splits.end());std::size_t from=s.a;
        for(const auto& [t_,v]:s.splits){(void)t_;if(v==from)continue;add_edge(from,v);from=v;}
        if(from!=s.b)add_edge(from,s.b);
    }
    struct Half {std::size_t from,to,twin;long delta;std::size_t cycle{none};};
    std::vector<Half> halves;std::vector<std::vector<std::size_t>> outgoing(g.vertices.size());
    for(const auto& [key,delta]:edges){if(!delta)continue;const auto h=halves.size();halves.push_back({key.first,key.second,h+1,delta});halves.push_back({key.second,key.first,h,-delta});outgoing[key.first].push_back(h);outgoing[key.second].push_back(h+1);}
    if(halves.empty())return std::nullopt;
    std::vector<std::size_t> position(halves.size());
    for(auto& list:outgoing){
        std::sort(list.begin(),list.end(),[&](std::size_t a,std::size_t b){const auto& o=g.vertices[halves[a].from].p;const auto& pa=g.vertices[halves[a].to].p;const auto& pb=g.vertices[halves[b].to].p;return std::atan2(pa.y-o.y,pa.x-o.x)<std::atan2(pb.y-o.y,pb.x-o.x);});
        for(std::size_t k=0;k<list.size();++k)position[list[k]]=k;
    }
    // next(h)：在 h 終點，從 twin(h) 往順時針方向的下一條出邊，沿左側的面前進。
    const auto next=[&](std::size_t h){const auto& list=outgoing[halves[h].to];const auto k=position[halves[h].twin];return list[(k+list.size()-1)%list.size()];};
    std::vector<std::vector<std::size_t>> cycles;std::vector<double> areas;
    for(std::size_t h=0;h<halves.size();++h){
        if(halves[h].cycle!=none)continue;std::vector<std::size_t> cycle;double area=0;auto e=h;
        do{halves[e].cycle=cycles.size();cycle.push_back(e);const auto& a=g.vertices[halves[e].from].p;const auto& b=g.vertices[halves[e].to].p;area+=a.x*b.y-b.x*a.y;e=next(e);}while(e!=h && cycle.size()<=halves.size());
        cycles.push_back(std::move(cycle));areas.push_back(area/2);
    }
    // 連通元件與其外圈（面積最小者）
    std::vector<std::size_t> parent(g.vertices.size());std::iota(parent.begin(),parent.end(),0);
    const auto find=[&](std::size_t v){while(parent[v]!=v)v=parent[v]=parent[parent[v]];return v;};
    for(const auto& h:halves)parent[find(h.from)]=find(h.to);
    std::map<std::size_t,std::size_t> outer_of;
    for(std::size_t c=0;c<cycles.size();++c){const auto root=find(halves[cycles[c].front()].from);const auto [it,inserted]=outer_of.try_emplace(root,c);if(!inserted && areas[c]<areas[it->second])it->second=c;}
    // 各元件外圈的環繞數＝其他元件的邊在該點的環繞數；元件內沿邊傳遞（左面－右面＝增量）。
    std::vector<long> winding(cycles.size(),0);std::vector<bool> known(cycles.size(),false);
    for(const auto& [root,outer]:outer_of){
        const auto& p=g.vertices[halves[cycles[outer].front()].from].p;long w=0;
        for(std::size_t h=0;h<halves.size();h+=2){
            if(find(halves[h].from)==root)continue;const auto& u=g.vertices[halves[h].from].p;const auto& v=g.vertices[halves[h].to].p;const auto d=halves[h].delta;
            if(u.y<=p.y){if(v.y>p.y && orient(u,v,p)>0)w+=d;}else if(v.y<=p.y && orient(u,v,p)<0)w-=d;
        }
        winding[outer]=w;known[outer]=true;std::vector<std::size_t> queue{outer};
        while(!queue.empty()){
            const auto c=queue.back();queue.pop_back();
            for(const auto h:cycles[c]){const auto other=halves[halves[h].twin].cycle;if(known[other])continue;winding[other]=winding[c]-halves[h].delta;known[other]=true;queue.push_back(other);}
        }
    }
    std::vector<bool> inside(cycles.size());for(std::size_t c=0;c<cycles.size();++c)inside[c]=areas[c]>0 && inside_rule(t.winding,winding[c]);
    // 外圈為負面積的元件是包含它的面的洞；外圈本身不是面。負面積外圈的「內部」取包含它的面。
    std::map<std::size_t,std::vector<std::size_t>> holes;
    const auto contains=[&](std::size_t cycle,const Point& p){
        bool in=false;for(const auto h:cycles[cycle]){const auto& a=g.vertices[halves[h].from].p;const auto& b=g.vertices[halves[h].to].p;if((a.y>p.y)!=(b.y>p.y) && p.x<a.x+(p.y-a.y)*(b.x-a.x)/(b.y-a.y))in=!in;}return in;
    };
    for(const auto& [root,outer]:outer_of){
        const auto& p=g.vertices[halves[cycles[outer].front()].from].p;std::size_t best=none;
        for(std::size_t c=0;c<cycles.size();++c)if(areas[c]>0 && find(halves[cycles[c].front()].from)!=root && contains(c,p) && (best==none || areas[c]<areas[best]))best=c;
        if(best!=none){holes[best].push_back(outer);inside[outer]=inside[best];}
    }
    const auto boundary_half=[&](std::size_t h){return inside[halves[h].cycle] && !inside[halves[halves[h].twin].cycle];};
    if(t.boundary_only){
        std::vector<bool> used(halves.size(),false);
        for(std::size_t h=0;h<halves.size();++h){
            if(used[h] || !boundary_half(h))continue;std::vector<std::size_t> loop;auto e=h;std::size_t guard=0;
            do{used[e]=true;loop.push_back(halves[e].from);auto n=next(e);std::size_t spin=0;while(!boundary_half(n) && ++spin<=halves.size())n=next(halves[n].twin);e=n;}while(e!=h && ++guard<=halves.size());
            if(loop.size()>=2)g.loops.push_back(std::move(loop));
        }
        return g;
    }
    std::map<std::pair<std::size_t,std::size_t>,std::size_t> half_index;for(std::size_t h=0;h<halves.size();++h)half_index[{halves[h].from,halves[h].to}]=h;
    for(std::size_t c=0;c<cycles.size();++c){
        if(areas[c]<=0 || !inside[c])continue;
        std::vector<std::size_t> outer;for(const auto h:cycles[c])outer.push_back(halves[h].from);
        std::vector<std::vector<std::size_t>> rings;if(const auto it=holes.find(c);it!=holes.end())for(const auto hole:it->second){std::vector<std::size_t> ring;for(const auto h:cycles[hole])ring.push_back(halves[h].from);rings.push_back(std::move(ring));}
        for(const auto& triangle:triangulate(g.vertices,outer,rings)){
            std::array<bool,3> flags{};
            for(std::size_t k=0;k<3;++k){const auto it=half_index.find({triangle[k],triangle[(k+1)%3]});flags[k]=it!=half_index.end() && !inside[halves[halves[it->second].twin].cycle];}
            g.triangles.push_back(triangle);g.boundary.push_back(flags);
        }
    }
    return g;
}
}
std::optional<GluTriangulation> glu_triangulate(const GluTessellator& t){
    const auto g=tessellate(t);if(!g)return std::nullopt;GluTriangulation r;
    for(const auto& v:g->vertices)r.positions.push_back(v.xyz);r.triangles=g->triangles;r.loops=g->loops;return r;
}
std::optional<std::uint32_t> WindowsServices::invoke_glu_tess(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto real=[&](unsigned i)->double{
        if(memory.execution_mode()==ExecutionMode::X86 || i>=4)return std::bit_cast<double>(arg(i));
        std::uint64_t value=0;for(unsigned k=0;k<8;++k)value|=std::uint64_t(state.xmm[i][k])<<(k*8);return std::bit_cast<double>(value);
    };
    if(name=="gluNewTess"){const auto block=allocate_heap(memory,32);if(!block)return finish(0);opengl_.glu_tessellators[block]=GluTessellator{};return finish(block);}
    const auto found=opengl_.glu_tessellators.find(arg(0));if(found==opengl_.glu_tessellators.end())return finish(0);auto& t=found->second;
    if(name=="gluDeleteTess"){opengl_.glu_tessellators.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
    std::vector<std::pair<std::uint32_t,std::uint64_t>> errors;std::optional<GluTessellator> render;
    const auto fail=[&](std::uint32_t code){errors.push_back({code,t.polygon_data});};
    // 依 GLU 規格補上缺少的 begin（往前）或捨棄未結束的部分（往後），並回報對應錯誤。
    const auto go=[&](std::uint32_t target){
        while(t.state<target){if(t.state==0){fail(100151);t.state=1;t.polygon_data=0;t.contours.clear();}else{fail(100152);t.contours.emplace_back();t.state=2;}}
        while(t.state>target){if(t.state==2){fail(100154);t.state=1;}else{fail(100153);t.state=0;t.contours.clear();}}
    };
    const auto end_polygon=[&](){render=t;t.state=0;t.contours.clear();};
    if(name=="gluTessCallback"){const auto which=d(1);if(which<tess_begin || which>tess_combine+6)fail(glu_invalid_enum);else t.callbacks[which-tess_begin]=arg(2);}
    else if(name=="gluTessProperty"){
        const auto which=d(1);const double value=real(2);
        if(which==100140){const auto rule=static_cast<std::uint32_t>(value);if(static_cast<double>(rule)!=value || rule<100130 || rule>100134)fail(glu_invalid_value);else t.winding=rule;}
        else if(which==100141)t.boundary_only=value!=0;
        else if(which==100142){if(value<0 || value>1)fail(glu_invalid_value);else t.tolerance=value;}
        else fail(glu_invalid_enum);
    }else if(name=="gluGetTessProperty"){
        const auto which=d(1);double value=0;
        if(which==100140)value=t.winding;else if(which==100141)value=t.boundary_only?1:0;else if(which==100142)value=t.tolerance;else fail(glu_invalid_enum);
        if(arg(2))memory.store(GuestAddress{arg(2)},64,std::bit_cast<std::uint64_t>(value));
    }else if(name=="gluTessNormal")t.normal={real(1),real(2),real(3)};
    else if(name=="gluTessBeginPolygon"){go(0);t.state=1;t.polygon_data=arg(1);t.contours.clear();}
    else if(name=="gluTessBeginContour"){go(1);t.contours.emplace_back();t.state=2;}
    else if(name=="gluTessVertex"){
        go(2);std::array<double,3> coords{};bool clamped=false;
        if(arg(1))for(unsigned k=0;k<3;++k){auto v=std::bit_cast<double>(memory.load(GuestAddress{arg(1)+k*8ULL},64));if(!(std::fabs(v)<=1e150)){v=std::isnan(v)?0:std::copysign(1e150,v);clamped=true;}coords[k]=v;}
        if(clamped)fail(100155);
        if(t.contours.back().size()<1U<<20)t.contours.back().push_back({coords,arg(2)});
    }else if(name=="gluTessEndContour"){go(2);t.state=1;}
    else if(name=="gluTessEndPolygon"){go(1);end_polygon();}
    else if(name=="gluBeginPolygon"){go(0);t.state=1;t.polygon_data=0;t.contours.clear();t.contours.emplace_back();t.state=2;}
    else if(name=="gluNextContour"){go(2);t.contours.emplace_back();}
    else if(name=="gluEndPolygon"){go(2);t.state=1;end_polygon();}
    if(errors.empty() && !render)return finish(0);
    const auto callbacks=t.callbacks;
    return nested_continuation(state,memory,"GLU tessellator",[this,service,errors,callbacks,render](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        // 捕獲值複製到協程框架，回呼期間不依賴 closure 的生命週期。
        const auto descriptor=service;const auto pending=errors;const auto table_at_call=callbacks;auto job=render;
        const bool x86=m.execution_mode()==ExecutionMode::X86;const unsigned pointer=x86?4:8;
        const auto pick=[](const std::array<std::uint64_t,12>& table,std::uint32_t which){const auto data=table[which-tess_begin+6];return std::pair<std::uint64_t,bool>{data?data:table[which-tess_begin],data!=0};};
        const auto error_pick=pick(table_at_call,tess_error);
        for(const auto& pair:pending){
            if(!error_pick.first)continue;
            if(error_pick.second)(void)co_await service_callback::call_value(error_pick.first,{pair.first,pair.second});else (void)co_await service_callback::call_value(error_pick.first,{pair.first});
        }
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto b:blocks)(void)self.free_heap(m,b,s);}} release{*this,m,s,blocks};
        if(job){
            const auto table=job->callbacks;const auto polygon=job->polygon_data;
            auto geometry=tessellate(*job);
            const auto error_entry=pick(table,tess_error);const auto render_error=error_entry.first;const bool render_error_data=error_entry.second;
            const auto report=[render_error_data,polygon](std::uint32_t code)->std::vector<std::uint64_t>{return render_error_data?std::vector<std::uint64_t>{code,polygon}:std::vector<std::uint64_t>{code};};
            bool ok=geometry.has_value();
            if(ok){
                const auto combine_entry=pick(table,tess_combine);const auto combine_fn=combine_entry.first;const bool combine_data=combine_entry.second;
                for(auto& v:geometry->vertices){
                    if(!v.intersection)continue;
                    if(!combine_fn){if(render_error)(void)co_await service_callback::call_value(render_error,report(100156));ok=false;break;}
                    if(blocks.empty()){for(const std::uint64_t size:{24U,4U*pointer,16U,pointer}){const auto b=allocate_heap(m,size);if(!b)throw std::bad_alloc{};blocks.push_back(b);}}
                    for(unsigned k=0;k<3;++k)m.store(GuestAddress{blocks[0]+k*8ULL},64,std::bit_cast<std::uint64_t>(v.xyz[k]));
                    for(unsigned k=0;k<4;++k){m.store(GuestAddress{blocks[1]+k*pointer},static_cast<std::uint8_t>(pointer*8),v.sources[k]);m.store(GuestAddress{blocks[2]+k*4ULL},32,std::bit_cast<std::uint32_t>(v.weights[k]));}
                    m.store(GuestAddress{blocks[3]},static_cast<std::uint8_t>(pointer*8),0);
                    if(combine_data)(void)co_await service_callback::call_value(combine_fn,{blocks[0],blocks[1],blocks[2],blocks[3],polygon});
                    else (void)co_await service_callback::call_value(combine_fn,{blocks[0],blocks[1],blocks[2],blocks[3]});
                    v.data=m.load(GuestAddress{blocks[3]},static_cast<std::uint8_t>(pointer*8));
                    if(!v.data){if(render_error)(void)co_await service_callback::call_value(render_error,report(100156));ok=false;break;}
                }
            }
            if(ok){
                const auto begin_entry=pick(table,tess_begin),vertex_entry=pick(table,tess_vertex),end_entry=pick(table,tess_end),flag_entry=pick(table,tess_edge_flag);
                const auto begin_fn=begin_entry.first,vertex_fn=vertex_entry.first,end_fn=end_entry.first,flag_fn=flag_entry.first;
                const bool begin_data=begin_entry.second,vertex_data=vertex_entry.second,end_data=end_entry.second,flag_data=flag_entry.second;
                const auto with=[polygon](bool data,std::vector<std::uint64_t> args){if(data)args.push_back(polygon);return args;};
                const auto emit_group=[with,begin_data](std::uint32_t type){return with(begin_data,{type});};
                if(job->boundary_only){
                    for(const auto& loop:geometry->loops){
                        if(begin_fn)(void)co_await service_callback::call_value(begin_fn,emit_group(2)); // GL_LINE_LOOP
                        for(const auto v:loop)if(vertex_fn)(void)co_await service_callback::call_value(vertex_fn,with(vertex_data,{geometry->vertices[v].data}));
                        if(end_fn)(void)co_await service_callback::call_value(end_fn,with(end_data,{}));
                    }
                }else if(!geometry->triangles.empty()){
                    if(begin_fn)(void)co_await service_callback::call_value(begin_fn,emit_group(4)); // GL_TRIANGLES
                    int last=-1;
                    for(std::size_t i=0;i<geometry->triangles.size();++i)for(std::size_t k=0;k<3;++k){
                        if(flag_fn){const int flag=geometry->boundary[i][k]?1:0;if(flag!=last){(void)co_await service_callback::call_value(flag_fn,with(flag_data,{static_cast<std::uint64_t>(flag)}));last=flag;}}
                        if(vertex_fn)(void)co_await service_callback::call_value(vertex_fn,with(vertex_data,{geometry->vertices[geometry->triangles[i][k]].data}));
                    }
                    if(end_fn)(void)co_await service_callback::call_value(end_fn,with(end_data,{}));
                }
            }
        }
        const WindowsCall call(s,m,descriptor);call.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
}
}
