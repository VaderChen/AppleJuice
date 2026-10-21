#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace x64arm::fx9 {
// FX2 的結構與非結構區分開解碼；所有 offset 都以容器區段為界，不解參照客體位址。
struct Type {unsigned scalar{},klass{},rows{},columns{},elements{};std::string name,semantic;std::vector<Type> members;unsigned count()const{return std::max(1U,elements);}unsigned bytes()const{std::uint64_t size=0;if(klass==5)for(const auto& member:members)size+=member.bytes();else size=klass==4?4U:rows*columns*4U;size*=count();if(size>16*1024*1024)throw std::invalid_argument("FX2 型別大小超限");return static_cast<unsigned>(size);}};
struct Assignment {unsigned state{},index{},type_offset{},value_offset{},kind{UINT32_MAX};std::vector<std::uint8_t> code;std::string parameter;};
struct Parameter {Type type;unsigned flags{};std::vector<std::uint8_t> values;std::vector<unsigned> objects;std::vector<std::vector<Assignment>> samplers;std::vector<Parameter> annotations;};
struct Pass {std::string name;std::vector<Parameter> annotations;std::vector<Assignment> assignments;};
struct Technique {std::string name;std::vector<Parameter> annotations;std::vector<Pass> passes;};
struct Program {
    std::vector<std::uint8_t> data,structured;
    std::vector<Parameter> parameters;
    std::vector<Technique> techniques;
    std::map<unsigned,std::vector<std::uint8_t>> objects;
    std::size_t cursor{};
    static unsigned word(std::span<const std::uint8_t> bytes,std::size_t at){if(at>bytes.size() || bytes.size()-at<4)throw std::invalid_argument("FX2 DWORD 越界");return unsigned(bytes[at])|(unsigned(bytes[at+1])<<8)|(unsigned(bytes[at+2])<<16)|(unsigned(bytes[at+3])<<24);}
    static unsigned bounded(unsigned count){if(count>65536)throw std::invalid_argument("FX2 元素數量超限");return count;}
    unsigned next(){auto result=word(structured,cursor);cursor+=4;return result;}
    unsigned take(std::size_t& at)const{auto result=word(data,at);at+=4;return result;}
    static std::vector<std::uint8_t> slice(std::span<const std::uint8_t> bytes,std::size_t at,std::size_t length){if(at>bytes.size() || length>bytes.size()-at)throw std::invalid_argument("FX2 資料越界");return {bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+length)};}
    std::string string(unsigned offset)const{if(!offset)return {};const auto length=word(data,offset);auto bytes=slice(data,std::size_t(offset)+4,length);if(bytes.empty() || bytes.back())throw std::invalid_argument("FX2 字串沒有結束符號");return {bytes.begin(),std::find(bytes.begin(),bytes.end(),0)};}
    Type type(std::size_t& at,unsigned depth=0)const{
        if(depth>64)throw std::invalid_argument("FX2 型別遞迴超限");Type value;value.scalar=take(at);value.klass=take(at);value.name=string(take(at));value.semantic=string(take(at));value.elements=bounded(take(at));
        if(value.klass<=3){value.rows=take(at);value.columns=take(at);if(value.klass==1)std::swap(value.rows,value.columns);if(!value.rows || !value.columns || value.rows>4 || value.columns>4)throw std::invalid_argument("FX2 數值維度無效");}
        else if(value.klass==5){const auto count=bounded(take(at));for(unsigned i=0;i<count;++i)value.members.push_back(type(at,depth+1));}
        else if(value.klass!=4)throw std::invalid_argument("FX2 型別類別無效");(void)value.bytes();return value;
    }
    Parameter parameter(unsigned type_offset,unsigned value_offset,unsigned flags=0)const{
        std::size_t at=type_offset;Parameter value;value.type=type(at);value.flags=flags;
        if(value.type.klass!=4){value.values=value_offset?slice(data,value_offset,value.type.bytes()):std::vector<std::uint8_t>(value.type.bytes());return value;}
        at=value_offset;const bool sampler=value.type.scalar>=10 && value.type.scalar<=14;
        for(unsigned i=0;i<value.type.count();++i){if(sampler){std::vector<Assignment> states;const auto count=value_offset?bounded(take(at)):0;for(unsigned j=0;j<count;++j){Assignment assignment;assignment.state=take(at);assignment.index=take(at);assignment.type_offset=take(at);assignment.value_offset=take(at);states.push_back(assignment);}value.samplers.push_back(std::move(states));}else value.objects.push_back(value_offset?take(at):0);}
        return value;
    }
    std::vector<Parameter> annotations(unsigned count){bounded(count);std::vector<Parameter> result;for(unsigned i=0;i<count;++i){const auto t=next(),v=next();result.push_back(parameter(t,v,4));}return result;}
    Assignment assignment(){Assignment value;value.state=next();value.index=next();value.type_offset=next();value.value_offset=next();return value;}
    std::vector<std::uint8_t> payload(){const auto size=next();auto bytes=slice(structured,cursor,size);cursor+=(std::size_t(size)+3)&~std::size_t(3);if(cursor>structured.size())throw std::invalid_argument("FX2 物件對齊越界");return bytes;}
    explicit Program(std::span<const std::uint8_t> bytes){
        if(bytes.size()<24 || word(bytes,0)!=0xfeff0901)throw std::invalid_argument("FX2 容器標頭無效");const auto size=word(bytes,4);data=slice(bytes,8,size);structured=slice(bytes,std::size_t(size)+8,bytes.size()-8-size);
        const auto parameter_count=bounded(next()),technique_count=bounded(next());(void)next();const auto object_count=bounded(next());
        for(unsigned i=0;i<parameter_count;++i){const auto t=next(),v=next(),flags=next(),count=bounded(next());auto value=parameter(t,v,flags);value.annotations=annotations(count);parameters.push_back(std::move(value));}
        for(unsigned i=0;i<technique_count;++i){Technique technique;technique.name=string(next());const auto count=bounded(next()),passes=bounded(next());technique.annotations=annotations(count);for(unsigned j=0;j<passes;++j){Pass pass;pass.name=string(next());const auto count_annotations=bounded(next()),states=bounded(next());pass.annotations=annotations(count_annotations);for(unsigned k=0;k<states;++k)pass.assignments.push_back(assignment());technique.passes.push_back(std::move(pass));}techniques.push_back(std::move(technique));}
        const auto blobs=bounded(next()),complex=bounded(next());for(unsigned i=0;i<blobs;++i){const auto id=next();if(id>=object_count || objects.contains(id))throw std::invalid_argument("FX2 物件 ID 無效");objects.emplace(id,payload());}
        for(unsigned i=0;i<complex;++i){const auto technique=next(),index=next(),element=next(),state=next(),kind=next();Assignment* assignment=nullptr;
            if(technique==UINT32_MAX){if(index>=parameters.size() || element>=parameters[index].samplers.size() || state>=parameters[index].samplers[element].size())throw std::invalid_argument("FX2 sampler 狀態索引無效");assignment=&parameters[index].samplers[element][state];}
            else {if(technique>=techniques.size() || index>=techniques[technique].passes.size() || state>=techniques[technique].passes[index].assignments.size())throw std::invalid_argument("FX2 pass 狀態索引無效");assignment=&techniques[technique].passes[index].assignments[state];}
            assignment->kind=kind;auto payload_bytes=payload();if(kind==1){if(payload_bytes.empty() || payload_bytes.back())throw std::invalid_argument("FX2 參數引用未結束");assignment->parameter=std::string(payload_bytes.begin(),std::find(payload_bytes.begin(),payload_bytes.end(),0));}
            else if(kind==0 || kind==2)assignment->code=std::move(payload_bytes);else throw std::invalid_argument("FX2 複合狀態種類無效");
        }
    }
};
struct Value {Type type;unsigned flags{};std::vector<std::uint8_t> bytes;std::vector<unsigned> objects;std::vector<std::uint64_t> interfaces;std::vector<std::string> strings;unsigned first{},last{};};
struct Node {unsigned kind{},index{},technique{},offset{};Type type;std::vector<unsigned> children,elements,annotations;};
struct Pool {std::multimap<std::string,std::shared_ptr<Value>> values;};
struct Update {unsigned index{},offset{},kind{};std::vector<std::uint8_t> bytes;std::uint64_t texture{};std::string text;};
struct Runtime {
    Program program;
    std::vector<std::shared_ptr<Value>> values;
    std::vector<Node> nodes;
    std::vector<unsigned> roots,techniques;
    std::vector<std::vector<unsigned>> passes;
    std::map<std::uint64_t,unsigned> handles;
    std::vector<std::uint64_t> pointers,allocations;
    std::map<std::string,std::uint64_t> strings;
    std::map<unsigned,std::uint64_t> shaders;
    std::map<std::string,std::pair<std::uint64_t,std::size_t>> argument_buffers;
    std::vector<std::uint8_t> bytecode;
    std::uint64_t pool{},manager{};
    unsigned technique{UINT32_MAX},pass{UINT32_MAX},flags{};
    bool begun{},recording{};
    std::map<unsigned,std::shared_ptr<Value>> recorded_values;
    std::vector<Update> recording_updates;
    std::map<std::uint64_t,std::vector<Update>> blocks;
    std::map<unsigned,std::pair<unsigned,unsigned>> array_ranges;
    static bool same_type(const Type& a,const Type& b){if(a.scalar!=b.scalar || a.klass!=b.klass || a.rows!=b.rows || a.columns!=b.columns || a.elements!=b.elements || a.members.size()!=b.members.size())return false;for(unsigned i=0;i<a.members.size();++i)if(a.members[i].name!=b.members[i].name || !same_type(a.members[i],b.members[i]))return false;return true;}
    unsigned add_type(const Type& type,unsigned value,unsigned offset=0,unsigned depth=0){
        if(depth>64 || nodes.size()>=65536)throw std::invalid_argument("FX2 handle 數量或階層超限");const auto index=static_cast<unsigned>(nodes.size());Node node;node.index=value;node.offset=offset;node.type=type;nodes.push_back(node);
        if(type.elements){auto element=type;element.elements=0;for(unsigned i=0;i<type.elements;++i){const auto child=add_type(element,value,offset+i*element.bytes(),depth+1);nodes[index].elements.push_back(child);}}
        else if(type.klass==5){for(const auto& member:type.members){const auto child=add_type(member,value,offset,depth+1);nodes[index].children.push_back(child);offset+=member.bytes();}}
        return index;
    }
    unsigned add_parameter(const Parameter& parameter,Pool* shared_pool=nullptr){
        auto value=std::make_shared<Value>();value->type=parameter.type;value->flags=parameter.flags;value->bytes=parameter.values;value->objects=parameter.objects;value->interfaces.resize(parameter.type.klass==4?parameter.type.count():0);value->last=parameter.type.count();
        if(parameter.type.scalar==4){for(auto id:parameter.objects){std::string text;if(auto it=program.objects.find(id);it!=program.objects.end())text.assign(it->second.begin(),std::find(it->second.begin(),it->second.end(),0));value->strings.push_back(text);}}
        if(shared_pool && (parameter.flags&1)){
            // 同名、同型別與同 semantic 才共用；其他參數在 pool 中各自保存。
            const auto range=shared_pool->values.equal_range(parameter.type.name);const auto found=std::find_if(range.first,range.second,[&](const auto& item){return item.second->type.semantic==parameter.type.semantic && same_type(item.second->type,parameter.type);});
            if(found!=range.second)value=found->second;else shared_pool->values.emplace(parameter.type.name,value);
        }
        const auto value_index=static_cast<unsigned>(values.size());values.push_back(value);const auto node=add_type(parameter.type,value_index);for(const auto& annotation:parameter.annotations){const auto child=add_parameter(annotation);nodes[node].annotations.push_back(child);}return node;
    }
    explicit Runtime(std::span<const std::uint8_t> bytes,Pool* pool_data=nullptr):program(bytes),bytecode(bytes.begin(),bytes.end()){
        for(const auto& parameter:program.parameters)roots.push_back(add_parameter(parameter,pool_data));
        for(unsigned i=0;i<program.techniques.size();++i){Node technique_node;technique_node.kind=1;technique_node.index=i;const auto index=static_cast<unsigned>(nodes.size());nodes.push_back(technique_node);techniques.push_back(index);for(const auto& a:program.techniques[i].annotations){const auto child=add_parameter(a);nodes[index].annotations.push_back(child);}std::vector<unsigned> pass_nodes;
            for(unsigned j=0;j<program.techniques[i].passes.size();++j){Node pass_node;pass_node.kind=2;pass_node.index=j;pass_node.technique=i;const auto pass_index=static_cast<unsigned>(nodes.size());nodes.push_back(pass_node);pass_nodes.push_back(pass_index);for(const auto& a:program.techniques[i].passes[j].annotations){const auto child=add_parameter(a);nodes[pass_index].annotations.push_back(child);}}passes.push_back(std::move(pass_nodes));}
    }
    unsigned find(unsigned parent,std::string_view path)const{
        if(path.empty())return UINT32_MAX;const auto split=path.find_first_of(".[");const auto name=path.substr(0,split);const auto& children=parent==UINT32_MAX?roots:nodes.at(parent).children;unsigned result=UINT32_MAX;for(auto child:children)if(nodes[child].type.name==name){result=child;break;}if(result==UINT32_MAX)return result;
        std::size_t at=split;while(at!=path.npos && at<path.size()){if(path[at]=='.')return find(result,path.substr(at+1));if(path[at]!='[')return UINT32_MAX;auto end=path.find(']',at+1);if(end==path.npos || end==at+1)return UINT32_MAX;unsigned element=0;for(auto c:path.substr(at+1,end-at-1)){if(c<'0' || c>'9' || element>65535)return UINT32_MAX;element=element*10+static_cast<unsigned>(c-'0');}if(element>=nodes[result].elements.size())return UINT32_MAX;result=nodes[result].elements[element];at=end+1;}return result;
    }
};

}

#include "d3dx9_effect_preshader.hpp"
