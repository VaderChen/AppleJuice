#pragma once
#include "hlsl_classes.hpp"
#include <functional>

namespace x64arm::hlsl_linkage {
// Effects 的宣告／pass 由既有 FX 編譯器處理；每一個 shader 使用相同的 class
// 展開器實際編譯。容器組裝時必須替換全部識別 shader，才可交付客體。
struct FxShader {std::string key,profile,entry,source;Metadata metadata;};
struct FxFunction {Tokens head,body;std::vector<Tokens> parameters;std::string name;std::size_t open{},close{};};
struct FxUnit {Tokens tokens;bool function{},technique{},shader{},interface{};};
inline std::vector<Tokens> fx_arguments(const Tokens& tokens,std::size_t begin,std::size_t end){
    std::vector<Tokens> result;auto start=begin;
    for(auto i=begin;i<end;++i){if(tokens[i]=="(" || tokens[i]=="[" || tokens[i]=="{"){i=matching(tokens,i);continue;}if(tokens[i]==","){result.emplace_back(tokens.begin()+static_cast<std::ptrdiff_t>(start),tokens.begin()+static_cast<std::ptrdiff_t>(i));start=i+1;}}
    if(start<end)result.emplace_back(tokens.begin()+static_cast<std::ptrdiff_t>(start),tokens.begin()+static_cast<std::ptrdiff_t>(end));return result;
}
struct FxLinkage {
    ClassLowering parsed;
    std::vector<FxUnit> units;
    std::map<std::string,FxFunction> functions;
    std::map<std::string,Tokens> shader_initializers;
    std::vector<std::pair<std::string,std::string>> interfaces;
    std::vector<FxShader> shaders;
    std::string source;
    explicit FxLinkage(std::string_view input){
        parsed.parse(tokenize(input));if(parsed.classes.empty()){source=std::string(input);return;}const auto& t=parsed.remaining;
        for(std::size_t start=0;start<t.size();){
            std::size_t end=start,brace=SIZE_MAX,paren=SIZE_MAX;bool assignment=false;
            for(;end<t.size();++end){
                if(t[end]=="=")assignment=true;
                if(t[end]=="(" || t[end]=="["){if(t[end]=="(" && paren==SIZE_MAX && !assignment)paren=end;end=matching(t,end);continue;}
                if(t[end]=="{"){brace=end;end=matching(t,end)+1;if(end<t.size() && t[end]==";")++end;break;}
                if(t[end]==";"){++end;break;}
            }
            if(end<=start)throw std::invalid_argument("FX 宣告不完整");
            FxUnit u;u.tokens=Tokens(t.begin()+static_cast<std::ptrdiff_t>(start),t.begin()+static_cast<std::ptrdiff_t>(end));u.technique=t[start].starts_with("technique") || t[start]=="fxgroup";
            u.shader=t[start]=="VertexShader" || t[start]=="PixelShader" || t[start]=="GeometryShader" || t[start]=="HullShader" || t[start]=="DomainShader" || t[start]=="ComputeShader";
            u.interface=parsed.classes.contains(t[start]) && parsed.classes.at(t[start]).abstract;
            u.function=brace!=SIZE_MAX && paren!=SIZE_MAX && t[paren-1]!="register" && t[paren-1]!="packoffset" && !assignment && t[start]!="cbuffer" && t[start]!="tbuffer" && !u.technique;
            if(u.function){
                FxFunction f;f.name=t[paren-1];f.head=Tokens(t.begin()+static_cast<std::ptrdiff_t>(start),t.begin()+static_cast<std::ptrdiff_t>(brace));f.open=paren-start;f.close=matching(f.head,f.open);f.parameters=fx_arguments(f.head,f.open+1,f.close);f.body=Tokens(t.begin()+static_cast<std::ptrdiff_t>(brace),t.begin()+static_cast<std::ptrdiff_t>(matching(t,brace))+1);
                if(functions.contains(f.name))throw std::invalid_argument("FX shader entry overload 尚未支援："+f.name);functions.emplace(f.name,std::move(f));
            }
            if(u.shader){if(u.tokens.size()<5 || u.tokens[2]!="=")throw std::invalid_argument("FX shader 宣告缺少 initializer");shader_initializers[u.tokens[1]]=Tokens(u.tokens.begin()+3,u.tokens.end()-1);}
            if(u.interface){if(u.tokens.size()!=3 || u.tokens[2]!=";")throw std::invalid_argument("FX interface 宣告尚未支援");interfaces.emplace_back(u.tokens[1],u.tokens[0]);}
            units.push_back(std::move(u));start=end;
        }
        std::string declarations;
        for(const auto& name:parsed.order){const auto& c=parsed.classes.at(name);if(c.abstract)continue;declarations+="struct "+name+" { ";for(const auto& f:c.fields){declarations+=f.type+" "+f.name;if(f.count>1)declarations+='['+std::to_string(f.count)+']';declarations+="; ";}if(c.fields.empty())declarations+="uint __empty; ";declarations+="};\n";}
        std::string effect;
        for(const auto& u:units)if(!u.function && !u.interface)effect+=join(replace_compiles(u.tokens));
        for(const auto& shader:shaders){
            if(!shader.profile.starts_with("vs_") && !shader.profile.starts_with("ps_"))throw std::invalid_argument("FX 動態 class 的 shader 階段尚未支援："+shader.profile);
            // 只作為 FX 編譯器中的 shader 容器識別；finish() 保證實際 bytecode 全數置換。
            declarations+="float4 "+shader.key+"(float4 v : "+shader.key+") : "+(shader.profile.starts_with("vs_")?"SV_Position":"SV_Target")+" { return v; }\n";
        }
        source=declarations+effect;
    }
    Tokens replace_compiles(const Tokens& in){
        Tokens out;
        for(std::size_t i=0;i<in.size();++i){
            if((in[i]!="CompileShader" && in[i]!="BindInterfaces") || i+1>=in.size() || in[i+1]!="("){out.push_back(in[i]);continue;}
            const auto end=matching(in,i+1);const auto args=fx_arguments(in,i+2,end);std::vector<Tokens> bindings;Tokens compile;
            if(in[i]=="BindInterfaces"){
                if(args.empty() || args[0].size()!=1 || !shader_initializers.contains(args[0][0]))throw std::invalid_argument("FX BindInterfaces 缺少有效 shader 變數");compile=shader_initializers.at(args[0][0]);bindings.assign(args.begin()+1,args.end());
            }else compile=Tokens(in.begin()+static_cast<std::ptrdiff_t>(i),in.begin()+static_cast<std::ptrdiff_t>(end)+1);
            const auto parts=fx_arguments(compile,2,compile.size()-1);
            if(parts.size()!=2 || parts[0].size()!=1 || parts[1].size()<3 || parts[1][1]!="(")throw std::invalid_argument("FX CompileShader 語法不完整");
            const auto entry=parts[1][0],profile=parts[0][0];auto uniform=fx_arguments(parts[1],2,parts[1].size()-1);
            const auto key=make_shader(entry,profile,uniform,bindings);
            out.insert(out.end(),{"CompileShader","(",profile,",",key,"(",")",")"});i=end;
        }
        return out;
    }
    std::string make_shader(const std::string& entry,const std::string& profile,const std::vector<Tokens>& uniform,const std::vector<Tokens>& bound){
        const auto found=functions.find(entry);if(found==functions.end())throw std::invalid_argument("FX shader entry 不存在："+entry);
        const auto& fn=found->second;FxShader shader;shader.key="__X64ARM_FX_"+std::to_string(shaders.size())+"_ID";shader.entry=shader.key+"_entry";shader.profile=profile;
        ClassLowering compiler=parsed;compiler.remaining.clear();compiler.metadata={};
        std::set<std::string> reachable;std::function<void(const std::string&)> visit=[&](const std::string& name){if(!reachable.insert(name).second)return;for(const auto& token:functions.at(name).body)if(functions.contains(token))visit(token);};visit(entry);
        Tokens used;for(const auto& name:reachable){const auto& f=functions.at(name);used.insert(used.end(),f.body.begin(),f.body.end());}
        for(const auto& u:units){
            if(u.technique || u.shader)continue;
            if(u.function){const auto name=*(std::find(u.tokens.begin(),u.tokens.end(),"(")-1);if(!reachable.contains(name))continue;}
            if(u.interface && std::find(used.begin(),used.end(),u.tokens[1])==used.end())continue;
            auto tokens=u.tokens;if(u.function)tokens.erase(std::remove(tokens.begin(),tokens.end(),"uniform"),tokens.end());
            compiler.remaining.insert(compiler.remaining.end(),tokens.begin(),tokens.end());
        }
        Tokens wrapper_head(fn.head.begin(),fn.head.begin()+static_cast<std::ptrdiff_t>(fn.open));wrapper_head.back()=shader.entry;wrapper_head.push_back("(");Tokens call{entry,"("};
        std::map<std::string,std::pair<std::string,bool>> aliases;std::size_t uniform_index=0,bound_index=0;bool first=true,first_input=true;
        for(const auto& parameter:fn.parameters){
            auto p=parameter;const bool is_uniform=std::find(p.begin(),p.end(),"uniform")!=p.end();p.erase(std::remove(p.begin(),p.end(),"uniform"),p.end());
            if(p.size()<2)throw std::invalid_argument("FX entry 參數不完整");const auto type_at=std::find_if(p.begin(),p.end(),[&](const auto& token){return parsed.classes.contains(token) && parsed.classes.at(token).abstract;});
            Tokens actual;
            if(type_at!=p.end()){
                const auto type=*type_at;const auto alias=shader.key+"_arg_"+std::to_string(aliases.size());bool parameter_binding=false;
                if(is_uniform){if(uniform_index>=uniform.size())throw std::invalid_argument("FX 缺少 uniform 參數");actual=uniform[uniform_index++];}
                else if(bound_index<bound.size())actual=bound[bound_index++];
                else {actual={alias};parameter_binding=true;}
                if(actual.size()!=1)throw std::invalid_argument("FX interface 陣列／運算式綁定尚未支援");
                compiler.remaining.insert(compiler.remaining.begin(),{type,alias,";"});aliases[alias]={actual[0],parameter_binding};
                if(!profile.starts_with("ps_5_") && !profile.starts_with("vs_5_")){
                    if(parameter_binding)throw std::invalid_argument("SM4 shader 必須靜態綁定 interface");compiler.static_bindings[alias]=actual[0];
                }
                actual={alias};
            }else if(is_uniform){if(uniform_index>=uniform.size())throw std::invalid_argument("FX 缺少 uniform 參數");actual=uniform[uniform_index++];}
            else {
                if(!first_input)wrapper_head.push_back(",");first_input=false;wrapper_head.insert(wrapper_head.end(),p.begin(),p.end());
                auto name=std::find(p.begin(),p.end(),":");const auto size=static_cast<std::size_t>(name-p.begin());if(size<2)throw std::invalid_argument("FX entry 參數缺少名稱");actual={p[size-1]};
            }
            if(!first)call.push_back(",");first=false;call.insert(call.end(),actual.begin(),actual.end());
        }
        if(uniform_index!=uniform.size() || bound_index!=bound.size())throw std::invalid_argument("FX interface／uniform 參數數量不符");
        wrapper_head.insert(wrapper_head.end(),fn.head.begin()+static_cast<std::ptrdiff_t>(fn.close),fn.head.end());wrapper_head.push_back("{");if(fn.head[0]!="void")wrapper_head.push_back("return");wrapper_head.insert(wrapper_head.end(),call.begin(),call.end());wrapper_head.insert(wrapper_head.end(),{")",";","}"});compiler.remaining.insert(compiler.remaining.end(),wrapper_head.begin(),wrapper_head.end());
        auto result=compiler.lower();shader.source=std::move(result.source);shader.metadata=std::move(result.metadata);
        for(auto& interface:shader.metadata.interfaces)if(auto alias=aliases.find(interface.name);alias!=aliases.end()){interface.name=alias->second.first;interface.parameter=alias->second.second;}
        shaders.push_back(std::move(shader));return shaders.back().key;
    }
};
}
