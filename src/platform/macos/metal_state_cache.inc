// MetalGl 各自持有不可變狀態，D3D9／10／11 與 OpenGL 共用相同內容鍵。
// command buffer 保留已編碼的 Metal 物件；快取輪替不縮短在途命令的生命週期。
std::map<std::array<std::uint64_t,16>,id<MTLDepthStencilState>> depth_states_;
std::map<std::array<std::uint64_t,16>,id<MTLSamplerState>> sampler_states_;
std::uint64_t depth_state_hits_{},depth_state_builds_{},sampler_state_hits_{},sampler_state_builds_{};
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
        if(const auto found=depth_states_.find(key);found!=depth_states_.end()){++depth_state_hits_;return found->second;}
    }
    auto state=[device_ newDepthStencilStateWithDescriptor:descriptor];if(!state)throw std::bad_alloc{};++depth_state_builds_;
    if(cached){if(depth_states_.size()>=512)depth_states_.clear();depth_states_.emplace(key,state);}return state;
}
id<MTLSamplerState> cached_sampler_state(MTLSamplerDescriptor* descriptor){
    const bool cached=immutable_state_cache_enabled() && !descriptor.label;
    std::array<std::uint64_t,16> key{};
    if(cached){
        key={descriptor.minFilter,descriptor.magFilter,descriptor.mipFilter,descriptor.maxAnisotropy,
             descriptor.sAddressMode,descriptor.tAddressMode,descriptor.rAddressMode,descriptor.borderColor,
             static_cast<std::uint64_t>(descriptor.normalizedCoordinates),std::bit_cast<std::uint32_t>(descriptor.lodMinClamp),
             std::bit_cast<std::uint32_t>(descriptor.lodMaxClamp),static_cast<std::uint64_t>(descriptor.lodAverage),
             descriptor.compareFunction,static_cast<std::uint64_t>(descriptor.supportArgumentBuffers),0,0};
        if(@available(macOS 26.0,*)){key[14]=descriptor.reductionMode;key[15]=std::bit_cast<std::uint32_t>(descriptor.lodBias);}
        if(const auto found=sampler_states_.find(key);found!=sampler_states_.end()){++sampler_state_hits_;return found->second;}
    }
    auto state=[device_ newSamplerStateWithDescriptor:descriptor];if(!state)throw std::invalid_argument("Metal 取樣器不合法");++sampler_state_builds_;
    if(cached){if(sampler_states_.size()>=512)sampler_states_.clear();sampler_states_.emplace(key,state);}return state;
}
