// 只保存不依賴 Metal 裝置的翻譯結果。LC_UUID 由共用快取驗證；Metal 函式仍由目前裝置建立。
constexpr std::uint32_t metal_hlslcc_record_version=2;
static_assert(std::is_trivially_copyable_v<GraphicsBinding> && sizeof(GraphicsBinding)==8*sizeof(std::uint32_t));

std::vector<std::uint8_t> encode_metal_hlslcc_shader(const MetalShader& shader){
    std::vector<std::uint8_t> bytes;
    bytes.reserve(shader.source.size()+shader.bindings.size()*sizeof(GraphicsBinding)+256);
    const auto put=[&](const auto& value){
        const auto* data=reinterpret_cast<const std::uint8_t*>(&value);bytes.insert(bytes.end(),data,data+sizeof(value));
    };
    const auto text=[&](std::string_view value){
        put(static_cast<std::uint32_t>(value.size()));bytes.insert(bytes.end(),value.begin(),value.end());
    };
    const auto varyings=[&](const auto& list){
        put(static_cast<std::uint32_t>(list.size()));
        for(const auto& v:list){
            text(v.name);
            text(v.semantic);
            put(v.index);
            put(v.components);
            put(v.type);
            put(v.system);
            put(v.clip_offset);
            put(v.clip_count);
        }
    };
    put(metal_hlslcc_record_version);text(shader.source);put(shader.buffer_views_slot);
    for(const auto value:shader.threads)put(value);
    put(static_cast<std::uint32_t>(shader.bindings.size()));
    for(const auto& binding:shader.bindings)put(binding);
    put(static_cast<std::uint32_t>(shader.inputs.size()));
    for(const auto& input:shader.inputs){text(input.semantic);put(input.index);put(input.attribute);put(input.component);}
    varyings(shader.input_varyings);varyings(shader.output_varyings);
    return bytes;
}

bool decode_metal_hlslcc_shader(std::span<const std::uint8_t> bytes,MetalShader& shader){
    const auto get=[&](auto& value){
        if(bytes.size()<sizeof(value))return false;
        std::memcpy(&value,bytes.data(),sizeof(value));bytes=bytes.subspan(sizeof(value));return true;
    };
    const auto text=[&](std::string& value){
        std::uint32_t size=0;if(!get(size) || size>bytes.size())return false;
        value.assign(reinterpret_cast<const char*>(bytes.data()),size);bytes=bytes.subspan(size);return true;
    };
    const auto list=[&](auto& values,std::size_t minimum,const auto& read){
        std::uint32_t count=0;if(!get(count) || count>bytes.size()/minimum)return false;
        values.resize(count);return std::ranges::all_of(values,read);
    };
    const auto varying=[&](MetalVarying& v){
        return text(v.name) && text(v.semantic) && get(v.index) && get(v.components) &&
            get(v.type) && get(v.system) && get(v.clip_offset) && get(v.clip_count);
    };
    std::uint32_t version=0;
    if(!get(version) || version!=metal_hlslcc_record_version || !text(shader.source) || shader.source.empty() || !get(shader.buffer_views_slot))return false;
    if(!std::ranges::all_of(shader.threads,get))return false;
    if(!list(shader.bindings,sizeof(GraphicsBinding),get))return false;
    if(!list(shader.inputs,4*sizeof(std::uint32_t),[&](GraphicsInput& input){return text(input.semantic) && get(input.index) && get(input.attribute) && get(input.component);}))return false;
    return list(shader.input_varyings,8*sizeof(std::uint32_t),varying) &&
        list(shader.output_varyings,8*sizeof(std::uint32_t),varying) && bytes.empty();
}
