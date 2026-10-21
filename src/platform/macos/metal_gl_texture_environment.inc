// 紋理環境的 scalar／vector／query 共用一份狀態與驗證。
void MetalGl::texture_environment(GlOperation op,std::span<const std::uint64_t> args,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    const auto target=static_cast<unsigned>(args[0]),pname=static_cast<unsigned>(args[1]);
    const bool get=op==GlOperation::GetTexEnviv || op==GlOperation::GetTexEnvfv;
    const bool floating=op==GlOperation::TexEnvf || op==GlOperation::TexEnvfv || op==GlOperation::GetTexEnvfv;
    const bool vector=op==GlOperation::TexEnviv || op==GlOperation::TexEnvfv;
    if(target!=0x2300 || (pname!=0x2200 && pname!=0x2201) || (pname==0x2201 && !get && !vector)){error(0x500);return;}
    auto& environment=state_.environments[gl_.active_texture];const unsigned count=pname==0x2201?4U:1U;
    if(get){
        if(output.size()<count*4){error(0x501);return;}
        for(unsigned i=0;i<count;++i){const float value=pname==0x2201?environment.color[i]:static_cast<float>(environment.mode?environment.mode:0x2100);
            const auto scaled=pname==0x2201?static_cast<double>(value)*INT32_MAX:value;
            const auto word=floating?std::bit_cast<unsigned>(value):static_cast<unsigned>(static_cast<std::int32_t>(std::isfinite(scaled)?std::round(scaled):0.));
            std::memcpy(output.data()+i*4,&word,4);}
        return;
    }
    if(vector && data.size()<count*4){error(0x501);return;}
    std::array<double,4> values{};
    for(unsigned i=0;i<count;++i){const auto word=vector?static_cast<unsigned>(number(data,i*4,0x1405)):static_cast<unsigned>(args[2]);
        values[i]=floating?std::bit_cast<float>(word):std::bit_cast<std::int32_t>(word);}
    if(pname==0x2200){const auto value=values[0];if(value!=0x2100 && value!=0x2101 && value!=0xbe2 && value!=0x1e01 && value!=0x104){error(0x500);return;}environment.mode=static_cast<unsigned>(value);}
    else for(unsigned i=0;i<4;++i)environment.color[i]=static_cast<float>(std::clamp(floating?values[i]:values[i]/INT32_MAX,0.,1.));
}
