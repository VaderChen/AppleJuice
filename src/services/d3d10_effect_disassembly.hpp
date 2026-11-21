#pragma once
#include "d3d10_effect.hpp"
#include "shader_disassembly.hpp"
#include "x64arm/windows_graphics.hpp"

namespace x64arm::fx10 {
inline std::string disassemble(const Program& program,const Program* pool,unsigned flags){
    if(flags&~0xffU)throw std::invalid_argument("FX4 反組譯旗標無效");
    const bool instructions_only=(flags&0x40U)!=0;
    std::string text;
    const auto append=[&](std::string_view line){
        if(line.size()>64U*1024U*1024U-text.size())throw std::bad_alloc{};
        text+=line;
    };
    const auto value=[&](unsigned kind,std::uint32_t bits){
        if(flags&0x80U)return shader_disassembly::hex(bits,8);
        if(kind==4)return std::string(bits?"true":"false");
        if(kind==2)return std::to_string(std::bit_cast<std::int32_t>(bits));
        if(kind==1)return std::to_string(std::bit_cast<float>(bits));
        return std::to_string(bits);
    };
    const auto literal=[&](std::string_view string){
        append("\"");
        for(char character:string){
            if(character=='\\' || character=='"')append("\\");
            if(character=='\n')append("\\n");
            else if(character=='\r')append("\\r");
            else append(std::string_view(&character,1));
        }
        append("\"");
    };
    const auto shader=[&](const Program& scope,std::uint32_t offset,std::uint32_t output){
        const auto bytes=scope.blob(offset);
        if(bytes.empty()){
            if(!instructions_only)append("NULL");
            return;
        }
        if(!instructions_only)append("asm {\n");
        append(graphics_disassemble_shader(bytes,flags&~1U,{}));
        if(!instructions_only){
            append("}");
            if(output){append(" streamout(");literal(scope.string(output));append(")");}
        }
    };
    const auto assignment=[&](const Program& scope,const Assignment& item,std::string_view indent){
        static constexpr std::array<std::string_view,56> names{
            "RasterizerState","DepthStencilState","BlendState","RenderTargetView","DepthStencilView","GenerateMips",
            "VertexShader","PixelShader","GeometryShader","StencilRef","BlendFactor","SampleMask",
            "FillMode","CullMode","FrontCounterClockwise","DepthBias","DepthBiasClamp","SlopeScaledDepthBias",
            "DepthClipEnable","ScissorEnable","MultisampleEnable","AntialiasedLineEnable",
            "DepthEnable","DepthWriteMask","DepthFunc","StencilEnable","StencilReadMask","StencilWriteMask",
            "FrontFace.StencilFailOp","FrontFace.StencilDepthFailOp","FrontFace.StencilPassOp","FrontFace.StencilFunc",
            "BackFace.StencilFailOp","BackFace.StencilDepthFailOp","BackFace.StencilPassOp","BackFace.StencilFunc",
            "AlphaToCoverageEnable","BlendEnable","SrcBlend","DestBlend","BlendOp","SrcBlendAlpha","DestBlendAlpha",
            "BlendOpAlpha","RenderTargetWriteMask","Filter","AddressU","AddressV","AddressW","MipLODBias","MaxAnisotropy",
            "ComparisonFunc","BorderColor","MinLOD","MaxLOD","Texture"
        };
        if(instructions_only){
            if(item.kind==7)shader(scope,scope.get(item.offset),scope.get(item.offset+4));
            return;
        }
        append(indent);
        append(item.state<names.size()?std::string(names[item.state]):"State"+std::to_string(item.state));
        if(item.index)append("["+std::to_string(item.index)+"]");
        append(" = ");
        if(item.kind==1){
            const auto count=Program::bounded(scope.get(item.offset));
            if(count!=1)append("{ ");
            for(unsigned i=0;i<count;++i){
                if(i)append(", ");
                const auto at=item.offset+4U+i*8U;
                const auto bits=scope.get(at+4);
                if(count==1 && item.state<=8 && !bits)append("NULL");
                else append(value(scope.get(at),bits));
            }
            if(count!=1)append(" }");
        }else if(item.kind==2)append(scope.string(item.offset));
        else if(item.kind==3 || item.kind==4){
            append(scope.string(scope.get(item.offset)));
            append("[");
            const auto subscript=scope.get(item.offset+4);
            append(item.kind==4?scope.string(subscript):std::to_string(subscript));
            append("]");
        }else if(item.kind==7)shader(scope,scope.get(item.offset),scope.get(item.offset+4));
        else{
            auto offset=item.offset;
            if(item.kind==5){append(scope.string(scope.get(offset)));append("[");offset=scope.get(offset+4);}
            append("expression(0x");
            for(auto byte:scope.blob(offset))append(shader_disassembly::hex(byte,2).substr(2));
            append(item.kind==5?")]":")");
        }
        append(";\n");
    };
    const auto declaration=[&](const Variable& variable){
        append(variable.type->name);
        append(" ");
        append(variable.name);
        if(variable.type->elements)append("["+std::to_string(variable.type->elements)+"]");
        if(!variable.semantic.empty())append(" : "+variable.semantic);
    };
    const auto variables=[&](const Program& scope,bool shared){
        if(!instructions_only)for(const auto& buffer:scope.buffers){
            if(shared)append("shared ");
            append((buffer.flags&1)?"tbuffer ":"cbuffer ");
            append(buffer.name);
            if(buffer.bind!=UINT32_MAX)append(" : register("+std::string((buffer.flags&1)?"t":"b")+std::to_string(buffer.bind)+")");
            append(" {\n");
            for(auto index:buffer.variables){
                const auto& variable=scope.variables.at(index);
                append("    ");
                declaration(variable);
                append(";");
                if(flags&2U){
                    append(" // +"+std::to_string(variable.offset)+" = { ");
                    for(unsigned i=0;i<variable.type->size/4;++i){
                        if(i)append(", ");
                        const auto bits=Program::word(buffer.bytes,variable.offset+i*4);
                        append(variable.type->kind==1?value(variable.type->scalar(),bits):shader_disassembly::hex(bits,8));
                    }
                    append(" }");
                }
                append("\n");
            }
            append("};\n");
        }
        for(const auto& variable:scope.variables){
            if(variable.buffer!=UINT32_MAX)continue;
            const auto detail=variable.type->detail;
            const bool shader_type=detail>=5 && detail<=8;
            if(!instructions_only){
                if(shared)append("shared ");
                declaration(variable);
                if(variable.bind!=UINT32_MAX)append(" /* bind "+std::to_string(variable.bind)+" */");
                append(";\n");
            }
            for(unsigned i=0;i<variable.objects.size();++i){
                const auto& object=variable.objects[i];
                if(!shader_type && (instructions_only || (object.assignments.empty() && detail!=1)))continue;
                if(!instructions_only){
                    append(variable.name);
                    if(variable.type->elements)append("["+std::to_string(i)+"]");
                    append(" = ");
                }
                if(shader_type)shader(scope,object.data,object.stream_output);
                else if(detail==1)literal(scope.string(object.data));
                else{
                    append("{\n");
                    for(const auto& item:object.assignments)assignment(scope,item,"    ");
                    append("}");
                }
                if(!instructions_only)append(";\n");
            }
        }
    };

    if(!instructions_only)append(program.independent_blend?"// fx_4_1\n":"// fx_4_0\n");
    if(pool)variables(*pool,true);
    variables(program,false);
    for(const auto& technique:program.techniques){
        if(!instructions_only)append("technique10 "+technique.name+" {\n");
        for(const auto& pass:technique.passes){
            if(!instructions_only)append("    pass "+pass.name+" {\n");
            for(const auto& item:pass.assignments)assignment(program,item,"        ");
            if(!instructions_only)append("    }\n");
        }
        if(!instructions_only)append("}\n");
    }
    return flags&1U?shader_disassembly::html(text):text;
}
}
