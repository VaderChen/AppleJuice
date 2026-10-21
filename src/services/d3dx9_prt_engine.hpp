#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>
#include "d3dx_sh.hpp"
namespace x64arm::d3dx_prt {
using Vec=std::array<float,3>;
inline Vec add(Vec a,Vec b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline Vec subtract(Vec a,Vec b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline Vec scale(Vec a,float s){return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(Vec a,Vec b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline Vec cross(Vec a,Vec b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline Vec unit(Vec a){const float length=std::sqrt(dot(a,a));return length>0?scale(a,1/length):Vec{0,0,1};}
struct Material {Vec diffuse{1,1,1},absorption{},scattering{};float refraction{1.3F};bool subsurface{};};
struct Hit {unsigned face{UINT32_MAX};float distance{},u{},v{};};
struct Engine {
    struct Triangle {Vec a,b,c;unsigned scene_face{UINT32_MAX};};
    struct Node {Vec low{},high{};unsigned begin{},count{},left{},right{};};
    std::vector<Vec> positions,normals,albedo;std::vector<std::array<float,2>> uv;
    std::vector<unsigned> indices,attributes,vertex_attributes;std::vector<Material> materials;
    std::vector<Triangle> blockers,triangles;std::vector<unsigned> order;std::vector<Node> nodes;
    unsigned channels{3},rays{1024};bool sphere{},cosine{true};float minimum{1e-5F},maximum{std::numeric_limits<float>::max()},length_scale{1};
    std::uint64_t callback{},context{};float frequency{.01F};
    unsigned build_node(unsigned begin,unsigned count){
        Node node;node.begin=begin;node.count=count;node.low.fill(INFINITY);node.high.fill(-INFINITY);
        for(unsigned i=begin;i<begin+count;++i){const auto& t=triangles[order[i]];for(const auto& v:{t.a,t.b,t.c})for(unsigned axis=0;axis<3;++axis){node.low[axis]=std::min(node.low[axis],v[axis]);node.high[axis]=std::max(node.high[axis],v[axis]);}}
        const unsigned at=static_cast<unsigned>(nodes.size());nodes.push_back(node);
        if(count>8){unsigned axis=0;for(unsigned a=1;a<3;++a)if(node.high[a]-node.low[a]>node.high[axis]-node.low[axis])axis=a;
            const unsigned middle=begin+count/2;std::nth_element(order.data()+begin,order.data()+middle,order.data()+begin+count,[&](unsigned a,unsigned b){const auto& x=triangles[a];const auto& y=triangles[b];return x.a[axis]+x.b[axis]+x.c[axis]<y.a[axis]+y.b[axis]+y.c[axis];});
            nodes[at].left=build_node(begin,count/2);nodes[at].right=build_node(middle,count-count/2);nodes[at].count=0;
        }return at;
    }
    void rebuild(){
        triangles=blockers;for(unsigned face=0;face<indices.size()/3;++face)triangles.push_back({positions.at(indices[face*3]),positions.at(indices[face*3+1]),positions.at(indices[face*3+2]),face});
        order.resize(triangles.size());std::iota(order.begin(),order.end(),0U);nodes.clear();if(!triangles.empty())build_node(0,static_cast<unsigned>(triangles.size()));
        vertex_attributes.assign(positions.size(),0);for(unsigned face=0;face<attributes.size();++face)for(unsigned corner=0;corner<3;++corner)vertex_attributes.at(indices[face*3+corner])=attributes[face];
        if(albedo.empty())albedo.assign(positions.size(),Vec{1,1,1});
    }
    bool intersect(Vec origin,Vec direction,Hit& hit,bool closest=true)const{
        hit={UINT32_MAX,maximum,0,0};if(nodes.empty())return false;std::array<unsigned,128> stack{};unsigned pending=1;stack[0]=0;
        while(pending){const auto& node=nodes[stack[--pending]];float lo=minimum,hi=hit.distance;bool intersects=true;
            for(unsigned axis=0;axis<3;++axis){if(std::abs(direction[axis])<1e-30F){if(origin[axis]<node.low[axis] || origin[axis]>node.high[axis])intersects=false;}else {float a=(node.low[axis]-origin[axis])/direction[axis],b=(node.high[axis]-origin[axis])/direction[axis];if(a>b)std::swap(a,b);lo=std::max(lo,a);hi=std::min(hi,b);if(lo>hi)intersects=false;}}
            if(!intersects)continue;
            if(!node.count){if(pending+2>stack.size())throw std::runtime_error("PRT 空間索引深度超出限制");stack[pending++]=node.left;stack[pending++]=node.right;continue;}
            for(unsigned j=0;j<node.count;++j){const unsigned face=order[node.begin+j];const auto& t=triangles[face];const Vec e1=subtract(t.b,t.a),e2=subtract(t.c,t.a),p=cross(direction,e2);const double determinant=dot(e1,p);if(std::abs(determinant)<1e-20)continue;
                const Vec delta=subtract(origin,t.a);const double u=dot(delta,p)/determinant;if(u<0 || u>1)continue;const Vec q=cross(delta,e1);const double v=dot(direction,q)/determinant;if(v<0 || u+v>1)continue;const double distance=dot(e2,q)/determinant;
                if(distance>=minimum && distance<=hit.distance){hit={face,static_cast<float>(distance),static_cast<float>(1-u-v),static_cast<float>(u)};if(!closest)return true;}
            }
        }return hit.face!=UINT32_MAX;
    }
    static float radical(unsigned n){n=(n<<16)|(n>>16);n=((n&0x55555555U)<<1)|((n&0xaaaaaaaaU)>>1);n=((n&0x33333333U)<<2)|((n&0xccccccccU)>>2);n=((n&0x0f0f0f0fU)<<4)|((n&0xf0f0f0f0U)>>4);n=((n&0x00ff00ffU)<<8)|((n&0xff00ff00U)>>8);return static_cast<float>(static_cast<double>(n)*2.3283064365386963e-10);}
    Vec direction(unsigned ray,Vec normal,bool volume,float& weight)const{
        constexpr float pi=3.14159265358979323846F;const float a=(static_cast<float>(ray)+.5F)/static_cast<float>(rays),angle=2*pi*radical(ray);
        if(volume || sphere){const float z=1-2*a,r=std::sqrt(std::max(0.F,1-z*z));Vec d{r*std::cos(angle),r*std::sin(angle),z};weight=volume?4*pi:cosine?4*std::max(0.F,dot(normal,d)):4*pi;return d;}
        const float z=cosine?std::sqrt(1-a):a,r=std::sqrt(std::max(0.F,1-z*z));const Vec tangent=unit(cross(std::abs(normal[2])<.9F?Vec{0,0,1}:Vec{0,1,0},normal)),bitangent=cross(normal,tangent);weight=cosine?1.F:2*pi;return add(add(scale(tangent,r*std::cos(angle)),scale(bitangent,r*std::sin(angle))),scale(normal,z));
    }
#include "d3dx9_prt_ldprt.inc"
    std::uint32_t direct(unsigned sh_order,std::span<const Vec> locations,std::span<const Vec> sample_normals,unsigned out_channels,std::vector<float>& output,const std::function<std::uint32_t(float)>& progress)const{
        if(sh_order<2 || sh_order>6 || locations.size()!=sample_normals.size() || !rays || (out_channels!=1 && out_channels!=3))throw std::invalid_argument("PRT 直接光照參數不合法");
        const unsigned coefficients=sh_order*sh_order;output.assign(locations.size()*coefficients*out_channels,0);float notified=-1;
        for(unsigned sample=0;sample<locations.size();++sample){std::array<double,36> sums{};const Vec normal=unit(sample_normals[sample]);
            for(unsigned ray=0;ray<rays;++ray){float weight=0;const auto d=direction(ray,normal,false,weight);if(weight==0)continue;Hit hit;if(intersect(locations[sample],d,hit,false))continue;std::array<float,36> basis{};sh_evaluate(basis.data(),sh_order,d.data());for(unsigned i=0;i<coefficients;++i)sums[i]+=static_cast<double>(weight)*basis[i];}
            for(unsigned channel=0;channel<out_channels;++channel)for(unsigned i=0;i<coefficients;++i)output[(std::size_t(sample)*out_channels+channel)*coefficients+i]=static_cast<float>(sums[i]/rays);
            const float done=static_cast<float>(sample+1)/static_cast<float>(locations.size());if(done-notified>=frequency || sample+1==locations.size()){if(auto code=progress(done))return code;notified=done;}
        }return 0;
    }
    std::uint32_t bounce(std::span<const float> input,unsigned coefficients,unsigned in_channels,std::span<const Vec> locations,std::span<const Vec> sample_normals,std::vector<float>& output,const std::function<std::uint32_t(float)>& progress)const{
        if(input.size()!=positions.size()*coefficients*in_channels || locations.size()!=sample_normals.size() || !rays)throw std::invalid_argument("PRT 反射緩衝區不相容");output.assign(locations.size()*coefficients*in_channels,0);float notified=-1;
        for(unsigned sample=0;sample<locations.size();++sample){std::vector<double> sum(std::size_t(coefficients)*in_channels);const auto normal=unit(sample_normals[sample]);
            for(unsigned ray=0;ray<rays;++ray){float weight=0;const auto d=direction(ray,normal,false,weight);if(weight==0)continue;Hit hit;if(!intersect(locations[sample],d,hit))continue;const auto face=triangles[hit.face].scene_face;if(face==UINT32_MAX)continue;const std::array<float,3> bary{hit.u,hit.v,1-hit.u-hit.v};Vec hit_normal{};for(unsigned corner=0;corner<3;++corner)hit_normal=add(hit_normal,scale(normals[indices[face*3+corner]],bary[corner]));if(dot(hit_normal,d)>=0)continue;
                for(unsigned channel=0;channel<in_channels;++channel)for(unsigned coefficient=0;coefficient<coefficients;++coefficient){double radiance=0;for(unsigned corner=0;corner<3;++corner){const auto vertex=indices[face*3+corner];const auto color=albedo[vertex];const float reflectance=in_channels==1?color[0]*.2125F+color[1]*.7154F+color[2]*.0721F:color[channel];radiance+=bary[corner]*reflectance*input[(std::size_t(vertex)*in_channels+channel)*coefficients+coefficient];}sum[std::size_t(channel)*coefficients+coefficient]+=weight*radiance;}
            }
            for(unsigned i=0;i<sum.size();++i)output[std::size_t(sample)*sum.size()+i]=static_cast<float>(sum[i]/rays);const float done=static_cast<float>(sample+1)/static_cast<float>(locations.size());if(done-notified>=frequency || sample+1==locations.size()){if(auto code=progress(done))return code;notified=done;}
        }return 0;
    }
};
}
