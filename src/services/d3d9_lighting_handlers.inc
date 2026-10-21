                }else if(symbol=="SetMaterial" || symbol=="GetMaterial"){
                    require(arg(1));
                    if(symbol=="GetMaterial")memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(object.material.data()),68));
                    else {std::array<float,17> material{};memory.read(GuestAddress{arg(1)},std::span(reinterpret_cast<std::uint8_t*>(material.data()),68));for(auto component:material)require(std::isfinite(component));require(material[16]>=0);object.material=material;}
                }else if(symbol=="SetLight" || symbol=="GetLight" || symbol=="LightEnable" || symbol=="GetLightEnable"){
                    const auto index=u(1);
                    if(symbol=="SetLight"){
                        require(arg(2));D3DLight light;memory.read(GuestAddress{arg(2)},std::span(reinterpret_cast<std::uint8_t*>(&light),sizeof(light)));require(light.type>=1 && light.type<=3);
                        std::array<float,25> fields{};std::memcpy(fields.data(),reinterpret_cast<const std::uint8_t*>(&light)+4,100);for(auto component:fields)require(std::isfinite(component));
                        if(light.type!=3)require(light.range>=0 && light.attenuation0>=0 && light.attenuation1>=0 && light.attenuation2>=0);
                        if(light.type==2)require(light.falloff>=0 && light.theta>=0 && light.phi>=light.theta && light.phi<=3.141592654F);
                        object.lights[index]=light;object.light_enabled.try_emplace(index,false);
                    }else if(symbol=="GetLight"){require(arg(2) && object.lights.contains(index));const auto& light=object.lights.at(index);memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(&light),sizeof(light)));}
                    else if(symbol=="GetLightEnable"){require(arg(2) && object.lights.contains(index));memory.store(GuestAddress{arg(2)},32,object.light_enabled.at(index)?128:0);}
                    else {const bool enabled=u(2)!=0;if(enabled && !object.light_enabled[index]){unsigned active=0;for(auto [number,on]:object.light_enabled){(void)number;active+=on?1U:0U;}require(active<8);}object.lights.try_emplace(index);object.light_enabled[index]=enabled;}
