#include "metal_depth_stencil.inc"
#include "metal_layered_rendering.inc"
#include "metal_unordered_clear.inc"
#include "metal_buffer_views.inc"
#include "metal_texture1d.inc"
bool MetalGl::graphics_vertex_format_supported(std::uint32_t format){
    try{(void)metal_vertex_format(format);return true;}catch(const std::invalid_argument&){return false;}
}
bool MetalGl::graphics_samples_supported(std::uint32_t format,std::uint32_t samples){@autoreleasepool{
    try{(void)metal_format(format);}catch(const std::invalid_argument&){return false;}
    if(graphics_format_bc_family(format))return samples==1 && device_.supportsBCTextureCompression;
    if(samples==1)return true;
    // D3D11 的固定樣本模式包含標準與中心兩種；能力查詢必須涵蓋兩者。
    if(!device_.areProgrammableSamplePositionsSupported)return false;
    if(format==67 || (format>=5 && format<=8))return false;
    if(samples!=2 && samples!=4 && samples!=8)return false;
    // 整數格式沒有本引擎的平均 resolve；不宣告尚未提供的 MSAA 能力。
    switch(format){case 3:case 4:case 12:case 14:case 17:case 18:case 25:case 30:case 32:case 36:case 38:case 42:case 43:case 50:case 52:case 57:case 59:case 62:case 64:return false;default:break;}
    if(format==1 || format==2 || format==15 || format==16 || format==39 || format==41){if(@available(macOS 11.0,*)){if(!device_.supports32BitMSAA)return false;}else return false;}
    return [device_ supportsTextureSampleCount:samples];
}}
std::shared_ptr<GraphicsResource> MetalGl::graphics_resource(const GraphicsResourceDesc& d){@autoreleasepool{
    auto r=std::make_shared<MetalResource>();r->desc=d;
    if(!d.width || !d.levels || !d.layers || !d.depth || !d.samples || (d.dimension!=1 && d.width>16384) || d.height>16384 || d.depth>2048 || d.layers>2048)throw std::invalid_argument("資源尺寸不合法");
    if(d.dimension!=1 && (!graphics_samples_supported(d.format,d.samples) || (d.samples>1 && (d.dimension!=3 || d.levels!=1 || (d.bind&0x80)))))throw std::invalid_argument("Metal 資源格式／取樣數不支援");
    if(d.sample_quality && d.sample_quality!=UINT32_MAX && d.sample_quality!=UINT32_MAX-1U)throw std::invalid_argument("Metal MSAA 品質模式不支援");
    if(d.dimension!=1 && d.format>=5 && d.format<=8 && (d.bind&0x60))throw std::invalid_argument("RGB32 紋理不支援渲染／深度目標綁定");
    std::uint64_t size=0;
    if(d.dimension==1)size=(std::uint64_t{d.width}+4095)&~std::uint64_t{4095};
    else for(unsigned i=0;i<d.levels;++i){const auto pitch=graphics_format_pitch(d.format>=5 && d.format<=8?2:d.format,std::max(1U,d.width>>i),std::max(1U,d.height>>i));size+=std::uint64_t{pitch[0]}*pitch[1]*std::max(1U,d.depth>>i)*d.layers*d.samples*((d.format>=44 && d.format<=47)?2U:1U);}
    if(size>SIZE_MAX)throw std::bad_alloc{};r->lease=reserve_gpu_memory(static_cast<std::size_t>(size));
    // 實體配置向頁邊界補齊，供不同 DXGI 格式建立對齊的 texture-buffer view；邏輯長度仍為 d.width。
    if(d.dimension==1){r->buffer=[device_ newBufferWithLength:size options:MTLResourceStorageModeShared];if(!r->buffer)throw std::bad_alloc{};std::memset(r->buffer.contents,0,size);
        if(metal_backing_generation_enabled()){r->backing=std::make_shared<MetalBufferBacking>();r->backing->buffer=r->buffer;r->backing->lease=r->lease;}}
    else {
        auto t=[MTLTextureDescriptor new];t.pixelFormat=metal_format(d.format);if(d.format==22 || d.format==47)t.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;if(d.bind&0x40){if(d.format==39)t.pixelFormat=MTLPixelFormatDepth32Float;if(d.format==53)t.pixelFormat=MTLPixelFormatDepth16Unorm;}t.width=d.width;t.height=d.height;t.depth=d.depth;t.mipmapLevelCount=d.levels;t.arrayLength=d.dimension==4?1:d.layers;t.sampleCount=d.samples;
        t.textureType=d.dimension==4?MTLTextureType3D:d.samples>1?(d.layers>1?MTLTextureType2DMultisampleArray:MTLTextureType2DMultisample):(d.layers>1?MTLTextureType2DArray:MTLTextureType2D);
        t.usage=MTLTextureUsagePixelFormatView;if(d.bind&8)t.usage|=MTLTextureUsageShaderRead;if(d.bind&0x80)t.usage|=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;if(d.bind&0x60)t.usage|=MTLTextureUsageRenderTarget;
        if((d.bind&0x80) && (t.pixelFormat==MTLPixelFormatR32Uint || t.pixelFormat==MTLPixelFormatR32Sint)){
            if(@available(macOS 14.0,*))t.usage|=MTLTextureUsageShaderAtomic;
        }
        if(metal_depth_format(t.pixelFormat) && d.dimension!=3)throw std::invalid_argument("深度／模板資源需要二維紋理");
        t.storageMode=metal_depth_format(t.pixelFormat) || d.samples>1?MTLStorageModePrivate:MTLStorageModeShared;
        r->texture=[device_ newTextureWithDescriptor:t];if(!r->texture)throw std::bad_alloc{};
    }
    return r;
}}
std::shared_ptr<GraphicsShader> MetalGl::graphics_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes){
    // 串流輸出簽章容器（stage 6）只保存位元組，不經快取。
    if(stage==6)return graphics_shader_create(stage,bytes);
    std::uint64_t key=UINT64_C(1469598103934665603)^stage;for(const auto value:bytes){key^=value;key*=UINT64_C(1099511628211);}
    {
        const std::lock_guard lock(shader_cache_mutex_);
        if(const auto found=shader_cache_.find(key);found!=shader_cache_.end())for(const auto& cached:found->second)
            if(cached->stage==stage && cached->bytecode.size()==bytes.size() && std::equal(cached->bytecode.begin(),cached->bytecode.end(),bytes.begin()))return cached;
    }
    auto created=graphics_shader_create(stage,bytes);
    if(auto native=std::dynamic_pointer_cast<MetalShader>(created)){
        native->content_key=key;
        // 診斷：X64ARM_METAL_DUMP_KEYS=<雜湊,…> 與 X64ARM_METAL_DUMP_DIR=<目錄>：把命中的著色器原始位元組與翻譯後的 MSL 寫出
        // （雜湊同 X64ARM_METAL_LABELS 的標籤）。
        static const char* dump_keys=std::getenv("X64ARM_METAL_DUMP_KEYS");static const char* dump_dir=std::getenv("X64ARM_METAL_DUMP_DIR");
        if(dump_keys && dump_dir){
            char hex[17];std::snprintf(hex,sizeof hex,"%016llx",static_cast<unsigned long long>(key));
            if(std::string_view(dump_keys).find(hex)!=std::string_view::npos){
                const auto base=std::string(dump_dir)+"/"+hex;
                if(auto* out=std::fopen((base+".bin").c_str(),"wb")){std::fwrite(bytes.data(),1,bytes.size(),out);std::fclose(out);}
                if(auto* out=std::fopen((base+".metal").c_str(),"wb")){std::fwrite(native->source.data(),1,native->source.size(),out);std::fclose(out);}
            }
        }
    }
    if(auto native=std::dynamic_pointer_cast<MetalShader>(created);native && native->bytecode.size()==bytes.size()){
        const std::lock_guard lock(shader_cache_mutex_);
        shader_cache_[key].push_back(std::move(native));shader_cache_order_.push_back(key);
        // 上限 2048 個範本，先進先出淘汰（被淘汰者只是下次重新編譯）。
        while(shader_cache_order_.size()>2048){const auto old=shader_cache_order_.front();shader_cache_order_.pop_front();
            if(const auto found=shader_cache_.find(old);found!=shader_cache_.end()){found->second.erase(found->second.begin());if(found->second.empty())shader_cache_.erase(found);}}
    }
    return created;
}
// 私有複本：共用快取中的編譯結果，但給新 serial 並清掉串流輸出狀態，供呼叫端之後自行設定。
std::shared_ptr<GraphicsShader> MetalGl::graphics_shader_private(std::uint32_t stage,std::span<const std::uint8_t> bytes){
    auto shared=graphics_shader(stage,bytes);
    if(const auto* native=dynamic_cast<const MetalShader*>(shared.get())){
        auto copy=std::make_shared<MetalShader>(*native);copy->serial=++shader_serial_;copy->stream_output.clear();copy->stream_strides={};copy->rasterized_stream={};return copy;
    }
    return shared;
}
std::shared_ptr<GraphicsShader> MetalGl::graphics_shader_create(std::uint32_t stage,std::span<const std::uint8_t> bytes){@autoreleasepool{
    // stage 6 是僅含輸出簽章的 SO 轉送物件；實際資料由目前綁定的 VS 產生。
    if(stage==6){
        if(bytes.size()<32 || metal_word(bytes,0)!=0x43425844 || metal_word(bytes,24)!=bytes.size())throw std::invalid_argument("SO 簽章容器不合法");
        auto shader=std::make_shared<MetalShader>();shader->stage=0;shader->serial=++shader_serial_;shader->bytecode.assign(bytes.begin(),bytes.end());return shader;
    }
    if(bytes.size()>=8 && (metal_word(bytes,0)>>16)==(stage==0?0xfffeU:0xffffU)){
        auto translated=graphics_legacy_shader(stage,bytes);auto shader=std::make_shared<MetalShader>();shader->stage=stage;shader->serial=++shader_serial_;shader->source=std::move(translated.source);shader->bindings=std::move(translated.bindings);shader->inputs=std::move(translated.inputs);shader->bytecode.assign(bytes.begin(),bytes.end());
        if(std::getenv("X64ARM_D3D9_CAPTURE_SURFACES"))if(const auto* folder=std::getenv("X64ARM_D3D9_CAPTURE_DIR");folder && *folder){std::filesystem::create_directories(folder);std::ofstream out(std::filesystem::path(folder)/("legacy-"+std::to_string(shader->serial)+"-stage-"+std::to_string(stage)+".metal"));out<<shader->source;}
        shader->writes_depth=stage==1 && shader->source.find("[[depth(")!=std::string::npos;
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:shader->source.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("D3D9 Metal shader 編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+shader->source);
        shader->function=[library newFunctionWithName:[NSString stringWithUTF8String:translated.entry.c_str()]];if(!shader->function)throw std::invalid_argument("D3D9 Metal shader 沒有入口");return shader;
    }
    if(graphics_modern_shader_required(bytes)){
        // HLSLcc 只處理 SM4／5.0；SM5.1 與 DXIL 改由 SPIR-V 路徑轉成 MSL。
        auto translated=graphics_modern_shader(stage,bytes);auto shader=std::make_shared<MetalShader>();
        shader->stage=stage;shader->serial=++shader_serial_;shader->bytecode.assign(bytes.begin(),bytes.end());shader->source=std::move(translated.source);
        shader->bindings=std::move(translated.bindings);shader->inputs=std::move(translated.inputs);shader->threads=translated.threads;
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:shader->source.c_str()] options:options error:&error];
        if(!library)throw std::invalid_argument("Metal 著色器編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+shader->source);
        shader->function=[library newFunctionWithName:[NSString stringWithUTF8String:translated.entry.c_str()]];if(!shader->function)throw std::invalid_argument("Metal 著色器沒有入口");
        if(stage==5){shader->compute=[device_ newComputePipelineStateWithFunction:shader->function error:&error];if(!shader->compute)throw std::invalid_argument("Metal compute pipeline 建立失敗："+std::string(error.localizedDescription.UTF8String));}
        return shader;
    }
    return graphics_translate_shader(stage,bytes,nullptr);
}}
std::shared_ptr<GraphicsShader> MetalGl::graphics_compute_source(const GraphicsModernShader& source){@autoreleasepool{
    // 與 SM5.1／DXIL 路徑相同的 compute pipeline 建立；fast math 開啟（相容層核心自行處理精度需求）。
    auto shader=std::make_shared<MetalShader>();shader->stage=5;shader->serial=++shader_serial_;shader->source=source.source;
    shader->bindings=source.bindings;shader->threads=source.threads;
    NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=YES;
    auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:shader->source.c_str()] options:options error:&error];
    if(!library)throw std::invalid_argument("Metal 核心編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+shader->source);
    shader->function=[library newFunctionWithName:[NSString stringWithUTF8String:source.entry.c_str()]];if(!shader->function)throw std::invalid_argument("Metal 核心沒有入口");
    shader->compute=[device_ newComputePipelineStateWithFunction:shader->function error:&error];
    if(!shader->compute)throw std::invalid_argument("Metal compute pipeline 建立失敗："+std::string(error.localizedDescription.UTF8String));
    return shader;
}}
std::shared_ptr<GraphicsShader> MetalGl::graphics_translate_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes,GLSLCrossDependencyData* shared){@autoreleasepool{
    if(stage>5)throw std::invalid_argument("Metal 著色器階段不合法");
    // 先檢查容器與 token 邊界，避免把截斷的客體資料交給第三方解碼器。
    if(bytes.size()<32 || metal_word(bytes,0)!=0x43425844 || metal_word(bytes,24)!=bytes.size() || metal_word(bytes,28)>64 || 32ULL+4ULL*metal_word(bytes,28)>bytes.size())throw std::invalid_argument("DXBC 容器不合法");
    bool code=false;
    for(unsigned i=0;i<metal_word(bytes,28);++i){const auto off=metal_word(bytes,32+4*i);if(off>bytes.size()-8)throw std::invalid_argument("DXBC 區塊越界");const auto n=metal_word(bytes,off+4);if(std::uint64_t{off}+8+n>bytes.size())throw std::invalid_argument("DXBC 區塊長度不合法");const auto tag=metal_word(bytes,off);
        if(tag==0x52444853 || tag==0x58454853){if(n<8 || n%4 || metal_word(bytes,off+12)!=n/4)throw std::invalid_argument("DXBC token 長度不合法");const auto version=metal_word(bytes,off+8);const unsigned expected=stage==0?1:stage==1?0:stage==2?2:stage==3?3:stage==4?4:5;if((version>>16)!=expected || ((version>>4)&15)<4 || ((version>>4)&15)>5)throw std::invalid_argument("DXBC 著色器階段不符");for(std::uint32_t at=2;at<n/4;){const auto token=metal_word(bytes,off+8+4*at);const auto len=(token&0x7ff)==53?metal_word(bytes,off+12+4*at):(token>>24)&0x7f;if(!len || len>n/4-at)throw std::invalid_argument("DXBC 指令截斷");at+=len;}code=true;}
    }
    if(!code)throw std::invalid_argument("DXBC 沒有 shader model 4／5 token");
    if(const char* directory=std::getenv("X64ARM_DUMP_DXBC");directory && *directory){
        // 只在明確開啟診斷時保存原始 bytecode，供離線重現轉譯問題。
        static std::atomic<std::uint64_t> sequence{};
        const auto path=std::filesystem::path(directory)/(std::to_string(getpid())+"-"+std::to_string(++sequence)+"-stage"+std::to_string(stage)+".dxbc");
        std::filesystem::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!output)throw std::runtime_error("無法保存 DXBC 診斷資料");
    }
    if((stage==3 || stage==4) && !shared){auto shader=std::make_shared<MetalShader>();shader->stage=stage;shader->serial=++shader_serial_;shader->bytecode.assign(bytes.begin(),bytes.end());return shader;}
    struct Reflection final:HLSLccReflection {
        std::map<std::string,std::uint32_t> constants,buffers,textures,inputs;
        std::array<std::uint32_t,3> threads{1,1,1};std::string error;
        std::array<unsigned,4> tessellation{};
        void OnTessellationInfo(unsigned partition,unsigned winding,unsigned factor,unsigned patches)override{tessellation={partition,winding,factor,patches};}
        void OnDiagnostics(const std::string& message,int,bool bad)override{if(bad)error+=message;}
        void OnInputBinding(const std::string& name,int bind)override{
            // Metal 的宣告使用 input.SEMANTIC；DXBC signature 只保存 SEMANTIC。
            // 使用轉譯器分配的位置，不能由原始暫存器編號推測活躍 attribute。
            const auto key=name.starts_with("input.")?name.substr(6):name;
            inputs[key]=static_cast<std::uint32_t>(bind);
        }
        void OnConstantBufferBinding(const std::string& name,int bind)override{constants[name]=static_cast<std::uint32_t>(bind);}
        void OnBufferBinding(const std::string& name,int bind,bool)override{buffers[name]=static_cast<std::uint32_t>(bind);}
        void OnTextureBinding(const std::string& name,int bind,int,bool,HLSLCC_TEX_DIMENSION,bool)override{textures[name]=static_cast<std::uint32_t>(bind);}
        void OnThreadGroupSize(unsigned x,unsigned y,unsigned z)override{threads={x,y,z};}
    } reflection;
    GLSLShader translated;GLSLCrossDependencyData deps;HLSLccSamplerPrecisionInfo precision;
    std::vector<std::uint32_t> aligned((bytes.size()+3)/4);std::memcpy(aligned.data(),bytes.data(),bytes.size());
    if(!TranslateHLSLFromMem(reinterpret_cast<const char*>(aligned.data()),HLSLCC_FLAG_INOUT_SEMANTIC_NAMES|HLSLCC_FLAG_UNIFORM_BUFFER_OBJECT|HLSLCC_FLAG_TRANSLATE_MATRICES|(shared?HLSLCC_FLAG_METAL_TESSELLATION:0),LANG_METAL,nullptr,shared?shared:&deps,precision,reflection,&translated))throw std::invalid_argument("DXBC 至 Metal 轉譯失敗："+reflection.error);
    if(!reflection.error.empty())throw std::invalid_argument("DXBC 至 Metal 語義不支援："+reflection.error);
    auto shader=std::make_shared<MetalShader>();shader->stage=stage;shader->serial=++shader_serial_;shader->threads=reflection.threads;shader->source=translated.sourceCode;
    shader->bytecode.assign(bytes.begin(),bytes.end());shader->tessellation=reflection.tessellation;
    if(shared){if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){std::ofstream out(std::filesystem::path(folder)/("tess-stage-"+std::to_string(stage)+".metal"));out<<shader->source;}}
    shader->writes_depth=stage==1 && metal_writes_depth(shader->source);
    metal_shader_varyings(*shader,translated.reflection);if(stage==2)metal_geometry_metadata(*shader,bytes);
    const auto matching=[&](const std::map<std::string,std::uint32_t>& names,const std::string& name)->std::uint32_t{auto it=names.find(name);if(it!=names.end())return it->second;for(const auto& [key,slot]:names)if(key=="_"+name || key.ends_with(name))return slot;throw std::invalid_argument("Metal 反射找不到資源："+name);};
    // DXBC 資源陣列的名稱可含 [n]，Metal 宣告會將括號改成底線。
    // 每個實際宣告獨立查表，不能假定 HLSL 暫存器與 Metal binding 連續。
    const auto samplers=metal_sampler_bindings(shader->source);
    const auto has_name=[&](const auto& map,const std::string& name){return map.contains(name) || map.contains("_"+name) || (shared && std::any_of(map.begin(),map.end(),[&](const auto& item){return item.first.ends_with("_"+name);}));};
    for(const auto& binding:translated.reflection.psResourceBindings){
        const auto type=static_cast<unsigned>(binding.eType);
        std::string base=binding.name;for(auto& character:base)if(character=='[' || character==']')character='_';
        for(unsigned j=0;j<binding.ui32BindCount;++j){std::uint32_t native=0,kind=0;const auto name=base+(j?std::to_string(j):std::string{});
            if(type==0){auto constant=name;if(!constant.empty() && constant[0]=='$')constant=(shared?"":stage==0?"V":stage==1?"F":stage==2?"G":"")+constant.substr(1);native=matching(reflection.constants,constant);kind=0;}
            else if(type==3){if(!has_name(samplers,name) && !has_name(samplers,"x64arm_sampler_"+name))continue;native=matching(samplers,name);kind=2;}
            else if(has_name(reflection.textures,name)){native=matching(reflection.textures,name);kind=type==4?5:1;}
            else if(has_name(reflection.buffers,name)){native=matching(reflection.buffers,name);kind=ShaderInfo::ResourceTypeToResourceGroup(binding.eType)==RGROUP_UAV?4:3;}
            else continue;
            shader->bindings.push_back({kind,binding.ui32BindPoint+j,native&0xffff,(native>>16)?(native>>16)-1:UINT32_MAX});
        }
    }
    metal_texture1d_shader(*shader);
    if(!shared)metal_buffer_shader(*shader);
    for(const auto& input:translated.reflection.psInputSignatures){
        if(input.eSystemValueType!=NAME_UNDEFINED)continue;
        const auto name=input.semanticName+std::to_string(input.ui32SemanticIndex);std::optional<std::uint32_t> slot;
        if(auto it=reflection.inputs.find(name);it!=reflection.inputs.end())slot=it->second;
        else if(auto prefixed=reflection.inputs.find("_"+name);prefixed!=reflection.inputs.end())slot=prefixed->second;
        else if(shared)for(const auto& [key,v]:reflection.inputs)if(key.ends_with("_"+name)){slot=v;break;}
        // signature 可保留已最佳化移除的輸入；未產生 Metal 宣告就不建立 fetch。
        if(slot)shader->inputs.push_back({input.semanticName,input.ui32SemanticIndex,*slot});
    }
    if(shared)return shader;
    // 診斷：X64ARM_DUMP_MSL=<目錄> 保存轉譯後的 MSL（檔名含階段與序號，與繪製追蹤的 vs／ps／cs 編號相同）。
    if(const char* folder=std::getenv("X64ARM_DUMP_MSL");folder && *folder){std::ofstream out(std::filesystem::path(folder)/("stage"+std::to_string(stage)+"-"+std::to_string(shader->serial)+".metal"));out<<shader->source;}
    if(stage==2){shader->geometry_original=shader->source;shader->source=metal_geometry_clean(shader->source);}
    // VS 接到 GS 時可以只輸出自訂 varying，裁切位置由 GS 產生。
    // 仍編譯驗證 VS 主體，真正執行沿用幾何管線的 rasterization-disabled capture。
    const bool intermediate=stage==0 && std::none_of(shader->output_varyings.begin(),shader->output_varyings.end(),[](const auto& v){return v.system==NAME_POSITION;});
    // 此路徑先由原始 EXE A/B 驗證；開關不影響 GPU 執行位置或浮點模式。
    if(stage==5)if(const auto* compact=std::getenv("X64ARM_METAL_COMPUTE_LOOP_COMPACTION");compact && std::string_view(compact)=="1"){
        unsigned block=1;if(const auto* value=std::getenv("X64ARM_METAL_COMPUTE_LOOP_BLOCK")){const auto parsed=std::string_view(value);if(parsed=="2")block=2;else if(parsed=="4")block=4;else if(parsed=="8")block=8;else if(parsed=="16")block=16;}
        auto folded=metal_compact_compute_loops(shader->source,block);
        if(folded!=shader->source)std::fprintf(stderr,"[metal-compute-loop] source_bytes=%zu compacted_bytes=%zu\n",shader->source.size(),folded.size());
        shader->source=std::move(folded);
    }
    auto compilation=stage==2?shader->source+"\nkernel void x64GeometryValidate() {}\n":shader->source;
    if(intermediate){compilation=metal_strip_attributes(compilation);metal_replace(compilation,"vertex Mtl_VertexOut xlatMtlMain(","Mtl_VertexOut xlatMtlMain(");compilation+="\nkernel void x64VertexValidate() {}\n";}
    NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
    // 一般 VS／PS／CS 改為非同步編譯：讀檔時 UE4 連續建立大量著色器，原本每個都在建立呼叫內等 Metal 編譯器
    // （讀檔時段主執行緒約 15%）。多個編譯同時在 Metal 編譯服務進行，第一次繪製／運算才等待結果。
    // 僅供驗證的 GS 與 VS→GS 中介編譯維持同步（沒有後續使用點可回報錯誤）。X64ARM_METAL_ASYNC_SHADER=0 關閉。
    static const bool async_shader=[] {const auto* value=std::getenv("X64ARM_METAL_ASYNC_SHADER");return !value || std::string_view(value)!="0";}();
    if(async_shader && stage!=2 && !intermediate){
        auto pending=std::make_shared<MetalPendingLibrary>();pending->entry=stage==5?@"computeMain":@"xlatMtlMain";shader->pending=pending;
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:compilation.c_str()] options:options completionHandler:^(id<MTLLibrary> library,NSError* failure){
            std::lock_guard lock(pending->mutex);pending->library=library;
            if(!library)pending->error=failure?std::string(failure.localizedDescription.UTF8String):std::string("未知錯誤");
            pending->done=true;pending->ready.notify_all();
        }];
        return shader;
    }
    auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:compilation.c_str()] options:options error:&error];
    if(!library)throw std::invalid_argument("Metal 著色器編譯失敗："+std::string(error.localizedDescription.UTF8String)+"\n"+compilation);
    if(stage==2 || intermediate)return shader;
    shader->function=[library newFunctionWithName:stage==5?@"computeMain":@"xlatMtlMain"];if(!shader->function)throw std::invalid_argument("Metal 著色器沒有入口");
    if(stage==5){shader->compute=[device_ newComputePipelineStateWithFunction:shader->function error:&error];if(!shader->compute)throw std::invalid_argument("Metal compute pipeline 建立失敗："+std::string(error.localizedDescription.UTF8String));}
    return shader;
}}
std::shared_ptr<GraphicsSampler> MetalGl::graphics_sampler(std::span<const std::uint8_t> bytes){@autoreleasepool{
    if(bytes.size()!=52)throw std::invalid_argument("取樣器結構長度不合法");auto d=[MTLSamplerDescriptor new];const auto filter=metal_word(bytes,0);d.minFilter=(filter&0x10)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;d.magFilter=(filter&4)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;d.mipFilter=(filter&1)?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
    d.sAddressMode=metal_address(metal_word(bytes,4));d.tAddressMode=metal_address(metal_word(bytes,8));d.rAddressMode=metal_address(metal_word(bytes,12));d.maxAnisotropy=(filter&0x40)?std::clamp(metal_word(bytes,20),1U,16U):1;
    if(filter&0x80)d.compareFunction=static_cast<MTLCompareFunction>(metal_word(bytes,24)-1);d.lodMinClamp=metal_float(bytes,44);d.lodMaxClamp=metal_float(bytes,48);
    if(metal_float(bytes,28)==1 && metal_float(bytes,32)==1 && metal_float(bytes,36)==1 && metal_float(bytes,40)==1)d.borderColor=MTLSamplerBorderColorOpaqueWhite;else d.borderColor=metal_float(bytes,40)==0?MTLSamplerBorderColorTransparentBlack:MTLSamplerBorderColorOpaqueBlack;
    return cached_sampler(d);
}}
// Metal 沒有 RGB32 紋理；實體配置 RGBA32，客體仍保留緊密 RGB32 與原始位元。
std::vector<std::uint8_t> metal_rgb32_expand(std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image,std::uint32_t w,std::uint32_t h,std::uint32_t z){
    std::vector<std::uint8_t> expanded(std::size_t{w}*h*z*16);
    for(std::uint32_t iz=0;iz<z;++iz)for(std::uint32_t iy=0;iy<h;++iy)for(std::uint32_t ix=0;ix<w;++ix)
        std::memcpy(expanded.data()+((std::size_t{iz}*h+iy)*w+ix)*16,data.data()+std::size_t{iz}*image+std::size_t{iy}*row+ix*12,12);
    return expanded;
}
bool MetalGl::graphics_upload_buffer(const std::shared_ptr<GraphicsResource>& destination,std::span<const std::uint8_t> data,std::uint32_t offset){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_METAL_ASYNC_BUFFER_UPLOAD");return !value || std::string_view(value)=="1";}();
    if(!enabled || data.empty() || offset%4 || data.size()%4)return false;
    reap();
    if(!metal_batch_uses(graphics_pending_,destination) && std::none_of(flights_.begin(),flights_.end(),[&](const auto& flight){return metal_batch_uses(flight.objects,destination);}))return false;
    GraphicsResourceDesc descriptor;descriptor.width=static_cast<std::uint32_t>(data.size());
    std::shared_ptr<GraphicsResource> staging;
    try{staging=graphics_resource(descriptor);}catch(const std::bad_alloc&){return false;}
    auto& source=metal_resource(staging);auto& target=metal_resource(destination);
    std::memcpy(source.buffer.contents,data.data(),data.size());
    // CPU 來源先完成快照；在同一 queue 中排程複製，保留前次讀取及後續繪製的順序。
    // staging 和目的資源都保留至命令完成，局部更新不改寫範圍外的資料。
    graphics_pending_.push_back(staging);graphics_pending_.push_back(destination);begin_commands();
    auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope scope{encoder};
    [encoder copyFromBuffer:source.buffer sourceOffset:0 toBuffer:target.buffer destinationOffset:offset size:data.size()];
    scope.end();if(++pending_>=128)flush(false,"batch_128");return true;
}
void MetalGl::graphics_write_buffer(const std::shared_ptr<GraphicsResource>& resource,std::span<const std::uint8_t> data,std::uint32_t offset){
    auto& target=metal_resource(resource);
    if(target.constant_bytes_match(data,offset)){++constant_upload_skips_;constant_upload_saved_bytes_+=data.size();return;}
    static const bool rename=[] {const auto* value=std::getenv("X64ARM_METAL_CONSTANT_RENAME");return !value || std::string_view(value)=="1";}();
    if(rename && target.desc.bind==4 && !offset && data.size()==target.desc.width){
        reap();
        const bool used=metal_batch_uses(graphics_pending_,resource) || std::any_of(flights_.begin(),flights_.end(),[&](const auto& flight){return metal_batch_uses(flight.objects,resource);});
        if(used && graphics_replace_buffer(resource,data))return;
    }
    if(!graphics_upload_buffer(resource,data,offset)){
        wait_resource(resource);std::memcpy(static_cast<std::uint8_t*>(target.buffer.contents)+offset,data.data(),data.size());
    }
    target.remember_constant_bytes(data,offset);
}
bool MetalGl::graphics_replace_buffer(const std::shared_ptr<GraphicsResource>& destination,std::span<const std::uint8_t> data){
    auto& resource=metal_resource(destination);
    // 沒有待提交或在飛命令使用此 backing 時，DISCARD 可以直接就地寫入：舊內容不會再被 GPU 讀到。
    if(!metal_backing_generation_enabled() && metal_discard_pool_enabled()){
        reap();
        const bool in_use=metal_batch_uses(graphics_pending_,destination) || std::any_of(flights_.begin(),flights_.end(),[&](const auto& flight){return metal_batch_uses(flight.objects,destination);});
        if(!in_use){
            std::memcpy(resource.buffer.contents,data.data(),resource.desc.width);
            resource.constant_bytes_valid=false;resource.remember_constant_bytes(data.first(resource.desc.width),0);
            ++discard_in_place_;return true;
        }
    }
    std::shared_ptr<GraphicsResource> replacement;std::shared_ptr<MetalResource> previous;
    try{
        if(!metal_backing_generation_enabled()){
            if(metal_discard_pool_enabled()){
                const auto length=static_cast<std::size_t>(resource.buffer.length);
                if(auto pooled=discard_pool_->take(length)){
                    // 池裡的 buffer 沒有額度：先重新保留，額度不足就讓它釋放、照常配置。
                    std::shared_ptr<GpuMemoryReservation> lease;
                    try{lease=reserve_gpu_memory(length);}catch(const std::bad_alloc&){lease=nullptr;}
                    if(lease){auto pooled_resource=std::make_shared<MetalResource>();pooled_resource->desc=resource.desc;pooled_resource->buffer=pooled;pooled_resource->lease=std::move(lease);replacement=pooled_resource;}
                }
            }
            previous=std::make_shared<MetalResource>();if(metal_discard_pool_enabled())previous->recycle=discard_pool_;
        }
        if(!replacement)replacement=graphics_resource(resource.desc);
    }catch(const std::bad_alloc&){return false;}
    auto& next=metal_resource(replacement);std::memcpy(next.buffer.contents,data.data(),resource.desc.width);
    next.remember_constant_bytes(data.first(resource.desc.width),0);
    // 保持客體 resource／view 身分；已編碼命令使用舊 backing，後續綁定使用新 backing。
    if(previous){
        previous->desc=resource.desc;previous->buffer=resource.buffer;previous->lease=resource.lease;
        if(graphics_pending_.contains(&resource))graphics_pending_.push_back(previous);
        for(auto& flight:flights_)if(flight.objects.contains(&resource))flight.objects.push_back(previous);
    }
    resource.buffer=next.buffer;resource.lease=next.lease;resource.backing=next.backing;
    resource.constant_bytes=std::move(next.constant_bytes);resource.constant_bytes_valid=next.constant_bytes_valid;
    return true;
}
void MetalGl::graphics_write(const GraphicsView& v,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){@autoreleasepool{
    graphics_trace_upload("write",v,data);
    auto& r=metal_resource(v.resource);if(r.buffer){const auto size=v.count?v.count:r.desc.width-v.first;if(std::uint64_t{v.first}+size>r.desc.width || data.size()<size)throw std::invalid_argument("緩衝區寫入越界");graphics_write_buffer(v.resource,data.first(size),v.first);return;}
    wait_resource(v.resource);
    const auto w=std::max(1U,r.desc.width>>v.level),h=std::max(1U,r.desc.height>>v.level),z=std::max(1U,r.desc.depth>>v.level);
    const auto pitch=graphics_format_pitch(r.desc.format,w,h);if(!row)row=pitch[0];if(!image)image=row*pitch[1];if(row<pitch[0] || image<row*pitch[1] || data.size()<std::uint64_t{image}*(z-1)+std::uint64_t{row}*(pitch[1]-1)+pitch[0])throw std::invalid_argument("紋理寫入越界");
    if(metal_depth_format(r.texture.pixelFormat)){std::vector<std::uint8_t> source(data.begin(),data.end());metal_depth_transfer(device_,queue_,v,source,row,{0,0,w,h},true);return;}
    if(r.texture.storageMode==MTLStorageModePrivate)throw std::invalid_argument("MSAA 紋理不能直接 CPU 寫入");
    std::vector<std::uint8_t> expanded;if(r.desc.format>=5 && r.desc.format<=8){expanded=metal_rgb32_expand(data,row,image,w,h,z);data=expanded;row=w*16;image=row*h;}
    [r.texture replaceRegion:MTLRegionMake3D(0,0,0,w,h,z) mipmapLevel:v.level slice:v.slice withBytes:data.data() bytesPerRow:row bytesPerImage:z>1?image:0];
}}
void MetalGl::graphics_write_discard(const GraphicsView& v,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){@autoreleasepool{
    graphics_trace_upload("discard",v,data);
    auto& resource=metal_resource(v.resource);
    if(!resource.buffer || v.first || (v.count && v.count!=resource.desc.width)){graphics_write(v,data,row,image);return;}
    if(data.size()<resource.desc.width)throw std::invalid_argument("Discard 緩衝區來源不足");
    if(resource.constant_bytes_match(data.first(resource.desc.width),0)){++constant_upload_skips_;constant_upload_saved_bytes_+=resource.desc.width;return;}
    if(!graphics_replace_buffer(v.resource,data))graphics_write(v,data,row,image);
}}
void MetalGl::graphics_write_no_overwrite(const GraphicsView& v,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image){@autoreleasepool{
    graphics_trace_upload("no_overwrite",v,data);
    // D3D11 持久映射的 NO_OVERWRITE 上傳：客體保證不改寫 GPU 使用中的範圍，而這些範圍與上次上傳的位元組相同，
    // 所以直接寫入共享記憶體 buffer，不等待 GPU、不另建 staging。非整個 buffer 或私有儲存時沿用一般寫入。
    auto& resource=metal_resource(v.resource);
    if(!resource.buffer || !resource.buffer.contents || v.first || (v.count && v.count!=resource.desc.width) || data.size()<resource.desc.width){graphics_write(v,data,row,image);return;}
    const auto bytes=data.first(resource.desc.width);
    if(resource.constant_bytes_match(bytes,0)){++constant_upload_skips_;constant_upload_saved_bytes_+=resource.desc.width;return;}
    std::memcpy(resource.buffer.contents,bytes.data(),bytes.size());
    resource.remember_constant_bytes(bytes,0);
}}
void MetalGl::graphics_write_region(const GraphicsView& v,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image,std::array<std::uint32_t,6> box){@autoreleasepool{
    if(box[0]>=box[3] || box[1]>=box[4] || box[2]>=box[5])return;
    auto& r=metal_resource(v.resource);const auto w=box[3]-box[0],h=box[4]-box[1],z=box[5]-box[2];
    if(r.buffer){
        if(box[3]>r.desc.width || box[1] || box[2] || box[4]!=1 || box[5]!=1 || data.size()<w)throw std::invalid_argument("緩衝區區域寫入越界");
        graphics_write_buffer(v.resource,data.first(w),box[0]);return;
    }
    if(v.level>=r.desc.levels || v.slice>=r.desc.layers || box[3]>std::max(1U,r.desc.width>>v.level) || box[4]>std::max(1U,r.desc.height>>v.level) || box[5]>std::max(1U,r.desc.depth>>v.level))throw std::invalid_argument("紋理區域寫入越界");
    if(graphics_format_bc_family(r.desc.format) && (box[0]%4 || box[1]%4 || (box[3]%4 && box[3]!=std::max(1U,r.desc.width>>v.level)) || (box[4]%4 && box[4]!=std::max(1U,r.desc.height>>v.level))))throw std::invalid_argument("BC 紋理區域必須對齊壓縮區塊或 mip 邊界");
    const auto pitch=graphics_format_pitch(r.desc.format,w,h);const auto line=pitch[0];if(!row)row=line;
    const auto plane=std::uint64_t{row}*pitch[1];if(!image && z>1){if(plane>UINT32_MAX)throw std::invalid_argument("紋理區域平面過大");image=static_cast<std::uint32_t>(plane);}
    if(row<line || (z>1 && image<plane) || data.size()<std::uint64_t{image}*(z-1)+std::uint64_t{row}*(pitch[1]-1)+line)throw std::invalid_argument("紋理區域來源資料不足");
    wait_resource(v.resource);
    if(metal_depth_format(r.texture.pixelFormat)){if(box[2] || z!=1)throw std::invalid_argument("深度紋理區域不合法");std::vector<std::uint8_t> source(data.begin(),data.end());metal_depth_transfer(device_,queue_,v,source,row,{box[0],box[1],w,h},true);return;}
    if(r.texture.storageMode==MTLStorageModePrivate)throw std::invalid_argument("MSAA 紋理不能直接 CPU 寫入");
    std::vector<std::uint8_t> expanded;if(r.desc.format>=5 && r.desc.format<=8){expanded=metal_rgb32_expand(data,row,image,w,h,z);data=expanded;row=w*16;image=row*h;}
    [r.texture replaceRegion:MTLRegionMake3D(box[0],box[1],box[2],w,h,z) mipmapLevel:v.level slice:v.slice withBytes:data.data() bytesPerRow:row bytesPerImage:z>1?image:0];
}}
// 描述元表需要被間接引用的配置常駐：以 MTLResidencySet（macOS 15+）記錄，表綁定前提交變更。
void MetalGl::metal_make_resident(MetalResource* owner,id<MTLResource> allocation){
    if(!allocation)return;
    if(@available(macOS 15.0,*)){
        if(!residency_set_){
            auto descriptor=[MTLResidencySetDescriptor new];descriptor.label=@"x64arm D3D12 descriptor tables";NSError* error=nil;
            residency_set_=[device_ newResidencySetWithDescriptor:descriptor error:&error];
            if(!residency_set_)throw std::runtime_error("無法建立 Metal residency set");[queue_ addResidencySet:(id<MTLResidencySet>)residency_set_];
        }
        if(owner){if(owner->residency)return;owner->residency=residency_set_;}
        [(id<MTLResidencySet>)residency_set_ addAllocation:(id<MTLAllocation>)allocation];residency_dirty_=true;
    }else throw std::invalid_argument("描述元表需要 macOS 15 以上的 Metal residency set");
}
void MetalGl::metal_commit_residency(){
    if(!residency_dirty_)return;
    if(@available(macOS 15.0,*))[(id<MTLResidencySet>)residency_set_ commit];
    residency_dirty_=false;
}
bool MetalGl::graphics_descriptor(const GraphicsView& v,std::uint32_t type,std::array<std::uint64_t,2>& value,std::shared_ptr<void>& keep){@autoreleasepool{
    value={0,0};keep.reset();const bool buffer_entry=type==8 || type==9;
    if(@available(macOS 13.0,*)){
        if(!v.resource){
            // 空描述元：貼圖指向 1×1 零貼圖；緩衝為長度 0 的空位址。
            if(buffer_entry)return true;
            if(!null_texture_){
                auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
                d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;d.storageMode=MTLStorageModePrivate;null_texture_=[device_ newTextureWithDescriptor:d];
                if(!null_texture_)throw std::bad_alloc{};metal_make_resident(nullptr,null_texture_);
            }
            value[0]=null_texture_.gpuResourceID._impl;return true;
        }
        auto& r=metal_resource(v.resource);
        if(buffer_entry){
            if(!r.buffer)return false;
            const std::uint64_t first=v.first,length=v.count?v.count:(r.buffer.length>first?r.buffer.length-first:0);
            value={r.buffer.gpuAddress+first,length};keep=v.resource;metal_make_resident(&r,r.buffer);return true;
        }
        // raw／structured 緩衝沒有貼圖形式（現代著色器以 storage buffer 存取）：貼圖鏡像改放空描述元。
        if(r.buffer && (r.desc.stride || (v.flags&1)))return false;
        id<MTLTexture> texture=r.buffer?metal_buffer_texture(v,true):metal_shader_texture(v);
        if(!texture)return false;
        value[0]=texture.gpuResourceID._impl;
        // view 可能被資源的 view 快取淘汰；由描述元保留到被覆寫為止。
        keep=std::shared_ptr<void>((__bridge_retained void*)texture,[](void* object){CFRelease(object);});
        metal_make_resident(&r,r.texture?(id<MTLResource>)r.texture:(id<MTLResource>)r.buffer);
        return true;
    }
    return false;
}}
std::vector<std::uint8_t> MetalGl::graphics_read(const GraphicsView& v){@autoreleasepool{
    wait_resource(v.resource);auto& r=metal_resource(v.resource);if(r.buffer){const auto count=v.count?v.count:r.desc.width-v.first;if(std::uint64_t{v.first}+count>r.desc.width)throw std::invalid_argument("緩衝區讀回越界");auto* p=static_cast<std::uint8_t*>(r.buffer.contents)+v.first;return {p,p+count};}
    const auto w=std::max(1U,r.desc.width>>v.level),h=std::max(1U,r.desc.height>>v.level),z=std::max(1U,r.desc.depth>>v.level);const auto pitch=graphics_format_pitch(r.desc.format,w,h);const auto row=pitch[0];std::vector<std::uint8_t> data(static_cast<std::size_t>(row)*pitch[1]*z);
    if(metal_depth_format(r.texture.pixelFormat)){metal_depth_transfer(device_,queue_,v,data,row,{0,0,w,h},false);return data;}
    if(r.texture.storageMode==MTLStorageModePrivate)throw std::invalid_argument("MSAA 讀回需要對應 resolve 格式");if(r.desc.format>=5 && r.desc.format<=8){
        std::vector<std::uint8_t> expanded(std::size_t{w}*h*z*16);[r.texture getBytes:expanded.data() bytesPerRow:w*16 bytesPerImage:z>1?w*h*16:0 fromRegion:MTLRegionMake3D(0,0,0,w,h,z) mipmapLevel:v.level slice:v.slice];
        for(std::size_t i=0;i<std::size_t{w}*h*z;++i)std::memcpy(data.data()+i*12,expanded.data()+i*16,12);return data;
    }[r.texture getBytes:data.data() bytesPerRow:row bytesPerImage:z>1?row*pitch[1]:0 fromRegion:MTLRegionMake3D(0,0,0,w,h,z) mipmapLevel:v.level slice:v.slice];return data;
}}
// Shared 緩衝在完成旗標之後即可由 CPU 直接讀取；不經 wait_resource（它會提交目前批次並等待所有使用同一緩衝的批次）。
// 其他資源沿用 graphics_read。資格判斷見 metal_completed_readable。
std::vector<std::uint8_t> MetalGl::graphics_read_completed(const GraphicsView& v){
    if(!metal_completed_readable(v.resource))return graphics_read(v);
    const auto& r=metal_resource(v.resource);const auto count=v.count?v.count:r.desc.width-v.first;
    if(std::uint64_t{v.first}+count>r.desc.width)throw std::invalid_argument("緩衝區讀回越界");
    const auto* p=static_cast<const std::uint8_t*>(r.buffer.contents)+v.first;
    return std::vector<std::uint8_t>(p,p+count);
}
void MetalGl::graphics_copy(const GraphicsView& dst,const GraphicsView& src,std::array<std::uint32_t,3> origin,std::array<std::uint32_t,6> box){@autoreleasepool{
    auto& d=metal_resource(dst.resource);auto& s=metal_resource(src.resource);
    // D3D12 允許位元大小相同的未壓縮格式與 BC 區塊互拷（未壓縮的每個 texel 對應一個 4×4 區塊，例如 GPU 壓縮後
    // R32G32_UINT → BC4）。Metal 不能以 view 在兩者間轉換：經 GPU 暫存緩衝做兩次 blit，位元組原樣搬移。
    if(d.texture && s.texture && (graphics_format_bc_family(s.desc.format)!=0)!=(graphics_format_bc_family(d.desc.format)!=0)){
        const bool to_bc=graphics_format_bc_family(d.desc.format)!=0;const auto& bc=to_bc?d:s;const auto& plain=to_bc?s:d;
        const auto block=graphics_format_pitch(bc.desc.format,4,4)[0],bpp=graphics_format_bytes(plain.desc.format);
        if(block!=bpp || src.level>=s.desc.levels || dst.level>=d.desc.levels || src.slice>=s.desc.layers || dst.slice>=d.desc.layers || s.desc.samples!=1 || d.desc.samples!=1)
            throw std::invalid_argument("未壓縮與 BC 格式複製的位元大小或子資源不相容");
        const auto sw=std::max(1U,s.desc.width>>src.level),sh=std::max(1U,s.desc.height>>src.level),sz=std::max(1U,s.desc.depth>>src.level);
        const auto dw=std::max(1U,d.desc.width>>dst.level),dh=std::max(1U,d.desc.height>>dst.level),dz=std::max(1U,d.desc.depth>>dst.level);
        if(!box[3])box={0,0,0,sw,sh,sz};
        if(box[0]>=box[3] || box[1]>=box[4] || box[2]>=box[5] || box[3]>sw || box[4]>sh || box[5]>sz)throw std::invalid_argument("未壓縮與 BC 格式複製的來源範圍不合法");
        // 以區塊為單位的寬高：未壓縮端一個 texel＝一個區塊。
        const auto blocks_x=to_bc?box[3]-box[0]:(box[3]-box[0]+3)/4,blocks_y=to_bc?box[4]-box[1]:(box[4]-box[1]+3)/4,depth=box[5]-box[2];
        const auto row=std::uint64_t{blocks_x}*block,image=row*blocks_y;
        if(std::uint64_t{origin[2]}+depth>dz)throw std::invalid_argument("未壓縮與 BC 格式複製的目的範圍不合法");
        // 目的端尺寸：BC 端以像素計（邊緣不足 4 時裁到 mip 大小），未壓縮端以區塊數計。
        const auto out_x=to_bc?std::min<std::uint64_t>(std::uint64_t{blocks_x}*4,dw>origin[0]?dw-origin[0]:0):blocks_x;
        const auto out_y=to_bc?std::min<std::uint64_t>(std::uint64_t{blocks_y}*4,dh>origin[1]?dh-origin[1]:0):blocks_y;
        if(!out_x || !out_y || (!to_bc && (std::uint64_t{origin[0]}+out_x>dw || std::uint64_t{origin[1]}+out_y>dh)))throw std::invalid_argument("未壓縮與 BC 格式複製的目的範圍不合法");
        auto lease=reserve_gpu_memory(static_cast<std::size_t>(image*depth));
        auto staging=[device_ newBufferWithLength:static_cast<NSUInteger>(image*depth) options:MTLResourceStorageModePrivate];if(!staging)throw std::bad_alloc{};
        pending_leases_.push_back(std::move(lease));
        begin_commands(true,"copy_bc_cast");auto e=[commands_ blitCommandEncoder];MetalEncoderScope e_scope{e};
        [e copyFromTexture:s.texture sourceSlice:src.slice sourceLevel:src.level sourceOrigin:MTLOriginMake(box[0],box[1],box[2]) sourceSize:MTLSizeMake(box[3]-box[0],box[4]-box[1],depth)
                  toBuffer:staging destinationOffset:0 destinationBytesPerRow:static_cast<NSUInteger>(row) destinationBytesPerImage:static_cast<NSUInteger>(image)];
        [e copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:static_cast<NSUInteger>(row) sourceBytesPerImage:static_cast<NSUInteger>(image) sourceSize:MTLSizeMake(static_cast<NSUInteger>(out_x),static_cast<NSUInteger>(out_y),depth)
                 toTexture:d.texture destinationSlice:dst.slice destinationLevel:dst.level destinationOrigin:MTLOriginMake(origin[0],origin[1],origin[2])];
        e_scope.end();graphics_pending_.push_back(dst.resource);graphics_pending_.push_back(src.resource);return;
    }
    if(graphics_format_bc_family(s.desc.format)){
        if(src.level>=s.desc.levels || dst.level>=d.desc.levels || src.slice>=s.desc.layers || dst.slice>=d.desc.layers ||
           graphics_format_bc_family(s.desc.format)!=graphics_format_bc_family(d.desc.format))throw std::invalid_argument("BC 複製的格式或子資源不相容");
        const auto sw=std::max(1U,s.desc.width>>src.level),sh=std::max(1U,s.desc.height>>src.level),sz=std::max(1U,s.desc.depth>>src.level);
        const auto dw=std::max(1U,d.desc.width>>dst.level),dh=std::max(1U,d.desc.height>>dst.level),dz=std::max(1U,d.desc.depth>>dst.level);
        if(!box[3])box={0,0,0,sw,sh,sz};
        if(box[0]>=box[3] || box[1]>=box[4] || box[2]>=box[5] || box[3]>sw || box[4]>sh || box[5]>sz ||
           std::uint64_t{origin[0]}+box[3]-box[0]>dw || std::uint64_t{origin[1]}+box[4]-box[1]>dh || std::uint64_t{origin[2]}+box[5]-box[2]>dz ||
           box[0]%4 || box[1]%4 || (box[3]%4 && box[3]!=sw) || (box[4]%4 && box[4]!=sh))throw std::invalid_argument("BC 複製的區塊範圍不合法");
        // 同一 BC 家族不同 typed view 保留原始壓縮位元，不進行色彩空間轉換。
        auto pixels=graphics_read(src);const auto pitch=graphics_format_pitch(s.desc.format,sw,sh);
        const auto block=graphics_format_pitch(s.desc.format,4,4)[0];
        const auto plane=pitch[0]*pitch[1];const auto offset=std::size_t{box[2]}*plane+std::size_t{box[1]/4}*pitch[0]+std::size_t{box[0]/4}*block;
        graphics_write_region(dst,std::span<const std::uint8_t>(pixels).subspan(offset),pitch[0],plane,
            {origin[0],origin[1],origin[2],origin[0]+box[3]-box[0],origin[1]+box[4]-box[1],origin[2]+box[5]-box[2]});return;
    }
    if(d.texture && s.texture && d.texture.pixelFormat!=s.texture.pixelFormat && (metal_depth_format(d.texture.pixelFormat) || metal_depth_format(s.texture.pixelFormat))){
        if(d.desc.samples!=1 || s.desc.samples!=1)throw std::invalid_argument("不同 Metal 深度實體格式不能複製 MSAA");
        const auto sw=std::max(1U,s.desc.width>>src.level),sh=std::max(1U,s.desc.height>>src.level),bpp=graphics_format_bytes(s.desc.format);
        if(!box[3])box={0,0,0,sw,sh,1};
        if(box[0]>=box[3] || box[1]>=box[4] || box[3]>sw || box[4]>sh || box[2] || box[5]!=1 || origin[2])throw std::invalid_argument("深度資源複製區域不合法");
        auto pixels=graphics_read(src);const auto offset=(std::size_t{box[1]}*sw+box[0])*bpp;
        graphics_write_region(dst,std::span<const std::uint8_t>(pixels).subspan(offset),sw*bpp,0,{origin[0],origin[1],0,origin[0]+box[3]-box[0],origin[1]+box[4]-box[1],1});return;
    }
    begin_commands(true,"copy");auto e=[commands_ blitCommandEncoder];MetalEncoderScope e_scope{e};
    if(d.buffer && s.buffer){if(!box[3])box[3]=s.desc.width;if(box[0]>box[3] || box[3]>s.desc.width || std::uint64_t{origin[0]}+box[3]-box[0]>d.desc.width)throw std::invalid_argument("緩衝區複製越界");d.constant_bytes_valid=false;[e copyFromBuffer:s.buffer sourceOffset:box[0] toBuffer:d.buffer destinationOffset:origin[0] size:box[3]-box[0]];}
    else if(d.texture && s.texture){if(!box[3]){box[3]=std::max(1U,s.desc.width>>src.level);box[4]=std::max(1U,s.desc.height>>src.level);box[5]=std::max(1U,s.desc.depth>>src.level);}for(unsigned i=0;i<3;++i)if(box[i]>=box[i+3])throw std::invalid_argument("紋理複製區域不合法");auto source_texture=s.texture;if(source_texture.pixelFormat!=d.texture.pixelFormat){source_texture=[source_texture newTextureViewWithPixelFormat:d.texture.pixelFormat];if(!source_texture)throw std::invalid_argument("Metal 不能建立同格式家族的複製 view");}[e copyFromTexture:source_texture sourceSlice:src.slice sourceLevel:src.level sourceOrigin:MTLOriginMake(box[0],box[1],box[2]) sourceSize:MTLSizeMake(box[3]-box[0],box[4]-box[1],box[5]-box[2]) toTexture:d.texture destinationSlice:dst.slice destinationLevel:dst.level destinationOrigin:MTLOriginMake(origin[0],origin[1],origin[2])];}
    else throw std::invalid_argument("資源複製類型不符");e_scope.end();graphics_pending_.push_back(dst.resource);graphics_pending_.push_back(src.resource);
}}
void MetalGl::graphics_resolve(const GraphicsView& dst,const GraphicsView& src,std::uint32_t format){@autoreleasepool{
    auto& d=metal_resource(dst.resource);auto& s=metal_resource(src.resource);
    if(!d.texture || !s.texture || d.desc.dimension!=3 || s.desc.dimension!=3 || d.desc.samples!=1 || s.desc.samples<=1 ||
       dst.level>=d.desc.levels || src.level>=s.desc.levels || dst.slice>=d.desc.layers || src.slice>=s.desc.layers ||
       std::max(1U,d.desc.width>>dst.level)!=s.desc.width || std::max(1U,d.desc.height>>dst.level)!=s.desc.height ||
       metal_depth_format(s.texture.pixelFormat) || !graphics_samples_supported(format,s.desc.samples) ||
       d.texture.pixelFormat!=metal_format(format) || s.texture.pixelFormat!=metal_format(format))throw std::invalid_argument("Metal resolve 的格式／子資源／取樣數不相容");
    begin_commands(true,"resolve");auto pass=[MTLRenderPassDescriptor renderPassDescriptor];auto color=pass.colorAttachments[0];
    color.texture=s.texture;color.level=src.level;color.slice=src.slice;color.loadAction=MTLLoadActionLoad;
    color.resolveTexture=d.texture;color.resolveLevel=dst.level;color.resolveSlice=dst.slice;
    // ResolveSubresource 不破壞來源；保留 MSAA samples，不能只用 discard resolve。
    color.storeAction=MTLStoreActionStoreAndMultisampleResolve;
    auto encoder=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope encoder_scope{encoder};if(!encoder)throw std::runtime_error("Metal resolve encoder 建立失敗");
    encoder_scope.end();graphics_pending_.push_back(dst.resource);graphics_pending_.push_back(src.resource);
}}
void MetalGl::graphics_clear(const GraphicsView& v,std::array<float,4> color,std::uint32_t flags){@autoreleasepool{
    const auto target=metal_render_target(v);begin_commands(true,"clear");for(unsigned slice=0;slice<target.layers;++slice){auto p=[MTLRenderPassDescriptor renderPassDescriptor];if(!flags){metal_attach_target(p.colorAttachments[0],target,slice);p.colorAttachments[0].loadAction=MTLLoadActionClear;p.colorAttachments[0].storeAction=MTLStoreActionStore;p.colorAttachments[0].clearColor=MTLClearColorMake(color[0],color[1],color[2],color[3]);}
    else{metal_attach_target(p.depthAttachment,target,slice);p.depthAttachment.loadAction=(flags&1)?MTLLoadActionClear:MTLLoadActionLoad;p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=color[0];if(target.texture.pixelFormat==MTLPixelFormatDepth32Float_Stencil8){metal_attach_target(p.stencilAttachment,target,slice);p.stencilAttachment.loadAction=(flags&2)?MTLLoadActionClear:MTLLoadActionLoad;p.stencilAttachment.storeAction=MTLStoreActionStore;p.stencilAttachment.clearStencil=static_cast<std::uint32_t>(color[1]);}}
    auto e=[commands_ renderCommandEncoderWithDescriptor:p];MetalEncoderScope e_scope{e};if(!e)throw std::runtime_error("Metal clear encoder 建立失敗");e_scope.end();}graphics_pending_.push_back(v.resource);
}}
void MetalGl::graphics_bind(id<MTLRenderCommandEncoder> render,id<MTLComputeCommandEncoder> compute,const GraphicsStage& stage,bool vertex){
    if(!stage.shader)return;graphics_pending_.push_back(stage.shader);
    const auto views_slot=dynamic_cast<MetalShader&>(*stage.shader).buffer_views_slot;
    if(views_slot!=UINT32_MAX){
        std::array<std::array<std::uint32_t,4>,136> views{};
        for(unsigned i=0;i<stage.resources.size();++i)views[i]=metal_buffer_view_info(stage.resources[i]);
        for(unsigned i=0;i<stage.unordered.size();++i)views[128+i]=metal_buffer_view_info(stage.unordered[i]);
        if(compute)[compute setBytes:views.data() length:sizeof(views) atIndex:views_slot];else if(vertex)[render setVertexBytes:views.data() length:sizeof(views) atIndex:views_slot];else [render setFragmentBytes:views.data() length:sizeof(views) atIndex:views_slot];
    }
    for(const auto& b:stage.shader->bindings){std::shared_ptr<GraphicsResource> resource;GraphicsView v;id<MTLSamplerState> sampler=nil;
        if(b.type>=6 && b.type<=9){
            // D3D12 描述元表：綁定描述元堆積的 GPU 鏡像，著色器以執行期陣列讀取 MTLResourceID 或位址＋長度。
            const GraphicsDescriptorTable* table=nullptr;
            for(const auto& t:stage.tables)if(t.type==b.type && t.space==b.space && t.slot==b.slot){table=&t;break;}
            if(!table || !table->heap)throw std::invalid_argument("著色器描述元表缺少根簽章對應的範圍");
            if(!encoder_chain_fence_){encoder_chain_fence_=[device_ newFence];if(!encoder_chain_fence_)throw std::bad_alloc{};metal_encoder_chain=encoder_chain_fence_;}
            metal_commit_residency();auto& mirror=metal_resource(table->heap);graphics_pending_.push_back(table->heap);graphics_pending_.tables=true;
            if(compute)[compute setBuffer:mirror.buffer offset:table->offset atIndex:b.native_slot];
            else if(vertex)[render setVertexBuffer:mirror.buffer offset:table->offset atIndex:b.native_slot];
            else [render setFragmentBuffer:mirror.buffer offset:table->offset atIndex:b.native_slot];
            // 有界小表的項目已逐項解析：宣告使用，讓 Metal 依讀寫追蹤與前後 pass 的危害。
            const MTLResourceUsage usage=b.type==7 || b.type==9?MTLResourceUsageRead|MTLResourceUsageWrite:MTLResourceUsageRead;
            for(const auto& view:table->views){
                if(!view.resource)continue;auto& native=metal_resource(view.resource);id<MTLResource> used=native.texture?(id<MTLResource>)native.texture:(id<MTLResource>)native.buffer;
                if(!used)continue;graphics_pending_.push_back(view.resource);
                if(compute)[compute useResource:used usage:usage];else [render useResource:used usage:usage stages:vertex?MTLRenderStageVertex:MTLRenderStageFragment];
            }
            continue;
        }
        if(b.type==0){if(b.slot>=stage.constants.size())throw std::invalid_argument("常數緩衝區超出範圍");resource=stage.constants[b.slot];}
        else if(b.type==2){if(b.slot>=stage.samplers.size())throw std::invalid_argument("取樣器超出範圍");if(stage.samplers[b.slot]){sampler=dynamic_cast<MetalSampler&>(*stage.samplers[b.slot]).sampler;graphics_pending_.push_back(stage.samplers[b.slot]);}}
        else {const bool uav=b.type==4 || b.type==5;if(b.slot>=(uav?stage.unordered.size():stage.resources.size()))throw std::invalid_argument("著色器資源超出範圍");v=uav?stage.unordered[b.slot]:stage.resources[b.slot];resource=v.resource;}
        if(b.type==2){if(compute)[compute setSamplerState:sampler atIndex:b.native_slot];else if(vertex)[render setVertexSamplerState:sampler atIndex:b.native_slot];else [render setFragmentSamplerState:sampler atIndex:b.native_slot];continue;}
        id<MTLBuffer> buffer=nil;id<MTLTexture> texture=nil;std::size_t offset=0;if(resource){auto& native=metal_resource(resource);buffer=native.buffer;texture=native.texture?metal_shader_texture(v):nil;offset=v.first;graphics_pending_.push_back(resource);}if(b.type==0 && resource)offset=stage.constant_offsets[b.slot];
        if(b.type==1 || b.type==5){if(buffer)texture=metal_buffer_texture(v,views_slot==UINT32_MAX);if(compute)[compute setTexture:texture atIndex:b.native_slot];else if(vertex)[render setVertexTexture:texture atIndex:b.native_slot];else [render setFragmentTexture:texture atIndex:b.native_slot];}
        else {if(compute)[compute setBuffer:buffer offset:offset atIndex:b.native_slot];else if(vertex)[render setVertexBuffer:buffer offset:offset atIndex:b.native_slot];else [render setFragmentBuffer:buffer offset:offset atIndex:b.native_slot];if(b.counter_slot!=UINT32_MAX && resource){if(!v.counter)throw std::invalid_argument("著色器需要具有計數器的 UAV view");auto counter=metal_resource(v.counter).buffer;graphics_pending_.push_back(v.counter);if(compute)[compute setBuffer:counter offset:0 atIndex:b.counter_slot];else [render setFragmentBuffer:counter offset:0 atIndex:b.counter_slot];}}
    }
}
#include "metal_tessellation.inc"
void MetalGl::graphics_draw(const GraphicsDraw& original){@autoreleasepool{
    if(original.indirect){
        const auto size=original.indexed?20U:16U;auto& arguments=metal_resource(original.indirect);
        if(!arguments.buffer || (original.indirect_offset&3U) || std::uint64_t{original.indirect_offset}+size>arguments.desc.width)throw std::invalid_argument("間接繪製參數越界或未對齊");
        // 細分、幾何／SO 需要 CPU 端數量：等 GPU 寫完後讀回參數，轉成一般繪製。SO 統計只在 GS 路徑累計；
        // 每個 context 都持有統計物件，不能以其存在判斷（否則每次間接繪製都同步等待 GPU）。
        if(original.stages[2].shader || original.stages[3].shader || original.stages[4].shader ||
           std::any_of(original.stream_targets.begin(),original.stream_targets.end(),[](const auto& target){return bool(target);})){
            GraphicsView view;view.resource=original.indirect;view.first=original.indirect_offset;view.count=size;
            const auto bytes=graphics_read(view);std::array<std::uint32_t,5> values{};std::memcpy(values.data(),bytes.data(),size);
            // 副本放堆積：GraphicsDraw 約 62 KB，放在堆疊會讓每次繪製的框架都多出這段空間。
            auto resolved=std::make_unique<GraphicsDraw>(original);resolved->indirect={};resolved->indirect_offset=0;resolved->count=values[0];resolved->instances=values[1];resolved->start=values[2];
            if(original.indexed){resolved->base_vertex=static_cast<std::int32_t>(values[3]);resolved->start_instance=values[4];}else resolved->start_instance=values[3];
            graphics_draw(*resolved);return;
        }
    }
    std::unique_ptr<GraphicsDraw> raster;
    if(metal_stream_output_gpu_enabled() && original.stages[2].shader && !original.stages[2].shader->stream_output.empty() &&
       original.stages[2].shader->stage==0 && original.stages[2].shader->rasterized_stream!=UINT32_MAX &&
       !original.stages[3].shader && !original.stages[4].shader &&
       std::none_of(original.stream_targets.begin(),original.stream_targets.end(),[](const auto& target){return bool(target);}) &&
       (original.depth.resource || std::any_of(original.targets.begin(),original.targets.end(),[](const auto& target){return bool(target.resource);}))){
        // SO 簽章沒有可執行的 GS；未綁定 SO buffer 時直接使用目前 VS 光柵化。
        // 保留原始 draw 供診斷，並維持既有 SO 所需圖元統計。
        raster=std::make_unique<GraphicsDraw>(original);raster->stages[2]={};
    }
    const auto& d=raster?*raster:original;
    if(!d.indirect && (!d.count || !d.instances))return;if(!d.stages[0].shader)throw std::invalid_argument("繪製缺少 VS 著色器");const bool tessellated=bool(d.stages[3].shader || d.stages[4].shader);
    if(tessellated && (!d.stages[3].shader || !d.stages[4].shader))throw std::invalid_argument("細分繪製缺少 HS 或 DS");
    for(const auto& stage:d.stages)if(stage.shader)if(auto* metal=dynamic_cast<MetalShader*>(stage.shader.get()))metal_resolve_shader(device_,*metal);
    auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=dynamic_cast<MetalShader&>(*d.stages[0].shader).function;desc.fragmentFunction=d.stages[1].shader?dynamic_cast<MetalShader&>(*d.stages[1].shader).function:nil;desc.sampleCount=1;desc.alphaToCoverageEnabled=!d.blend.empty() && metal_word(d.blend,0);
    if(d.alpha_function && d.alpha_function!=8){
        if(d.alpha_function>8 || !d.stages[1].shader)throw std::invalid_argument("Alpha test 需要有效比較函數及像素著色器");
        auto& shader=dynamic_cast<MetalShader&>(*d.stages[1].shader);const unsigned key=d.alpha_function*256+(d.alpha_reference&255);
        auto& function=shader.alpha_functions[key];
        if(!function){
            std::smatch color;const std::regex declaration(R"((?:float|half)4\s+(\w+)\s*\[\[\s*color\s*\(\s*0\s*\)\s*\]\])");
            if(!std::regex_search(shader.source,color,declaration))throw std::invalid_argument("Alpha test 找不到 RT0 輸出");
            auto source=shader.source;const auto begin=source.find("fragment ");const auto opening=source.find('{',begin);if(begin==source.npos || opening==source.npos)throw std::invalid_argument("Alpha test 找不到像素入口");
            unsigned depth=1;std::size_t closing=opening+1;for(;closing<source.size() && depth;++closing){if(source[closing]=='{')++depth;else if(source[closing]=='}')--depth;}if(depth)throw std::invalid_argument("Alpha test 像素入口不完整");
            const std::regex returns(R"(return\s+(\w+)\s*;)");auto body=source.substr(opening,closing-opening);if(!std::regex_search(body,returns))throw std::invalid_argument("Alpha test 找不到像素回傳");
            constexpr const char* comparisons[]{"","","<","==","<=",">","!=",">="};
            const std::string condition=d.alpha_function==1?"true":"!($1."+color[1].str()+".a "+comparisons[d.alpha_function]+" ("+std::to_string(d.alpha_reference&255)+".0f/255.0f))";
            body=std::regex_replace(body,returns,"if ("+condition+") discard_fragment(); return $1;");source.replace(opening,closing-opening,body);source=std::regex_replace(source,std::regex(R"(\[\[early_fragment_tests\]\])"),"");
            NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
            if(!library)throw std::invalid_argument("Metal Alpha test 編譯失敗："+std::string(error.localizedDescription.UTF8String));function=[library newFunctionWithName:@"xlatMtlMain"];if(!function)throw std::invalid_argument("Metal Alpha test 沒有入口");
        }
        desc.fragmentFunction=function;
    }
    const bool streaming=d.stages[2].shader && !d.stages[2].shader->stream_output.empty();
    const bool stream_only=streaming && (d.stages[2].shader->rasterized_stream==UINT32_MAX || (!d.depth.resource && std::none_of(d.targets.begin(),d.targets.end(),[](const auto& v){return bool(v.resource);})));
    const auto& raster_shader=dynamic_cast<MetalShader&>(*d.stages[tessellated?4:d.stages[2].shader?2:0].shader);
    const auto raster_topology=tessellated?4U:d.stages[2].shader && d.stages[2].shader->stage==2?(raster_shader.geometry_output==1?1U:raster_shader.geometry_output==3?2U:4U):d.topology;
    desc.inputPrimitiveTopology=metal_primitive_class(raster_topology);
    if(!tessellated && !d.stages[2].shader && raster_topology!=1){
        auto& shader=dynamic_cast<MetalShader&>(*d.stages[0].shader);
        // 原始碼在著色器建立後不變；同時保存「不需替換」的結果，避免每次 draw 掃描。
        // 關閉快取僅供同一引擎二進位的效能與畫面 A/B 比對。
        static const bool cache_analysis=[] {const auto* value=std::getenv("X64ARM_METAL_SHADER_ANALYSIS_CACHE");return !value || std::string_view(value)!="0";}();
        if(!cache_analysis || !shader.non_point_checked){
            const auto source=metal_nonpoint_source(shader.source);
            if(source!=shader.source && !shader.non_point_function){
                NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
                auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
                if(!library)throw std::invalid_argument("Metal 非點圖元著色器編譯失敗："+std::string(error.localizedDescription.UTF8String));
                shader.non_point_function=[library newFunctionWithName:@"xlatMtlMain"];
                if(!shader.non_point_function)throw std::invalid_argument("Metal 非點圖元著色器沒有入口");
            }
            shader.non_point_checked=true;
        }
        if(shader.non_point_function)desc.vertexFunction=shader.non_point_function;
    }
    const bool layered=std::any_of(raster_shader.output_varyings.begin(),raster_shader.output_varyings.end(),[](const auto& v){return v.system==NAME_RENDER_TARGET_ARRAY_INDEX;});
    const bool dual_source=[&]{if(!d.stages[1].shader || d.blend.empty() || !metal_word(d.blend,8))return false;for(const auto offset:{12U,16U,24U,28U}){const auto factor=metal_word(d.blend,offset);if(factor>=16 && factor<=19)return true;}return false;}();
    if(dual_source){
        // D3D 的 o1 在雙來源混色時是 RT0 的第二來源；一般模式仍是 MRT1。
        // 同一份 PS 可切換兩種 blend state，保留原入口並快取各自的實際 Metal function。
        if(!d.targets[0].resource || std::any_of(d.targets.begin()+1,d.targets.end(),[](const auto& view){return bool(view.resource);}))throw std::invalid_argument("雙來源混色只能綁定 RT0");
        auto& shader=dynamic_cast<MetalShader&>(*d.stages[1].shader);
        if(!shader.dual_source_function){
            const std::regex output(R"(\[\[\s*color\s*\(\s*(?:1|xlt_remap_o\s*\[\s*1\s*\])\s*\)\s*\]\])");
            if(!std::regex_search(shader.source,output))throw std::invalid_argument("雙來源混色缺少像素著色器 o1");
            const auto source=std::regex_replace(shader.source,output,"[[color(0), index(1)]]");
            NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
            auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
            if(!library)throw std::invalid_argument("Metal 雙來源著色器編譯失敗："+std::string(error.localizedDescription.UTF8String));
            shader.dual_source_function=[library newFunctionWithName:@"xlatMtlMain"];
            if(!shader.dual_source_function)throw std::invalid_argument("Metal 雙來源著色器沒有入口");
        }
        desc.fragmentFunction=shader.dual_source_function;
    }
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];unsigned width=0,height=0,layers=0,sample_count=0;bool center_samples=false;
    const auto sample_pattern=[&](const GraphicsResourceDesc& resource){
        const bool center=resource.samples>1 && resource.sample_quality==UINT32_MAX-1U;
        if(sample_count && (sample_count!=resource.samples || center_samples!=center))throw std::invalid_argument("Render-target／depth 的取樣數與品質模式不相容");
        sample_count=resource.samples;center_samples=center;
    };
    const auto target_extent=[&](unsigned w,unsigned h){width=width?std::min(width,w):w;height=height?std::min(height,h):h;};
    // 各 view 層數不同時取最小值：超出較短 view 的層 D3D 視為捨棄，Metal 的 renderTargetArrayLength 也不得超過任一 attachment。
    const auto target_layers=[&](unsigned count){if(!count)throw std::invalid_argument("Render-target／depth view 沒有層");layers=layers?std::min(layers,count):count;};
    for(const auto& target:d.targets)if(target.resource)sample_pattern(target.resource->desc);
    if(d.depth.resource)sample_pattern(d.depth.resource->desc);
    if(sample_count>1){
        std::array<MTLSamplePosition,8> positions{};
        if(center_samples)std::fill_n(positions.begin(),sample_count,MTLSamplePosition{0.5F,0.5F});
        else [device_ getDefaultSamplePositions:positions.data() count:sample_count];
        [pass setSamplePositions:positions.data() count:sample_count];
    }
    for(unsigned i=0;i<8;++i)if(d.targets[i].resource){const auto& v=d.targets[i];const auto target=metal_render_target(v);target_layers(target.layers);auto texture=target.texture;desc.colorAttachments[i].pixelFormat=texture.pixelFormat;desc.sampleCount=texture.sampleCount;metal_attach_target(pass.colorAttachments[i],target);pass.colorAttachments[i].loadAction=MTLLoadActionLoad;pass.colorAttachments[i].storeAction=MTLStoreActionStore;target_extent(std::max(1U,v.resource->desc.width>>v.level),std::max(1U,v.resource->desc.height>>v.level));graphics_pending_.push_back(v.resource);
        if(!d.blend.empty()){const auto off=8+32*(metal_word(d.blend,4)?i:0);auto a=desc.colorAttachments[i];a.blendingEnabled=metal_word(d.blend,off);if(a.blendingEnabled){a.sourceRGBBlendFactor=metal_d3d_blend(metal_word(d.blend,off+4));a.destinationRGBBlendFactor=metal_d3d_blend(metal_word(d.blend,off+8));a.rgbBlendOperation=static_cast<MTLBlendOperation>(metal_word(d.blend,off+12)-1);a.sourceAlphaBlendFactor=metal_d3d_blend(metal_word(d.blend,off+16));a.destinationAlphaBlendFactor=metal_d3d_blend(metal_word(d.blend,off+20));a.alphaBlendOperation=static_cast<MTLBlendOperation>(metal_word(d.blend,off+24)-1);}const auto mask=d.blend[off+28];a.writeMask=static_cast<MTLColorWriteMask>(((mask&1)?MTLColorWriteMaskRed:0)|((mask&2)?MTLColorWriteMaskGreen:0)|((mask&4)?MTLColorWriteMaskBlue:0)|((mask&8)?MTLColorWriteMaskAlpha:0));}}
    if(!d.stages[1].shader)for(unsigned i=0;i<8;++i)desc.colorAttachments[i].writeMask=MTLColorWriteMaskNone;
    if(d.depth.resource){const auto target=metal_render_target(d.depth);target_layers(target.layers);auto texture=target.texture;desc.depthAttachmentPixelFormat=texture.pixelFormat;metal_attach_target(pass.depthAttachment,target);pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;target_extent(std::max(1U,d.depth.resource->desc.width>>d.depth.level),std::max(1U,d.depth.resource->desc.height>>d.depth.level));desc.sampleCount=texture.sampleCount;graphics_pending_.push_back(d.depth.resource);if(texture.pixelFormat==MTLPixelFormatDepth32Float_Stencil8){desc.stencilAttachmentPixelFormat=texture.pixelFormat;metal_attach_target(pass.stencilAttachment,target);pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}}
    if(layered || layers>1)pass.renderTargetArrayLength=std::max(1U,layers);
    // D3D 未綁定 DSV 時忽略 SV_Depth；Metal 仍要求匹配的 attachment。
    // 私有暫存深度不測試、不寫入、不保存，且不暴露給客體。
    if(!d.depth.resource && width && height && d.stages[1].shader && dynamic_cast<MetalShader&>(*d.stages[1].shader).writes_depth){
        const std::array<unsigned,4> size{width,height,static_cast<unsigned>(desc.sampleCount),std::max(1U,layers)};
        auto found=discard_depth_.find(size);if(found==discard_depth_.end()){if(discard_depth_.size()>=8)discard_depth_.clear();GraphicsResourceDesc resource;resource.dimension=3;resource.width=width;resource.height=height;resource.format=40;resource.bind=0x40;resource.samples=size[2];resource.layers=size[3];found=discard_depth_.emplace(size,graphics_resource(resource)).first;}
        auto texture=metal_resource(found->second).texture;desc.depthAttachmentPixelFormat=texture.pixelFormat;pass.depthAttachment.texture=texture;pass.depthAttachment.loadAction=MTLLoadActionDontCare;pass.depthAttachment.storeAction=MTLStoreActionDontCare;graphics_pending_.push_back(found->second);
    }
    if(d.visibility){pass.visibilityResultBuffer=metal_resource(d.visibility).buffer;graphics_pending_.push_back(d.visibility);}
    if((!width || !height) && std::any_of(d.stages[1].unordered.begin(),d.stages[1].unordered.end(),[](const auto& view){return bool(view.resource);})){for(const auto& viewport:d.viewports){width=std::max(width,static_cast<unsigned>(std::max(0.0F,std::ceil(viewport[0]+viewport[2]))));height=std::max(height,static_cast<unsigned>(std::max(0.0F,std::ceil(viewport[1]+viewport[3]))));}pass.renderTargetWidth=width;pass.renderTargetHeight=height;pass.defaultRasterSampleCount=1;}
    if(stream_only){width=height=1;}
    if(!width || !height)throw std::invalid_argument("繪製沒有輸出目標或 UAV viewport");
    std::array<int,32> slots;slots.fill(-1);std::set<unsigned> occupied;for(const auto& b:d.stages[0].shader->bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=9)){occupied.insert(b.native_slot);if(b.counter_slot!=UINT32_MAX)occupied.insert(b.counter_slot);}const auto views_slot=dynamic_cast<MetalShader&>(*d.stages[0].shader).buffer_views_slot;if(views_slot!=UINT32_MAX)occupied.insert(views_slot);int next=30;auto vertex=[MTLVertexDescriptor vertexDescriptor];
    std::set<unsigned> input_attributes;std::vector<std::array<std::uint32_t,4>> fetched; // attribute、D3D slot、offset、大小。
    for(const auto& input:d.stages[0].shader->inputs){if(input.attribute>=31 || !input_attributes.insert(input.attribute).second)throw std::invalid_argument("Metal 頂點 attribute 位置越界或重複");auto it=std::find_if(d.layout.begin(),d.layout.end(),[&](const auto& e){return e.semantic==input.semantic && e.index==input.index;});if(it==d.layout.end())throw std::invalid_argument("頂點 layout 缺少 semantic："+input.semantic);const auto& e=*it;if(e.slot>=32 || !d.vertices[e.slot].resource)throw std::invalid_argument("頂點緩衝區未綁定");if(slots[e.slot]<0){while(next>=0 && occupied.contains(static_cast<unsigned>(next)))--next;if(next<0)throw std::invalid_argument("Metal 頂點與常數 buffer binding 配額不足");slots[e.slot]=next--;}
        const auto s=static_cast<unsigned>(slots[e.slot]);vertex.attributes[input.attribute].format=metal_vertex_format(e.format);vertex.attributes[input.attribute].offset=e.offset;vertex.attributes[input.attribute].bufferIndex=s;vertex.layouts[s].stride=d.vertices[e.slot].stride;vertex.layouts[s].stepFunction=e.classification?MTLVertexStepFunctionPerInstance:MTLVertexStepFunctionPerVertex;vertex.layouts[s].stepRate=e.classification?std::max(1U,e.step):1;
        fetched.push_back({input.attribute,e.slot,e.offset,graphics_format_bytes(e.format)});
    }
    // D3D11 允許 stride 0（每個頂點讀同一筆；UE4 沒有頂點色的網格綁定 stride 0 的空顏色緩衝）與 offset＋大小超出 stride 的
    // attribute；Metal 頂點描述兩者都在建 pipeline 時驗證失敗並直接 abort。stride 0 改為常數步進，以 attribute 涵蓋範圍
    // （4 位元組對齊）當 stride；超出 stride 的 attribute 改綁到另一個 buffer index，offset 併入該 binding 的 buffer offset。
    std::vector<std::array<std::uint32_t,3>> shifted; // Metal buffer index、D3D slot、額外 offset。
    {
        std::array<std::uint32_t,32> extent{};for(const auto& a:fetched)extent[a[1]]=std::max(extent[a[1]],a[2]+a[3]);
        for(unsigned i=0;i<32;++i)if(slots[i]>=0 && !d.vertices[i].stride){auto layout=vertex.layouts[static_cast<unsigned>(slots[i])];layout.stepFunction=MTLVertexStepFunctionConstant;layout.stepRate=0;layout.stride=std::max(4U,(extent[i]+3U)&~3U);}
        std::array<bool,32> kept{};
        for(const auto& a:fetched){
            const auto stride=d.vertices[a[1]].stride;if(!stride || a[2]+a[3]<=stride){kept[a[1]]=true;continue;}
            auto found=std::find_if(shifted.begin(),shifted.end(),[&](const auto& b){return b[1]==a[1] && b[2]==a[2];});
            if(found==shifted.end()){
                while(next>=0 && occupied.contains(static_cast<unsigned>(next)))--next;if(next<0)throw std::invalid_argument("Metal 頂點與常數 buffer binding 配額不足");
                const auto index=static_cast<std::uint32_t>(next--);const auto source=vertex.layouts[static_cast<unsigned>(slots[a[1]])];auto layout=vertex.layouts[index];
                layout.stride=stride;layout.stepFunction=source.stepFunction;layout.stepRate=source.stepRate;found=shifted.insert(shifted.end(),{index,a[1],a[2]});
            }
            vertex.attributes[a[0]].bufferIndex=(*found)[0];vertex.attributes[a[0]].offset=0;
        }
        // 原 binding 沒有剩下任何 attribute 時，Metal 不接受仍設 stride 的 layout：恢復為未使用。
        for(unsigned i=0;i<32;++i)if(slots[i]>=0 && !kept[i]){auto layout=vertex.layouts[static_cast<unsigned>(slots[i])];layout.stride=0;layout.stepFunction=MTLVertexStepFunctionPerVertex;layout.stepRate=1;}
    }
    if(!d.stages[0].shader->inputs.empty())desc.vertexDescriptor=vertex;
    MetalPipelineKey key;key<<dynamic_cast<MetalShader&>(*d.stages[0].shader).serial<<':'<<(d.stages[1].shader?dynamic_cast<MetalShader&>(*d.stages[1].shader).serial:0)<<':'<<desc.sampleCount<<':'<<desc.depthAttachmentPixelFormat;
    for(unsigned i=0;i<d.targets.size();++i)key<<':'<<desc.colorAttachments[i].pixelFormat;key<<":alpha:"<<d.alpha_function<<':'<<d.alpha_reference;key<<":stencil:"<<desc.stencilAttachmentPixelFormat<<":input-topology:"<<metal_primitive_class(raster_topology);for(const auto& e:d.layout)key<<e.semantic<<':'<<e.index<<':'<<e.format<<':'<<e.slot<<':'<<e.offset<<':'<<e.classification<<':'<<e.step;{std::array<std::uint32_t,32> strides{};for(std::size_t i=0;i<strides.size();++i)strides[i]=d.vertices[i].stride;key<<':';key.raw(strides.data(),sizeof strides);}key<<':'<<d.blend.size();key.raw(d.blend.data(),d.blend.size());
    MetalTessellationDraw tessellation;
    if(tessellated){
        key<<":tess:"<<dynamic_cast<MetalShader&>(*d.stages[3].shader).serial<<':'<<dynamic_cast<MetalShader&>(*d.stages[4].shader).serial<<':'<<d.topology;
        tessellation=graphics_tessellation(d,key.str());if(!tessellation.count)return;
        const auto& p=*tessellation.pipeline;desc.vertexFunction=p.domain;desc.vertexDescriptor=p.descriptor;
        desc.tessellationPartitionMode=static_cast<MTLTessellationPartitionMode>(p.settings[0]);desc.tessellationOutputWindingOrder=static_cast<MTLWinding>(p.settings[1]);desc.maxTessellationFactor=std::clamp(p.settings[2],1U,64U);
        desc.tessellationFactorFormat=MTLTessellationFactorFormatHalf;desc.tessellationFactorStepFunction=MTLTessellationFactorStepFunctionPerPatch;desc.tessellationControlPointIndexType=MTLTessellationControlPointIndexTypeNone;
    }
    MetalGeometryDraw geometry;
    if(d.stages[2].shader){geometry=graphics_geometry(d,vertex,slots,shifted,key.str());if(stream_only || !geometry.function){++draws_;graphics_capture(original);return;}if(!geometry.count)return;desc.vertexFunction=geometry.function;desc.vertexDescriptor=nil;key<<":geometry:"<<dynamic_cast<MetalShader&>(*d.stages[2].shader).serial;}
    key<<":samples:"<<desc.sampleCount<<":topology:"<<desc.inputPrimitiveTopology;
    id<MTLRenderPipelineState> pipeline=nil;const auto& k=key.str();if(const auto found=graphics_pipelines_.find(k);found!=graphics_pipelines_.end())pipeline=found->second;else {
        // 未命中才記錄（建立失敗時 Metal 可能直接 abort，用量快照留下最後一個嘗試的頂點佈局）。
        {std::string text="vs="+std::to_string(dynamic_cast<MetalShader&>(*d.stages[0].shader).serial)+" topology="+std::to_string(d.topology);for(const auto& e:d.layout)text+=" "+e.semantic+std::to_string(e.index)+":f"+std::to_string(e.format)+"@"+std::to_string(e.slot)+"+"+std::to_string(e.offset)+(e.classification?"i"+std::to_string(e.step):"")+"/s"+std::to_string(e.slot<32?d.vertices[e.slot].stride:0);set_diagnostic("metal_pipeline_last",text);record_diagnostic("metal_pipelines_created");}
        NSError* error=nil;pipeline=[device_ newRenderPipelineStateWithDescriptor:desc error:&error];if(!pipeline)throw std::invalid_argument("Metal D3D pipeline："+std::string(error.localizedDescription.UTF8String));if(graphics_pipelines_.size()>2048)graphics_pipelines_.clear();graphics_pipelines_[k]=pipeline;}
    auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=d.depth_state.empty()?MTLCompareFunctionLess:metal_word(d.depth_state,0)?static_cast<MTLCompareFunction>(metal_word(d.depth_state,8)-1):MTLCompareFunctionAlways;depth.depthWriteEnabled=d.depth_state.empty() || (metal_word(d.depth_state,0) && metal_word(d.depth_state,4));
    if(d.depth.flags&1)depth.depthWriteEnabled=NO;
    if(!d.depth.resource){depth.depthCompareFunction=MTLCompareFunctionAlways;depth.depthWriteEnabled=NO;}
    if(d.depth.resource && desc.stencilAttachmentPixelFormat!=MTLPixelFormatInvalid && !d.depth_state.empty() && metal_word(d.depth_state,12)){for(unsigned i=0;i<2;++i){const auto off=20+i*16;auto stencil=make_metal_stencil(static_cast<MTLCompareFunction>(metal_word(d.depth_state,off+12)-1),static_cast<MTLStencilOperation>(metal_word(d.depth_state,off)-1),static_cast<MTLStencilOperation>(metal_word(d.depth_state,off+4)-1),static_cast<MTLStencilOperation>(metal_word(d.depth_state,off+8)-1),d.depth_state[16],(d.depth.flags&2)?0:d.depth_state[17]);if(i)depth.backFaceStencil=stencil;else depth.frontFaceStencil=stencil;}}
    // 附件設定相同的連續繪製沿用同一個 render encoder：每個新 pass 在 TBDR GPU 上都要重新載入／寫回附件，
    // CPU 端也省去建立 encoder。像素 UAV、細分、幾何擷取不合併（D3D11 保證 draw 間 UAV 依序；後兩者另有編碼）。
    // 沿用時前一次繪製的固定功能狀態會殘留，下面一律重設。X64ARM_METAL_RENDER_COALESCE=0 關閉。
    static const bool coalesce_enabled=[] {const auto* value=std::getenv("X64ARM_METAL_RENDER_COALESCE");return !value || std::string_view(value)!="0";}();
    const bool coalesce=coalesce_enabled && !tessellated && !geometry.function &&
        std::none_of(d.stages[1].unordered.begin(),d.stages[1].unordered.end(),[](const auto& view){return bool(view.resource);});
    std::vector<std::uintptr_t> pass_key;
    if(coalesce){
        const auto object=[](id value){return reinterpret_cast<std::uintptr_t>((__bridge void*)value);};
        pass_key.reserve(48);
        for(unsigned i=0;i<8;++i){auto a=pass.colorAttachments[i];pass_key.insert(pass_key.end(),{object(a.texture),a.level,a.slice,a.depthPlane});}
        pass_key.insert(pass_key.end(),{object(pass.depthAttachment.texture),pass.depthAttachment.level,pass.depthAttachment.slice,static_cast<std::uintptr_t>(pass.depthAttachment.loadAction),
            object(pass.stencilAttachment.texture),pass.stencilAttachment.level,pass.stencilAttachment.slice,object(pass.visibilityResultBuffer),
            pass.renderTargetArrayLength,pass.renderTargetWidth,pass.renderTargetHeight,pass.defaultRasterSampleCount,sample_count,center_samples?1U:0U});
    }
    // 不計數的繪製可沿用已掛遮蔽查詢可見度緩衝（pass_key[39]）的 encoder：附件相同時只把可見度模式設為 Disabled。
    // 反向（開著的 encoder 沒有可見度緩衝、這次需要）仍須換 pass。X64ARM_METAL_VISIBILITY_REUSE=0 關閉。
    static const bool visibility_reuse=[] {const auto* value=std::getenv("X64ARM_METAL_VISIBILITY_REUSE");return !value || std::string_view(value)!="0";}();
    const auto compatible=[&]{
        if(pass_key==open_render_key_)return true;
        if(!visibility_reuse || d.visibility || pass_key.size()!=open_render_key_.size() || pass_key.size()<=39 || pass_key[39] || !open_render_key_[39])return false;
        for(std::size_t i=0;i<pass_key.size();++i)if(i!=39 && pass_key[i]!=open_render_key_[i])return false;
        return true;
    };
    id<MTLRenderCommandEncoder> enc=nil;const bool reused=coalesce && open_render_ && compatible();
    if(reused){enc=open_render_;++coalesced_draws_;}
    else{
        // 診斷分類：附件相同、只有遮蔽查詢可見度緩衝（pass_key[39]）不同而換 encoder 的次數另記。
        const char* why=coalesce?"draw_pass_change":"draw_uncoalesced";
        if(coalesce && open_render_ && pass_key.size()==open_render_key_.size() && pass_key.size()>39 && pass_key[39]!=open_render_key_[39]){
            bool only_visibility=true;
            for(std::size_t i=0;only_visibility && i<pass_key.size();++i)if(i!=39 && pass_key[i]!=open_render_key_[i])only_visibility=false;
            if(only_visibility)why="draw_visibility_change";
        }
        begin_commands(true,why);enc=[commands_ renderCommandEncoderWithDescriptor:pass];if(!enc)throw std::runtime_error("Metal draw encoder 建立失敗");++render_encoders_;if(coalesce)metal_chain_wait(enc);
        if(metal_labels()){
            NSMutableString* label=[NSMutableString stringWithString:@"draw"];
            for(const auto& item:d.stages)if(const auto* shader=dynamic_cast<const MetalShader*>(item.shader.get()))
                [label appendFormat:@" s%u:%016llx",static_cast<unsigned>(shader->stage),static_cast<unsigned long long>(shader->content_key)];
            id<MTLTexture> target=pass.colorAttachments[0].texture?pass.colorAttachments[0].texture:pass.depthAttachment.texture;
            if(target)[label appendFormat:@" %ux%u fmt%u",static_cast<unsigned>(target.width),static_cast<unsigned>(target.height),static_cast<unsigned>(target.pixelFormat)];
            enc.label=label;
        }
        if(coalesce){open_render_=enc;open_render_key_=std::move(pass_key);}}
    std::optional<MetalEncoderScope> enc_scope;if(!coalesce)enc_scope.emplace(enc);
    [enc setRenderPipelineState:pipeline];if(d.visibility)[enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:d.visibility_offset];else if(reused)[enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];[enc setDepthStencilState:cached_depth_state(depth)];[enc setStencilReferenceValue:d.stencil_reference];[enc setBlendColorRed:d.blend_factor[0] green:d.blend_factor[1] blue:d.blend_factor[2] alpha:d.blend_factor[3]];
    if(!d.viewports.empty()){std::vector<MTLViewport> views;for(auto v:d.viewports)views.push_back({v[0],v[1],v[2],v[3],v[4],v[5]});[enc setViewports:views.data() count:views.size()];}else [enc setViewport:MTLViewport{0,0,static_cast<double>(width),static_cast<double>(height),0,1}];
    if(!d.raster.empty()){const auto cull=metal_word(d.raster,4);[enc setCullMode:cull==1?MTLCullModeNone:cull==2?MTLCullModeFront:MTLCullModeBack];[enc setFrontFacingWinding:metal_word(d.raster,8)?MTLWindingCounterClockwise:MTLWindingClockwise];[enc setTriangleFillMode:metal_word(d.raster,0)==2?MTLTriangleFillModeLines:MTLTriangleFillModeFill];[enc setDepthBias:static_cast<float>(static_cast<std::int32_t>(metal_word(d.raster,12))) slopeScale:metal_float(d.raster,20) clamp:metal_float(d.raster,16)];[enc setDepthClipMode:metal_word(d.raster,24)?MTLDepthClipModeClip:MTLDepthClipModeClamp];if(metal_word(d.raster,28) && !d.scissors.empty()){const auto r=d.scissors.front();const auto x=static_cast<unsigned>(std::clamp(r[0],0,static_cast<int>(width))),y=static_cast<unsigned>(std::clamp(r[1],0,static_cast<int>(height))),right=static_cast<unsigned>(std::clamp(r[2],static_cast<int>(x),static_cast<int>(width))),bottom=static_cast<unsigned>(std::clamp(r[3],static_cast<int>(y),static_cast<int>(height)));[enc setScissorRect:MTLScissorRect{x,y,right-x,bottom-y}];}else if(reused)[enc setScissorRect:MTLScissorRect{0,0,width,height}];}
    else {[enc setCullMode:MTLCullModeBack];[enc setFrontFacingWinding:MTLWindingClockwise];
        if(reused){[enc setTriangleFillMode:MTLTriangleFillModeFill];[enc setDepthBias:0 slopeScale:0 clamp:0];[enc setDepthClipMode:MTLDepthClipModeClip];[enc setScissorRect:MTLScissorRect{0,0,width,height}];}}
    if(tessellated){auto stage=d.stages[4];stage.shader=tessellation.pipeline->stages[2];graphics_bind(enc,nil,stage,true);}
    else if(geometry.function)[enc setVertexBuffer:geometry.vertices offset:0 atIndex:0];else graphics_bind(enc,nil,d.stages[0],true);graphics_bind(enc,nil,d.stages[1],false);
    for(unsigned i=0;i<32;++i)if(!tessellated && !geometry.function && slots[i]>=0){auto& v=d.vertices[i];auto& r=metal_resource(v.resource);if(v.offset>=r.desc.width)throw std::invalid_argument("頂點 buffer offset 越界");[enc setVertexBuffer:r.buffer offset:v.offset atIndex:static_cast<unsigned>(slots[i])];graphics_pending_.push_back(v.resource);}
    if(!tessellated && !geometry.function)for(const auto& b:shifted){const auto& v=d.vertices[b[1]];[enc setVertexBuffer:metal_resource(v.resource).buffer offset:std::uint64_t{v.offset}+b[2] atIndex:b[0]];}
    MTLPrimitiveType primitive;switch(raster_topology){case 1:primitive=MTLPrimitiveTypePoint;break;case 2:primitive=MTLPrimitiveTypeLine;break;case 3:primitive=MTLPrimitiveTypeLineStrip;break;case 4:primitive=MTLPrimitiveTypeTriangle;break;case 5:primitive=MTLPrimitiveTypeTriangleStrip;break;default:throw std::invalid_argument("Metal topology 尚未映射");}
    if(tessellated){const auto& p=*tessellation.pipeline;for(unsigned instance=0;instance<d.instances;++instance){
        const std::uint64_t first=std::uint64_t{instance}*tessellation.count;
        [enc setVertexBuffer:tessellation.controls offset:first*p.control_count*p.control_stride atIndex:p.domain_control_slot];
        [enc setVertexBuffer:tessellation.patches offset:first*p.patch_stride atIndex:p.domain_patch_slot];
        [enc setTessellationFactorBuffer:tessellation.factors offset:first*p.factor_stride instanceStride:0];
        [enc drawPatches:p.control_count patchStart:0 patchCount:tessellation.count patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:std::uint64_t{d.start_instance}+instance];
    }}
    else if(geometry.function)[enc drawPrimitives:geometry.primitive vertexStart:0 vertexCount:geometry.count];
    else if(d.indirect){
        // 參數可能由先前的 GPU 工作寫入；Metal 依資源追蹤排序，不讀回 CPU。
        auto& arguments=metal_resource(d.indirect);
        if(d.indexed){auto& r=metal_resource(d.indices);[enc drawIndexedPrimitives:primitive indexType:d.index_format==57?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:r.buffer indexBufferOffset:d.index_offset indirectBuffer:arguments.buffer indirectBufferOffset:d.indirect_offset];graphics_pending_.push_back(d.indices);}
        else [enc drawPrimitives:primitive indirectBuffer:arguments.buffer indirectBufferOffset:d.indirect_offset];
        graphics_pending_.push_back(d.indirect);
    }
    else if(d.indexed){auto& r=metal_resource(d.indices);const auto bpp=d.index_format==57?2U:4U;const auto offset=std::uint64_t{d.index_offset}+std::uint64_t{d.start}*bpp;if(offset+std::uint64_t{d.count}*bpp>r.desc.width)throw std::invalid_argument("索引 buffer 越界");[enc drawIndexedPrimitives:primitive indexCount:d.count indexType:bpp==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:r.buffer indexBufferOffset:offset instanceCount:d.instances baseVertex:d.base_vertex baseInstance:d.start_instance];graphics_pending_.push_back(d.indices);}
    else [enc drawPrimitives:primitive vertexStart:d.start vertexCount:d.count instanceCount:d.instances baseInstance:d.start_instance];if(enc_scope)enc_scope->end();++draws_;if(++pending_>=128)flush(false,"batch_128");
    if(raster && original.stream_statistics)original.stream_statistics->needed+=std::uint64_t{metal_input_primitives(original.topology,original.count).first}*original.instances;
    graphics_capture(original);
}}
// 診斷：X64ARM_METAL_DISPATCH_SYNC=1 時每次 compute dispatch 後提交並等待完成，用來判斷 compute 寫入與後續讀取間的順序問題。
static bool metal_dispatch_sync(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_DISPATCH_SYNC");return text && std::string_view(text)=="1";}();return value;}
void MetalGl::graphics_dispatch_indirect(const GraphicsStage& stage,const std::shared_ptr<GraphicsResource>& arguments,std::uint32_t offset){@autoreleasepool{
    if(!stage.shader)throw std::invalid_argument("DispatchIndirect 缺少 CS");auto& args=metal_resource(arguments);
    if(!args.buffer || (offset&3U) || std::uint64_t{offset}+12>args.desc.width)throw std::invalid_argument("間接運算參數越界或未對齊");
    auto& shader=dynamic_cast<MetalShader&>(*stage.shader);metal_resolve_shader(device_,shader);begin_commands(true,"dispatch");auto e=[commands_ computeCommandEncoder];MetalEncoderScope e_scope{e};[e setComputePipelineState:shader.compute];graphics_bind(nil,e,stage,false);
    [e dispatchThreadgroupsWithIndirectBuffer:args.buffer indirectBufferOffset:offset threadsPerThreadgroup:MTLSizeMake(shader.threads[0],shader.threads[1],shader.threads[2])];
    graphics_pending_.push_back(arguments);e_scope.end();graphics_trace_dispatch(stage,{0,0,0},arguments,offset);if(metal_dispatch_sync())flush(true,"dispatch_sync");else if(++pending_>=128)flush(false,"batch_128");
}}
void MetalGl::graphics_dispatch(const GraphicsStage& stage,std::array<std::uint32_t,3> groups){@autoreleasepool{if(!stage.shader)throw std::invalid_argument("Dispatch 缺少 CS");auto& shader=dynamic_cast<MetalShader&>(*stage.shader);metal_resolve_shader(device_,shader);begin_commands(true,"dispatch");auto e=[commands_ computeCommandEncoder];MetalEncoderScope e_scope{e};
    if(metal_labels())e.label=[NSString stringWithFormat:@"cs %016llx %ux%ux%u",static_cast<unsigned long long>(shader.content_key),groups[0],groups[1],groups[2]];[e setComputePipelineState:shader.compute];graphics_bind(nil,e,stage,false);[e dispatchThreadgroups:MTLSizeMake(groups[0],groups[1],groups[2]) threadsPerThreadgroup:MTLSizeMake(shader.threads[0],shader.threads[1],shader.threads[2])];e_scope.end();graphics_trace_dispatch(stage,groups,{},0);if(metal_dispatch_sync())flush(true,"dispatch_sync");else if(++pending_>=128)flush(false,"batch_128");}}
void MetalGl::graphics_mips(const GraphicsView& v){@autoreleasepool{begin_commands(true,"mips");auto e=[commands_ blitCommandEncoder];MetalEncoderScope e_scope{e};[e generateMipmapsForTexture:metal_texture(v)];e_scope.end();graphics_pending_.push_back(v.resource);}}
void MetalGl::graphics_handoff(NativeGraphics& consumer,const GraphicsView& view){@autoreleasepool{
    // 命令執行緒代理：先清空其佇列，再交接給底層後端。
    NativeGraphics* target=&consumer;
    if(auto* proxy=dynamic_cast<MetalCommandThread*>(target)){proxy->graphics_sync();target=&proxy->inner();}
    auto* next=dynamic_cast<MetalGl*>(target);
    if(next==this)return;
    if(!next || next->device_.registryID!=device_.registryID){flush(true);return;}
    if(!handoff_event_ || handoff_value_==UINT64_MAX){flush(true);handoff_event_=[device_ newEvent];handoff_value_=0;}
    if(!handoff_event_){flush(true);return;}
    begin_commands(false);if(view.resource)graphics_pending_.push_back(view.resource);
    const auto value=++handoff_value_;[commands_ encodeSignalEvent:handoff_event_ value:value];flush(false);
    next->begin_commands(false);
    // 將待交接資源納入一般 CPU read/write 等待與 GPU 生命週期追蹤。
    // 只依賴此資源的 CPU 存取會等待事件；無關的 discard buffer 可繼續準備。
    if(view.resource)next->graphics_pending_.push_back(view.resource);
    [next->commands_ encodeWaitForEvent:handoff_event_ value:value];
}}
void MetalGl::graphics_present(const GraphicsView& v){@autoreleasepool{
    auto texture=metal_texture(v);if(texture.width!=width_ || texture.height!=height_)throw std::invalid_argument("交換鏈尺寸與共用呈現表面不符");begin_commands(true,"present");
    // 呈現保留交換鏈已編碼的 sRGB 值；格式轉換取樣不可再次解碼成線性值。
    if(texture.pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB)texture=[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    else if(texture.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB)texture=[texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    if(!texture)throw std::invalid_argument("無法建立交換鏈呈現 view");
    if(texture.pixelFormat==color_.pixelFormat){auto e=[commands_ blitCommandEncoder];MetalEncoderScope e_scope{e};[e copyFromTexture:texture sourceSlice:v.slice sourceLevel:v.level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1) toTexture:color_ destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];e_scope.end();}
    else {
        // 以 D3D9／OpenGL 既有的紋理管線做格式轉換，避免另建呈現 renderer。
        const auto saved=state_;const auto transforms=matrices_;const unsigned temporary=UINT32_MAX-1;
        Texture t;t.native=texture;t.min=t.mag=0x2600;t.s=t.t=0x812f;textures_[temporary]=t;
        state_=State{};state_.viewport={0,0,static_cast<int>(width_),static_cast<int>(height_)};state_.binding=temporary;state_.enabled.insert(0xde1);state_.texture_color_op=state_.texture_alpha_op=2;for(auto& stack:matrices_)stack={identity()};
        std::array<Vertex,6> quad{};const std::array<Vec,6> positions{{{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{-1,1,0,1},{1,-1,0,1},{1,1,0,1}}};for(unsigned i=0;i<6;++i){quad[i].position=positions[i];quad[i].color={1,1,1,1};quad[i].texcoord={(positions[i][0]+1)*.5F,(1-positions[i][1])*.5F,0,1};}
        try{draw(quad,MTLPrimitiveTypeTriangle);}catch(...){state_=saved;matrices_=transforms;textures_.erase(temporary);throw;}state_=saved;matrices_=transforms;textures_.erase(temporary);
    }
    graphics_pending_.push_back(v.resource);if(!swap())throw std::runtime_error("Metal drawable 無法呈現");graphics_capture_present(v);
}}

void MetalGl::graphics_export_frame(const GraphicsView& view,void* bytes,std::size_t length,std::uint32_t stride,std::shared_ptr<void> owner){@autoreleasepool{
    auto texture=metal_texture(view);
    auto buffer=exported_frame_.buffer(device_,bytes,length,owner);
    begin_commands();auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope scope{encoder};
    const auto width=std::max(1U,view.resource->desc.width>>view.level),height=std::max(1U,view.resource->desc.height>>view.level);
    [encoder copyFromTexture:texture sourceSlice:view.slice sourceLevel:view.level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:static_cast<NSUInteger>(stride)*height];
    scope.end();graphics_pending_.push_back(view.resource);
    // 影格鎖釋放前完成寫入；消費端讀完才允許下一幀覆寫。
    flush(true);
}}
