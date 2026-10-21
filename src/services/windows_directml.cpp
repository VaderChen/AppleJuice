#include "x64arm/windows_services.hpp"
#include "x64arm/directml_abi.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace x64arm {
// 內建 DirectML（DirectML.dll）：運算子層級的 ML API，建立在既有 D3D12 實作上。
// 運算子描述依 DirectMLSchema.h 通用解析（scripts/generate_directml_abi.py）。編譯後的運算子或圖是一組 MSL compute 核心，
// 形狀、stride 與屬性寫成常數；每個核心的參數指向圖輸入、圖輸出、內部中間張量或 DML 擁有的持續性資料
// （DML_TENSOR_FLAG_OWNED_BY_DML 於 initializer 時複製、圖常數節點於首次執行時寫入）。
// RecordDispatch 錄成 D3D12 命令清單回呼，ExecuteCommandLists 時以裝置的共用 Metal 後端執行。
// 參考：https://learn.microsoft.com/windows/ai/directml/dml
struct DirectMLState {
    enum class Kind {Device,Operator,Compiled,Initializer,Table,Recorder};
    struct Object {Kind kind{};unsigned references{1};std::uint64_t device{};std::map<std::array<std::uint8_t,16>,std::vector<std::uint8_t>> data;};
    struct Tensor {bool present{};std::uint32_t data_type{},flags{};std::vector<std::uint32_t> sizes,strides;std::uint64_t total{};};
    // 解析後的運算子：輸入／輸出張量依 schema 順序（含未提供的選用張量）；屬性以欄位名稱保存原始位元。
    struct Operation {
        std::uint32_t type{};std::vector<Tensor> inputs,outputs;std::map<std::string,std::uint64_t> scalars;std::map<std::string,std::vector<std::uint32_t>> arrays;
        bool scale_bias{};float scale{1},bias{};std::shared_ptr<Operation> fused;
    };
    // 核心參數來源：圖輸入、圖輸出、內部中間張量、DML 擁有的持續性資料。
    struct Ref {enum Kind {Input,Output,Intermediate,Persistent} kind{Input};std::uint32_t index{};};
    struct Kernel {std::string source;std::uint64_t threads{};std::vector<Ref> inputs,outputs;};
    struct Runtime {std::vector<std::shared_ptr<GraphicsShader>> shaders;std::vector<std::shared_ptr<GraphicsResource>> intermediates,persistent;bool constants_written{};};
    struct Compiled {
        std::vector<Kernel> kernels;std::uint32_t input_count{},output_count{};std::vector<std::uint64_t> intermediate_sizes,persistent_sizes;
        // owned：圖輸入 → 持續性資料（initializer 時由綁定的緩衝複製）；constants：持續性資料 → 常數節點內容。
        std::vector<std::pair<std::uint32_t,std::uint32_t>> owned;std::map<std::uint32_t,std::vector<std::uint8_t>> constants;
        std::map<NativeGraphics*,Runtime> runtime;
    };
    struct Device {std::uint64_t d3d12{};std::uint32_t flags{};};
    struct Binding {std::uint64_t resource{},offset{},size{};};
    // 綁定內容屬於描述元堆積範圍（以 GPU 描述元 handle 為鍵）：RecordDispatch 只記錄範圍，
    // 執行（ExecuteCommandLists 重播）時才讀取當下內容，與 DML 寫入描述元、GPU 執行時讀取的語意一致。
    struct Contents {std::vector<Binding> inputs,outputs;std::vector<std::vector<Binding>> input_arrays;};
    struct Table {std::uint64_t dispatchable{};std::shared_ptr<Contents> contents;};
    std::map<std::uint64_t,std::weak_ptr<Contents>> descriptor_ranges;
    std::map<std::uint64_t,Object> objects;std::map<std::uint64_t,Device> devices;std::map<std::uint64_t,std::shared_ptr<Operation>> operators;
    std::map<std::uint64_t,std::shared_ptr<Compiled>> compiled;std::map<std::uint64_t,std::vector<std::uint64_t>> initializers;std::map<std::uint64_t,Table> tables;
    std::map<std::uint64_t,std::shared_ptr<GraphicsShader>> copy_shaders;std::map<Kind,std::uint64_t> vtables;std::set<std::string> reported;
};
namespace {
using DmlKind=DirectMLState::Kind;
using Tensor=DirectMLState::Tensor;
using Operation=DirectMLState::Operation;
constexpr std::uint32_t ok=0,not_implemented=0x80004001U,no_interface=0x80004002U,pointer_error=0x80004003U,invalid=0x80070057U,unsupported=0x887a0004U;
constexpr std::uint32_t max_dimensions=8;
struct DmlKindInfo {DmlKind kind;std::string_view prefix,top;std::vector<std::string_view> chain;};
const std::vector<DmlKindInfo>& dml_kinds(){
    static const std::vector<DmlKindInfo> list{
        {DmlKind::Device,"Device.","IDMLDevice1",{"IDMLObject","IDMLDevice","IDMLDevice1"}},
        {DmlKind::Operator,"Operator.","IDMLOperator",{"IDMLObject","IDMLDeviceChild","IDMLOperator"}},
        {DmlKind::Compiled,"Compiled.","IDMLCompiledOperator",{"IDMLObject","IDMLDeviceChild","IDMLPageable","IDMLDispatchable","IDMLCompiledOperator"}},
        {DmlKind::Initializer,"Initializer.","IDMLOperatorInitializer",{"IDMLObject","IDMLDeviceChild","IDMLPageable","IDMLDispatchable","IDMLOperatorInitializer"}},
        {DmlKind::Table,"Table.","IDMLBindingTable",{"IDMLObject","IDMLDeviceChild","IDMLBindingTable"}},
        {DmlKind::Recorder,"Recorder.","IDMLCommandRecorder",{"IDMLObject","IDMLDeviceChild","IDMLCommandRecorder"}},
    };
    return list;
}
const DmlKindInfo* dml_info(DmlKind kind){for(const auto& item:dml_kinds())if(item.kind==kind)return &item;return nullptr;}
constexpr std::array<std::uint8_t,16> dml_iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};

// ---- MSL 產生輔助 ----
const char* msl_type(std::uint32_t data_type){
    switch(data_type){
    case dmlabi::DML_TENSOR_DATA_TYPE_FLOAT32:return "float";case dmlabi::DML_TENSOR_DATA_TYPE_FLOAT16:return "half";
    case dmlabi::DML_TENSOR_DATA_TYPE_UINT32:return "uint";case dmlabi::DML_TENSOR_DATA_TYPE_INT32:return "int";
    case dmlabi::DML_TENSOR_DATA_TYPE_UINT16:return "ushort";case dmlabi::DML_TENSOR_DATA_TYPE_INT16:return "short";
    case dmlabi::DML_TENSOR_DATA_TYPE_UINT8:return "uchar";case dmlabi::DML_TENSOR_DATA_TYPE_INT8:return "char";
    case dmlabi::DML_TENSOR_DATA_TYPE_UINT64:return "ulong";case dmlabi::DML_TENSOR_DATA_TYPE_INT64:return "long";
    default:return nullptr;
    }
}
std::vector<std::uint32_t> packed_strides(const std::vector<std::uint32_t>& sizes){
    std::vector<std::uint32_t> strides(sizes.size());std::uint32_t step=1;
    for(std::size_t i=sizes.size();i-->0;){strides[i]=step;step*=sizes[i];}
    return strides;
}
std::vector<std::uint32_t> strides_of(const Tensor& t){return t.strides.empty()?packed_strides(t.sizes):t.strides;}
std::uint64_t element_count(const std::vector<std::uint32_t>& sizes){std::uint64_t count=1;for(const auto size:sizes)count*=size;return count;}
std::string float_literal(float value){
    if(std::isnan(value))return "NAN";if(std::isinf(value))return value>0?"INFINITY":"(-INFINITY)";
    std::ostringstream text;text.precision(9);text<<std::scientific<<value<<"f";return text.str();
}
float as_float(std::uint64_t bits){const auto raw=static_cast<std::uint32_t>(bits);float value;std::memcpy(&value,&raw,4);return value;}
std::string u(std::uint64_t value){return std::to_string(value)+"u";}
// 張量元素位移：c 為各維座標運算式；尺寸為 1 或 stride 為 0 的維度視為廣播。
std::string index_of(const Tensor& t,const std::vector<std::string>& c){
    const auto strides=strides_of(t);std::string out;
    for(std::size_t d=0;d<t.sizes.size() && d<c.size();++d){
        if(t.sizes[d]==1 || !strides[d])continue;
        if(!out.empty())out+="+";out+="("+c[d]+")*"+u(strides[d]);
    }
    return out.empty()?"0u":out;
}
// gid 依 sizes 拆成座標 c0..cN-1（gid 對應列優先的線性索引）。
std::string decompose(const std::vector<std::uint32_t>& sizes,const std::string& prefix="c"){
    std::ostringstream code;code<<"    uint r=gid;\n";
    for(std::size_t d=sizes.size();d-->0;)code<<"    uint "<<prefix<<d<<"=r%"<<u(sizes[d])<<";r/="<<u(sizes[d])<<";\n";
    return code.str();
}
std::vector<std::string> coords(std::size_t count,const std::string& prefix="c"){std::vector<std::string> out;for(std::size_t d=0;d<count;++d)out.push_back(prefix+std::to_string(d));return out;}
std::string signature(const std::vector<const Tensor*>& inputs,const std::vector<const Tensor*>& outputs){
    std::ostringstream src;src<<"#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(";
    std::size_t slot=0;
    for(std::size_t i=0;i<inputs.size();++i,++slot)src<<"device const "<<(inputs[i] && inputs[i]->present?msl_type(inputs[i]->data_type):"float")<<"* in"<<i<<" [[buffer("<<slot<<")]],";
    for(std::size_t i=0;i<outputs.size();++i,++slot)src<<"device "<<(outputs[i] && outputs[i]->present?msl_type(outputs[i]->data_type):"float")<<"* out"<<i<<" [[buffer("<<slot<<")]],";
    src<<"uint gid [[thread_position_in_grid]]){\n";return src.str();
}
float scalar_float(const Operation& op,const char* name,float fallback){const auto it=op.scalars.find(name);return it==op.scalars.end()?fallback:as_float(it->second);}
std::uint32_t scalar_uint(const Operation& op,const char* name,std::uint32_t fallback){const auto it=op.scalars.find(name);return it==op.scalars.end()?fallback:static_cast<std::uint32_t>(it->second);}
std::vector<std::uint32_t> array_of(const Operation& op,const char* name){const auto it=op.arrays.find(name);return it==op.arrays.end()?std::vector<std::uint32_t>{}:it->second;}

