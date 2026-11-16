// Metal 後端的選取與回饋模式（opengl_metal.mm 引入）。GL_SELECT／GL_FEEDBACK 下不繪製：End 時把圖元在 CPU 端換到剪裁座標，
// 以六個剪裁面裁切（多邊形為 Sutherland–Hodgman、線段為參數裁切），裁切後依視窗座標的有號面積套用剔除。
// 選取模式更新命中旗標與視窗 z 範圍，名稱堆疊改變或離開模式時寫入命中紀錄；回饋模式依 glFeedbackBuffer 的型別輸出記號與頂點
// （視窗 x、y、z、剪裁 w、光照後顏色、經貼圖矩陣的貼圖座標）。結果在 RenderMode 離開時以「字數＋內容」交給服務層寫回客體。
namespace {
MetalGl::FeedbackVertex feedback_mix(const MetalGl::FeedbackVertex& a,const MetalGl::FeedbackVertex& b,double t){
    MetalGl::FeedbackVertex r;
    for(unsigned c=0;c<4;++c){r.clip[c]=a.clip[c]+(b.clip[c]-a.clip[c])*t;r.eye[c]=a.eye[c]+(b.eye[c]-a.eye[c])*t;r.color[c]=static_cast<float>(a.color[c]+(b.color[c]-a.color[c])*t);r.texcoord[c]=static_cast<float>(a.texcoord[c]+(b.texcoord[c]-a.texcoord[c])*t);}
    return r;
}
// 第 plane 個剪裁面的有號距離（≥0 在內）：前 6 個為 x+w、w−x、y+w、w−y、z+w、w−z，之後是已啟用的使用者剪裁面（眼座標）。
using UserClipPlanes=std::vector<std::array<double,4>>;
double clip_distance(const MetalGl::FeedbackVertex& v,unsigned plane,const UserClipPlanes& user){
    if(plane>=6){const auto& p=user[plane-6];return p[0]*v.eye[0]+p[1]*v.eye[1]+p[2]*v.eye[2]+p[3]*v.eye[3];}
    const double value=v.clip[plane/2];return plane%2?v.clip[3]-value:v.clip[3]+value;
}
std::vector<MetalGl::FeedbackVertex> clip_feedback_polygon(std::vector<MetalGl::FeedbackVertex> polygon,const UserClipPlanes& user){
    for(unsigned plane=0;plane<6+user.size() && !polygon.empty();++plane){
        std::vector<MetalGl::FeedbackVertex> kept;
        for(std::size_t i=0;i<polygon.size();++i){
            const auto& a=polygon[i];const auto& b=polygon[(i+1)%polygon.size()];const double da=clip_distance(a,plane,user),db=clip_distance(b,plane,user);
            if(da>=0)kept.push_back(a);
            if((da>=0)!=(db>=0))kept.push_back(feedback_mix(a,b,da/(da-db)));
        }
        polygon=std::move(kept);
    }
    return polygon;
}
bool clip_feedback_line(MetalGl::FeedbackVertex& a,MetalGl::FeedbackVertex& b,const UserClipPlanes& user){
    double t0=0,t1=1;
    for(unsigned plane=0;plane<6+user.size();++plane){
        const double da=clip_distance(a,plane,user),db=clip_distance(b,plane,user);
        if(da<0 && db<0)return false;
        if(da<0)t0=std::max(t0,da/(da-db));else if(db<0)t1=std::min(t1,da/(da-db));
    }
    if(t0>t1)return false;
    const auto start=feedback_mix(a,b,t0),end=feedback_mix(a,b,t1);a=start;b=end;return true;
}
}
std::array<double,4> MetalGl::window_coordinates(const std::array<double,4>& c)const{
    const auto& v=state_.viewport;const auto& range=state_.depth_range;const double w=c[3]!=0?c[3]:1;
    return {(c[0]/w+1)*v[2]/2+v[0],(c[1]/w+1)*v[3]/2+v[1],(range[1]-range[0])/2*(c[2]/w)+(range[0]+range[1])/2,c[3]};
}
void MetalGl::select_hit(double z){
    z=std::clamp(z,0.0,1.0);
    if(!select_.hit){select_.hit=true;select_.min_z=select_.max_z=z;}else{select_.min_z=std::min(select_.min_z,z);select_.max_z=std::max(select_.max_z,z);}
}
void MetalGl::write_hit_record(){
    if(!select_.hit)return;
    // 命中紀錄：名稱數、最小 z、最大 z（[0,1] 乘上 2³²−1）、堆疊中的名稱（底端在前）；放不下時設溢位，RenderMode 回傳 -1。
    const auto scale=[](double z){return static_cast<std::uint32_t>(std::llround(z*4294967295.0));};
    std::vector<std::uint32_t> record{static_cast<std::uint32_t>(select_.names.size()),scale(select_.min_z),scale(select_.max_z)};record.insert(record.end(),select_.names.begin(),select_.names.end());
    for(const auto word:record){if(select_.records.size()>=select_.size){select_.overflow=true;break;}select_.records.push_back(word);}
    ++select_.hits;select_.hit=false;
}
void MetalGl::feedback_values(std::initializer_list<double> values){
    for(const double value:values){if(feedback_.values.size()>=feedback_.size){feedback_.overflow=true;return;}feedback_.values.push_back(static_cast<float>(value));}
}
void MetalGl::feedback_vertex(const FeedbackVertex& v){
    // GL_2D 0x0600、GL_3D 0x0601、GL_3D_COLOR 0x0602、GL_3D_COLOR_TEXTURE 0x0603、GL_4D_COLOR_TEXTURE 0x0604。
    const auto window=window_coordinates(v.clip);const auto type=feedback_.type;
    feedback_values({window[0],window[1]});
    if(type>=0x0601)feedback_values({window[2]});
    if(type==0x0604)feedback_values({window[3]});
    if(type>=0x0602)feedback_values({v.color[0],v.color[1],v.color[2],v.color[3]});
    if(type>=0x0603)feedback_values({v.texcoord[0],v.texcoord[1],v.texcoord[2],v.texcoord[3]});
}
void MetalGl::raster_feedback(unsigned token){
    // 光柵位置在選取模式產生命中；回饋模式依 token 輸出 GL_BITMAP_TOKEN（0x0704）、GL_DRAW_PIXEL_TOKEN（0x0705）或 GL_COPY_PIXEL_TOKEN（0x0706）與光柵頂點；
    // token 為 0 時只處理選取命中。
    if(!state_.raster_valid)return;
    if(state_.render_mode==0x1c02){select_hit(state_.raster_position[2]);return;}
    if(!token || state_.render_mode!=0x1c01)return;
    const auto& r=state_.raster_position;const auto type=feedback_.type;
    feedback_values({static_cast<double>(token),r[0],r[1]});
    if(type>=0x0601)feedback_values({r[2]});
    if(type==0x0604)feedback_values({r[3]});
    if(type>=0x0602)feedback_values({state_.raster_color[0],state_.raster_color[1],state_.raster_color[2],state_.raster_color[3]});
    if(type>=0x0603)feedback_values({state_.raster_texcoord[0],state_.raster_texcoord[1],state_.raster_texcoord[2],state_.raster_texcoord[3]});
}
void MetalGl::feedback_primitive(){
    const bool select=state_.render_mode==0x1c02;
    const auto& mv=matrices_[0].back();const auto& projection=matrices_[1].back();
    // 回饋的色彩型別需要光照後顏色：與 fixed_gl.metal 相同的光源陣列、法線矩陣與旗標。
    const bool lit=!select && feedback_.type>=0x0602 && state_.enabled.contains(0xb50);
    std::array<Light,9> lights{};Matrix normal{};unsigned mask=0;
    if(lit){
        std::copy(state_.lights.begin(),state_.lights.end(),lights.begin());lights[8].ambient=state_.light_model_ambient;
        lights[8].spot={state_.light_model_local?1.F:0.F,state_.light_model_two_side?1.F:0.F,state_.light_model_color_control==0x81fa?1.F:0.F,0};
        normal=normal_matrix(mv);for(unsigned i=0;i<8;++i)if(state_.enabled.contains(0x4000+i))mask|=1U<<i;
    }
    UserClipPlanes user;for(unsigned i=0;i<max_clip_planes;++i)if(state_.enabled.contains(0x3000+i))user.push_back(state_.clip_planes[i]);
    std::vector<FeedbackVertex> v;v.reserve(vertices_.size());
    for(const auto& source:vertices_){
        FeedbackVertex f;const auto eye=transform(mv,source.position);const auto clip=transform(projection,eye);for(unsigned c=0;c<4;++c){f.clip[c]=clip[c];f.eye[c]=eye[c];}
        if(lit)f.color=light_vertex(source.normal,source.ambient,source.diffuse,source.specular,source.emission,source.shininess[0],eye,normal,lights,state_.enabled.contains(0xba1),mask);
        else for(unsigned c=0;c<4;++c)f.color[c]=std::clamp(source.color[c],0.F,1.F);
        f.texcoord=source.texcoord;v.push_back(f);
    }
    const auto point=[&](const FeedbackVertex& p){
        for(unsigned plane=0;plane<6+user.size();++plane)if(clip_distance(p,plane,user)<0)return;
        if(select)select_hit(window_coordinates(p.clip)[2]);else{feedback_values({0x0701});feedback_vertex(p);}
    };
    // GL_LINE_TOKEN 0x0702；線段樣式重新開始的第一段為 GL_LINE_RESET_TOKEN 0x0707。
    const auto line=[&](FeedbackVertex a,FeedbackVertex b,bool reset){
        if(!clip_feedback_line(a,b,user))return;
        if(select){select_hit(window_coordinates(a.clip)[2]);select_hit(window_coordinates(b.clip)[2]);}
        else{feedback_values({reset?double{0x0707}:double{0x0702}});feedback_vertex(a);feedback_vertex(b);}
    };
    const auto polygon=[&](std::vector<FeedbackVertex> p){
        const auto clipped=clip_feedback_polygon(std::move(p),user);if(clipped.size()<3)return;
        if(state_.enabled.contains(0xb44)){
            double area=0;for(std::size_t i=0;i<clipped.size();++i){const auto a=window_coordinates(clipped[i].clip),b=window_coordinates(clipped[(i+1)%clipped.size()].clip);area+=a[0]*b[1]-b[0]*a[1];}
            const bool front=(area>0)==(state_.front_face==0x901);
            if(state_.cull_face==0x408 || (state_.cull_face==0x405 && !front) || (state_.cull_face==0x404 && front))return;
        }
        if(gl_.polygon_mode==0x1b01){for(std::size_t i=0;i<clipped.size();++i)line(clipped[i],clipped[(i+1)%clipped.size()],i==0);return;}
        if(select){for(const auto& p2:clipped)select_hit(window_coordinates(p2.clip)[2]);return;}
        feedback_values({0x0703,static_cast<double>(clipped.size())});for(const auto& p2:clipped)feedback_vertex(p2);
    };
    const auto n=v.size();
    switch(primitive_){
    case 0:for(const auto& p:v)point(p);break;
    case 1:for(std::size_t i=0;i+1<n;i+=2)line(v[i],v[i+1],true);break;
    case 2:case 3:for(std::size_t i=1;i<n;++i)line(v[i-1],v[i],i==1);if(primitive_==2 && n>2)line(v[n-1],v[0],false);break;
    case 4:for(std::size_t i=0;i+2<n;i+=3)polygon({v[i],v[i+1],v[i+2]});break;
    case 5:for(std::size_t i=2;i<n;++i)polygon(i%2?std::vector<FeedbackVertex>{v[i-1],v[i-2],v[i]}:std::vector<FeedbackVertex>{v[i-2],v[i-1],v[i]});break;
    case 6:for(std::size_t i=2;i<n;++i)polygon({v[0],v[i-1],v[i]});break;
    case 7:for(std::size_t i=0;i+3<n;i+=4)polygon({v[i],v[i+1],v[i+2],v[i+3]});break;
    case 8:for(std::size_t i=2;i+1<n;i+=2)polygon({v[i-2],v[i-1],v[i+1],v[i]});break;
    case 9:if(n>=3)polygon(v);break;
    default:break;
    }
}
std::uint64_t MetalGl::select_feedback(GlOperation op,std::span<const std::uint64_t> a,std::span<std::uint8_t> output){
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};const bool select=state_.render_mode==0x1c02;
    switch(op){
    case GlOperation::SelectBuffer:if(select){error(0x502);return 0;}if(std::bit_cast<std::int32_t>(u(0))<0){error(0x501);return 0;}select_.size=u(0);select_.buffer=true;return 0;
    case GlOperation::FeedbackBuffer:
        if(state_.render_mode==0x1c01){error(0x502);return 0;}if(u(1)<0x0600 || u(1)>0x0604){error(0x500);return 0;}if(std::bit_cast<std::int32_t>(u(0))<0){error(0x501);return 0;}
        feedback_.size=u(0);feedback_.type=u(1);feedback_.buffer=true;return 0;
    // 名稱堆疊只在選取模式有作用；改變前先寫入待寫的命中紀錄。
    case GlOperation::InitNames:if(!select)return 0;write_hit_record();select_.names.clear();return 0;
    case GlOperation::LoadName:if(!select)return 0;if(select_.names.empty()){error(0x502);return 0;}write_hit_record();select_.names.back()=u(0);return 0;
    case GlOperation::PushName:if(!select)return 0;write_hit_record();if(select_.names.size()>=max_name_stack_depth){error(0x503);return 0;}select_.names.push_back(u(0));return 0;
    case GlOperation::PopName:if(!select)return 0;write_hit_record();if(select_.names.empty()){error(0x504);return 0;}select_.names.pop_back();return 0;
    case GlOperation::PassThrough:if(state_.render_mode==0x1c01)feedback_values({0x0700,std::bit_cast<float>(u(0))});return 0;
    case GlOperation::RenderMode:{
        const auto mode=u(0);if(mode<0x1c00 || mode>0x1c02){error(0x500);return 0;}
        if((mode==0x1c02 && !select_.buffer) || (mode==0x1c01 && !feedback_.buffer)){error(0x502);return 0;}
        std::int64_t result=0;std::vector<std::uint32_t> words;
        if(select){write_hit_record();result=select_.overflow?-1:static_cast<std::int64_t>(select_.hits);words=select_.records;}
        else if(state_.render_mode==0x1c01){result=feedback_.overflow?-1:static_cast<std::int64_t>(feedback_.values.size());for(const float value:feedback_.values)words.push_back(std::bit_cast<std::uint32_t>(value));}
        if(output.size()>=4){
            const auto count=std::min<std::size_t>(words.size(),output.size()/4-1);const auto header=static_cast<std::uint32_t>(count);
            std::memcpy(output.data(),&header,4);if(count)std::memcpy(output.data()+4,words.data(),count*4);
        }
        state_.render_mode=mode;
        if(mode==0x1c02){select_.names.clear();select_.records.clear();select_.hits=0;select_.hit=false;select_.overflow=false;}
        if(mode==0x1c01){feedback_.values.clear();feedback_.overflow=false;}
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
    }
    default:return 0;
    }
}
