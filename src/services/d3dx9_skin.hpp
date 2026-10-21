#pragma once
namespace x64arm::d3dx_skin {
struct Bone {
    std::string name;std::vector<unsigned> vertices;std::vector<float> weights;std::array<float,16> offset{};
    std::uint64_t guest_name{},guest_offset{};
    Bone(){offset[0]=offset[5]=offset[10]=offset[15]=1;}
};
struct Info {
    unsigned vertices{};float minimum{};std::vector<Bone> bones;
    std::vector<std::vector<std::pair<unsigned,float>>> influences()const{
        std::vector<std::vector<std::pair<unsigned,float>>> out(vertices);
        for(unsigned b=0;b<bones.size();++b){const auto& bone=bones[b];for(std::size_t j=0;j<bone.vertices.size();++j)if(bone.weights[j]>=minimum && bone.weights[j]!=0)out.at(bone.vertices[j]).emplace_back(b,bone.weights[j]);}
        return out;
    }
};
}
