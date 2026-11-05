                    // 與可程式化管線共用請求、去重及容量限制；未啟用時不回讀資源。
                    if(object.frame_capture && object.frame_capture->select()){
                        auto& capture=*object.frame_capture;
                        try {
                            std::ofstream out(capture.directory/("draw-"+std::to_string(capture.draw-1)+".txt"));
                            out<<"frame="<<capture.frame<<" draw="<<capture.draw-1<<" pipeline=fixed topology="<<drawing_type
                               <<" fvf="<<object.fvf<<" rhw="<<rhw<<" gpu_vertices="<<gpu_vertices<<" count="<<count<<" indexed="<<indexed<<'\n';
                            out<<"input-vertices stride="<<stride<<" minimum="<<min<<" data="<<capture.save(bytes)<<'\n';
                            out<<"input-indices data="<<capture.save(std::span(reinterpret_cast<const std::uint8_t*>(indices.data()),indices.size()*sizeof(indices[0])))<<'\n';
                            out<<"packed-vertices stride="<<sizeof(FixedGraphicsVertex)<<" data="<<capture.save(std::span(reinterpret_cast<const std::uint8_t*>(packed.data()),packed.size()*sizeof(packed[0])))<<'\n';
                            out<<"packed-indices data="<<capture.save(std::span(reinterpret_cast<const std::uint8_t*>(packed_indices.data()),packed_indices.size()*sizeof(packed_indices[0])))<<'\n';
                            if(textured){const auto texture=get(object.texture,Kind::Texture);
                                out<<"texture format="<<texture->format<<" levels="<<texture->levels<<" lod="<<texture->lod<<'\n';
                                for(unsigned level=0;level<texture->levels;++level){const auto surface=get(texture->subresources.at(level),Kind::Surface);
                                    if(surface->native_view.resource)capture.view(out,"texture-native-"+std::to_string(level),*object.native,surface->native_view);
                                    else out<<"texture-cpu-"<<level<<" width="<<surface->width<<" height="<<surface->height<<" data="<<capture.save(surface->pixels)<<'\n';
                                }
                            }
                            for(const auto& [key,state_value]:object.render)out<<"render "<<key<<' '<<state_value<<'\n';
                            for(const auto& [key,state_value]:object.sampler)out<<"sampler "<<key<<' '<<state_value<<'\n';
                            for(const auto& [key,state_value]:object.stage)out<<"stage "<<key<<' '<<state_value<<'\n';
                            for(unsigned key:{256U,2U,3U}){out<<"transform "<<key;for(auto component:object.transforms.at(key))out<<' '<<component;out<<'\n';}
                            out<<"viewport";for(auto component:object.viewport)out<<' '<<component;out<<'\n';
                            out<<"scissor";for(auto component:object.scissor)out<<' '<<component;out<<'\n';
                            capture.view(out,"target-after-0",*object.native,get(object.render_targets[0],Kind::Surface)->native_view);
                            if(object.depth_target)capture.view(out,"depth-after",*object.native,get(object.depth_target,Kind::Surface)->native_view);
                        }catch(const std::exception& error){std::fprintf(stderr,"[d3d9 capture] draw=%llu %s\n",static_cast<unsigned long long>(capture.draw-1),error.what());}
                    }
