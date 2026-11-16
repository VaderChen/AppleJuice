// Metal 後端的累積緩衝與邏輯運算（opengl_metal.mm 引入）。
// 累積緩衝：繪製表面大小的 RGBA32Float 紋理，第一次使用時配置並整張清為 0，表面尺寸改變時重建。
// GL_ACCUM／GL_LOAD 讀取目前讀取緩衝的色彩，GL_MULT／GL_ADD 與 glClear(GL_ACCUM_BUFFER_BIT) 只改累積值，都以運算著色器在剪裁區域內處理；
// GL_RETURN 把 value 倍的累積值（夾到 [0,1]）畫回繪製緩衝，只受剪裁測試與色彩寫入遮罩影響，不經混色、alpha／深度／模板測試、霧化與邏輯運算。
// glLogicOp：啟用 GL_COLOR_LOGIC_OP 且模式不是 GL_COPY 時，片段著色器讀取目前像素（framebuffer fetch）後以 8 位元整數做位元運算並取代混色；
// GL_INDEX_LOGIC_OP 只保存狀態（本後端為 RGBA 像素格式）；glClear 不受影響。GLSL 程式的繪製不套用邏輯運算。
namespace {
constexpr unsigned accum_channel_bits=16;
}
std::uint64_t MetalGl::accumulation(GlOperation op,std::span<const std::uint64_t> a){
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};const auto f=[&](unsigned i){return std::bit_cast<float>(u(i));};
    if(op==GlOperation::ClearAccum){for(unsigned i=0;i<4;++i)state_.accum_clear[i]=std::clamp(f(i),-1.F,1.F);return 0;}
    if(op==GlOperation::LogicOp){if(u(0)<0x1500 || u(0)>0x150f)error(0x500);else state_.logic_op=u(0);return 0;}
    // GL_ACCUM 0x100、GL_LOAD 0x101、GL_RETURN 0x102、GL_MULT 0x103、GL_ADD 0x104。
    if(u(0)<0x100 || u(0)>0x104){error(0x500);return 0;}if(begun_){error(0x502);return 0;}
    if(state_.render_mode!=0x1c00 || !width_ || !height_)return 0;
    if(u(0)==0x102)accum_return(f(1));else accum_compute(u(0),f(1),{});
    return 0;
}
std::optional<std::vector<float>> MetalGl::accumulation_query(unsigned pname)const{
    if(pname==0xb80)return std::vector<float>(state_.accum_clear.begin(),state_.accum_clear.end());
    if(pname>=0xd58 && pname<=0xd5b)return std::vector<float>{static_cast<float>(accum_channel_bits)};
    if(pname==0xbf0)return std::vector<float>{static_cast<float>(state_.logic_op)};
    return std::nullopt;
}
bool MetalGl::logic_active(bool clearing,bool fixed)const{
    return !clearing && !fixed && pixel_draw_.kind!=3 && pixel_draw_.kind!=4 && state_.enabled.contains(0xbf2) && state_.logic_op!=0x1503;
}
void MetalGl::ensure_accum(){
    if(accum_)return;
    auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:width_ height:height_ mipmapped:NO];
    desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
    auto lease=reserve_gpu_memory(static_cast<std::size_t>(width_)*height_*16);auto texture=[device_ newTextureWithDescriptor:desc];if(!texture)throw std::bad_alloc{};
    accum_=texture;accum_lease_=std::move(lease);
    // 初始內容未定義，先整張清為 0。
    accum_dispatch(0,0,{},MTLScissorRect{0,0,width_,height_});
}
void MetalGl::accum_compute(unsigned op,float value,Vec clear){
    const auto region=scissor(state_.enabled.contains(0xc11),width_,height_);if(!region.width || !region.height)return;
    ensure_accum();accum_dispatch(op,value,clear,region);
}
void MetalGl::accum_dispatch(unsigned op,float value,Vec clear,MTLScissorRect region){
    if(!accum_pipeline_){
        NSError* e=nil;auto function=[library_ newFunctionWithName:@"accum_operation"];accum_pipeline_=function?[device_ newComputePipelineStateWithFunction:function error:&e]:nil;
        if(!accum_pipeline_)throw std::runtime_error(std::string("Metal 累積緩衝運算管線建立失敗：")+(e?e.localizedDescription.UTF8String:"找不到函式"));
    }
    auto enc=compute_encoder();[enc setComputePipelineState:accum_pipeline_];[enc setTexture:accum_ atIndex:0];[enc setTexture:(state_.read_buffer==0x404?front_:color_) atIndex:1];
    // params[0]：value、運算（0 為清除）；params[1]：清除值。region 為 Metal 座標（左上原點）的剪裁矩形。
    const std::array<float,8> params{value,static_cast<float>(op),0,0,clear[0],clear[1],clear[2],clear[3]};[enc setBytes:params.data() length:sizeof(params) atIndex:0];
    const std::array<std::uint32_t,4> area{static_cast<std::uint32_t>(region.x),static_cast<std::uint32_t>(region.y),static_cast<std::uint32_t>(region.width),static_cast<std::uint32_t>(region.height)};
    [enc setBytes:area.data() length:sizeof(area) atIndex:1];
    [enc dispatchThreads:MTLSizeMake(region.width,region.height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
}
void MetalGl::accum_return(float value){
    ensure_accum();
    pixel_draw_={4,accum_,{value,0,0,0,0,0,0,0}};struct Reset {PixelDraw& draw;~Reset(){draw={};}} reset{pixel_draw_};
    window_space([&](const auto& clip){
        const double w=width_,h=height_;const auto a0=clip(0,0),a1=clip(w,0),a2=clip(w,h),a3=clip(0,h);vertex(a0);vertex(a1);vertex(a2);vertex(a0);vertex(a2);vertex(a3);
    });
}
