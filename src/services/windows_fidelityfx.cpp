#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace x64arm {
// 內建 AMD FidelityFX API（FSR 3.1 起的 ffx_api：ffxCreateContext／ffxDestroyContext／ffxConfigure／ffxQuery／ffxDispatch）：
// 升頻工作對應 Apple GPU 的 MetalFX 時間性升頻器，與內建 libxess.dll 共用 D3D12 命令清單回呼與 Metal 後端。
// amd_fidelityfx_loader_dx12.dll（主程式直接匯入）、amd_fidelityfx_upscaler_dx12.dll（執行期載入）、amd_fidelityfx_dx12.dll（FSR 3.1.0～3.1.3）
// 以裸檔名載入時都由這裡承接；遊戲自帶的 DLL 需要 SM 6.x 的 DXIL 運算著色器。畫面生成與其交換鏈回「沒有提供者」，遊戲據此關閉該選項。
// 參考：https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK（ffx_api.h、ffx_api_types.h、ffx_upscale.h、dx12/ffx_api_dx12.h，MIT）。
struct FidelityFxState {
    struct Context {
        std::uint64_t device{},history{};std::uint32_t flags{},max_render_width{},max_render_height{},max_upscale_width{},max_upscale_height{};
    };
    std::map<std::uint64_t,Context> contexts;std::uint64_t next_history{1ULL<<40};std::uint64_t version_name{};
};
namespace {
// ffxReturnCode_t。
constexpr std::uint32_t ffx_ok=0,ffx_error=1,ffx_unknown_desctype=2,ffx_no_provider=4,ffx_parameter=6;
// 結構型別（ffxStructType_t）。
constexpr std::uint64_t desc_global_debug=1,desc_get_versions=4,desc_override_version=5,desc_provider_version=6,desc_backend_dx12=2;
constexpr std::uint64_t desc_create_upscale=0x00010000,desc_dispatch_upscale=0x00010001,desc_ratio=0x00010002,desc_render_resolution=0x00010003,
    desc_jitter_count=0x00010004,desc_jitter_offset=0x00010005,desc_reactive_mask=0x00010006,desc_keyvalue=0x00010007,desc_memory_usage=0x00010008;
// ffxCreateContextDescUpscale.flags：HDR、顯示解析度動態向量、動態向量抵銷抖動、反向深度、無限遠深度、自動曝光、動態解析度、除錯檢查、非線性色彩空間。
constexpr std::uint32_t flag_display_motion=1U<<1,flag_depth_inverted=1U<<3,flag_auto_exposure=1U<<5,flag_known=0x3ffU;
// 提供者版本：每個內建實作一個固定編號。
constexpr std::uint64_t provider_version=0x4d46580003010400ULL; // "MFX" 3.1.4
constexpr char provider_name[]="3.1.4 (MetalFX)";
// 各品質模式的輸出／輸入比例：原生 AA、品質、平衡、效能、極致效能。
float quality_ratio(std::uint32_t mode){
    switch(mode){case 0:return 1.0F;case 1:return 1.5F;case 2:return 1.7F;case 3:return 2.0F;case 4:return 3.0F;default:return 0;}
}
// FSR 的抖動序列：Halton(2,3)，相位數 8×(顯示寬／繪製寬)²。
float halton(std::int32_t index,std::int32_t base){
    float f=1.0F,result=0.0F;
    for(auto current=index;current>0;current/=base){f/=static_cast<float>(base);result+=f*static_cast<float>(current%base);}
    return result;
}
}
std::span<const WindowsServiceDescriptor> fidelityfx_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"amd_fidelityfx_loader_dx12.dll","ffxCreateContext",3},{"amd_fidelityfx_loader_dx12.dll","ffxDestroyContext",2},
        {"amd_fidelityfx_loader_dx12.dll","ffxConfigure",2},{"amd_fidelityfx_loader_dx12.dll","ffxQuery",2},{"amd_fidelityfx_loader_dx12.dll","ffxDispatch",2},
    };
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_fidelityfx(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const std::string_view symbol=service.symbol;
    // 診斷：X64ARM_TRACE_FIDELITYFX=1 於 stderr 列出每次呼叫、描述結構型別與結果。
    static const bool trace=[]{const char* value=std::getenv("X64ARM_TRACE_FIDELITYFX");return value && *value && *value!='0';}();
    const auto load=[&](std::uint64_t at,unsigned bits){return memory.load(GuestAddress{at},static_cast<std::uint8_t>(bits));};
    const auto store=[&](std::uint64_t at,unsigned bits,std::uint64_t value){memory.store(GuestAddress{at},static_cast<std::uint8_t>(bits),value);};
    const auto load_float=[&](std::uint64_t at){const auto bits=static_cast<std::uint32_t>(load(at,32));float value;std::memcpy(&value,&bits,4);return value;};
    const auto store_float=[&](std::uint64_t at,float value){std::uint32_t bits;std::memcpy(&bits,&value,4);store(at,32,bits);};
    std::uint64_t traced_type=0;
    const auto finish=[&](std::uint32_t value){
        if(trace)std::fprintf(stderr,"[fidelityfx] %.*s ctx=0x%llx type=0x%llx -> %u\n",static_cast<int>(symbol.size()),symbol.data(),
            static_cast<unsigned long long>(arg(0)),static_cast<unsigned long long>(traced_type),value);
        arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(x86_)return finish(ffx_no_provider);
    if(!fidelityfx_)fidelityfx_=std::make_shared<FidelityFxState>();
    auto& fx=*fidelityfx_;
    // 描述結構以 ffxApiHeader（type、pNext）串接；沿 pNext 找指定型別，步數設上限避免循環。
    const auto find=[&](std::uint64_t desc,std::uint64_t type)->std::uint64_t{
        for(unsigned steps=0;desc && steps<64;++steps,desc=load(desc+8,64))if(load(desc,64)==type)return desc;
        return 0;};
    const auto context_of=[&](std::uint64_t pointer)->FidelityFxState::Context*{
        if(!pointer)return nullptr;const auto handle=load(pointer,64);const auto found=fx.contexts.find(handle);return found==fx.contexts.end()?nullptr:&found->second;};

    if(symbol=="ffxCreateContext"){
        if(!arg(0) || !arg(1))return finish(ffx_parameter);
        traced_type=load(arg(1),64);store(arg(0),64,0);
        if(traced_type!=desc_create_upscale)return finish(ffx_no_provider); // 畫面生成、其交換鏈與其他效果沒有內建提供者。
        const auto desc=arg(1);const auto backend=find(load(desc+8,64),desc_backend_dx12);if(!backend)return finish(ffx_parameter);
        const auto device=d3d12_device_of(load(backend+16,64));if(!device)return finish(ffx_parameter);
        if(const auto version=find(load(desc+8,64),desc_override_version);version && load(version+16,64)!=provider_version)return finish(ffx_no_provider);
        // ffxCreateContextDescUpscale：flags(16)、maxRenderSize(20)、maxUpscaleSize(28)、fpMessage(40)。
        FidelityFxState::Context context;context.device=device;context.history=fx.next_history++;context.flags=static_cast<std::uint32_t>(load(desc+16,32));
        context.max_render_width=static_cast<std::uint32_t>(load(desc+20,32));context.max_render_height=static_cast<std::uint32_t>(load(desc+24,32));
        context.max_upscale_width=static_cast<std::uint32_t>(load(desc+28,32));context.max_upscale_height=static_cast<std::uint32_t>(load(desc+32,32));
        if((context.flags&~flag_known) || !context.max_upscale_width || !context.max_upscale_height)return finish(ffx_parameter);
        // MetalFX 的動態向量與輸入同解析度；顯示解析度的動態向量不支援。
        if(context.flags&flag_display_motion)return finish(ffx_no_provider);
        const auto handle=allocate_heap(memory,16);if(!handle)throw std::bad_alloc();memory.fill(GuestAddress{handle},0,16);
        fx.contexts.emplace(handle,context);store(arg(0),64,handle);
        // 遊戲啟用 FSR：內建的補幀與 2 倍放大停用，銷毀後恢復。
        game_upscaler_contexts().fetch_add(1,std::memory_order_acq_rel);
        return finish(ffx_ok);
    }
    if(symbol=="ffxDestroyContext"){
        if(!arg(0))return finish(ffx_parameter);const auto handle=load(arg(0),64);const auto found=fx.contexts.find(handle);
        if(found==fx.contexts.end())return finish(ffx_parameter);
        fx.contexts.erase(found);(void)free_heap(memory,handle,state);store(arg(0),64,0);
        game_upscaler_contexts().fetch_sub(1,std::memory_order_acq_rel);
        return finish(ffx_ok);
    }
    if(!arg(1))return finish(ffx_parameter);
    const auto desc=arg(1);traced_type=load(desc,64);
    if(symbol=="ffxConfigure"){
        if(traced_type==desc_global_debug)return finish(ffx_ok);
        if(!context_of(arg(0)))return finish(ffx_parameter);
        // 速度、反應與累積等調整鍵值屬 FSR 自身的演算法參數；MetalFX 沒有對應項，接受後不改變結果。
        return finish(traced_type==desc_keyvalue?ffx_ok:ffx_unknown_desctype);
    }
    if(symbol=="ffxQuery"){
        if(traced_type==desc_get_versions){
            // ffxQueryDescGetVersions：createDescType(16)、device(24)、outputCount*(32)、versionIds*(40)、versionNames*(48)。
            const auto count=load(desc+32,64);if(!count)return finish(ffx_parameter);
            const bool supported=load(desc+16,64)==desc_create_upscale;const auto capacity=load(count,64);
            if(supported && capacity){
                if(const auto ids=load(desc+40,64))store(ids,64,provider_version);
                if(const auto names=load(desc+48,64)){
                    if(!fx.version_name){fx.version_name=allocate_heap(memory,sizeof(provider_name));if(!fx.version_name)throw std::bad_alloc();
                        memory.write(GuestAddress{fx.version_name},{reinterpret_cast<const std::uint8_t*>(provider_name),sizeof(provider_name)});}
                    store(names,64,fx.version_name);
                }
            }
            store(count,64,supported?1:0);return finish(ffx_ok);
        }
        if(traced_type==desc_ratio){
            // qualityMode(16)、pOutUpscaleRatio(24)。
            const auto ratio=quality_ratio(static_cast<std::uint32_t>(load(desc+16,32)));if(ratio<=0)return finish(ffx_parameter);
            if(const auto out=load(desc+24,64))store_float(out,ratio);return finish(ffx_ok);
        }
        if(traced_type==desc_render_resolution){
            // displayWidth(16)、displayHeight(20)、qualityMode(24)、pOutRenderWidth(32)、pOutRenderHeight(40)。
            const auto width=static_cast<std::uint32_t>(load(desc+16,32)),height=static_cast<std::uint32_t>(load(desc+20,32));
            const auto ratio=quality_ratio(static_cast<std::uint32_t>(load(desc+24,32)));if(ratio<=0 || !width || !height)return finish(ffx_parameter);
            if(const auto out=load(desc+32,64))store(out,32,std::max<std::uint32_t>(1,static_cast<std::uint32_t>(static_cast<float>(width)/ratio)));
            if(const auto out=load(desc+40,64))store(out,32,std::max<std::uint32_t>(1,static_cast<std::uint32_t>(static_cast<float>(height)/ratio)));
            return finish(ffx_ok);
        }
        if(traced_type==desc_jitter_count){
            // renderWidth(16)、displayWidth(20)、pOutPhaseCount(24)。
            const auto render=static_cast<std::uint32_t>(load(desc+16,32)),display=static_cast<std::uint32_t>(load(desc+20,32));if(!render)return finish(ffx_parameter);
            const auto ratio=static_cast<float>(display)/static_cast<float>(render);
            if(const auto out=load(desc+24,64))store(out,32,static_cast<std::uint32_t>(static_cast<std::int32_t>(8.0F*ratio*ratio)));
            return finish(ffx_ok);
        }
        if(traced_type==desc_jitter_offset){
            // index(16)、phaseCount(20)、pOutX(24)、pOutY(32)：Halton 值置中到 [-0.5, 0.5)。
            const auto index=static_cast<std::int32_t>(load(desc+16,32)),phases=static_cast<std::int32_t>(load(desc+20,32));if(phases<=0)return finish(ffx_parameter);
            const auto step=((index%phases)+phases)%phases+1;
            if(const auto out=load(desc+24,64))store_float(out,halton(step,2)-0.5F);
            if(const auto out=load(desc+32,64))store_float(out,halton(step,3)-0.5F);
            return finish(ffx_ok);
        }
        if(traced_type==desc_provider_version){
            if(!context_of(arg(0)))return finish(ffx_parameter);
            if(!fx.version_name){fx.version_name=allocate_heap(memory,sizeof(provider_name));if(!fx.version_name)throw std::bad_alloc();
                memory.write(GuestAddress{fx.version_name},{reinterpret_cast<const std::uint8_t*>(provider_name),sizeof(provider_name)});}
            store(desc+16,64,provider_version);store(desc+24,64,fx.version_name);return finish(ffx_ok);
        }
        if(traced_type==desc_memory_usage){
            // MetalFX 的暫存資源由後端管理，不占遊戲的 D3D12 配額。
            if(!context_of(arg(0)))return finish(ffx_parameter);
            if(const auto out=load(desc+16,64)){store(out,64,0);store(out+8,64,0);}return finish(ffx_ok);
        }
        return finish(ffx_unknown_desctype);
    }
    if(symbol=="ffxDispatch"){
        auto* context=context_of(arg(0));if(!context)return finish(ffx_parameter);
        // 反應遮罩產生是 FSR 自身的前處理；MetalFX 不使用反應遮罩，回成功且不寫輸出。
        if(traced_type==desc_reactive_mask)return finish(ffx_ok);
        if(traced_type!=desc_dispatch_upscale)return finish(ffx_unknown_desctype);
        // ffxDispatchDescUpscale：commandList(16)、七個 FfxApiResource（各 48 bytes，resource 指標在開頭）：color(24)、depth(72)、
        // motionVectors(120)、exposure(168)、reactive(216)、transparencyAndComposition(264)、output(312)；jitterOffset(360)、
        // motionVectorScale(368)、renderSize(376)、upscaleSize(384)、enableSharpening(392)、sharpness(396)、frameTimeDelta(400)、
        // preExposure(404)、reset(408)、cameraNear(412)…flags(428)。
        const auto list=load(desc+16,64),color=load(desc+24,64),depth=load(desc+72,64),motion=load(desc+120,64),exposure=load(desc+168,64),output=load(desc+312,64);
        if(!list || !color || !depth || !motion || !output)return finish(ffx_parameter);
        GraphicsTemporalUpscale u;
        u.input_width=static_cast<std::uint32_t>(load(desc+376,32));u.input_height=static_cast<std::uint32_t>(load(desc+380,32));
        u.output_width=static_cast<std::uint32_t>(load(desc+384,32));u.output_height=static_cast<std::uint32_t>(load(desc+388,32));
        if(!u.output_width || !u.output_height){u.output_width=context->max_upscale_width;u.output_height=context->max_upscale_height;}
        if(!u.input_width || !u.input_height || u.input_width>u.output_width || u.input_height>u.output_height)return finish(ffx_parameter);
        // 抖動與動態向量都是繪製解析度像素：抖動 x 向右、y 向下；動態向量乘上 motionVectorScale 後為像素位移。
        u.jitter_x=load_float(desc+360);u.jitter_y=load_float(desc+364);
        u.motion_scale_x=load_float(desc+368);u.motion_scale_y=load_float(desc+372);
        if(!std::isfinite(u.jitter_x) || !std::isfinite(u.jitter_y) || !std::isfinite(u.motion_scale_x) || !std::isfinite(u.motion_scale_y))return finish(ffx_parameter);
        const float pre_exposure=load_float(desc+404);u.pre_exposure=pre_exposure>0 && std::isfinite(pre_exposure)?pre_exposure:1.0F;
        u.reset=load(desc+408,8)!=0;u.depth_reversed=(context->flags&flag_depth_inverted)!=0;u.auto_exposure=(context->flags&flag_auto_exposure)!=0;u.history=context->history;
        const bool use_exposure=exposure && !u.auto_exposure;
        const bool recorded=d3d12_record(memory,list,[u,color,motion,depth,exposure,output,use_exposure](NativeGraphics& native,const std::function<std::shared_ptr<GraphicsResource>(std::uint64_t)>& resource_of){
            auto upscale=u;upscale.color=GraphicsView{resource_of(color)};upscale.motion=GraphicsView{resource_of(motion)};upscale.depth=GraphicsView{resource_of(depth)};
            upscale.output=GraphicsView{resource_of(output)};if(use_exposure)upscale.exposure=GraphicsView{resource_of(exposure)};
            try{native.graphics_temporal_upscale(upscale);}
            catch(const std::exception& error){static std::atomic<unsigned> reported{0};if(reported.fetch_add(1)<4)std::fprintf(stderr,"[fidelityfx] MetalFX 升頻失敗：%s\n",error.what());}
        });
        return finish(recorded?ffx_ok:ffx_parameter);
    }
    return finish(ffx_error);
}
}
