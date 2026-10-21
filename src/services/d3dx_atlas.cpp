#include "d3dx_atlas.hpp"
#include <UVAtlas.h>
#include <cstring>
#include <limits>
namespace x64arm::d3dx_atlas {
std::uint32_t run(const Request& in,const Progress& callback,Result& out){
    std::vector<DirectX::XMFLOAT3> positions;positions.reserve(in.positions.size());
    for(const auto& p:in.positions)positions.emplace_back(p[0],p[1],p[2]);
    std::vector<DirectX::XMFLOAT2> texcoords;texcoords.reserve(in.texcoords.size());
    for(const auto& p:in.texcoords)texcoords.emplace_back(p[0],p[1]);
    const auto progress=[&](float value)->HRESULT{return static_cast<HRESULT>(callback(value));};
    HRESULT hr=E_INVALIDARG;const auto faces=in.indices.size()/3;
    if(in.mode==0){
        std::vector<DirectX::UVAtlasVertex> vertices;std::vector<std::uint8_t> indices;std::size_t charts=0;
        hr=DirectX::UVAtlasCreate(positions.data(),positions.size(),in.indices.data(),DXGI_FORMAT_R32_UINT,faces,in.charts,in.stretch,in.width,in.height,in.gutter,in.adjacency.data(),in.false_edges.empty()?nullptr:in.false_edges.data(),in.imt.empty()?nullptr:in.imt.data(),progress,in.frequency,static_cast<DirectX::UVATLAS>(in.options),vertices,indices,&out.partitions,&out.remap,&out.stretch,&charts);
        if(charts>UINT32_MAX)return 0x8007000eU;out.charts=static_cast<std::uint32_t>(charts);
        if(hr>=0){out.uv.reserve(vertices.size());for(const auto& vertex:vertices)out.uv.push_back({vertex.uv.x,vertex.uv.y});out.indices.resize(indices.size()/4);std::memcpy(out.indices.data(),indices.data(),indices.size());}
    }else{
        out.imt.resize(faces*3);
        if(in.mode==1)hr=DirectX::UVAtlasComputeIMTFromPerVertexSignal(positions.data(),positions.size(),in.indices.data(),DXGI_FORMAT_R32_UINT,faces,in.signal.data(),in.dimension,in.stride,progress,out.imt.data());
        else if(in.mode==2)hr=DirectX::UVAtlasComputeIMTFromPerTexelSignal(positions.data(),texcoords.data(),positions.size(),in.indices.data(),DXGI_FORMAT_R32_UINT,faces,in.signal.data(),in.width,in.height,in.dimension,in.components,static_cast<DirectX::UVATLAS_IMT>(in.options),progress,out.imt.data());
        else if(in.mode==3)hr=DirectX::UVAtlasComputeIMTFromTexture(positions.data(),texcoords.data(),positions.size(),in.indices.data(),DXGI_FORMAT_R32_UINT,faces,in.signal.data(),in.width,in.height,static_cast<DirectX::UVATLAS_IMT>(in.options),progress,out.imt.data());
    }
    return static_cast<std::uint32_t>(hr);
}
}
