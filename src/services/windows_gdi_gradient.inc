    else if(symbol=="GradientFill" || symbol=="GdiGradientFill"){
        const auto vertices=d(2),meshes=d(4),mode=d(5);const unsigned corners=mode==2?3:2;
        if(!known_dc)last_error_=6;
        else if(mode>2 || (vertices && !arg(1)) || (meshes && (!arg(3) || !vertices)))last_error_=87;
        else if(vertices>1048576 || meshes>1048576)last_error_=8;
        else if(gui_.emf_recorders.contains(arg(0)))last_error_=50;
        else if(!meshes)result=1;
        else {
            memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(vertices)*16,Access::Read);memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(meshes)*corners*4,Access::Read);
            struct Vertex {double x{},y{};std::array<double,3> color{};};std::vector<Vertex> points(vertices);std::vector<std::array<std::uint32_t,3>> connections(meshes);bool valid=true;
            for(unsigned i=0;i<vertices;++i){const auto at=arg(1)+i*16;auto& v=points[i];v.x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)));v.y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32)));for(unsigned c=0;c<3;++c)v.color[c]=static_cast<double>(memory.load(GuestAddress{at+8+c*2},16))/256.0;}
            for(unsigned i=0;i<meshes;++i)for(unsigned c=0;c<corners;++c){const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+(i*corners+c)*4},32));connections[i][c]=index;valid&=index<vertices;}
            const auto transform=gui_device_transform(dc);const auto determinant=transform[0]*transform[3]-transform[1]*transform[2];
            if(!valid)last_error_=87;
            else if(std::any_of(transform.begin(),transform.end(),[](double value){return !std::isfinite(value);}))last_error_=87;
            else if(std::abs(determinant)<1e-15)result=1;
            else if(const auto destination=raster_surface(memory,arg(0));!destination)last_error_=50;
            else {
                result=1;auto device=dc;device.world=gui_identity;device.map_mode=1;device.window_origin={};device.viewport_origin={};device.window_extent={1,1};device.viewport_extent={1,1};
                for(const auto& mesh:connections){const auto& a=points[mesh[0]];const auto& b=points[mesh[1]];const auto& c=points[mesh[2]];
                    const auto area=mode==2?(b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y):(b.x-a.x)*(b.y-a.y);if(std::abs(area)<1e-15)continue;
                    std::array<std::array<double,2>,4> bounds{};const auto mapped=[&](double x,double y){return std::array<double,2>{transform[0]*x+transform[2]*y+transform[4],transform[1]*x+transform[3]*y+transform[5]};};
                    bounds[0]=mapped(a.x,a.y);bounds[1]=mapped(b.x,b.y);bounds[2]=mode==2?mapped(c.x,c.y):mapped(a.x,b.y);bounds[3]=mode==2?bounds[2]:mapped(b.x,a.y);
                    double min_x=bounds[0][0],max_x=min_x,min_y=bounds[0][1],max_y=min_y;for(const auto& p:bounds){min_x=std::min(min_x,p[0]);max_x=std::max(max_x,p[0]);min_y=std::min(min_y,p[1]);max_y=std::max(max_y,p[1]);}
                    const auto edge=[](double value,std::uint32_t limit,bool upper){return static_cast<std::int32_t>(std::clamp(upper?std::ceil(value):std::floor(value),0.0,static_cast<double>(limit)));};
                    const std::array<std::int32_t,4> rect{edge(min_x,destination->width(),false),edge(min_y,destination->height(),false),edge(max_x,destination->width(),true),edge(max_y,destination->height(),true)};const auto width=rect[2]-rect[0],height=rect[3]-rect[1];if(width<=0 || height<=0)continue;
                    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width)*static_cast<std::size_t>(height));
                    for(std::int32_t py=0;py<height;++py)for(std::int32_t px=0;px<width;++px){const double x=rect[0]+px+0.5-transform[4],y=rect[1]+py+0.5-transform[5];const double lx=(transform[3]*x-transform[2]*y)/determinant,ly=(transform[0]*y-transform[1]*x)/determinant;
                        double wa=0,wb=0,wc=0;if(mode==2){wb=((lx-a.x)*(c.y-a.y)-(c.x-a.x)*(ly-a.y))/area;wc=((b.x-a.x)*(ly-a.y)-(lx-a.x)*(b.y-a.y))/area;wa=1-wb-wc;if(wa<0 || wb<0 || wc<0)continue;}
                        else {if(lx<std::min(a.x,b.x) || lx>=std::max(a.x,b.x) || ly<std::min(a.y,b.y) || ly>=std::max(a.y,b.y))continue;wb=mode==0?(lx-a.x)/(b.x-a.x):(ly-a.y)/(b.y-a.y);wa=1-wb;}
                        std::uint32_t color=0xff000000U;for(unsigned channel=0;channel<3;++channel)color|=static_cast<std::uint32_t>(std::clamp(std::round(a.color[channel]*wa+b.color[channel]*wb+(mode==2?c.color[channel]*wc:0)),0.0,255.0))<<(channel*8);
                        pixels[static_cast<std::size_t>(py)*static_cast<std::size_t>(width)+static_cast<std::size_t>(px)]=color;
                    }
                    const auto source=make_hardware_raster(static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height));source->set_dib_alpha(true);source->upload(pixels,true);
                    destination->blit(*source,rect,{0,0,width,height},device,0x01ff0000U);
                }
                if(window!=gui_.windows.end())present_gui(window->first);
            }
        }
    }
