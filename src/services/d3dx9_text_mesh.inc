        bool text_mesh_handled=symbol=="D3DXCreateTextA" || symbol=="D3DXCreateTextW";
        if(text_mesh_handled){
            output(5);if(arg(6))output(6);(void)get(arg(0),Kind::Device);
            const auto dc=gui_.contexts.find(arg(1));require(dc!=gui_.contexts.end() && arg(2));
            const auto model=gui_font(gui_,dc->second.font);const auto face=native_resolve_font(model);require(bool(face) && face->em>0,unavailable);
            const auto deviation=scalar_arg(3),extrusion=scalar_arg(4);require(std::isfinite(deviation) && deviation>=0 && std::isfinite(extrusion) && extrusion>=0);
            const auto text=symbol.ends_with('W')?read_utf16(memory,GuestAddress{arg(2)}):utf8_to_utf16_lossy(read_ansi(memory,arg(2)));require(!text.empty() && text.size()<=65536);
            std::vector<std::uint32_t> characters(text.begin(),text.end());const auto glyphs=native_font_glyph_indices(*face,characters);
            const double tolerance=deviation!=0?deviation:1./1024.,scale=1./face->em;
            const double em=model.height<0?-static_cast<double>(model.height):model.height?static_cast<double>(model.height)*face->em/std::max(1,face->ascent+face->descent):13;const double width_scale=model.width?std::abs(static_cast<double>(model.width))*face->em/(std::max(1,face->average_width)*em):1;
            using Point=std::array<double,2>;std::vector<std::array<float,6>> vertices;std::vector<std::uint32_t> indices;double pen_x=0,pen_y=0;
            const auto midpoint=[](Point a,Point b){return Point{(a[0]+b[0])*.5,(a[1]+b[1])*.5};};
            const auto distance=[](Point p,Point a,Point b){const auto dx=b[0]-a[0],dy=b[1]-a[1],length=dx*dx+dy*dy;if(length==0)return std::hypot(p[0]-a[0],p[1]-a[1]);const auto t=std::clamp(((p[0]-a[0])*dx+(p[1]-a[1])*dy)/length,0.,1.);return std::hypot(p[0]-a[0]-t*dx,p[1]-a[1]-t*dy);};
            const auto triangle=[&](std::array<float,3> a,std::array<float,3> b,std::array<float,3> c,std::array<float,3> normal){
                require(vertices.size()<3000000,out_of_memory);for(const auto& p:{a,b,c}){indices.push_back(static_cast<std::uint32_t>(vertices.size()));vertices.push_back({p[0],p[1],p[2],normal[0],normal[1],normal[2]});}
            };
            for(unsigned character=0;character<glyphs.size();++character){
                const auto glyph=native_gdi_glyph(*face,glyphs[character],{width_scale,0,0,1},0);
                std::vector<std::vector<Point>> contours;
                const auto normalized=[&](Point p){return Point{p[0]*scale,p[1]*scale};};
                for(const auto& outline:glyph.contours){
                    std::vector<Point> points{normalized(outline.start)};Point current=points.back();
                    std::function<void(Point,Point,Point,Point,unsigned)> cubic;
                    cubic=[&](Point a,Point b,Point c,Point d,unsigned depth){
                        const auto error=std::max(distance(b,a,d),distance(c,a,d));require(depth<20 || error<=tolerance,out_of_memory);if(error<=tolerance){require(points.size()<1048576,out_of_memory);points.push_back(d);return;}
                        const auto ab=midpoint(a,b),bc=midpoint(b,c),cd=midpoint(c,d),abc=midpoint(ab,bc),bcd=midpoint(bc,cd),center=midpoint(abc,bcd);
                        cubic(a,ab,abc,center,depth+1);cubic(center,bcd,cd,d,depth+1);
                    };
                    for(const auto& segment:outline.segments){
                        if(segment.kind==1){current=normalized(segment.points[0]);points.push_back(current);}
                        else if(segment.kind==2){const auto control=normalized(segment.points[0]),end=normalized(segment.points[1]);const Point b{current[0]+(control[0]-current[0])*2/3,current[1]+(control[1]-current[1])*2/3},c{end[0]+(control[0]-end[0])*2/3,end[1]+(control[1]-end[1])*2/3};cubic(current,b,c,end,0);current=end;}
                        else {require(segment.kind==3);const auto end=normalized(segment.points[2]);cubic(current,normalized(segment.points[0]),normalized(segment.points[1]),end,0);current=end;}
                    }
                    points.erase(std::unique(points.begin(),points.end()),points.end());if(points.size()>1 && points.front()==points.back())points.pop_back();if(points.size()>=3)contours.push_back(std::move(points));
                }
                // 統一外框逆時針、孔洞順時針；側面法線因此一律指向填色區域外側。
                const auto contains=[](const std::vector<Point>& polygon,Point p){bool inside=false;for(std::size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++){const auto a=polygon[i],b=polygon[j];if((a[1]>p[1])!=(b[1]>p[1]) && p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0])inside=!inside;}return inside;};
                for(unsigned i=0;i<contours.size();++i){auto& contour=contours[i];unsigned depth=0;for(unsigned j=0;j<contours.size();++j)if(i!=j && contains(contours[j],contour.front()))++depth;double area=0;for(unsigned j=0;j<contour.size();++j){const auto a=contour[j],b=contour[(j+1)%contour.size()];area+=a[0]*b[1]-b[0]*a[1];}if((area>0)!=(depth%2==0))std::reverse(contour.begin(),contour.end());}
                const auto position=[&](Point p,float z){return std::array<float,3>{static_cast<float>(p[0]+pen_x),static_cast<float>(p[1]+pen_y),z};};
                if(!contours.empty()){
                    const auto caps=native_polygon_triangles(contours);
                    for(std::size_t i=0;i<caps.size();i+=3){triangle(position(caps[i],0),position(caps[i+1],0),position(caps[i+2],0),{0,0,1});if(extrusion!=0)triangle(position(caps[i+2],-extrusion),position(caps[i+1],-extrusion),position(caps[i],-extrusion),{0,0,-1});}
                    if(extrusion!=0)for(const auto& contour:contours)for(unsigned i=0;i<contour.size();++i){const auto a=contour[i],b=contour[(i+1)%contour.size()];const auto dx=b[0]-a[0],dy=b[1]-a[1],length=std::hypot(dx,dy);if(length==0)continue;const std::array<float,3> normal{static_cast<float>(dy/length),static_cast<float>(-dx/length),0};triangle(position(a,0),position(a,-extrusion),position(b,0),normal);triangle(position(b,0),position(a,-extrusion),position(b,-extrusion),normal);}
                }
                if(arg(7)){const std::array<float,6> metrics{static_cast<float>((glyph.bounds[2]-glyph.bounds[0])*scale),static_cast<float>((glyph.bounds[3]-glyph.bounds[1])*scale),static_cast<float>(glyph.bounds[0]*scale),static_cast<float>(glyph.bounds[3]*scale),static_cast<float>(glyph.advance[0]*scale),static_cast<float>(glyph.advance[1]*scale)};memory.write(GuestAddress{arg(7)+character*24},std::span(reinterpret_cast<const std::uint8_t*>(metrics.data()),24));}
                pen_x+=glyph.advance[0]*scale;pen_y+=glyph.advance[1]*scale;
            }
            require(!indices.empty());const unsigned flags=0x220U|(vertices.size()>65536?1U:0U);auto mesh=make_mesh(static_cast<unsigned>(indices.size()/3),static_cast<unsigned>(vertices.size()),flags,fvf_decl(0x12),arg(0));mesh->fvf=0x12;
            try{auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);std::memcpy(vb->pixels.data(),vertices.data(),vertices.size()*24);for(unsigned i=0;i<indices.size();++i)if(flags&1)put(ib->pixels,i*4,indices[i]);else{ib->pixels[i*2]=static_cast<std::uint8_t>(indices[i]);ib->pixels[i*2+1]=static_cast<std::uint8_t>(indices[i]>>8);}
                if(arg(6)){const auto adjacent=adjacency(*mesh,1e-6F);store_pointer(arg(6),blob(std::span(reinterpret_cast<const std::uint8_t*>(adjacent.data()),adjacent.size()*4))->root);}store_pointer(arg(5),mesh->root);
            }catch(...){release(mesh->root);throw;}
        }
