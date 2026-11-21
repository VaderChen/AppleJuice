            const bool direction=symbol=="D3DXSHEvalDirection";
            const bool hemi=symbol=="D3DXSHEvalHemisphereLight";
            const bool sphere=symbol=="D3DXSHEvalSphericalLight";
            const bool cone=symbol=="D3DXSHEvalConeLight";
            const unsigned order=static_cast<unsigned>(arg(direction?1:0));
            const bool valid=order>=2 && order<=6;
            const unsigned red_index=hemi?4U:(sphere || cone)?6U:5U;
            result=direction?arg(0):0;
            if(valid || (hemi && order)){
                const auto input=arg(direction?2:1), red=arg(direction?0:red_index);
                if(!direction && (!input || !red)){arg.return_to(state,0x8876086cU);return std::nullopt;}
                auto v=load(input,3);
                std::array<float,36> basis{};
                if(direction){
                    sh_light_basis(basis.data(),order,v[0],v[1],v[2]);
                    store(red,basis,order*order);
                }else{
                    const std::array outputs{red,arg(red_index+1),arg(red_index+2)};
                    const auto put=[&](unsigned c,std::uint64_t i,float value){
                        memory.store(GuestAddress{outputs[c]+4*i},32,std::bit_cast<std::uint32_t>(value));
                    };
                    if(hemi){
                        const auto top=load(arg(2),4),bottom=load(arg(3),4);
                        sh_light_basis(basis.data(),2,v[0],v[1],v[2]);
                        for(unsigned c=0;c<3;++c)if(outputs[c]){
                            for(std::uint64_t i=0;i<std::uint64_t{order}*order;++i)
                                put(c,i,sh_light_hemisphere(i,basis.data(),top[c],bottom[c]));
                        }
                    }else{
                        const unsigned first_color=red_index-3;
                        const Vector color{f(first_color),f(first_color+1),f(first_color+2),0};
                        float radius=sphere || cone?f(2):0.0f;
                        const bool directional=!sphere && (!cone || radius<=0);
                        std::array<float,6> weights{};
                        if(sphere){
                            const float distance=std::sqrt(dot(v,v));
                            sh_light_sphere(weights.data(),order,distance,std::fabs(radius));
                            if(distance!=0.0f)for(unsigned k=0;k<3;++k)v[k]/=distance;
                        }else if(!directional){
                            const double s=std::sin(static_cast<double>(radius)),half=std::sin(0.5*radius);
                            sh_light_cap(weights.data(),order,std::cos(static_cast<double>(radius)),s*s,
                                         2*half*half,radius<=SH_LIGHT_PI/2?s*s:1);
                        }
                        sh_light_basis(basis.data(),order,v[0],v[1],v[2]);
                        store(red,basis,order*order);
                        const float normalization=sh_light_cosine_norm(order);
                        for(unsigned l=0;l<order;++l)for(unsigned i=l*l;i<(l+1)*(l+1);++i){
                            const float coefficient=load(red+4*i,1)[0];
                            const float value=directional?coefficient/normalization:coefficient*weights[l];
                            for(unsigned c=0;c<3;++c)if(outputs[c])put(c,i,value*color[c]);
                        }
                    }
                }
            }
