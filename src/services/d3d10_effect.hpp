#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::fx10 {
// FX4 容器依 SDK EffectBinaryFormat.h 解碼；不從 HLSL 字串猜測 pass 行為。
struct Type;
struct Member { std::string name,semantic;std::uint32_t offset{};std::shared_ptr<Type> type; };
struct Type {
    std::string name;
    std::uint32_t kind{},elements{},size{},stride{},packed{},detail{};
    std::vector<Member> members;
    unsigned count()const{return std::max(1U,elements);}
    unsigned rows()const{return (detail>>8)&7;}
    unsigned columns()const{return (detail>>11)&7;}
    unsigned scalar()const{return (detail>>3)&31;}
};
struct Assignment { std::uint32_t state{},index{},kind{},offset{}; };
struct Annotation { std::string name;std::shared_ptr<Type> type;std::vector<std::uint32_t> values; };
struct ObjectValue { std::uint32_t data{},stream_output{};std::vector<Assignment> assignments; };
struct Variable {
    std::string name,semantic;std::shared_ptr<Type> type;
    std::uint32_t buffer{UINT32_MAX},offset{},flags{},bind{UINT32_MAX};
    std::vector<Annotation> annotations;
    std::vector<ObjectValue> objects;
};
struct Buffer { std::string name;std::uint32_t flags{},bind{};std::vector<std::uint8_t> bytes;std::vector<unsigned> variables;std::vector<Annotation> annotations; };
struct Pass { std::string name;std::vector<Annotation> annotations;std::vector<Assignment> assignments; };
struct Technique { std::string name;std::vector<Annotation> annotations;std::vector<Pass> passes; };
struct Program {
    std::vector<std::uint8_t> data,structured;
    std::vector<Buffer> buffers;
    std::vector<Variable> variables;
    std::vector<Technique> techniques;
    std::map<std::uint32_t,std::shared_ptr<Type>> types;
    std::size_t cursor{};
    bool independent_blend{};
    static std::uint32_t word(std::span<const std::uint8_t> b,std::size_t at){
        if(at>b.size() || b.size()-at<4)throw std::invalid_argument("FX4 DWORD 越界");
        return std::uint32_t{b[at]}|(std::uint32_t{b[at+1]}<<8)|(std::uint32_t{b[at+2]}<<16)|(std::uint32_t{b[at+3]}<<24);
    }
    static void put(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t value){if(at>b.size() || b.size()-at<4)throw std::invalid_argument("FX4 常數越界");for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
    std::uint32_t get(std::size_t at)const{return word(data,at);}
    std::uint32_t next(){const auto value=word(structured,cursor);cursor+=4;return value;}
    static unsigned bounded(unsigned n){if(n>65536)throw std::invalid_argument("FX4 元素數量超限");return n;}
    std::string string(std::uint32_t at)const{
        if(!at)return {};if(at>=data.size())throw std::invalid_argument("FX4 字串越界");
        const auto end=std::find(data.begin()+at,data.end(),0);if(end==data.end())throw std::invalid_argument("FX4 字串未結束");return std::string(data.begin()+at,end);
    }
    std::vector<std::uint8_t> blob(std::uint32_t at)const{
        if(!at)return {};const auto size=get(at);if(size>data.size()-at-4)throw std::invalid_argument("FX4 shader 越界");return {data.begin()+at+4,data.begin()+at+4+size};
    }
    std::shared_ptr<Type> type(std::uint32_t at,unsigned depth=0){
        if(depth>64)throw std::invalid_argument("FX4 型別遞迴超限");if(auto found=types.find(at);found!=types.end())return found->second;
        auto t=std::make_shared<Type>();t->name=string(get(at));t->kind=get(at+4);t->elements=bounded(get(at+8));t->size=get(at+12);t->stride=get(at+16);t->packed=get(at+20);t->detail=get(at+24);
        if(t->size>16*1024*1024 || t->stride>16*1024*1024 || t->packed>16*1024*1024)throw std::invalid_argument("FX4 型別大小超限");
        if(t->kind==3){const auto count=bounded(t->detail);for(unsigned i=0;i<count;++i){const auto m=at+28+16*i;Member v;v.name=string(get(m));v.semantic=string(get(m+4));v.offset=get(m+8);v.type=type(get(m+12),depth+1);t->members.push_back(std::move(v));}}
        else if(t->kind!=1 && t->kind!=2)throw std::invalid_argument("FX4 型別種類無效");
        types.emplace(at,t);return t;
    }
    void unpack(const Type& t,std::uint32_t& from,std::vector<std::uint8_t>& to,std::uint32_t offset)const{
        for(unsigned element=0;element<t.count();++element){const auto base=offset+element*t.stride;
            if(t.kind==3){for(const auto& member:t.members)unpack(*member.type,from,to,base+member.offset);}
            else if(t.kind==1){const auto rows=t.rows(),columns=t.columns();if(!rows || rows>4 || !columns || columns>4)throw std::invalid_argument("FX4 數值維度無效");
                for(unsigned row=0;row<rows;++row)for(unsigned column=0;column<columns;++column){const auto target=(t.detail&7)==3?((t.detail&0x4000)?column*16+row*4:row*16+column*4):column*4;put(to,base+target,get(from));from+=4;}
            }else throw std::invalid_argument("FX4 物件不可當數值初始化");
        }
    }
    std::vector<Annotation> annotations(){
        std::vector<Annotation> result;const auto count=bounded(next());for(unsigned i=0;i<count;++i){Annotation a;a.name=string(next());a.type=type(next());const auto n=a.type->kind==2 && a.type->detail==1?a.type->count():1;for(unsigned j=0;j<n;++j)a.values.push_back(next());result.push_back(std::move(a));}return result;
    }
    std::vector<Assignment> assignments(unsigned count){std::vector<Assignment> result;bounded(count);for(unsigned i=0;i<count;++i){Assignment a;a.state=next();a.index=next();a.kind=next();a.offset=next();if(a.kind<1 || a.kind>7)throw std::invalid_argument("FX4 assignment 種類無效");result.push_back(a);}return result;}
    explicit Program(std::span<const std::uint8_t> bytes){
        if(bytes.size()<76 || (word(bytes,0)!=0xfeff1001 && word(bytes,0)!=0xfeff1011))throw std::invalid_argument("FX4 容器標頭無效");
        independent_blend=word(bytes,0)==0xfeff1011;
        const auto size=word(bytes,32);if(size>bytes.size()-76)throw std::invalid_argument("FX4 區段長度無效");data.assign(bytes.begin()+76,bytes.begin()+76+size);structured.assign(bytes.begin()+76+size,bytes.end());
        if(word(bytes,16) || word(bytes,20) || word(bytes,24))throw std::invalid_argument("FX4 shared pool 需要對應 pool 資料");
        const auto cb=bounded(word(bytes,4)),objects=bounded(word(bytes,12)),techs=bounded(word(bytes,28));
        for(unsigned i=0;i<cb;++i){Buffer b;b.name=string(next());const auto length=next();if(length>65536)throw std::invalid_argument("FX4 cbuffer 大小超限");b.bytes.resize(length);b.flags=next();const auto count=bounded(next());b.bind=next();b.annotations=annotations();
            for(unsigned j=0;j<count;++j){Variable v;v.name=string(next());v.type=type(next());v.semantic=string(next());v.offset=next();auto defaults=next();v.flags=next();v.buffer=i;v.annotations=annotations();if(v.offset>b.bytes.size() || v.type->size>b.bytes.size()-v.offset)throw std::invalid_argument("FX4 變數超出 cbuffer");if(defaults)unpack(*v.type,defaults,b.bytes,v.offset);b.variables.push_back(static_cast<unsigned>(variables.size()));variables.push_back(std::move(v));}buffers.push_back(std::move(b));
        }
        for(unsigned i=0;i<objects;++i){Variable v;v.name=string(next());v.type=type(next());v.semantic=string(next());v.bind=next();if(v.type->kind!=2)throw std::invalid_argument("FX4 物件變數型別無效");const auto object=v.type->detail;
            for(unsigned j=0;j<v.type->count();++j){ObjectValue value;if(object==1 || (object>=5 && object<=8)){value.data=next();if(object==8)value.stream_output=next();}else if(object==2 || object==3 || object==4 || object==21)value.assignments=assignments(next());else if(object<9 || object>23)throw std::invalid_argument("FX4 物件類別無效");v.objects.push_back(std::move(value));}v.annotations=annotations();variables.push_back(std::move(v));
        }
        for(unsigned i=0;i<techs;++i){Technique t;t.name=string(next());const auto count=bounded(next());t.annotations=annotations();for(unsigned j=0;j<count;++j){Pass p;p.name=string(next());const auto n=bounded(next());p.annotations=annotations();p.assignments=assignments(n);t.passes.push_back(std::move(p));}techniques.push_back(std::move(t));}
        if(cursor!=structured.size())throw std::invalid_argument("FX4 結構尾端資料未解析");
    }
};
}
