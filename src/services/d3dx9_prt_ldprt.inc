    // 每個 SH 頻帶以 Y_lm(n) 的最小平方投影求係數；法線可再於球面最佳化。
    // 回傳值直接乘 Y_lm(n) 重建 transfer，與 SDK 範例的係數布局一致。
    std::uint32_t ldprt(std::span<const float> input,unsigned sh_order,unsigned in_channels,bool optimize,
        std::vector<float>& output,const std::function<std::uint32_t(float)>& progress)const{
        const unsigned coefficients=sh_order*sh_order;
        if(sh_order<2 || sh_order>6 || (in_channels!=1 && in_channels!=3) || input.size()!=positions.size()*coefficients*in_channels)
            throw std::invalid_argument("LDPRT 輸入不相容");
        const std::size_t values=positions.size()*sh_order*in_channels;output.assign(values+(optimize?positions.size()*3:0),0);float notified=-1;
        for(unsigned sample=0;sample<positions.size();++sample){
            const auto data=input.subspan(std::size_t(sample)*coefficients*in_channels,coefficients*in_channels);
            const auto fit=[&](Vec normal,float* destination)->double{
                std::array<float,36> basis{};sh_evaluate(basis.data(),sh_order,normal.data());double captured=0;
                for(unsigned band=0;band<sh_order;++band){const unsigned begin=band*band,end=(band+1)*(band+1);double norm=0;
                    for(unsigned i=begin;i<end;++i)norm+=double(basis[i])*basis[i];
                    for(unsigned channel=0;channel<in_channels;++channel){double product=0;for(unsigned i=begin;i<end;++i)product+=double(data[channel*coefficients+i])*basis[i];
                        captured+=product*product/norm;if(destination)destination[channel*sh_order+band]=static_cast<float>(product/norm);
                    }
                }return captured;
            };
            Vec normal=unit(normals[sample]);double best=fit(normal,nullptr);
            if(optimize){
                Vec first_band{};for(unsigned channel=0;channel<in_channels;++channel)first_band=add(first_band,Vec{-data[channel*coefficients+3],-data[channel*coefficients+1],data[channel*coefficients+2]});
                if(dot(first_band,first_band)>1e-20F){auto candidate=unit(first_band);const double score=fit(candidate,nullptr);if(score>best){normal=candidate;best=score;}}
                // 球面切平面搜尋；步幅逐次減半，以誤差不增加為接受条件。
                float step=.5F;for(unsigned iteration=0;iteration<96 && step>1e-4F;++iteration){
                    const auto tangent=unit(cross(std::abs(normal[2])<.9F?Vec{0,0,1}:Vec{0,1,0},normal)),bitangent=cross(normal,tangent);Vec selected=normal;double score=best;
                    for(unsigned direction_index=0;direction_index<8;++direction_index){const float angle=static_cast<float>(direction_index)*.7853981633974483F;
                        const auto candidate=unit(add(normal,scale(add(scale(tangent,std::cos(angle)),scale(bitangent,std::sin(angle))),step)));
                        const double value=fit(candidate,nullptr);if(value>score+1e-12*std::max(1.,best)){score=value;selected=candidate;}}
                    if(score>best){normal=selected;best=score;}else step*=.5F;
                }
                if(dot(normal,normals[sample])<0)normal=scale(normal,-1);
            }
            (void)fit(normal,output.data()+std::size_t(sample)*sh_order*in_channels);
            if(optimize)for(unsigned axis=0;axis<3;++axis)output[values+std::size_t(sample)*3+axis]=normal[axis];
            const float done=static_cast<float>(sample+1)/static_cast<float>(positions.size());if(done-notified>=frequency || sample+1==positions.size()){if(auto code=progress(done))return code;notified=done;}
        }return 0;
    }
