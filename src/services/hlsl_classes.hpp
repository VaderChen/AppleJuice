#pragma once
#include "hlsl_linkage.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace x64arm::hlsl_linkage {
using Tokens=std::vector<std::string>;
inline Tokens tokenize(std::string_view text){
    Tokens out;
    for(std::size_t i=0;i<text.size();){
        const auto c=static_cast<unsigned char>(text[i]);
        if(std::isspace(c)){++i;continue;}
        if(text[i]=='#'){while(i<text.size() && text[i]!='\n')++i;continue;}
        if(text.substr(i,2)=="//"){while(i<text.size() && text[i]!='\n')++i;continue;}
        if(text.substr(i,2)=="/*"){const auto end=text.find("*/",i+2);if(end==text.npos)throw std::invalid_argument("HLSL 註解未結束");i=end+2;continue;}
        const auto start=i++;
        if(std::isalpha(c) || c=='_'){while(i<text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i]=='_'))++i;}
        else if(std::isdigit(c) || (c=='.' && i<text.size() && std::isdigit(static_cast<unsigned char>(text[i])))){
            while(i<text.size()){
                const auto next=static_cast<unsigned char>(text[i]);
                if(std::isalnum(next) || next=='.' || ((next=='+' || next=='-') && text.substr(start,2)!="0x" && text.substr(start,2)!="0X" && (text[i-1]=='e' || text[i-1]=='E')))++i;else break;
            }
        }else if(c=='"' || c=='\''){
            bool closed=false;while(i<text.size()){if(text[i]=='\\'){i=std::min(text.size(),i+2);continue;}if(static_cast<unsigned char>(text[i++])==c){closed=true;break;}}if(!closed)throw std::invalid_argument("HLSL 字串未結束");
        }else if(i<text.size()){
            const auto pair=text.substr(start,2);
            if(pair=="::" || pair=="==" || pair=="!=" || pair=="<=" || pair==">=" || pair=="&&" || pair=="||" || pair=="++" || pair=="--" || pair=="+=" || pair=="-=" || pair=="*=" || pair=="/=" || pair=="<<" || pair==">>")++i;
        }
        out.emplace_back(text.substr(start,i-start));
    }
    return out;
}
inline std::string join(const Tokens& tokens,std::size_t start=0,std::size_t end=SIZE_MAX){
    std::string text;for(auto i=start;i<std::min(end,tokens.size());++i){text+=tokens[i];text+=' ';}return text;
}
inline std::size_t matching(const Tokens& tokens,std::size_t at){
    if(at>=tokens.size())throw std::invalid_argument("HLSL 缺少括號");
    const auto close=tokens[at]=="("?")":tokens[at]=="["?"]":tokens[at]=="{"?"}":"";
    if(!*close)throw std::invalid_argument("HLSL 括號不合法");unsigned level=1;
    for(auto i=at+1;i<tokens.size();++i){if(tokens[i]==tokens[at])++level;else if(tokens[i]==close && !--level)return i;}
    throw std::invalid_argument("HLSL 括號未結束");
}
struct Field {std::string type,name;unsigned count{1},offset{},width{};};
struct Method {std::string result,name;Tokens parameters,body;};
struct Class {
    std::string name;bool abstract{};std::vector<std::string> parents;
    std::vector<Field> fields;std::map<std::string,Method> methods;unsigned size{},id{};bool flattened{};
};
struct Lowered {std::string source;Metadata metadata;};
struct ClassLowering {
    std::map<std::string,Class> classes;
    std::vector<std::string> order;
    Tokens remaining;
    Metadata metadata;
    std::map<std::string,std::string> static_bindings;
    static unsigned width(const std::string& type){
        for(const std::string base:{"float","uint","int","bool"})if(type.starts_with(base)){
            const auto suffix=type.substr(base.size());if(suffix.empty())return 4;
            if(suffix.size()==1 && suffix[0]>='1' && suffix[0]<='4')return static_cast<unsigned>(suffix[0]-'0')*4;
        }
        throw std::invalid_argument("HLSL 動態 class 成員型別尚未支援："+type);
    }
    static unsigned aligned(unsigned offset){return (offset+15)&~15U;}
    static std::vector<std::string> arguments(const Tokens& parameters){
        std::vector<std::string> names;std::size_t start=0;
        for(std::size_t i=0;i<=parameters.size();++i)if(i==parameters.size() || parameters[i]==","){
            if(i>start){auto end=i;if(end>start && parameters[end-1]=="]"){while(end>start && parameters[end-1]!="[")--end;if(end>start)--end;}if(end<=start)throw std::invalid_argument("HLSL method 參數缺少名稱");names.push_back(parameters[end-1]);}start=i+1;
        }
        return names;
    }
    void parse(Tokens tokens){
        for(std::size_t i=0;i<tokens.size();){
            if(tokens[i]!="class" && tokens[i]!="interface"){remaining.push_back(tokens[i++]);continue;}
            Class c;c.abstract=tokens[i++]=="interface";if(i>=tokens.size())throw std::invalid_argument("HLSL class 缺少名稱");c.name=tokens[i++];
            if(i<tokens.size() && tokens[i]==":"){++i;while(i<tokens.size() && tokens[i]!="{"){if(tokens[i]!=",")c.parents.push_back(tokens[i]);++i;}}
            if(i>=tokens.size() || tokens[i]!="{")throw std::invalid_argument("HLSL class 缺少內容");const auto end=matching(tokens,i++);
            while(i<end){
                if(tokens[i]==";"){++i;continue;}
                if(i+2>=end)throw std::invalid_argument("HLSL class 成員不完整");
                const auto type=tokens[i++],name=tokens[i++];
                if(tokens[i]=="("){
                    const auto close=matching(tokens,i);Method m{type,name,Tokens(tokens.begin()+static_cast<std::ptrdiff_t>(i+1),tokens.begin()+static_cast<std::ptrdiff_t>(close)),{}};i=close+1;
                    if(i<end && tokens[i]=="{"){const auto close_body=matching(tokens,i);m.body=Tokens(tokens.begin()+static_cast<std::ptrdiff_t>(i+1),tokens.begin()+static_cast<std::ptrdiff_t>(close_body));i=close_body+1;}
                    if(c.methods.contains(name))throw std::invalid_argument("HLSL class method overload 尚未支援："+name);c.methods.emplace(name,std::move(m));
                }else {
                    Field f{type,name};if(tokens[i]=="["){const auto close=matching(tokens,i);if(close!=i+2)throw std::invalid_argument("HLSL class 陣列長度必須為常數");f.count=static_cast<unsigned>(std::stoul(tokens[i+1]));i=close+1;}
                    if(i>=end || tokens[i]!=";")throw std::invalid_argument("HLSL class 成員宣告尚未支援："+name);
                    if(!f.count || f.count>4096)throw std::invalid_argument("HLSL class 陣列長度不合法");f.width=width(type);c.fields.push_back(std::move(f));++i;
                }
            }
            i=end+1;if(i<tokens.size() && tokens[i]==";")++i;
            if(classes.contains(c.name))throw std::invalid_argument("HLSL class 重複宣告");order.push_back(c.name);classes.emplace(c.name,std::move(c));
        }
        // 收集 Class::Method 的定義，與 class 內定義共用同一後續處理。
        Tokens kept;
        for(std::size_t i=0;i<remaining.size();){
            if(i+4<remaining.size() && classes.contains(remaining[i+1]) && remaining[i+2]=="::" && remaining[i+4]=="("){
                auto& c=classes.at(remaining[i+1]);const auto close=matching(remaining,i+4);
                if(close+1>=remaining.size() || remaining[close+1]!="{")throw std::invalid_argument("HLSL class 外部 method 缺少內容");const auto end=matching(remaining,close+1);
                Method m{remaining[i],remaining[i+3],Tokens(remaining.begin()+static_cast<std::ptrdiff_t>(i+5),remaining.begin()+static_cast<std::ptrdiff_t>(close)),Tokens(remaining.begin()+static_cast<std::ptrdiff_t>(close+2),remaining.begin()+static_cast<std::ptrdiff_t>(end))};
                auto at=c.methods.find(m.name);if(at==c.methods.end() || !at->second.body.empty())throw std::invalid_argument("HLSL class method 定義不符");at->second=std::move(m);i=end+1;
            }else kept.push_back(remaining[i++]);
        }
        remaining=std::move(kept);std::set<std::string> active;
        for(const auto& name:order)flatten(name,active);
    }
    void flatten(const std::string& name,std::set<std::string>& active){
        auto& c=classes.at(name);if(c.flattened)return;if(!active.insert(name).second)throw std::invalid_argument("HLSL class 循環繼承");
        std::vector<Field> inherited;std::map<std::string,Method> methods;
        for(const auto& parent:c.parents){if(!classes.contains(parent))throw std::invalid_argument("HLSL class 基底不存在："+parent);flatten(parent,active);const auto& base=classes.at(parent);if(!base.abstract)inherited.insert(inherited.end(),base.fields.begin(),base.fields.end());for(const auto& [key,value]:base.methods)if(!value.body.empty() || c.abstract)methods[key]=value;}
        inherited.insert(inherited.end(),c.fields.begin(),c.fields.end());c.fields=std::move(inherited);for(const auto& [key,value]:c.methods)methods[key]=value;c.methods=std::move(methods);
        unsigned offset=0;for(auto& f:c.fields){if(f.count>1 || offset%16+f.width>16)offset=aligned(offset);f.offset=offset;offset+=f.count>1?aligned(f.width)*f.count:f.width;}
        c.size=aligned(offset);if(c.size>61440)throw std::invalid_argument("HLSL class 常數資料過大");c.flattened=true;active.erase(name);
    }
    bool implements(const Class& c,const std::string& name)const{
        if(c.name==name)return true;for(const auto& parent:c.parents)if(implements(classes.at(parent),name))return true;return false;
    }
    Tokens rewrite(const Tokens& input,const std::map<std::string,std::string>& globals,const Class* self=nullptr,const Method* method=nullptr)const{
        auto variables=globals;std::set<std::string> locals;
        if(method)for(const auto& name:arguments(method->parameters))locals.insert(name);
        for(std::size_t i=0;i+1<input.size();++i){
            if(classes.contains(input[i]))variables[input[i+1]]=input[i];
            if(input[i]=="float" || input[i]=="float2" || input[i]=="float3" || input[i]=="float4" || input[i]=="int" || input[i]=="uint" || input[i]=="bool")locals.insert(input[i+1]);
        }
        Tokens out;
        for(std::size_t i=0;i<input.size();++i){
            if(i+3<input.size() && input[i+1]=="." && input[i+3]=="(" && variables.contains(input[i])){
                const auto& type=classes.at(variables.at(input[i]));if(!type.methods.contains(input[i+2]))throw std::invalid_argument("HLSL method 不存在："+input[i+2]);
                out.push_back("__x64_"+type.name+"_"+input[i+2]);out.push_back("(");out.push_back(input[i]);if(i+4<input.size() && input[i+4]!=")")out.push_back(",");i+=3;continue;
            }
            if(self && (i==0 || input[i-1]!=".") && !locals.contains(input[i])){
                if(i+1<input.size() && input[i+1]=="(" && self->methods.contains(input[i])){
                    out.push_back("__x64_"+self->name+"_"+input[i]);out.push_back("(");out.push_back("__self");if(i+2<input.size() && input[i+2]!=")")out.push_back(",");++i;continue;
                }
                if(std::any_of(self->fields.begin(),self->fields.end(),[&](const auto& f){return f.name==input[i];})){out.push_back("__self");out.push_back(".");}
            }
            out.push_back(input[i]);
        }
        return out;
    }
    Lowered lower(){
        std::ostringstream declarations,prototypes,definitions;std::map<std::string,std::string> variables;
        unsigned type_id=0;
        for(const auto& name:order){auto& c=classes.at(name);declarations<<"struct "<<name<<" { ";
            if(c.abstract)declarations<<"uint __slot; ";else {c.id=++type_id;metadata.types.push_back({name,c.id,c.size});if(c.fields.empty())declarations<<"uint __empty; ";for(const auto& f:c.fields){declarations<<f.type<<' '<<f.name;if(f.count>1)declarations<<'['<<f.count<<']';declarations<<"; ";}}
            declarations<<"};\n";
        }
        // cbuffer 依 HLSL register 邊界配置；動態 class 的資料仍由原程式更新。
        for(std::size_t i=0;i<remaining.size();++i)if(remaining[i]=="cbuffer"){
            auto brace=i+1;while(brace<remaining.size() && remaining[brace]!="{")++brace;const auto end=matching(remaining,brace);unsigned slot=0,offset=0;
            bool has_classes=false;for(auto j=brace+1;j<end;++j)if(classes.contains(remaining[j]) && !classes.at(remaining[j]).abstract)has_classes=true;
            if(!has_classes){i=end;continue;}
            for(auto j=i+1;j<brace;++j)if(remaining[j].size()>1 && remaining[j][0]=='b' && std::isdigit(static_cast<unsigned char>(remaining[j][1])))slot=static_cast<unsigned>(std::stoul(remaining[j].substr(1)));
            for(auto j=brace+1;j<end;){
                if(remaining[j]==";"){++j;continue;}if(j+2>=end)throw std::invalid_argument("HLSL cbuffer 宣告不完整");
                const auto type=remaining[j++],name=remaining[j++];unsigned count=1;if(remaining[j]=="["){const auto close=matching(remaining,j);if(close!=j+2)throw std::invalid_argument("HLSL cbuffer 陣列長度尚未支援");count=static_cast<unsigned>(std::stoul(remaining[j+1]));j=close+1;}
                if(remaining[j]!=";")throw std::invalid_argument("HLSL cbuffer 成員宣告尚未支援");++j;
                if(classes.contains(type)){const auto& c=classes.at(type);offset=aligned(offset);metadata.instances.push_back({name,type,slot,offset,count});offset+=c.size*count;variables[name]=type;}
                else {const auto bytes=width(type);if(count>1 || offset%16+bytes>16)offset=aligned(offset);offset+=count>1?aligned(bytes)*count:bytes;}
            }
            i=end;
        }
        Tokens code;unsigned slot=0,depth=0;
        for(std::size_t i=0;i<remaining.size();++i){
            if(!depth && classes.contains(remaining[i]) && classes.at(remaining[i]).abstract && i+2<remaining.size() && remaining[i+2]==";"){
                const auto type=remaining[i],name=remaining[i+1];if(slot>=256)throw std::invalid_argument("HLSL interface 超過上限");Interface entry{name,type,slot,1,{}};for(const auto& concrete_name:order){const auto& concrete=classes.at(concrete_name);if(!concrete.abstract && implements(concrete,type))entry.types.push_back(concrete.id);}metadata.interfaces.push_back(std::move(entry));variables[name]=type;
                code.insert(code.end(),{"static","const",type,name,"=","{",std::to_string(slot++),"}",";"});i+=2;continue;
            }
            if(remaining[i]=="{")++depth;else if(remaining[i]=="}")--depth;code.push_back(remaining[i]);
        }
        if(!static_bindings.empty())for(const auto& interface:metadata.interfaces){
            const auto binding=static_bindings.find(interface.name);if(binding==static_bindings.end())throw std::invalid_argument("靜態 shader 缺少 interface 綁定："+interface.name);
            const auto instance=std::find_if(metadata.instances.begin(),metadata.instances.end(),[&](const auto& value){return value.name==binding->second;});
            if(instance==metadata.instances.end() || instance->count!=1 || !implements(classes.at(instance->type),interface.type))throw std::invalid_argument("靜態 shader class 綁定不相容："+binding->second);
        }
        if(!metadata.interfaces.empty() && static_bindings.empty())declarations<<"cbuffer __x64_Linkage : register(b14) { uint4 __x64_link_header[256]; uint4 __x64_link_data[3840]; };\n";
        for(const auto& name:order){const auto& c=classes.at(name);
            for(const auto& [key,m]:c.methods){
                const auto signature=m.result+" __x64_"+name+"_"+key+"("+name+" __self"+(m.parameters.empty()?"":", "+join(m.parameters))+")";
                prototypes<<signature<<";\n";definitions<<signature<<" {\n";
                if(!c.abstract){if(m.body.empty())throw std::invalid_argument("HLSL class method 缺少實作："+name+"::"+key);definitions<<join(rewrite(m.body,variables,&c,&m));}
                else {
                    for(const auto& concrete_name:order){const auto& concrete=classes.at(concrete_name);if(concrete.abstract || !implements(concrete,name))continue;
                        if(!concrete.methods.contains(key) || concrete.methods.at(key).body.empty())throw std::invalid_argument("HLSL class 未完成 interface："+concrete.name);
                        if(!static_bindings.empty()){
                            std::vector<std::pair<unsigned,std::string>> selected;
                            for(const auto& interface:metadata.interfaces)if(auto binding=static_bindings.find(interface.name);binding!=static_bindings.end()){
                                const auto instance=std::find_if(metadata.instances.begin(),metadata.instances.end(),[&](const auto& v){return v.name==binding->second;});
                                if(instance!=metadata.instances.end() && instance->type==concrete.name)selected.emplace_back(interface.slot,binding->second);
                            }
                            for(const auto& [selected_slot,instance]:selected){definitions<<"if (__self.__slot == "<<selected_slot<<") { ";if(m.result!="void")definitions<<"return ";definitions<<"__x64_"<<concrete.name<<'_'<<key<<'('<<instance;for(const auto& arg:arguments(m.parameters))definitions<<", "<<arg;definitions<<"); ";if(m.result=="void")definitions<<"return; ";definitions<<"}\n";}
                            continue;
                        }
                        if(metadata.interfaces.empty())continue;
                        definitions<<"if (__x64_link_header[__self.__slot].x == "<<concrete.id<<") { "<<concrete.name<<" value; uint base = __x64_link_header[__self.__slot].y; ";
                        for(const auto& f:concrete.fields)for(unsigned n=0;n<f.count;++n){const auto offset=f.offset+(f.count>1?aligned(f.width)*n:0);const auto channels=std::string("xyzw").substr((offset%16)/4,f.width/4);const auto raw="__x64_link_data[base + "+std::to_string(offset/16)+"]."+channels;
                            definitions<<"value."<<f.name;if(f.count>1)definitions<<'['<<n<<']';definitions<<" = ";
                            if(f.type.starts_with("float"))definitions<<"asfloat("<<raw<<")";else if(f.type.starts_with("int"))definitions<<"asint("<<raw<<")";else if(f.type.starts_with("bool"))definitions<<'('<<raw<<" != 0)";else definitions<<raw;definitions<<"; ";
                        }
                        // 具名 class 直接讀取原 cbuffer，讓編譯器保留真實的資源相依性。
                        // CreateClassInstance 的自訂 offset 則使用前述 raw payload。
                        for(std::size_t instance_index=0;instance_index<metadata.instances.size();++instance_index){
                            const auto& instance=metadata.instances[instance_index];if(instance.type!=concrete.name)continue;
                            definitions<<"if (__x64_link_header[__self.__slot].z == "<<(instance_index+1)<<") value = "<<instance.name;
                            if(instance.count>1)definitions<<"[__x64_link_header[__self.__slot].w]";definitions<<"; ";
                        }
                        if(m.result!="void")definitions<<"return ";definitions<<"__x64_"<<concrete.name<<'_'<<key<<"(value";for(const auto& arg:arguments(m.parameters))definitions<<", "<<arg;definitions<<"); ";if(m.result=="void")definitions<<"return; ";definitions<<"}\n";
                    }
                    // 宿主綁定檢查拒絕不存在／不相容的 type；編譯器仍要求完整回傳路徑。
                    if(m.result!="void")definitions<<"return ("<<m.result<<")0;";
                }
                definitions<<"\n}\n";
            }
        }
        if(!static_bindings.empty()){for(const auto& interface:metadata.interfaces)if(!static_bindings.contains(interface.name))throw std::invalid_argument("靜態 shader 缺少 interface 綁定："+interface.name);metadata.interfaces.clear();}
        return {declarations.str()+prototypes.str()+join(rewrite(code,variables))+definitions.str(),metadata};
    }
};
inline Lowered lower_classes(std::string_view source){ClassLowering compiler;compiler.parse(tokenize(source));if(compiler.classes.empty())return {std::string(source),{}};return compiler.lower();}
}
