// Metal 後端的 glTexGen（opengl_metal.mm 引入）：設定與查詢 S、T、R、Q 的產生模式與平面，以及 vertex() 在套用貼圖矩陣前產生座標。
// EYE_LINEAR 的平面在設定當下乘上模型視圖矩陣的反矩陣（p' = p M⁻¹）；球面、法線與反射貼圖以眼座標法線（M⁻¹ 的轉置作用於法線）
// 與從原點指向頂點的單位向量計算。只有啟用的座標會被取代，其餘沿用目前的貼圖座標。
namespace {
// 欄優先 4×4 反矩陣；不可逆時回傳空值。
std::optional<std::array<double,16>> texgen_inverse(const Matrix& m){
    std::array<std::array<double,8>,4> t{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){t[r][c]=m[c*4+r];t[r][c+4]=r==c?1:0;}
    for(unsigned c=0;c<4;++c){
        unsigned p=c;for(unsigned r=c+1;r<4;++r)if(std::fabs(t[r][c])>std::fabs(t[p][c]))p=r;
        if(t[p][c]==0)return std::nullopt;std::swap(t[p],t[c]);const double pivot=t[c][c];for(auto& v:t[c])v/=pivot;
        for(unsigned r=0;r<4;++r)if(r!=c){const double f=t[r][c];if(f!=0)for(unsigned k=0;k<8;++k)t[r][k]-=f*t[c][k];}
    }
    std::array<double,16> result{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)result[c*4+r]=t[r][c+4];return result;
}
}
unsigned MetalGl::texgen_mask()const{unsigned mask=0;for(unsigned i=0;i<4;++i)if(state_.enabled.contains(0xc60+i))mask|=1U<<i;return mask;}
std::uint64_t MetalGl::texgen(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    // coord：GL_S 0x2000～GL_Q 0x2003；pname：GL_TEXTURE_GEN_MODE 0x2500、GL_OBJECT_PLANE 0x2501、GL_EYE_PLANE 0x2502。
    const auto coord=static_cast<unsigned>(a[0]),pname=static_cast<unsigned>(a[1]);
    if(coord<0x2000 || coord>0x2003 || pname<0x2500 || pname>0x2502){error(0x500);return 0;}
    auto& g=state_.texgen[coord-0x2000];
    if(op==GlOperation::GetTexGendv){
        std::array<double,4> values{};std::size_t count=4;
        if(pname==0x2500){values[0]=g.mode;count=1;}else values=pname==0x2501?g.object:g.eye;
        count=std::min(count,output.size()/8);if(count)std::memcpy(output.data(),values.data(),count*8);return count;
    }
    const std::size_t count=std::min<std::size_t>(data.size()/8,4);std::array<double,4> values{};if(count)std::memcpy(values.data(),data.data(),count*8);
    if(pname==0x2500){
        // GL_EYE_LINEAR 0x2400、GL_OBJECT_LINEAR 0x2401、GL_SPHERE_MAP 0x2402（只限 S、T）、GL_NORMAL_MAP 0x8511 與 GL_REFLECTION_MAP 0x8512（只限 S、T、R）。
        const auto mode=count?static_cast<unsigned>(values[0]):0U;
        const bool valid=count && static_cast<double>(mode)==values[0] && (mode==0x2400 || mode==0x2401 || (mode==0x2402 && coord<=0x2001) || ((mode==0x8511 || mode==0x8512) && coord<=0x2002));
        if(!valid){error(0x500);return 0;}
        g.mode=mode;return 0;
    }
    if(count<4){error(0x500);return 0;}
    if(pname==0x2501){g.object=values;return 0;}
    const auto inverse=texgen_inverse(matrices_[0].back());
    if(!inverse){g.eye=values;return 0;}
    std::array<double,4> eye{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)eye[i]+=values[j]*(*inverse)[i*4+j];
    g.eye=eye;return 0;
}
void MetalGl::apply_texgen(const Vec& position,Vec& texcoord){
    const auto& mv=matrices_[0].back();
    std::array<double,4> eye{};for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)eye[r]+=static_cast<double>(mv[k*4+r])*position[k];
    bool needs_normal=false;
    for(unsigned i=0;i<4;++i)if((state_.texgen_mask>>i)&1U){const auto mode=state_.texgen[i].mode;needs_normal=needs_normal || mode==0x2402 || mode==0x8511 || mode==0x8512;}
    std::array<double,3> normal{},reflection{};std::array<double,2> sphere{0.5,0.5};
    if(needs_normal){
        if(const auto inverse=texgen_inverse(mv))for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)normal[i]+=state_.normal[j]*(*inverse)[i*4+j];
        else normal={state_.normal[0],state_.normal[1],state_.normal[2]};
        const double length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);if(length>0)for(auto& c:normal)c/=length;
        std::array<double,3> u{eye[0],eye[1],eye[2]};const double distance=std::sqrt(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);if(distance>0)for(auto& c:u)c/=distance;
        const double dot=normal[0]*u[0]+normal[1]*u[1]+normal[2]*u[2];for(unsigned i=0;i<3;++i)reflection[i]=u[i]-2*normal[i]*dot;
        const double m=2*std::sqrt(reflection[0]*reflection[0]+reflection[1]*reflection[1]+(reflection[2]+1)*(reflection[2]+1));
        if(m>0)sphere={reflection[0]/m+0.5,reflection[1]/m+0.5};
    }
    for(unsigned i=0;i<4;++i){
        if(!((state_.texgen_mask>>i)&1U))continue;
        const auto& g=state_.texgen[i];double value=0;
        switch(g.mode){
        case 0x2401:for(unsigned k=0;k<4;++k)value+=g.object[k]*position[k];break;
        case 0x2400:for(unsigned k=0;k<4;++k)value+=g.eye[k]*eye[k];break;
        case 0x2402:value=i<2?sphere[i]:0;break;
        case 0x8511:value=i<3?normal[i]:0;break;
        case 0x8512:value=i<3?reflection[i]:0;break;
        default:break;
        }
        texcoord[i]=static_cast<float>(value);
    }
}
