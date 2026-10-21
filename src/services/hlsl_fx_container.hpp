#pragma once
#include "hlsl_fx_linkage.hpp"
#include <cstring>

namespace x64arm::hlsl_linkage {
struct FxContainer {
    std::vector<std::uint8_t> header,data,structured;
    const FxLinkage& plan;
    const std::map<std::string,std::vector<std::uint8_t>>& bytecodes;
    std::map<std::string,std::uint32_t> types,strings;
    std::map<std::uint32_t,std::uint32_t> rewritten;
    std::set<std::string> replaced;
    std::size_t cursor{};std::uint32_t instances{};
    static std::uint32_t word(const std::vector<std::uint8_t>& b,std::size_t at){if(at>b.size() || b.size()-at<4)throw std::invalid_argument("FX 容器讀取越界");return std::uint32_t{b[at]}|(std::uint32_t{b[at+1]}<<8)|(std::uint32_t{b[at+2]}<<16)|(std::uint32_t{b[at+3]}<<24);}
    static void set(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t value){if(at>b.size() || b.size()-at<4)throw std::invalid_argument("FX 容器寫入越界");for(unsigned n=0;n<4;++n)b[at+n]=static_cast<std::uint8_t>(value>>(8*n));}
    static std::uint32_t append(std::vector<std::uint8_t>& b,std::initializer_list<std::uint32_t> values){if(b.size()>UINT32_MAX-values.size()*4)throw std::invalid_argument("FX 容器過大");const auto at=static_cast<std::uint32_t>(b.size());for(auto v:values)for(unsigned n=0;n<4;++n)b.push_back(static_cast<std::uint8_t>(v>>(8*n)));return at;}
    std::string name(std::uint32_t at)const{if(at>=data.size())throw std::invalid_argument("FX 名稱越界");const auto end=std::find(data.begin()+at,data.end(),0);if(end==data.end())throw std::invalid_argument("FX 名稱未結束");return std::string(data.begin()+at,end);}
    std::uint32_t string(const std::string& value){if(auto i=strings.find(value);i!=strings.end())return i->second;const auto at=static_cast<std::uint32_t>(data.size());data.insert(data.end(),value.begin(),value.end());data.push_back(0);strings[value]=at;return at;}
    std::uint32_t type(const std::string& key,unsigned elements=0){
        const auto id=key+"["+std::to_string(elements)+"]";if(auto at=types.find(id);at!=types.end())return at->second;
        const auto n=string(key);std::uint32_t at=0;
        if(auto found=plan.parsed.classes.find(key);found!=plan.parsed.classes.end()){
            const auto& c=found->second;
            if(c.abstract)at=append(data,{n,4,elements,0,0,0});
            else {
                std::vector<std::uint32_t> field_types,field_names,interfaces;std::uint32_t packed=0,base=0;
                for(const auto& field:c.fields){field_types.push_back(type(field.type,field.count>1?field.count:0));field_names.push_back(string(field.name));packed+=field.width*field.count;}
                for(const auto& parent:c.parents){if(plan.parsed.classes.at(parent).abstract)interfaces.push_back(type(parent));else {if(base)throw std::invalid_argument("FX class 多重具體繼承不合法");base=type(parent);}}
                at=append(data,{n,3,elements,c.size*std::max(1U,elements),c.size,packed*std::max(1U,elements),static_cast<std::uint32_t>(c.fields.size())});
                for(std::size_t i=0;i<c.fields.size();++i)append(data,{field_names[i],0,c.fields[i].offset,field_types[i]});append(data,{base,static_cast<std::uint32_t>(interfaces.size())});for(auto value:interfaces)append(data,{value});
            }
        }else {
            const auto width=ClassLowering::width(key),columns=width/4;unsigned scalar=key.starts_with("float")?1:key.starts_with("uint")?3:key.starts_with("int")?2:4;
            const auto descriptor=(columns==1?1U:2U)|(scalar<<3)|(1U<<8)|(columns<<11);
            at=append(data,{n,1,elements,elements?16*(elements-1)+width:width,16,width*std::max(1U,elements),descriptor});
        }
        types[id]=at;return at;
    }
    std::uint32_t rewrite_type(std::uint32_t old,unsigned depth=0){
        if(depth>64)throw std::invalid_argument("FX 型別層次過深");if(auto at=rewritten.find(old);at!=rewritten.end())return at->second;
        const auto typename_=name(word(data,old));if(plan.parsed.classes.contains(typename_)){const auto at=type(typename_,word(data,old+8));rewritten[old]=at;return at;}
        rewritten[old]=old;const auto kind=word(data,old+4);
        if(kind==3){const auto members=word(data,old+24);if(members>65536)throw std::invalid_argument("FX struct 成員過多");for(unsigned i=0;i<members;++i){const auto field=old+28+i*16+12;const auto value=rewrite_type(word(data,field),depth+1);set(data,field,value);}}
        return old;
    }
    std::uint32_t next(){const auto value=word(structured,cursor);cursor+=4;return value;}
    void skip(std::size_t size){if(size>structured.size()-cursor)throw std::invalid_argument("FX 結構越界");cursor+=size;}
    std::uint32_t type_field(){const auto at=cursor;const auto old=next();const auto rewritten_type=rewrite_type(old);set(structured,at,rewritten_type);return rewritten_type;}
    void annotations(){const auto count=next();if(count>65536)throw std::invalid_argument("FX annotation 數量過多");for(unsigned i=0;i<count;++i){next();const auto at=type_field();const auto kind=word(data,at+4);skip(kind==2 && word(data,at+24)==1?std::max(1U,word(data,at+8))*4:4);}}
    void shader(std::vector<std::uint8_t>& block,std::size_t at){
        const auto old=word(block,at);if(!old)return;const auto size=word(data,old);if(size>data.size()-old-4)throw std::invalid_argument("FX shader bytecode 越界");
        std::string selected;for(const auto& [key,bytes]:bytecodes){std::vector<std::uint8_t> marker(key.begin(),key.end());marker.push_back(0);if(std::search(data.begin()+old+4,data.begin()+old+4+size,marker.begin(),marker.end())!=data.begin()+old+4+size){if(!selected.empty())throw std::invalid_argument("FX shader 識別衝突");selected=key;}}
        if(selected.empty())throw std::invalid_argument("FX 容器含未對應的 shader");const auto& bytes=bytecodes.at(selected);if(bytes.size()<32 || std::memcmp(bytes.data(),"DXBC",4))throw std::invalid_argument("FX shader 編譯結果無效");
        const bool same=&block==&data;const auto target=append(data,{static_cast<std::uint32_t>(bytes.size())});data.insert(data.end(),bytes.begin(),bytes.end());if(same)set(data,at,target);else set(block,at,target);replaced.insert(selected);
    }
    void assignments(unsigned count){if(count>65536)throw std::invalid_argument("FX pass assignment 過多");for(unsigned i=0;i<count;++i){next();next();const auto kind=next(),offset=next();if(kind==8 || kind==7)shader(data,offset);}}
    FxContainer(const std::vector<std::uint8_t>& bytes,const FxLinkage& p,const std::map<std::string,std::vector<std::uint8_t>>& codes):plan(p),bytecodes(codes){
        if(bytes.size()<96 || word(bytes,0)!=0xfeff2001)throw std::invalid_argument("動態 linkage 需要有效的 FX5 容器");const auto size=word(bytes,32);if(size>bytes.size()-96)throw std::invalid_argument("FX 區段長度無效");header.assign(bytes.begin(),bytes.begin()+96);data.assign(bytes.begin()+96,bytes.begin()+96+size);structured.assign(bytes.begin()+96+size,bytes.end());
        if(word(header,16) || word(header,20) || word(header,24))throw std::invalid_argument("FX class 的 shared pool 尚未支援");
    }
    std::vector<std::uint8_t> finish(){
        const auto buffers=word(header,4),objects=word(header,12),groups=word(header,76);
        for(unsigned b=0;b<buffers;++b){next();next();next();const auto count=next();next();annotations();for(unsigned v=0;v<count;++v){next();const auto at=type_field();skip(16);annotations();const auto key=name(word(data,at));if(auto c=plan.parsed.classes.find(key);c!=plan.parsed.classes.end() && !c->second.abstract){bool implements=false;for(const auto& candidate:plan.parsed.order)if(plan.parsed.classes.at(candidate).abstract && plan.parsed.implements(c->second,candidate))implements=true;if(implements)instances+=std::max(1U,word(data,at+8));}}}
        for(unsigned n=0;n<objects;++n){next();const auto at=type_field();skip(8);if(word(data,at+4)!=2)throw std::invalid_argument("FX object 型別無效");const auto object=word(data,at+24),count=std::max(1U,word(data,at+8));
            for(unsigned element=0;element<count;++element){
                if(object==1)next();else if(object==2 || object==3 || object==4 || object==21)assignments(next());
                else if(object>=25 && object<=30){shader(structured,cursor);skip(36);}
                else if(object>=5 && object<=8){shader(structured,cursor);skip(object==8?8:4);}
                else if(object<9 || object>44)throw std::invalid_argument("FX object 種類未知");
            }annotations();
        }
        const auto insertion=cursor;std::vector<std::uint8_t> variables;
        for(const auto& [name_,typename_]:plan.interfaces)append(variables,{string(name_),type(typename_),0,0,0});
        for(unsigned g=0;g<groups;++g){next();const auto techniques=next();annotations();for(unsigned t=0;t<techniques;++t){next();const auto passes=next();annotations();for(unsigned pass=0;pass<passes;++pass){next();const auto count=next();annotations();assignments(count);}}}
        if(cursor!=structured.size())throw std::invalid_argument("FX 結構尾端未完整解析");if(replaced.size()!=bytecodes.size())throw std::invalid_argument("FX 有 shader 未寫入容器");
        structured.insert(structured.begin()+static_cast<std::ptrdiff_t>(insertion),variables.begin(),variables.end());set(header,32,static_cast<std::uint32_t>(data.size()));set(header,84,static_cast<std::uint32_t>(plan.interfaces.size()));set(header,88,static_cast<std::uint32_t>(plan.interfaces.size()));set(header,92,instances);
        auto result=header;result.insert(result.end(),data.begin(),data.end());result.insert(result.end(),structured.begin(),structured.end());return result;
    }
};
}
