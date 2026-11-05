                            if(object.frame_capture && object.frame_capture->select()){
                                auto& capture=*object.frame_capture;
                                try {
                                    std::ofstream out(capture.directory/("draw-"+std::to_string(capture.draw-1)+".txt"));
                                    out<<"frame="<<capture.frame<<" draw="<<capture.draw-1<<" topology="<<draw.topology<<" count="<<draw.count<<" indexed="<<draw.indexed
                                       <<" start="<<draw.start<<" base_vertex="<<draw.base_vertex<<" instances="<<draw.instances<<" index_format="<<draw.index_format<<'\n';
                                    for(const auto& e:draw.layout)out<<"element semantic="<<e.semantic<<" index="<<e.index<<" format="<<e.format<<" slot="<<e.slot<<" offset="<<e.offset<<" classification="<<e.classification<<" step="<<e.step<<'\n';
                                    for(unsigned slot=0;slot<draw.vertices.size();++slot)if(draw.vertices[slot].resource){out<<"vertex slot="<<slot<<" stride="<<draw.vertices[slot].stride<<" offset="<<draw.vertices[slot].offset<<'\n';capture.view(out,"vertex-"+std::to_string(slot),*object.native,GraphicsView{draw.vertices[slot].resource});}
                                    capture.view(out,"indices",*object.native,GraphicsView{draw.indices});
                                    for(unsigned stage=0;stage<2;++stage){
                                        if(object.shaders[stage]){const auto shader=get(object.shaders[stage],stage?Kind::PixelShader:Kind::VertexShader);const auto code=capture.save(shader->pixels);out<<"shader stage="<<stage<<" object="<<shader->root<<" data="<<code<<'\n';
                                            if(code!="omitted-budget"){const auto path=capture.directory/(code+".metal");if(!std::filesystem::exists(path)){std::ofstream source(path);source<<graphics_legacy_shader(stage,shader->pixels).source;}}}
                                        for(unsigned slot=0;slot<3;++slot)capture.view(out,"constant-"+std::to_string(stage)+"-"+std::to_string(slot),*object.native,GraphicsView{draw.stages[stage].constants[slot]});
                                        for(unsigned slot=0;slot<draw.stages[stage].resources.size();++slot)capture.view(out,"texture-"+std::to_string(stage)+"-"+std::to_string(slot),*object.native,draw.stages[stage].resources[slot]);
                                    }
                                    for(const auto& [key,state_value]:object.render)out<<"render "<<key<<' '<<state_value<<'\n';
                                    for(const auto& [key,state_value]:object.sampler)out<<"sampler "<<key<<' '<<state_value<<'\n';
                                    out<<"viewport";for(const auto component:object.viewport)out<<' '<<component;out<<'\n';
                                    out<<"scissor";for(const auto component:object.scissor)out<<' '<<component;out<<'\n';
                                    for(unsigned slot=0;slot<4;++slot)capture.view(out,"target-after-"+std::to_string(slot),*object.native,draw.targets[slot]);
                                    capture.view(out,"depth-after",*object.native,draw.depth);
                                }catch(const std::exception& error){std::fprintf(stderr,"[d3d9 capture] draw=%llu %s\n",static_cast<unsigned long long>(capture.draw-1),error.what());}
                            }
