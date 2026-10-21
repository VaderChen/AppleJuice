#pragma once
// 依 D3D9 契約計算光照，光柵化與紋理混合仍由 Metal 執行。
std::pair<Vector,Vector> lit_colors(const Object& device,Vector position,Vector normal,Vector diffuse,Vector specular,bool has_diffuse,bool has_specular,bool world_space=false){
    const auto dot=[](const Vector& a,const Vector& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto unit=[&](Vector v){auto length=std::sqrt(dot(v,v));if(length>0)for(unsigned i=0;i<3;++i)v[i]/=length;return v;};
    const auto material=[&](unsigned offset,unsigned state){auto source=device.render.at(141)?device.render.at(state):0U;if(source==1 && has_diffuse)return diffuse;if(source==2 && has_specular)return specular;Vector value{};std::copy_n(device.material.begin()+offset,4,value.begin());return value;};
    const auto md=material(0,145),ma=material(4,147),ms=material(8,146),me=material(12,148),ambient=color(device.render.at(139));
    Matrix model{};const auto world=world_space?identity():device.transforms.at(256);const auto& view=device.transforms.at(2);
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)model[r*4+c]+=world[r*4+k]*view[k*4+c];
    const auto eye=transform(position,model);Vector n{};
    const double determinant=double(model[0])*(double(model[5])*model[10]-double(model[6])*model[9])-double(model[1])*(double(model[4])*model[10]-double(model[6])*model[8])+double(model[2])*(double(model[4])*model[9]-double(model[5])*model[8]);
    if(determinant!=0)for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r){auto r1=(r+1)%3,r2=(r+2)%3,c1=(c+1)%3,c2=(c+2)%3;n[c]+=static_cast<float>(normal[r]*(double(model[r1*4+c1])*model[r2*4+c2]-double(model[r1*4+c2])*model[r2*4+c1])/determinant);}
    if(device.render.at(143))n=unit(n);
    Vector result{},highlight{};for(unsigned c=0;c<3;++c)result[c]=me[c]+ma[c]*ambient[c];result[3]=md[3];
    const auto viewer=device.render.at(142)?unit(Vector{-eye[0],-eye[1],-eye[2],0}):Vector{0,0,-1,0};
    for(const auto& [index,light]:device.lights){if(!device.light_enabled.at(index))continue;Vector direction{};float attenuation=1;
        if(light.type==3)direction=unit(transform(Vector{-light.direction[0],-light.direction[1],-light.direction[2],0},view));
        else {auto location=transform(Vector{light.position[0],light.position[1],light.position[2],1},view);for(unsigned c=0;c<3;++c)direction[c]=location[c]-eye[c];auto distance=std::sqrt(dot(direction,direction));if(distance>light.range)continue;direction=unit(direction);auto denominator=light.attenuation0+light.attenuation1*distance+light.attenuation2*distance*distance;attenuation=denominator>0?1/denominator:1;
            if(light.type==2){auto axis=unit(transform(Vector{light.direction[0],light.direction[1],light.direction[2],0},view));auto cosine=-dot(axis,direction),inner=std::cos(light.theta*.5F),outer=std::cos(light.phi*.5F);if(cosine<=outer)attenuation=0;else if(cosine<inner)attenuation*=std::pow((cosine-outer)/(inner-outer),light.falloff);}
        }
        auto lambert=std::max(0.F,dot(n,direction));Vector halfway{};for(unsigned c=0;c<3;++c)halfway[c]=direction[c]+viewer[c];auto shine=device.render.at(29) && lambert>0?std::pow(std::max(0.F,dot(n,unit(halfway))),device.material[16]):0.F;
        for(unsigned c=0;c<3;++c){result[c]+=attenuation*(ma[c]*light.ambient[c]+md[c]*light.diffuse[c]*lambert);highlight[c]+=attenuation*ms[c]*light.specular[c]*shine;}
    }
    for(unsigned c=0;c<4;++c){result[c]=std::clamp(result[c],0.F,1.F);highlight[c]=std::clamp(highlight[c],0.F,1.F);}return {result,highlight};
}
