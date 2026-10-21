#include "x64arm/glsl.hpp"
#if defined(__APPLE__)
#include "x64arm/resource_cache.hpp"
#include <cstdlib>
#include <cstdio>
#endif
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Include/intermediate.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <glslang/MachineIndependent/iomapper.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
namespace x64arm {
namespace {
std::mutex compiler_mutex;
void initialize(){static const bool initialized=glslang::InitializeProcess();if(!initialized)throw std::runtime_error("glslang 初始化失敗");}
EShLanguage language(std::uint32_t stage){if(stage==0x8b31)return EShLangVertex;if(stage==0x8b30)return EShLangFragment;throw std::invalid_argument("尚未支援此 GLSL shader stage");}
using Defaults=std::map<std::string,std::vector<std::uint32_t>>;
class Initializers:public glslang::TIntermTraverser {
public:
    Defaults values;bool unsupported{};
    void visitSymbol(glslang::TIntermSymbol* symbol)override{
        if(symbol->getQualifier().storage!=glslang::EvqUniform)return;
        const auto& array=symbol->getConstArray();if(array.empty())return;
        auto& out=values[std::string(symbol->getName().c_str())];out.clear();
        for(int n=0;n<array.size();++n){const auto& c=array[n];switch(c.getType()){
        case glslang::EbtFloat:case glslang::EbtDouble:out.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(c.getDConst())));break;
        case glslang::EbtInt:out.push_back(static_cast<std::uint32_t>(c.getIConst()));break;
        case glslang::EbtUint:out.push_back(c.getUConst());break;
        case glslang::EbtBool:out.push_back(c.getBConst()?1U:0U);break;
        default:unsupported=true;out.push_back(0);break;}}
    }
};
std::unique_ptr<glslang::TShader> parse(const GlslSource& source,bool spirv,std::string& log,Defaults* defaults=nullptr){
    auto shader=std::make_unique<glslang::TShader>(language(source.stage));const char* text=source.source.c_str();const auto size=static_cast<int>(source.source.size());shader->setStringsWithLengths(&text,&size,1);if(!source.preamble.empty())shader->setPreamble(source.preamble.c_str());
    if(spirv){shader->setEnvInput(glslang::EShSourceGlsl,language(source.stage),glslang::EShClientVulkan,100);shader->setEnvInputVulkanRulesRelaxed();shader->setEnvClient(glslang::EShClientVulkan,glslang::EShTargetVulkan_1_1);shader->setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_3);shader->setAutoMapBindings(true);shader->setAutoMapLocations(true);shader->setGlobalUniformBlockName("x64arm_uniforms");shader->setGlobalUniformSet(0);shader->setGlobalUniformBinding(0);}
    const auto flags=spirv?static_cast<EShMessages>(EShMsgSpvRules|EShMsgVulkanRules):EShMsgDefault;
    const bool ok=shader->parse(GetDefaultResources(),110,false,flags);if(!ok || !spirv){log+=shader->getInfoLog();log+=shader->getInfoDebugLog();}if(!ok)return {};
    if(defaults){Initializers visitor;shader->getIntermediate()->getTreeRoot()->traverse(&visitor);if(visitor.unsupported)throw std::invalid_argument("Metal 不支援此 uniform 初始化型別");defaults->insert(visitor.values.begin(),visitor.values.end());}return shader;
}
bool map_interfaces(glslang::TProgram& program) {
    // Vulkan 預設 mapper 會各 stage 獨立編號；OpenGL 的隱式 varying 必須
    // 依名稱跨 stage 對齊，並保留顯式 location、矩陣及陣列佔用的 slots。
    auto* first=program.getIntermediate(EShLangVertex);
    if(!first)first=program.getIntermediate(EShLangFragment);
    if(!first)return false;
    glslang::TDefaultGlslIoResolver resolver(*first);glslang::TGlslIoMapper mapper;
    return program.mapIO(&resolver,&mapper);
}
unsigned array_count(const spirv_cross::SPIRType& type){std::uint64_t count=1;for(auto size:type.array){if(!size || count>65536/size)throw std::invalid_argument("GLSL uniform array 超過上限");count*=size;}return static_cast<unsigned>(count);}
GlslStage translate(glslang::TIntermediate& intermediate,std::uint32_t stage,const Defaults& defaults){
    std::vector<unsigned> words;glslang::SpvOptions options;options.disableOptimizer=true;glslang::GlslangToSpv(intermediate,words,&options);
    spirv_cross::CompilerMSL compiler(words);auto opts=compiler.get_msl_options();opts.platform=spirv_cross::CompilerMSL::Options::macOS;opts.set_msl_version(2,3);opts.argument_buffers=true;opts.argument_buffers_tier=spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;opts.force_active_argument_buffer_resources=true;opts.pad_fragment_output_components=true;compiler.set_msl_options(opts);
    auto common=compiler.get_common_options();common.vertex.fixup_clipspace=true;compiler.set_common_options(common);
    const auto resources=compiler.get_shader_resources();GlslStage out;out.stage=stage;
    if(!resources.storage_buffers.empty() || !resources.storage_images.empty())throw std::invalid_argument("目前 GLSL 繪圖路徑尚未提供 storage buffer/image");
    for(const auto& uniform:resources.uniform_buffers){if(uniform.name!="x64arm_uniforms")throw std::invalid_argument("目前 GLSL 路徑尚未提供具名 uniform block");

    }
    out.msl=compiler.compile();if(stage==0x8b31){spirv_cross::CompilerMSL flipped(words);flipped.set_msl_options(opts);common.vertex.flip_vert_y=true;flipped.set_common_options(common);out.flipped_msl=flipped.compile();}out.entry=compiler.get_cleansed_entry_point_name("main",stage==0x8b31?spv::ExecutionModelVertex:spv::ExecutionModelFragment);
    for(const auto& attr:resources.stage_inputs){const auto& type=compiler.get_type(attr.type_id);out.attributes.push_back({attr.name,compiler.get_decoration(attr.id,spv::DecorationLocation),type.vecsize,type.columns});}
    for(const auto& sampler:resources.sampled_images){const auto& type=compiler.get_type(sampler.type_id);out.samplers.push_back({sampler.name,compiler.get_automatic_msl_resource_binding(sampler.id),compiler.get_automatic_msl_resource_binding_secondary(sampler.id),array_count(type)});}
    for(const auto& block:resources.uniform_buffers){const auto& type=compiler.get_type(block.base_type_id);out.buffer_index=compiler.get_automatic_msl_resource_binding(block.id);out.initial_uniforms.resize(compiler.get_declared_struct_size(type));
        for(unsigned index=0;index<type.member_types.size();++index){const auto& member=compiler.get_type(type.member_types[index]);GlslUniform u;u.name=compiler.get_member_name(type.self,index);u.offset=compiler.type_struct_member_offset(type,index);u.components=member.vecsize;u.columns=member.columns;u.count=array_count(member);
            using Type=spirv_cross::SPIRType;u.type=member.basetype==Type::Float?0x1406:member.basetype==Type::Int?0x1404:member.basetype==Type::UInt?0x1405:member.basetype==Type::Boolean?0x8b56:0;
            if(!u.type || member.width!=32)throw std::invalid_argument("目前 GLSL uniform 型別尚未提供 Metal layout");
            if(!member.array.empty())u.array_stride=compiler.type_struct_member_array_stride(type,index);if(u.columns>1)u.matrix_stride=compiler.type_struct_member_matrix_stride(type,index);
            if(const auto found=defaults.find(u.name);found!=defaults.end()){
                const auto& initial=found->second;for(unsigned a=0;a<u.count;++a)for(unsigned c=0;c<u.columns;++c)for(unsigned r=0;r<u.components;++r){const auto source=(a*u.columns+c)*u.components+r;const auto at=u.offset+a*u.array_stride+c*u.matrix_stride+r*4;
                    if(source>=initial.size() || at+4>out.initial_uniforms.size())throw std::invalid_argument("GLSL uniform 初始化 layout 不一致");std::memcpy(out.initial_uniforms.data()+at,&initial[source],4);}}
            out.uniforms.push_back(std::move(u));
        }
    }return out;
}
}
static GlslResult validate_glsl_uncached(const GlslSource& source){std::lock_guard lock(compiler_mutex);GlslResult result;try{initialize();result.success=bool(parse(source,false,result.log));}catch(const std::exception& e){result.log+=e.what();}return result;}
static GlslResult link_glsl_uncached(std::span<const GlslSource> sources){std::lock_guard lock(compiler_mutex);GlslResult result;try{
    initialize();std::vector<std::unique_ptr<glslang::TShader>> original,shaders;glslang::TProgram validation,program;std::map<std::uint32_t,Defaults> defaults;
    for(const auto& source:sources){auto shader=parse(source,false,result.log,&defaults[source.stage]);if(!shader)return result;validation.addShader(shader.get());original.push_back(std::move(shader));}
    if(!validation.link(EShMsgDefault)){result.log+=validation.getInfoLog();return result;}
    for(const auto& source:sources){auto shader=parse(source,true,result.log);if(!shader)return result;program.addShader(shader.get());shaders.push_back(std::move(shader));}
    if(!program.link(static_cast<EShMessages>(EShMsgSpvRules|EShMsgVulkanRules)) || !map_interfaces(program)){result.log+=program.getInfoLog();return result;}
    // 以 SPIR-V 的 active builtin 反射決定狀態 adapter，避免依原始碼字串猜測使用情況。
    unsigned clip_count=0;bool point_size=false;
    if(auto* intermediate=program.getIntermediate(EShLangVertex)){
        std::vector<unsigned> words;glslang::GlslangToSpv(*intermediate,words);spirv_cross::Compiler reflection(words);reflection.update_active_builtins();
        if(reflection.has_active_builtin(spv::BuiltInClipDistance,spv::StorageClassOutput))for(const auto& resource:reflection.get_shader_resources().builtin_outputs)if(resource.builtin==spv::BuiltInClipDistance)clip_count=array_count(reflection.get_type(resource.value_type_id));
        point_size=reflection.has_active_builtin(spv::BuiltInPointSize,spv::StorageClassOutput);
    }
    std::vector<std::unique_ptr<glslang::TShader>> adjusted_shaders;std::unique_ptr<glslang::TProgram> adjusted;std::vector<GlslSource> adjusted_sources;
    std::string prefix="x64arm_native_";while(std::any_of(sources.begin(),sources.end(),[&](const auto& source){return source.source.find(prefix)!=std::string::npos;}))prefix+="x";
    if(clip_count || point_size){
        adjusted=std::make_unique<glslang::TProgram>();adjusted_sources.assign(sources.begin(),sources.end());bool wrapper=false;
        for(auto& source:adjusted_sources){if(source.stage==0x8b31){source.preamble="#define main "+prefix+"main\n";
            if(!wrapper){source.source+="\n#undef main\n";if(clip_count)source.source+="uniform uint "+prefix+"clip_mask;\n";if(point_size)source.source+="uniform int "+prefix+"point_size;\n";
                source.source+="void "+prefix+"main();\nvoid main(){"+prefix+"main();";
                for(unsigned i=0;i<clip_count;++i)source.source+="if(("+prefix+"clip_mask & "+std::to_string(1U<<i)+"u)==0u)gl_ClipDistance["+std::to_string(i)+"]=1.;";
                if(point_size)source.source+="if("+prefix+"point_size==0)gl_PointSize=1.;";source.source+="}\n";wrapper=true;}
        }auto shader=parse(source,true,result.log);if(!shader)return result;adjusted->addShader(shader.get());adjusted_shaders.push_back(std::move(shader));}
        if(!adjusted->link(static_cast<EShMessages>(EShMsgSpvRules|EShMsgVulkanRules)) || !map_interfaces(*adjusted)){result.log+=adjusted->getInfoLog();return result;}
    }
    for(const auto stage:{0x8b31U,0x8b30U})if(auto* intermediate=(adjusted?adjusted.get():&program)->getIntermediate(language(stage))){auto translated=translate(*intermediate,stage,defaults[stage]);if(stage==0x8b31){if(clip_count)translated.clip_mask=prefix+"clip_mask";if(point_size)translated.point_size_enabled=prefix+"point_size";}result.stages.push_back(std::move(translated));}
    result.success=!result.stages.empty();
}catch(const std::exception& e){result.log+=e.what();result.stages.clear();}return result;}
#if defined(__APPLE__)
namespace {
#include "glsl_resource_cache.inc"
}
#endif
GlslResult validate_glsl(const GlslSource& source){
#if defined(__APPLE__)
    return cached_shader_resource(std::span(&source,1),false,[&]{return validate_glsl_uncached(source);});
#else
    return validate_glsl_uncached(source);
#endif
}
GlslResult link_glsl(std::span<const GlslSource> sources){
#if defined(__APPLE__)
    return cached_shader_resource(sources,true,[&]{return link_glsl_uncached(sources);});
#else
    return link_glsl_uncached(sources);
#endif
}
}
