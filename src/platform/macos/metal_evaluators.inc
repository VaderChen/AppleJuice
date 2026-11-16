// Metal 後端的 OpenGL 求值器（opengl_metal.mm 引入）。glMap 的控制點在呼叫當下複製；EvalCoord 依目前啟用的 map 以 de Casteljau 求 Bézier 值，
// 產生的顏色、法線與貼圖座標只用於該頂點，不更新目前值。GL_AUTO_NORMAL 開啟時，2D 頂點 map 以兩個偏導數的外積（正規化）作為法線。
// EvalMesh 展開成 Begin／EvalCoord／End；display list 只記錄 EvalMesh 本身，展開的命令不再錄製。
namespace {
using EvalVector=std::array<double,4>;
// 一條 Bézier 曲線在參數 t（已換算成 0～1）的值與對 t 的導數。
std::pair<EvalVector,EvalVector> eval_bezier(std::vector<EvalVector> points,double t){
    const auto order=points.size();if(order<=1)return {points.empty()?EvalVector{}:points[0],EvalVector{}};
    while(points.size()>2){for(std::size_t i=0;i+1<points.size();++i)for(unsigned c=0;c<4;++c)points[i][c]=(1-t)*points[i][c]+t*points[i+1][c];points.pop_back();}
    EvalVector value{},slope{};
    for(unsigned c=0;c<4;++c){value[c]=(1-t)*points[0][c]+t*points[1][c];slope[c]=static_cast<double>(order-1)*(points[1][c]-points[0][c]);}
    return {value,slope};
}
double eval_grid(double count,double first,double last,std::int32_t i){return i==static_cast<std::int32_t>(count)?last:first+(last-first)*i/count;}
}
const MetalGl::EvalMap& MetalGl::eval_map(unsigned target){
    if(const auto it=eval_maps_.find(target);it!=eval_maps_.end())return it->second;
    // 未定義的 map：定義域 [0,1]、一階，唯一的控制點是該屬性的預設值。
    EvalMap map;map.points.resize(gl_map_components(target));
    switch(target&0xf){case 0:map.points={1,1,1,1};break;case 1:map.points={1};break;case 2:map.points={0,0,1};break;case 6:map.points={0,0,0,1};break;case 8:map.points={0,0,0,1};break;default:break;}
    return eval_maps_.emplace(target,std::move(map)).first->second;
}
// 回傳 map 在 (u,v) 的值；du／dv 非空時一併回傳偏導數（已除以定義域長度）。
EvalVector MetalGl::eval_value(unsigned target,double u,double v,EvalVector* du,EvalVector* dv){
    const auto& m=eval_map(target);const auto k=gl_map_components(target);const bool two=target>=0xdb0;
    const auto point=[&](std::size_t i,std::size_t j){EvalVector p{0,0,0,1};for(unsigned c=0;c<k;++c)p[c]=m.points[(i*m.vorder+j)*k+c];return p;};
    const double s=(u-m.u1)/(m.u2-m.u1);
    if(!two){std::vector<EvalVector> curve;for(std::size_t i=0;i<m.uorder;++i)curve.push_back(point(i,0));const auto [value,slope]=eval_bezier(std::move(curve),s);
        if(du)for(unsigned c=0;c<4;++c)(*du)[c]=slope[c]/(m.u2-m.u1);return value;}
    const double t=(v-m.v1)/(m.v2-m.v1);
    // 先沿 v 對每一列求值與 v 方向導數，再沿 u 合成。
    std::vector<EvalVector> rows,row_slopes;
    for(std::size_t i=0;i<m.uorder;++i){std::vector<EvalVector> column;for(std::size_t j=0;j<m.vorder;++j)column.push_back(point(i,j));auto [value,slope]=eval_bezier(std::move(column),t);rows.push_back(value);row_slopes.push_back(slope);}
    const auto [value,slope]=eval_bezier(rows,s);
    if(du)for(unsigned c=0;c<4;++c)(*du)[c]=slope[c]/(m.u2-m.u1);
    if(dv){const auto across=eval_bezier(std::move(row_slopes),s).first;for(unsigned c=0;c<4;++c)(*dv)[c]=across[c]/(m.v2-m.v1);}
    return value;
}
void MetalGl::eval_vertex(double u,double v,bool two){
    const unsigned base=two?0xdb0U:0xd90U;const auto enabled=[&](unsigned offset){return state_.enabled.contains(base+offset);};
    const unsigned vertex_map=enabled(8)?8U:enabled(7)?7U:0U;
    if(!vertex_map)return;
    const auto saved_color=state_.color,saved_normal=state_.normal,saved_texcoord=state_.texcoord;
    const auto to_vec=[](const EvalVector& value){return Vec{static_cast<float>(value[0]),static_cast<float>(value[1]),static_cast<float>(value[2]),static_cast<float>(value[3])};};
    if(enabled(0))state_.color=to_vec(eval_value(base,u,v,nullptr,nullptr));
    // 貼圖座標取已啟用的最高維度 map；控制點未提供的分量固定為 0、0、1，求值後仍是這些值。
    for(const unsigned offset:{6U,5U,4U,3U})if(enabled(offset)){state_.texcoord=to_vec(eval_value(base+offset,u,v,nullptr,nullptr));break;}
    EvalVector du{},dv{};const auto position=eval_value(base+vertex_map,u,v,two?&du:nullptr,two?&dv:nullptr);
    if(two && state_.enabled.contains(0xd80)){
        // 解析法線：VERTEX_4 先換算成 xyz/w 的導數（商的導數）。
        std::array<double,3> pu{},pv{};const double w=vertex_map==8?position[3]:1;
        for(unsigned c=0;c<3;++c){pu[c]=vertex_map==8?(du[c]*w-position[c]*du[3])/(w*w):du[c];pv[c]=vertex_map==8?(dv[c]*w-position[c]*dv[3])/(w*w):dv[c];}
        std::array<double,3> normal{pu[1]*pv[2]-pu[2]*pv[1],pu[2]*pv[0]-pu[0]*pv[2],pu[0]*pv[1]-pu[1]*pv[0]};
        const double length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);if(length>0)for(auto& c:normal)c/=length;
        state_.normal={static_cast<float>(normal[0]),static_cast<float>(normal[1]),static_cast<float>(normal[2]),0};
    }else if(enabled(2)){const auto value=eval_value(base+2,u,v,nullptr,nullptr);state_.normal={static_cast<float>(value[0]),static_cast<float>(value[1]),static_cast<float>(value[2]),0};}
    auto p=to_vec(position);if(vertex_map==7)p[3]=1;
    vertex(p);
    state_.color=saved_color;state_.normal=saved_normal;state_.texcoord=saved_texcoord;
}
std::uint64_t MetalGl::evaluator(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};const auto n=[&](unsigned i){return std::bit_cast<std::int32_t>(u(i));};const auto r=[&](unsigned i){return std::bit_cast<double>(a[i]);};
    // EvalMesh 的展開以重播身分執行，不重複錄進 display list。
    const auto expand=[&](auto&& body){const bool saved=replaying_;replaying_=true;try{body();}catch(...){replaying_=saved;throw;}replaying_=saved;};
    const auto primitive=[&](unsigned mode,auto&& body){const std::array<std::uint64_t,1> begin{mode};execute(GlOperation::Begin,begin,{},{});body();execute(GlOperation::End,{},{},{});};
    switch(op){
    case GlOperation::Map1d:case GlOperation::Map2d:{
        const auto target=u(0);const auto k=gl_map_components(target);const bool two=op==GlOperation::Map2d;
        if(!k || two!=(target>=0xdb0)){error(0x500);return 0;}
        const auto uorder=u(3),vorder=two?u(6):1U;
        if(uorder<1 || uorder>gl_max_eval_order || vorder<1 || vorder>gl_max_eval_order || r(1)==r(2) || (two && r(4)==r(5))){error(0x501);return 0;}
        const auto count=static_cast<std::size_t>(uorder)*vorder*k;if(data.size()<count*8){error(0x501);return 0;}
        EvalMap map;map.u1=r(1);map.u2=r(2);map.uorder=uorder;if(two){map.v1=r(4);map.v2=r(5);map.vorder=vorder;}
        map.points.resize(count);std::memcpy(map.points.data(),data.data(),count*8);eval_maps_[target]=std::move(map);return 0;
    }
    case GlOperation::MapGrid1d:if(n(0)<=0){error(0x501);return 0;}state_.grid1={static_cast<double>(n(0)),r(1),r(2)};return 0;
    case GlOperation::MapGrid2d:if(n(0)<=0 || n(3)<=0){error(0x501);return 0;}state_.grid2={static_cast<double>(n(0)),r(1),r(2),static_cast<double>(n(3)),r(4),r(5)};return 0;
    case GlOperation::EvalCoord1d:eval_vertex(r(0),0,false);return 0;
    case GlOperation::EvalCoord2d:eval_vertex(r(0),r(1),true);return 0;
    case GlOperation::EvalPoint1:{const auto& g=state_.grid1;eval_vertex(eval_grid(g[0],g[1],g[2],n(0)),0,false);return 0;}
    case GlOperation::EvalPoint2:{const auto& g=state_.grid2;eval_vertex(eval_grid(g[0],g[1],g[2],n(0)),eval_grid(g[3],g[4],g[5],n(1)),true);return 0;}
    case GlOperation::EvalMesh1:{
        // GL_POINT 0x1B00 → GL_POINTS；GL_LINE 0x1B01 → GL_LINE_STRIP。
        const auto mode=u(0);if(mode!=0x1b00 && mode!=0x1b01){error(0x500);return 0;}const auto g=state_.grid1;
        expand([&]{primitive(mode==0x1b00?0U:3U,[&]{for(auto i=n(1);i<=n(2);++i)eval_vertex(eval_grid(g[0],g[1],g[2],i),0,false);});});return 0;
    }
    case GlOperation::EvalMesh2:{
        // (mode,i1,i2,j1,j2)：i 為 u 格線、j 為 v 格線。FILL 每一列 v 以 QUAD_STRIP 依序送出 (u_i,v_j)、(u_i,v_j+1)；LINE 先畫等 v 線再畫等 u 線。
        const auto mode=u(0);if(mode!=0x1b00 && mode!=0x1b01 && mode!=0x1b02){error(0x500);return 0;}const auto g=state_.grid2;
        const auto at=[&](std::int32_t i,std::int32_t j){eval_vertex(eval_grid(g[0],g[1],g[2],i),eval_grid(g[3],g[4],g[5],j),true);};
        const auto i1=n(1),i2=n(2),j1=n(3),j2=n(4);
        expand([&]{
            if(mode==0x1b02){for(auto j=j1;j<j2;++j)primitive(8,[&]{for(auto i=i1;i<=i2;++i){at(i,j);at(i,j+1);}});}
            else if(mode==0x1b01){for(auto j=j1;j<=j2;++j)primitive(3,[&]{for(auto i=i1;i<=i2;++i)at(i,j);});for(auto i=i1;i<=i2;++i)primitive(3,[&]{for(auto j=j1;j<=j2;++j)at(i,j);});}
            else primitive(0,[&]{for(auto j=j1;j<=j2;++j)for(auto i=i1;i<=i2;++i)at(i,j);});
        });
        return 0;
    }
    case GlOperation::GetMapdv:{
        // GL_COEFF 0x0A00、GL_ORDER 0x0A01、GL_DOMAIN 0x0A02。
        const auto target=u(0),query=u(1);if(!gl_map_components(target) || query<0xa00 || query>0xa02){error(0x500);return 0;}
        const auto& m=eval_map(target);const bool two=target>=0xdb0;std::vector<double> values;
        if(query==0xa00)values=m.points;
        else if(query==0xa01){values={static_cast<double>(m.uorder)};if(two)values.push_back(m.vorder);}
        else {values={m.u1,m.u2};if(two){values.push_back(m.v1);values.push_back(m.v2);}}
        const auto count=std::min(values.size(),output.size()/8);if(count)std::memcpy(output.data(),values.data(),count*8);return count;
    }
    default:return 0;
    }
}
