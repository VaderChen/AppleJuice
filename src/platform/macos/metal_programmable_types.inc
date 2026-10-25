// 每批命令只保留一次資源所有權；雜湊查詢避免每次寫入都線性掃描全部 draw 的重複綁定。
bool metal_backing_generation_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_BACKING_GENERATION");return text && std::string_view(text)=="1";}();return value;}
struct MetalBufferBacking final:GraphicsObject {
    id<MTLBuffer> buffer;
    std::shared_ptr<GpuMemoryReservation> lease;
};
// D3D11 pipeline 狀態的快取鍵：以固定寬度的二進位欄位串接，取代每次繪圖的 ostringstream 格式化（DQ3 每次 DrawIndexed 約 42 µs 的主要成分）。
// 鍵只作 map 查找與串接，不被解析；相同欄位序列產生相同位元組。
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
// DISCARD 更換 backing 時回收舊 buffer：舊 buffer 在 GPU 用完（flight 退役）後進入池，下一次同長度的更換直接取用，
// 省掉 newBufferWithLength＋整段清零＋額度保留。池上限 64 MiB，超出即照常釋放。
// 池只保留 MTLBuffer，不保留 GPU 額度：入池即釋放額度（否則 Steam GPU 程序 256 MiB 額度會被池吃掉 64 MiB），
// 取用時重新保留，失敗就改走一般配置。X64ARM_METAL_DISCARD_POOL=0 關閉就地寫入與回收池，回到每次更換新 buffer。
bool metal_discard_pool_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_DISCARD_POOL");return !text || std::string_view(text)!="0";}();return value;}
struct MetalBufferPool {
    std::multimap<std::size_t,id<MTLBuffer>> free;std::size_t bytes{};std::uint64_t hits{},puts{},drops{};
    static constexpr std::size_t limit=64U<<20;
    id<MTLBuffer> take(std::size_t length){auto it=free.find(length);if(it==free.end())return nil;auto buffer=it->second;free.erase(it);bytes-=length;++hits;return buffer;}
    void put(id<MTLBuffer> buffer){
        const auto length=static_cast<std::size_t>(buffer.length);
        if(!length || bytes+length>limit){++drops;return;}
        free.emplace(length,buffer);bytes+=length;++puts;
    }
};
class MetalPendingObjects {
    static bool indexed(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_RESOURCE_INDEX");return !text || std::string_view(text)!="0";}();return value;}
    std::vector<std::shared_ptr<GraphicsObject>> objects_;
    std::unordered_set<const GraphicsObject*> index_;
    void append(std::shared_ptr<GraphicsObject> object){if(!indexed()){objects_.push_back(std::move(object));return;}auto [position,added]=index_.insert(object.get());if(added){try{objects_.push_back(std::move(object));}catch(...){index_.erase(position);throw;}}}
public:
    void push_back(std::shared_ptr<GraphicsObject>);
    auto end(){return objects_.end();}
    template<class Iterator> void insert(std::vector<std::shared_ptr<GraphicsObject>>::iterator,Iterator first,Iterator last){for(;first!=last;++first)push_back(*first);}
    bool contains(const GraphicsObject* object)const{if(indexed())return index_.contains(object);return std::any_of(objects_.begin(),objects_.end(),[&](const auto& value){return value.get()==object;});}
    void clear(){objects_.clear();index_.clear();}
};
// 與固定管線共用 MetalGl；本檔只提供資源與可程式化管線的共用資料。
struct MetalResource final:GraphicsResource {
    id<MTLBuffer> buffer;id<MTLTexture> texture;
    std::shared_ptr<GpuMemoryReservation> lease;
    std::shared_ptr<MetalBufferBacking> backing;
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
void MetalPendingObjects::push_back(std::shared_ptr<GraphicsObject> object){
    // 捕捉實際編碼時的不可變 backing，避免後續更換 buffer 後依邏輯身分誤判使用範圍。
    // 先保存 backing；即使後續配置節點失敗，已編碼工作的實體額度仍有所有者。
    if(metal_backing_generation_enabled())if(auto* resource=dynamic_cast<MetalResource*>(object.get());resource && resource->backing)append(resource->backing);
    append(std::move(object));
}
const GraphicsObject* metal_resource_use_identity(const std::shared_ptr<GraphicsResource>& resource){
    if(metal_backing_generation_enabled())if(auto* native=dynamic_cast<MetalResource*>(resource.get());native && native->backing)return native->backing.get();
    return resource.get();
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
struct MetalShader final:GraphicsShader {
    std::vector<std::uint8_t> bytecode;
    std::shared_ptr<MetalPendingLibrary> pending;
    std::array<unsigned,4> tessellation{}; // 分割、繞序、最大係數、每群 patch 數。
    id<MTLFunction> function,dual_source_function,non_point_function;id<MTLComputePipelineState> compute;
    std::map<unsigned,id<MTLFunction>> alpha_functions;
    std::string source,geometry_original;
    std::uint64_t serial{};
    bool writes_depth{};
    bool non_point_checked{};
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
    std::array<unsigned,4> settings{};
    bool captured{};
    std::map<std::string,std::array<unsigned,2>> control_fields,patch_fields;
};
struct MetalTessellationDraw {
    std::shared_ptr<MetalTessellationPipeline> pipeline;
    id<MTLBuffer> controls,patches,factors;
    unsigned count{};
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
    case 94:case 95:return MTLPixelFormatBC6H_RGBUfloat;case 96:return MTLPixelFormatBC6H_RGBFloat;
    case 97:case 98:return MTLPixelFormatBC7_RGBAUnorm;case 99:return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case 87:case 88:case 90:case 92:return MTLPixelFormatBGRA8Unorm;case 91:case 93:return MTLPixelFormatBGRA8Unorm_sRGB;
    default:throw std::invalid_argument("Metal 尚未映射 DXGI 格式："+std::to_string(f));
    }
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
