        const auto make_skin=[&](unsigned vertices,const std::vector<std::uint8_t>& layout,unsigned bones){
            require(vertices<=16777216 && bones<=65536);const auto stride=stride_of(layout);auto object=create(Kind::Skin);
            object->skin=std::make_shared<d3dx_skin::Info>();object->skin->vertices=vertices;object->skin->bones.resize(bones);object->presentation=layout;object->stream_stride=stride;object->fvf=decl_fvf(layout);return object;
        };
        const bool skin_handled=symbol=="D3DXCreateSkinInfo" || symbol=="D3DXCreateSkinInfoFVF" || symbol.starts_with("Skin.");
        if(skin_handled){
            mesh_handled=true;
            if(symbol.starts_with("D3DXCreateSkinInfo")){output(3);store_pointer(arg(3),make_skin(u(0),symbol.ends_with("FVF")?fvf_decl(u(1)):declaration(arg(1)),u(2))->root);}
            else {
                auto object=get(arg(0),Kind::Skin);auto& info=*object->skin;const auto method=symbol.substr(5);
                const auto bone_at=[&](unsigned index)->d3dx_skin::Bone&{require(index<info.bones.size());return info.bones[index];};
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(supports(Kind::Skin,requested)){hold(object);store_pointer(arg(2),object->root);}else result=no_interface;}
                else if(method=="AddRef")result=hold(object);else if(method=="Release")result=release(object->root);
                else if(method=="GetNumBones")result=info.bones.size();
                else if(method=="SetBoneInfluence"){
                    auto& bone=bone_at(u(1));require(u(2)<=info.vertices && (!u(2) || (arg(3) && arg(4))));
                    std::vector<unsigned> vertices(u(2));std::vector<float> weights(u(2));std::set<unsigned> unique;
                    for(unsigned i=0;i<u(2);++i){vertices[i]=static_cast<unsigned>(memory.load(GuestAddress{arg(3)+i*4ULL},32));weights[i]=real(static_cast<unsigned>(memory.load(GuestAddress{arg(4)+i*4ULL},32)));require(vertices[i]<info.vertices && unique.insert(vertices[i]).second && std::isfinite(weights[i]) && weights[i]>=0);}
                    bone.vertices=std::move(vertices);bone.weights=std::move(weights);
                }else if(method=="SetBoneVertexInfluence"){auto& bone=bone_at(u(1));const auto weight=scalar_arg(3);require(u(2)<bone.weights.size() && std::isfinite(weight) && weight>=0);bone.weights[u(2)]=weight;}
                else if(method=="GetNumBoneInfluences")result=bone_at(u(1)).vertices.size();
                else if(method=="GetBoneInfluence"){const auto& bone=bone_at(u(1));require(bone.vertices.empty() || arg(2));if(arg(2))memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(bone.vertices.data()),bone.vertices.size()*4));if(arg(3))memory.write(GuestAddress{arg(3)},std::span(reinterpret_cast<const std::uint8_t*>(bone.weights.data()),bone.weights.size()*4));}
                else if(method=="GetBoneVertexInfluence"){const auto& bone=bone_at(u(1));require(u(2)<bone.vertices.size() && arg(3));memory.store(GuestAddress{arg(3)},32,bits(bone.weights[u(2)]));if(arg(4))memory.store(GuestAddress{arg(4)},32,bone.vertices[u(2)]);}
                else if(method=="FindBoneVertexInfluenceIndex"){const auto& bone=bone_at(u(1));require(arg(3));const auto it=std::find(bone.vertices.begin(),bone.vertices.end(),u(2));require(it!=bone.vertices.end());memory.store(GuestAddress{arg(3)},32,static_cast<unsigned>(it-bone.vertices.begin()));}
                else if(method=="GetMaxVertexInfluences"){require(arg(1));unsigned maximum=0;for(const auto& v:info.influences())maximum=std::max(maximum,static_cast<unsigned>(v.size()));memory.store(GuestAddress{arg(1)},32,maximum);}
                else if(method=="GetMaxFaceInfluences"){
                    require(arg(3));auto ib=get(arg(1),Kind::IndexBuffer);const unsigned bytes=ib->format==102?4:2;require(!ib->mapped && u(2)<=ib->pixels.size()/(bytes*3));const auto influences=info.influences();unsigned maximum=0;
                    for(unsigned f=0;f<u(2);++f){std::set<unsigned> bones;for(unsigned c=0;c<3;++c){const auto at=(std::size_t(f)*3+c)*bytes;const auto vertex=bytes==4?word(ib->pixels,at):unsigned(ib->pixels[at])|unsigned(ib->pixels[at+1])<<8;require(vertex<info.vertices);for(const auto& [bone,weight]:influences[vertex]){(void)weight;bones.insert(bone);}}maximum=std::max(maximum,static_cast<unsigned>(bones.size()));}memory.store(GuestAddress{arg(3)},32,maximum);
                }else if(method=="SetMinBoneInfluence"){const auto value=scalar_arg(1);require(std::isfinite(value) && value>=0);info.minimum=value;}
                else if(method=="GetMinBoneInfluence")return_scalar(info.minimum,true);
                else if(method=="SetBoneName"){auto& bone=bone_at(u(1));require(arg(2));auto name=read_ansi(memory,arg(2));if(bone.guest_name){(void)free_heap(memory,bone.guest_name,state);bone.guest_name=0;}bone.name=std::move(name);}
                else if(method=="GetBoneName"){auto& bone=bone_at(u(1));if(!bone.name.empty() && !bone.guest_name){bone.guest_name=allocate_heap(memory,bone.name.size()+1);require(bone.guest_name,out_of_memory);std::vector<std::uint8_t> bytes(bone.name.begin(),bone.name.end());bytes.push_back(0);memory.write(GuestAddress{bone.guest_name},bytes);}result=bone.guest_name;}
                else if(method=="SetBoneOffsetMatrix"){auto& bone=bone_at(u(1));require(arg(2));auto bytes=read(arg(2),64);std::memcpy(bone.offset.data(),bytes.data(),64);if(bone.guest_offset)memory.write(GuestAddress{bone.guest_offset},bytes);}
                else if(method=="GetBoneOffsetMatrix"){auto& bone=bone_at(u(1));if(!bone.guest_offset){bone.guest_offset=allocate_heap(memory,64);require(bone.guest_offset,out_of_memory);memory.write(GuestAddress{bone.guest_offset},std::span(reinterpret_cast<const std::uint8_t*>(bone.offset.data()),64));}result=bone.guest_offset;}
                else if(method=="Clone"){output(1);auto clone=make_skin(info.vertices,object->presentation,static_cast<unsigned>(info.bones.size()));*clone->skin=info;for(auto& bone:clone->skin->bones)bone.guest_name=bone.guest_offset=0;store_pointer(arg(1),clone->root);}
                else if(method=="Remap"){
                    require(u(1)<=16777216 && (!u(1) || arg(2)));std::vector<std::vector<unsigned>> remap(info.vertices);for(unsigned i=0;i<u(1);++i){const auto original=memory.load(GuestAddress{arg(2)+i*4ULL},32);require(original<info.vertices);remap[original].push_back(i);}
                    for(auto& bone:info.bones){std::vector<unsigned> vertices;std::vector<float> weights;for(std::size_t j=0;j<bone.vertices.size();++j)for(auto vertex:remap[bone.vertices[j]]){vertices.push_back(vertex);weights.push_back(bone.weights[j]);}bone.vertices=std::move(vertices);bone.weights=std::move(weights);}info.vertices=u(1);
                }else if(method=="SetFVF" || method=="SetDeclaration"){auto layout=method=="SetFVF"?fvf_decl(u(1)):declaration(arg(1));const auto stride=stride_of(layout);object->fvf=decl_fvf(layout);object->presentation=std::move(layout);object->stream_stride=stride;}
                else if(method=="GetFVF")result=object->fvf;
                else if(method=="GetDeclaration"){require(arg(1));memory.write(GuestAddress{arg(1)},object->presentation);}
                else if(method=="UpdateSkinnedMesh"){
                    require(arg(1) && arg(3) && arg(4));const auto source=read(arg(3),std::size_t(info.vertices)*object->stream_stride);auto destination=source;const auto influences=info.influences();std::vector<Matrix> transforms(info.bones.size()),normals(info.bones.size());
                    for(unsigned b=0;b<info.bones.size();++b){const auto matrix=read(arg(1)+b*64ULL,64);std::memcpy(transforms[b].data(),matrix.data(),64);if(arg(2)){const auto normal=read(arg(2)+b*64ULL,64);std::memcpy(normals[b].data(),normal.data(),64);}else normals[b]=transforms[b];}
                    for(unsigned vertex=0;vertex<info.vertices;++vertex)for(std::size_t e=0;e+8<object->presentation.size();e+=8){const auto type=object->presentation[e+4],usage=object->presentation[e+6];if(usage!=0 && usage!=3 && usage!=6 && usage!=7)continue;require(type==2 || type==3);const auto offset=std::size_t(vertex)*object->stream_stride+(unsigned(object->presentation[e+2])|unsigned(object->presentation[e+3])<<8);Vector input{},sum{};for(unsigned c=0;c<3;++c)input[c]=real(word(source,offset+c*4));input[3]=usage==0?1.f:0.f;
                        for(const auto& [bone,weight]:influences[vertex]){const auto value=transform(input,usage==0?transforms[bone]:normals[bone]);for(unsigned c=0;c<3;++c)sum[c]+=value[c]*weight;}
                        for(unsigned c=0;c<3;++c)put(destination,offset+c*4,bits(sum[c]));
                    }
                    memory.write(GuestAddress{arg(4)},destination);
                }
#include "d3dx9_skin_convert.inc"
                else require(false);
            }
        }
