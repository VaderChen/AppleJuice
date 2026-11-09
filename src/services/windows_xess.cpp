#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace x64arm {
// 內建 libxess.dll（Intel XeSS 1.x 的公開 API）：升頻工作對應 Apple GPU 的 MetalFX 時間性升頻器。
// 遊戲以裸檔名載入 libxess.dll 時改由這裡承接（內建模組優先），遊戲自帶的 DLL 需要 SM 6.4 的 DXIL 著色器。
// xessD3D12Execute 錄成 D3D12 命令清單回呼，ExecuteCommandLists 重播到該位置時以裝置的 Metal 後端升頻。
// 參考：https://github.com/intel/xess（inc/xess/xess.h、xess_d3d12.h，MIT）。
struct XessState {
    struct Context {
        std::uint64_t device{},history{};bool initialized{};
        std::uint32_t width{},height{},quality{},flags{};
        float jitter_x{1},jitter_y{1},velocity_x{1},velocity_y{1};
    };
    std::map<std::uint64_t,Context> contexts;std::uint64_t next_history{1};
};
namespace {
// xess_result_t。
constexpr std::int32_t xess_success=0,xess_unsupported_device=-1,xess_uninitialized=-3,xess_invalid_argument=-4,xess_invalid_context=-8,xess_unsupported=-10,xess_wrong_call_order=-12;
// xess_init_flags_t：高解析度動態向量、反向深度、曝光紋理、反應遮罩、NDC 動態向量、外部描述元堆積、LDR 輸入、抖動的動態向量、自動曝光。
constexpr std::uint32_t flag_high_res_mv=1,flag_inverted_depth=2,flag_exposure_texture=4,flag_ndc_velocity=16,flag_jittered_mv=128,flag_auto_exposure=256,flag_known=0x1ff;
// 各品質設定的輸出／輸入比例（XeSS 1.x）。
double quality_ratio(std::uint32_t quality){
    switch(quality){case 100:return 3.0;case 101:return 2.0;case 102:return 1.7;case 103:return 1.5;case 104:return 1.3;case 105:return 1.2;case 106:return 1.0;default:return 0;}
}
}
std::span<const WindowsServiceDescriptor> xess_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"libxess.dll","xessGetVersion",1},{"libxess.dll","xessGetIntelXeFXVersion",2},{"libxess.dll","xessGetProperties",3},{"libxess.dll","xessGetInputResolution",4},
        {"libxess.dll","xessGetJitterScale",3},{"libxess.dll","xessGetVelocityScale",3},{"libxess.dll","xessSetJitterScale",3},{"libxess.dll","xessSetVelocityScale",3},
        {"libxess.dll","xessDestroyContext",1},{"libxess.dll","xessSetLoggingCallback",3},{"libxess.dll","xessIsOptimalDriver",1},{"libxess.dll","xessSelectNetworkModel",2},
        {"libxess.dll","xessSetContextParameterF",3},{"libxess.dll","xessGetProfilingData",2},{"libxess.dll","xessStartDump",2},
        {"libxess.dll","xessD3D12CreateContext",2},{"libxess.dll","xessD3D12BuildPipelines",4},{"libxess.dll","xessD3D12Init",2},{"libxess.dll","xessD3D12GetInitParams",2},
        {"libxess.dll","xessD3D12Execute",3},{"libxess.dll","xessD3D12GetResourcesToDump",2},
    };
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_xess(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const std::string_view symbol=service.symbol;
    // 診斷：X64ARM_TRACE_XESS=1 於 stderr 列出每次呼叫與結果。
    static const bool trace=[]{const char* value=std::getenv("X64ARM_TRACE_XESS");return value && *value && *value!='0';}();
    const auto finish=[&](std::int32_t value){
        if(trace)std::fprintf(stderr,"[xess] %.*s a0=0x%llx a1=0x%llx -> %d\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(arg(0)),
            static_cast<unsigned long long>(service.arguments>1?arg(1):0),value);
        arg.return_to(state,static_cast<std::uint32_t>(value));return std::optional<std::uint32_t>{};};
    if(x86_)return finish(xess_unsupported_device);
    if(!xess_)xess_=std::make_shared<XessState>();
    auto& xess=*xess_;
    const auto load=[&](std::uint64_t at,unsigned bits){return memory.load(GuestAddress{at},static_cast<std::uint8_t>(bits));};
    const auto store=[&](std::uint64_t at,unsigned bits,std::uint64_t value){memory.store(GuestAddress{at},static_cast<std::uint8_t>(bits),value);};
    const auto load_float=[&](std::uint64_t at){const auto bits=static_cast<std::uint32_t>(load(at,32));float value;std::memcpy(&value,&bits,4);return value;};
    const auto store_float=[&](std::uint64_t at,float value){std::uint32_t bits;std::memcpy(&bits,&value,4);store(at,32,bits);};
    if(symbol=="xessGetVersion"){if(!arg(0))return finish(xess_invalid_argument);store(arg(0),16,1);store(arg(0)+2,16,1);store(arg(0)+4,16,0);store(arg(0)+6,16,0);return finish(xess_success);}
    if(symbol=="xessD3D12CreateContext"){
        if(!arg(1))return finish(xess_invalid_argument);store(arg(1),64,0);
        const auto device=d3d12_device_of(arg(0));if(!device || device!=arg(0))return finish(xess_invalid_argument);
        const auto handle=allocate_heap(memory,16);if(!handle)throw std::bad_alloc();memory.fill(GuestAddress{handle},0,16);
        XessState::Context context;context.device=device;context.history=xess.next_history++;xess.contexts.emplace(handle,context);
        store(arg(1),64,handle);return finish(xess_success);
    }
    const auto found=xess.contexts.find(arg(0));
    if(found==xess.contexts.end())return finish(xess_invalid_context);
    auto& context=found->second;
    if(symbol=="xessDestroyContext"){(void)free_heap(memory,arg(0),state);xess.contexts.erase(found);return finish(xess_success);}
    if(symbol=="xessGetIntelXeFXVersion")return finish(xess_unsupported_device);
    if(symbol=="xessIsOptimalDriver" || symbol=="xessSetLoggingCallback" || symbol=="xessSelectNetworkModel" || symbol=="xessSetContextParameterF")return finish(xess_success);
    if(symbol=="xessD3D12BuildPipelines")return finish(xess_success);
    if(symbol=="xessGetProfilingData" || symbol=="xessStartDump" || symbol=="xessD3D12GetResourcesToDump")return finish(xess_unsupported);
    if(symbol=="xessGetJitterScale" || symbol=="xessGetVelocityScale"){
        if(!arg(1) || !arg(2))return finish(xess_invalid_argument);const bool jitter=symbol=="xessGetJitterScale";
        store_float(arg(1),jitter?context.jitter_x:context.velocity_x);store_float(arg(2),jitter?context.jitter_y:context.velocity_y);return finish(xess_success);
    }
    if(symbol=="xessSetJitterScale" || symbol=="xessSetVelocityScale"){
        // float 參數在 x64 以 XMM1／XMM2 傳入（前四個參數依位置使用浮點暫存器）。
        const auto fp=[&](unsigned i){std::uint32_t word;std::memcpy(&word,state.xmm[i].data(),4);return std::bit_cast<float>(word);};
        const auto x=fp(1),y=fp(2);if(!std::isfinite(x) || !std::isfinite(y))return finish(xess_invalid_argument);
        if(symbol=="xessSetJitterScale"){context.jitter_x=x;context.jitter_y=y;}else{context.velocity_x=x;context.velocity_y=y;}return finish(xess_success);
    }
    if(symbol=="xessGetProperties"){
        // 不需要外部暫存堆積或描述元：MetalFX 自行管理暫存資源。xess_properties_t：uint32 描述元數、uint64 緩衝堆積、uint64 紋理堆積。
        if(!arg(1) || !arg(2))return finish(xess_invalid_argument);store(arg(2),32,0);store(arg(2)+4,32,0);store(arg(2)+8,64,0);store(arg(2)+16,64,0);return finish(xess_success);
    }
    if(symbol=="xessGetInputResolution"){
        if(!arg(1) || !arg(3))return finish(xess_invalid_argument);const auto ratio=quality_ratio(static_cast<std::uint32_t>(arg(2)));if(ratio<=0)return finish(xess_invalid_argument);
        const auto width=static_cast<std::uint32_t>(load(arg(1),32)),height=static_cast<std::uint32_t>(load(arg(1)+4,32));if(!width || !height)return finish(xess_invalid_argument);
        store(arg(3),32,std::max<std::uint32_t>(1,static_cast<std::uint32_t>(std::lround(width/ratio))));store(arg(3)+4,32,std::max<std::uint32_t>(1,static_cast<std::uint32_t>(std::lround(height/ratio))));
        return finish(xess_success);
    }
    if(symbol=="xessD3D12Init"){
        // xess_d3d12_init_params_t：outputResolution(0)、qualitySetting(8)、initFlags(12)、兩個節點遮罩(16、20)、暫存堆積與位移(24…56)、管線庫(64)。
        if(!arg(1))return finish(xess_invalid_argument);
        const auto width=static_cast<std::uint32_t>(load(arg(1),32)),height=static_cast<std::uint32_t>(load(arg(1)+4,32)),quality=static_cast<std::uint32_t>(load(arg(1)+8,32)),flags=static_cast<std::uint32_t>(load(arg(1)+12,32));
        if(!width || !height || quality_ratio(quality)<=0 || (flags&~flag_known))return finish(xess_invalid_argument);
        // MetalFX 的動態向量與輸入同解析度；輸出解析度的動態向量不支援。
        if(flags&flag_high_res_mv)return finish(xess_unsupported);
        context.width=width;context.height=height;context.quality=quality;context.flags=flags;context.initialized=true;return finish(xess_success);
    }
    if(symbol=="xessD3D12GetInitParams"){
        if(!arg(1))return finish(xess_invalid_argument);if(!context.initialized)return finish(xess_uninitialized);
        memory.fill(GuestAddress{arg(1)},0,72);store(arg(1),32,context.width);store(arg(1)+4,32,context.height);store(arg(1)+8,32,context.quality);store(arg(1)+12,32,context.flags);
        store(arg(1)+16,32,1);store(arg(1)+20,32,1);return finish(xess_success);
    }
    if(symbol=="xessD3D12Execute"){
        if(!context.initialized)return finish(xess_wrong_call_order);if(!arg(1) || !arg(2))return finish(xess_invalid_argument);
        // xess_d3d12_execute_params_t：六個資源指標(0…40)、jitter(48、52)、exposureScale(56)、resetHistory(60)、輸入尺寸(64、68)、
        // 各輸入與輸出的基準座標(72…119)、外部描述元堆積(120、128)。
        const auto p=arg(2);const auto color=load(p,64),velocity=load(p+8,64),depth=load(p+16,64),exposure=load(p+24,64),output=load(p+40,64);
        if(!color || !velocity || !depth || !output || ((context.flags&flag_exposure_texture) && !exposure))return finish(xess_invalid_argument);
        for(std::uint64_t at=72;at<120;at+=4)if(load(p+at,32))return finish(xess_invalid_argument); // MetalFX 從紋理原點讀寫。
        GraphicsTemporalUpscale u;
        u.input_width=static_cast<std::uint32_t>(load(p+64,32));u.input_height=static_cast<std::uint32_t>(load(p+68,32));u.output_width=context.width;u.output_height=context.height;
        if(!u.input_width || !u.input_height)return finish(xess_invalid_argument);
        u.jitter_x=load_float(p+48)*context.jitter_x;u.jitter_y=load_float(p+52)*context.jitter_y;
        const float exposure_scale=load_float(p+56);u.pre_exposure=exposure_scale>0 && std::isfinite(exposure_scale)?1.0F/exposure_scale:1.0F;
        u.reset=load(p+60,32)!=0;u.depth_reversed=(context.flags&flag_inverted_depth)!=0;u.auto_exposure=(context.flags&flag_auto_exposure)!=0;u.history=context.history;
        // 動態向量換算成輸入像素：NDC 動態向量的 x 向右為正、y 向上為正。
        u.motion_scale_x=context.velocity_x*((context.flags&flag_ndc_velocity)?0.5F*static_cast<float>(u.input_width):1.0F);
        u.motion_scale_y=context.velocity_y*((context.flags&flag_ndc_velocity)?-0.5F*static_cast<float>(u.input_height):1.0F);
        (void)flag_jittered_mv;
        const bool use_exposure=(context.flags&flag_exposure_texture)!=0;
        const bool recorded=d3d12_record(memory,arg(1),[u,color,velocity,depth,exposure,output,use_exposure](NativeGraphics& native,const std::function<std::shared_ptr<GraphicsResource>(std::uint64_t)>& resource_of){
            auto upscale=u;upscale.color=GraphicsView{resource_of(color)};upscale.motion=GraphicsView{resource_of(velocity)};upscale.depth=GraphicsView{resource_of(depth)};
            upscale.output=GraphicsView{resource_of(output)};if(use_exposure)upscale.exposure=GraphicsView{resource_of(exposure)};
            try{native.graphics_temporal_upscale(upscale);}
            catch(const std::exception& error){static std::atomic<unsigned> reported{0};if(reported.fetch_add(1)<4)std::fprintf(stderr,"[xess] MetalFX 升頻失敗：%s\n",error.what());}
        });
        return finish(recorded?xess_success:xess_invalid_argument);
    }
    return finish(xess_unsupported);
}
}
