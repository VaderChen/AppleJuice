#include "x64arm/windows_graphics.hpp"
#include "hlsl_include.hpp"
#include "d3dx9_assembler.hpp"
#include "hlsl_classes.hpp"
#include "hlsl_fx_container.hpp"
#include "dxbc_reflection.hpp"
#include "shader_disassembly.hpp"
#include <vkd3d_shader.h>
#include <cstring>
#include <map>
#include <filesystem>
#include <fstream>
namespace x64arm {
std::string graphics_disassemble_shader(std::span<const std::uint8_t> bytes,std::uint32_t flags,std::string_view comments){
    if(bytes.size()<4 || bytes.size()>16U*1024U*1024U || (flags&~0xffU))throw std::invalid_argument("shader 反組譯參數無效");
    const dxbc::Reader input{bytes};const bool container=input.u32(0)==0x43425844U;
    auto source=container?VKD3D_SHADER_SOURCE_DXBC_TPF:VKD3D_SHADER_SOURCE_D3D_BYTECODE;
    std::vector<std::uint64_t> offsets;std::span<const std::uint8_t> tokens=bytes;std::uint64_t token_base=0;
    if(container){
        if(bytes.size()<32 || input.u32(24)!=bytes.size())throw std::invalid_argument("DXBC 反組譯容器無效");
        const auto count=input.u32(28);if(count>65536)throw std::invalid_argument("DXBC 區塊數量超過上限");input.check(32,std::uint64_t{count}*4);bool found=false;
        for(unsigned i=0;i<count;++i){const auto at=input.u32(32+4*i);input.check(at,8);const auto size=input.u32(at+4);input.check(std::uint64_t{at}+8,size);const auto tag=input.u32(at);
            if(tag==0x4c495844U){source=VKD3D_SHADER_SOURCE_DXBC_DXIL;tokens={};found=true;}
            if(tag==0x52444853U || tag==0x58454853U){tokens=bytes.subspan(static_cast<std::size_t>(at)+8,size);token_base=std::uint64_t{at}+8;found=true;}
        }if(!found)throw std::invalid_argument("DXBC 缺少 shader 指令");
    }
    if(!tokens.empty()){
        const dxbc::Reader code{tokens};if(tokens.size()%4)throw std::invalid_argument("shader token 沒有對齊");
        const auto words=tokens.size()/4;const auto begin=container?2U:1U;
        if(container && (words<2 || code.u32(4)!=words))throw std::invalid_argument("DXBC 指令長度無效");
        for(std::size_t at=begin;at<words;){const auto token=code.u32(at*4);std::size_t length=0;
            if(container){length=(token&0x7ffU)==53U?code.u32(at*4+4):(token>>24)&0x7fU;}
            else {const auto opcode=token&0xffffU;if(opcode==0xffffU){offsets.push_back(token_base+at*4);break;}if(opcode==0xfffeU){length=1+((token>>16)&0x7fffU);}
                else {length=1+((token>>24)&15U);if(((code.u32(0)>>8)&255U)==1U){const auto size=shader_disassembly::sm1_length(opcode,code.u32(0));if(!size)throw std::invalid_argument("SM1 shader opcode 無效");length=*size;}}
            }
            if(!length || length>words-at)throw std::invalid_argument("shader 指令被截斷");
            if(container || (token&0xffffU)!=0xfffeU)offsets.push_back(token_base+at*4);at+=length;
        }
    }
    struct Output{vkd3d_shader_code code{};char* messages{};~Output(){vkd3d_shader_free_shader_code(&code);vkd3d_shader_free_messages(messages);}} output;
    const std::array<vkd3d_shader_compile_option,2> options{{
        {VKD3D_SHADER_COMPILE_OPTION_FORMATTING,VKD3D_SHADER_COMPILE_OPTION_FORMATTING_INDENT},
        {VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG,flags&0x10U?1U:0U}}};
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.source={bytes.data(),bytes.size()};info.source_type=source;
    info.target_type=VKD3D_SHADER_TARGET_D3D_ASM;info.options=options.data();info.option_count=static_cast<unsigned>(options.size());info.log_level=VKD3D_SHADER_LOG_WARNING;
    const auto status=vkd3d_shader_compile(&info,&output.code,&output.messages);
    if(status==VKD3D_ERROR_OUT_OF_MEMORY)throw std::bad_alloc();
    if(status<0 || !output.code.code || !output.code.size)throw std::invalid_argument("shader 反組譯失敗："+std::string(output.messages?output.messages:""));
    std::string text(static_cast<const char*>(output.code.code),output.code.size);while(!text.empty() && !text.back())text.pop_back();
    std::string header;
    if(!(flags&0x40U) && container && source==VKD3D_SHADER_SOURCE_DXBC_TPF){
        const dxbc::Reflection reflection{bytes};if(!reflection.creator.empty())header+="// "+reflection.creator+"\n";
        if(flags&2U)for(const auto& buffer:reflection.buffers)for(const auto& variable:buffer.variables)if(!variable.initial.empty()){
            header+="// "+buffer.name+"."+variable.name+" = ";for(std::size_t at=0;at<variable.initial.size();at+=4){
                std::uint32_t value=0;for(std::size_t i=0;i<4 && at+i<variable.initial.size();++i)value|=std::uint32_t{variable.initial[at+i]}<<(i*8);
                if(at)header+=", ";header+=shader_disassembly::hex(value,8);
            }header+='\n';
        }
    }
    return shader_disassembly::format(text,flags,comments,header,offsets);
}
GraphicsCompilation graphics_compile_hlsl(std::span<const std::uint8_t> source,const std::string& name,
    const std::string& entry,const std::string& profile,const std::vector<std::pair<std::string,std::string>>& macros,std::uint32_t flags){
    return graphics_compile_hlsl_includes(source,name,entry,profile,macros,flags,{});
}
GraphicsCompilation graphics_preprocess_shader(std::span<const std::uint8_t> source,const std::string& name,
    const std::vector<std::pair<std::string,std::string>>& macros,const HlslIncludeLoader& loader){
    std::vector<vkd3d_shader_macro> defines;for(const auto& [key,value]:macros)defines.push_back({key.c_str(),value.c_str()});
    vkd3d_shader_preprocess_info pre{};pre.type=VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;pre.macros=defines.data();pre.macro_count=static_cast<unsigned>(defines.size());
    struct Includes {const HlslIncludeLoader& loader;const std::string& root;std::map<const void*,HlslIncludeSource> active;std::size_t total{};};
    Includes includes{loader,name,{},0};pre.include_context=&includes;
    pre.pfn_open_include=[](const char* filename,bool local,const char* parent,void* context,vkd3d_shader_code* out){
        auto& files=*static_cast<Includes*>(context);if(!files.loader || files.active.size()>=128)return -3;
        try{
            const auto found=files.active.find(parent);auto loaded=files.loader(filename,local,found==files.active.end()?files.root:found->second.name);
            if(!loaded || loaded->bytes.size()>16U*1024U*1024U || files.total+loaded->bytes.size()>64U*1024U*1024U)return -3;
            const auto size=loaded->bytes.size();files.total+=size;loaded->bytes.push_back(0);const auto data=loaded->bytes.data();
            files.active.emplace(data,std::move(*loaded));out->code=data;out->size=size;return 0;
        }catch(...){return -3;}
    };
    pre.pfn_close_include=[](const vkd3d_shader_code* code,void* context){static_cast<Includes*>(context)->active.erase(code->code);};
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.next=&pre;
    info.source={source.data(),source.size()};info.source_type=VKD3D_SHADER_SOURCE_HLSL;info.source_name=name.c_str();info.log_level=VKD3D_SHADER_LOG_WARNING;
    vkd3d_shader_code output{};char* messages=nullptr;GraphicsCompilation compiled;
    const auto result=vkd3d_shader_preprocess(&info,&output,&messages);
    if(messages){compiled.messages=messages;vkd3d_shader_free_messages(messages);}
    if(result>=0 && output.size){const auto* data=static_cast<const std::uint8_t*>(output.code);compiled.bytecode.assign(data,data+output.size);}
    vkd3d_shader_free_shader_code(&output);return compiled;
}
GraphicsCompilation graphics_assemble_shader(std::span<const std::uint8_t> source,const std::string& name,
    const std::vector<std::pair<std::string,std::string>>& macros,std::uint32_t flags,const HlslIncludeLoader& loader){
    auto expanded=graphics_preprocess_shader(source,name,macros,loader);if(expanded.bytecode.empty())return expanded;
    auto result=d3dx9::Assembler{}.assemble(expanded.bytecode,name);result.messages=expanded.messages+result.messages;
    if(!result.bytecode.empty() && !(flags&2)){
        vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
        info.source={result.bytecode.data(),result.bytecode.size()};info.source_type=VKD3D_SHADER_SOURCE_D3D_BYTECODE;
        info.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;info.source_name=name.c_str();info.log_level=VKD3D_SHADER_LOG_WARNING;
        char* messages=nullptr;const auto status=vkd3d_shader_scan(&info,&messages);
        if(messages){result.messages+=messages;vkd3d_shader_free_messages(messages);}if(status<0)result.bytecode.clear();
    }
    return result;
}
static GraphicsCompilation compile_hlsl_impl(std::span<const std::uint8_t> source,const std::string& name,
    const std::string& entry,const std::string& profile,const std::vector<std::pair<std::string,std::string>>& macros,std::uint32_t flags,const HlslIncludeLoader& loader,std::uint32_t effect_flags,const hlsl_linkage::Metadata* supplied=nullptr){
    std::array<vkd3d_shader_compile_option,5> options{{
        {VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER,(flags&8)?VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR:VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR},
        {VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY,(flags&0x1000)?1U:0U},
        {VKD3D_SHADER_COMPILE_OPTION_WARN_IMPLICIT_TRUNCATION,1},
        {VKD3D_SHADER_COMPILE_OPTION_CHILD_EFFECT,effect_flags&1U},
        {static_cast<vkd3d_shader_compile_option_name>(0x58434c53U),0}}};
    vkd3d_shader_preprocess_info pre{};pre.type=VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    vkd3d_shader_hlsl_source_info hlsl{};hlsl.type=VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;hlsl.next=&pre;hlsl.entry_point=entry.c_str();hlsl.profile=profile.c_str();
    vkd3d_shader_compile_info info{};info.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;info.next=&hlsl;info.source={source.data(),source.size()};info.source_type=VKD3D_SHADER_SOURCE_HLSL;info.target_type=profile.starts_with("fx_")?VKD3D_SHADER_TARGET_FX:((profile.starts_with("vs_") || profile.starts_with("ps_")) && profile.size()>3 && profile[3]>='1' && profile[3]<='3')?VKD3D_SHADER_TARGET_D3D_BYTECODE:VKD3D_SHADER_TARGET_DXBC_TPF;info.log_level=VKD3D_SHADER_LOG_WARNING;info.source_name=name.c_str();info.options=options.data();info.option_count=static_cast<unsigned>(options.size());
    vkd3d_shader_code output{};char* messages=nullptr;
    auto compiled=graphics_preprocess_shader(source,name,macros,loader);
    if(compiled.bytecode.empty())return compiled;
    std::string expanded(reinterpret_cast<const char*>(compiled.bytecode.data()),compiled.bytecode.size());compiled.bytecode.clear();
    hlsl_linkage::Metadata linkage=supplied?*supplied:hlsl_linkage::Metadata{};
    std::unique_ptr<hlsl_linkage::FxLinkage> effect;
    std::map<std::string,std::vector<std::uint8_t>> effect_shaders;
    if(profile.starts_with("fx_") && (expanded.find("interface")!=expanded.npos || expanded.find("class")!=expanded.npos)){
        try{
            auto parsed=std::make_unique<hlsl_linkage::FxLinkage>(expanded);
            if(!parsed->parsed.classes.empty()){
                effect=std::move(parsed);
                for(const auto& shader:effect->shaders){
                    const auto bytes=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(shader.source.data()),shader.source.size());
                    auto result=compile_hlsl_impl(bytes,name,shader.entry,shader.profile,{},flags,{},0,&shader.metadata);
                    compiled.messages+=result.messages;
                    if(result.bytecode.empty())return compiled;
                    effect_shaders.emplace(shader.key,std::move(result.bytecode));
                }
                expanded=effect->source;
            }
        }catch(const std::exception& error){compiled.messages+=name+": "+error.what()+"\n";return compiled;}
    }
    if(!supplied && !profile.starts_with("fx_") && (expanded.find("interface")!=expanded.npos || expanded.find("class")!=expanded.npos)){
        try{
            auto lowered=hlsl_linkage::lower_classes(expanded);expanded=std::move(lowered.source);linkage=std::move(lowered.metadata);
            if(!linkage.types.empty())if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){
                std::filesystem::create_directories(folder);
                std::ofstream diagnostic(std::filesystem::path(folder)/("linkage-"+std::to_string(std::hash<std::string>{}(name+entry+profile))+".hlsl"));diagnostic<<expanded;
            }
        }
        catch(const std::exception& error){compiled.messages+=name+": "+error.what()+"\n";return compiled;}
    }
    if((effect || supplied) && std::getenv("X64ARM_DUMP_DXBC")){
        const std::filesystem::path directory(std::getenv("X64ARM_DUMP_DXBC"));std::filesystem::create_directories(directory);
        std::ofstream diagnostic(directory/("fx-"+entry+"-"+profile+".hlsl"));diagnostic<<expanded;
    }
    options[4].value=linkage.interfaces.empty()?0U:1U;
    info.source={expanded.data(),expanded.size()};pre.macro_count=0;pre.macros=nullptr;
    const auto result=vkd3d_shader_compile(&info,&output,&messages);
    if(messages){compiled.messages+=messages;vkd3d_shader_free_messages(messages);}
    if(result>=0){
        if(!linkage.interfaces.empty()){
            vkd3d_shader_dxbc_desc desc{};
            if(vkd3d_shader_parse_dxbc(&output,0,&desc,nullptr)<0){vkd3d_shader_free_shader_code(&output);throw std::invalid_argument("編譯器產生無效 DXBC");}
            const auto metadata=linkage.encode();std::vector<vkd3d_shader_dxbc_section_desc> sections(desc.sections,desc.sections+desc.section_count);
            sections.push_back({hlsl_linkage::tag,{metadata.data(),metadata.size()}});vkd3d_shader_code linked{};
            const auto serialized=vkd3d_shader_serialize_dxbc(sections.size(),sections.data(),&linked,nullptr);
            vkd3d_shader_free_dxbc(&desc);vkd3d_shader_free_shader_code(&output);output=linked;
            if(serialized<0){vkd3d_shader_free_shader_code(&output);throw std::invalid_argument("無法寫入 linkage 中繼資料");}
        }
        const auto* data=static_cast<const std::uint8_t*>(output.code);compiled.bytecode.assign(data,data+output.size);
    }
    vkd3d_shader_free_shader_code(&output);
    if(effect && !compiled.bytecode.empty()){
        try{compiled.bytecode=hlsl_linkage::FxContainer(compiled.bytecode,*effect,effect_shaders).finish();}
        catch(const std::exception& error){compiled.bytecode.clear();compiled.messages+=name+": "+error.what()+"\n";}
    }
    return compiled;
}
GraphicsCompilation graphics_compile_hlsl_includes(std::span<const std::uint8_t> source,const std::string& name,
    const std::string& entry,const std::string& profile,const std::vector<std::pair<std::string,std::string>>& macros,std::uint32_t flags,const HlslIncludeLoader& loader,std::uint32_t effect_flags){
    return compile_hlsl_impl(source,name,entry,profile,macros,flags,loader,effect_flags);
}
std::vector<std::uint8_t> graphics_signature_blob(std::span<const std::uint8_t> bytes,unsigned mask){
    const vkd3d_shader_code input{bytes.data(),bytes.size()};vkd3d_shader_dxbc_desc desc{};
    if(vkd3d_shader_parse_dxbc(&input,0,&desc,nullptr)<0)throw std::invalid_argument("DXBC signature 容器不合法");
    vkd3d_shader_code output{};
    try{
        std::vector<vkd3d_shader_dxbc_section_desc> sections;
        for(std::size_t i=0;i<desc.section_count;++i){const auto& section=desc.sections[i];const auto tag=section.tag;
            const unsigned kind=(tag==0x4e475349 || tag==0x31475349)?1U:
                (tag==0x4e47534f || tag==0x3147534f || tag==0x3547534f)?2U:
                (tag==0x47534350 || tag==0x31475350)?4U:0U;
            if(kind&mask)sections.push_back(section);
        }
        if(sections.empty() || vkd3d_shader_serialize_dxbc(sections.size(),sections.data(),&output,nullptr)<0)throw std::invalid_argument("DXBC 缺少要求的 signature");
        const auto* data=static_cast<const std::uint8_t*>(output.code);std::vector<std::uint8_t> result(data,data+output.size);
        vkd3d_shader_free_shader_code(&output);vkd3d_shader_free_dxbc(&desc);return result;
    }catch(...){vkd3d_shader_free_shader_code(&output);vkd3d_shader_free_dxbc(&desc);throw;}
}
std::uint32_t graphics_format_bytes(std::uint32_t f){
    switch(f){
    case 1:case 2:case 3:case 4:return 16;
    case 5:case 6:case 7:case 8:return 12;
    case 9:case 10:case 11:case 12:case 13:case 14:case 15:case 16:case 17:case 18:case 19:case 20:case 21:case 22:return 8;
    case 23:case 24:case 25:case 26:case 27:case 28:case 29:case 30:case 31:case 32:case 33:case 34:case 35:case 36:case 37:case 38:case 39:case 40:case 41:case 42:case 43:case 44:case 45:case 46:case 47:case 67:case 87:case 88:case 90:case 91:case 92:case 93:return 4;
    case 48:case 49:case 50:case 51:case 52:case 53:case 54:case 55:case 56:case 57:case 58:case 59:return 2;
    case 60:case 61:case 62:case 63:case 64:case 65:return 1;
    default:throw std::invalid_argument("尚未映射的 DXGI 資源格式："+std::to_string(f));
    }
}
std::uint32_t graphics_format_bc_family(std::uint32_t f) noexcept {
    if(f>=70 && f<=84)return 70+(f-70)/3*3;
    if(f>=94 && f<=99)return 94+(f-94)/3*3;
    return 0;
}
std::array<std::uint32_t,2> graphics_format_pitch(std::uint32_t f,std::uint32_t width,std::uint32_t height){
    if(const auto family=graphics_format_bc_family(f))return {((width+3)/4)*(family==70 || family==79?8U:16U),(height+3)/4};
    return {width*graphics_format_bytes(f),height};
}
}
