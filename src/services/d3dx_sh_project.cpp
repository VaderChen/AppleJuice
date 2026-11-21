#include "d3dx_sh_project.hpp"
#include "d3dx_sh.hpp"
#include "texture_transform.hpp"
#include "x64arm/windows_graphics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace x64arm {
std::array<float,4> d3dx_cube_direction(unsigned face,float u,float v){
    switch(face){
        case 0:return { 1,-v,-u,0};
        case 1:return {-1,-v, u,0};
        case 2:return { u, 1, v,0};
        case 3:return { u,-1,-v,0};
        case 4:return { u,-v, 1,0};
        default:return {-u,-v,-1,0};
    }
}

void ShCubeProjection::add_face(unsigned face,unsigned edge,std::uint32_t format,
    std::span<const std::uint8_t> pixels){
    const std::array size{edge,edge,1U};
    const auto rgba=texture_image::transform(pixels,format,size,{0,0,0,edge,edge,1},2,size,1);
    const auto area=[](double u,double v){return std::atan2(u*v,std::sqrt(1+u*u+v*v));};
    const bool differential=weighting_==Weighting::NormalizedDifferential;
    // DirectXTex 會展開單一 R 通道。SHMath 投影則讓單通道浮點貼圖的 G、B 維持零。
    const auto channels=differential && (format==41 || format==54)?1U:3U;
    for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x){
        const double left=2.0*x/edge-1,right=2.0*(x+1)/edge-1;
        const double top=2.0*y/edge-1,bottom=2.0*(y+1)/edge-1;
        const double u=(left+right)*0.5,v=(top+bottom)*0.5;
        const double r2=1+u*u+v*v;
        const double omega=differential?4/(r2*std::sqrt(r2)):
            area(right,bottom)-area(left,bottom)-area(right,top)+area(left,top);
        auto direction=d3dx_cube_direction(face,static_cast<float>(u),static_cast<float>(v));
        const auto length=std::sqrt(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);
        for(unsigned axis=0;axis<3;++axis)direction[axis]/=length;
        std::array<float,36> basis;
        sh_evaluate(basis.data(),order_,direction.data());
        std::array<float,4> color;
        std::memcpy(color.data(),rgba.data()+(std::size_t(y)*edge+x)*sizeof(color),sizeof(color));
        for(unsigned channel=0;channel<channels;++channel)
            for(unsigned i=0;i<order_*order_;++i)sums_[channel][i]+=omega*color[channel]*basis[i];
        weight_+=omega;
    }
}

ShCubeCoefficients ShCubeProjection::coefficients() const{
    const double scale=weighting_==Weighting::NormalizedDifferential?4*std::numbers::pi/weight_:1;
    ShCubeCoefficients result{};
    for(unsigned channel=0;channel<3;++channel)
        for(unsigned i=0;i<order_*order_;++i)result[channel][i]=static_cast<float>(sums_[channel][i]*scale);
    return result;
}

namespace {
bool sh_metal_format(unsigned format){
    switch(format){
        case 2: case 6: case 10: case 16: case 26: case 34: case 41: case 54: // 浮點 HDR
        case 28: case 87: // RGBA8／BGRA8
            return true;
        default:return false;
    }
}

struct ProjectionParameters {
    std::uint32_t edge,order,tiles_x,tiles_y;
    std::array<float,36> normalization;
};

std::array<float,36> sh_normalization(){
    std::array<float,36> values{};
    for(unsigned l=0;l<6;++l){
        double ratio=1;
        for(unsigned m=0;m<=l;++m){
            if(m)ratio/=double((l-m+1)*(l+m));
            const double n=(2*l+1)*ratio/(4*std::numbers::pi);
            values[l*6+m]=static_cast<float>(std::sqrt(m?2*n:n));
        }
    }
    return values;
}
}

std::optional<ShCubeCoefficients> sh_project_cube_metal(NativeGraphics& native,
    const std::shared_ptr<GraphicsResource>& cube,unsigned order,ShCubeShaders& shaders){
    static const bool enabled=[] {
        const auto* value=std::getenv("X64ARM_METAL_D3DX_SH_PROJECT");
        return !value || std::string_view(value)!="0";
    }();
    const auto& desc=cube->desc;
    if(!enabled || !(desc.bind&8) || !sh_metal_format(desc.format))return {};
    static const auto normalization=sh_normalization();
    const unsigned count=order*order+1;
    const unsigned tiles_x=std::min((desc.width+7)/8,24U),tiles_y=std::min((desc.width+3)/4,24U);
    const ProjectionParameters options{desc.width,order,tiles_x,tiles_y,normalization};
    using Integral=std::array<float,4>;
    constexpr unsigned integral_bytes=sizeof(Integral);
    std::shared_ptr<GraphicsResource> constants,partial,output;
    try{
        if(!shaders[0] || !shaders[1]){
            GraphicsModernShader program;
            program.source=
#include "d3dx_sh_project_metal.inc"
            ;
            program.entry="project_tiles";
            program.threads={8,4,1};
            program.bindings={{0,0,0},{1,0,0},{4,0,1}};
            auto project=native.graphics_compute_source(program);
            program.entry="reduce_tiles";
            program.threads={64,1,1};
            program.bindings={{0,0,0},{3,0,1},{4,0,2}};
            auto reduce=native.graphics_compute_source(program);
            shaders={std::move(project),std::move(reduce)};
        }
        const auto buffer=[&](unsigned bytes,unsigned bind){
            GraphicsResourceDesc resource;
            resource.width=bytes;
            resource.bind=bind;
            resource.initialized=bytes;
            return native.graphics_resource(resource);
        };
        constants=buffer(sizeof(options),4);
        partial=buffer(tiles_x*tiles_y*6*count*integral_bytes,0x88);
        output=buffer(count*integral_bytes,0x80);
        native.graphics_write(GraphicsView{constants},
            {reinterpret_cast<const std::uint8_t*>(&options),sizeof(options)},sizeof(options),sizeof(options));
    }catch(const std::runtime_error&){
        return {};
    }catch(const std::invalid_argument&){
        return {};
    }catch(const std::bad_alloc&){
        return {};
    }
    auto project=std::make_shared<GraphicsStage>();
    project->shader=shaders[0];
    project->constants[0]=constants;
    project->resources[0]=GraphicsView{cube,desc.format,0,1,0,6};
    project->resources[0].dimension=5;
    project->unordered[0]=GraphicsView{partial};
    native.graphics_dispatch_shared(project,{tiles_x,tiles_y,6});

    auto reduce=std::make_shared<GraphicsStage>();
    reduce->shader=shaders[1];
    reduce->constants[0]=constants;
    reduce->resources[0]=GraphicsView{partial};
    reduce->unordered[0]=GraphicsView{output};
    native.graphics_dispatch_shared(reduce,{count,1,1});
    const auto bytes=native.graphics_read(GraphicsView{output});
    if(bytes.size()!=count*sizeof(Integral))return {};
    std::array<Integral,37> integrals;
    std::memcpy(integrals.data(),bytes.data(),bytes.size());
    const auto weight=integrals[count-1][0];
    if(!(weight>0) || !std::isfinite(weight))return {};
    const double scale=4*std::numbers::pi/weight;
    ShCubeCoefficients result{};
    for(unsigned channel=0;channel<3;++channel)
        for(unsigned i=0;i<count-1;++i)result[channel][i]=static_cast<float>(integrals[i][channel]*scale);
    return result;
}
}
