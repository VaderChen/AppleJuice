#include "d3dx_mesh.hpp"
#include <DirectXMesh.h>
#include <algorithm>
#include <numeric>
#include <map>
namespace x64arm::d3dx_mesh {
std::uint32_t optimize(std::span<const std::uint32_t> indices,unsigned vertices,std::span<const std::uint32_t> adjacency,std::span<const std::uint32_t> attributes,unsigned flags,Optimized& out){
    constexpr unsigned invalid=0x8876086c;
    const auto faces=indices.size()/3;
    if(!faces || indices.size()%3 || attributes.size()!=faces || (!adjacency.empty() && adjacency.size()!=indices.size()) || (flags&~0x3f400000U) || ((flags&0xc000000)==0xc000000))return invalid;
    for(auto index:indices)if(index>=vertices)return invalid;
    for(auto face:adjacency)if(face!=UINT32_MAX && face>=faces)return invalid;
    out={};out.faces.resize(faces);std::iota(out.faces.begin(),out.faces.end(),0U);
    if(flags&0xc000000){
        if(adjacency.empty())return invalid;
        const auto hr=DirectX::OptimizeFacesEx(indices.data(),faces,vertices,adjacency.data(),attributes.data(),out.faces.data(),(flags&0x8000000)?DirectX::OPTFACES_V_STRIPORDER:DirectX::OPTFACES_V_DEFAULT);
        if(hr<0)return static_cast<unsigned>(hr);
    }
    if(flags&0x2000000)std::stable_sort(out.faces.begin(),out.faces.end(),[&](auto a,auto b){return attributes[a]<attributes[b];});
    std::vector<unsigned> inverse(faces,UINT32_MAX);
    for(unsigned i=0;i<faces;++i){if(out.faces[i]>=faces || inverse[out.faces[i]]!=UINT32_MAX)return invalid;inverse[out.faces[i]]=i;}
    out.indices.resize(indices.size());out.attributes.resize(faces);
    if(!adjacency.empty())out.adjacency.resize(indices.size());
    for(unsigned i=0;i<faces;++i){auto old=out.faces[i];out.attributes[i]=attributes[old];for(unsigned j=0;j<3;++j){out.indices[i*3+j]=indices[old*3+j];if(!adjacency.empty()){auto neighbor=adjacency[old*3+j];out.adjacency[i*3+j]=neighbor==UINT32_MAX?neighbor:inverse[neighbor];}}}
    if(flags&0x10000000){out.vertices.resize(vertices);std::iota(out.vertices.begin(),out.vertices.end(),0U);return 0;}
    // 新頂點 -> 原頂點映射，同時處理材質群組邊界的頂點分割。
    std::map<std::pair<unsigned,unsigned>,unsigned> remap;
    std::vector<bool> used(vertices);
    const bool split=(flags&0x2000000) && !(flags&0x20000000);
    for(unsigned i=0;i<out.indices.size();++i){auto old=out.indices[i];auto key=std::pair{old,split?out.attributes[i/3]:0U};auto [it,inserted]=remap.emplace(key,out.vertices.size());if(inserted)out.vertices.push_back(old);used[old]=true;out.indices[i]=it->second;}
    if(!(flags&0x1000000))for(unsigned i=0;i<vertices;++i)if(!used[i])out.vertices.push_back(i);
    return 0;
}
std::uint32_t validate(std::span<const std::uint32_t> indices,unsigned vertices,std::span<const std::uint32_t> adjacency,std::string& messages){
    if(indices.size()%3 || (!adjacency.empty() && adjacency.size()!=indices.size()))return 0x8876086c;
    std::wstring detail;const auto flags=DirectX::VALIDATE_DEGENERATE|DirectX::VALIDATE_UNUSED|(adjacency.empty()?0:DirectX::VALIDATE_BACKFACING|DirectX::VALIDATE_BOWTIES|DirectX::VALIDATE_ASYMMETRIC_ADJ);
    const auto hr=DirectX::Validate(indices.data(),indices.size()/3,vertices,adjacency.empty()?nullptr:adjacency.data(),static_cast<DirectX::VALIDATE_FLAGS>(flags),&detail);
    // 上游診斷字串目前為 ASCII；以 UTF-8 保存，亦容納未來的非 ASCII 字元。
    for(auto ch:detail){const auto c=static_cast<std::uint32_t>(ch);if(c<128)messages.push_back(static_cast<char>(c));else if(c<2048){messages.push_back(static_cast<char>(0xc0|(c>>6)));messages.push_back(static_cast<char>(0x80|(c&63)));}else if(c<65536){messages.push_back(static_cast<char>(0xe0|(c>>12)));messages.push_back(static_cast<char>(0x80|((c>>6)&63)));messages.push_back(static_cast<char>(0x80|(c&63)));}else{messages.push_back(static_cast<char>(0xf0|(c>>18)));messages.push_back(static_cast<char>(0x80|((c>>12)&63)));messages.push_back(static_cast<char>(0x80|((c>>6)&63)));messages.push_back(static_cast<char>(0x80|(c&63)));}}
    return static_cast<std::uint32_t>(hr);
}
std::uint32_t clean(std::vector<std::uint32_t>& indices,unsigned vertices,std::vector<std::uint32_t>& adjacency,std::span<const std::uint32_t> attributes,std::vector<std::uint32_t>& duplicates,bool bowties){
    if(indices.size()%3 || (!adjacency.empty() && adjacency.size()!=indices.size()) || attributes.size()!=indices.size()/3)return 0x8876086c;
    return static_cast<std::uint32_t>(DirectX::Clean(indices.data(),indices.size()/3,vertices,adjacency.empty()?nullptr:adjacency.data(),attributes.data(),duplicates,bowties));
}
}
