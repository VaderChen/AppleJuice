        if(symbol=="D3DXFillTexture" || symbol=="D3DXFillCubeTexture" || symbol=="D3DXSHProjectCubeMap"){
            texture_handled=true;mesh_handled=true;
            const auto cube_direction=[](unsigned face,float u,float v){
                switch(face){case 0:return Vector{1,-v,-u,0};case 1:return Vector{-1,-v,u,0};case 2:return Vector{u,1,v,0};case 3:return Vector{u,-1,-v,0};case 4:return Vector{u,-v,1,0};default:return Vector{-u,-v,-1,0};}
            };
            if(symbol=="D3DXSHProjectCubeMap"){
                const unsigned order=u(0);require(order>=2 && order<=6 && arg(2) && arg(3) && arg(4));auto texture=get(arg(1),Kind::CubeTexture);
                std::array<std::array<double,36>,3> sum{};const auto edge=texture->width;
                const auto area=[](double x,double y){return std::atan2(x*y,std::sqrt(x*x+y*y+1));};
                for(unsigned face=0;face<6;++face){auto surface=get(texture->subresources.at(face*texture->levels),Kind::Surface);require(!surface->mapped);read_surface(*surface);
                    const auto rgba=texture_image::transform(surface->pixels,native_format(surface->format),{edge,edge,1},{0,0,0,edge,edge,1},2,{edge,edge,1},1);
                    for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x){const double left=2.0*x/edge-1,right=2.0*(x+1)/edge-1,top=2.0*y/edge-1,bottom=2.0*(y+1)/edge-1;
                        const double omega=area(right,bottom)-area(left,bottom)-area(right,top)+area(left,top);
                        auto direction=cube_direction(face,static_cast<float>((left+right)*.5),static_cast<float>((top+bottom)*.5));const auto length=std::sqrt(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);for(unsigned c=0;c<3;++c)direction[c]/=length;
                        std::array<float,36> basis{};sh_evaluate(basis.data(),order,direction.data());
                        for(unsigned c=0;c<3;++c){const auto value=real(word(rgba,(std::size_t(y)*edge+x)*16+c*4));for(unsigned i=0;i<order*order;++i)sum[c][i]+=omega*value*basis[i];}
                    }
                }
                for(unsigned c=0;c<3;++c){std::array<float,36> coefficients{};for(unsigned i=0;i<order*order;++i)coefficients[i]=static_cast<float>(sum[c][i]);memory.write(GuestAddress{arg(c+2)},std::span(reinterpret_cast<const std::uint8_t*>(coefficients.data()),order*order*4));
                    if(std::getenv("X64ARM_TRACE_D3DX_SH")){std::fprintf(stderr,"[d3dx9 sh] order=%u channel=%u",order,c);for(unsigned i=0;i<order*order;++i)std::fprintf(stderr," %.9g",double(coefficients[i]));std::fprintf(stderr,"\n");}}
            }else{
                const bool cube=symbol=="D3DXFillCubeTexture";auto texture=get(arg(0),cube?Kind::CubeTexture:Kind::Texture);require(arg(1));
                D3DXFillOperation* operation=nullptr;for(auto& [id,item]:bridge.fill_operations){(void)id;if(item.thread==threads_.current && item.stack==state.reg(Gpr::Rsp) && item.service.symbol==service.symbol){operation=&item;break;}}
                if(!operation){D3DXFillOperation next;next.id=++bridge.next_atlas_operation;next.thread=threads_.current;next.stack=state.reg(Gpr::Rsp);next.service=service;operation=&bridge.fill_operations.emplace(next.id,std::move(next)).first->second;operation->temporary=allocate_heap(memory,128*48);require(operation->temporary,out_of_memory);}
                auto& work=*operation;auto surface=get(texture->subresources.at(work.subresource),Kind::Surface);require(!surface->mapped);const auto count=surface->width*surface->height;
                if(work.pixels.empty())work.pixels.resize(std::size_t(count)*16);
                if(work.returned){for(unsigned n=0;n<work.batch;++n)memory.read(GuestAddress{work.temporary+n*48},std::span(work.pixels).subspan(std::size_t(work.index+n)*16,16));work.index+=work.batch;work.returned=false;}
                if(work.index==count){surface->pixels=texture_image::transform(work.pixels,2,{surface->width,surface->height,1},{0,0,0,surface->width,surface->height,1},native_format(surface->format),{surface->width,surface->height,1},1);write_surface(*surface);work.pixels.clear();work.index=0;++work.subresource;}
                if(work.subresource<texture->subresources.size()){
                    surface=get(texture->subresources.at(work.subresource),Kind::Surface);require(!surface->mapped);const auto width=surface->width,height=surface->height,face=work.subresource/texture->levels;const float dx=1.0F/static_cast<float>(width),dy=1.0F/static_cast<float>(height);
                    work.batch=std::min(128U,width*height-work.index);std::vector<GuestCallback> callbacks;callbacks.reserve(work.batch);
                    for(unsigned n=0;n<work.batch;++n){const auto pixel=work.index+n;const float x=(static_cast<float>(pixel%width)+.5F)*dx,y=(static_cast<float>(pixel/width)+.5F)*dy;Vector position=cube?cube_direction(face,2*x-1,2*y-1):Vector{x,y,0,0};Vector size=cube?Vector{2*dx,2*dy,2*dx,0}:Vector{dx,dy,0,0};if(cube)size[face/2]=0;
                        std::array<float,12> data{};std::copy_n(position.begin(),3,data.begin()+4);std::copy_n(size.begin(),3,data.begin()+8);const auto at=work.temporary+n*48;memory.write(GuestAddress{at},std::span(reinterpret_cast<const std::uint8_t*>(data.data()),48));callbacks.push_back({arg(1),{at,at+16,at+32,arg(2)}});}
                    start_callbacks(state,memory,std::move(callbacks),Continuation::D3DXProgressOperation,work.id);return std::nullopt;
                }
            }
        }
