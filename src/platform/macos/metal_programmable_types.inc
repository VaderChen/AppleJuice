// 每批命令只保留一次資源所有權；雜湊查詢避免每次寫入都線性掃描全部 draw 的重複綁定。
bool metal_backing_generation_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_BACKING_GENERATION");return text && std::string_view(text)=="1";}();return value;}
struct MetalBufferBacking final:GraphicsObject {
    id<MTLBuffer> buffer;
    std::shared_ptr<GpuMemoryReservation> lease;
};
// D3D11 pipeline 狀態的快取鍵：以固定寬度的二進位欄位串接，取代每次繪圖的 ostringstream 格式化（DQ3 每次 DrawIndexed 約 42 µs 的主要成分）。
// 鍵只作 map 查找與串接，不被解析；相同欄位序列產生相同位元組。
// serial 為 D3D12 PSO 序號，或 D3D11 頂點著色器序號（另有佈局、混色與像素著色器序號）。
// strides 只填輸入佈局用到的槽位。
struct MetalFastPipelineKey {
    std::uint64_t serial{},layout_serial{},blend_serial{},pixel_serial{};
    std::array<std::uint32_t,8> colors{};std::uint32_t depth{},stencil{},samples{},topology{};std::array<std::uint32_t,2> alpha{};
    std::array<std::uint32_t,32> strides{};
    bool operator==(const MetalFastPipelineKey&)const=default;
};
struct MetalFastPipelineHash {
    std::size_t operator()(const MetalFastPipelineKey& key)const noexcept{
        std::uint64_t hash=key.serial*0x9E3779B97F4A7C15ULL;const auto mix=[&](std::uint64_t value){hash^=value+0x9E3779B97F4A7C15ULL+(hash<<6)+(hash>>2);};
        if(key.layout_serial){mix(key.layout_serial);mix(key.blend_serial);mix(key.pixel_serial);}
        for(const auto value:key.colors)mix(value);mix(key.depth);mix(key.stencil);mix(key.samples);mix(key.topology);mix(key.alpha[0]);mix(key.alpha[1]);
        for(const auto value:key.strides)mix(value);
        return static_cast<std::size_t>(hash);
    }
};
// 快速管線鍵命中時沿用的繪製計畫：管線、D3D 頂點槽對應的 Metal buffer index、超出 stride 改綁的額外 binding 與分層輸出旗標，
// 都只由 PSO（著色器、輸入佈局）與鍵內的附件格式／頂點跨距決定。
struct MetalFastPipeline {
    id<MTLRenderPipelineState> pipeline;std::array<int,32> slots;std::vector<std::array<std::uint32_t,3>> shifted;bool layered{};
};
// D3D12 PSO 序號對應的頂點輸入解析：VS 每個輸入（attribute）對應的輸入佈局項索引。序號決定 VS 與輸入佈局，
// 第一次繪製解析後沿用，繪製時不再逐項比對語意字串。attribute 越界／重複、佈局缺語意或槽位越界時 valid 為 false，照原路徑檢查並拋錯。
struct MetalPipelineInputs {
    std::vector<std::array<std::uint32_t,2>> inputs;bool valid{true};
};
struct MetalPipelineKey {
    std::string bytes;
    MetalPipelineKey(){bytes.reserve(512);}
    template<class T> requires (std::is_integral_v<T> || std::is_enum_v<T>) && (!std::is_same_v<T,char>)
    MetalPipelineKey& operator<<(T value){const auto raw=static_cast<std::uint64_t>(static_cast<std::int64_t>(value));bytes.append(reinterpret_cast<const char*>(&raw),sizeof raw);return *this;}
    MetalPipelineKey& operator<<(char value){bytes.push_back(value);return *this;}
    MetalPipelineKey& operator<<(const char* text){bytes.append(text);bytes.push_back('\0');return *this;}
    MetalPipelineKey& operator<<(const std::string& text){bytes.append(text);bytes.push_back('\0');return *this;}
    MetalPipelineKey& operator<<(float value){bytes.append(reinterpret_cast<const char*>(&value),sizeof value);return *this;}
    MetalPipelineKey& operator<<(double value){bytes.append(reinterpret_cast<const char*>(&value),sizeof value);return *this;}
    // 固定格式的原始位元組（呼叫端負責長度前綴）；blend 描述等逐位元組展開會讓鍵膨脹約九倍。
    MetalPipelineKey& raw(const void* data,std::size_t size){bytes.append(static_cast<const char*>(data),size);return *this;}
    const std::string& str()const{return bytes;}
};
// 繪圖管線／頂點／render pass 描述的純值版本：欄位名稱與 Metal 描述物件相同，繪圖時先填這些值並據以組管線快取鍵，
// 只在快取未命中（或需要新的 render encoder）時才建立 Metal 描述物件；每次繪製不再配置與釋放數個 Objective-C 物件。
// 預設值與 Metal 描述物件的預設值相同。
struct MetalVertexSpec {
    struct Attribute {MTLVertexFormat format{MTLVertexFormatInvalid};NSUInteger offset{},bufferIndex{};};
    struct Layout {NSUInteger stride{};MTLVertexStepFunction stepFunction{MTLVertexStepFunctionPerVertex};NSUInteger stepRate{1};};
    std::array<Attribute,31> attributes;std::array<Layout,31> layouts;
    MTLVertexDescriptor* make()const{
        auto vertex=[MTLVertexDescriptor vertexDescriptor];
        for(unsigned i=0;i<attributes.size();++i)if(const auto& a=attributes[i];a.format!=MTLVertexFormatInvalid || a.offset || a.bufferIndex){auto out=vertex.attributes[i];out.format=a.format;out.offset=a.offset;out.bufferIndex=a.bufferIndex;}
        for(unsigned i=0;i<layouts.size();++i)if(const auto& l=layouts[i];l.stride || l.stepFunction!=MTLVertexStepFunctionPerVertex || l.stepRate!=1){auto out=vertex.layouts[i];out.stride=l.stride;out.stepFunction=l.stepFunction;out.stepRate=l.stepRate;}
        return vertex;
    }
};
struct MetalPipelineSpec {
    struct Color {
        MTLPixelFormat pixelFormat{MTLPixelFormatInvalid};BOOL blendingEnabled{NO};
        MTLBlendFactor sourceRGBBlendFactor{MTLBlendFactorOne},destinationRGBBlendFactor{MTLBlendFactorZero};MTLBlendOperation rgbBlendOperation{MTLBlendOperationAdd};
        MTLBlendFactor sourceAlphaBlendFactor{MTLBlendFactorOne},destinationAlphaBlendFactor{MTLBlendFactorZero};MTLBlendOperation alphaBlendOperation{MTLBlendOperationAdd};
        MTLColorWriteMask writeMask{MTLColorWriteMaskAll};
    };
    id<MTLFunction> vertexFunction,fragmentFunction;NSUInteger sampleCount{1};BOOL alphaToCoverageEnabled{NO};
    MTLPrimitiveTopologyClass inputPrimitiveTopology{MTLPrimitiveTopologyClassUnspecified};
    std::array<Color,8> colorAttachments;MTLPixelFormat depthAttachmentPixelFormat{MTLPixelFormatInvalid},stencilAttachmentPixelFormat{MTLPixelFormatInvalid};
    // 頂點描述：vertexDescriptor 指向本次繪製的純值描述；細分改用其管線的描述物件（vertexDescriptorObject）。
    const MetalVertexSpec* vertexDescriptor{};MTLVertexDescriptor* vertexDescriptorObject{};
    bool tessellation{};MTLTessellationPartitionMode tessellationPartitionMode{MTLTessellationPartitionModePow2};MTLWinding tessellationOutputWindingOrder{MTLWindingClockwise};
    NSUInteger maxTessellationFactor{16};MTLTessellationFactorFormat tessellationFactorFormat{MTLTessellationFactorFormatHalf};
    MTLTessellationFactorStepFunction tessellationFactorStepFunction{MTLTessellationFactorStepFunctionConstant};MTLTessellationControlPointIndexType tessellationControlPointIndexType{MTLTessellationControlPointIndexTypeNone};
    MTLRenderPipelineDescriptor* make()const{
        auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=vertexFunction;desc.fragmentFunction=fragmentFunction;desc.sampleCount=sampleCount;
        desc.alphaToCoverageEnabled=alphaToCoverageEnabled;desc.inputPrimitiveTopology=inputPrimitiveTopology;
        for(unsigned i=0;i<colorAttachments.size();++i){
            const auto& c=colorAttachments[i];auto a=desc.colorAttachments[i];a.pixelFormat=c.pixelFormat;a.blendingEnabled=c.blendingEnabled;
            a.sourceRGBBlendFactor=c.sourceRGBBlendFactor;a.destinationRGBBlendFactor=c.destinationRGBBlendFactor;a.rgbBlendOperation=c.rgbBlendOperation;
            a.sourceAlphaBlendFactor=c.sourceAlphaBlendFactor;a.destinationAlphaBlendFactor=c.destinationAlphaBlendFactor;a.alphaBlendOperation=c.alphaBlendOperation;a.writeMask=c.writeMask;
        }
        desc.depthAttachmentPixelFormat=depthAttachmentPixelFormat;desc.stencilAttachmentPixelFormat=stencilAttachmentPixelFormat;
        if(vertexDescriptor)desc.vertexDescriptor=vertexDescriptor->make();else if(vertexDescriptorObject)desc.vertexDescriptor=vertexDescriptorObject;
        if(tessellation){
            desc.tessellationPartitionMode=tessellationPartitionMode;desc.tessellationOutputWindingOrder=tessellationOutputWindingOrder;desc.maxTessellationFactor=maxTessellationFactor;
            desc.tessellationFactorFormat=tessellationFactorFormat;desc.tessellationFactorStepFunction=tessellationFactorStepFunction;desc.tessellationControlPointIndexType=tessellationControlPointIndexType;
        }
        return desc;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
    // mesh 管線（GS 的 mesh 路徑）：附件、混色與取樣設定與一般管線相同，頂點與細分設定不適用。
    MTLMeshRenderPipelineDescriptor* make_mesh(id<MTLFunction> object,id<MTLFunction> mesh,NSUInteger per_mesh,NSUInteger payload)const{
        auto desc=[MTLMeshRenderPipelineDescriptor new];desc.objectFunction=object;desc.meshFunction=mesh;desc.fragmentFunction=fragmentFunction;
        desc.rasterSampleCount=sampleCount;desc.alphaToCoverageEnabled=alphaToCoverageEnabled;
        for(unsigned i=0;i<colorAttachments.size();++i){
            const auto& c=colorAttachments[i];auto a=desc.colorAttachments[i];a.pixelFormat=c.pixelFormat;a.blendingEnabled=c.blendingEnabled;
            a.sourceRGBBlendFactor=c.sourceRGBBlendFactor;a.destinationRGBBlendFactor=c.destinationRGBBlendFactor;a.rgbBlendOperation=c.rgbBlendOperation;
            a.sourceAlphaBlendFactor=c.sourceAlphaBlendFactor;a.destinationAlphaBlendFactor=c.destinationAlphaBlendFactor;a.alphaBlendOperation=c.alphaBlendOperation;a.writeMask=c.writeMask;
        }
        desc.depthAttachmentPixelFormat=depthAttachmentPixelFormat;desc.stencilAttachmentPixelFormat=stencilAttachmentPixelFormat;
        desc.payloadMemoryLength=payload;desc.maxTotalThreadsPerObjectThreadgroup=1;desc.maxTotalThreadsPerMeshThreadgroup=per_mesh;desc.maxTotalThreadgroupsPerMeshGrid=1024;
        return desc;
    }
#pragma clang diagnostic pop
};
// 深度／stencil 狀態的純值版本：未設定的 stencil 面與 Metal 預設相同（Always、Keep、遮罩全開），快取鍵與描述物件版本一致。
struct MetalDepthSpec {
    struct Stencil {
        MTLCompareFunction stencilCompareFunction{MTLCompareFunctionAlways};
        MTLStencilOperation stencilFailureOperation{MTLStencilOperationKeep},depthFailureOperation{MTLStencilOperationKeep},depthStencilPassOperation{MTLStencilOperationKeep};
        std::uint32_t readMask{UINT32_MAX},writeMask{UINT32_MAX};bool set{};
    };
    MTLCompareFunction depthCompareFunction{MTLCompareFunctionAlways};BOOL depthWriteEnabled{NO};Stencil frontFaceStencil,backFaceStencil;
    void stencil(bool back,MTLCompareFunction compare,MTLStencilOperation fail,MTLStencilOperation depth_fail,MTLStencilOperation pass,std::uint32_t read_mask,std::uint32_t write_mask){
        auto& face=back?backFaceStencil:frontFaceStencil;face={compare,fail,depth_fail,pass,read_mask,write_mask,true};
    }
    MTLDepthStencilDescriptor* make()const{
        auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=depthCompareFunction;depth.depthWriteEnabled=depthWriteEnabled;
        for(unsigned i=0;i<2;++i)if(const auto& f=i?backFaceStencil:frontFaceStencil;f.set){
            auto stencil=make_metal_stencil(f.stencilCompareFunction,f.stencilFailureOperation,f.depthFailureOperation,f.depthStencilPassOperation,f.readMask,f.writeMask);
            if(i)depth.backFaceStencil=stencil;else depth.frontFaceStencil=stencil;
        }
        return depth;
    }
};
struct MetalPassSpec {
    struct Attachment {id<MTLTexture> texture;NSUInteger level{},slice{},depthPlane{};MTLLoadAction loadAction{MTLLoadActionDontCare};MTLStoreAction storeAction{MTLStoreActionStore};
        // loadAction 為 Clear 時的清除值（延後清除併入 pass 時設定）。
        MTLClearColor clearColor{0,0,0,0};double clearDepth{1};std::uint32_t clearStencil{};};
    std::array<Attachment,8> colorAttachments;Attachment depthAttachment,stencilAttachment;
    id<MTLBuffer> visibilityResultBuffer;NSUInteger renderTargetArrayLength{},renderTargetWidth{},renderTargetHeight{},defaultRasterSampleCount{};
    std::array<MTLSamplePosition,8> samplePositions{};NSUInteger samplePositionCount{};
    void setSamplePositions(const MTLSamplePosition* positions,NSUInteger count){samplePositionCount=std::min<NSUInteger>(count,samplePositions.size());std::copy_n(positions,samplePositionCount,samplePositions.begin());}
    MTLRenderPassDescriptor* make()const{
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        const auto copy=[](MTLRenderPassAttachmentDescriptor* out,const Attachment& a){out.texture=a.texture;out.level=a.level;out.slice=a.slice;out.depthPlane=a.depthPlane;out.loadAction=a.loadAction;out.storeAction=a.storeAction;};
        for(unsigned i=0;i<colorAttachments.size();++i)if(colorAttachments[i].texture){copy(pass.colorAttachments[i],colorAttachments[i]);pass.colorAttachments[i].clearColor=colorAttachments[i].clearColor;}
        if(depthAttachment.texture){copy(pass.depthAttachment,depthAttachment);pass.depthAttachment.clearDepth=depthAttachment.clearDepth;}
        if(stencilAttachment.texture){copy(pass.stencilAttachment,stencilAttachment);pass.stencilAttachment.clearStencil=stencilAttachment.clearStencil;}
        pass.visibilityResultBuffer=visibilityResultBuffer;
        if(renderTargetArrayLength)pass.renderTargetArrayLength=renderTargetArrayLength;
        if(renderTargetWidth)pass.renderTargetWidth=renderTargetWidth;if(renderTargetHeight)pass.renderTargetHeight=renderTargetHeight;
        if(defaultRasterSampleCount)pass.defaultRasterSampleCount=defaultRasterSampleCount;
        if(samplePositionCount)[pass setSamplePositions:samplePositions.data() count:samplePositionCount];
        return pass;
    }
};
// DISCARD 更換 backing 時回收舊 buffer：舊 buffer 在 GPU 用完（flight 退役）後進入池，下一次同長度的更換直接取用，
// 省掉 newBufferWithLength＋整段清零＋額度保留。池上限 64 MiB，超出即照常釋放。
// 池只保留 MTLBuffer，不保留 GPU 額度：入池即釋放額度（否則 Steam GPU 程序 256 MiB 額度會被池吃掉 64 MiB），
// 取用時重新保留，失敗就改走一般配置。X64ARM_METAL_DISCARD_POOL=0 關閉就地寫入與回收池，回到每次更換新 buffer。
bool metal_discard_pool_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_DISCARD_POOL");return !text || std::string_view(text)!="0";}();return value;}
// 取用／歸還可能同時發生在協調者（CreateBuffer／Release 釋放身分）與 Metal 命令執行緒（在途退役釋放最後一個持有者），以 mutex 保護。
struct MetalBufferPool {
    std::mutex mutex;
    std::multimap<std::size_t,id<MTLBuffer>> free;std::size_t bytes{};std::uint64_t hits{},misses{},puts{},drops{};
    const std::size_t limit;
    explicit MetalBufferPool(std::size_t capacity=std::size_t{64}<<20):limit(capacity){}
    id<MTLBuffer> take(std::size_t length){const std::lock_guard lock(mutex);auto it=free.find(length);if(it==free.end()){++misses;return nil;}auto buffer=it->second;free.erase(it);bytes-=length;++hits;return buffer;}
    void put(id<MTLBuffer> buffer){
        const auto length=static_cast<std::size_t>(buffer.length);const std::lock_guard lock(mutex);
        if(!length || bytes+length>limit){++drops;return;}
        free.emplace(length,buffer);bytes+=length;++puts;
    }
    std::string statistics(){const std::lock_guard lock(mutex);return " metal_buffer_pool_hits="+std::to_string(hits)+" metal_buffer_pool_misses="+std::to_string(misses)+" metal_buffer_pool_puts="+std::to_string(puts)+" metal_buffer_pool_drops="+std::to_string(drops)+" metal_buffer_pool_bytes="+std::to_string(bytes)+" metal_buffer_pool_entries="+std::to_string(free.size());}
};
// 在途期間保活、批次完成時歸還池的暫存 buffer（幾何階段的輸入／輸出／計數）。
struct MetalPooledBuffer final:GraphicsObject {
    id<MTLBuffer> buffer;std::shared_ptr<MetalBufferPool> pool;
    ~MetalPooledBuffer() override {if(pool && buffer)pool->put(buffer);}
};
// CreateBuffer 建立的緩衝釋放後也交回同一個池，下一次同長度（頁對齊後）的 CreateBuffer 直接取用並清零，省掉 IOGPU 配置與釋放
// （雙人成行每幀約 40 次 CreateBuffer／Release，協調者約 5% 時間在 IOGPUResourceCreate／Finalize）。X64ARM_METAL_CREATE_POOL=0 關閉。
bool metal_create_pool_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_CREATE_POOL");return (!text || std::string_view(text)!="0") && metal_discard_pool_enabled() && !metal_backing_generation_enabled();}();return value;}
class MetalPendingObjects {
    static bool indexed(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_RESOURCE_INDEX");return !text || std::string_view(text)!="0";}();return value;}
    static std::uint64_t next_batch() noexcept {static std::atomic<std::uint64_t> next{0};return next.fetch_add(1,std::memory_order_relaxed)+1;}
    std::vector<std::shared_ptr<GraphicsObject>> objects_;
    // 每個批次（清單實例；清空或被移走後換新）有唯一編號，加入時寫入物件的 pending_stamp：同批重複加入只比對編號，
    // 不查雜湊表、不配置節點（每次繪製都重複加入同一批目標、著色器與常數緩衝）。不同後端同時加入同一物件時
    // 編號互相覆寫，最多造成重複項目，不會漏加。contains 很少呼叫（等資源閒置、更換 backing），第一次查詢時才建立索引。
    std::uint64_t batch_{next_batch()};
    mutable std::unordered_set<const GraphicsObject*> index_;mutable bool index_valid_{};
    template<class T> void append(const std::shared_ptr<T>& object){
        GraphicsObject* raw=object.get();
        if(!indexed()){objects_.push_back(object);return;}
        if(!raw)return;
        std::atomic_ref<std::uint64_t> stamp(raw->pending_stamp);
        if(stamp.load(std::memory_order_relaxed)==batch_)return;
        objects_.push_back(object);stamp.store(batch_,std::memory_order_relaxed);
        if(index_valid_)index_.insert(raw);
    }
public:
    MetalPendingObjects()=default;
    MetalPendingObjects(const MetalPendingObjects&)=delete;MetalPendingObjects& operator=(const MetalPendingObjects&)=delete;
    MetalPendingObjects(MetalPendingObjects&& other) noexcept {*this=std::move(other);}
    MetalPendingObjects& operator=(MetalPendingObjects&& other) noexcept {
        objects_=std::move(other.objects_);index_=std::move(other.index_);index_valid_=other.index_valid_;batch_=other.batch_;tables=other.tables;
        other.objects_.clear();other.index_.clear();other.index_valid_=false;other.batch_=next_batch();other.tables=false;return *this;
    }
    template<class T> void push_back(const std::shared_ptr<T>& object);
    auto end(){return objects_.end();}
    template<class Iterator> void insert(std::vector<std::shared_ptr<GraphicsObject>>::iterator,Iterator first,Iterator last){for(;first!=last;++first)push_back(*first);}
    bool contains(const GraphicsObject* object)const{
        if(!indexed())return std::any_of(objects_.begin(),objects_.end(),[&](const auto& value){return value.get()==object;});
        if(!index_valid_){index_.clear();index_.reserve(objects_.size());for(const auto& value:objects_)index_.insert(value.get());index_valid_=true;}
        return index_.contains(object);
    }
    // 本批次綁定過 D3D12 描述元表：表內資源不在清單中，曾被表引用的資源一律視為可能使用中。
    bool tables{};
    void clear(){objects_.clear();index_.clear();index_valid_=false;batch_=next_batch();tables=false;}
};
// 與固定管線共用 MetalGl；本檔只提供資源與可程式化管線的共用資料。
struct MetalResource final:GraphicsResource {
    // 零複製緩衝的宿主記憶體擁有者：宣告在 buffer 之前，解構時最後釋放（buffer 先放掉對記憶體的引用）。
    std::shared_ptr<void> host_owner;
    id<MTLBuffer> buffer;id<MTLTexture> texture;
    std::shared_ptr<GpuMemoryReservation> lease;
    std::shared_ptr<MetalBufferBacking> backing;
    // 自上次全部等待以來，這個緩衝是否可能有 GPU 複製寫入尚未完成（CPU 直接寫入不算）。
    // CPU 需要讀取內容的路徑（幾何著色器的索引範圍）只在此時才等 GPU；UAV／串流輸出目標另依 bind 判斷。
    bool gpu_written{};
    // 最後一次被 GPU 寫入（上傳、複製）時的主命令緩衝序號＋1；等於目前序號＋1 表示寫入就在尚未提交的主命令緩衝內。
    // 細分因子的 compute 只有在它讀的緩衝都沒有這種寫入時，才可以排到主命令緩衝之前（見 MetalGl::side_commands_）。
    std::uint64_t gpu_write_mark{};
    // 被 DISCARD 更換下來的舊 backing：最後一個持有者（flight 或更換點）釋放時交回池。
    std::shared_ptr<MetalBufferPool> recycle;
    // 被 D3D12 描述元表引用時加入的 residency set；資源釋放時一併移除。
    id residency;
    ~MetalResource() override {
        if(residency){if(@available(macOS 15.0,*))[(id<MTLResidencySet>)residency removeAllocation:(id<MTLAllocation>)(texture?(id)texture:(id)buffer)];}
        if(recycle && buffer && !texture)recycle->put(buffer);
    }
    // shader 用的 texture view／typed buffer view：每次繪製重建約占綁定成本四成。鍵含目前 backing 的位址，
    // backing 更換（DISCARD、重新配置）後整批丟棄；同一 backing 物件上的 view 內容與 backing 共用，可跨命令重用。
    struct ShaderView {const void* base{};std::array<std::uint32_t,6> key{};id<MTLTexture> view;};
    std::vector<ShaderView> shader_views;
    template<class Make> id<MTLTexture> shader_view(const void* base,const std::array<std::uint32_t,6>& key,Make&& make){
        static const bool enabled=[] {const auto* text=std::getenv("X64ARM_METAL_VIEW_CACHE");return !text || std::string_view(text)!="0";}();
        if(!enabled)return make();
        for(const auto& entry:shader_views)if(entry.base==base && entry.key==key)return entry.view;
        std::erase_if(shader_views,[&](const auto& entry){return entry.base!=base;});
        auto view=make();
        if(shader_views.size()>=16)shader_views.erase(shader_views.begin());
        shader_views.push_back({base,key,view});return view;
    }
    // 常數 buffer 不允許 GPU 寫入綁定；保存最後排程的 CPU 內容，GPU copy 目的端必須失效。
    std::vector<std::uint8_t> constant_bytes;
    bool constant_bytes_valid{};
    static bool constant_snapshot_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_CONSTANT_UPLOAD_SNAPSHOT");return !text || std::string_view(text)=="1";}();return value;}
    bool constant_snapshot_eligible()const{return constant_snapshot_enabled() && buffer && desc.dimension==1 && desc.bind==4 && desc.width<=65536;}
    bool constant_bytes_match(std::span<const std::uint8_t> data,std::uint32_t offset)const{
        return constant_snapshot_eligible() && constant_bytes_valid && offset<=constant_bytes.size() &&
            data.size()<=constant_bytes.size()-offset && std::equal(data.begin(),data.end(),constant_bytes.begin()+offset);
    }
    void remember_constant_bytes(std::span<const std::uint8_t> data,std::uint32_t offset){
        if(!constant_snapshot_eligible())return;
        if(!constant_bytes_valid){
            if(offset || data.size()!=desc.width)return;
            try{constant_bytes.assign(data.begin(),data.end());constant_bytes_valid=true;}catch(const std::bad_alloc&){constant_bytes_valid=false;}
        }else std::copy(data.begin(),data.end(),constant_bytes.begin()+offset);
    }
};
template<class T> void MetalPendingObjects::push_back(const std::shared_ptr<T>& object){
    // 捕捉實際編碼時的不可變 backing，避免後續更換 buffer 後依邏輯身分誤判使用範圍。
    // 先保存 backing；即使後續配置節點失敗，已編碼工作的實體額度仍有所有者。
    if(metal_backing_generation_enabled())if(auto* resource=dynamic_cast<MetalResource*>(static_cast<GraphicsObject*>(object.get()));resource && resource->backing)append(resource->backing);
    append(object);
}
const GraphicsObject* metal_resource_use_identity(const std::shared_ptr<GraphicsResource>& resource){
    if(metal_backing_generation_enabled())if(auto* native=dynamic_cast<MetalResource*>(resource.get());native && native->backing)return native->backing.get();
    return resource.get();
}
// 批次是否使用資源：明確綁定，或批次用過描述元表且資源曾被描述元表引用（residency set 成員）。
bool metal_batch_uses(const MetalPendingObjects& objects,const std::shared_ptr<GraphicsResource>& resource){
    if(objects.contains(metal_resource_use_identity(resource)))return true;
    if(!objects.tables)return false;
    const auto* native=dynamic_cast<const MetalResource*>(resource.get());return native && native->residency;
}
struct MetalVarying {
    std::string name,semantic;
    unsigned index{},components{},type{},system{};
    unsigned clip_offset{},clip_count{};
};
// 非同步編譯中的 Metal 函式庫：完成回呼只寫入這個共享狀態；第一次用到著色器時（命令執行緒的繪製／運算）才等待。
struct MetalPendingLibrary {
    std::mutex mutex;std::condition_variable ready;bool done{};
    id<MTLLibrary> library;std::string error;NSString* entry;
};
// 診斷：X64ARM_METAL_LABELS=1 時以著色器內容雜湊標記 encoder（Metal System Trace 可依標籤彙總 GPU 時間）。
inline bool metal_labels() noexcept {static const bool enabled=std::getenv("X64ARM_METAL_LABELS")!=nullptr;return enabled;}
// 診斷 X64ARM_METAL_GPU_PROFILE=<檔案>（須同時設 X64ARM_METAL_LABELS=1）：每個 encoder 一個命令緩衝，依標籤累計 GPU 時間，每 5 秒附加一份累計表到檔案。
// 診斷 X64ARM_METAL_PASS_TRACE=<檔案>：依序記錄 encoder（X 換 pass 細項、R 繪製、C 運算、E 結束原因、P 呈現），用來分析哪些 pass 能合併。
// X64ARM_METAL_PASS_TRACE_FROM=<第幾次呈現> 之後才記錄，上限約 40 萬行。
inline unsigned& metal_pass_trace_frames() noexcept {static unsigned value=0;return value;}
inline std::FILE* metal_pass_trace() noexcept {
    static std::FILE* const file=[]()->std::FILE* {const auto* path=std::getenv("X64ARM_METAL_PASS_TRACE");return path?std::fopen(path,"w"):nullptr;}();
    if(!file)return nullptr;
    static const unsigned from=[] {const auto* text=std::getenv("X64ARM_METAL_PASS_TRACE_FROM");return text?static_cast<unsigned>(std::strtoul(text,nullptr,10)):0U;}();
    static unsigned lines=0;
    return metal_pass_trace_frames()>=from && ++lines<400000?file:nullptr;
}
inline const char* metal_gpu_profile_path() noexcept {static const char* path=std::getenv("X64ARM_METAL_GPU_PROFILE");return path;}
inline bool metal_gpu_profile() noexcept {return metal_gpu_profile_path()!=nullptr && metal_labels();}
// 非同步剖析（X64ARM_METAL_GPU_PROFILE_ASYNC=1，搭配 X64ARM_METAL_GPU_PROFILE）：不逐 encoder 提交與等待，命令緩衝內每個繪製／運算
// encoder 各取階段邊界時間戳，完成時依 encoder 標籤歸屬。FPS 接近一般執行，可剖析長場景；TBDR 的頂點與片段跨 pass 重疊，總和可能大於實際。
inline bool metal_gpu_profile_async() noexcept {static const bool value=std::getenv("X64ARM_METAL_GPU_PROFILE_ASYNC")!=nullptr;return value && metal_gpu_profile();}
inline void metal_gpu_profile_add(const std::string& key,double seconds,std::uint64_t vertices,std::uint64_t vertex_ticks,std::uint64_t fragment_ticks){
    struct Entry {double seconds{};std::uint64_t count{},vertices{},vertex_ticks{},fragment_ticks{};};
    static std::mutex mutex;static std::map<std::string,Entry> totals;static double last_dump=0;
    const std::lock_guard lock(mutex);auto& entry=totals[key];entry.seconds+=seconds;++entry.count;entry.vertices+=vertices;entry.vertex_ticks+=vertex_ticks;entry.fragment_ticks+=fragment_ticks;
    const double now=CFAbsoluteTimeGetCurrent();if(now-last_dump<5)return;last_dump=now;
    if(auto* file=std::fopen(metal_gpu_profile_path(),"a")){std::fprintf(file,"=== %.3f\n",now+kCFAbsoluteTimeIntervalSince1970);for(const auto& [name,value]:totals)std::fprintf(file,"%.6f %llu %llu %llu %llu %s\n",value.seconds,static_cast<unsigned long long>(value.count),static_cast<unsigned long long>(value.vertices),static_cast<unsigned long long>(value.vertex_ticks),static_cast<unsigned long long>(value.fragment_ticks),name.c_str());std::fclose(file);}
}
struct MetalShader final:GraphicsShader {
    std::vector<std::uint8_t> bytecode;
    std::shared_ptr<MetalPendingLibrary> pending;
    std::array<unsigned,4> tessellation{}; // 分割、繞序、最大係數、每群 patch 數。
    id<MTLFunction> function,dual_source_function,non_point_function;id<MTLComputePipelineState> compute;
    std::map<unsigned,id<MTLFunction>> alpha_functions;
    // 與前一階段輸出對齊後的 PS 變體（鍵：缺少或改型別的輸入清單），見 metal_link_fragment。
    std::map<std::string,id<MTLFunction>> linked_functions;
    // 依前一階段（序號×2＋是否為 GS）快取對齊結果：值為實際使用的 PS 函式，nil 表示不需要變體。原始碼建立後不變，
    // 不必每次繪製都以 regex 重新解析兩邊的結構（Fire Strike 命令執行緒原本約兩成時間耗在這裡）。
    std::unordered_map<std::uint64_t,id<MTLFunction>> linked_by_producer;
    std::string source,geometry_original;
    std::uint64_t serial{};
    std::uint64_t content_key{}; // 著色器位元組內容雜湊（快取鍵）；X64ARM_METAL_LABELS=1 時作為 encoder 標籤。
    bool writes_depth{};
    bool non_point_checked{};
    signed char uav_write_only{-1}; // 首次需要時由 bytecode 判定（見 metal_dxbc_uav_write_only）。
    std::vector<MetalVarying> input_varyings,output_varyings;
    unsigned geometry_input{},geometry_output{},geometry_max{},geometry_instances{1};
    unsigned buffer_views_slot{UINT32_MAX};
};
// 取得非同步編譯的結果：建立 function（運算著色器再建立 compute pipeline）。失敗時沿用同步路徑的錯誤訊息。
void metal_resolve_shader(id<MTLDevice> device,MetalShader& shader){
    if(!shader.pending)return;
    const auto pending=shader.pending;
    {std::unique_lock lock(pending->mutex);pending->ready.wait(lock,[&]{return pending->done;});}
    if(!pending->library)throw std::invalid_argument("Metal 著色器編譯失敗："+pending->error+"\n"+shader.source);
    shader.function=[pending->library newFunctionWithName:pending->entry];if(!shader.function)throw std::invalid_argument("Metal 著色器沒有入口");
    if(shader.stage==5){NSError* error=nil;shader.compute=[device newComputePipelineStateWithFunction:shader.function error:&error];
        if(!shader.compute)throw std::invalid_argument("Metal compute pipeline 建立失敗："+std::string(error.localizedDescription.UTF8String));}
    shader.pending.reset();
}
struct MetalTessellationPipeline {
    std::array<std::shared_ptr<MetalShader>,3> stages;
    id<MTLComputePipelineState> compute;
    id<MTLFunction> domain;
    MTLVertexDescriptor* descriptor;
    unsigned control_slot{},patch_slot{},factor_slot{},info_slot{},vertex_id_slot{};
    unsigned domain_control_slot{30},domain_patch_slot{29};
    unsigned control_stride{},patch_stride{},factor_stride{},control_count{},input_count{},group_patches{};
    unsigned raw_factor_slot{UINT32_MAX},domain_raw_factor_slot{UINT32_MAX},raw_factor_stride{};
    std::array<unsigned,4> settings{};
    bool captured{};
    // direct：逐頂點輸入由 GPU 直接讀原始緩衝（索引走 stage-in 索引步進），不經 CPU 收集；uses_vertex_id：VS 讀 SV_VertexID，要由 CPU 準備編號。
    bool direct{},uses_vertex_id{true};unsigned index_slot{};
    std::map<std::string,std::array<unsigned,2>> control_fields,patch_fields;
};
struct MetalTessellationDraw {
    std::shared_ptr<MetalTessellationPipeline> pipeline;
    id<MTLBuffer> controls,patches,factors,raw_factors;
    unsigned count{};
    // 暫存緩衝的持有者：細分輸出要活到使用它的 render pass 所屬批次完成才能回到池裡。
    std::vector<std::shared_ptr<GraphicsObject>> holders;
};
struct MetalGeometryPipeline {
    std::shared_ptr<GraphicsShader> vertex,geometry;
    id<MTLRenderPipelineState> capture;
    id<MTLComputePipelineState> expand;
    id<MTLFunction> draw;
    unsigned capture_buffer{},capture_params{},input_buffer{},index_buffer{},output_buffer{},params_buffer{},counts_buffer{};
};
struct MetalGeometryDraw {
    id<MTLFunction> function;id<MTLBuffer> vertices;
    unsigned count{};MTLPrimitiveType primitive{};
};
// GS 的 mesh 路徑（見 graphics_geometry_mesh）：每個 mesh 執行緒群組 per_mesh 個 GS 呼叫，每個 object 執行緒群組至多 1024 個 mesh 群組。
// limit：單次繪製可處理的 GS 呼叫上限（不低於 compute 展開路徑的暫存上限）。
struct MetalGeometryMesh {
    id<MTLFunction> object,mesh;unsigned per_mesh{},per_object{};std::uint32_t limit{};
    // vertex：VS 併入 mesh 函式；vertex_offsets：VS 的 buffer／texture／sampler 槽位在 mesh 綁定表中的位移。
    bool vertex{};std::array<unsigned,3> vertex_offsets{};
};
struct MetalSampler final:GraphicsSampler {id<MTLSamplerState> sampler;};
MTLPrimitiveTopologyClass metal_primitive_class(std::uint32_t topology){
    switch(topology){case 1:return MTLPrimitiveTopologyClassPoint;case 2:case 3:case 10:case 11:return MTLPrimitiveTopologyClassLine;
    case 4:case 5:case 12:case 13:return MTLPrimitiveTopologyClassTriangle;default:throw std::invalid_argument("Metal topology 尚未映射");}
}
// 著色器轉譯後處理的屬性掃描：每個著色器都要跑，std::regex 在讀檔時段占主執行緒約 6%，改為直接掃描。
// 回傳 "[[" 之後略過空白、比對 word、再略過空白的位置；不符回傳 npos。
std::size_t metal_attribute_word(const std::string& text,std::size_t open,std::string_view word){
    auto at=open+2;while(at<text.size() && std::isspace(static_cast<unsigned char>(text[at])))++at;
    if(text.compare(at,word.size(),word)!=0)return std::string::npos;
    at+=word.size();while(at<text.size() && std::isspace(static_cast<unsigned char>(text[at])))++at;
    return at;
}
// 是否含 [[ depth( 形式的輸出屬性（[[\s*depth\s*\(）。
bool metal_writes_depth(const std::string& source){
    for(auto open=source.find("[[");open!=std::string::npos;open=source.find("[[",open+2)){
        const auto at=metal_attribute_word(source,open,"depth");if(at!=std::string::npos && at<source.size() && source[at]=='(')return true;
    }
    return false;
}
std::string metal_nonpoint_source(const std::string& source){
    // 線／三角形不使用點大小；保留欄位值供階段連接，移除 Metal 的點圖元限制（[[\s*point_size\s*]]）。
    std::string output;output.reserve(source.size()+64);std::size_t copied=0;
    for(auto open=source.find("[[");open!=std::string::npos;open=source.find("[[",open+2)){
        const auto at=metal_attribute_word(source,open,"point_size");
        if(at==std::string::npos || source.compare(at,2,"]]")!=0)continue;
        output.append(source,copied,open-copied);output+="[[user(x64arm_point_size)]]";copied=at+2;open=at;
    }
    if(!copied)return source;
    output.append(source,copied,std::string::npos);return output;
}
// 移除同一行內的所有 [[...]] 屬性（等同 ECMAScript 的 \[\[.*?\]\]，. 不跨行）。
std::string metal_strip_attributes(const std::string& source){
    std::string output;output.reserve(source.size());std::size_t at=0;
    while(at<source.size()){
        const auto open=source.find("[[",at);if(open==std::string::npos){output.append(source,at,std::string::npos);break;}
        const auto line_end=source.find('\n',open);const auto close=source.find("]]",open+2);
        if(close==std::string::npos || (line_end!=std::string::npos && close>line_end)){output.append(source,at,open+2-at);at=open+2;continue;}
        output.append(source,at,open-at);at=close+2;
    }
    return output;
}
// sampler 宣告與 binding（\bsampler\s+(識別字)\s*\[\[\s*sampler\s*\(\s*(\d+)\s*\)）。
std::map<std::string,std::uint32_t> metal_sampler_bindings(const std::string& source){
    std::map<std::string,std::uint32_t> result;
    const auto identifier=[](char c){return std::isalnum(static_cast<unsigned char>(c)) || c=='_';};
    for(auto at=source.find("sampler");at!=std::string::npos;at=source.find("sampler",at+7)){
        if(at && identifier(source[at-1]))continue;
        auto cursor=at+7;if(cursor>=source.size() || !std::isspace(static_cast<unsigned char>(source[cursor])))continue;
        while(cursor<source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))++cursor;
        if(cursor>=source.size() || !(std::isalpha(static_cast<unsigned char>(source[cursor])) || source[cursor]=='_'))continue;
        const auto name_begin=cursor;while(cursor<source.size() && identifier(source[cursor]))++cursor;
        const auto name=source.substr(name_begin,cursor-name_begin);
        while(cursor<source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))++cursor;
        if(source.compare(cursor,2,"[[")!=0)continue;
        auto inner=metal_attribute_word(source,cursor,"sampler");if(inner==std::string::npos || inner>=source.size() || source[inner]!='(')continue;
        ++inner;while(inner<source.size() && std::isspace(static_cast<unsigned char>(source[inner])))++inner;
        const auto digits=inner;while(inner<source.size() && std::isdigit(static_cast<unsigned char>(source[inner])))++inner;
        if(inner==digits)continue;const auto value=source.substr(digits,inner-digits);
        while(inner<source.size() && std::isspace(static_cast<unsigned char>(source[inner])))++inner;
        if(inner>=source.size() || source[inner]!=')')continue;
        result[name]=static_cast<std::uint32_t>(std::stoul(value));
    }
    return result;
}
MTLPixelFormat metal_format(std::uint32_t f){
    switch(f){
    case 1:case 2:return MTLPixelFormatRGBA32Float;case 3:return MTLPixelFormatRGBA32Uint;case 4:return MTLPixelFormatRGBA32Sint;
    case 5:case 6:return MTLPixelFormatRGBA32Float;case 7:return MTLPixelFormatRGBA32Uint;case 8:return MTLPixelFormatRGBA32Sint;
    case 9:case 10:return MTLPixelFormatRGBA16Float;case 11:return MTLPixelFormatRGBA16Unorm;case 12:return MTLPixelFormatRGBA16Uint;case 13:return MTLPixelFormatRGBA16Snorm;case 14:return MTLPixelFormatRGBA16Sint;
    case 15:case 16:return MTLPixelFormatRG32Float;case 17:return MTLPixelFormatRG32Uint;case 18:return MTLPixelFormatRG32Sint;
    case 19:case 20:case 21:return MTLPixelFormatDepth32Float_Stencil8;case 22:return MTLPixelFormatX32_Stencil8;
    case 23:case 24:return MTLPixelFormatRGB10A2Unorm;case 25:return MTLPixelFormatRGB10A2Uint;case 26:return MTLPixelFormatRG11B10Float;case 67:return MTLPixelFormatRGB9E5Float;
    case 27:case 28:return MTLPixelFormatRGBA8Unorm;case 29:return MTLPixelFormatRGBA8Unorm_sRGB;case 30:return MTLPixelFormatRGBA8Uint;case 31:return MTLPixelFormatRGBA8Snorm;case 32:return MTLPixelFormatRGBA8Sint;
    case 33:case 34:return MTLPixelFormatRG16Float;case 35:return MTLPixelFormatRG16Unorm;case 36:return MTLPixelFormatRG16Uint;case 37:return MTLPixelFormatRG16Snorm;case 38:return MTLPixelFormatRG16Sint;
    case 39:case 41:return MTLPixelFormatR32Float;case 40:return MTLPixelFormatDepth32Float;case 42:return MTLPixelFormatR32Uint;case 43:return MTLPixelFormatR32Sint;
    case 44:case 45:case 46:return MTLPixelFormatDepth32Float_Stencil8;case 47:return MTLPixelFormatX32_Stencil8;
    case 48:case 49:return MTLPixelFormatRG8Unorm;case 50:return MTLPixelFormatRG8Uint;case 51:return MTLPixelFormatRG8Snorm;case 52:return MTLPixelFormatRG8Sint;
    case 53:case 54:return MTLPixelFormatR16Float;case 55:return MTLPixelFormatDepth16Unorm;case 56:return MTLPixelFormatR16Unorm;case 57:return MTLPixelFormatR16Uint;case 58:return MTLPixelFormatR16Snorm;case 59:return MTLPixelFormatR16Sint;
    case 60:case 61:return MTLPixelFormatR8Unorm;case 65:return MTLPixelFormatA8Unorm;case 62:return MTLPixelFormatR8Uint;case 63:return MTLPixelFormatR8Snorm;case 64:return MTLPixelFormatR8Sint;
    case 70:case 71:return MTLPixelFormatBC1_RGBA;case 72:return MTLPixelFormatBC1_RGBA_sRGB;
    case 73:case 74:return MTLPixelFormatBC2_RGBA;case 75:return MTLPixelFormatBC2_RGBA_sRGB;
    case 76:case 77:return MTLPixelFormatBC3_RGBA;case 78:return MTLPixelFormatBC3_RGBA_sRGB;
    case 79:case 80:return MTLPixelFormatBC4_RUnorm;case 81:return MTLPixelFormatBC4_RSnorm;
    case 82:case 83:return MTLPixelFormatBC5_RGUnorm;case 84:return MTLPixelFormatBC5_RGSnorm;
    case 85:return MTLPixelFormatB5G6R5Unorm;case 86:return MTLPixelFormatBGR5A1Unorm;
    case 115:return MTLPixelFormatABGR4Unorm; // CPU 傳輸時調整 alpha nibble 的位置。
    case 94:case 95:return MTLPixelFormatBC6H_RGBUfloat;case 96:return MTLPixelFormatBC6H_RGBFloat;
    case 97:case 98:return MTLPixelFormatBC7_RGBAUnorm;case 99:return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case 87:case 88:case 90:case 92:return MTLPixelFormatBGRA8Unorm;case 91:case 93:return MTLPixelFormatBGRA8Unorm_sRGB;
    default:throw std::invalid_argument("Metal 尚未映射 DXGI 格式："+std::to_string(f));
    }
}
// 像素著色器的 UAV 是否全部只寫：掃描 DXBC（SHEX／SHDR）每條指令。STORE_UAV_TYPED／STORE_RAW／STORE_STRUCTURED 以外只要有 UAV 運算元就視為讀取
// （讀取、原子、計數器都在內）；帶 ROV 或 globally coherent 的宣告同樣回傳 false。立即值偶然長得像 UAV 運算元只會少合併，不影響正確性。無法解析時回傳 false。
bool metal_dxbc_uav_write_only(std::span<const std::uint8_t> bytes){
    const auto word=[&](std::size_t at){std::uint32_t value{};std::memcpy(&value,bytes.data()+at,4);return value;};
    if(bytes.size()<32 || std::memcmp(bytes.data(),"DXBC",4))return false;
    const auto chunks=word(28);if(chunks>64 || 32+std::uint64_t{chunks}*4>bytes.size())return false;
    for(std::uint32_t c=0;c<chunks;++c){
        const std::uint64_t chunk=word(32+c*4);if(chunk+8>bytes.size())return false;
        const auto kind=word(chunk);if(kind!=0x58454853U && kind!=0x52444853U)continue;
        const std::uint64_t size=word(chunk+4),data=chunk+8;if(data+size>bytes.size() || size<8)return false;
        const std::uint64_t count=std::min<std::uint64_t>(word(data+4),size/4);
        std::uint64_t at=2;
        for(;at<count;){
            const auto token=word(data+at*4);const unsigned opcode=token&0x7ffU;std::uint64_t length=(token>>24)&0x7fU;
            if(opcode==53){if(at+1>=count)return false;length=word(data+(at+1)*4);if(length<2)return false;at+=length;continue;} // CUSTOMDATA：長度在下一個 token。
            if(!length)return false;
            if(opcode>=156 && opcode<=158){if(token&0x30000U)return false;at+=length;continue;}
            if(opcode!=164 && opcode!=166 && opcode!=168)for(std::uint64_t n=1;n<length && at+n<count;++n)if(((word(data+(at+n)*4)>>12)&0xffU)==30U)return false;
            at+=length;
        }
        return at==count; // 走訪必須剛好停在結尾，否則視為無法解析。
    }
    return false;
}
MTLVertexFormat metal_vertex_format(std::uint32_t f){
    switch(f){case 2:return MTLVertexFormatFloat4;case 6:return MTLVertexFormatFloat3;case 16:return MTLVertexFormatFloat2;case 41:return MTLVertexFormatFloat;
    case 3:return MTLVertexFormatUInt4;case 7:return MTLVertexFormatUInt3;case 17:return MTLVertexFormatUInt2;case 42:return MTLVertexFormatUInt;
    case 4:return MTLVertexFormatInt4;case 8:return MTLVertexFormatInt3;case 18:return MTLVertexFormatInt2;case 43:return MTLVertexFormatInt;
    case 10:return MTLVertexFormatHalf4;case 34:return MTLVertexFormatHalf2;case 54:return MTLVertexFormatHalf;
    case 87:return MTLVertexFormatUChar4Normalized_BGRA;case 28:return MTLVertexFormatUChar4Normalized;case 30:return MTLVertexFormatUChar4;case 31:return MTLVertexFormatChar4Normalized;case 32:return MTLVertexFormatChar4;
    case 11:return MTLVertexFormatUShort4Normalized;case 12:return MTLVertexFormatUShort4;case 13:return MTLVertexFormatShort4Normalized;case 14:return MTLVertexFormatShort4;
    case 35:return MTLVertexFormatUShort2Normalized;case 36:return MTLVertexFormatUShort2;case 37:return MTLVertexFormatShort2Normalized;case 38:return MTLVertexFormatShort2;
    case 49:return MTLVertexFormatUChar2Normalized;case 50:return MTLVertexFormatUChar2;case 51:return MTLVertexFormatChar2Normalized;case 52:return MTLVertexFormatChar2;
    case 56:return MTLVertexFormatUShortNormalized;case 57:return MTLVertexFormatUShort;case 58:return MTLVertexFormatShortNormalized;case 59:return MTLVertexFormatShort;
    case 61:return MTLVertexFormatUCharNormalized;case 62:return MTLVertexFormatUChar;case 63:return MTLVertexFormatCharNormalized;case 64:return MTLVertexFormatChar;
    case 24:return MTLVertexFormatUInt1010102Normalized;
    case 26:if(@available(macOS 14.0,*))return MTLVertexFormatFloatRG11B10;break;
    default:break;}
    throw std::invalid_argument("Metal 頂點格式不支援：DXGI="+std::to_string(f));
}
std::uint32_t metal_word(std::span<const std::uint8_t> data,std::size_t offset){std::uint32_t v{};if(offset+4>data.size())throw std::invalid_argument("图形結構超出範圍");std::memcpy(&v,data.data()+offset,4);return v;}
bool metal_independent_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes){
    return stage<=5 && bytes.size()>=32 && metal_word(bytes,0)==0x43425844;
}
float metal_float(std::span<const std::uint8_t> data,std::size_t offset){return std::bit_cast<float>(metal_word(data,offset));}
MetalResource& metal_resource(const std::shared_ptr<GraphicsResource>& r){auto* p=dynamic_cast<MetalResource*>(r.get());if(!p)throw std::invalid_argument("圖形資源不屬於 Metal 後端");return *p;}
// graphics_read_completed 的直接讀取資格：Shared 儲存的緩衝。命令執行緒代理也用此判斷決定可否不排空佇列，
// 只讀資源物件本身，不觸及 MetalGl 的批次狀態。
bool metal_completed_readable(const std::shared_ptr<GraphicsResource>& resource){
    const auto* r=dynamic_cast<const MetalResource*>(resource.get());return r && r->buffer && r->buffer.storageMode==MTLStorageModeShared;
}
id<MTLTexture> metal_texture(const GraphicsView& v){auto& r=metal_resource(v.resource);if(!r.texture)throw std::invalid_argument("此資源不是紋理");return r.texture;}
MTLBlendFactor metal_d3d_blend(std::uint32_t f){switch(f){case 1:return MTLBlendFactorZero;case 2:return MTLBlendFactorOne;case 3:return MTLBlendFactorSourceColor;case 4:return MTLBlendFactorOneMinusSourceColor;case 5:return MTLBlendFactorSourceAlpha;case 6:return MTLBlendFactorOneMinusSourceAlpha;case 7:return MTLBlendFactorDestinationAlpha;case 8:return MTLBlendFactorOneMinusDestinationAlpha;case 9:return MTLBlendFactorDestinationColor;case 10:return MTLBlendFactorOneMinusDestinationColor;case 11:return MTLBlendFactorSourceAlphaSaturated;case 14:return MTLBlendFactorBlendColor;case 15:return MTLBlendFactorOneMinusBlendColor;case 16:return MTLBlendFactorSource1Color;case 17:return MTLBlendFactorOneMinusSource1Color;case 18:return MTLBlendFactorSource1Alpha;case 19:return MTLBlendFactorOneMinusSource1Alpha;default:throw std::invalid_argument("混色因子不合法");}}
MTLSamplerAddressMode metal_address(std::uint32_t v){switch(v){case 1:return MTLSamplerAddressModeRepeat;case 2:return MTLSamplerAddressModeMirrorRepeat;case 3:return MTLSamplerAddressModeClampToEdge;case 4:return MTLSamplerAddressModeClampToBorderColor;case 5:return MTLSamplerAddressModeMirrorClampToEdge;default:throw std::invalid_argument("取樣器位址模式不合法");}}
