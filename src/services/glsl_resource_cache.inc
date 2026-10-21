// 僅序列化可重建的 MSL／反射；不保存 MTLDevice、pipeline、uniform 當前值或宿主指標。
struct ShaderResourceWriter {
    std::vector<std::uint8_t> bytes;
    void number(std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
    void data(std::span<const std::uint8_t> value){if(value.size()>UINT32_MAX)throw std::length_error("著色器產物過大");number(static_cast<std::uint32_t>(value.size()));bytes.insert(bytes.end(),value.begin(),value.end());}
    void text(std::string_view value){data({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});}
};
struct ShaderResourceReader {
    std::span<const std::uint8_t> bytes;
    std::uint32_t number(){if(bytes.size()<4)throw std::invalid_argument("著色器快取截斷");std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[i]}<<(i*8);bytes=bytes.subspan(4);return value;}
    std::uint32_t count(){const auto n=number();if(n>65536 || n>bytes.size()/4)throw std::invalid_argument("著色器快取數量不合法");return n;}
    std::span<const std::uint8_t> data(){const auto n=number();if(n>bytes.size())throw std::invalid_argument("著色器快取長度不合法");const auto value=bytes.first(n);bytes=bytes.subspan(n);return value;}
    std::string text(){const auto value=data();return {reinterpret_cast<const char*>(value.data()),value.size()};}
};
std::vector<std::uint8_t> encode_shader_resource(const GlslResult& result){
    ShaderResourceWriter out;out.text("glsl-msl-1");out.text(result.log);out.number(static_cast<std::uint32_t>(result.stages.size()));
    for(const auto& stage:result.stages){out.number(stage.stage);out.number(stage.buffer_index);
        for(const auto* text:{&stage.msl,&stage.flipped_msl,&stage.entry,&stage.clip_mask,&stage.point_size_enabled})out.text(*text);
        out.data(stage.initial_uniforms);out.number(static_cast<std::uint32_t>(stage.attributes.size()));
        for(const auto& a:stage.attributes){out.text(a.name);out.number(a.location);out.number(a.components);out.number(a.columns);}
        out.number(static_cast<std::uint32_t>(stage.uniforms.size()));for(const auto& u:stage.uniforms){out.text(u.name);for(const auto n:{u.type,u.components,u.columns,u.count,u.offset,u.array_stride,u.matrix_stride})out.number(n);}
        out.number(static_cast<std::uint32_t>(stage.samplers.size()));for(const auto& s:stage.samplers){out.text(s.name);out.number(s.texture);out.number(s.sampler);out.number(s.count);}
    }return out.bytes;
}
GlslResult decode_shader_resource(std::span<const std::uint8_t> bytes){
    ShaderResourceReader in{bytes};if(in.text()!="glsl-msl-1")throw std::invalid_argument("著色器快取版本不合法");
    GlslResult result;result.log=in.text();const auto stages=in.number();if(stages>2)throw std::invalid_argument("著色器快取 stage 過多");
    for(unsigned i=0;i<stages;++i){GlslStage stage;stage.stage=in.number();if(stage.stage!=0x8b31 && stage.stage!=0x8b30)throw std::invalid_argument("著色器快取 stage 不合法");stage.buffer_index=in.number();
        stage.msl=in.text();stage.flipped_msl=in.text();stage.entry=in.text();stage.clip_mask=in.text();stage.point_size_enabled=in.text();
        const auto initial=in.data();stage.initial_uniforms.assign(initial.begin(),initial.end());
        const auto attributes=in.count();for(unsigned n=0;n<attributes;++n){GlslAttribute a;a.name=in.text();a.location=in.number();a.components=in.number();a.columns=in.number();stage.attributes.push_back(std::move(a));}
        const auto uniforms=in.count();for(unsigned n=0;n<uniforms;++n){GlslUniform u;u.name=in.text();u.type=in.number();u.components=in.number();u.columns=in.number();u.count=in.number();u.offset=in.number();u.array_stride=in.number();u.matrix_stride=in.number();stage.uniforms.push_back(std::move(u));}
        const auto samplers=in.count();for(unsigned n=0;n<samplers;++n){GlslSampler s;s.name=in.text();s.texture=in.number();s.sampler=in.number();s.count=in.number();stage.samplers.push_back(std::move(s));}
        result.stages.push_back(std::move(stage));
    }
    if(!in.bytes.empty())throw std::invalid_argument("著色器快取有多餘資料");result.success=true;return result;
}
template<class Compile> GlslResult cached_shader_resource(std::span<const GlslSource> sources,bool link,Compile&& compile){
    const auto directory=generated_resource_cache_directory();if(directory.empty())return compile();
    ShaderResourceWriter identity;identity.text("x64arm-glsl-resource-1");identity.text(X64ARM_GLSL_RESOURCE_REVISION);identity.number(link?1:0);
    identity.number(static_cast<std::uint32_t>(sources.size()));for(const auto& source:sources){identity.number(source.stage);identity.text(source.source);identity.text(source.preamble);}
    GlslResult compiled;
    const auto resource=ResourceCache(directory).get_or_create(identity.bytes,[&]{compiled=compile();return compiled.success?encode_shader_resource(compiled):std::vector<std::uint8_t>{};});
    if(const auto* trace=std::getenv("X64ARM_RESOURCE_CACHE_TRACE");trace && std::string_view(trace)=="1")
        std::fprintf(stderr,"shader_cache link=%u hit=%u stored=%u pending=%u memory_hit=%u bytes=%zu stored_bytes=%llu build_ms=%.3f read_ms=%.3f decode_ms=%.3f encode_ms=%.3f write_ms=%.3f\n",link,resource.hit,resource.stored,resource.pending,resource.memory_hit,resource.bytes.size(),static_cast<unsigned long long>(resource.stored_bytes),resource.build_ms,resource.read_ms,resource.decode_ms,resource.encode_ms,resource.write_ms);
    if(resource.hit){try{return decode_shader_resource(resource.bytes);}catch(const std::invalid_argument&){return compile();}}
    return compiled;
}
