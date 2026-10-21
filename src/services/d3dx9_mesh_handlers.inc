        // D3DX9 網格以共用 D3D9 buffer 保存資料，鎖定後的改動直接供 CPU 前處理使用。
        bool mesh_handled=symbol.starts_with("RenderToSurface.") || symbol.starts_with("RenderToEnvMap.") || symbol.starts_with("Effect.") || symbol.starts_with("EffectPool.") || symbol.starts_with("Sprite.") || symbol.starts_with("Font.") || symbol.starts_with("Mesh.") || symbol.starts_with("Blob.") || symbol.starts_with("PRT.") || symbol.starts_with("PRTComp.") || symbol.starts_with("PRTEngine.") || symbol.starts_with("D3DX");
        const auto declaration=[&](std::uint64_t address){
            std::vector<std::uint8_t> data;require(address!=0);
            for(unsigned i=0;i<65;++i){auto e=read(address+i*8,8);data.insert(data.end(),e.begin(),e.end());if(e[0]==255 && e[1]==0 && e[4]==17)return data;require(i<64 && e[0]==0 && e[1]==0 && e[4]<17 && e[5]==0);}
            throw Failure{invalid};
        };
        const auto element_bytes=[](unsigned type){constexpr unsigned sizes[]{4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};require(type<17);return sizes[type];};
        const auto stride_of=[&](const auto& decl){unsigned stride=0;for(std::size_t i=0;i+8<decl.size();i+=8)stride=std::max(stride,unsigned(decl[i+2]|decl[i+3]<<8)+element_bytes(decl[i+4]));require(stride && stride<=4096);return stride;};
        const auto fvf_decl=[&](unsigned fvf){
            std::vector<std::uint8_t> d;unsigned offset=0;
            const auto add=[&](unsigned type,unsigned usage,unsigned index=0){d.insert(d.end(),{0,0,static_cast<std::uint8_t>(offset),static_cast<std::uint8_t>(offset>>8),static_cast<std::uint8_t>(type),0,static_cast<std::uint8_t>(usage),static_cast<std::uint8_t>(index)});offset+=element_bytes(type);};
            const auto position=fvf&0x400e;const bool blended=position>=6 && position<=14;
            require(!(fvf&0x2001) && (position==2 || position==4 || position==0x4002 || blended));
            const auto last_beta=fvf&0x9000;require(last_beta!=0x9000 && (!last_beta || blended));
            add(position==4 || position==0x4002?3:2,position==4?9:0);
            if(blended){const unsigned betas=(position-4)/2,weights=betas-(last_beta?1U:0U);if(weights)add(std::min(weights,4U)-1,1);if(weights==5)add(0,2);else if(last_beta)add(last_beta==0x1000?5:4,2);}
            if(fvf&0x10)add(2,3);if(fvf&0x20)add(0,4);if(fvf&0x40)add(4,10);if(fvf&0x80)add(4,10,1);
            const unsigned count=(fvf>>8)&15;require(count<=8);for(unsigned i=0;i<count;++i){const unsigned code=(fvf>>(16+2*i))&3;constexpr unsigned types[]{1,2,3,0};add(types[code],5,i);}
            d.insert(d.end(),{255,0,0,0,17,0,0,0});return d;
        };
        // 只有元素、順序、偏移均能往返的宣告才具有 FVF；任意 shader 宣告維持 0。
        const auto decl_fvf=[&](const std::vector<std::uint8_t>& d)->unsigned{
            unsigned fvf=0,weights=0,index_type=0,tex_count=0;
            for(std::size_t i=0;i+8<d.size();i+=8){
                const unsigned type=d[i+4],usage=d[i+6],index=d[i+7];
                if(d[i] || d[i+1] || d[i+5])return 0;
                if(usage==0 && index==0 && (type==2 || type==3))fvf|=type==2?2U:0x4002U;
                else if(usage==9 && index==0 && type==3)fvf|=4;
                else if(usage==1 && index==0 && type<4)weights=type+1;
                else if(usage==2 && index==0 && (type==0 || type==4 || type==5))index_type=type+1;
                else if(usage==3 && index==0 && type==2)fvf|=0x10;
                else if(usage==4 && index==0 && type==0)fvf|=0x20;
                else if(usage==10 && index<2 && type==4)fvf|=index?0x80:0x40;
                else if(usage==5 && index<8 && type<4){constexpr unsigned codes[]{3,0,1,2};fvf|=codes[type]<<(16+2*index);tex_count=std::max(tex_count,index+1);}
                else return 0;
            }
            if(weights || index_type){if((fvf&0x400e)!=2)return 0;const unsigned betas=weights+(index_type?1U:0U);if(betas>5 || (index_type==1 && weights!=4))return 0;fvf=(fvf&~0xeU)|(4+2*betas);if(index_type==5)fvf|=0x8000;if(index_type==6)fvf|=0x1000;}
            fvf|=tex_count<<8;
            if(!(fvf&0x400e))return 0;
            try{return fvf_decl(fvf)==d?fvf:0;}catch(const Failure&){return 0;}
        };
        const auto make_mesh=[&](unsigned faces,unsigned vertices,unsigned flags,const std::vector<std::uint8_t>& decl,std::uint64_t device){
            (void)get(device,Kind::Device);if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS") && (!faces || !vertices || (!(flags&1) && vertices>65536)))std::fprintf(stderr,"[d3dx9 mesh] faces=%u vertices=%u flags=%x\n",faces,vertices,flags);require(faces && vertices && faces<=16777216 && vertices<=16777216 && ((flags&1) || vertices<=65536));
            const auto stride=stride_of(decl);auto mesh=create(Kind::Mesh,device);mesh->width=vertices;mesh->height=faces;mesh->flags=flags;mesh->stream_stride=stride;mesh->presentation=decl;mesh->fvf=decl_fvf(decl);
            try{auto vb=create(Kind::VertexBuffer,device);mesh->vertex_buffer=vb->root;vb->pool=(flags&0x10)?2:(flags&0x20)?1:0;vb->usage=(flags&0x40)?8:0;vb->format=100;vb->storage=reserve_gpu_memory(std::size_t(vertices)*stride);vb->pixels.resize(std::size_t(vertices)*stride);
                auto ib=create(Kind::IndexBuffer,device);mesh->index_buffer=ib->root;ib->pool=(flags&0x100)?2:(flags&0x200)?1:0;ib->format=(flags&1)?102:101;ib->storage=reserve_gpu_memory(std::size_t(faces)*3*((flags&1)?4:2));ib->pixels.resize(std::size_t(faces)*3*((flags&1)?4:2));mesh->pixels.resize(std::size_t(faces)*4);
            }catch(...){release(mesh->root);throw;}return mesh;
        };
        const auto mesh_indices=[&](const Object& mesh){auto ib=get(mesh.index_buffer,Kind::IndexBuffer);require(!ib->mapped);std::vector<unsigned> values(mesh.height*3);const unsigned bytes=(mesh.flags&1)?4:2;for(unsigned i=0;i<values.size();++i){values[i]=bytes==4?word(ib->pixels,i*4):unsigned(ib->pixels[i*2])|(unsigned(ib->pixels[i*2+1])<<8);require(values[i]<mesh.width);}return values;};
        const auto offset_of=[](const Object& mesh,unsigned usage,unsigned index){for(std::size_t i=0;i+8<mesh.presentation.size();i+=8)if(mesh.presentation[i+6]==usage && mesh.presentation[i+7]==index)return unsigned(i);return UINT32_MAX;};
        const auto positions=[&](const Object& mesh,unsigned usage=0,unsigned index=0,unsigned components=3){
            const auto e=offset_of(mesh,usage,index);require(e!=UINT32_MAX && mesh.presentation[e+4]<4 && unsigned(mesh.presentation[e+4])+1>=components);const auto at=unsigned(mesh.presentation[e+2]|mesh.presentation[e+3]<<8);auto vb=get(mesh.vertex_buffer,Kind::VertexBuffer);require(!vb->mapped);std::vector<Vector> values(mesh.width);for(unsigned i=0;i<mesh.width;++i)for(unsigned c=0;c<components;++c)values[i][c]=real(word(vb->pixels,std::size_t(i)*mesh.stream_stride+at+c*4));return values;
        };
        const auto write_vectors=[&](Object& mesh,unsigned usage,unsigned index,const std::vector<Vector>& values){const auto e=offset_of(mesh,usage,index);require(e!=UINT32_MAX && mesh.presentation[e+4]>=2 && mesh.presentation[e+4]<=3);const auto at=unsigned(mesh.presentation[e+2]|mesh.presentation[e+3]<<8);auto vb=get(mesh.vertex_buffer,Kind::VertexBuffer);require(!vb->mapped);for(unsigned i=0;i<mesh.width;++i)for(unsigned c=0;c<3;++c)put(vb->pixels,std::size_t(i)*mesh.stream_stride+at+c*4,bits(values[i][c]));};
        const auto subtract=[](Vector a,const Vector& b){for(unsigned c=0;c<3;++c)a[c]-=b[c];return a;};
        const auto cross3=[](const Vector& a,const Vector& b){return Vector{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0],0};};
        const auto dot3=[](const Vector& a,const Vector& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
        const auto normalize=[&](Vector a){const auto length=std::sqrt(dot3(a,a));if(length>0)for(unsigned c=0;c<3;++c)a[c]/=length;return a;};
        const auto adjacency_from_reps=[&](const std::vector<unsigned>& ix,const std::vector<unsigned>& reps){
            std::vector<unsigned> out(ix.size(),UINT32_MAX);std::map<std::pair<unsigned,unsigned>,std::vector<unsigned>> edges;
            for(unsigned i=0;i<ix.size();++i){const auto a=reps[ix[i]],b=reps[ix[i/3*3+(i%3+1)%3]];if(a==b)continue;auto& reverse=edges[{b,a}];auto found=std::find_if(reverse.begin(),reverse.end(),[&](unsigned j){return j/3!=i/3;});if(found!=reverse.end()){const auto j=*found;reverse.erase(found);out[i]=j/3;out[j]=i/3;}else edges[{a,b}].push_back(i);}return out;
        };
        const auto adjacency=[&](const Object& mesh,float epsilon){
            require(std::isfinite(epsilon) && epsilon>=0);const auto p=positions(mesh);const auto ix=mesh_indices(mesh);std::vector<unsigned> reps(mesh.width);std::map<std::array<long long,3>,std::vector<unsigned>> cells;
            for(unsigned i=0;i<p.size();++i){reps[i]=i;if(epsilon==0){std::array<long long,3> key{bits(p[i][0]),bits(p[i][1]),bits(p[i][2])};auto& bucket=cells[key];if(bucket.empty())bucket.push_back(i);else reps[i]=bucket.front();continue;}
                std::array<long long,3> key{};for(unsigned c=0;c<3;++c){require(std::isfinite(p[i][c]) && std::abs(double(p[i][c])/epsilon)<9e18);key[c]=static_cast<long long>(std::floor(double(p[i][c])/epsilon));}
                for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){const auto it=cells.find({key[0]+x,key[1]+y,key[2]+z});if(it!=cells.end())for(auto j:it->second){auto delta=subtract(p[i],p[j]);if(dot3(delta,delta)<=epsilon*epsilon)reps[i]=std::min(reps[i],j);}}
                cells[key].push_back(i);
            }
            return adjacency_from_reps(ix,reps);
        };
        const auto scalar_arg=[&](unsigned i){std::uint32_t v=u(i);if(!x86_ && i<4)std::memcpy(&v,state.xmm[i].data(),4);return real(v);};
        const auto blob=[&](std::span<const std::uint8_t> bytes){auto value=create(Kind::Blob);value->pixels.assign(bytes.begin(),bytes.end());return value;};
