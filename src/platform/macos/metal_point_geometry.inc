// 從 SPIRV-Cross 的頂點介面取回實際成員名稱，依 D3D 簽章連結兩個階段。
std::vector<MetalVarying> metal_point_members(const std::string& source,const std::vector<GraphicsShaderSemantic>& signature,bool input){
    static const std::regex member(R"(\b(float|uint|int)([234]?)\s+(\w+)\s*\[\[\s*(?:attribute\((\d+)\)|user\(locn(\d+)(?:_(\d+))?\)))");
    std::vector<MetalVarying> fields;
    for(std::sregex_iterator it(source.begin(),source.end(),member),end;it!=end;++it){
        const auto& m=*it;
        if(input!=m[4].matched)continue;
        const auto location=std::stoul(m[input?4:5].str());
        const unsigned first=m[6].matched?unsigned(std::stoul(m[6].str())):0U;
        const auto sem=std::find_if(signature.begin(),signature.end(),[&](const auto& e){return e.location==location && e.mask && unsigned(std::countr_zero(e.mask))==first;});
        if(sem==signature.end())throw std::invalid_argument("幾何階段的 Metal 成員缺少對應簽章");
        const unsigned width=m[2].length()?unsigned(m[2].str()[0]-'0'):1;
        if(sem->mask!=((1U<<width)-1)<<first)
            throw std::invalid_argument("幾何階段的共用暫存器分量需要重新排列");
        const unsigned type=m[1]=="uint"?INOUT_COMPONENT_UINT32:m[1]=="int"?INOUT_COMPONENT_SINT32:INOUT_COMPONENT_FLOAT32;
        fields.push_back({m[3].str(),sem->semantic,sem->index,width,type,0});
    }
    return fields;
}