std::string activation_expression(const Operation& op,const std::string& x){
    const auto f=[&](const char* name,float fallback){return float_literal(scalar_float(op,name,fallback));};
    switch(op.type){
    case dmlabi::DML_OPERATOR_ACTIVATION_RELU:return "max("+x+",0.0f)";
    case dmlabi::DML_OPERATOR_ACTIVATION_SIGMOID:return "(1.0f/(1.0f+exp(-("+x+"))))";
    case dmlabi::DML_OPERATOR_ACTIVATION_TANH:return "tanh("+x+")";
    case dmlabi::DML_OPERATOR_ACTIVATION_LEAKY_RELU:return "("+x+">=0.0f?"+x+":"+x+"*"+f("Alpha",0.01f)+")";
    case dmlabi::DML_OPERATOR_ACTIVATION_ELU:return "("+x+">=0.0f?"+x+":"+f("Alpha",1.0f)+"*(exp("+x+")-1.0f))";
    case dmlabi::DML_OPERATOR_ACTIVATION_LINEAR:return "("+f("Alpha",1.0f)+"*"+x+"+"+f("Beta",0.0f)+")";
    case dmlabi::DML_OPERATOR_ACTIVATION_SOFTPLUS:return "(log(1.0f+exp("+f("Steepness",1.0f)+"*"+x+"))/"+f("Steepness",1.0f)+")";
    case dmlabi::DML_OPERATOR_ACTIVATION_HARD_SIGMOID:return "clamp("+f("Alpha",0.2f)+"*"+x+"+"+f("Beta",0.5f)+",0.0f,1.0f)";
    case dmlabi::DML_OPERATOR_ACTIVATION_SCALED_ELU:return f("Gamma",1.0507f)+"*("+x+">0.0f?"+x+":"+f("Alpha",1.6733f)+"*(exp("+x+")-1.0f))";
    case dmlabi::DML_OPERATOR_ACTIVATION_SOFTSIGN:return "("+x+"/(1.0f+fabs("+x+")))";
    case dmlabi::DML_OPERATOR_ACTIVATION_THRESHOLDED_RELU:return "("+x+">"+f("Alpha",1.0f)+"?"+x+":0.0f)";
    case dmlabi::DML_OPERATOR_ACTIVATION_IDENTITY:return x;
    default:return {};
    }
}
std::string unary_expression(const Operation& op,const std::string& x){
    const auto f=[&](const char* name,float fallback){return float_literal(scalar_float(op,name,fallback));};
    switch(op.type){
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_IDENTITY:return x;
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_ABS:return "fabs("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_EXP:return "exp("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_LOG:return "log("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_SQRT:return "sqrt("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_RECIP:return "(1.0f/"+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_FLOOR:return "floor("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_CEIL:return "ceil("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_SIN:return "sin("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_COS:return "cos("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_TAN:return "tan("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_TANH:return "tanh("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_ERF:return "precise::erf("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_CLIP:return "clamp("+x+","+f("Min",-INFINITY)+","+f("Max",INFINITY)+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_CONSTANT_POW:return "pow("+x+","+f("Exponent",1.0f)+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_ROUND:return "rint("+x+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_SIGN:return "sign("+x+")";
    default:return activation_expression(op,x);
    }
}
std::string binary_expression(std::uint32_t type,const std::string& a,const std::string& b){
    switch(type){
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_ADD:case dmlabi::DML_OPERATOR_ELEMENT_WISE_ADD1:return "("+a+"+"+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_SUBTRACT:return "("+a+"-"+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_MULTIPLY:return "("+a+"*"+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_DIVIDE:return "("+a+"/"+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_MAX:return "max("+a+","+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_MIN:return "min("+a+","+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_POW:return "pow("+a+","+b+")";
    case dmlabi::DML_OPERATOR_ELEMENT_WISE_MEAN:return "(("+a+"+"+b+")*0.5f)";
    default:return {};
    }
}
// 核心規格：source、執行緒數，以及參數對應的運算子輸入／輸出索引。
struct KernelSpec {std::string source;std::uint64_t threads{};std::vector<std::size_t> inputs,outputs;};
struct Generator {
    const Operation& op;std::string& reason;std::vector<KernelSpec> kernels;
    std::string fused(const std::string& value){
        if(!op.fused)return value;const auto expression=activation_expression(*op.fused,value);if(expression.empty())reason="不支援的 FusedActivation";return expression;
    }
    const Tensor& in(std::size_t i){return op.inputs.at(i);}
    const Tensor& out(std::size_t i){return op.outputs.at(i);}
    bool count_ok(std::uint64_t count){if(count>UINT32_MAX){reason="元素數量";return false;}return true;}
    // 逐元素：所有張量與輸出同形狀（廣播以尺寸 1 或 stride 0 表示）。
    bool elementwise(const std::vector<std::size_t>& inputs,const std::function<std::string(const std::vector<std::string>&)>& body,bool direct_copy=false){
        const auto& o=out(0);const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        std::vector<const Tensor*> ins;for(const auto i:inputs){if(in(i).sizes.size()!=o.sizes.size()){reason="維度不同";return false;}ins.push_back(&in(i));}
        const auto c=coords(o.sizes.size());
        std::ostringstream src;src<<signature(ins,{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes);
        std::vector<std::string> values;for(std::size_t i=0;i<ins.size();++i)values.push_back(direct_copy?"in"+std::to_string(i)+"["+index_of(*ins[i],c)+"]":"float(in"+std::to_string(i)+"["+index_of(*ins[i],c)+"])");
        auto value=body(values);if(!reason.empty())return false;
        src<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"("<<value<<");\n}\n";
        kernels.push_back({src.str(),count,inputs,{0}});return true;
    }
    bool gemm(){
        const auto& a=in(0);const auto& b=in(1);const bool has_c=op.inputs.size()>2 && in(2).present;const auto& o=out(0);
        const auto dims=o.sizes.size();if(dims<2 || a.sizes.size()!=dims || b.sizes.size()!=dims || (has_c && in(2).sizes.size()!=dims)){reason="GEMM 維度";return false;}
        const bool ta=scalar_uint(op,"TransA",0)!=0,tb=scalar_uint(op,"TransB",0)!=0;
        const auto m=o.sizes[dims-2],n=o.sizes[dims-1],k=ta?a.sizes[dims-2]:a.sizes[dims-1];
        if((ta?a.sizes[dims-1]:a.sizes[dims-2])!=m || (tb?b.sizes[dims-1]:b.sizes[dims-2])!=k || (tb?b.sizes[dims-2]:b.sizes[dims-1])!=n){reason="GEMM 形狀";return false;}
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        auto c=coords(dims);
        // A[m,k]：TransA 時存放為 [k,m]；B[k,n]：TransB 時存放為 [n,k]。以座標替換取得位移。
        auto ca=c;ca[dims-2]=ta?"i":c[dims-2];ca[dims-1]=ta?c[dims-2]:"i";
        auto cb=c;cb[dims-2]=tb?c[dims-1]:"i";cb[dims-1]=tb?"i":c[dims-1];
        std::vector<const Tensor*> ins{&a,&b};if(has_c)ins.push_back(&in(2));
        std::ostringstream src;src<<signature(ins,{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes);
        src<<"    float sum=0.0f;\n    for(uint i=0;i<"<<u(k)<<";++i)sum+=float(in0["<<index_of(a,ca)<<"])*float(in1["<<index_of(b,cb)<<"]);\n";
        src<<"    float value="<<float_literal(scalar_float(op,"Alpha",1.0f))<<"*sum;\n";
        if(has_c)src<<"    value+="<<float_literal(scalar_float(op,"Beta",0.0f))<<"*float(in2["<<index_of(in(2),c)<<"]);\n";
        const auto value=fused("value");if(!reason.empty())return false;
        src<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"("<<value<<");\n}\n";
        std::vector<std::size_t> inputs{0,1};if(has_c)inputs.push_back(2);
        kernels.push_back({src.str(),count,inputs,{0}});return true;
    }
    // 卷積：張量為 [N, C, 空間...]；FORWARD 為一般卷積，BACKWARD 為轉置卷積（濾波器 [C, M/G, 核心...]）。
    bool convolution(){
        const auto& x=in(0);const auto& w=in(1);const bool has_bias=in(2).present;const auto& o=out(0);
        const auto dims=o.sizes.size();if(dims<3 || x.sizes.size()!=dims || w.sizes.size()!=dims){reason="卷積維度";return false;}
        const auto spatial=dims-2;const auto groups=std::max(1U,scalar_uint(op,"GroupCount",1));
        auto strides=array_of(op,"Strides"),dilations=array_of(op,"Dilations"),start=array_of(op,"StartPadding");
        strides.resize(spatial,1);dilations.resize(spatial,1);start.resize(spatial,0);
        const bool flip=scalar_uint(op,"Mode",dmlabi::DML_CONVOLUTION_MODE_CROSS_CORRELATION)==dmlabi::DML_CONVOLUTION_MODE_CONVOLUTION;
        const bool backward=scalar_uint(op,"Direction",dmlabi::DML_CONVOLUTION_DIRECTION_FORWARD)==dmlabi::DML_CONVOLUTION_DIRECTION_BACKWARD;
        const auto in_channels=x.sizes[1],out_channels=o.sizes[1];
        if(in_channels%groups || out_channels%groups){reason="GroupCount";return false;}
        const auto cg=in_channels/groups,mg=out_channels/groups;
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        auto c=coords(dims);
        std::vector<const Tensor*> ins{&x,&w};if(has_bias)ins.push_back(&in(2));
        std::ostringstream src;src<<signature(ins,{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes);
        src<<"    uint g=c1/"<<u(mg)<<";float sum=0.0f;\n    for(uint ci=0;ci<"<<u(cg)<<";++ci){\n";
        std::vector<std::string> xc{"c0","g*"+u(cg)+"+ci"},wc;
        if(backward)wc={"g*"+u(cg)+"+ci","c1%"+u(mg)};else wc={"c1","ci"};
        std::string close;std::string valid="true";
        for(std::size_t s=0;s<spatial;++s){
            const auto kd=w.sizes[2+s];const auto ks="k"+std::to_string(s),is="i"+std::to_string(s);
            src<<"    for(uint "<<ks<<"=0;"<<ks<<"<"<<u(kd)<<";++"<<ks<<"){\n";close+="    }\n";
            const auto tap=flip?"("+u(kd-1)+"-"+ks+")":ks;
            if(!backward){
                src<<"    int "<<is<<"=int(c"<<2+s<<")*"<<strides[s]<<"-"<<start[s]<<"+int("<<ks<<")*"<<dilations[s]<<";\n";
                valid+=" && "+is+">=0 && "+is+"<"+std::to_string(x.sizes[2+s]);
            }else{
                // 轉置卷積：o = i·stride − pad + k·dilation，反推 i 須為整數且在範圍內。
                src<<"    int "<<is<<"n=int(c"<<2+s<<")+"<<start[s]<<"-int("<<ks<<")*"<<dilations[s]<<";int "<<is<<"="<<is<<"n/"<<strides[s]<<";\n";
                valid+=" && "+is+"n>=0 && "+is+"n%"+std::to_string(strides[s])+"==0 && "+is+"<"+std::to_string(x.sizes[2+s]);
            }
            xc.push_back("uint("+is+")");wc.push_back(tap);
        }
        src<<"    if("<<valid<<")sum+=float(in0["<<index_of(x,xc)<<"])*float(in1["<<index_of(w,wc)<<"]);\n"<<close<<"    }\n";
        src<<"    float value=sum;\n";
        if(has_bias)src<<"    value+=float(in2["<<index_of(in(2),c)<<"]);\n";
        const auto value=fused("value");if(!reason.empty())return false;
        src<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"("<<value<<");\n}\n";
        std::vector<std::size_t> inputs{0,1};if(has_bias)inputs.push_back(2);
        kernels.push_back({src.str(),count,inputs,{0}});return true;
    }
    bool pooling(bool average){
        const auto& x=in(0);const auto& o=out(0);const auto dims=o.sizes.size();if(dims<3 || x.sizes.size()!=dims){reason="pooling 維度";return false;}
        const auto spatial=dims-2;auto strides=array_of(op,"Strides"),window=array_of(op,"WindowSize"),start=array_of(op,"StartPadding"),dilations=array_of(op,"Dilations");
        strides.resize(spatial,1);window.resize(spatial,1);start.resize(spatial,0);dilations.resize(spatial,1);
        const bool include=scalar_uint(op,"IncludePadding",0)!=0;const bool indices=op.outputs.size()>1 && out(1).present;
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        const auto c=coords(dims);std::vector<const Tensor*> outs{&o};if(indices)outs.push_back(&out(1));
        std::ostringstream src;src<<signature({&x},outs)<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes);
        src<<(average?"    float acc=0.0f;uint taken=0;\n":"    float acc=-INFINITY;uint best=0;\n");
        std::vector<std::string> xc{"c0","c1"};std::string close,valid="true",flat="0";
        std::uint64_t window_total=1;
        for(std::size_t s=0;s<spatial;++s){
            const auto ks="k"+std::to_string(s),is="i"+std::to_string(s);window_total*=window[s];
            src<<"    for(uint "<<ks<<"=0;"<<ks<<"<"<<u(window[s])<<";++"<<ks<<"){\n";close+="    }\n";
            src<<"    int "<<is<<"=int(c"<<2+s<<")*"<<strides[s]<<"-"<<start[s]<<"+int("<<ks<<")*"<<dilations[s]<<";\n";
            valid+=" && "+is+">=0 && "+is+"<"+std::to_string(x.sizes[2+s]);xc.push_back("uint("+is+")");
            flat="("+flat+")*"+std::to_string(x.sizes[2+s])+"+uint("+is+")";
        }
        src<<"    if("<<valid<<"){float v=float(in0["<<index_of(x,xc)<<"]);";
        if(average)src<<"acc+=v;++taken;}\n";else src<<"if(v>acc){acc=v;best="<<flat<<";}}\n";
        src<<close;
        if(average)src<<"    float value=acc/float("<<(include?u(window_total):"max(taken,1u)")<<");\n";else src<<"    float value=acc;\n";
        src<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"(value);\n";
        if(indices)src<<"    out1["<<index_of(out(1),c)<<"]="<<msl_type(out(1).data_type)<<"(best);\n";
        src<<"}\n";
        std::vector<std::size_t> outputs{0};if(indices)outputs.push_back(1);
        kernels.push_back({src.str(),count,{0},outputs});return true;
    }
    // 以「群組」為單位（非 axes 維度的座標），每個執行緒處理整個群組：softmax 類、MVN。
    struct Group {std::vector<std::size_t> axes,others;std::uint64_t groups{1};};
    Group group_of(const std::vector<std::uint32_t>& sizes,std::vector<std::uint32_t> axes){
        Group g;std::sort(axes.begin(),axes.end());axes.erase(std::unique(axes.begin(),axes.end()),axes.end());
        for(std::size_t d=0;d<sizes.size();++d){if(std::find(axes.begin(),axes.end(),static_cast<std::uint32_t>(d))!=axes.end())g.axes.push_back(d);else{g.others.push_back(d);g.groups*=sizes[d];}}
        return g;
    }
    std::string group_header(const std::vector<std::uint32_t>& sizes,const Group& g){
        std::ostringstream code;code<<"    if(gid>="<<u(g.groups)<<")return;\n    uint r=gid;\n";
        for(auto it=g.others.rbegin();it!=g.others.rend();++it)code<<"    uint c"<<*it<<"=r%"<<u(sizes[*it])<<";r/="<<u(sizes[*it])<<";\n";
        return code.str();
    }
    std::string loop_open(const std::vector<std::uint32_t>& sizes,const Group& g){std::string code;for(const auto d:g.axes)code+="    for(uint c"+std::to_string(d)+"=0;c"+std::to_string(d)+"<"+u(sizes[d])+";++c"+std::to_string(d)+"){\n";return code;}
    std::string loop_close(const Group& g){std::string code;for(std::size_t i=0;i<g.axes.size();++i)code+="    }\n";return code;}
    bool softmax(int mode,std::vector<std::uint32_t> axes){ // mode 0 softmax、1 log softmax、2 hardmax
        const auto& x=in(0);const auto& o=out(0);if(x.sizes!=o.sizes){reason="softmax 形狀";return false;}
        if(axes.empty())axes.push_back(static_cast<std::uint32_t>(o.sizes.size()-1));
        const auto g=group_of(o.sizes,axes);if(!count_ok(g.groups))return false;const auto c=coords(o.sizes.size());
        std::ostringstream src;src<<signature({&x},{&o})<<group_header(o.sizes,g);
        src<<"    float peak=-INFINITY;\n"<<loop_open(o.sizes,g)<<"    peak=max(peak,float(in0["<<index_of(x,c)<<"]));\n"<<loop_close(g);
        if(mode==2){
            src<<"    bool found=false;\n"<<loop_open(o.sizes,g)<<"    bool hit=!found && float(in0["<<index_of(x,c)<<"])==peak;found=found||hit;out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"(hit?1.0f:0.0f);\n"<<loop_close(g);
        }else{
            src<<"    float total=0.0f;\n"<<loop_open(o.sizes,g)<<"    total+=exp(float(in0["<<index_of(x,c)<<"])-peak);\n"<<loop_close(g);
            src<<loop_open(o.sizes,g)<<"    float v=float(in0["<<index_of(x,c)<<"])-peak;out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"("<<(mode==1?"v-log(total)":"exp(v)/total")<<");\n"<<loop_close(g);
        }
        src<<"}\n";kernels.push_back({src.str(),g.groups,{0},{0}});return true;
    }
    bool mean_variance(std::vector<std::uint32_t> axes){
        const auto& x=in(0);const bool has_scale=in(1).present,has_bias=in(2).present;const auto& o=out(0);if(x.sizes!=o.sizes){reason="MVN 形狀";return false;}
        const bool normalize=scalar_uint(op,"NormalizeVariance",1)!=0;const float epsilon=scalar_float(op,"Epsilon",1e-5f);
        const auto g=group_of(o.sizes,axes);if(!count_ok(g.groups))return false;const auto c=coords(o.sizes.size());
        std::uint64_t n=1;for(const auto d:g.axes)n*=o.sizes[d];
        std::vector<const Tensor*> ins{&x};std::vector<std::size_t> inputs{0};if(has_scale){ins.push_back(&in(1));inputs.push_back(1);}if(has_bias){ins.push_back(&in(2));inputs.push_back(2);}
        std::ostringstream src;src<<signature(ins,{&o})<<group_header(o.sizes,g);
        src<<"    float mean=0.0f;\n"<<loop_open(o.sizes,g)<<"    mean+=float(in0["<<index_of(x,c)<<"]);\n"<<loop_close(g)<<"    mean/=float("<<u(n)<<");\n";
        src<<"    float variance=0.0f;\n"<<loop_open(o.sizes,g)<<"    {float d=float(in0["<<index_of(x,c)<<"])-mean;variance+=d*d;}\n"<<loop_close(g)<<"    variance/=float("<<u(n)<<");\n";
        src<<"    float inv="<<(normalize?"rsqrt(variance+"+float_literal(epsilon)+")":"1.0f")<<";\n";
        std::string value="(float(in0["+index_of(x,c)+"])-mean)*inv";
        std::size_t next=1;if(has_scale)value="("+value+")*float(in"+std::to_string(next++)+"["+index_of(in(1),c)+"])";
        if(has_bias)value="("+value+")+float(in"+std::to_string(next)+"["+index_of(in(2),c)+"])";
        value=fused(value);if(!reason.empty())return false;
        src<<loop_open(o.sizes,g)<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"("<<value<<");\n"<<loop_close(g)<<"}\n";
        kernels.push_back({src.str(),g.groups,inputs,{0}});return true;
    }
    bool batch_normalization(){
        // Input、Mean、Variance、Scale、Bias（尺寸 1 的維度廣播）。
        const float epsilon=scalar_float(op,"Epsilon",1e-5f);
        return elementwise({0,1,2,3,4},[&](const std::vector<std::string>& v){
            return fused("(("+v[0]+"-"+v[1]+")*rsqrt("+v[2]+"+"+float_literal(epsilon)+")*"+v[3]+"+"+v[4]+")");});
    }
    bool reduce(){
        const auto& x=in(0);const auto& o=out(0);const auto function=scalar_uint(op,"Function",dmlabi::DML_REDUCE_FUNCTION_SUM);
        auto axes=array_of(op,"Axes");const auto dims=x.sizes.size();if(o.sizes.size()!=dims){reason="reduce 維度";return false;}
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        std::ostringstream src;src<<signature({&x},{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes,"o");
        std::vector<std::string> xc;std::string close;std::uint64_t n=1;
        for(std::size_t d=0;d<dims;++d){
            if(std::find(axes.begin(),axes.end(),static_cast<std::uint32_t>(d))==axes.end()){xc.push_back("o"+std::to_string(d));continue;}
            src<<"    for(uint a"<<d<<"=0;a"<<d<<"<"<<u(x.sizes[d])<<";++a"<<d<<"){\n";close+="    }\n";xc.push_back("a"+std::to_string(d));n*=x.sizes[d];
        }
        std::string init="0.0f",step;
        switch(function){
        case dmlabi::DML_REDUCE_FUNCTION_SUM:case dmlabi::DML_REDUCE_FUNCTION_AVERAGE:case dmlabi::DML_REDUCE_FUNCTION_LOG_SUM:step="acc+=v;";break;
        case dmlabi::DML_REDUCE_FUNCTION_L1:step="acc+=fabs(v);";break;
        case dmlabi::DML_REDUCE_FUNCTION_L2:case dmlabi::DML_REDUCE_FUNCTION_SUM_SQUARE:step="acc+=v*v;";break;
        case dmlabi::DML_REDUCE_FUNCTION_LOG_SUM_EXP:step="acc+=exp(v);";break;
        case dmlabi::DML_REDUCE_FUNCTION_MAX:init="-INFINITY";step="acc=max(acc,v);";break;
        case dmlabi::DML_REDUCE_FUNCTION_MIN:init="INFINITY";step="acc=min(acc,v);";break;
        case dmlabi::DML_REDUCE_FUNCTION_MULTIPLY:init="1.0f";step="acc*=v;";break;
        case dmlabi::DML_REDUCE_FUNCTION_ARGMAX:init="-INFINITY";step="if(v>acc){acc=v;best=index;}++index;";break;
        case dmlabi::DML_REDUCE_FUNCTION_ARGMIN:init="INFINITY";step="if(v<acc){acc=v;best=index;}++index;";break;
        default:reason="reduce 函式";return false;
        }
        std::ostringstream body;body<<"    float acc="<<init<<";uint best=0,index=0;\n";
        const auto prefix=body.str();
        std::string text=src.str();
        // 累加器宣告須在迴圈之前：插入於第一個 for 之前。
        const auto loop_at=text.find("    for(uint a");if(loop_at==std::string::npos)text+=prefix;else text.insert(loop_at,prefix);
        std::ostringstream tail;tail<<"    {float v=float(in0["<<index_of(x,xc)<<"]);"<<step<<"}\n"<<close;
        std::string value="acc";
        if(function==dmlabi::DML_REDUCE_FUNCTION_AVERAGE)value="acc/float("+u(n)+")";
        else if(function==dmlabi::DML_REDUCE_FUNCTION_L2)value="sqrt(acc)";
        else if(function==dmlabi::DML_REDUCE_FUNCTION_LOG_SUM || function==dmlabi::DML_REDUCE_FUNCTION_LOG_SUM_EXP)value="log(acc)";
        else if(function==dmlabi::DML_REDUCE_FUNCTION_ARGMAX || function==dmlabi::DML_REDUCE_FUNCTION_ARGMIN)value="best";
        tail<<"    out0["<<index_of(o,coords(dims,"o"))<<"]="<<msl_type(o.data_type)<<"("<<value<<");\n}\n";
        kernels.push_back({text+tail.str(),count,{0},{0}});return true;
    }
    bool join(){
        const auto& o=out(0);const auto axis=scalar_uint(op,"Axis",0);const auto dims=o.sizes.size();if(axis>=dims){reason="join 軸";return false;}
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;const auto c=coords(dims);
        std::vector<const Tensor*> ins;std::vector<std::size_t> inputs;for(std::size_t i=0;i<op.inputs.size();++i){ins.push_back(&in(i));inputs.push_back(i);}
        std::ostringstream src;src<<signature(ins,{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes);
        std::uint32_t base=0;
        for(std::size_t i=0;i<ins.size();++i){
            auto local=c;local[axis]="(c"+std::to_string(axis)+"-"+u(base)+")";
            src<<"    "<<(i?"else ":"")<<"if(c"<<axis<<"<"<<u(base+ins[i]->sizes[axis])<<")out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"(in"<<i<<"["<<index_of(*ins[i],local)<<"]);\n";
            base+=ins[i]->sizes[axis];
        }
        src<<"}\n";kernels.push_back({src.str(),count,inputs,{0}});return true;
    }
    bool split(){
        const auto& x=in(0);const auto axis=scalar_uint(op,"Axis",0);const auto dims=x.sizes.size();if(axis>=dims){reason="split 軸";return false;}
        std::uint32_t base=0;
        for(std::size_t i=0;i<op.outputs.size();++i){
            const auto& o=out(i);const auto count=element_count(o.sizes);if(!count_ok(count))return false;
            auto c=coords(dims);auto source=c;source[axis]="(c"+std::to_string(axis)+"+"+u(base)+")";
            std::ostringstream src;src<<signature({&x},{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes)<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"(in0["<<index_of(x,source)<<"]);\n}\n";
            kernels.push_back({src.str(),count,{0},{i}});base+=o.sizes[axis];
        }
        return true;
    }
    bool slice(bool v1){
        const auto& x=in(0);const auto& o=out(0);const auto dims=o.sizes.size();
        const auto offsets=array_of(op,v1?"InputWindowOffsets":"Offsets"),windows=array_of(op,v1?"InputWindowSizes":"Sizes"),strides=array_of(op,v1?"InputWindowStrides":"Strides");
        if(offsets.size()!=dims || strides.size()!=dims){reason="slice 參數";return false;}
        const auto c=coords(dims);std::vector<std::string> source;
        for(std::size_t d=0;d<dims;++d){
            const auto stride=static_cast<std::int32_t>(strides[d]);
            if(stride>=0)source.push_back("("+u(offsets[d])+"+c"+std::to_string(d)+"*"+u(static_cast<std::uint32_t>(stride))+")");
            else source.push_back("("+u(offsets[d]+(d<windows.size()?windows[d]:1)-1)+"-c"+std::to_string(d)+"*"+u(static_cast<std::uint32_t>(-stride))+")"); // 負 stride 反向走訪視窗。
        }
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        std::ostringstream src;src<<signature({&x},{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes)<<"    out0["<<index_of(o,c)<<"]="<<msl_type(o.data_type)<<"(in0["<<index_of(x,source)<<"]);\n}\n";
        kernels.push_back({src.str(),count,{0},{0}});return true;
    }
    bool padding(bool v1){
        const auto& x=in(0);const auto& o=out(0);const auto dims=o.sizes.size();
        const auto mode=scalar_uint(op,"PaddingMode",dmlabi::DML_PADDING_MODE_CONSTANT);auto start=array_of(op,"StartPadding");start.resize(dims,0);
        float value=v1?0.0f:scalar_float(op,"PaddingValue",0.0f);
        if(v1){const auto raw=op.scalars.count("PaddingValue")?op.scalars.at("PaddingValue"):0;const auto type=scalar_uint(op,"PaddingValueDataType",dmlabi::DML_TENSOR_DATA_TYPE_FLOAT32);
            if(type==dmlabi::DML_TENSOR_DATA_TYPE_FLOAT32)value=as_float(raw);else if(type==dmlabi::DML_TENSOR_DATA_TYPE_INT32)value=static_cast<float>(static_cast<std::int32_t>(raw));else value=static_cast<float>(raw);}
        const auto count=element_count(o.sizes);if(!count_ok(count))return false;
        std::ostringstream src;src<<signature({&x},{&o})<<"    if(gid>="<<u(count)<<")return;\n"<<decompose(o.sizes)<<"    bool inside=true;\n";
        std::vector<std::string> source;
        for(std::size_t d=0;d<dims;++d){
            const auto n=std::to_string(x.sizes[d]);const auto p="p"+std::to_string(d);
            src<<"    int "<<p<<"=int(c"<<d<<")-"<<start[d]<<";\n";
            if(mode==dmlabi::DML_PADDING_MODE_CONSTANT)src<<"    inside=inside && "<<p<<">=0 && "<<p<<"<"<<n<<";\n";
            else if(mode==dmlabi::DML_PADDING_MODE_EDGE)src<<"    "<<p<<"=clamp("<<p<<",0,"<<n<<"-1);\n";
            else if(mode==dmlabi::DML_PADDING_MODE_REFLECTION)src<<"    if("<<p<<"<0)"<<p<<"=-"<<p<<";if("<<p<<">="<<n<<")"<<p<<"=2*("<<n<<"-1)-"<<p<<";\n";
            else if(mode==dmlabi::DML_PADDING_MODE_SYMMETRIC)src<<"    if("<<p<<"<0)"<<p<<"=-"<<p<<"-1;if("<<p<<">="<<n<<")"<<p<<"=2*"<<n<<"-1-"<<p<<";\n";
            else if(mode==dmlabi::DML_PADDING_MODE_WRAP)src<<"    "<<p<<"=(("<<p<<"%"<<n<<")+"<<n<<")%"<<n<<";\n";
            else{reason="padding 模式";return false;}
            source.push_back("uint(clamp("+p+",0,"+n+"-1))");
        }
        src<<"    out0["<<index_of(o,coords(dims))<<"]="<<msl_type(o.data_type)<<"(inside?float(in0["<<index_of(x,source)<<"]):"<<float_literal(value)<<");\n}\n";
        kernels.push_back({src.str(),count,{0},{0}});return true;
    }
    bool run(){
        for(const auto* tensors:{&op.inputs,&op.outputs})for(const auto& tensor:*tensors)
            if(tensor.present && (!msl_type(tensor.data_type) || tensor.sizes.size()>max_dimensions)){reason="張量型別或維度";return false;}
        const auto t=op.type;
        if(!binary_expression(t,"a","b").empty())return elementwise({0,1},[&](const std::vector<std::string>& v){return fused(binary_expression(t,v[0],v[1]));});
        if(t==dmlabi::DML_OPERATOR_CAST)return elementwise({0},[&](const std::vector<std::string>& v){return v[0];},true);
        if(t==dmlabi::DML_OPERATOR_ELEMENT_WISE_IDENTITY && !op.scale_bias)return elementwise({0},[&](const std::vector<std::string>& v){return v[0];},true);
        if(!unary_expression(op,"x").empty())return elementwise({0},[&](const std::vector<std::string>& v){
            const auto x=op.scale_bias?"("+v[0]+"*"+float_literal(op.scale)+"+"+float_literal(op.bias)+")":v[0];return fused(unary_expression(op,x));});
        switch(t){
        case dmlabi::DML_OPERATOR_GEMM:return gemm();
        case dmlabi::DML_OPERATOR_CONVOLUTION:return convolution();
        case dmlabi::DML_OPERATOR_AVERAGE_POOLING:case dmlabi::DML_OPERATOR_AVERAGE_POOLING1:return pooling(true);
        case dmlabi::DML_OPERATOR_MAX_POOLING:case dmlabi::DML_OPERATOR_MAX_POOLING1:case dmlabi::DML_OPERATOR_MAX_POOLING2:return pooling(false);
        case dmlabi::DML_OPERATOR_ACTIVATION_SOFTMAX:return softmax(0,{});
        case dmlabi::DML_OPERATOR_ACTIVATION_LOG_SOFTMAX:return softmax(1,{});
        case dmlabi::DML_OPERATOR_ACTIVATION_HARDMAX:return softmax(2,{});
        case dmlabi::DML_OPERATOR_ACTIVATION_SOFTMAX1:return softmax(0,array_of(op,"Axes"));
        case dmlabi::DML_OPERATOR_ACTIVATION_LOG_SOFTMAX1:return softmax(1,array_of(op,"Axes"));
        case dmlabi::DML_OPERATOR_ACTIVATION_HARDMAX1:return softmax(2,array_of(op,"Axes"));
        case dmlabi::DML_OPERATOR_BATCH_NORMALIZATION:return batch_normalization();
        case dmlabi::DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION:{
            const auto dims=out(0).sizes.size();std::vector<std::uint32_t> axes;for(std::uint32_t d=scalar_uint(op,"CrossChannel",0)?1U:2U;d<dims;++d)axes.push_back(d);return mean_variance(axes);}
        case dmlabi::DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION1:return mean_variance(array_of(op,"Axes"));
        case dmlabi::DML_OPERATOR_REDUCE:return reduce();
        case dmlabi::DML_OPERATOR_JOIN:return join();
        case dmlabi::DML_OPERATOR_SPLIT:return split();
        case dmlabi::DML_OPERATOR_SLICE:return slice(false);
        case dmlabi::DML_OPERATOR_SLICE1:return slice(true);
        case dmlabi::DML_OPERATOR_PADDING:return padding(false);
        case dmlabi::DML_OPERATOR_PADDING1:return padding(true);
        default:reason="尚未支援的運算子";return false;
        }
    }
};
bool generate(const Operation& op,std::vector<KernelSpec>& kernels,std::string& reason){
    Generator generator{op,reason,{}};if(!generator.run()){if(reason.empty())reason="無法產生核心";return false;}
    kernels=std::move(generator.kernels);return true;
}
std::uint64_t tensor_bytes(const Tensor& t){
    if(t.total)return t.total;std::uint64_t last=0;const auto strides=strides_of(t);
    for(std::size_t d=0;d<t.sizes.size();++d)if(t.sizes[d])last+=static_cast<std::uint64_t>(t.sizes[d]-1)*strides[d];
    const auto element=t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_FLOAT16 || t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_INT16 || t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_UINT16?2U:
        t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_UINT8 || t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_INT8?1U:t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_UINT64 || t.data_type==dmlabi::DML_TENSOR_DATA_TYPE_INT64?8U:4U;
    return (last+1)*element;
}
}

// 公告的功能層級：ONNX Runtime 的 DirectML EP 以 DMLCreateDevice1 要求至少 5.0。
// 運算子仍為子集，未支援者於 CreateOperator 回報 E_NOTIMPL 並記錄 directml_note（見 docs/directml.md）。
constexpr std::uint32_t claimed_feature_level=dmlabi::DML_FEATURE_LEVEL_5_0;
std::span<const WindowsServiceDescriptor> directml_export_service_descriptors(){
    // DirectML.lib 以序號匯入：#1 DMLCreateDevice、#2 DMLCreateDevice1。
    static constexpr WindowsServiceDescriptor table[]{{"directml.dll","DMLCreateDevice",4},{"directml.dll","DMLCreateDevice1",5},{"directml.dll","#1",4},{"directml.dll","#2",5}};
    return table;
}
std::span<const WindowsServiceDescriptor> directml_object_service_descriptors(){
    // 各介面完整 vtable（含 IUnknown／IDMLObject）；參數數量含 This。
    static const auto descriptors=[]{
        static std::vector<std::string> names;std::vector<std::pair<std::size_t,unsigned>> rows;
        for(const auto& item:dml_kinds()){const auto* abi=dmlabi::find_interface(item.top);for(const auto& method:abi->methods){rows.push_back({names.size(),method.arguments+1});names.push_back(std::string(item.prefix)+std::string(method.name));}}
        std::vector<WindowsServiceDescriptor> out;
        for(const auto& [index,count]:rows)out.push_back(WindowsServiceDescriptor{"internal-directml",names[index],count});
        return out;
    }();
    return descriptors;
}

std::optional<std::uint32_t> WindowsServices::invoke_directml(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const std::string_view symbol=service.symbol=="#1"?"DMLCreateDevice":service.symbol=="#2"?"DMLCreateDevice1":service.symbol;
    // 診斷：X64ARM_TRACE_DIRECTML=1 於 stderr 列出每次呼叫與 HRESULT。
    static const bool trace=[]{const char* value=std::getenv("X64ARM_TRACE_DIRECTML");return value && *value && *value!='0';}();
    const auto finish=[&](std::uint32_t value){
        if(trace)std::fprintf(stderr,"[directml] %.*s this=0x%llx a1=0x%llx a2=0x%llx -> 0x%x\n",static_cast<int>(symbol.size()),symbol.data(),
            static_cast<unsigned long long>(arg(0)),static_cast<unsigned long long>(service.arguments>1?arg(1):0),static_cast<unsigned long long>(service.arguments>2?arg(2):0),value);
        arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(x86_)return finish(unsupported);
    if(!directml_)directml_=std::make_shared<DirectMLState>();
    auto& dml=*directml_;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto load=[&](std::uint64_t at,unsigned bits){return memory.load(GuestAddress{at},static_cast<std::uint8_t>(bits));};
    const auto store=[&](std::uint64_t at,unsigned bits,std::uint64_t value){if(at)memory.store(GuestAddress{at},static_cast<std::uint8_t>(bits),value);};
    const auto report=[&](const std::string& what){if(dml.reported.insert(what).second)runtime_.module_events.push_back("directml_note="+what);};
    // DML_BINDING_TABLE_DESC：Dispatchable、CPU handle、GPU handle、SizeInDescriptors。同一 GPU 範圍共用內容。
    const auto bind_range=[&](DirectMLState::Table& table,std::uint64_t desc){
        if(!desc){table.contents=std::make_shared<DirectMLState::Contents>();return;}
        table.dispatchable=load(desc,64);const auto gpu=load(desc+16,64);
        if(gpu){if(auto shared=dml.descriptor_ranges[gpu].lock()){table.contents=std::move(shared);return;}}
        table.contents=std::make_shared<DirectMLState::Contents>();if(gpu)dml.descriptor_ranges[gpu]=table.contents;
        std::erase_if(dml.descriptor_ranges,[](const auto& item){return item.second.expired();});
    };
    const auto create=[&](DmlKind kind,std::uint64_t device)->std::uint64_t{
        auto& table=dml.vtables[kind];
        if(!table){
            const auto* item=dml_info(kind);const auto* abi=dmlabi::find_interface(item->top);table=runtime_allocate(memory,{},abi->methods.size()*8);
            for(std::size_t i=0;i<abi->methods.size();++i)store(table+i*8,64,resolve("internal-directml",std::string(item->prefix)+std::string(abi->methods[i].name)).value);
        }
        const auto pointer=allocate_heap(memory,16);if(!pointer)throw std::bad_alloc();
        store(pointer,64,table);store(pointer+8,64,0);
        DirectMLState::Object object;object.kind=kind;object.device=device;dml.objects.emplace(pointer,std::move(object));
        if(device)++dml.objects.at(device).references;
        return pointer;
    };
    const auto query=[&](std::uint64_t self,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        if(!out)return pointer_error;store(out,64,0);if(!riid)return invalid;
        const auto found=dml.objects.find(self);if(found==dml.objects.end())return invalid;
        std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{riid},iid);bool match=iid==dml_iid_unknown;
        if(const auto* item=dml_info(found->second.kind))for(const auto name:item->chain)if(const auto* abi=dmlabi::find_interface(name);abi && abi->iid==iid)match=true;
        if(!match)return no_interface;
        ++found->second.references;store(out,64,self);return ok;
    };
    const auto give=[&](std::uint64_t object,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
        const auto result=query(object,riid,out);--dml.objects.at(object).references;
        if(result!=ok && !dml.objects.at(object).references){dml.objects.erase(object);}
        return result;
    };
    // 張量與運算子描述（依 DirectMLSchema.h 欄位）。
    const auto read_tensor=[&](std::uint64_t at,Tensor& tensor)->bool{
        tensor={};if(!at)return true;
        if(load(at,32)!=dmlabi::DML_TENSOR_TYPE_BUFFER)return false;const auto desc=load(at+8,64);if(!desc)return false;
        tensor.present=true;tensor.data_type=static_cast<std::uint32_t>(load(desc,32));tensor.flags=static_cast<std::uint32_t>(load(desc+4,32));
        const auto dims=static_cast<std::uint32_t>(load(desc+8,32));if(!dims || dims>max_dimensions)return false;
        const auto sizes=load(desc+16,64);const auto strides=load(desc+24,64);if(!sizes)return false;
        for(std::uint32_t i=0;i<dims;++i)tensor.sizes.push_back(static_cast<std::uint32_t>(load(sizes+i*4,32)));
        if(strides)for(std::uint32_t i=0;i<dims;++i)tensor.strides.push_back(static_cast<std::uint32_t>(load(strides+i*4,32)));
        tensor.total=load(desc+32,64);return true;
    };
    // fused：FusedActivation 的巢狀描述，依規格其輸入／輸出張量必須為 NULL。
    std::function<std::shared_ptr<Operation>(std::uint64_t,std::string&,bool)> read_operation;
    read_operation=[&](std::uint64_t at,std::string& reason,bool fused)->std::shared_ptr<Operation>{
        if(!at){reason="缺少運算子描述";return {};}
        auto op=std::make_shared<Operation>();op->type=static_cast<std::uint32_t>(load(at,32));const auto desc=load(at+8,64);
        const auto* schema=dmlabi::find_operator(op->type);if(!schema || !desc){reason="未知的運算子類型 "+std::to_string(op->type);return {};}
        std::vector<std::uint64_t> raw;
        for(const auto& field:schema->fields){
            const auto at_field=desc+field.offset;
            switch(field.type){
            case 0:{ // TENSOR_DESC
                Tensor tensor;if(!read_tensor(load(at_field,64),tensor) || (fused?tensor.present:(!tensor.present && !field.optional))){reason="張量描述："+std::string(field.name);return {};}
                (field.kind==1?op->outputs:op->inputs).push_back(std::move(tensor));raw.push_back(0);break;
            }
            case 1:{ // TENSOR_DESC_ARRAY：DML_TENSOR_DESC 陣列（每項 16 位元組）
                const auto pointer=load(at_field,64);const auto count=field.count>=0?static_cast<std::uint32_t>(raw.at(static_cast<std::size_t>(field.count))):0U;
                if(count>128 || (count && !pointer)){reason="張量陣列："+std::string(field.name);return {};}
                for(std::uint32_t i=0;i<count;++i){Tensor tensor;if(!read_tensor(pointer+i*16,tensor) || !tensor.present){reason="張量陣列："+std::string(field.name);return {};}(field.kind==1?op->outputs:op->inputs).push_back(std::move(tensor));}
                raw.push_back(pointer);break;
            }
            case 2:{ // OPERATOR_DESC（FusedActivation）
                const auto nested=load(at_field,64);if(nested){if(fused){reason="巢狀 FusedActivation";return {};}op->fused=read_operation(nested,reason,true);if(!op->fused)return {};}
                raw.push_back(nested);break;
            }
            case 4:case 6:case 7:case 14:raw.push_back(load(at_field,32));op->scalars[std::string(field.name)]=raw.back();break;
            case 5:case 13:raw.push_back(load(at_field,64));op->scalars[std::string(field.name)]=raw.back();break;
            case 8:case 9:case 10:{ // UINT／INT／FLOAT 陣列：長度取自對應欄位
                const auto pointer=load(at_field,64);const auto count=field.count>=0?static_cast<std::uint32_t>(raw.at(static_cast<std::size_t>(field.count))):0U;
                std::vector<std::uint32_t> values;if(pointer)for(std::uint32_t i=0;i<count && i<4096;++i)values.push_back(static_cast<std::uint32_t>(load(pointer+i*4,32)));
                op->arrays[std::string(field.name)]=std::move(values);raw.push_back(pointer);break;
            }
            case 11:{ // SCALE_BIAS
                const auto pointer=load(at_field,64);
                if(pointer){op->scale_bias=true;op->scale=as_float(load(pointer,32));op->bias=as_float(load(pointer+4,32));}
                raw.push_back(pointer);break;
            }
            case 12:raw.push_back(load(at_field,64));op->scalars[std::string(field.name)]=raw.back();break; // SIZE_2D
            default:reason="尚未支援的欄位型別："+std::string(field.name);return {};
            }
        }
        return op;
    };
    const auto device_of_child=[&](std::uint64_t self)->std::uint64_t{const auto found=dml.objects.find(self);return found==dml.objects.end()?0:found->second.device;};
    const auto operator_name=[](const Operation& op){const auto* schema=dmlabi::find_operator(op.type);return std::string(schema?schema->name:"?");};
    // 單一運算子編譯：運算子輸入／輸出直接對應綁定索引；OWNED_BY_DML 的輸入改由持續性資料提供。
    const auto compile_single=[&](const std::shared_ptr<Operation>& op,DirectMLState::Compiled& compiled,std::string& reason)->bool{
        std::vector<KernelSpec> specs;if(!generate(*op,specs,reason))return false;
        compiled.input_count=static_cast<std::uint32_t>(op->inputs.size());compiled.output_count=static_cast<std::uint32_t>(op->outputs.size());
        std::map<std::size_t,std::uint32_t> persistent;
        for(std::size_t i=0;i<op->inputs.size();++i)if(op->inputs[i].present && (op->inputs[i].flags&dmlabi::DML_TENSOR_FLAG_OWNED_BY_DML)){
            persistent[i]=static_cast<std::uint32_t>(compiled.persistent_sizes.size());compiled.persistent_sizes.push_back(tensor_bytes(op->inputs[i]));
            compiled.owned.push_back({static_cast<std::uint32_t>(i),persistent[i]});
        }
        for(auto& spec:specs){
            DirectMLState::Kernel kernel{std::move(spec.source),spec.threads,{},{}};
            for(const auto i:spec.inputs)kernel.inputs.push_back(persistent.contains(i)?DirectMLState::Ref{DirectMLState::Ref::Persistent,persistent.at(i)}:DirectMLState::Ref{DirectMLState::Ref::Input,static_cast<std::uint32_t>(i)});
            for(const auto i:spec.outputs)kernel.outputs.push_back({DirectMLState::Ref::Output,static_cast<std::uint32_t>(i)});
            compiled.kernels.push_back(std::move(kernel));
        }
        return true;
    };
    // 圖編譯：依邊建立節點輸入／輸出的來源，拓撲排序後串接各節點的核心；中間張量與常數節點由 DML 內部配置。
    const auto compile_graph=[&](std::uint64_t desc,DirectMLState::Compiled& compiled,std::string& reason)->bool{
        compiled.input_count=static_cast<std::uint32_t>(load(desc,32));compiled.output_count=static_cast<std::uint32_t>(load(desc+4,32));
        const auto node_count=static_cast<std::uint32_t>(load(desc+8,32));const auto nodes=load(desc+16,64);
        if(!node_count || !nodes || node_count>65536){reason="圖節點";return false;}
        struct Node {std::shared_ptr<Operation> op;bool constant{};std::vector<std::uint8_t> data;std::vector<DirectMLState::Ref> in,out;std::vector<bool> in_set;};
        std::vector<Node> graph(node_count);
        for(std::uint32_t i=0;i<node_count;++i){
            const auto type=static_cast<std::uint32_t>(load(nodes+i*16,32));const auto node=load(nodes+i*16+8,64);
            if(type==dmlabi::DML_GRAPH_NODE_TYPE_OPERATOR){
                const auto found=dml.operators.find(node?load(node,64):0);if(found==dml.operators.end()){reason="圖節點運算子";return false;}
                graph[i].op=found->second;graph[i].in.resize(found->second->inputs.size());graph[i].in_set.resize(found->second->inputs.size());graph[i].out.resize(found->second->outputs.size());
                for(std::size_t o=0;o<graph[i].out.size();++o){ // 預設：未連接的輸出寫入內部中間張量。
                    graph[i].out[o]={DirectMLState::Ref::Intermediate,static_cast<std::uint32_t>(compiled.intermediate_sizes.size())};compiled.intermediate_sizes.push_back(tensor_bytes(found->second->outputs[o]));}
            }else if(type==dmlabi::DML_GRAPH_NODE_TYPE_CONSTANT){
                const auto data=load(node,64);const auto size=load(node+8,64);if(!data || !size || size>(std::uint64_t{1}<<30)){reason="常數節點";return false;}
                graph[i].constant=true;graph[i].data.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{data},graph[i].data);
                const auto index=static_cast<std::uint32_t>(compiled.persistent_sizes.size());compiled.persistent_sizes.push_back(size);compiled.constants[index]=graph[i].data;
                graph[i].out={{DirectMLState::Ref::Persistent,index}};
            }else{reason="圖節點型別";return false;}
        }
        const auto edges=[&](std::uint64_t count_at,std::uint64_t pointer_at,auto&& handle)->bool{
            const auto count=static_cast<std::uint32_t>(load(desc+count_at,32));const auto pointer=load(desc+pointer_at,64);
            for(std::uint32_t e=0;e<count;++e){const auto edge=load(pointer+e*16+8,64);if(!edge || !handle(edge))return false;}
            return true;
        };
        std::vector<std::vector<std::uint32_t>> after(node_count);std::vector<std::uint32_t> pending(node_count,0);
        // 輸出邊：節點輸出直接寫入圖輸出。
        if(!edges(40,48,[&](std::uint64_t e){const auto from=static_cast<std::uint32_t>(load(e,32)),slot=static_cast<std::uint32_t>(load(e+4,32)),target=static_cast<std::uint32_t>(load(e+8,32));
            if(from>=node_count || slot>=graph[from].out.size() || target>=compiled.output_count){reason="輸出邊";return false;}graph[from].out[slot]={DirectMLState::Ref::Output,target};return true;}))return false;
        if(!edges(24,32,[&](std::uint64_t e){const auto input=static_cast<std::uint32_t>(load(e,32)),to=static_cast<std::uint32_t>(load(e+4,32)),slot=static_cast<std::uint32_t>(load(e+8,32));
            if(input>=compiled.input_count || to>=node_count || !graph[to].op || slot>=graph[to].in.size()){reason="輸入邊";return false;}
            graph[to].in[slot]={DirectMLState::Ref::Input,input};graph[to].in_set[slot]=true;return true;}))return false;
        if(!edges(56,64,[&](std::uint64_t e){const auto from=static_cast<std::uint32_t>(load(e,32)),out=static_cast<std::uint32_t>(load(e+4,32)),to=static_cast<std::uint32_t>(load(e+8,32)),slot=static_cast<std::uint32_t>(load(e+12,32));
            if(from>=node_count || to>=node_count || out>=graph[from].out.size() || !graph[to].op || slot>=graph[to].in.size()){reason="中間邊";return false;}
            graph[to].in[slot]=graph[from].out[out];graph[to].in_set[slot]=true;after[from].push_back(to);++pending[to];return true;}))return false;
        // 圖輸入若被標記 OWNED_BY_DML，於 initializer 時複製成持續性資料。
        std::map<std::uint32_t,std::uint32_t> owned;
        for(auto& node:graph)if(node.op)for(std::size_t s=0;s<node.in.size();++s)
            if(node.in_set[s] && node.in[s].kind==DirectMLState::Ref::Input && (node.op->inputs[s].flags&dmlabi::DML_TENSOR_FLAG_OWNED_BY_DML)){
                const auto input=node.in[s].index;
                if(!owned.contains(input)){owned[input]=static_cast<std::uint32_t>(compiled.persistent_sizes.size());compiled.persistent_sizes.push_back(tensor_bytes(node.op->inputs[s]));compiled.owned.push_back({input,owned[input]});}
                node.in[s]={DirectMLState::Ref::Persistent,owned[input]};
            }
        std::vector<std::uint32_t> order,ready;for(std::uint32_t i=0;i<node_count;++i)if(!pending[i])ready.push_back(i);
        while(!ready.empty()){const auto n=ready.back();ready.pop_back();order.push_back(n);for(const auto next:after[n])if(!--pending[next])ready.push_back(next);}
        if(order.size()!=node_count){reason="圖有循環";return false;}
        for(const auto n:order){
            auto& node=graph[n];if(!node.op)continue;
            std::vector<KernelSpec> specs;if(!generate(*node.op,specs,reason)){reason=operator_name(*node.op)+"："+reason;return false;}
            for(auto& spec:specs){
                DirectMLState::Kernel kernel{std::move(spec.source),spec.threads,{},{}};
                for(const auto i:spec.inputs){if(!node.in_set[i]){reason="節點輸入未連接";return false;}kernel.inputs.push_back(node.in[i]);}
                for(const auto i:spec.outputs)kernel.outputs.push_back(node.out[i]);
                compiled.kernels.push_back(std::move(kernel));
            }
        }
        return true;
    };

    if(symbol=="DMLCreateDevice" || symbol=="DMLCreateDevice1"){
        const bool v1=symbol=="DMLCreateDevice1";const auto riid=arg(v1?3:2),out=arg(v1?4:3);
        if(!out)return finish(pointer_error);store(out,64,0);
        const auto d3d12=arg(0);if(!d3d12 || d3d12_device_of(d3d12)!=d3d12)return finish(invalid);
        if(v1 && u32(2)>claimed_feature_level)return finish(unsupported);
        const auto device=create(DmlKind::Device,0);dml.devices[device]={d3d12,u32(1)};
        return finish(give(device,riid,out));
    }
    const auto dot=symbol.find('.');if(dot==std::string_view::npos)return finish(not_implemented);
    const auto prefix=symbol.substr(0,dot+1),member=symbol.substr(dot+1);
    const auto self=arg(0);const auto found=dml.objects.find(self);
    if(found==dml.objects.end())return finish(invalid);
    // IUnknown／IDMLObject／IDMLDeviceChild。
    if(member=="QueryInterface")return finish(query(self,arg(1),arg(2)));
    if(member=="AddRef")return finish(++found->second.references);
    if(member=="Release"){
        const auto remaining=--found->second.references;
        if(!remaining){
            const auto device=found->second.device;dml.objects.erase(found);dml.devices.erase(self);dml.operators.erase(self);dml.compiled.erase(self);dml.initializers.erase(self);dml.tables.erase(self);
            if(device && dml.objects.contains(device) && !--dml.objects.at(device).references){dml.objects.erase(device);dml.devices.erase(device);}
        }
        return finish(remaining);
    }
    if(member=="SetName")return finish(ok);
    if(member=="GetPrivateData" || member=="SetPrivateData" || member=="SetPrivateDataInterface"){
        std::array<std::uint8_t,16> guid{};if(!arg(1))return finish(invalid);memory.read(GuestAddress{arg(1)},guid);auto& data=found->second.data;
        if(member=="SetPrivateDataInterface"){data.erase(guid);return finish(ok);}
        if(member=="SetPrivateData"){const auto size=u32(2);if(!size || !arg(3)){data.erase(guid);return finish(ok);}std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(3)},bytes);data[guid]=std::move(bytes);return finish(ok);}
        const auto entry=data.find(guid);if(!arg(2))return finish(invalid);if(entry==data.end()){store(arg(2),32,0);return finish(0x887a0002U);}
        const auto capacity=static_cast<std::uint32_t>(load(arg(2),32));store(arg(2),32,entry->second.size());
        if(!arg(3))return finish(ok);if(capacity<entry->second.size())return finish(0x887a0003U);memory.write(GuestAddress{arg(3)},entry->second);return finish(ok);
    }
    if(member=="GetDevice")return finish(query(device_of_child(self),arg(1),arg(2)));

    if(prefix=="Device."){
        const auto device=self;
        if(member=="CheckFeatureSupport"){
            const auto feature=u32(1),query_size=u32(2),support_size=u32(4);const auto query_data=arg(3),support_data=arg(5);
            if(feature==dmlabi::DML_FEATURE_TENSOR_DATA_TYPE_SUPPORT){
                if(query_size<4 || support_size<4 || !query_data || !support_data)return finish(invalid);
                store(support_data,32,msl_type(static_cast<std::uint32_t>(load(query_data,32)))?1U:0U);return finish(ok);
            }
            if(feature==dmlabi::DML_FEATURE_FEATURE_LEVELS){
                if(query_size<16 || support_size<4 || !query_data || !support_data)return finish(invalid);
                const auto count=static_cast<std::uint32_t>(load(query_data,32));const auto levels=load(query_data+8,64);std::uint32_t best=0;
                for(std::uint32_t i=0;i<count && levels;++i){const auto level=static_cast<std::uint32_t>(load(levels+i*4,32));if(level<=claimed_feature_level)best=std::max(best,level);}
                store(support_data,32,best?best:dmlabi::DML_FEATURE_LEVEL_1_0);return finish(ok);
            }
            return finish(invalid);
        }
        if(member=="CreateOperator"){
            if(!arg(3))return finish(pointer_error);store(arg(3),64,0);
            std::string reason;auto op=read_operation(arg(1),reason,false);if(!op){report("CreateOperator 描述不合法："+reason);return finish(invalid);}
            std::vector<KernelSpec> specs;if(!generate(*op,specs,reason)){report("尚未支援 "+operator_name(*op)+"（"+reason+"）");return finish(not_implemented);}
            const auto object=create(DmlKind::Operator,device);dml.operators[object]=std::move(op);
            return finish(give(object,arg(2),arg(3)));
        }
        if(member=="CompileOperator" || member=="CompileGraph"){
            const bool graph=member=="CompileGraph";const auto out=arg(4);
            if(!out)return finish(pointer_error);store(out,64,0);
            auto compiled=std::make_shared<DirectMLState::Compiled>();std::string reason;
            if(graph){if(!arg(1) || !compile_graph(arg(1),*compiled,reason)){report("CompileGraph："+reason);return finish(reason.find("尚未支援")!=std::string::npos?not_implemented:invalid);}}
            else{
                const auto op=dml.operators.find(arg(1));if(op==dml.operators.end())return finish(invalid);
                if(!compile_single(op->second,*compiled,reason)){report("CompileOperator："+reason);return finish(not_implemented);}
            }
            const auto object=create(DmlKind::Compiled,device);dml.compiled[object]=std::move(compiled);
            return finish(give(object,arg(3),out));
        }
        if(member=="CreateOperatorInitializer"){
            if(!arg(4))return finish(pointer_error);store(arg(4),64,0);
            std::vector<std::uint64_t> ops;for(std::uint32_t i=0;i<u32(1);++i){const auto op=load(arg(2)+i*8,64);if(!dml.compiled.contains(op))return finish(invalid);ops.push_back(op);}
            const auto object=create(DmlKind::Initializer,device);dml.initializers[object]=std::move(ops);
            return finish(give(object,arg(3),arg(4)));
        }
        if(member=="CreateCommandRecorder"){
            if(!arg(2))return finish(pointer_error);store(arg(2),64,0);
            return finish(give(create(DmlKind::Recorder,device),arg(1),arg(2)));
        }
        if(member=="CreateBindingTable"){
            if(!arg(3))return finish(pointer_error);store(arg(3),64,0);
            const auto object=create(DmlKind::Table,device);auto& table=dml.tables[object];bind_range(table,arg(1));
            return finish(give(object,arg(2),arg(3)));
        }
        if(member=="Evict" || member=="MakeResident" || member=="GetDeviceRemovedReason")return finish(ok);
        if(member=="GetParentDevice"){
            // 轉交 D3D12 裝置自身的 QueryInterface（客體 vtable），其回傳值即為本方法的 HRESULT。
            if(!arg(2))return finish(pointer_error);store(arg(2),64,0);
            const auto parent=dml.devices.at(device).d3d12;
            start_callbacks(state,memory,{{load(load(parent,64),64),{parent,arg(1),arg(2)}}},Continuation::ReturnCallback);return std::nullopt;
        }
    }
    if(prefix=="Compiled." || prefix=="Initializer."){
        if(member=="GetBindingProperties"){
            // DML_BINDING_PROPERTIES：RequiredDescriptorCount、TemporaryResourceSize、PersistentResourceSize（以隱藏 RetVal 回傳）。
            // 中間張量與持續性資料由 DML 內部配置，暫存／持續性資源大小回報 0。
            const auto out=arg(1);std::uint32_t descriptors=1;
            if(const auto compiled=dml.compiled.find(self);compiled!=dml.compiled.end())descriptors=std::max(1U,compiled->second->input_count+compiled->second->output_count);
            if(const auto init=dml.initializers.find(self);init!=dml.initializers.end()){std::uint32_t total=0;for(const auto op:init->second)total+=dml.compiled.at(op)->input_count;descriptors=std::max(1U,total);}
            store(out,32,descriptors);store(out+4,32,0);store(out+8,64,0);store(out+16,64,0);arg.return_to(state,out);return std::optional<std::uint32_t>{};
        }
        if(member=="Reset" && prefix=="Initializer."){
            std::vector<std::uint64_t> ops;for(std::uint32_t i=0;i<u32(1);++i){const auto op=load(arg(2)+i*8,64);if(!dml.compiled.contains(op))return finish(invalid);ops.push_back(op);}
            dml.initializers[self]=std::move(ops);return finish(ok);
        }
    }
    if(prefix=="Table."){
        auto& table=dml.tables[self];
        const auto buffer_at=[&](std::uint64_t desc)->DirectMLState::Binding{return {load(desc,64),load(desc+8,64),load(desc+16,64)};};
        if(member=="BindInputs" || member=="BindOutputs"){
            const bool inputs=member=="BindInputs";const auto count=u32(1);const auto descs=arg(2);
            auto& contents=*table.contents;auto& singles=inputs?contents.inputs:contents.outputs;singles.assign(count,{});if(inputs)contents.input_arrays.assign(count,{});
            for(std::uint32_t i=0;i<count && descs;++i){
                const auto type=static_cast<std::uint32_t>(load(descs+i*16,32));const auto desc=load(descs+i*16+8,64);if(!desc)continue;
                if(type==dmlabi::DML_BINDING_TYPE_BUFFER)singles[i]=buffer_at(desc);
                else if(type==dmlabi::DML_BINDING_TYPE_BUFFER_ARRAY && inputs){
                    const auto n=static_cast<std::uint32_t>(load(desc,32));const auto bindings=load(desc+8,64);
                    for(std::uint32_t b=0;b<n && bindings && b<4096;++b)contents.input_arrays[i].push_back(buffer_at(bindings+b*24));
                }
            }
            return finish(0);
        }
        if(member=="BindTemporaryResource" || member=="BindPersistentResource")return finish(0);
        if(member=="Reset"){table=DirectMLState::Table{};bind_range(table,arg(1));return finish(ok);}
    }
    if(prefix=="Recorder." && member=="RecordDispatch"){
        const auto list=arg(1),dispatchable=arg(2),binding=arg(3);const auto table=dml.tables.find(binding);
        if(table==dml.tables.end()){report("RecordDispatch 缺少 binding table");return finish(0);}
        auto state_ptr=directml_;
        // 共用：每個 Metal 後端一份核心、中間張量與持續性資料。
        const auto runtime_of=[state_ptr](DirectMLState::Compiled& compiled,NativeGraphics& native)->DirectMLState::Runtime&{
            auto& runtime=compiled.runtime[&native];
            if(runtime.shaders.empty()){
                for(const auto& kernel:compiled.kernels){
                    GraphicsModernShader source;source.source=kernel.source;source.entry="main0";source.threads={256,1,1};std::uint32_t slot=0;
                    for(std::size_t i=0;i<kernel.inputs.size();++i,++slot)source.bindings.push_back(GraphicsBinding{3,static_cast<std::uint32_t>(i),slot});
                    for(std::size_t i=0;i<kernel.outputs.size();++i,++slot)source.bindings.push_back(GraphicsBinding{4,static_cast<std::uint32_t>(i),slot});
                    runtime.shaders.push_back(native.graphics_compute_source(source));
                }
                const auto make=[&](std::uint64_t size){GraphicsResourceDesc desc;desc.dimension=1;desc.width=static_cast<std::uint32_t>(std::max<std::uint64_t>((size+15)&~std::uint64_t{15},16));desc.bind=0x8c;return native.graphics_resource(desc);};
                for(const auto size:compiled.intermediate_sizes)runtime.intermediates.push_back(make(size));
                for(const auto size:compiled.persistent_sizes)runtime.persistent.push_back(make(size));
            }
            if(!runtime.constants_written){
                for(const auto& [index,data]:compiled.constants){std::vector<std::uint8_t> padded(data);padded.resize(runtime.persistent[index]->desc.width);
                    native.graphics_write(GraphicsView{runtime.persistent[index]},padded,runtime.persistent[index]->desc.width,runtime.persistent[index]->desc.width);}
                runtime.constants_written=true;
            }
            return runtime;
        };
        if(const auto init=dml.initializers.find(dispatchable);init!=dml.initializers.end()){
            // 初始化：把 OWNED_BY_DML 的輸入（每個運算子一個 BUFFER_ARRAY）以 GPU 複製到持續性資料。
            std::vector<std::shared_ptr<DirectMLState::Compiled>> ops;for(const auto op:init->second)ops.push_back(dml.compiled.at(op));
            const auto contents=table->second.contents;bool needed=false;for(const auto& op:ops)if(!op->owned.empty() || !op->constants.empty())needed=true;
            if(!needed)return finish(0);
            const bool recorded=d3d12_record(list,[state_ptr,ops,contents,runtime_of](NativeGraphics& native,const std::function<std::shared_ptr<GraphicsResource>(std::uint64_t)>& resource_of){
                const auto& arrays=contents->input_arrays;
                for(std::size_t j=0;j<ops.size();++j){
                    auto& runtime=runtime_of(*ops[j],native);
                    for(const auto& [input,persistent]:ops[j]->owned){
                        if(j>=arrays.size() || input>=arrays[j].size() || !arrays[j][input].resource)continue;
                        const auto& binding=arrays[j][input];const auto words=(std::min<std::uint64_t>(binding.size?binding.size:ops[j]->persistent_sizes[persistent],ops[j]->persistent_sizes[persistent])+3)/4;
                        auto& shader=state_ptr->copy_shaders[words^reinterpret_cast<std::uintptr_t>(&native)];
                        if(!shader){
                            GraphicsModernShader source;source.entry="main0";source.threads={256,1,1};
                            source.source="#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(device const uint* src [[buffer(0)]],device uint* dst [[buffer(1)]],uint gid [[thread_position_in_grid]]){if(gid<"+std::to_string(words)+"u)dst[gid]=src[gid];}\n";
                            source.bindings={GraphicsBinding{3,0,0},GraphicsBinding{4,0,1}};shader=native.graphics_compute_source(source);
                        }
                        GraphicsStage stage;stage.shader=shader;stage.resources[0]=GraphicsView{resource_of(binding.resource),0,0,1,0,1,static_cast<std::uint32_t>(binding.offset)};
                        stage.unordered[0]=GraphicsView{runtime.persistent[persistent]};
                        native.graphics_dispatch(stage,{static_cast<std::uint32_t>((words+255)/256),1,1});
                    }
                }
            });
            if(!recorded)report("RecordDispatch 的命令清單不是開啟中的 D3D12 清單");
            return finish(0);
        }
        const auto compiled=dml.compiled.find(dispatchable);
        if(compiled==dml.compiled.end()){report("RecordDispatch 參數不合法");return finish(0);}
        // 執行時才讀取綁定範圍的內容、編譯核心（每個 Metal 後端一份）並依序 dispatch。
        auto job=compiled->second;const auto contents=table->second.contents;
        const bool recorded=d3d12_record(list,[job,contents,runtime_of](NativeGraphics& native,const std::function<std::shared_ptr<GraphicsResource>(std::uint64_t)>& resource_of){
            auto& runtime=runtime_of(*job,native);const auto& inputs=contents->inputs;const auto& outputs=contents->outputs;
            const auto view=[&](const DirectMLState::Ref& ref)->GraphicsView{
                switch(ref.kind){
                case DirectMLState::Ref::Input:if(ref.index<inputs.size() && inputs[ref.index].resource)return GraphicsView{resource_of(inputs[ref.index].resource),0,0,1,0,1,static_cast<std::uint32_t>(inputs[ref.index].offset)};return {};
                case DirectMLState::Ref::Output:if(ref.index<outputs.size() && outputs[ref.index].resource)return GraphicsView{resource_of(outputs[ref.index].resource),0,0,1,0,1,static_cast<std::uint32_t>(outputs[ref.index].offset)};return {};
                case DirectMLState::Ref::Intermediate:return GraphicsView{runtime.intermediates.at(ref.index)};
                case DirectMLState::Ref::Persistent:return GraphicsView{runtime.persistent.at(ref.index)};
                }
                return {};
            };
            for(std::size_t k=0;k<job->kernels.size();++k){
                const auto& kernel=job->kernels[k];GraphicsStage stage;stage.shader=runtime.shaders[k];
                for(std::size_t i=0;i<kernel.inputs.size() && i<stage.resources.size();++i)stage.resources[i]=view(kernel.inputs[i]);
                for(std::size_t i=0;i<kernel.outputs.size() && i<stage.unordered.size();++i)stage.unordered[i]=view(kernel.outputs[i]);
                const auto groups=static_cast<std::uint32_t>((kernel.threads+255)/256);
                if(groups)native.graphics_dispatch(stage,{groups,1,1});
            }
        });
        if(!recorded)report("RecordDispatch 的命令清單不是開啟中的 D3D12 清單");
        return finish(0);
    }
    report("尚未支援 "+std::string(symbol));
    return finish(not_implemented);
}
}
