        const bool concatenate_handled=symbol=="D3DXConcatenateMeshes";
        if(concatenate_handled){
            output(7);require(arg(0) && u(1) && u(1)<=65536);(void)get(arg(6),Kind::Device);std::vector<std::shared_ptr<Object>> meshes;std::uint64_t vertex_count=0,face_count=0;
            std::vector<std::uint8_t> layout;unsigned stride=0;
            for(unsigned i=0;i<u(1);++i){auto mesh=get(memory.load(GuestAddress{arg(0)+i*ps},pb),Kind::Mesh);require(!get(mesh->vertex_buffer,Kind::VertexBuffer)->mapped && !get(mesh->index_buffer,Kind::IndexBuffer)->mapped);vertex_count+=mesh->width;face_count+=mesh->height;meshes.push_back(mesh);
                if(!arg(5))for(std::size_t e=0;e+8<mesh->presentation.size();e+=8){const auto& source=mesh->presentation;auto found=layout.size();for(std::size_t j=0;j<layout.size();j+=8)if(layout[j+6]==source[e+6] && layout[j+7]==source[e+7]){found=j;break;}
                    if(found==layout.size())layout.insert(layout.end(),source.begin()+static_cast<std::ptrdiff_t>(e),source.begin()+static_cast<std::ptrdiff_t>(e+8));
                    else if(layout[found+4]!=source[e+4]){require(layout[found+4]<4 && source[e+4]<4);layout[found+4]=std::max(layout[found+4],source[e+4]);}
                }
            }
            require(vertex_count<=UINT32_MAX && face_count<=UINT32_MAX/3 && ((u(2)&1) || vertex_count<=65536));
            if(arg(5))layout=declaration(arg(5));else {require(layout.size()<=64*8);for(std::size_t e=0;e<layout.size();e+=8){layout[e]=layout[e+1]=0;layout[e+2]=static_cast<std::uint8_t>(stride);layout[e+3]=static_cast<std::uint8_t>(stride>>8);stride+=element_bytes(layout[e+4]);}layout.insert(layout.end(),{255,0,0,0,17,0,0,0});}
            auto combined=make_mesh(static_cast<unsigned>(face_count),static_cast<unsigned>(vertex_count),u(2),layout,arg(6));
            try{auto vertices=get(combined->vertex_buffer,Kind::VertexBuffer),indices=get(combined->index_buffer,Kind::IndexBuffer);unsigned vertex_base=0,face_base=0,attribute_base=0;
                for(unsigned m=0;m<meshes.size();++m){const auto& mesh=*meshes[m];auto source=get(mesh.vertex_buffer,Kind::VertexBuffer);Matrix geometry=identity(),texture=identity(),normal=identity();if(arg(3))memory.read(GuestAddress{arg(3)+m*64ULL},std::span(reinterpret_cast<std::uint8_t*>(geometry.data()),64));if(arg(4))memory.read(GuestAddress{arg(4)+m*64ULL},std::span(reinterpret_cast<std::uint8_t*>(texture.data()),64));
                    if(arg(3)){for(float value:geometry)require(std::isfinite(value));const double a=geometry[0],b=geometry[1],c=geometry[2],d=geometry[4],e=geometry[5],f=geometry[6],g=geometry[8],h=geometry[9],i=geometry[10],det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);require(std::abs(det)>1e-30);
                        const std::array<double,9> cofactor{e*i-f*h,f*g-d*i,d*h-e*g,c*h-b*i,a*i-c*g,b*g-a*h,b*f-c*e,c*d-a*f,a*e-b*d};for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column)normal[row*4+column]=static_cast<float>(cofactor[row*3+column]/det);
                    }
                    for(std::size_t element=0;element+8<layout.size();element+=8){const auto original=offset_of(mesh,layout[element+6],layout[element+7]);if(original==UINT32_MAX)continue;const unsigned from_type=mesh.presentation[original+4],to_type=layout[element+4],from_offset=unsigned(mesh.presentation[original+2])|unsigned(mesh.presentation[original+3])<<8,to_offset=unsigned(layout[element+2])|unsigned(layout[element+3])<<8,usage=layout[element+6];require(from_type==to_type || (from_type<4 && to_type<4));
                        const bool geom=arg(3) && (usage==0 || usage==3 || usage==6 || usage==7 || usage==9),uv=arg(4) && usage==5;
                        if(geom || uv)require(from_type<4 && to_type<4);
                        for(unsigned vertex=0;vertex<mesh.width;++vertex){const auto from=std::size_t(vertex)*mesh.stream_stride+from_offset,to=std::size_t(vertex+vertex_base)*combined->stream_stride+to_offset;
                            if(!geom && !uv){std::copy_n(source->pixels.data()+from,std::min(element_bytes(from_type),element_bytes(to_type)),vertices->pixels.data()+to);continue;}
                            Vector value{0,0,0,(usage==3 || usage==6 || usage==7)?0.F:1.F};for(unsigned n=0;n<=from_type;++n)value[n]=real(word(source->pixels,from+n*4));value=transform(value,uv?texture:usage==3?normal:geometry);
                            if((usage==0 || usage==5) && to_type<3 && value[3]!=0)for(unsigned n=0;n<3;++n)value[n]/=value[3];for(unsigned n=0;n<=to_type;++n)put(vertices->pixels,to+n*4,bits(value[n]));
                        }
                    }
                    const auto source_indices=mesh_indices(mesh);for(unsigned n=0;n<source_indices.size();++n){const auto value=source_indices[n]+vertex_base,offset=face_base*3+n;if(combined->flags&1)put(indices->pixels,offset*4,value);else {indices->pixels[offset*2]=static_cast<std::uint8_t>(value);indices->pixels[offset*2+1]=static_cast<std::uint8_t>(value>>8);}}
                    unsigned count=0;for(unsigned face=0;face<mesh.height;++face){const auto attribute=word(mesh.pixels,face*4);require(attribute<UINT32_MAX-attribute_base);put(combined->pixels,(face_base+face)*4,attribute_base+attribute);count=std::max(count,attribute+1);}require(count<=UINT32_MAX-attribute_base);attribute_base+=count;vertex_base+=mesh.width;face_base+=mesh.height;
                }
                store_pointer(arg(7),combined->root);
            }catch(...){release(combined->root);throw;}
        }
