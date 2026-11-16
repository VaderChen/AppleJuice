// opengl32 glTexGen／glGetTexGen（由 windows_opengl.cpp 的 invoke_opengl 引入）：i／f／d 純量與向量版本一律換成雙精度交給後端 TexGendv／GetTexGendv。
// GL_TEXTURE_GEN_MODE 一個值、GL_OBJECT_PLANE／GL_EYE_PLANE 四個值；純量版本只接受 GL_TEXTURE_GEN_MODE。查詢的整數版本依規格四捨五入。
if(symbol.starts_with("glTexGen") || symbol.starts_with("glGetTexGen")){
    const auto coord=d(0),pname=d(1);
    if(coord<0x2000 || coord>0x2003 || pname<0x2500 || pname>0x2502){error(c,invalid_enum);return finish(0);}
    const bool vector=symbol.ends_with("v");const char type=vector?symbol[symbol.size()-2]:symbol.back();
    const unsigned count=pname==0x2500?1U:4U,width=type=='d'?8U:4U;
    const auto run=[&](GlOperation operation,std::span<const std::uint64_t> values,std::span<const std::uint8_t> data,std::span<std::uint8_t> out)->std::uint64_t{
        try{return c.native->execute(operation,values,data,out);}catch(const std::bad_alloc&){error(c,out_of_memory);return 0;}};
    const std::array<std::uint64_t,2> header{coord,pname};
    if(symbol.starts_with("glGetTexGen")){
        auto& result=c.output_scratch;result.assign(32,0);
        const auto written=static_cast<unsigned>(std::min<std::uint64_t>(run(GlOperation::GetTexGendv,header,{},result),count));
        for(unsigned i=0;i<written;++i){double value;std::memcpy(&value,result.data()+i*8,8);
            const std::uint64_t raw=type=='d'?std::bit_cast<std::uint64_t>(value):type=='f'?std::bit_cast<std::uint32_t>(static_cast<float>(value)):static_cast<std::uint32_t>(gl_integer(value,false));
            memory.store(GuestAddress{arg(2)+i*width},static_cast<std::uint8_t>(width*8),raw);}
        return finish(0);
    }
    std::array<double,4> values{};
    if(!vector){
        if(pname!=0x2500){error(c,invalid_enum);return finish(0);}
        const auto raw=type=='i'?arg(2):floating_argument(2,type=='d');
        values[0]=type=='i'?static_cast<double>(n(2)):type=='d'?std::bit_cast<double>(raw):static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
    }else for(unsigned i=0;i<count;++i){
        const auto raw=memory.load(GuestAddress{arg(2)+i*width},static_cast<std::uint8_t>(width*8));
        values[i]=type=='d'?std::bit_cast<double>(raw):type=='f'?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))):static_cast<double>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw)));
    }
    auto& input=c.input_scratch;input.resize(count*8);std::memcpy(input.data(),values.data(),count*8);
    run(GlOperation::TexGendv,header,input,{});return finish(0);
}
