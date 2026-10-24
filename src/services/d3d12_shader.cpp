#include "x64arm/windows_graphics.hpp"
#include <vkd3d_shader.h>
#include <spirv_msl.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <cstring>
#include <map>
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
GraphicsModernShader graphics_modern_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes){
    static std::mutex lock;const std::lock_guard guard(lock);
    if(stage!=0 && stage!=1 && stage!=5)throw std::invalid_argument("現代著色器目前支援 VS／PS／CS");
    if(!graphics_modern_shader_required(bytes))throw std::invalid_argument("著色器不是 DXIL 或 SM5.1 DXBC");
    if(const char* directory=std::getenv("X64ARM_DUMP_DXBC");directory && *directory){
        // 與 SM4／5 相同的診斷開關：保存原始容器，供 d3d12_shader_corpus 離線重現轉譯。
        static std::atomic<std::uint64_t> sequence{};
        const auto path=std::filesystem::path(directory)/(std::to_string(getpid())+"-"+std::to_string(++sequence)+"-stage"+std::to_string(stage)+".dxil");
        std::filesystem::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    }
    bool dxil=false;
    for(unsigned i=0;i<word(bytes,28);++i)if(word(bytes,word(bytes,32+4*i))==0x4c495844U)dxil=true;
    struct Scan {
        vkd3d_shader_scan_descriptor_info descriptors{};vkd3d_shader_scan_signature_info signatures{};vkd3d_shader_code code{};char* messages{};
        ~Scan(){vkd3d_shader_free_scan_descriptor_info(&descriptors);vkd3d_shader_free_scan_signature_info(&signatures);vkd3d_shader_free_shader_code(&code);vkd3d_shader_free_messages(messages);}
    } scan;
    scan.descriptors.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;scan.descriptors.next=&scan.signatures;
    scan.signatures.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    // Apple GPU 的 SIMD group 對應 D3D wave（寬 32），MSL 支援 64 位元整數但不支援 double。
    // 無格式型別 UAV 讀取對應 Metal read_write 紋理。threadgroup 零初始化暫不開：SPIRV-Cross 產生的初始化碼引用未宣告的 gl_WorkGroupSize。
    const std::array<vkd3d_shader_compile_option,3> options{{{VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV,VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV_STORAGE_BUFFER},
        {VKD3D_SHADER_COMPILE_OPTION_FEATURE,VKD3D_SHADER_COMPILE_OPTION_FEATURE_INT64|VKD3D_SHADER_COMPILE_OPTION_FEATURE_WAVE_OPS},
        {VKD3D_SHADER_COMPILE_OPTION_TYPED_UAV,VKD3D_SHADER_COMPILE_OPTION_TYPED_UAV_READ_FORMAT_UNKNOWN}}};
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.source={bytes.data(),bytes.size()};
    info.source_type=dxil?VKD3D_SHADER_SOURCE_DXBC_DXIL:VKD3D_SHADER_SOURCE_DXBC_TPF;info.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;
    info.options=options.data();info.option_count=static_cast<unsigned>(options.size());info.log_level=VKD3D_SHADER_LOG_WARNING;info.next=&scan.descriptors;
    if(vkd3d_shader_scan(&info,&scan.messages)<0){
        scan.descriptors.descriptors=nullptr;scan.descriptors.descriptor_count=0;scan.signatures.input={};scan.signatures.output={};scan.signatures.patch_constant={};
        throw std::invalid_argument("現代著色器掃描失敗："+std::string(scan.messages?scan.messages:""));
    }
    vkd3d_shader_free_messages(scan.messages);scan.messages=nullptr;
    // 每個 D3D 描述元給一個 SPIR-V binding；register space 0 對應 D3D11 的 t／s／u／b 暫存器槽。
    struct Descriptor {vkd3d_shader_descriptor_type type;unsigned register_index,count;bool buffer;};
    std::vector<vkd3d_shader_resource_binding> bindings;std::map<unsigned,Descriptor> by_binding;
    for(unsigned i=0;i<scan.descriptors.descriptor_count;++i){
        const auto& d=scan.descriptors.descriptors[i];
        if(d.register_space)throw std::invalid_argument("現代著色器尚不支援 register space "+std::to_string(d.register_space));
        if(!d.count || d.count==~0U || d.count>128)throw std::invalid_argument("現代著色器尚不支援無界或過大的描述元陣列");
        if(d.flags&VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_COUNTER)throw std::invalid_argument("現代著色器尚不支援 UAV counter");
        const bool buffer=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV || d.resource_type==VKD3D_SHADER_RESOURCE_BUFFER;
        vkd3d_shader_resource_binding binding{};binding.type=d.type;binding.register_space=0;binding.register_index=d.register_index;
        binding.shader_visibility=VKD3D_SHADER_VISIBILITY_ALL;binding.binding.set=0;binding.binding.binding=static_cast<unsigned>(bindings.size());binding.binding.count=d.count;
        binding.flags=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER?0U:buffer?VKD3D_SHADER_BINDING_FLAG_BUFFER:VKD3D_SHADER_BINDING_FLAG_IMAGE;
        by_binding.emplace(binding.binding.binding,Descriptor{d.type,d.register_index,d.count,buffer});bindings.push_back(binding);
    }
    vkd3d_shader_spirv_target_info target{};target.type=VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;target.environment=VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;
    vkd3d_shader_interface_info interface{};interface.type=VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;interface.next=&target;
    interface.bindings=bindings.data();interface.binding_count=static_cast<unsigned>(bindings.size());info.next=&interface;
    if(vkd3d_shader_compile(&info,&scan.code,&scan.messages)<0){scan.code={};throw std::invalid_argument("現代著色器轉譯失敗："+std::string(scan.messages?scan.messages:""));}
    std::vector<std::uint32_t> words(scan.code.size/4);std::memcpy(words.data(),scan.code.code,words.size()*4);
    spirv_cross::CompilerMSL compiler(std::move(words));
    const auto model=stage==0?spv::ExecutionModelVertex:stage==1?spv::ExecutionModelFragment:spv::ExecutionModelGLCompute;
    auto msl=compiler.get_msl_options();msl.platform=spirv_cross::CompilerMSL::Options::macOS;msl.set_msl_version(2,3);msl.pad_fragment_output_components=true;compiler.set_msl_options(msl);
    GraphicsModernShader result;
    // 依 SPIRV-Cross 反射的實際資源型別分配 Metal 索引，與宣告順序無關。
    unsigned next_buffer=0,next_texture=0,next_sampler=0;
    const auto assign=[&](const spirv_cross::SmallVector<spirv_cross::Resource>& resources,unsigned group){
        for(const auto& resource:resources){
            const auto binding=compiler.get_decoration(resource.id,spv::DecorationBinding);const auto found=by_binding.find(binding);
            if(found==by_binding.end())throw std::invalid_argument("現代著色器資源缺少 D3D 描述元");
            const auto& d=found->second;unsigned kind=0,native=0;
            spirv_cross::MSLResourceBinding mapping{};mapping.stage=model;mapping.desc_set=0;mapping.binding=binding;mapping.count=d.count;
            if(group==0){kind=0;native=next_buffer;next_buffer+=d.count;mapping.msl_buffer=native;}
            else if(group==1){kind=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV?4:3;native=next_buffer;next_buffer+=d.count;mapping.msl_buffer=native;}
            else if(group==2){kind=d.type==VKD3D_SHADER_DESCRIPTOR_TYPE_UAV?5:1;native=next_texture;next_texture+=d.count;mapping.msl_texture=native;}
            else {kind=2;native=next_sampler;next_sampler+=d.count;mapping.msl_sampler=native;}
            compiler.add_msl_resource_binding(mapping);
            for(unsigned j=0;j<d.count;++j)result.bindings.push_back({kind,d.register_index+j,native+j});
        }
    };
    const auto resources=compiler.get_shader_resources();
    assign(resources.uniform_buffers,0);assign(resources.storage_buffers,1);
    assign(resources.separate_images,2);assign(resources.storage_images,2);assign(resources.separate_samplers,3);
    if(next_buffer>31 || next_texture>128 || next_sampler>16)throw std::invalid_argument("現代著色器資源超出 Metal 綁定上限");
    if(stage==0)for(const auto& input:resources.stage_inputs){
        // 頂點屬性位置即 D3D 輸入簽章暫存器；以語意供 input layout 對應。
        const auto location=compiler.get_decoration(input.id,spv::DecorationLocation);
        const auto& signature=scan.signatures.input;
        const auto found=std::find_if(signature.elements,signature.elements+signature.element_count,[&](const auto& e){return e.register_index==location && !e.sysval_semantic;});
        if(found==signature.elements+signature.element_count)throw std::invalid_argument("現代頂點著色器輸入缺少語意");
        result.inputs.push_back({found->semantic_name,found->semantic_index,location});
    }
    if(stage==5)for(unsigned i=0;i<3;++i)result.threads[i]=std::max(1U,compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize,i));
    compiler.rename_entry_point("main","xlatMtlMain",model);result.source=compiler.compile();result.entry="xlatMtlMain";
    return result;
}
}
