#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace x64arm::d3dx9 {
struct Constant {
    std::string name;
    unsigned name_offset{},set{},index{},registers{},klass{},type{},rows{},columns{},elements{},default_offset{},bytes{};
    std::vector<unsigned> members,array;
};
struct Constants {
    std::vector<std::uint8_t> data;
    std::vector<Constant> nodes;
    std::vector<unsigned> roots;
    unsigned version{},creator{};
    unsigned find(unsigned parent,std::string_view path)const{
        if(path.empty())return UINT32_MAX;
        auto scope=parent==UINT32_MAX?roots:nodes.at(parent).members;
        const auto end=path.find_first_of("[.");const auto name=path.substr(0,end);unsigned current=UINT32_MAX;
        for(auto i:scope)if(nodes[i].name==name){current=i;break;}
        if(current==UINT32_MAX)return current;
        std::size_t at=end==path.npos?path.size():end;
        while(at<path.size()){
            if(path[at]=='.')return find(current,path.substr(at+1));
            if(path[at++]!='[')return UINT32_MAX;
            unsigned index=0;const auto begin=at;
            while(at<path.size() && path[at]>='0' && path[at]<='9'){if(index>65535)return UINT32_MAX;index=index*10+unsigned(path[at++]-'0');}
            if(at==begin || at==path.size() || path[at++]!=']' || index>=nodes[current].array.size())return UINT32_MAX;
            current=nodes[current].array[index];
        }
        return current;
    }
    unsigned number(std::size_t at,unsigned size=4)const{if(at>data.size() || size>data.size()-at)throw std::invalid_argument("CTAB 欄位越界");unsigned v=0;std::memcpy(&v,data.data()+at,size);return v;}
    std::string text(unsigned at)const{if(at>=data.size())throw std::invalid_argument("CTAB 名稱越界");const auto begin=data.begin()+at,end=std::find(begin,data.end(),0);if(end==data.end())throw std::invalid_argument("CTAB 名稱缺少結尾");return {begin,end};}
    unsigned parse(unsigned name,unsigned type,unsigned set,unsigned index,unsigned registers,unsigned defaults,unsigned depth=0,unsigned array_count=0){
        if(depth>32 || nodes.size()>=65536)throw std::invalid_argument("CTAB 結構超限");
        Constant n;n.name=text(name);n.name_offset=name;n.set=set;n.index=index;n.registers=registers;n.default_offset=defaults;n.klass=number(type,2);n.type=number(type+2,2);n.rows=number(type+4,2);n.columns=number(type+6,2);n.elements=array_count?1:number(type+8,2);const auto members=number(type+10,2),member_offset=number(type+12);
        if(n.klass>5 || n.type>19 || set>3 || !n.elements || n.elements>65536 || members>65536)throw std::invalid_argument("CTAB 型別不合法");
        const unsigned id=static_cast<unsigned>(nodes.size());nodes.push_back(n);
        unsigned consumed=0,total_bytes=0;
        if(n.elements>1){for(unsigned i=0;i<n.elements;++i){const auto child=parse(name,type,set,index+consumed,registers>consumed?registers-consumed:0,defaults?defaults+consumed*(set==0?4:16):0,depth+1,1);nodes[id].array.push_back(child);consumed+=nodes[child].registers;total_bytes+=nodes[child].bytes;}}
        else if(n.klass==5){for(unsigned i=0;i<members;++i){const auto child=parse(number(member_offset+i*8),number(member_offset+i*8+4),set,index+consumed,registers>consumed?registers-consumed:0,defaults?defaults+consumed*(set==0?4:16):0,depth+1);nodes[id].members.push_back(child);consumed+=nodes[child].registers;total_bytes+=nodes[child].bytes;}}
        else{if(!n.rows || !n.columns || n.rows>4 || n.columns>4)throw std::invalid_argument("CTAB 分量不合法");consumed=set==0?n.rows*n.columns:n.klass==2?n.rows:n.klass==3?n.columns:1;total_bytes=n.rows*n.columns*4;}
        nodes[id].registers=std::min(registers,consumed);nodes[id].bytes=total_bytes;return id;
    }
    explicit Constants(std::span<const std::uint8_t> shader,bool allow_absent=false){
        const auto word=[&](std::size_t at){if(at>shader.size() || shader.size()-at<4)throw std::invalid_argument("shader token 截斷");unsigned v=0;std::memcpy(&v,shader.data()+at,4);return v;};
        // CTAB 是 COMMENT payload；只接受有完整範圍的容器。
        for(std::size_t at=4;at+8<=shader.size();at+=4){const auto token=word(at);if((token&0xffff)!=0xfffe || word(at+4)!=0x42415443)continue;const auto count=(token>>16)&0x7fff;if(count<8 || std::uint64_t{count}*4>shader.size()-at-4)throw std::invalid_argument("CTAB 容器長度不合法");data.assign(shader.begin()+static_cast<std::ptrdiff_t>(at+8),shader.begin()+static_cast<std::ptrdiff_t>(at+4+count*4));break;}
        if(data.empty()){if(allow_absent){version=word(0);return;}throw std::invalid_argument("shader 沒有 CTAB");}if(number(0)!=28)throw std::invalid_argument("CTAB 標頭不合法");creator=number(4);version=number(8);(void)text(creator);const auto count=number(12),offset=number(16);if(count>65536)throw std::invalid_argument("CTAB 常數過多");
        for(unsigned i=0;i<count;++i){const auto at=offset+i*20;roots.push_back(parse(number(at),number(at+12),number(at+4,2),number(at+6,2),number(at+8,2),number(at+16)));}
    }
};
}
