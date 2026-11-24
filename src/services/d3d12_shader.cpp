#include "x64arm/windows_graphics.hpp"
#include "shader_memory_cache.hpp"
#include <vkd3d_shader.h>
#include <spirv_msl.hpp>
#include "spirv_point_geometry.hpp"
#include "spirv_geometry_stream.hpp"
#include "d3d12_geometry_stream.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <cstring>
#include <map>
#include <set>
#include <regex>
#include <mutex>

namespace x64arm {
// SM5.1 DXBC 與 DXIL（SM6）：vkd3d-shader 轉 SPIR-V，再由 SPIRV-Cross 產生 MSL。
// SM4／5.0 仍走既有 HLSLcc 路徑；此處只接手 HLSLcc 無法處理的格式。
// 綁定表沿用 Metal 後端慣例：0 CBV、1 SRV 紋理、2 取樣器、3 SRV 緩衝、4 UAV 緩衝、5 UAV 紋理。
namespace {
std::uint32_t word(std::span<const std::uint8_t> bytes,std::size_t at){std::uint32_t value;std::memcpy(&value,bytes.data()+at,4);return value;}
// 回傳 0：非 DXBC；1：需要現代路徑（DXIL 或 SM5.1）；2：舊路徑可處理。
unsigned classify(std::span<const std::uint8_t> bytes){
    if(bytes.size()<32 || word(bytes,0)!=0x43425844U || word(bytes,24)!=bytes.size())return 0;
    const auto count=word(bytes,28);if(count>64 || 32ULL+4ULL*count>bytes.size())return 0;
    unsigned result=0;
    for(unsigned i=0;i<count;++i){
        const auto at=word(bytes,32+4*i);if(at>bytes.size()-8)return 0;const auto tag=word(bytes,at),size=word(bytes,at+4);if(std::uint64_t{at}+8+size>bytes.size())return 0;
        if(tag==0x4c495844U)return 1; // DXIL
        if((tag==0x52444853U || tag==0x58454853U) && size>=8){const auto version=word(bytes,at+8);result=((version>>4)&15U)==5 && (version&15U)>=1?1:2;}
    }
    return result;
}
}
bool graphics_modern_shader_required(std::span<const std::uint8_t> bytes){return classify(bytes)==1;}
namespace {
// MIN／MAX 縮減取樣器的著色器內模擬：Apple GPU 不支援取樣器縮減（MTLSamplerReductionMode 在 M4 以前被忽略），
// D3D 的 MINIMUM_*／MAXIMUM_* 取樣器（Hi-Z 等 min/max 金字塔）因此改在 MSL 中以 2×2 texel 讀取取 min／max。
// 取樣器的縮減旗標（後端 graphics_sampler_flags：位元 0～1 縮減 2＝MIN、3＝MAX，位元 2 線性 mip，位元 4～6／8～10 U／V 定址）：
// 描述元陣列取樣器讀取樣器堆積鏡像 1（綁定種類 13，與取樣器表同範圍），離散取樣器讀依原生取樣器槽排列的常數（種類 14）。
// 入口函式內的 texture2d<float>／texture2d_array<float> 明確 LOD 取樣（X64ARM_D3D12_SAMPLER_REDUCTION_IMPLICIT=1 時另含像素著色器的隱含 LOD）改寫為
// 「旗標 <2 → 原呼叫，否則模擬」，一般取樣器的結果與原本相同。
// 模擬分支以巨集閘門：預設 0（編譯器刪除分支與旗標載入，與未改寫的著色器同成本；電馭叛客 Benchmark 執行期判斷約 −7%），
// 程序建立過 MIN／MAX 取樣器後，後端把這一行換成 1 再編譯（見 metal_reduction_source）。
constexpr const char* reduction_helpers=R"(
#define SPV_REDUCTION_ENABLED 0
static inline int spvReduceAddress(int c, int size, uint mode)
{
    if (mode == 1u) { int m = c % size; return m < 0 ? m + size : m; }
    if (mode == 2u) { int period = 2 * size; int m = c % period; if (m < 0) m += period; return m < size ? m : period - 1 - m; }
    if (mode == 5u) { int a = c < 0 ? -c - 1 : c; return min(a, size - 1); }
    return clamp(c, 0, size - 1);
}
static inline float4 spvReduceCombine(float4 a, float4 b, uint mode) { return mode == 2u ? min(a, b) : max(a, b); }
template<typename T>
static inline float4 spvReduceFootprint2D(T t, float2 uv, uint mip, uint flags, int2 offset)
{
    const int2 size = int2(int(t.get_width(mip)), int(t.get_height(mip)));
    const float2 p = uv * float2(size) - 0.5f; const float2 b = floor(p); const float2 f = p - b;
    const int2 i = int2(b) + offset; const uint au = (flags >> 4) & 7u, av = (flags >> 8) & 7u, mode = flags & 3u;
    const uint x0 = uint(spvReduceAddress(i.x, size.x, au)), x1 = uint(spvReduceAddress(i.x + 1, size.x, au));
    const uint y0 = uint(spvReduceAddress(i.y, size.y, av)), y1 = uint(spvReduceAddress(i.y + 1, size.y, av));
    float4 r = float4(t.read(uint2(x0, y0), mip));
    if (f.x > 0.0f) r = spvReduceCombine(r, float4(t.read(uint2(x1, y0), mip)), mode);
    if (f.y > 0.0f) { r = spvReduceCombine(r, float4(t.read(uint2(x0, y1), mip)), mode); if (f.x > 0.0f) r = spvReduceCombine(r, float4(t.read(uint2(x1, y1), mip)), mode); }
    return r;
}
template<typename T>
__attribute__((noinline)) static float4 spvReduceSample2D(T t, uint flags, float2 uv, float lod, int2 offset)
{
    const uint levels = max(t.get_num_mip_levels(), 1u);
    const float l = clamp(lod, 0.0f, float(levels - 1u));
    const bool mip_linear = (flags & 4u) != 0u;
    const uint mip = mip_linear ? uint(floor(l)) : uint(floor(l + 0.5f));
    float4 r = spvReduceFootprint2D(t, uv, mip, flags, offset);
    if (mip_linear && l > float(mip) && mip + 1u < levels) r = spvReduceCombine(r, spvReduceFootprint2D(t, uv, mip + 1u, flags, offset), flags & 3u);
    return r;
}
template<typename T>
static inline float4 spvReduceFootprint2DArray(T t, float2 uv, uint layer, uint mip, uint flags, int2 offset)
{
    const int2 size = int2(int(t.get_width(mip)), int(t.get_height(mip)));
    const float2 p = uv * float2(size) - 0.5f; const float2 b = floor(p); const float2 f = p - b;
    const int2 i = int2(b) + offset; const uint au = (flags >> 4) & 7u, av = (flags >> 8) & 7u, mode = flags & 3u;
    const uint x0 = uint(spvReduceAddress(i.x, size.x, au)), x1 = uint(spvReduceAddress(i.x + 1, size.x, au));
    const uint y0 = uint(spvReduceAddress(i.y, size.y, av)), y1 = uint(spvReduceAddress(i.y + 1, size.y, av));
    float4 r = float4(t.read(uint2(x0, y0), layer, mip));
    if (f.x > 0.0f) r = spvReduceCombine(r, float4(t.read(uint2(x1, y0), layer, mip)), mode);
    if (f.y > 0.0f) { r = spvReduceCombine(r, float4(t.read(uint2(x0, y1), layer, mip)), mode); if (f.x > 0.0f) r = spvReduceCombine(r, float4(t.read(uint2(x1, y1), layer, mip)), mode); }
    return r;
}
template<typename T>
__attribute__((noinline)) static float4 spvReduceSample2DArray(T t, uint flags, float2 uv, uint layer, float lod, int2 offset)
{
    const uint levels = max(t.get_num_mip_levels(), 1u);
    const float l = clamp(lod, 0.0f, float(levels - 1u));
    const bool mip_linear = (flags & 4u) != 0u;
    const uint mip = mip_linear ? uint(floor(l)) : uint(floor(l + 0.5f));
    const uint clamped = min(layer, t.get_array_size() - 1u);
    float4 r = spvReduceFootprint2DArray(t, uv, clamped, mip, flags, offset);
    if (mip_linear && l > float(mip) && mip + 1u < levels) r = spvReduceCombine(r, spvReduceFootprint2DArray(t, uv, clamped, mip + 1u, flags, offset), flags & 3u);
    return r;
}
)";
// 從 at 處的開大括號找到對應的閉大括號。
std::size_t reduction_match_brace(const std::string& text,std::size_t at){
    int depth=0;for(std::size_t i=at;i<text.size();++i){if(text[i]=='{')++depth;else if(text[i]=='}' && --depth==0)return i;}
    return std::string::npos;
}
// 從 at 處的開括號（( 或 [）找到對應的閉括號位置；失敗回 npos。
std::size_t reduction_match(const std::string& text,std::size_t at){
    const char open=text[at],close=open=='('?')':']';int depth=0;
    for(std::size_t i=at;i<text.size();++i){const char c=text[i];if(c=='(' || c=='[')++depth;else if(c==')' || c==']'){if(--depth==0)return c==close?i:std::string::npos;}}
    return std::string::npos;
}
// 依頂層逗號切分參數。
std::vector<std::string> reduction_split(const std::string& text){
    std::vector<std::string> out;int depth=0;std::size_t begin=0;
    // 只以 ()／[] 計算深度：角括號可能是位移或比較運算子（as_type<uint>(…) 內不會有逗號）。
    for(std::size_t i=0;i<text.size();++i){const char c=text[i];if(c=='(' || c=='[')++depth;else if(c==')' || c==']')--depth;else if(c==',' && depth==0){out.push_back(text.substr(begin,i-begin));begin=i+1;}}
    out.push_back(text.substr(begin));
    for(auto& item:out){const auto first=item.find_first_not_of(' ');const auto last=item.find_last_not_of(' ');item=first==std::string::npos?std::string{}:item.substr(first,last-first+1);}
    return out;
}
void rewrite_reduction_samples(GraphicsModernShader& result,std::uint32_t stage){
    // X64ARM_D3D12_SAMPLER_REDUCTION=0 關閉（對照）；預設模擬明確 LOD 的取樣（SampleLevel，Hi-Z 等 min/max 金字塔的用法）；
    // X64ARM_D3D12_SAMPLER_REDUCTION_IMPLICIT=1 另含像素著色器的隱含 LOD 取樣（對照用，待量測成本後決定預設）。
    static const int mode=[] {const auto* text=std::getenv("X64ARM_D3D12_SAMPLER_REDUCTION");if(text && std::string_view(text)=="0")return 0;
        const auto* implicit=std::getenv("X64ARM_D3D12_SAMPLER_REDUCTION_IMPLICIT");return implicit && std::string_view(implicit)=="1"?2:1;}();
    const bool enabled=mode!=0;
    auto& source=result.source;
    if(!enabled || source.find(".sample(")==std::string::npos)return;
    const auto entry=source.rfind(" xlatMtlMain(");if(entry==std::string::npos)return;
    const auto params_open=source.find('(',entry);const auto params_close=reduction_match(source,params_open);if(params_close==std::string::npos)return;
    const auto body_open=source.find('{',params_close);if(body_open==std::string::npos)return;const auto body_close=reduction_match_brace(source,body_open);if(body_close==std::string::npos)return;
    const std::string params=source.substr(params_open+1,params_close-params_open-1);
    // 已用的 buffer 槽：新增的旗標參數接在最大槽之後（不得超過 30）。
    unsigned next_slot=0;{static const std::regex slot(R"(\[\[\s*buffer\((\d+)\)\s*\]\])");for(auto it=std::sregex_iterator(params.begin(),params.end(),slot);it!=std::sregex_iterator();++it)next_slot=std::max(next_slot,static_cast<unsigned>(std::stoul((*it)[1]))+1);}
    if(result.buffer_sizes_slot!=UINT32_MAX)next_slot=std::max(next_slot,result.buffer_sizes_slot+1);
    for(const auto& b:result.bindings)if(b.type==0 || b.type==3 || b.type==4 || (b.type>=6 && b.type<=12))next_slot=std::max(next_slot,b.native_slot+1);
    // 取樣器陣列：名稱 → 參數 buffer 槽（對應種類 11 的綁定）。
    struct ArraySampler {unsigned native{};std::string flags;};std::map<std::string,ArraySampler> arrays;
    {static const std::regex declared(R"(spvDescriptorArray<sampler>\s+(\w+)\s*\{\s*(\w+)\s*\})");
        for(auto it=std::sregex_iterator(source.begin()+static_cast<std::ptrdiff_t>(body_open),source.begin()+static_cast<std::ptrdiff_t>(body_close),declared);it!=std::sregex_iterator();++it){
            std::smatch slot;const std::string param=(*it)[2];const std::regex find_param("device const void\\*\\s+"+param+R"(\s*\[\[\s*buffer\((\d+)\)\s*\]\])");
            if(std::regex_search(params,slot,find_param))arrays[(*it)[1]]={static_cast<unsigned>(std::stoul(slot[1])),{}};
        }}
    std::map<std::string,unsigned> discrete;
    {static const std::regex declared(R"(\bsampler\s+(\w+)\s*\[\[\s*sampler\((\d+)\)\s*\]\])");
        for(auto it=std::sregex_iterator(params.begin(),params.end(),declared);it!=std::sregex_iterator();++it)discrete[(*it)[1]]=static_cast<unsigned>(std::stoul((*it)[2]));}
    if(arrays.empty() && discrete.empty())return;
    // 貼圖型別：只處理 float 的 2D 與 2D 陣列（描述元陣列或離散參數）。
    std::map<std::string,int> textures; // 1＝texture2d<float>、2＝texture2d_array<float>
    {static const std::regex declared(R"(spvDescriptorArray<(texture2d|texture2d_array)<float>>\s+(\w+))");
        for(auto it=std::sregex_iterator(source.begin()+static_cast<std::ptrdiff_t>(body_open),source.begin()+static_cast<std::ptrdiff_t>(body_close),declared);it!=std::sregex_iterator();++it)textures[(*it)[2]]=(*it)[1]=="texture2d"?1:2;
        static const std::regex parameter(R"(\b(texture2d|texture2d_array)<float>\s+(\w+)\s*\[\[\s*texture\()");
        for(auto it=std::sregex_iterator(params.begin(),params.end(),parameter);it!=std::sregex_iterator();++it)textures[(*it)[2]]=(*it)[1]=="texture2d"?1:2;}
    if(textures.empty())return;
    std::vector<std::size_t> calls;for(auto at=source.find(".sample(",body_open);at!=std::string::npos && at<body_close;at=source.find(".sample(",at+1))calls.push_back(at);
    bool used_discrete=false;std::set<std::string> used_arrays;unsigned discrete_slot=UINT32_MAX;
    const auto ident=[](char c){return std::isalnum(static_cast<unsigned char>(c)) || c=='_';};
    for(auto call=calls.rbegin();call!=calls.rend();++call){
        const auto dot=*call;
        // 接收者：識別字或「識別字[...]」。
        std::size_t start=dot;
        if(start && source[start-1]==']'){int depth=0;std::size_t i=start;while(i>0){--i;if(source[i]==']')++depth;else if(source[i]=='[' && --depth==0)break;}start=i;}
        std::size_t name_begin=start;while(name_begin>0 && ident(source[name_begin-1]))--name_begin;
        const std::string name=source.substr(name_begin,start-name_begin);const auto texture=textures.find(name);if(texture==textures.end() || name.empty())continue;
        const std::string receiver=source.substr(name_begin,dot-name_begin);
        const auto open=dot+7;const auto close=reduction_match(source,open);if(close==std::string::npos)continue;
        const auto args=reduction_split(source.substr(open+1,close-open-1));if(args.size()<2)continue;
        // 取樣器旗標運算式。
        std::string flags;const auto& sampler=args[0];
        if(const auto bracket=sampler.find('[');bracket!=std::string::npos && sampler.back()==']'){
            const auto found=arrays.find(sampler.substr(0,bracket));if(found==arrays.end())continue;
            flags="spvSamplerFlags"+std::to_string(found->second.native)+sampler.substr(bracket)+".x";used_arrays.insert(found->first);
        }else{const auto found=discrete.find(sampler);if(found==discrete.end() || found->second>=16)continue;flags="spvSamplerSlotFlags["+std::to_string(found->second)+"].x";used_discrete=true;}
        const bool array=texture->second==2;const std::size_t base=array?3:2; // 選項參數的位置
        std::string lod,offset="int2(0)";
        if(args.size()>base){
            const auto& option=args[base];
            if(option.rfind("level(",0)==0 && option.back()==')')lod="float("+option.substr(6,option.size()-7)+")";else continue;
            if(args.size()==base+2)offset=args[base+1];else if(args.size()>base+2)continue;
        }else if(args.size()==base){if(stage!=1 || mode!=2)continue;lod=receiver+".calculate_clamped_lod("+sampler+", "+args[1]+")";}
        else continue;
        const std::string original=source.substr(name_begin,close+1-name_begin);
        const std::string emulated=array?"spvReduceSample2DArray("+receiver+", "+flags+", "+args[1]+", uint("+args[2]+"), "+lod+", "+offset+")":
            "spvReduceSample2D("+receiver+", "+flags+", "+args[1]+", "+lod+", "+offset+")";
        source.replace(name_begin,close+1-name_begin,"((SPV_REDUCTION_ENABLED == 0 || ("+flags+") < 2u) ? "+original+" : "+emulated+")");
    }
    if(used_arrays.empty() && !used_discrete)return;
    // 新增入口參數與綁定（種類 13：取樣器陣列旗標表；種類 14：離散取樣器旗標常數，16 筆 uint4）。
    std::string extra;
    for(const auto& name:used_arrays){
        const auto& array=arrays.at(name);const auto binding=std::find_if(result.bindings.begin(),result.bindings.end(),[&](const auto& b){return b.type==11 && b.native_slot==array.native;});
        if(binding==result.bindings.end() || next_slot>30)throw std::invalid_argument("取樣器縮減旗標缺少對應的取樣器表或 buffer 槽位");
        extra+=", device const uint4* spvSamplerFlags"+std::to_string(array.native)+" [[buffer("+std::to_string(next_slot)+")]]";
        auto flags=*binding;flags.type=13;flags.native_slot=next_slot++;result.bindings.push_back(flags);
    }
    if(used_discrete){
        if(next_slot>30)throw std::invalid_argument("取樣器縮減旗標缺少 buffer 槽位");
        discrete_slot=next_slot++;extra+=", constant uint4* spvSamplerSlotFlags [[buffer("+std::to_string(discrete_slot)+")]]";
        GraphicsBinding flags{};flags.type=14;flags.native_slot=discrete_slot;result.bindings.push_back(flags);
    }
    source.insert(params_close,extra);
    const auto header=source.find("using namespace metal;");
    if(header==std::string::npos)throw std::invalid_argument("MSL 缺少 metal 命名空間宣告");
    source.insert(source.find('\n',header)+1,reduction_helpers);
}
}
// io_layout：轉譯前的介面變數形狀 {輸出, location, 分量, 向量長度, patch}，供細分連結對齊（只有連結建立時需要）。
static GraphicsModernShader translate_modern_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes,bool point_capture=false,GraphicsGeometryStream* geometry=nullptr,const GraphicsModernTessellationLink* link=nullptr,
                                                    std::vector<std::array<std::uint32_t,5>>* io_layout=nullptr){
    static std::mutex lock;const std::lock_guard guard(lock);
    if(!graphics_modern_shader_required(bytes))throw std::invalid_argument("著色器不是 DXIL 或 SM5.1 DXBC");
    if(const char* directory=std::getenv("X64ARM_DUMP_DXBC");directory && *directory){
        // 與 SM4／5 相同的診斷開關：保存原始容器，供 d3d12_shader_corpus 離線重現轉譯（尚未支援的階段也保存）。
        static std::atomic<std::uint64_t> sequence{};
        const auto path=std::filesystem::path(directory)/(std::to_string(getpid())+"-"+std::to_string(++sequence)+"-stage"+std::to_string(stage)+".dxil");
        std::filesystem::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    }
    if(stage>5)throw std::invalid_argument("現代著色器階段不合法："+std::to_string(stage));
    bool dxil=false;
    for(unsigned i=0;i<word(bytes,28);++i)if(word(bytes,word(bytes,32+4*i))==0x4c495844U)dxil=true;
    struct Scan {
        vkd3d_shader_scan_descriptor_info descriptors{};vkd3d_shader_scan_signature_info signatures{};vkd3d_shader_code code{};char* messages{};
        ~Scan(){vkd3d_shader_free_scan_descriptor_info(&descriptors);vkd3d_shader_free_scan_signature_info(&signatures);vkd3d_shader_free_shader_code(&code);vkd3d_shader_free_messages(messages);}
    } scan;
    scan.descriptors.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;scan.descriptors.next=&scan.signatures;
    scan.signatures.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    // Apple GPU 的 SIMD group 對應 D3D wave（寬 32），MSL 支援 64 位元整數但不支援 double。
    // 無格式型別 UAV 讀取對應 Metal read_write 紋理；threadgroup 零初始化由 SPIRV-Cross 產生協作清零迴圈（見 SpirvCrossWorkgroupZeroInit.cmake）。
    const std::array<vkd3d_shader_compile_option,3> options{{{VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV,VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV_STORAGE_BUFFER},
        {VKD3D_SHADER_COMPILE_OPTION_FEATURE,VKD3D_SHADER_COMPILE_OPTION_FEATURE_INT64|VKD3D_SHADER_COMPILE_OPTION_FEATURE_WAVE_OPS|VKD3D_SHADER_COMPILE_OPTION_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY},
        {VKD3D_SHADER_COMPILE_OPTION_TYPED_UAV,VKD3D_SHADER_COMPILE_OPTION_TYPED_UAV_READ_FORMAT_UNKNOWN}}};
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.source={bytes.data(),bytes.size()};
    info.source_type=dxil?VKD3D_SHADER_SOURCE_DXBC_DXIL:VKD3D_SHADER_SOURCE_DXBC_TPF;info.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;
    info.options=options.data();info.option_count=static_cast<unsigned>(options.size());info.log_level=VKD3D_SHADER_LOG_WARNING;info.next=&scan.descriptors;
    if(vkd3d_shader_scan(&info,&scan.messages)<0){
        scan.descriptors.descriptors=nullptr;scan.descriptors.descriptor_count=0;scan.signatures.input={};scan.signatures.output={};scan.signatures.patch_constant={};
        throw std::invalid_argument("現代著色器掃描失敗："+std::string(scan.messages?scan.messages:""));
    }
    vkd3d_shader_free_messages(scan.messages);scan.messages=nullptr;
    // 每個 D3D 描述元給一個 SPIR-V binding；register space 0 的一般描述元對應 D3D11 的 t／s／u／b 暫存器槽。
    // 超過 Metal 離散綁定上限的大陣列與無界陣列（bindless）改為描述元表：SPIRV-Cross 以執行期陣列產生
    // device const void* 緩衝參數，執行時綁定描述元堆積鏡像與表起點偏移。
    // 共用後端的離散 UAV 槽只有 u0–u7，Metal 函式參數最多 8 張 read_write 紋理；超出的 UAV 紋理同樣改走描述元表
    // （D3D12 的 UAV 紋理必定來自描述元表；UAV 緩衝可能是根描述元，不走此路）。單一描述元以長度 2 宣告，讓 vkd3d 產生執行期陣列並以索引 0 存取；
    // 這類綁定依暫存器由大到小排在最後，vkd3d 依序比對時其他暫存器會先命中自己的綁定。
    struct Descriptor {vkd3d_shader_descriptor_type type;unsigned register_index,count,space;bool buffer,table;};
    std::vector<vkd3d_shader_resource_binding> bindings,overflow;std::vector<Descriptor> overflow_descriptors;std::map<unsigned,Descriptor> by_binding;
    unsigned read_write=0;
    // 離散 UAV 緩衝：共用後端只有 u0–u7 八個 UAV 槽。D3D12（resource binding tier 2）的著色器可宣告更多離散 UAV，
    // 超出時把暫存器較高、沒有計數器的單一 UAV 緩衝改走單項描述元表（16 位元組位址＋長度記錄）；執行期依根簽章取
    // 描述元表範圍的鏡像，或把根 UAV 描述元寫成單項記錄（見 windows_d3d12.cpp bind_tables）。
    // UAV 緩衝的宣告方式（見 Vkd3dTypedUavTexelBuffers.cmake）：raw／structured 與用到原子運算的型別化 UAV 是 storage buffer，
    // 其餘型別化 UAV（RWBuffer<T>）是 texel buffer，由 view 格式轉換，與 UAV 紋理同樣占讀寫紋理名額。
    constexpr std::uint32_t structured_flag=0x40000000U;
    const auto storage_uav=[&](const vkd3d_shader_descriptor_info& d){
        return d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV && d.resource_type==VKD3D_SHADER_RESOURCE_BUFFER &&
            (d.flags&(VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_RAW_BUFFER|VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_ATOMICS|structured_flag));
    };
    std::set<unsigned> spilled_buffers;
    {
        unsigned slots=0,writes=0;std::vector<std::pair<unsigned,unsigned>> candidates; // (暫存器, 描述元索引)
        for(unsigned i=0;i<scan.descriptors.descriptor_count;++i){
            const auto& d=scan.descriptors.descriptors[i];
            if(d.type!=VKD3D_SHADER_DESCRIPTOR_TYPE_UAV || !d.count || d.count==~0U || d.count>8 || (d.register_space>=0xffff0000U && d.register_space<=0xffffffefU))continue;
            if(storage_uav(d)){
                slots+=d.count;
                if(d.count==1 && !(d.flags&VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_COUNTER))candidates.emplace_back(d.register_index,i);
                continue;
            }
            const bool reads=d.flags&(VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_READ|VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_ATOMICS);
            if(d.register_index+d.count>8 || (reads && writes+d.count>8))continue; // 這類貼圖本來就改走描述元表
            if(reads)writes+=d.count;
            slots+=d.count;
        }
        std::sort(candidates.begin(),candidates.end(),std::greater<>());
        for(const auto& [reg,index]:candidates){if(slots<=8)break;(void)reg;spilled_buffers.insert(index);--slots;}
    }
    for(unsigned i=0;i<scan.descriptors.descriptor_count;++i){
        const auto& d=scan.descriptors.descriptors[i];
        if(!d.count)throw std::invalid_argument("現代著色器描述元陣列長度為 0");
        const bool buffer=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV || d.resource_type==VKD3D_SHADER_RESOURCE_BUFFER;
        // SM6.6 動態資源：vkd3d 把每種堆積視圖宣告在保留 space（資源 0xFFFF0000+n、取樣器 0xFFFF8000+n），範圍為整個堆積；
        // 一律走描述元表，執行期綁定目前 SetDescriptorHeaps 的堆積鏡像、偏移 0。
        const bool heap=d.register_space>=0xffff0000U && d.register_space<=0xffffffefU;
        if(heap && (d.register_index || d.count!=~0U))throw std::invalid_argument("現代著色器的描述元堆積宣告範圍不合法");
        bool table=heap || d.count==~0U || d.count>8,spilled=false;
        if(!table && spilled_buffers.count(i))table=spilled=true;
        if(!table && d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV && !storage_uav(d)){
            const bool reads=d.flags&(VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_READ|VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_ATOMICS);
            if(d.register_index+d.count>8 || (reads && read_write+d.count>8))table=spilled=true;
            else if(reads)read_write+=d.count;
        }
        // UAV 計數器（IncrementCounter／Append 等）：離散的單一結構化 UAV 另給一個計數器 binding；描述元表與堆積的 UAV
        // 以同範圍的計數器執行期陣列存取計數器鏡像（見下方 counters）。
        if((d.flags&VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_COUNTER) && !table && d.count!=1)throw std::invalid_argument("現代著色器尚不支援離散陣列的 UAV counter");
        vkd3d_shader_resource_binding binding{};binding.type=d.type;binding.register_space=d.register_space;binding.register_index=d.register_index;
        binding.shader_visibility=VKD3D_SHADER_VISIBILITY_ALL;binding.binding.set=0;binding.binding.count=spilled?std::max(d.count,2U):d.count;
        binding.flags=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER?0U:buffer?VKD3D_SHADER_BINDING_FLAG_BUFFER:VKD3D_SHADER_BINDING_FLAG_IMAGE;
        const Descriptor descriptor{d.type,d.register_index,d.count,d.register_space,buffer,table};
        if(spilled){overflow.push_back(binding);overflow_descriptors.push_back(descriptor);continue;}
        binding.binding.binding=static_cast<unsigned>(bindings.size());by_binding.emplace(binding.binding.binding,descriptor);bindings.push_back(binding);
    }
    // UAV 計數器 binding 排在所有描述元之後；SPIRV-Cross 反射到這些 binding 時配置 Metal 緩衝索引，再寫入對應 UAV 綁定的 counter_slot。
    std::vector<vkd3d_shader_uav_counter_binding> counters;std::map<unsigned,std::pair<unsigned,unsigned>> counter_registers;
    std::map<unsigned,unsigned> counter_tables; // 計數器 binding → 表的描述元數量（~0 為無界）
    std::map<std::pair<unsigned,unsigned>,unsigned> counter_native;
    std::vector<std::size_t> order(overflow.size());for(std::size_t i=0;i<order.size();++i)order[i]=i;
    std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return overflow[a].register_index>overflow[b].register_index;});
    for(const auto i:order){auto binding=overflow[i];binding.binding.binding=static_cast<unsigned>(bindings.size());by_binding.emplace(binding.binding.binding,overflow_descriptors[i]);bindings.push_back(binding);}
    for(unsigned i=0;i<scan.descriptors.descriptor_count;++i){
        const auto& d=scan.descriptors.descriptors[i];if(!(d.flags&VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_COUNTER))continue;
        const bool table=(d.register_space>=0xffff0000U && d.register_space<=0xffffffefU) || d.count==~0U || d.count>8;
        vkd3d_shader_uav_counter_binding counter{};counter.register_space=d.register_space;counter.register_index=d.register_index;
        counter.shader_visibility=VKD3D_SHADER_VISIBILITY_ALL;counter.binding.set=0;counter.binding.count=table?d.count:1;
        counter.binding.binding=static_cast<unsigned>(bindings.size()+counters.size());
        if(table)counter_tables.emplace(counter.binding.binding,d.count);
        counter_registers.emplace(counter.binding.binding,std::pair<unsigned,unsigned>{d.register_space,d.register_index});counters.push_back(counter);
    }
    // 描述元表（bindless）需要動態索引描述元陣列：開啟 EXT_descriptor_indexing，SPIRV-Cross 再以執行期陣列輸出。
    static constexpr vkd3d_shader_spirv_extension extensions[]{VKD3D_SHADER_SPIRV_EXTENSION_EXT_DESCRIPTOR_INDEXING};
    vkd3d_shader_spirv_target_info target{};target.type=VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;target.environment=VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;
    target.extensions=extensions;target.extension_count=1;
    vkd3d_shader_interface_info interface{};interface.type=VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;interface.next=&target;
    interface.bindings=bindings.data();interface.binding_count=static_cast<unsigned>(bindings.size());
    interface.uav_counters=counters.data();interface.uav_counter_count=static_cast<unsigned>(counters.size());info.next=&interface;
    if(vkd3d_shader_compile(&info,&scan.code,&scan.messages)<0){scan.code={};throw std::invalid_argument("現代著色器轉譯失敗："+std::string(scan.messages?scan.messages:""));}
    std::vector<std::uint32_t> words(scan.code.size/4);std::memcpy(words.data(),scan.code.code,words.size()*4);
    // 診斷：X64ARM_DUMP_SPIRV=<目錄> 保存 vkd3d 輸出的 SPIR-V（檔名為內容雜湊），供 spirv-dis 檢查。
    if(const char* folder=std::getenv("X64ARM_DUMP_SPIRV");folder && *folder){std::uint64_t hash=1469598103934665603ULL;for(const auto w:words){hash^=w;hash*=1099511628211ULL;}
        std::ofstream out(std::filesystem::path(folder)/(std::to_string(hash)+".spv"),std::ios::binary);out.write(reinterpret_cast<const char*>(words.data()),static_cast<std::streamsize>(words.size()*4));}
    if(geometry){
        auto stream=spirv_geometry_stream(words);
        geometry->input_vertices=stream.input_vertices;
        geometry->output_width=stream.output_topology==spv::ExecutionModeOutputPoints?1:
                               stream.output_topology==spv::ExecutionModeOutputLineStrip?2:3;
        geometry->max_vertices=stream.max_vertices;
        geometry->input_registers=stream.input_registers;
        geometry->output_registers=stream.output_registers;
        geometry->output_primitive_words=stream.output_primitive_words();
        for(const auto& field:stream.interface){
            const auto& signature=field.input?scan.signatures.input:scan.signatures.output;
            const auto mask=((1U<<field.width)-1)<<field.component;
            const auto* sem=std::find_if(signature.elements,signature.elements+signature.element_count,[&](const auto& e){
                return field.position?e.sysval_semantic==VKD3D_SHADER_SV_POSITION:
                    e.sysval_semantic==VKD3D_SHADER_SV_NONE && e.register_index==field.reg && e.mask==mask;
            });
            if(sem==signature.elements+signature.element_count || !sem->semantic_name)
                throw std::invalid_argument("幾何階段的暫存器分量與簽章不符");
            geometry->interface.push_back({sem->semantic_name,sem->semantic_index,field.reg,field.component,field.width,
                static_cast<uint32_t>(sem->component_type),field.input,field.position});
        }
        words=std::move(stream.words);
    }else if(stage==2)words=spirv_point_geometry(words);
    spirv_cross::CompilerMSL compiler(std::move(words));
    if(io_layout){
        const auto io=compiler.get_shader_resources();
        const auto capture=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& list,std::uint32_t output){
            for(const auto& item:list)if(compiler.has_decoration(item.id,spv::DecorationLocation))
                io_layout->push_back({output,compiler.get_decoration(item.id,spv::DecorationLocation),
                    compiler.has_decoration(item.id,spv::DecorationComponent)?compiler.get_decoration(item.id,spv::DecorationComponent):0U,
                    compiler.get_type(item.base_type_id).vecsize,compiler.has_decoration(item.id,spv::DecorationPatch)?1U:0U});
        };
        capture(io.stage_inputs,0);capture(io.stage_outputs,1);
    }
    const auto model=(stage==0 || (stage==2 && !geometry))?spv::ExecutionModelVertex:stage==1?spv::ExecutionModelFragment:
        stage==3?spv::ExecutionModelTessellationControl:stage==4?spv::ExecutionModelTessellationEvaluation:spv::ExecutionModelGLCompute;
    // HS 的細分設定（domain、spacing、winding、輸出控制點數、point mode）；DS 依連結補上 Metal 需要的同一組設定。
    std::array<std::uint32_t,6> tess_info{};
    if(stage==3){
        const auto& modes=compiler.get_execution_mode_bitset();
        tess_info[graphics_tess_domain]=modes.get(spv::ExecutionModeQuads)?1:modes.get(spv::ExecutionModeIsolines)?2:0;
        tess_info[graphics_tess_spacing]=modes.get(spv::ExecutionModeSpacingFractionalOdd)?1:modes.get(spv::ExecutionModeSpacingFractionalEven)?2:0;
        tess_info[graphics_tess_winding]=modes.get(spv::ExecutionModeVertexOrderCcw)?1:0;
        tess_info[graphics_tess_output_points]=compiler.get_execution_mode_argument(spv::ExecutionModeOutputVertices);
        tess_info[graphics_tess_point_mode]=modes.get(spv::ExecutionModePointMode)?1:0;
        // Metal 沒有等值線細分器，SPIRV-Cross 會拒絕；綁定表與 domain 無關，先以四邊形設定轉譯，讓 PSO 建立與 D3D 一樣成功。
        // 實際繪製由 graphics_modern_tessellation 依 tess_info 的 domain 判斷。
        if(tess_info[graphics_tess_domain]==2){compiler.unset_execution_mode(spv::ExecutionModeIsolines);compiler.set_execution_mode(spv::ExecutionModeQuads);}
    }
    if(stage==4){
        const auto& modes=compiler.get_execution_mode_bitset();
        const auto domain=link?link->info[graphics_tess_domain]:modes.get(spv::ExecutionModeQuads)?1U:modes.get(spv::ExecutionModeIsolines)?2U:0U;
        for(const auto mode:{spv::ExecutionModeTriangles,spv::ExecutionModeQuads,spv::ExecutionModeIsolines})compiler.unset_execution_mode(mode);
        compiler.set_execution_mode(domain==0?spv::ExecutionModeTriangles:spv::ExecutionModeQuads); // 等值線同上，以四邊形轉譯。
        if(link){
            for(const auto mode:{spv::ExecutionModeSpacingEqual,spv::ExecutionModeSpacingFractionalOdd,spv::ExecutionModeSpacingFractionalEven,spv::ExecutionModeVertexOrderCw,spv::ExecutionModeVertexOrderCcw})compiler.unset_execution_mode(mode);
            const auto spacing=link->info[graphics_tess_spacing];
            compiler.set_execution_mode(spacing==1?spv::ExecutionModeSpacingFractionalOdd:spacing==2?spv::ExecutionModeSpacingFractionalEven:spv::ExecutionModeSpacingEqual);
            compiler.set_execution_mode(link->info[graphics_tess_winding]?spv::ExecutionModeVertexOrderCcw:spv::ExecutionModeVertexOrderCw);
        }
        compiler.unset_execution_mode(spv::ExecutionModeOutputVertices);
        compiler.set_execution_mode(spv::ExecutionModeOutputVertices,std::max(1U,link?link->info[graphics_tess_output_points]:3U));
    }
    // 細分階段之間以 location 對齊：每個 location 補齊成 4 個 32 位元分量，缺少的 location 由 SPIRV-Cross 補位，
    // 生產端與消費端的結構版面因此相同。patch 常數的 location 由 vkd3d 依各自控制點簽章偏移，這裡改成連結共用的 patch_base 起算。
    // 沒有連結的單獨轉譯（PSO 建立時取綁定表、連結時取細分設定）：HS 輸入輸出與 DS 輸入的 Component 改成各自獨立的 location，
    // 讓 SPIRV-Cross 可以轉譯；實際繪製另以連結重新轉譯。
    if(!link && (stage==3 || stage==4)){
        const auto io=compiler.get_shader_resources();
        const auto separate=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& list){
            for(const auto& item:list)if(compiler.has_decoration(item.id,spv::DecorationComponent) && compiler.has_decoration(item.id,spv::DecorationLocation)){
                const auto location=compiler.get_decoration(item.id,spv::DecorationLocation),component=compiler.get_decoration(item.id,spv::DecorationComponent);
                compiler.set_decoration(item.id,spv::DecorationLocation,0x1000U+location*4U+component);compiler.unset_decoration(item.id,spv::DecorationComponent);
            }
        };
        separate(io.stage_inputs);if(stage==3)separate(io.stage_outputs);
    }
    if(link && (stage==0 || stage==3 || stage==4)){
        const auto resources_io=compiler.get_shader_resources();
        const std::uint32_t own_offset=stage==3?link->hull_patch_offset:link->domain_patch_offset;
        // 帶非零 Component 的變數改到連結指定的獨立 location（patch 常數以未正規化的暫存器比對），並移除 Component。
        std::set<std::uint32_t> unpacked;
        const auto unpack=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& list,bool output){
            for(const auto& item:list){
                if(!compiler.has_decoration(item.id,spv::DecorationLocation) || !compiler.has_decoration(item.id,spv::DecorationComponent))continue;
                const auto component=compiler.get_decoration(item.id,spv::DecorationComponent);if(!component)continue;
                const bool patch=compiler.has_decoration(item.id,spv::DecorationPatch),vertex_interface=stage==0 || (stage==3 && !output);
                const auto& packed=vertex_interface?link->vertex_packed:patch?link->patch_packed:link->control_packed;
                const auto base=vertex_interface?link->vertex_packed_base:patch?link->patch_packed_base:link->control_packed_base;
                const std::uint32_t offset=patch?own_offset:0U;const auto location=compiler.get_decoration(item.id,spv::DecorationLocation);
                const std::array<std::uint32_t,2> slot{location-offset,component};
                if(location<offset || std::find(packed.begin(),packed.end(),slot)==packed.end())
                    throw std::invalid_argument("細分介面的分量變數不在生產端的版面中");
                compiler.set_decoration(item.id,spv::DecorationLocation,base+slot[0]*4U+component);compiler.unset_decoration(item.id,spv::DecorationComponent);
                unpacked.insert(static_cast<std::uint32_t>(item.id));
            }
        };
        if(stage==0)unpack(resources_io.stage_outputs,true);
        if(stage==3){unpack(resources_io.stage_inputs,false);unpack(resources_io.stage_outputs,true);}
        if(stage==4)unpack(resources_io.stage_inputs,false);
        const auto normalize=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& list){
            for(const auto& item:list)if(!unpacked.contains(static_cast<std::uint32_t>(item.id)) && compiler.has_decoration(item.id,spv::DecorationPatch) && compiler.has_decoration(item.id,spv::DecorationLocation)){
                const auto location=compiler.get_decoration(item.id,spv::DecorationLocation);
                if(location<own_offset)throw std::invalid_argument("細分 patch 常數的 location 小於偏移");
                compiler.set_decoration(item.id,spv::DecorationLocation,location-own_offset+link->patch_base);
            }
        };
        if(stage==3)normalize(resources_io.stage_outputs);
        if(stage==4)normalize(resources_io.stage_inputs);
        // VS 的位置與點大小在細分管線中只是交給 HS 的資料：VS 輸出與 HS 輸入的這兩個內建變數改成一般 location，
        // 結構成員依 location 排序，兩端版面一致（SPIRV-Cross 會把內建成員排在一般成員之後，混用時無法對齊）。
        const auto relocate=[&](const spirv_cross::SmallVector<spirv_cross::BuiltInResource>& list){
            for(const auto& item:list){
                const auto location=item.builtin==spv::BuiltInPosition?link->vertex_position:item.builtin==spv::BuiltInPointSize?link->vertex_point_size:UINT32_MAX;
                if(location==UINT32_MAX)continue;
                compiler.unset_decoration(item.resource.id,spv::DecorationBuiltIn);compiler.set_decoration(item.resource.id,spv::DecorationLocation,location);
            }
        };
        if(stage==0)relocate(resources_io.builtin_outputs);
        if(stage==3)relocate(resources_io.builtin_inputs);
        const auto variable=[](std::uint32_t location,spirv_cross::MSLShaderVariableRate rate,spv::BuiltIn builtin=spv::BuiltInMax,std::uint32_t components=4){
            spirv_cross::MSLShaderInterfaceVariable value{};value.location=location;value.component=0;value.vecsize=components;value.rate=rate;value.builtin=builtin;
            value.format=builtin==spv::BuiltInMax?spirv_cross::MSL_SHADER_VARIABLE_FORMAT_ANY32:spirv_cross::MSL_SHADER_VARIABLE_FORMAT_OTHER;return value;
        };
        const auto vertex_rate=spirv_cross::MSL_SHADER_VARIABLE_RATE_PER_VERTEX,patch_rate=spirv_cross::MSL_SHADER_VARIABLE_RATE_PER_PATCH;
        if(stage==0){
            for(const auto& [location,components]:link->vertex_locations)compiler.add_msl_shader_output(variable(location,vertex_rate,spv::BuiltInMax,components));
            if(link->vertex_position!=UINT32_MAX)compiler.add_msl_shader_output(variable(link->vertex_position,vertex_rate));
            // vkd3d 一律為頂點著色器寫入 PointSize；改成固定的 location，HS 輸入在同一位置補位。
            compiler.add_msl_shader_output(variable(link->vertex_point_size,vertex_rate,spv::BuiltInMax,1));
        }
        if(stage==3){
            for(const auto& [location,components]:link->vertex_locations)compiler.add_msl_shader_input(variable(location,vertex_rate,spv::BuiltInMax,components));
            if(link->vertex_position!=UINT32_MAX)compiler.add_msl_shader_input(variable(link->vertex_position,vertex_rate));
            compiler.add_msl_shader_input(variable(link->vertex_point_size,vertex_rate,spv::BuiltInMax,1));
        }
        if(stage==3 || stage==4){
            const auto add=[&](const spirv_cross::MSLShaderInterfaceVariable& value){if(stage==3)compiler.add_msl_shader_output(value);else compiler.add_msl_shader_input(value);};
            for(const auto& [location,components]:link->control_locations)add(variable(location,vertex_rate,spv::BuiltInMax,components));
            for(const auto& [location,components]:link->patch_locations)add(variable(location,patch_rate,spv::BuiltInMax,components));
        }
    }
    auto msl=compiler.get_msl_options();msl.platform=spirv_cross::CompilerMSL::Options::macOS;msl.set_msl_version(3,1);msl.pad_fragment_output_components=true;
    // 描述元表的 storage buffer 項目帶長度（spvBufferDescriptor：位址、長度、補齊，共 16 位元組）。
    // MSL 3.1：影像原子運算用原生紋理原子（UAV 紋理已帶 MTLTextureUsageShaderAtomic），不再產生會占用 buffer 槽的 _atomic 別名。
    // 無界描述元陣列要求 argument buffer tier 2（Apple Silicon 皆支援）。
    // typed buffer（Buffer<T>／RWBuffer<T>）用原生 texture_buffer，與執行期的 MTLTextureTypeTextureBuffer 檢視一致；
    // raw／structured 緩衝已由 vkd3d 改為 storage buffer（見 Vkd3dStorageBufferArrays.cmake）。
    msl.texture_buffer_native=true;
    const bool tess_vertex=stage==0 && link;
    msl.capture_output_to_buffer=point_capture || tess_vertex;
    msl.vertex_for_tessellation=point_capture || tess_vertex;
    if(tess_vertex)msl.vertex_index_type=link->index_type==1?spirv_cross::CompilerMSL::Options::IndexType::UInt16:
        link->index_type==2?spirv_cross::CompilerMSL::Options::IndexType::UInt32:spirv_cross::CompilerMSL::Options::IndexType::None;
    msl.multi_patch_workgroup=stage==3;

    msl.raw_buffer_tese_input=stage==4;
    msl.runtime_array_rich_descriptor=true;msl.argument_buffers_tier=spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;compiler.set_msl_options(msl);
    GraphicsModernShader result;
    // 離散 UAV 緩衝可能來自根描述元，不能改走描述元表；暫存器超出 u7 時改用本著色器未占用的 u0–u7 slot，
    // 綁定保存原始暫存器，執行期依此放置 view。
    std::array<bool,8> uav_used{};
    for(const auto& [binding,d]:by_binding)if(d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV && !d.table && !d.space)
        for(unsigned j=0;j<d.count;++j)if(d.register_index+j<8)uav_used[d.register_index+j]=true;
    const auto uav_slot=[&](unsigned reg)->std::pair<unsigned,unsigned>{
        if(reg<8)return {reg,UINT32_MAX};
        for(unsigned slot=0;slot<8;++slot)if(!uav_used[slot]){uav_used[slot]=true;return {slot,reg};}
        throw std::invalid_argument("現代著色器的離散 UAV 超出 8 個槽位");
    };
    // 非零 register space 的離散描述元：共用後端的 b／t／s／u 槽位沒有 space，改用本著色器 space 0 未占用的槽位；
    // 綁定保存原始 (space, register)，執行期依根簽章找到對應的根參數或表範圍後放入該槽位。
    std::array<bool,15> constant_used{};std::array<bool,128> resource_used{};std::array<bool,16> sampler_used{};
    for(const auto& [binding,d]:by_binding)if(!d.table && !d.space)for(unsigned j=0;j<d.count;++j){
        const auto reg=d.register_index+j;
        if(d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV && reg<constant_used.size())constant_used[reg]=true;
        else if(d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV && reg<resource_used.size())resource_used[reg]=true;
        else if(d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER && reg<sampler_used.size())sampler_used[reg]=true;
    }
    const auto spaced_slot=[&](vkd3d_shader_descriptor_type type)->unsigned{
        const auto take=[](auto& used,const char* what)->unsigned{
            for(unsigned slot=0;slot<used.size();++slot)if(!used[slot]){used[slot]=true;return slot;}
            throw std::invalid_argument(std::string("現代著色器的非零 space ")+what+" 超出槽位");
        };
        if(type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV)return take(constant_used,"常數緩衝");
        if(type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV)return take(resource_used,"SRV");
        if(type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER)return take(sampler_used,"取樣器");
        return take(uav_used,"UAV");
    };
    // 依 SPIRV-Cross 反射的實際資源型別分配 Metal 索引，與宣告順序無關。
    unsigned next_buffer=0,next_texture=0,next_sampler=0;
    const auto assign=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& resources,unsigned group){
        for(const auto& resource:resources){
            const auto binding=compiler.get_decoration(resource.id,spv::DecorationBinding);const auto found=by_binding.find(binding);
            if(geometry && compiler.get_decoration(resource.id,spv::DecorationDescriptorSet)==1){
                if(group!=1 || binding>1)throw std::invalid_argument("幾何暫存緩衝綁定不合法");
                spirv_cross::MSLResourceBinding mapping{};mapping.stage=model;mapping.desc_set=1;
                mapping.binding=binding;mapping.count=1;mapping.msl_buffer=next_buffer++;
                compiler.add_msl_resource_binding(mapping);
                compiler.set_name(resource.base_type_id,"AJGeometryStorage");
                compiler.set_member_name(resource.base_type_id,0,"words");
                compiler.set_name(resource.id,binding==0?"ajGeometryInput":"ajGeometryOutput");
                (binding==0?geometry->input_slot:geometry->output_slot)=mapping.msl_buffer;
                continue;
            }
            if(found==by_binding.end()){
                const auto counter=counter_registers.find(binding);
                if(counter==counter_registers.end() || group!=1)throw std::invalid_argument("現代著色器資源缺少 D3D 描述元");
                if(const auto table=counter_tables.find(binding);table!=counter_tables.end()){
                    // 計數器表：執行期陣列讀取計數器鏡像（每項 16 位元組：計數器緩衝位址＋長度），種類 12。
                    spirv_cross::MSLResourceBinding mapping{};mapping.stage=model;mapping.desc_set=0;mapping.binding=binding;mapping.count=0;
                    mapping.msl_buffer=mapping.msl_texture=mapping.msl_sampler=next_buffer;compiler.add_msl_resource_binding(mapping);
                    result.bindings.push_back({12U,counter->second.second,next_buffer++,UINT32_MAX,counter->second.first,table->second});
                    continue;
                }
                spirv_cross::MSLResourceBinding mapping{};mapping.stage=model;mapping.desc_set=0;mapping.binding=binding;mapping.count=1;
                mapping.msl_buffer=next_buffer;compiler.add_msl_resource_binding(mapping);counter_native[counter->second]=next_buffer++;
                continue;
            }
            const auto& d=found->second;unsigned kind=0,native=0;
            spirv_cross::MSLResourceBinding mapping{};mapping.stage=model;mapping.desc_set=0;mapping.binding=binding;mapping.count=d.count;
            if(d.table){
                // 執行期陣列：SPIRV-Cross 以 [[buffer(n)]] 的 device const void* 傳入，n 取自該資源種類的索引。
                // 表種類：6／7 SRV／UAV 貼圖（8 位元組資源 ID）、8／9 SRV／UAV 緩衝與 10 CBV（16 位元組位址＋長度）、11 取樣器（8 位元組資源 ID）。
                const bool uav=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;native=next_buffer++;
                mapping.count=0;mapping.msl_buffer=mapping.msl_texture=mapping.msl_sampler=native;compiler.add_msl_resource_binding(mapping);
                const unsigned type=group==0?10U:group==3?11U:group==1?(uav?9U:8U):(uav?7U:6U);
                result.bindings.push_back({type,d.register_index,native,UINT32_MAX,d.space,d.count});
                continue;
            }
            if(group==0){kind=0;native=next_buffer;next_buffer+=d.count;mapping.msl_buffer=native;}
            else if(group==1){kind=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV?4:3;native=next_buffer;next_buffer+=d.count;mapping.msl_buffer=native;}
            else if(group==2){kind=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV?5:1;native=next_texture;next_texture+=d.count;mapping.msl_texture=native;}
            else {kind=2;native=next_sampler;next_sampler+=d.count;mapping.msl_sampler=native;}
            compiler.add_msl_resource_binding(mapping);
            for(unsigned j=0;j<d.count;++j){
                const auto reg=d.register_index+j;
                // D3D12 的暫存器編號不等於 D3D11 型別的內部槽位；space 0 也可能使用 s67、b43、t171。
                // 先保留所有可直接使用的低槽位，再把非零 space 與高編號資源配置到其餘槽位。
                const bool high=(d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV && reg>=constant_used.size()) ||
                    (d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV && reg>=resource_used.size()) ||
                    (d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER && reg>=sampler_used.size());
                if(d.space || high){result.bindings.push_back({kind,spaced_slot(d.type),native+j,UINT32_MAX,d.space,0,reg});continue;}
                const auto [slot,source]=kind==4?uav_slot(reg):std::pair<unsigned,unsigned>{reg,UINT32_MAX};
                result.bindings.push_back({kind,slot,native+j,UINT32_MAX,0,0,source});
            }
        }
    };
    const auto resources=compiler.get_shader_resources();
    assign(resources.uniform_buffers,0);assign(resources.storage_buffers,1);
    assign(resources.separate_images,2);assign(resources.storage_images,2);assign(resources.separate_samplers,3);
    for(auto& b:result.bindings)if(b.type==4){
        const auto reg=b.source!=UINT32_MAX?b.source:b.slot;
        if(const auto found=counter_native.find({b.space,reg});found!=counter_native.end())b.counter_slot=found->second;
    }
    if(next_buffer>31 || next_texture>128 || next_sampler>16)throw std::invalid_argument("現代著色器資源超出 Metal 綁定上限");
    const auto signatures=[&](const vkd3d_shader_signature& signature,std::vector<GraphicsShaderSemantic>& out){
        for(unsigned i=0;i<signature.element_count;++i){const auto& e=signature.elements[i];
            if(stage==2 && !geometry && e.sysval_semantic!=VKD3D_SHADER_SV_NONE)
                throw std::invalid_argument("點幾何著色器的系統語意需要獨立映射");
            if(e.sysval_semantic==VKD3D_SHADER_SV_NONE && e.semantic_name && e.mask)
                out.push_back({e.semantic_name,e.semantic_index,e.register_index,e.mask});
        }
    };
    signatures(scan.signatures.input,result.signature_inputs);signatures(scan.signatures.output,result.signature_outputs);
    if(stage==0 || (stage==2 && !geometry))for(const auto& input:resources.stage_inputs){
        // 頂點屬性位置即 D3D 輸入簽章暫存器；以語意供 input layout 對應。
        const auto location=compiler.get_decoration(input.id,spv::DecorationLocation);
        const auto& signature=scan.signatures.input;
        const auto found=std::find_if(signature.elements,signature.elements+signature.element_count,[&](const auto& e){return e.register_index==location && !e.sysval_semantic;});
        if(found==signature.elements+signature.element_count)throw std::invalid_argument("現代頂點著色器輸入缺少語意");
        result.inputs.push_back({found->semantic_name,found->semantic_index,location});
    }
    if(stage==5)for(unsigned i=0;i<3;++i)result.threads[i]=std::max(1U,compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize,i));
    // OpArrayLength（structured／raw GetDimensions）需要以原生 buffer 索引查詢 view 的位元組長度。
    // 輔助表使用下一個未占用槽位，不能沿用預設 25 而覆蓋一般資源。
    if(point_capture){
        if(next_buffer>=31)throw std::invalid_argument("幾何點擷取缺少輸出緩衝槽位");
        msl.shader_output_buffer_index=next_buffer++;
        result.capture_output_slot=msl.shader_output_buffer_index;
    }
    // 細分階段的內部緩衝接在資源之後；未用到的種類不配置。實際是否出現於產生的 MSL 於編譯後核對。
    std::array<std::uint32_t*,7> tess_targets{&msl.indirect_params_buffer_index,&msl.shader_input_buffer_index,&msl.shader_output_buffer_index,
        &msl.shader_patch_output_buffer_index,&msl.shader_tess_factor_buffer_index,&msl.shader_patch_input_buffer_index,&msl.shader_index_buffer_index};
    std::array<bool,7> tess_wanted{};
    if(tess_vertex)tess_wanted={true,false,true,false,false,false,link->index_type!=0};
    if(stage==3)tess_wanted={true,true,true,true,true,false,false};
    if(stage==4)tess_wanted={true,true,false,false,true,true,false};
    for(unsigned i=0;i<7;++i)if(tess_wanted[i]){
        if(next_buffer>=31)throw std::invalid_argument("細分內部緩衝超出 Metal 綁定上限");
        *tess_targets[i]=next_buffer;result.tess_slots[i]=next_buffer++;
    }
    msl.buffer_size_buffer_index=next_buffer;compiler.set_msl_options(msl);
    compiler.rename_entry_point("main","xlatMtlMain",model);result.source=compiler.compile();result.entry="xlatMtlMain";
    if(stage==3 || stage==4 || tess_vertex){
        // 只保留實際出現在入口參數的內部緩衝；其餘槽位標為未使用。
        static const std::array<const char*,7> names{"spvIndirectParams","spvIn","spvOut","spvPatchOut","spvTessLevel","spvPatchIn","spvIndices"};
        for(unsigned i=0;i<7;++i)if(result.tess_slots[i]!=UINT32_MAX &&
            !std::regex_search(result.source,std::regex(std::string("\\b")+names[i]+R"(\s*\[\[\s*buffer\()"+std::to_string(result.tess_slots[i])+R"(\)\s*\]\])")))result.tess_slots[i]=UINT32_MAX;
        result.tess_info=tess_info;
    }
    if(compiler.needs_buffer_size_buffer()){
        if(next_buffer>=31)throw std::invalid_argument("現代著色器的緩衝大小表超出 Metal 綁定上限");
        result.buffer_sizes_slot=next_buffer;
    }
    rewrite_reduction_samples(result,stage);
    // Metal 沒有 64 位元浮點，裝置也回報 DoublePrecisionFloatShaderOps=FALSE；DXIL 轉譯仍可能為布林轉換產生 double
    // （例如 float(double(a<b)*double(c<d))）。整個字詞換成對應的 float 型別，這類用法的結果不變。
    if(result.source.find("double")!=std::string::npos){
        static const std::regex type(R"(\bdouble([234](x[234])?)?\b)");
        result.source=std::regex_replace(result.source,type,"float$1");
    }
    return result;
}
namespace {
// 跨程序快取的記錄格式（格式版本、MSL、入口、綁定、頂點輸入、執行緒群組大小）；欄位全部以小端 32 位元長度前綴。
constexpr std::uint32_t modern_record_version=10; // 10：MIN／MAX 取樣器模擬（綁定種類 13／14、明確 LOD、不內嵌、SPV_REDUCTION_ENABLED 巨集閘門）。
std::vector<std::uint8_t> encode_modern_shader(const GraphicsModernShader& shader){
    std::vector<std::uint8_t> out;out.reserve(shader.source.size()+shader.bindings.size()*28+256);
    const auto put=[&](std::uint32_t value){const auto* bytes=reinterpret_cast<const std::uint8_t*>(&value);out.insert(out.end(),bytes,bytes+4);};
    const auto text=[&](const std::string& value){put(static_cast<std::uint32_t>(value.size()));out.insert(out.end(),value.begin(),value.end());};
    put(modern_record_version);text(shader.source);text(shader.entry);
    put(static_cast<std::uint32_t>(shader.bindings.size()));
    for(const auto& b:shader.bindings){put(b.type);put(b.slot);put(b.native_slot);put(b.counter_slot);put(b.space);put(b.count);put(b.source);}
    put(static_cast<std::uint32_t>(shader.inputs.size()));
    for(const auto& input:shader.inputs){text(input.semantic);put(input.index);put(input.attribute);}
    for(const auto value:shader.threads)put(value);
    put(shader.buffer_sizes_slot);
    const auto signatures=[&](const std::vector<GraphicsShaderSemantic>& values){
        put(static_cast<std::uint32_t>(values.size()));for(const auto& v:values){text(v.semantic);put(v.index);put(v.location);put(v.mask);}
    };
    signatures(shader.signature_inputs);signatures(shader.signature_outputs);
    return out;
}
std::optional<GraphicsModernShader> decode_modern_shader(std::span<const std::uint8_t> bytes){
    std::size_t at=0;bool ok=true;
    const auto get=[&]()->std::uint32_t{if(bytes.size()-at<4){ok=false;return 0;}std::uint32_t value;std::memcpy(&value,bytes.data()+at,4);at+=4;return value;};
    const auto text=[&]{std::string value;const auto size=get();if(!ok || bytes.size()-at<size){ok=false;return value;}
        value.assign(reinterpret_cast<const char*>(bytes.data()+at),size);at+=size;return value;};
    if(get()!=modern_record_version || !ok)return std::nullopt;
    GraphicsModernShader shader;shader.source=text();shader.entry=text();
    const auto bindings=get();if(!ok || bindings>(bytes.size()-at)/28)return std::nullopt;
    shader.bindings.resize(bindings);
    for(auto& b:shader.bindings){b.type=get();b.slot=get();b.native_slot=get();b.counter_slot=get();b.space=get();b.count=get();b.source=get();}
    const auto inputs=get();if(!ok || inputs>(bytes.size()-at)/12)return std::nullopt;
    shader.inputs.resize(inputs);
    for(auto& input:shader.inputs){input.semantic=text();input.index=get();input.attribute=get();}
    for(auto& value:shader.threads)value=get();
    shader.buffer_sizes_slot=get();
    if(shader.buffer_sizes_slot!=UINT32_MAX && shader.buffer_sizes_slot>=31)return std::nullopt;
    const auto signatures=[&](std::vector<GraphicsShaderSemantic>& values){
        const auto count=get();if(!ok || count>(bytes.size()-at)/16){ok=false;return;}
        values.resize(count);for(auto& v:values){v.semantic=text();v.index=get();v.location=get();v.mask=get();
            if(v.semantic.empty() || !v.mask || (v.mask&~15U))ok=false;
        }
    };
    signatures(shader.signature_inputs);signatures(shader.signature_outputs);
    if(!ok || at!=bytes.size() || shader.source.empty() || shader.entry.empty())return std::nullopt;
    return shader;
}
}
GraphicsGeometryStream graphics_modern_geometry_stream(std::span<const uint8_t> bytes){
    GraphicsGeometryStream result;
    result.shader=translate_modern_shader(2,bytes,false,&result);
    return result;
}
GraphicsModernShader graphics_modern_point_capture(std::span<const std::uint8_t> bytes){
    return translate_modern_shader(2,bytes,true);
}
namespace {
// 細分連結只需要 D3D 簽章（暫存器、系統值）；以 vkd3d 掃描取得，不產生程式碼。
struct ModernSignatures {
    struct Element {std::uint32_t register_index,sysval,mask;};
    std::vector<Element> input,output,patch;
};
ModernSignatures modern_signatures(std::span<const std::uint8_t> bytes){
    if(!graphics_modern_shader_required(bytes))throw std::invalid_argument("細分階段不是 DXIL 或 SM5.1 DXBC");
    bool dxil=false;for(unsigned i=0;i<word(bytes,28);++i)if(word(bytes,word(bytes,32+4*i))==0x4c495844U)dxil=true;
    vkd3d_shader_scan_signature_info signatures{};signatures.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.source={bytes.data(),bytes.size()};
    info.source_type=dxil?VKD3D_SHADER_SOURCE_DXBC_DXIL:VKD3D_SHADER_SOURCE_DXBC_TPF;info.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;info.log_level=VKD3D_SHADER_LOG_NONE;info.next=&signatures;
    char* messages=nullptr;const auto status=vkd3d_shader_scan(&info,&messages);vkd3d_shader_free_messages(messages);
    if(status<0)throw std::invalid_argument("細分階段簽章掃描失敗");
    ModernSignatures result;
    const auto copy=[](const vkd3d_shader_signature& from,std::vector<ModernSignatures::Element>& to){
        for(unsigned i=0;i<from.element_count;++i)to.push_back({from.elements[i].register_index,static_cast<std::uint32_t>(from.elements[i].sysval_semantic),from.elements[i].mask});};
    copy(signatures.input,result.input);copy(signatures.output,result.output);copy(signatures.patch_constant,result.patch);
    vkd3d_shader_free_scan_signature_info(&signatures);
    return result;
}
}
// vkd3d 的 location 等於 D3D 暫存器（系統值改為內建變數，不占 location）；patch 常數另加上各自控制點簽章的下一個列號。
GraphicsModernTessellationLink graphics_modern_tessellation_link(std::span<const std::uint8_t> vertex,std::span<const std::uint8_t> hull,std::span<const std::uint8_t> domain){
    const auto vs=modern_signatures(vertex),hs=modern_signatures(hull),ds=modern_signatures(domain);
    GraphicsModernTessellationLink link;
    const auto user=[](const ModernSignatures::Element& e){return e.sysval==VKD3D_SHADER_SV_NONE;};
    const auto position=[](const ModernSignatures::Element& e){return e.sysval==VKD3D_SHADER_SV_POSITION;};
    // 生產端決定欄位：vkd3d 以遮罩的分量數宣告變數（同一暫存器多個元素時取聯集）。消費端只能讀生產端寫出的暫存器。
    const auto produce=[&](std::vector<std::array<std::uint32_t,2>>& out,const std::vector<ModernSignatures::Element>& producer,std::uint32_t base,bool sysvals=false){
        std::map<std::uint32_t,std::uint32_t> masks;for(const auto& e:producer)if(user(e) || sysvals)masks[e.register_index]|=e.mask&15U;
        for(const auto& [reg,mask]:masks){const auto count=static_cast<std::uint32_t>(__builtin_popcount(mask));if(count)out.push_back({base+reg,count});}
    };
    const auto next=[](const std::vector<ModernSignatures::Element>& list){std::uint32_t value=0;for(const auto& e:list)value=std::max(value,e.register_index+1);return value;};
    produce(link.vertex_locations,vs.output,0);
    // HS 輸出與 DS 輸入的系統值（如 SV_Position）在 vkd3d 是一般 location，一併對齊。
    produce(link.control_locations,hs.output,0,true);
    link.hull_patch_offset=next(hs.output);link.domain_patch_offset=next(ds.input);
    link.patch_base=std::max({link.hull_patch_offset,link.domain_patch_offset,link.control_locations.empty()?0U:link.control_locations.back()[0]+1});
    // 細分係數（SV_TessFactor／SV_InsideTessFactor）是內建變數，由 SPIRV-Cross 寫入／讀取專用的係數緩衝。
    produce(link.patch_locations,hs.patch,link.patch_base);
    // 生產端（VS 輸出、HS 控制點與 patch 輸出）實際的 SPIR-V 變數：vkd3d 對同一暫存器的元素可能合併成一個向量，
    // 也可能各自宣告（帶 Component）。有非零分量的暫存器改依變數逐一註冊：分量 0 留在原 location，其餘配置獨立 location。
    std::vector<std::array<std::uint32_t,5>> vs_io,hs_io;
    (void)translate_modern_shader(0,vertex,false,nullptr,nullptr,&vs_io);
    link.info=translate_modern_shader(3,hull,false,nullptr,nullptr,&hs_io).tess_info;
    std::uint32_t free_location=link.patch_base;
    for(const auto* list:{&link.vertex_locations,&link.control_locations,&link.patch_locations})for(const auto& item:*list)free_location=std::max(free_location,item[0]+1);
    const auto separate=[&](std::vector<std::array<std::uint32_t,2>>& out,std::vector<std::array<std::uint32_t,2>>& packed,std::uint32_t& packed_base,
                            const std::vector<std::array<std::uint32_t,5>>& io,bool patch,std::uint32_t offset,std::uint32_t base){
        std::map<std::uint32_t,std::vector<std::array<std::uint32_t,2>>> registers; // 暫存器 → {分量, 向量長度}
        for(const auto& [output,location,component,vecsize,is_patch]:io)if(output && (is_patch!=0)==patch && location>=offset)registers[location-offset].push_back({component,vecsize});
        std::uint32_t top=0;bool any=false;
        for(const auto& [reg,list]:registers)if(std::any_of(list.begin(),list.end(),[](const auto& v){return v[0]!=0;})){any=true;top=std::max(top,reg);}
        if(!any)return;
        packed_base=free_location;free_location+=(top+1)*4U;
        for(const auto& [reg,list]:registers){
            if(std::none_of(list.begin(),list.end(),[](const auto& v){return v[0]!=0;}))continue;
            std::erase_if(out,[&](const auto& item){return item[0]==base+reg;});
            for(const auto& [component,vecsize]:list){
                if(!component){out.push_back({base+reg,vecsize});continue;}
                packed.push_back({reg,component});out.push_back({packed_base+reg*4U+component,vecsize});
            }
        }
    };
    separate(link.vertex_locations,link.vertex_packed,link.vertex_packed_base,vs_io,false,0,0);
    separate(link.control_locations,link.control_packed,link.control_packed_base,hs_io,false,0,0);
    separate(link.patch_locations,link.patch_packed,link.patch_packed_base,hs_io,true,link.hull_patch_offset,link.patch_base);
    if(std::any_of(vs.output.begin(),vs.output.end(),position) || std::any_of(hs.input.begin(),hs.input.end(),position))link.vertex_position=free_location++;
    link.vertex_point_size=free_location++;
    if(!link.info[graphics_tess_output_points] || link.info[graphics_tess_output_points]>32)throw std::invalid_argument("HS 輸出控制點數不合法");
    return link;
}
GraphicsModernShader graphics_modern_tessellation_stage(std::uint32_t stage,std::span<const std::uint8_t> bytes,const GraphicsModernTessellationLink& link){
    if(stage!=0 && stage!=3 && stage!=4)throw std::invalid_argument("細分連結只適用於 VS、HS、DS");
    return translate_modern_shader(stage,bytes,false,nullptr,&link);
}
// 翻譯結果只取決於（階段、bytecode）與翻譯器本身；跨程序快取命中時略過 vkd3d／SPIRV-Cross（電馭判客 PSO 載入的主要成本）。
// 階段與格式檢查仍先做，失敗的翻譯不寫入快取。
GraphicsModernShader graphics_modern_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes){
    if(stage!=0 && stage!=1 && stage!=5)return translate_modern_shader(stage,bytes); // 尚未支援的階段：保存診斷傾印後丟出含階段的錯誤，不寫入快取。
    if(!graphics_modern_shader_required(bytes))throw std::invalid_argument("著色器不是 DXIL 或 SM5.1 DXBC");
    // 關閉 MIN／MAX 模擬（X64ARM_D3D12_SAMPLER_REDUCTION=0，對照用）時不讀寫跨程序快取：快取鍵不含此設定，避免與預設結果互相覆蓋。
    static const bool reduction_default=std::getenv("X64ARM_D3D12_SAMPLER_REDUCTION")==nullptr && std::getenv("X64ARM_D3D12_SAMPLER_REDUCTION_IMPLICIT")==nullptr;
    if(!detail::shader_memory_cache_enabled() || !reduction_default)return translate_modern_shader(stage,bytes);
    const auto key=detail::shader_cache_key(stage,bytes);
    if(const auto cached=detail::shader_memory_cache_find(key))
        if(auto shader=decode_modern_shader(*cached))return std::move(*shader);
    auto result=translate_modern_shader(stage,bytes);
    detail::shader_memory_cache_store(key,encode_modern_shader(result));
    return result;
}
}
