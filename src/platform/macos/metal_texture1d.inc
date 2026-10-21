// Metal 原生 1D texture 沒有 D3D 的 mip 取樣契約；使用高度 1 的 2D 儲存。
// 只改變已宣告的一維 texture 呼叫，保留 LOD、array slice 與 X 位址模式。
void metal_texture1d_shader(MetalShader& shader){
    auto& source=shader.source;std::map<std::string,bool> textures;
    const std::regex declaration(R"(texture1d(_array)?\s*<[^>]+>\s+([A-Za-z_]\w*))");
    for(std::sregex_iterator it(source.begin(),source.end(),declaration),end;it!=end;++it)textures[(*it)[2]]=(*it)[1].matched;
    if(textures.empty())return;
    const auto arguments=[](std::string_view text,std::size_t begin){
        std::vector<std::string> result;unsigned depth=1;std::size_t start=begin,at=begin;
        for(;at<text.size();++at){if(text[at]=='(')++depth;else if(text[at]==')')--depth;
            if(!depth || (text[at]==',' && depth==1)){result.emplace_back(text.substr(start,at-start));start=at+1;if(!depth)break;}}
        if(depth)throw std::invalid_argument("一維 texture 呼叫括號不完整");return std::pair{result,at};
    };
    for(const auto& [name,array]:textures)for(const auto* method:{"sample","sample_compare","read","write","calculate_clamped_lod","calculate_unclamped_lod"}){
        const auto prefix=name+"."+method+"(";std::size_t at=0;
        while((at=source.find(prefix,at))!=source.npos){
            const auto begin=at+prefix.size();auto [args,end]=arguments(source,begin);const std::string_view operation=method;
            const bool reading=operation=="read",writing=operation=="write";const unsigned coord=reading?0:1;
            if(args.size()<=coord)throw std::invalid_argument("一維 texture 呼叫缺少座標");
            args[coord]=std::string(reading || writing?"uint2(":"float2(")+args[coord]+(reading || writing?",0)":",0.5)");
            if(operation.starts_with("sample")){
                const unsigned extras=2U+(array?1U:0U)+(operation=="sample_compare"?1U:0U);
                for(unsigned i=extras;i<args.size();++i){
                    const auto first=args[i].find_first_not_of(" \t\r\n");const auto argument=first==args[i].npos?std::string{}:args[i].substr(first);
                    if(argument.starts_with("gradient1d(")){auto [gradient,last]=arguments(argument,11);(void)last;if(gradient.size()!=2)throw std::invalid_argument("一維 texture gradient 不合法");args[i]="gradient2d(float2("+gradient[0]+",0),float2("+gradient[1]+",0))";}
                    else if(!argument.starts_with("level(") && !argument.starts_with("bias(") && !argument.starts_with("gradient"))args[i]="int2("+args[i]+",0)";
                }
            }
            std::string replacement;for(unsigned i=0;i<args.size();++i){if(i)replacement+=",";replacement+=args[i];}
            source.replace(begin,end-begin,replacement);at=begin+replacement.size()+1;
        }
    }
    source=std::regex_replace(source,std::regex(R"(\btexture1d\b)"),"texture2d");
    source=std::regex_replace(source,std::regex(R"(\btexture1d_array\b)"),"texture2d_array");
}
