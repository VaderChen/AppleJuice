// 畫面內插（X64ARM_PRESENT_INTERPOLATION=1）：不依賴程式提供深度或動態向量，只用前後兩張已完成的畫面，
// 以 VideoToolbox 的低延遲畫面內插（macOS 26 以上）生成中間一張。內插器只接受 NV12（420v）且來源上限 1920×1080，
// 所以流程是：交換鏈畫面 → BT.709 影片範圍的 Y（R8）與 CbCr（RG8、半解析度）兩個 IOSurface 平面 → 內插 → 轉回 RGB。
// 只有生成的那一張經過色彩轉換與色度降採樣；真實畫面維持原樣呈現。全部工作都編進呼叫端的 Metal 命令緩衝，不做 CPU 同步。
// 模型載入要 1～2 秒，在背景佇列啟動；就緒前與不支援時 encode 回傳 nil，呈現流程照常只顯示真實畫面。
struct MetalFrameInterpolation {
    virtual ~MetalFrameInterpolation()=default;
    // 把 current（任何可取樣的 RGB 貼圖，內容是已編碼的顯示值）記為最新畫面，並回傳「上一張與這一張中間」的生成畫面；
    // 第一張、尚未就緒或失敗時回傳 nil。回傳的貼圖在下一次 encode 之前有效。
    virtual id<MTLTexture> encode(id<MTLCommandBuffer> commands,id<MTLTexture> current)=0;
};
inline bool metal_frame_interpolation_wanted() noexcept {
    static const bool value=[] {const char* text=std::getenv("X64ARM_PRESENT_INTERPOLATION");return text && std::string_view(text)=="1";}();
    return value;
}
class API_AVAILABLE(macos(26.0)) MetalVideoFrameInterpolation final:public MetalFrameInterpolation {
public:
    MetalVideoFrameInterpolation(id<MTLDevice> device,unsigned width,unsigned height,MTLPixelFormat output_format){ try {
        // 內插器要偶數寬高：奇數尺寸的交換鏈（例如 1281×721）以少一個像素的 NV12 緩衝處理，轉換著色器依正規化座標取樣，輸出仍是原尺寸。
        const unsigned output_width=width,output_height=height;width&=~1U;height&=~1U;
        auto configuration=[[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:width frameHeight:height numberOfInterpolatedFrames:1];
        if(!configuration)throw std::runtime_error("畫面內插不支援此解析度");
        NSMutableDictionary* source=[configuration.sourcePixelBufferAttributes mutableCopy];source[(id)kCVPixelBufferMetalCompatibilityKey]=@YES;
        NSMutableDictionary* destination=[configuration.destinationPixelBufferAttributes mutableCopy];destination[(id)kCVPixelBufferMetalCompatibilityKey]=@YES;
        for(unsigned i=0;i<3;++i){
            CVPixelBufferRef buffer=nullptr;
            if(CVPixelBufferCreate(kCFAllocatorDefault,width,height,kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,(__bridge CFDictionaryRef)(i<2?source:destination),&buffer)!=kCVReturnSuccess || !buffer)throw std::bad_alloc{};
            buffers_[i]=buffer;
            const auto surface=CVPixelBufferGetIOSurface(buffer);if(!surface)throw std::runtime_error("畫面內插的像素緩衝不是 IOSurface");
            auto luma=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:width height:height mipmapped:NO];luma.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
            auto chroma=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:width/2 height:height/2 mipmapped:NO];chroma.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
            luma_[i]=[device newTextureWithDescriptor:luma iosurface:surface plane:0];chroma_[i]=[device newTextureWithDescriptor:chroma iosurface:surface plane:1];
            if(!luma_[i] || !chroma_[i])throw std::bad_alloc{};
        }
        auto output=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:output_format width:output_width height:output_height mipmapped:NO];
        output.storageMode=MTLStorageModePrivate;output.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;output_=[device newTextureWithDescriptor:output];if(!output_)throw std::bad_alloc{};
        static const char* source_code=R"(#include <metal_stdlib>
using namespace metal;
struct Out {float4 position [[position]];float2 uv;};
vertex Out fullscreen(uint id [[vertex_id]]){Out o;float2 p=float2((id<<1)&2,id&2);o.position=float4(p*2.0-1.0,0,1);o.uv=float2(p.x,1.0-p.y);return o;}
constant sampler linear_sampler(filter::linear,address::clamp_to_edge);
fragment float4 to_luma(Out in [[stage_in]],texture2d<float> rgb [[texture(0)]]){float3 c=rgb.sample(linear_sampler,in.uv).rgb;return float4(16.0/255.0+dot(c,float3(0.2126,0.7152,0.0722))*(219.0/255.0),0,0,1);}
fragment float4 to_chroma(Out in [[stage_in]],texture2d<float> rgb [[texture(0)]]){float3 c=rgb.sample(linear_sampler,in.uv).rgb;
    return float4(128.0/255.0+dot(c,float3(-0.114572,-0.385428,0.5))*(224.0/255.0),128.0/255.0+dot(c,float3(0.5,-0.454153,-0.045847))*(224.0/255.0),0,1);}
fragment float4 to_rgb(Out in [[stage_in]],texture2d<float> luma [[texture(0)]],texture2d<float> chroma [[texture(1)]]){
    float y=(luma.sample(linear_sampler,in.uv).r-16.0/255.0)*(255.0/219.0);float2 c=(chroma.sample(linear_sampler,in.uv).rg-128.0/255.0)*(255.0/224.0);
    return float4(saturate(float3(y+1.5748*c.y,y-0.187324*c.x-0.468124*c.y,y+1.8556*c.x)),1);}
)";
        NSError* error=nil;auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source_code] options:nil error:&error];
        if(!library)throw std::runtime_error(std::string("畫面內插的著色器編譯失敗：")+(error.localizedDescription.UTF8String?:""));
        const auto pipeline=[&](NSString* fragment,MTLPixelFormat format){
            auto descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=[library newFunctionWithName:@"fullscreen"];descriptor.fragmentFunction=[library newFunctionWithName:fragment];
            descriptor.colorAttachments[0].pixelFormat=format;NSError* failure=nil;id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:descriptor error:&failure];
            if(!state)throw std::runtime_error("畫面內插的管線建立失敗");return state;
        };
        to_luma_=pipeline(@"to_luma",MTLPixelFormatR8Unorm);to_chroma_=pipeline(@"to_chroma",MTLPixelFormatRG8Unorm);to_rgb_=pipeline(@"to_rgb",output_format);
        processor_=[VTFrameProcessor new];
        // 0＝載入中、1＝就緒、2＝失敗。工作階段在背景佇列啟動，不擋住呈現。
        auto state=state_;auto processor=processor_;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
            NSError* failure=nil;const BOOL started=[processor startSessionWithConfiguration:configuration error:&failure];
            if(!started)std::fprintf(stderr,"[present-interpolation] 啟動失敗：%s\n",failure.localizedDescription.UTF8String?:"");
            state->store(started?1:2,std::memory_order_release);
        });
    } catch(...) {
        // 建構中途失敗：解構函式不會執行，已建立的像素緩衝在這裡釋放。
        for(auto& buffer:buffers_)if(buffer){CVPixelBufferRelease(buffer);buffer=nullptr;}
        throw;
    } }
    ~MetalVideoFrameInterpolation() override {
        // 載入尚未完成時不呼叫 endSession（背景區塊仍持有 processor，完成後隨它釋放）。
        if(state_->load(std::memory_order_acquire)==1)[processor_ endSession];
        for(auto& buffer:buffers_)if(buffer)CVPixelBufferRelease(buffer);
    }
    id<MTLTexture> encode(id<MTLCommandBuffer> commands,id<MTLTexture> current) override {
        if(state_->load(std::memory_order_acquire)!=1){primed_=false;return nil;}
        const unsigned slot=index_;index_^=1U;
        pass(commands,luma_[slot],to_luma_,current,nil);pass(commands,chroma_[slot],to_chroma_,current,nil);
        ++frame_;
        if(!primed_){primed_=true;return nil;}
        auto previous=[[VTFrameProcessorFrame alloc] initWithBuffer:buffers_[slot^1U] presentationTimeStamp:CMTimeMake(static_cast<int64_t>(frame_-1)*2,120)];
        auto source=[[VTFrameProcessorFrame alloc] initWithBuffer:buffers_[slot] presentationTimeStamp:CMTimeMake(static_cast<int64_t>(frame_)*2,120)];
        auto destination=[[VTFrameProcessorFrame alloc] initWithBuffer:buffers_[2] presentationTimeStamp:CMTimeMake(static_cast<int64_t>(frame_)*2-1,120)];
        if(!previous || !source || !destination)return nil;
        auto parameters=[[VTLowLatencyFrameInterpolationParameters alloc] initWithSourceFrame:source previousFrame:previous interpolationPhase:@[@0.5f] destinationFrames:@[destination]];
        if(!parameters)return nil;
        [processor_ processWithCommandBuffer:commands parameters:parameters];
        pass(commands,output_,to_rgb_,luma_[2],chroma_[2]);
        return output_;
    }
