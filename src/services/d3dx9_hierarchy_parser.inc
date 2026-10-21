    void animation_set(Hierarchy& hierarchy){
        AnimationSet set;if(!is("{"))set.name=tokens.at(cursor).text;brace();
        while(cursor<tokens.size()){punctuation();if(is("}")){++cursor;hierarchy.sets.push_back(std::move(set));return;}
            const auto type=tokens[cursor++].text;if(type!="Animation"){skip();continue;}Animation animation;brace();
            while(cursor<tokens.size()){punctuation();if(is("}")){++cursor;break;}
                if(is("{")){++cursor;check(cursor<tokens.size());animation.name=tokens[cursor++].text;punctuation();check(is("}"));++cursor;}
                else {const auto child=tokens[cursor++].text;if(child!="AnimationKey"){skip();continue;}brace();const auto kind=integer(),count=integer();check(kind<=4 && count<=1048576);auto& keys=animation.keys[kind];
                    for(unsigned i=0;i<count;++i){AnimationKey key;key.time=number();check(std::isfinite(key.time) && key.time>=0);const auto size=integer();check(size==(kind==0?4U:kind==1 || kind==2?3U:16U));key.values.resize(size);for(auto& value:key.values){value=static_cast<float>(number());check(std::isfinite(value));}keys.push_back(std::move(key));}
                    std::stable_sort(keys.begin(),keys.end(),[](const auto& a,const auto& b){return a.time<b.time;});punctuation();check(is("}"));++cursor;
                }
            }check(!animation.name.empty());set.animations.push_back(std::move(animation));
        }check(false);
    }
    void hierarchy_objects(Hierarchy& hierarchy,Frame* frame){
        const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        while(cursor<tokens.size()){
            punctuation();if(cursor==tokens.size())break;if(is("}")){check(frame!=nullptr);++cursor;return;}const auto type=tokens[cursor++].text;
            if(type=="template"){check(cursor<tokens.size());++cursor;skip();}
            else if(type=="Frame"){Frame child;if(!is("{"))child.name=tokens.at(cursor).text;brace();hierarchy_objects(hierarchy,&child);(frame?frame->children:hierarchy.frames).push_back(std::move(child));}
            else if(type=="FrameTransformMatrix"){check(frame!=nullptr);brace();for(auto& value:frame->transform){value=static_cast<float>(number());check(std::isfinite(value));}punctuation();check(is("}"));++cursor;}
            else if(type=="Mesh"){Mesh value;mesh(value,identity);if(frame)frame->meshes.push_back(std::move(value));else{Frame root;root.meshes.push_back(std::move(value));hierarchy.frames.push_back(std::move(root));}}
            else if(type=="Material"){const auto id=is("{")?std::string{}:tokens[cursor].text;auto value=material();if(!id.empty())named_materials[id]=std::move(value);}
            else if(type=="AnimationSet")animation_set(hierarchy);
            else if(type=="AnimTicksPerSecond"){brace();hierarchy.ticks_per_second=number();check(std::isfinite(hierarchy.ticks_per_second) && hierarchy.ticks_per_second>0);punctuation();check(is("}"));++cursor;}
            else skip();
        }check(frame==nullptr);
    }
