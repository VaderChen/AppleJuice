// MetalGl 各自持有不可變狀態，D3D9／10／11 與 OpenGL 共用相同內容鍵。
// command buffer 保留已編碼的 Metal 物件；快取輪替不縮短在途命令的生命週期。
BoundedCache<id<MTLDepthStencilState>> depth_states_{256U<<10,metal_cache_entries("X64ARM_METAL_STATE_CACHE_ENTRIES",512)};
BoundedCache<std::shared_ptr<MetalSampler>> sampler_states_{256U<<10,metal_cache_entries("X64ARM_METAL_STATE_CACHE_ENTRIES",512)};
std::uint64_t depth_state_hits_{},depth_state_builds_{},sampler_state_hits_{},sampler_state_builds_{};
static std::string state_cache_key(const std::array<std::uint64_t,16>& key){return {reinterpret_cast<const char*>(key.data()),sizeof(key)};}
std::string cache_statistics()const{
    std::string text;
    const auto add=[&](const char* name,const BoundedCacheStats& stats){
        const auto prefix=std::string(" metal_")+name+"_cache_";
        text+=prefix+"entries="+std::to_string(stats.entries)+prefix+"limit="+std::to_string(stats.entry_limit)+
              prefix+"key_bytes="+std::to_string(stats.bytes)+prefix+"key_limit="+std::to_string(stats.byte_limit)+
              prefix+"hits="+std::to_string(stats.hits)+prefix+"misses="+std::to_string(stats.misses)+
              prefix+"evictions="+std::to_string(stats.evictions)+prefix+"rejections="+std::to_string(stats.rejections);
    };
    add("gl_pipeline",gl_pipelines_.stats());add("depth_state",depth_states_.stats());add("sampler_state",sampler_states_.stats());return text;
}
static bool immutable_state_cache_enabled(){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_METAL_IMMUTABLE_STATE_CACHE");return !value || std::string_view(value)=="1";}();
    return enabled;
}
id<MTLDepthStencilState> cached_depth_state(MTLDepthStencilDescriptor* descriptor){
    const bool cached=immutable_state_cache_enabled() && !descriptor.label;
    std::array<std::uint64_t,16> key{};
    if(cached){
        key[0]=descriptor.depthCompareFunction;key[1]=descriptor.depthWriteEnabled;
        for(unsigned face=0;face<2;++face){
            auto stencil=face?descriptor.backFaceStencil:descriptor.frontFaceStencil;const auto offset=2+7*face;
            key[offset]=stencil!=nil;
            key[offset+1]=stencil.stencilCompareFunction;key[offset+2]=stencil.stencilFailureOperation;
            key[offset+3]=stencil.depthFailureOperation;key[offset+4]=stencil.depthStencilPassOperation;
            key[offset+5]=stencil.readMask;key[offset+6]=stencil.writeMask;
        }
        if(const auto found=depth_states_.get(state_cache_key(key))){++depth_state_hits_;return *found;}
    }
    auto state=[device_ newDepthStencilStateWithDescriptor:descriptor];if(!state)throw std::bad_alloc{};++depth_state_builds_;
    if(cached)depth_states_.put(state_cache_key(key),state);return state;
}
// 純值版本：鍵與上面相同，只在未命中時建立描述物件（繪圖熱路徑每次繪製都會查詢）。
id<MTLDepthStencilState> cached_depth_state(const MetalDepthSpec& spec){
    if(!immutable_state_cache_enabled())return cached_depth_state(spec.make());
    std::array<std::uint64_t,16> key{};key[0]=spec.depthCompareFunction;key[1]=spec.depthWriteEnabled;
    for(unsigned face=0;face<2;++face){
        const auto& stencil=face?spec.backFaceStencil:spec.frontFaceStencil;const auto offset=2+7*face;
        key[offset]=1;key[offset+1]=stencil.stencilCompareFunction;key[offset+2]=stencil.stencilFailureOperation;
        key[offset+3]=stencil.depthFailureOperation;key[offset+4]=stencil.depthStencilPassOperation;
        key[offset+5]=stencil.readMask;key[offset+6]=stencil.writeMask;
    }
    if(const auto found=depth_states_.get(state_cache_key(key))){++depth_state_hits_;return *found;}
    auto state=[device_ newDepthStencilStateWithDescriptor:spec.make()];if(!state)throw std::bad_alloc{};++depth_state_builds_;
    depth_states_.put(state_cache_key(key),state);return state;
}
std::shared_ptr<MetalSampler> cached_sampler(MTLSamplerDescriptor* descriptor){
    const bool cached=immutable_state_cache_enabled() && !descriptor.label;
    std::array<std::uint64_t,16> key{};
    if(cached){
        key={descriptor.minFilter,descriptor.magFilter,descriptor.mipFilter,descriptor.maxAnisotropy,
             descriptor.sAddressMode,descriptor.tAddressMode,descriptor.rAddressMode,descriptor.borderColor,
             static_cast<std::uint64_t>(descriptor.normalizedCoordinates),std::bit_cast<std::uint32_t>(descriptor.lodMinClamp),
             std::bit_cast<std::uint32_t>(descriptor.lodMaxClamp),static_cast<std::uint64_t>(descriptor.lodAverage),
             descriptor.compareFunction,static_cast<std::uint64_t>(descriptor.supportArgumentBuffers),0,0};
        if(@available(macOS 26.0,*)){key[14]=descriptor.reductionMode;key[15]=std::bit_cast<std::uint32_t>(descriptor.lodBias);}
        if(const auto found=sampler_states_.get(state_cache_key(key))){++sampler_state_hits_;return *found;}
    }
    auto state=std::make_shared<MetalSampler>();state->sampler=[device_ newSamplerStateWithDescriptor:descriptor];if(!state->sampler)throw std::invalid_argument("Metal 取樣器不合法");++sampler_state_builds_;
    if(cached)sampler_states_.put(state_cache_key(key),state,sizeof(MetalSampler));return state;
}
id<MTLSamplerState> cached_sampler_state(MTLSamplerDescriptor* descriptor){return cached_sampler(descriptor)->sampler;}