#include "d3dx9_loaded_mesh.inc"
        bool shader_handled=false;
#include "d3dx9_shader_handlers.inc"
#include "d3dx9_texture_handlers.inc"
#include "d3dx9_texture_fill.inc"
#include "d3dx9_sprite_handlers.inc"
#include "d3dx9_render_surface.inc"
#include "d3dx9_render_env.inc"
#include "d3dx9_effect_handlers.inc"
#include "d3dx9_mesh_clean.inc"
#include "d3dx9_mesh_tessellation.inc"
#include "d3dx9_text_mesh.inc"
#include "d3dx9_mesh_strips.inc"
#include "d3dx9_mesh_concatenate.inc"
#include "d3dx9_file_handlers.inc"
#include "d3dx9_atlas_handlers.inc"
#include "d3dx9_gutter_handlers.inc"
#include "d3dx9_skin_handlers.inc"
#include "d3dx9_animation_set.inc"
#include "d3dx9_animation_controller.inc"
#include "d3dx9_frame_handlers.inc"
#include "d3dx9_hierarchy_load.inc"
#include "d3dx9_save_mesh.inc"
        bool prt_handled=false;
#include "d3dx9_prt_handlers.inc"
#include "d3dx9_prt_comp_handlers.inc"
#include "d3dx9_prt_engine_handlers.inc"
        if(concatenate_handled || render_env_handled || hierarchy_handled || animation_set_handled || animation_controller_handled || frame_handled || skin_handled || gutter_handled || save_mesh_handled || atlas_handled || file_handled || render_surface_handled || effect_handled || sprite_handled || shader_handled || prt_handled || texture_handled || clean_handled || tessellation_handled || text_mesh_handled || strips_handled){}
        else if(symbol.starts_with("D3DXLoadMeshFromX")){
            const bool from_memory=symbol.ends_with("InMemory");const unsigned shift=from_memory?1:0;output(7+shift);for(unsigned i=3+shift;i<6+shift;++i)if(arg(i))output(i);if(arg(6+shift))memory.store(GuestAddress{arg(6+shift)},32,0);
            std::vector<std::uint8_t> bytes;if(from_memory){require(arg(1)<=512U*1024U*1024U);bytes=read(arg(0),arg(1));}
            else{const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));const auto opened=filesystem_.open_reader(d3dx_path(path));require(!opened.error,0x80070000U|opened.error);const auto meta=opened.file->information();require(!meta.error && meta.metadata.size<=512U*1024U*1024U);bytes.resize(meta.metadata.size);const auto io=opened.file->read(bytes,0);require(!io.error && io.bytes==bytes.size());}
            const auto decoded=d3d9_xfile::decompress(bytes);auto source=d3d9_xfile::Parser(decoded).parse();auto mesh=make_loaded_mesh(source,u(1+shift),arg(2+shift));std::shared_ptr<Object> adj_buffer,materials,effects;
            try{
                if(arg(3+shift)){const auto adj=adjacency(*mesh,1e-6F);adj_buffer=blob(std::span(reinterpret_cast<const std::uint8_t*>(adj.data()),adj.size()*4));}
                if(arg(4+shift))materials=material_buffer(source.materials);
                if(arg(5+shift))effects=effect_buffer(source.materials);
                if(adj_buffer)store_pointer(arg(3+shift),adj_buffer->root);if(materials)store_pointer(arg(4+shift),materials->root);if(effects)store_pointer(arg(5+shift),effects->root);
                if(arg(6+shift))memory.store(GuestAddress{arg(6+shift)},32,source.materials.size());store_pointer(arg(7+shift),mesh->root);
            }catch(...){if(adj_buffer)release(adj_buffer->root);if(materials)release(materials->root);if(effects)release(effects->root);release(mesh->root);throw;}
        }else if(symbol=="D3DXCreateBuffer"){
            output(1);require(u(0)!=0);store_pointer(arg(1),blob(std::vector<std::uint8_t>(u(0)))->root);
        }else if(symbol=="D3DXGetDeclVertexSize"){
            require(arg(0));result=0;for(unsigned i=0;i<65;++i){const auto e=read(arg(0)+i*8,8);const unsigned stream=unsigned(e[0])|(unsigned(e[1])<<8);if(stream==255 && e[4]==17)break;require(i<64 && e[4]<17);if(stream==u(1))result=std::max(result,std::uint64_t(unsigned(e[2])|(unsigned(e[3])<<8))+element_bytes(e[4]));}
        }else if(symbol=="D3DXIntersect" || symbol=="D3DXIntersectSubset"){
            const bool subset=symbol=="D3DXIntersectSubset";const unsigned shift=subset?1:0;auto mesh=get(arg(0),Kind::Mesh);require(arg(1+shift) && arg(2+shift) && arg(3+shift));
            if(arg(8+shift))output(8+shift);memory.store(GuestAddress{arg(3+shift)},32,0);if(arg(9+shift))memory.store(GuestAddress{arg(9+shift)},32,0);
            Vector origin{},direction{};for(unsigned c=0;c<3;++c){origin[c]=real(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1+shift)+c*4},32)));direction[c]=real(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2+shift)+c*4},32)));}
            const auto points=positions(*mesh);const auto ix=mesh_indices(*mesh);std::vector<std::array<std::uint32_t,4>> hits;
            for(unsigned face=0;face<mesh->height;++face){if(subset && word(mesh->pixels,face*4)!=u(1))continue;
                const auto a=ix[face*3],b=ix[face*3+1],c=ix[face*3+2];const auto e1=subtract(points[b],points[a]),e2=subtract(points[c],points[a]),h=cross3(direction,e2);const auto determinant=dot3(e1,h);if(std::abs(determinant)<1e-20F)continue;
                const auto delta=subtract(origin,points[a]);const float uv=dot3(delta,h)/determinant;if(uv<0 || uv>1)continue;const auto q=cross3(delta,e1);const float vv=dot3(direction,q)/determinant;if(vv<0 || uv+vv>1)continue;const float distance=dot3(e2,q)/determinant;if(distance<0)continue;hits.push_back({face,bits(uv),bits(vv),bits(distance)});
            }
            std::stable_sort(hits.begin(),hits.end(),[](const auto& a,const auto& b){return real(a[3])<real(b[3]);});
            memory.store(GuestAddress{arg(3+shift)},32,!hits.empty());if(arg(9+shift))memory.store(GuestAddress{arg(9+shift)},32,hits.size());
            if(!hits.empty()){for(unsigned c=0;c<4;++c)if(arg(4+shift+c))memory.store(GuestAddress{arg(4+shift+c)},32,hits.front()[c]);if(arg(8+shift))store_pointer(arg(8+shift),blob(std::span(reinterpret_cast<const std::uint8_t*>(hits.data()),hits.size()*16))->root);}
        }else if(symbol=="D3DXCreatePolygon"){
            const auto sides=u(2);const auto length=scalar_arg(1);require(arg(3) && sides>=3 && sides<65535 && std::isfinite(length) && length>=0);output(3);if(arg(4))output(4);auto mesh=make_mesh(sides,sides+1,0x220,fvf_decl(0x12),arg(0));mesh->fvf=0x12;
            try{auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);constexpr float pi=3.14159265358979323846F;const float radius=length/(2*std::sin(pi/static_cast<float>(sides)));
                for(unsigned i=0;i<=sides;++i){const float angle=2*pi*static_cast<float>(i?i-1:0)/static_cast<float>(sides);put(vb->pixels,i*24,bits(i?radius*std::cos(angle):0));put(vb->pixels,i*24+4,bits(i?radius*std::sin(angle):0));put(vb->pixels,i*24+20,bits(1.F));}
                for(unsigned i=0;i<sides;++i){const std::array<unsigned,3> indices{0,i+1,(i+1)%sides+1};for(unsigned c=0;c<3;++c){const auto at=(i*3+c)*2;ib->pixels[at]=static_cast<std::uint8_t>(indices[c]);ib->pixels[at+1]=static_cast<std::uint8_t>(indices[c]>>8);}}
                if(arg(4)){std::vector<unsigned> adj(sides*3);for(unsigned face=0;face<sides;++face){adj[face*3]=(face+sides-1)%sides;adj[face*3+1]=UINT32_MAX;adj[face*3+2]=(face+1)%sides;}store_pointer(arg(4),blob(std::span(reinterpret_cast<const std::uint8_t*>(adj.data()),adj.size()*4))->root);}store_pointer(arg(3),mesh->root);
            }catch(...){release(mesh->root);throw;}
        }else if(symbol=="D3DXGetDeclLength")result=declaration(arg(0)).size()/8-1;
        else if(symbol=="D3DXGetFVFVertexSize")result=stride_of(fvf_decl(u(0)));
        else if(symbol=="D3DXCreateMesh" || symbol=="D3DXCreateMeshFVF"){
            output(5);const bool fvf=symbol.ends_with("FVF");auto mesh=make_mesh(u(0),u(1),u(2),fvf?fvf_decl(u(3)):declaration(arg(3)),arg(4));if(fvf)mesh->fvf=u(3);store_pointer(arg(5),mesh->root);
        }else if(symbol=="D3DXComputeTangentFrameEx"){
#include "d3dx9_tangent_frame.inc"
        }else if(symbol=="D3DXComputeNormals"){
            auto mesh=get(arg(0),Kind::Mesh);const auto ix=mesh_indices(*mesh);const auto p=positions(*mesh);std::vector<Vector> normals(mesh->width);
            for(unsigned face=0;face<mesh->height;++face){const auto a=ix[face*3],b=ix[face*3+1],c=ix[face*3+2];const auto n=normalize(cross3(subtract(p[b],p[a]),subtract(p[c],p[a])));
                for(unsigned corner=0;corner<3;++corner){const auto v=ix[face*3+corner],next=ix[face*3+(corner+1)%3],prev=ix[face*3+(corner+2)%3];const auto weight=std::acos(std::clamp(dot3(normalize(subtract(p[next],p[v])),normalize(subtract(p[prev],p[v]))),-1.F,1.F));for(unsigned k=0;k<3;++k)normals[v][k]+=n[k]*weight;}}
            for(auto& n:normals)n=normalize(n);write_vectors(*mesh,3,0,normals);
        }else if(symbol.starts_with("Mesh.")){
            auto mesh=get(arg(0),Kind::Mesh);const auto name=symbol.substr(5);
            if(name=="DrawSubset"){
                auto device=get(mesh->parent,Kind::Device);require(device->scene && !mesh->mapped);auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);require(!vb->mapped && !ib->mapped);
                const auto ix=mesh_indices(*mesh);std::vector<GuestCallback> draws;
                const auto entry=resolve("internal-d3d9",device->ex?"DeviceEx.DrawIndexedPrimitive":"Device.DrawIndexedPrimitive").value;
                for(unsigned first=0;first<mesh->height;){if(word(mesh->pixels,first*4)!=u(1)){++first;continue;}unsigned end=first+1,minimum=UINT32_MAX,maximum=0;while(end<mesh->height && word(mesh->pixels,end*4)==u(1))++end;for(unsigned i=first*3;i<end*3;++i){minimum=std::min(minimum,ix[i]);maximum=std::max(maximum,ix[i]);}draws.push_back({entry,{device->root,4,0,minimum,maximum-minimum+1,first*3,end-first}});first=end;}
                if(!draws.empty()){
                    auto decl=create(Kind::VertexDeclaration,device->root);decl->pixels=mesh->presentation;++decl->bound;drop_binding(device->vertex_declaration);device->vertex_declaration=decl->root;release(decl->root);device->fvf=mesh->fvf;
                    for(auto pair:{std::pair{&device->vertex_buffer,vb},std::pair{&device->index_buffer,ib}})if(*pair.first!=pair.second->root){++pair.second->bound;drop_binding(*pair.first);*pair.first=pair.second->root;}
                    device->stream_offset=0;device->stream_stride=mesh->stream_stride;
                    start_callbacks(state,memory,std::move(draws),Continuation::ReturnUntilFailure);return std::nullopt;
                }
            }else if(name=="AddRef")result=hold(mesh);else if(name=="Release")result=release(mesh->root);
            else if(name=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);constexpr Guid mesh_iid{0xc2,0xe5,0x20,0x40,3,0x14,0x29,0x49,0x88,0x3f,0xe2,0xe8,0x49,0xfa,0xc1,0x95};constexpr Guid base_iid{0xdd,0x43,0xd9,0x7e,0xe8,0x52,0xb5,0x40,0xa8,0xd8,0x76,0x68,0x5c,0x40,0x63,0x30};if(requested==unknown || requested==mesh_iid || requested==base_iid){hold(mesh);store_pointer(arg(2),mesh->root);}else result=no_interface;}
            else if(name=="GetNumFaces")result=mesh->height;else if(name=="GetNumVertices")result=mesh->width;else if(name=="GetOptions")result=mesh->flags;else if(name=="GetFVF")result=mesh->fvf;else if(name=="GetNumBytesPerVertex")result=mesh->stream_stride;
            else if(name=="GetDeclaration")memory.write(GuestAddress{arg(1)},mesh->presentation);
            else if(name=="GetDevice" || name=="GetVertexBuffer" || name=="GetIndexBuffer"){const auto pointer=name=="GetDevice"?mesh->parent:name=="GetVertexBuffer"?mesh->vertex_buffer:mesh->index_buffer;output(1);hold(bridge.objects.at(pointer));store_pointer(arg(1),pointer);}
            else if(name.starts_with("Lock") || name.starts_with("Unlock")){
                const bool lock=name.starts_with("Lock"),vertex=name.find("Vertex")!=name.npos,index=name.find("Index")!=name.npos;auto buffer=vertex?get(mesh->vertex_buffer,Kind::VertexBuffer):index?get(mesh->index_buffer,Kind::IndexBuffer):mesh;
                if(lock){output(2);require(!buffer->mapped);buffer->mapped=allocate_heap(memory,buffer->pixels.size());require(buffer->mapped!=0,out_of_memory);memory.write(GuestAddress{buffer->mapped},buffer->pixels);buffer->lock_writable=!(u(1)&0x10);buffer->lock_count=1;store_pointer(arg(2),buffer->mapped);}
                else{require(buffer->mapped && buffer->lock_count==1);if(buffer->lock_writable)buffer->pixels=read(buffer->mapped,buffer->pixels.size());(void)free_heap(memory,buffer->mapped,state);buffer->mapped=0;buffer->lock_count=0;}
            }else if(name=="Optimize" || name=="OptimizeInplace"){
                const bool inplace=name=="OptimizeInplace";
                if(arg(5))store_pointer(arg(5),0);if(!inplace)output(6);
                auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);
                require(!mesh->mapped && !vb->mapped && !ib->mapped);
                if(inplace && !(u(1)&0x10000000))for(const auto& [pointer,other]:bridge.objects){(void)pointer;require(other->kind!=Kind::Mesh || other->root==mesh->root || other->vertex_buffer!=mesh->vertex_buffer);}
                const auto ix=mesh_indices(*mesh);std::vector<unsigned> adj,attrs(mesh->height);
                if(arg(2)){adj.resize(ix.size());memory.read(GuestAddress{arg(2)},std::span(reinterpret_cast<std::uint8_t*>(adj.data()),adj.size()*4));}
                else if(u(1)&0xc000000)adj=adjacency(*mesh,0.F);
                for(unsigned i=0;i<mesh->height;++i)attrs[i]=word(mesh->pixels,i*4);
                d3dx_mesh::Optimized optimized;const auto hr=d3dx_mesh::optimize(ix,mesh->width,adj,attrs,u(1)&0x3f400000,optimized);require(!(hr&0x80000000),hr);
                require(!(u(1)&~(inplace?0x3f400000U:0x3f400fffU)));
                require((mesh->flags&1) || optimized.vertices.size()<=65536);
                std::vector<std::uint8_t> vertices(optimized.vertices.size()*mesh->stream_stride),indices(ix.size()*((mesh->flags&1)?4:2)),attributes(mesh->pixels.size());
                for(unsigned i=0;i<optimized.vertices.size();++i)std::copy_n(vb->pixels.data()+std::size_t(optimized.vertices[i])*mesh->stream_stride,mesh->stream_stride,vertices.data()+std::size_t(i)*mesh->stream_stride);
                for(unsigned i=0;i<optimized.indices.size();++i){auto v=optimized.indices[i];if(mesh->flags&1)put(indices,i*4,v);else{indices[i*2]=static_cast<std::uint8_t>(v);indices[i*2+1]=static_cast<std::uint8_t>(v>>8);}}
                for(unsigned i=0;i<attrs.size();++i)put(attributes,i*4,optimized.attributes[i]);
                const auto write_words=[&](unsigned index,const auto& words){if(arg(index))memory.write(GuestAddress{arg(index)},std::span(reinterpret_cast<const std::uint8_t*>(words.data()),words.size()*4));};
                // 輸出先完成檢查；原地更新只在所有資料均已成功準備後提交。
                if(arg(3)){require(!optimized.adjacency.empty());write_words(3,optimized.adjacency);}write_words(4,optimized.faces);
                auto target=inplace?mesh:make_mesh(mesh->height,static_cast<unsigned>(optimized.vertices.size()),(mesh->flags&1)|(u(1)&0xffe),mesh->presentation,mesh->parent);
                try{
                    auto target_vb=get(target->vertex_buffer,Kind::VertexBuffer),target_ib=get(target->index_buffer,Kind::IndexBuffer);
                    auto storage=reserve_gpu_memory(vertices.size());
                    if(arg(5)){auto remap_blob=create(Kind::Blob);remap_blob->pixels.resize(optimized.vertices.size()*4);std::memcpy(remap_blob->pixels.data(),optimized.vertices.data(),remap_blob->pixels.size());store_pointer(arg(5),remap_blob->root);}
                    target_vb->storage=std::move(storage);target_vb->pixels=std::move(vertices);target_ib->pixels=std::move(indices);target->pixels=std::move(attributes);target->width=static_cast<unsigned>(optimized.vertices.size());target->fvf=mesh->fvf;target->attributes.clear();
                    if(!inplace)store_pointer(arg(6),target->root);
                }catch(...){if(!inplace)release(target->root);throw;}
            }else if(name=="GenerateAdjacency"){const auto values=adjacency(*mesh,scalar_arg(1));memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(values.data()),values.size()*4));}
            else if(name=="ConvertPointRepsToAdjacency" || name=="ConvertAdjacencyToPointReps"){
                require(arg(2));const auto ix=mesh_indices(*mesh);std::vector<unsigned> reps(mesh->width);for(unsigned i=0;i<mesh->width;++i)reps[i]=i;
                if(name=="ConvertPointRepsToAdjacency"){
                    if(arg(1)){const auto bytes=read(arg(1),std::uint64_t(mesh->width)*4);for(unsigned i=0;i<mesh->width;++i){reps[i]=word(bytes,i*4);require(reps[i]<mesh->width);}}
                    const auto out=adjacency_from_reps(ix,reps);memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));
                }else{
                    require(arg(1));const auto bytes=read(arg(1),std::uint64_t(ix.size())*4);const auto p=positions(*mesh);
                    const auto root=[&](unsigned v){while(reps[v]!=v){reps[v]=reps[reps[v]];v=reps[v];}return v;};
                    const auto join=[&](unsigned a,unsigned b){a=root(a);b=root(b);reps[std::max(a,b)]=std::min(a,b);};
                    for(unsigned i=0;i<ix.size();++i){const auto other=word(bytes,i*4),face=i/3;if(other==UINT32_MAX)continue;require(other<mesh->height && other!=face);if(other<face)continue;
                        const auto a=ix[i],b=ix[face*3+(i%3+1)%3];unsigned edge=UINT32_MAX;double distance=std::numeric_limits<double>::infinity();
                        // 依雙向鄰接配對共享邊；UV／法線接縫可有不同頂點索引。
                        for(unsigned j=0;j<3;++j)if(word(bytes,(other*3+j)*4)==face){const auto c=ix[other*3+(j+1)%3],d=ix[other*3+j];double error=0;for(unsigned k=0;k<3;++k){const double x=double(p[a][k])-p[c][k],y=double(p[b][k])-p[d][k];error+=x*x+y*y;}if(error<distance){distance=error;edge=j;}}
                        require(edge!=UINT32_MAX);join(a,ix[other*3+(edge+1)%3]);join(b,ix[other*3+edge]);
                    }
                    for(unsigned i=0;i<mesh->width;++i)reps[i]=root(i);memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(reps.data()),reps.size()*4));
                }
            }
            else if(name=="CloneMesh" || name=="CloneMeshFVF"){
                output(4);const bool fvf=name=="CloneMeshFVF";auto clone=make_mesh(mesh->height,mesh->width,u(1),fvf?fvf_decl(u(2)):declaration(arg(2)),arg(3));if(fvf)clone->fvf=u(2);
                try{auto from=get(mesh->vertex_buffer,Kind::VertexBuffer),to=get(clone->vertex_buffer,Kind::VertexBuffer);require(!from->mapped);for(std::size_t e=0;e+8<clone->presentation.size();e+=8){auto source=offset_of(*mesh,clone->presentation[e+6],clone->presentation[e+7]);if(source==UINT32_MAX)continue;const auto st=mesh->presentation[source+4],dt=clone->presentation[e+4];require(st==dt || (st<4 && dt<4));const unsigned size=std::min(element_bytes(st),element_bytes(dt)),s=unsigned(mesh->presentation[source+2])|unsigned(mesh->presentation[source+3])<<8,d=unsigned(clone->presentation[e+2])|unsigned(clone->presentation[e+3])<<8;for(unsigned i=0;i<mesh->width;++i)std::copy_n(from->pixels.data()+std::size_t(i)*mesh->stream_stride+s,size,to->pixels.data()+std::size_t(i)*clone->stream_stride+d);}
                    const auto ix=mesh_indices(*mesh);auto ib=get(clone->index_buffer,Kind::IndexBuffer);for(unsigned i=0;i<ix.size();++i)if(clone->flags&1)put(ib->pixels,i*4,ix[i]);else{require(ix[i]<=65535);ib->pixels[i*2]=static_cast<std::uint8_t>(ix[i]);ib->pixels[i*2+1]=static_cast<std::uint8_t>(ix[i]>>8);}clone->pixels=mesh->pixels;clone->attributes=mesh->attributes;
                    if(u(1)&0x1000){require(clone->presentation==mesh->presentation);hold(from);release(clone->vertex_buffer);clone->vertex_buffer=from->root;}
                    store_pointer(arg(4),clone->root);
                }catch(...){release(clone->root);throw;}
            }else if(name=="GetAttributeTable"){
                require(arg(1) || arg(2));const auto ix=mesh_indices(*mesh);std::vector<std::array<unsigned,5>> table;
                for(unsigned first=0;first<mesh->height;){const auto attr=word(mesh->pixels,first*4);unsigned end=first+1,minimum=UINT32_MAX,maximum=0;while(end<mesh->height && word(mesh->pixels,end*4)==attr)++end;for(unsigned i=first*3;i<end*3;++i){minimum=std::min(minimum,ix[i]);maximum=std::max(maximum,ix[i]);}table.push_back({attr,first,end-first,minimum,maximum-minimum+1});first=end;}
                if(!mesh->attributes.empty())table=mesh->attributes;if(arg(1)){if(arg(2))require(memory.load(GuestAddress{arg(2)},32)>=table.size());memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(table.data()),table.size()*20));}if(arg(2))memory.store(GuestAddress{arg(2)},32,table.size());
            }else if(name=="SetAttributeTable"){require(u(2)<=mesh->height);mesh->attributes.resize(u(2));memory.read(GuestAddress{arg(1)},std::span(reinterpret_cast<std::uint8_t*>(mesh->attributes.data()),mesh->attributes.size()*20));}
            else if(name=="UpdateSemantics"){auto d=declaration(arg(1));require(stride_of(d)==mesh->stream_stride);mesh->presentation=std::move(d);mesh->fvf=decl_fvf(mesh->presentation);}
            else throw HostCallFault("尚未實作 D3DX9 Mesh 方法："+std::string(name));
        }else if(symbol.starts_with("Blob.")){
            auto value=get(arg(0),Kind::Blob);const auto name=symbol.substr(5);
            if(name=="AddRef")result=hold(value);else if(name=="Release")result=release(value->root);
            else if(name=="GetBufferSize")result=value->pixels.size();
            else if(name=="GetBufferPointer"){if(!value->mapped){value->mapped=allocate_heap(memory,std::max<std::size_t>(1,value->pixels.size()));require(value->mapped!=0,out_of_memory);memory.write(GuestAddress{value->mapped},value->pixels);}result=value->mapped;}
            else if(name=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);constexpr Guid buffer_iid{0x08,0xfb,0xa5,0x8b,0x95,0x51,0xe2,0x40,0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02};if(requested==unknown || requested==buffer_iid){hold(value);store_pointer(arg(2),value->root);}else result=no_interface;}
        }else if(mesh_handled)throw HostCallFault("尚未實作 D3DX9 工具入口："+std::string(symbol));
