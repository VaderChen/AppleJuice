        const auto make_animation_controller=[&](unsigned outputs,unsigned sets,unsigned tracks,unsigned events){require(outputs<=65536 && sets<=65536 && tracks<=256 && events<=1048576);auto object=create(Kind::AnimController);object->animation_controller=std::make_shared<d3dx_animation::Controller>();auto& controller=*object->animation_controller;controller.maximum_outputs=outputs;controller.maximum_sets=sets;controller.maximum_events=events;controller.tracks.resize(tracks);return object;};
        const auto register_animation_output=[&](d3dx_animation::Controller& controller,std::string name,std::array<std::uint64_t,4> pointers){
            auto found=std::find_if(controller.outputs.begin(),controller.outputs.end(),[&](const auto& output){return output.name==name;});require(found!=controller.outputs.end() || controller.outputs.size()<controller.maximum_outputs);
            d3dx_animation::Output output;output.name=std::move(name);output.pointers=pointers;if(pointers[0]){auto bytes=read(pointers[0],64);Matrix matrix{};std::memcpy(matrix.data(),bytes.data(),64);output.initial=d3dx_animation::decompose(matrix);}
            for(unsigned n=1;n<4;++n)if(pointers[n]){const auto bytes=read(pointers[n],n==2?16:12);auto* destination=n==1?output.initial.scale.data():n==2?output.initial.rotation.data():output.initial.translation.data();std::memcpy(destination,bytes.data(),bytes.size());}
            if(found==controller.outputs.end())controller.outputs.push_back(std::move(output));else *found=std::move(output);
        };
        const bool animation_controller_handled=symbol=="D3DXCreateAnimationController" || symbol.starts_with("AnimationController.");
        if(animation_controller_handled){
            mesh_handled=true;
            if(symbol=="D3DXCreateAnimationController"){output(4);store_pointer(arg(4),make_animation_controller(u(0),u(1),u(2),u(3))->root);}
            else{
                auto object=get(arg(0),Kind::AnimController);auto& controller=*object->animation_controller;const auto method=symbol.substr(20);
                const auto track_at=[&](unsigned index)->d3dx_animation::Track&{require(index<controller.tracks.size());return controller.tracks[index];};
                const auto event_at=[&](unsigned handle)->d3dx_animation::Event&{auto found=std::find_if(controller.events.begin(),controller.events.end(),[&](const auto& event){return event.id==handle && !event.finished;});require(found!=controller.events.end());return *found;};
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(supports(Kind::AnimController,requested)){hold(object);store_pointer(arg(2),object->root);}else result=no_interface;}
                else if(method=="AddRef")result=hold(object);else if(method=="Release")result=release(object->root);
                else if(method=="GetMaxNumAnimationOutputs")result=controller.maximum_outputs;else if(method=="GetMaxNumAnimationSets")result=controller.maximum_sets;else if(method=="GetMaxNumTracks")result=controller.tracks.size();else if(method=="GetMaxNumEvents")result=controller.maximum_events;
                else if(method=="RegisterAnimationOutput"){require(arg(1) && (arg(2) || arg(3) || arg(4) || arg(5)));register_animation_output(controller,read_ansi(memory,arg(1)),{arg(2),arg(3),arg(4),arg(5)});}
                else if(method=="RegisterAnimationSet"){auto set=get_animation_set(arg(1));require(controller.sets.size()<controller.maximum_sets && std::find(controller.sets.begin(),controller.sets.end(),set->root)==controller.sets.end());hold(set);controller.sets.push_back(set->root);}
                else if(method=="UnregisterAnimationSet"){auto found=std::find(controller.sets.begin(),controller.sets.end(),arg(1));require(found!=controller.sets.end());release(*found);controller.sets.erase(found);}
                else if(method=="GetNumAnimationSets")result=controller.sets.size();
                else if(method=="GetAnimationSet" || method=="GetAnimationSetByName"){output(2);std::uint64_t pointer=0;if(method=="GetAnimationSet"){require(u(1)<controller.sets.size());pointer=controller.sets[u(1)];}else{require(arg(1));const auto name=read_ansi(memory,arg(1));for(auto set:controller.sets)if(get_animation_set(set)->animation_set->name==name){pointer=set;break;}require(pointer,0x88760866U);}hold(get_animation_set(pointer));store_pointer(arg(2),pointer);}
                else if(method=="GetTime")return_scalar(controller.time,false);
                else if(method=="ResetTime")controller.time=0;
                else if(method=="GetPriorityBlend")return_scalar(controller.blend,true);
                else if(method=="SetPriorityBlend"){const auto value=scalar_arg(1);require(std::isfinite(value) && value>=0 && value<=1);controller.blend=value;}
                else if(method=="SetTrackAnimationSet"){auto& track=track_at(u(1));if(arg(2))hold(get_animation_set(arg(2)));if(track.set)release(track.set);track.set=arg(2);}
                else if(method=="GetTrackAnimationSet"){output(2);const auto& track=track_at(u(1));if(track.set){hold(get_animation_set(track.set));store_pointer(arg(2),track.set);}}
                else if(method=="SetTrackDesc" || method=="GetTrackDesc"){
                    auto& track=track_at(u(1));require(arg(2));if(method=="GetTrackDesc"){std::vector<std::uint8_t> data(32);put(data,0,track.priority);put(data,4,bits(track.weight));put(data,8,bits(track.speed));std::memcpy(data.data()+16,&track.position,8);put(data,24,track.enabled?1U:0U);memory.write(GuestAddress{arg(2)},data);}
                    else{const auto data=read(arg(2),32);const auto priority=word(data,0);const auto weight=real(word(data,4)),speed=real(word(data,8));double position=0;std::memcpy(&position,data.data()+16,8);require(priority<=1 && std::isfinite(weight) && weight>=0 && std::isfinite(speed) && std::isfinite(position));track.priority=priority;track.weight=weight;track.speed=speed;track.position=position;track.enabled=word(data,24)!=0;}
                }else if(method=="SetTrackPriority"){require(u(2)<=1);track_at(u(1)).priority=u(2);}
                else if(method=="SetTrackEnable")track_at(u(1)).enabled=u(2)!=0;
                else if(method=="SetTrackPosition"){const auto value=double_arg(2);require(std::isfinite(value));track_at(u(1)).position=value;}
                else if(method=="SetTrackSpeed" || method=="SetTrackWeight"){const auto value=scalar_arg(2);require(std::isfinite(value) && (method=="SetTrackSpeed" || value>=0));auto& track=track_at(u(1));(method=="SetTrackSpeed"?track.speed:track.weight)=value;}
                else if(method.starts_with("Key")){
                    controller.events.erase(std::remove_if(controller.events.begin(),controller.events.end(),[](const auto& event){return event.finished;}),controller.events.end());const bool priority=method=="KeyPriorityBlend";
                    if(controller.events.size()>=controller.maximum_events || (!priority && u(1)>=controller.tracks.size()))result=0;
                    else {d3dx_animation::Event event;event.track=priority?0U:u(1);event.type=priority?4U:method=="KeyTrackSpeed"?0U:method=="KeyTrackWeight"?1U:method=="KeyTrackPosition"?2U:3U;event.target=event.type==2?double_arg(2):event.type==3?(u(2)?1.:0.):scalar_arg(priority?1:2);event.start=double_arg(priority?2:3);if(event.type==0 || event.type==1 || priority){event.duration=double_arg(priority?3:4);event.transition=u(priority?4:5);}
                        const bool valid=std::isfinite(event.target) && std::isfinite(event.start) && std::isfinite(event.duration) && event.duration>=0 && event.transition<=1 && (event.type!=1 || event.target>=0) && (event.type!=4 || (event.target>=0 && event.target<=1));
                        if(!valid)result=0;else{event.id=++controller.next_event;if(!event.id)event.id=++controller.next_event;controller.events.push_back(event);result=event.id;}
                    }
                }else if(method=="UnkeyEvent"){auto& event=event_at(u(1));event.finished=true;}
                else if(method=="UnkeyAllTrackEvents"){(void)track_at(u(1));for(auto& event:controller.events)if(event.type!=4 && event.track==u(1))event.finished=true;}
                else if(method=="UnkeyAllPriorityBlends"){for(auto& event:controller.events)if(event.type==4)event.finished=true;}
                else if(method=="ValidateEvent"){(void)event_at(u(1));}
                else if(method=="GetEventDesc"){const auto& event=event_at(u(1));require(arg(2));std::vector<std::uint8_t> data(40);put(data,0,event.type);put(data,4,event.track);std::memcpy(data.data()+8,&event.start,8);std::memcpy(data.data()+16,&event.duration,8);put(data,24,event.transition);if(event.type==2)std::memcpy(data.data()+32,&event.target,8);else put(data,32,event.type==3?static_cast<unsigned>(event.target):bits(static_cast<float>(event.target)));memory.write(GuestAddress{arg(2)},data);}
                else if(method.starts_with("GetCurrent") || method.starts_with("GetUpcoming")){
                    const bool priority=method.ends_with("PriorityBlend"),current=method.starts_with("GetCurrent");if(!priority)(void)track_at(u(1));double after=controller.time;unsigned previous=0;if(!current && u(priority?1:2)){const auto& event=event_at(u(priority?1:2));after=event.start;previous=event.id;}
                    const d3dx_animation::Event* selected=nullptr;
                    for(const auto& event:controller.events){if(event.finished || ((event.type==4)!=priority) || (!priority && event.track!=u(1)))continue;if(current){if(!event.started || (!priority && event.type!=u(2)))continue;}else if(event.start<after || (event.start==after && event.id<=previous))continue;
                        if(!selected || event.start<selected->start || (event.start==selected->start && event.id<selected->id))selected=&event;
                    }result=selected?selected->id:0;
                }else if(method=="CloneAnimationController"){
                    output(5);require(u(1)>=controller.outputs.size() && u(2)>=controller.sets.size() && u(3)>=controller.tracks.size() && u(4)>=controller.events.size());auto clone=make_animation_controller(u(1),u(2),u(3),u(4));auto& value=*clone->animation_controller;value.time=controller.time;value.blend=controller.blend;value.sets=controller.sets;for(auto set:value.sets)hold(get_animation_set(set));value.outputs=controller.outputs;value.events=controller.events;value.next_event=controller.next_event;std::copy(controller.tracks.begin(),controller.tracks.end(),value.tracks.begin());for(const auto& track:value.tracks)if(track.set)hold(get_animation_set(track.set));store_pointer(arg(5),clone->root);
                }
#include "d3dx9_animation_advance.inc"
                else require(false);
            }
        }