private:
    void pass(id<MTLCommandBuffer> commands,id<MTLTexture> target,id<MTLRenderPipelineState> pipeline,id<MTLTexture> first,id<MTLTexture> second){
        auto descriptor=[MTLRenderPassDescriptor renderPassDescriptor];descriptor.colorAttachments[0].texture=target;descriptor.colorAttachments[0].loadAction=MTLLoadActionDontCare;descriptor.colorAttachments[0].storeAction=MTLStoreActionStore;
        auto encoder=[commands renderCommandEncoderWithDescriptor:descriptor];[encoder setRenderPipelineState:pipeline];[encoder setFragmentTexture:first atIndex:0];if(second)[encoder setFragmentTexture:second atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    }
    std::array<CVPixelBufferRef,3> buffers_{};std::array<id<MTLTexture>,3> luma_{},chroma_{};id<MTLTexture> output_=nil;
    id<MTLRenderPipelineState> to_luma_=nil,to_chroma_=nil,to_rgb_=nil;VTFrameProcessor* processor_=nil;
    std::shared_ptr<std::atomic<int>> state_=std::make_shared<std::atomic<int>>(0);
    unsigned index_=0;bool primed_=false;std::uint64_t frame_=0;
};
// 不支援（系統版本、裝置或解析度超過上限）時回傳空指標。
inline std::unique_ptr<MetalFrameInterpolation> metal_frame_interpolation_create(id<MTLDevice> device,unsigned width,unsigned height,MTLPixelFormat output_format){
    // 上限以實際送進內插器的偶數尺寸判斷（1921×1081 的交換鏈以 1920×1080 處理）。
    if(const auto even_width=width&~1U,even_height=height&~1U;!metal_frame_interpolation_wanted() || even_width<2 || even_height<2 || even_width>1920U || even_height>1920U || std::uint64_t{even_width}*even_height>1920ULL*1080ULL){if(metal_frame_interpolation_wanted())std::fprintf(stderr,"[present-interpolation] 不支援的畫面大小 %ux%u\n",width,height);return nullptr;}
    if(@available(macOS 26.0,*)){
        if(!VTLowLatencyFrameInterpolationConfiguration.supported){std::fprintf(stderr,"[present-interpolation] 此裝置不支援低延遲畫面內插\n");return nullptr;}
        try{auto result=std::make_unique<MetalVideoFrameInterpolation>(device,width,height,output_format);std::fprintf(stderr,"[present-interpolation] 已建立 %ux%u\n",width,height);return result;}catch(const std::exception& error){std::fprintf(stderr,"[present-interpolation] 建立失敗：%s\n",error.what());return nullptr;}
    }
    std::fprintf(stderr,"[present-interpolation] 需要 macOS 26 以上\n");
    return nullptr;
}
