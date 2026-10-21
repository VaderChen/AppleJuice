#include "x64arm/windows_graphics.hpp"
#include "d3d9_shader.hpp"
#include <vkd3d_shader.h>
#include <spirv_msl.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace x64arm {
std::vector<std::array<std::uint32_t,2>> graphics_legacy_semantics(std::span<const std::uint8_t> bytes,bool output){
    struct Scan {
        vkd3d_shader_scan_signature_info signatures{};
        char* messages{};
        ~Scan(){vkd3d_shader_free_scan_signature_info(&signatures);vkd3d_shader_free_messages(messages);}
    } scan;
    scan.signatures.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.source={bytes.data(),bytes.size()};info.source_type=VKD3D_SHADER_SOURCE_D3D_BYTECODE;info.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;info.next=&scan.signatures;info.log_level=VKD3D_SHADER_LOG_WARNING;
    if(vkd3d_shader_scan(&info,&scan.messages)<0){scan.signatures.input={};scan.signatures.output={};scan.signatures.patch_constant={};throw std::invalid_argument("D3D9 shader 語意掃描失敗："+std::string(scan.messages?scan.messages:""));}
    std::vector<std::array<std::uint32_t,2>> result;const auto& signature=output?scan.signatures.output:scan.signatures.input;
    constexpr std::array<const char*,14> names{"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL","PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG","DEPTH","SAMPLE"};
    for(unsigned i=0;i<signature.element_count;++i){const auto& e=signature.elements[i];unsigned usage=0;for(;usage<names.size();++usage)if(std::string_view(e.semantic_name)==names[usage])break;if(usage==names.size())throw std::invalid_argument("D3D9 shader 含未知語意");result.push_back({usage,e.semantic_index});}
    return result;
}
// SM1–3 的暫存器、宣告與控制流程由同一 bytecode 編譯器處理；不依範例改写 shader。
GraphicsLegacyShader graphics_legacy_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes){
    static std::mutex lock;const std::lock_guard guard(lock);
    if(stage>1 || bytes.size()<8 || bytes.size()%4)throw std::invalid_argument("D3D9 shader 位元碼長度／階段不合法");
    std::uint32_t version{};std::memcpy(&version,bytes.data(),4);
    if((version>>16)!=(stage?0xffffU:0xfffeU) || ((version>>8)&255)>3)throw std::invalid_argument("D3D9 shader 版本與階段不符");
    struct Scan {
        vkd3d_shader_scan_descriptor_info descriptors{};
        vkd3d_shader_scan_signature_info signatures{};
        vkd3d_shader_code code{};
        char* messages{};
        ~Scan(){vkd3d_shader_free_scan_descriptor_info(&descriptors);vkd3d_shader_free_scan_signature_info(&signatures);vkd3d_shader_free_shader_code(&code);vkd3d_shader_free_messages(messages);}
    } scan;
    scan.descriptors.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;scan.descriptors.next=&scan.signatures;
    scan.signatures.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.source={bytes.data(),bytes.size()};info.source_type=VKD3D_SHADER_SOURCE_D3D_BYTECODE;info.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;info.next=&scan.descriptors;info.log_level=VKD3D_SHADER_LOG_WARNING;
    if(vkd3d_shader_scan(&info,&scan.messages)<0){
        // scan 失敗時，vkd3d 已釋放掃描輸出；不可再次由 RAII 釋放。
        scan.descriptors.descriptors=nullptr;scan.descriptors.descriptor_count=0;
        scan.signatures.input={};scan.signatures.output={};scan.signatures.patch_constant={};
        throw std::invalid_argument("D3D9 shader 掃描失敗："+std::string(scan.messages?scan.messages:""));
    }
    vkd3d_shader_free_messages(scan.messages);scan.messages=nullptr;
    GraphicsLegacyShader result;std::vector<vkd3d_shader_resource_binding> bindings;
    for(unsigned i=0;i<scan.descriptors.descriptor_count;++i){const auto& d=scan.descriptors.descriptors[i];
        if(d.count!=1 || d.register_space || (d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV?d.register_index>=3:d.register_index>=16))throw std::invalid_argument("D3D9 shader 資源宣告不合法");
        vkd3d_shader_resource_binding binding{};binding.type=d.type;binding.register_index=d.register_index;binding.shader_visibility=VKD3D_SHADER_VISIBILITY_ALL;binding.binding.count=1;
        binding.binding.binding=d.register_index+(d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV?0:d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV?16:32);binding.flags=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV?VKD3D_SHADER_BINDING_FLAG_BUFFER:VKD3D_SHADER_BINDING_FLAG_IMAGE;bindings.push_back(binding);
        result.bindings.push_back({d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV?0U:d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV?1U:2U,d.register_index,d.register_index});
    }
    vkd3d_shader_spirv_target_info target{};target.type=VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;target.environment=VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_0;
    vkd3d_shader_interface_info interface{};interface.type=VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;interface.next=&target;interface.bindings=bindings.data();interface.binding_count=static_cast<unsigned>(bindings.size());info.next=&interface;
    if(vkd3d_shader_compile(&info,&scan.code,&scan.messages)<0){scan.code={};throw std::invalid_argument("D3D9 shader 轉譯失敗："+std::string(scan.messages?scan.messages:""));}
    std::vector<std::uint32_t> words(scan.code.size/4);std::memcpy(words.data(),scan.code.code,scan.code.size);spirv_cross::CompilerMSL compiler(std::move(words));
    const auto model=stage?spv::ExecutionModelFragment:spv::ExecutionModelVertex;
    auto options=compiler.get_msl_options();options.platform=spirv_cross::CompilerMSL::Options::macOS;options.set_msl_version(2,3);options.pad_fragment_output_components=true;compiler.set_msl_options(options);
    for(const auto& b:bindings){spirv_cross::MSLResourceBinding binding{};binding.stage=model;binding.desc_set=0;binding.binding=b.binding.binding;binding.msl_buffer=b.register_index;binding.msl_texture=b.register_index;binding.msl_sampler=b.register_index;compiler.add_msl_resource_binding(binding);}
    // 舊版 VS 輸出與 PS 輸入的 register 編號可不同；依語意建立穩定連接位置。
    const auto location=[](const char* name,unsigned index){const std::string semantic=name;const std::array<const char*,14> names{"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL","PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG","DEPTH","SAMPLE"};for(unsigned i=0;i<names.size();++i)if(semantic==names[i])return i*16+index;throw std::invalid_argument("D3D9 shader 含未知語意："+semantic);};
    const auto resources=compiler.get_shader_resources();
    const auto remap=[&](const auto& resources,const auto& signature,bool input){for(const auto& resource:resources){const auto slot=compiler.get_decoration(resource.id,spv::DecorationLocation);const auto found=std::find_if(signature.elements,signature.elements+signature.element_count,[&](const auto& e){return e.register_index==slot && e.sysval_semantic==VKD3D_SHADER_SV_NONE;});if(found==signature.elements+signature.element_count)continue;
        if(stage==0 && input)result.inputs.push_back({found->semantic_name,found->semantic_index,slot});
        else if((stage==0 && !input) || (stage==1 && input)){
            const auto varying_location=location(found->semantic_name,found->semantic_index);
            compiler.set_decoration(resource.id,spv::DecorationLocation,varying_location);
            // D3D9 的跨階段暫存器固定為四分量；讀寫遮罩不改變介面型別。
            // Metal 要求兩端型別一致，不能把只讀 xy 的 PS 接成 float2。
            spirv_cross::MSLShaderInterfaceVariable varying;varying.location=varying_location;varying.vecsize=4;
            if(input)compiler.add_msl_shader_input(varying);else compiler.add_msl_shader_output(varying);
        }
    }};
    remap(resources.stage_inputs,scan.signatures.input,true);remap(resources.stage_outputs,scan.signatures.output,false);
    compiler.rename_entry_point("main","xlatMtlMain",model);result.source=compiler.compile();result.entry="xlatMtlMain";return result;
}
}
