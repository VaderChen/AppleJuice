#pragma once
#include "d3dx9_constants.hpp"
#include <array>
#include <bit>
#include <cmath>
namespace x64arm::fx9 {
// FX2 ARRAY_SELECTOR 使用標準 CTAB／CLIT／FXLC；每次套用 pass 重新讀取參數。
inline unsigned select_parameter(const Runtime& fx,const Assignment& assignment) {
    if(assignment.kind==1)return fx.find(UINT32_MAX,assignment.parameter);
    if(assignment.kind!=2)throw std::invalid_argument("FX2 不是參數選擇式");
    const auto& payload=assignment.code;const auto size=Program::word(payload,0);
    if(!size || size%4 || size>payload.size()-4)throw std::invalid_argument("FX2 選擇式名稱越界");
    const auto begin=payload.begin()+4,end=begin+size,zero=std::find(begin,end,0);
    if(zero==end)throw std::invalid_argument("FX2 選擇式名稱未結束");
    const unsigned root=fx.find(UINT32_MAX,std::string(begin,zero));
    if(root==UINT32_MAX)throw std::invalid_argument("FX2 選擇式找不到陣列");
    const auto code=std::span(payload).subspan(4+size);
    if(Program::word(code,0)!=0x46580200)throw std::invalid_argument("FX2 preshader 版本無效");
    std::array<std::vector<double>,8> registers;
    const auto write=[&](unsigned table,unsigned offset,double value){
        if(table<1 || table>7 || offset>=65536)throw std::invalid_argument("FX2 preshader 寫入越界");
        auto& values=registers[table];if(values.size()<=offset)values.resize(offset+1);
        values[offset]=table==5?double(value!=0):table==6?std::nearbyint(value):table==1?value:double(float(value));
    };
    d3dx9::Constants constants(code,true);
    for(auto id:constants.roots){const auto& c=constants.nodes[id];const unsigned node_id=fx.find(UINT32_MAX,c.name);
        if(node_id==UINT32_MAX)throw std::invalid_argument("FX2 preshader 參數不存在");
        const auto& node=fx.nodes[node_id];if(node.type.klass>3 || c.klass>3)throw std::invalid_argument("FX2 preshader 常數型別未支援");
        const auto& bytes=fx.values[node.index]->bytes;const unsigned table=c.set==2?2:c.set==1?6:5;
        for(unsigned e=0;e<std::min(node.type.count(),c.elements);++e)for(unsigned r=0;r<std::min(node.type.rows,c.rows);++r)for(unsigned col=0;col<std::min(node.type.columns,c.columns);++col){
            const unsigned raw=Program::word(bytes,node.offset+4*(e*node.type.rows*node.type.columns+r*node.type.columns+col));
            const double value=node.type.scalar==3?std::bit_cast<float>(raw):node.type.scalar==1?double(raw!=0):double(static_cast<std::int32_t>(raw));
            const unsigned per=c.klass==2?c.rows:c.klass==3?c.columns:1;
            const unsigned offset=c.set==0?c.index+e*c.rows*c.columns+r*c.columns+col:4*(c.index+e*per+(c.klass==3?col:r))+(c.klass==3?r:col);
            write(table,offset,value);
        }
    }
    std::span<const std::uint8_t> instructions;
    for(std::size_t at=4;at+4<=code.size();){const auto token=Program::word(code,at);if((token&65535)!=65534)break;
        const unsigned words=token>>16;if(!words || words>(code.size()-at-4)/4)throw std::invalid_argument("FX2 preshader 區段越界");
        const unsigned tag=Program::word(code,at+4);const auto data=code.subspan(at+8,(words-1)*4);
        if(tag==0x434c5846)instructions=data;
        if(tag==0x54494c43){const unsigned count=Program::word(data,0);if(count>65536 || count>(data.size()-4)/8)throw std::invalid_argument("FX2 常數表越界");for(unsigned i=0;i<count;++i){const std::uint64_t raw=Program::word(data,4+i*8)|(std::uint64_t(Program::word(data,8+i*8))<<32);write(1,i,std::bit_cast<double>(raw));}}
        at+=4+words*4;
    }
    const auto read=[&](unsigned table,unsigned offset){if(table<1 || table>7 || offset>=registers[table].size())throw std::invalid_argument("FX2 preshader 讀取越界");return registers[table][offset];};
    std::size_t at=0;const auto take=[&](){auto v=Program::word(instructions,at);at+=4;return v;};
    const unsigned count=Program::bounded(take());
    for(unsigned i=0;i<count;++i){const unsigned token=take(),op=(token>>20)&0x7ff,n=token&65535,inputs=take();
        if(!n || n>4 || inputs>3)throw std::invalid_argument("FX2 preshader 指令維度未支援");
        struct Operand{unsigned table,offset;};std::array<Operand,4> operands{};
        for(unsigned j=0;j<=inputs;++j){unsigned relative=take(),shift=0;if(relative){if(relative!=1 || j==inputs)throw std::invalid_argument("FX2 preshader 相對定址無效");auto table=take(),offset=take();const double index=read(table,offset);if(!std::isfinite(index) || index<0 || index>65535)throw std::invalid_argument("FX2 preshader 相對索引越界");shift=static_cast<unsigned>(index);}
            auto table=take(),offset=take();if(table==5)offset/=4;if(shift>65535-offset)throw std::invalid_argument("FX2 preshader 相對位移越界");operands[j]={table,offset+shift};}
        const auto expected=op>=0x100 && op<=0x10c?1U:op>=0x200 && op<=0x208?2U:op==0x300?3U:op==0x500?2U:UINT32_MAX;
        if(inputs!=expected)throw std::invalid_argument("FX2 preshader 運算未支援");
        std::array<double,4> result{};
        for(unsigned c=0;c<n;++c){std::array<double,3> v{};for(unsigned j=0;j<inputs;++j)v[j]=read(operands[j].table,operands[j].offset+((token&0x80000000U)&&j==0?0:c));
            double x=v[0],y=v[1];switch(op){case 0x100:result[c]=x;break;case 0x101:result[c]=-x;break;case 0x103:result[c]=1/x;break;case 0x104:result[c]=x-std::floor(x);break;case 0x105:result[c]=std::exp2(x);break;case 0x106:result[c]=std::log2(x);break;case 0x107:result[c]=1/std::sqrt(x);break;case 0x108:result[c]=std::sin(x);break;case 0x109:result[c]=std::cos(x);break;case 0x10a:result[c]=std::asin(x);break;case 0x10b:result[c]=std::acos(x);break;case 0x10c:result[c]=std::atan(x);break;case 0x200:result[c]=std::min(x,y);break;case 0x201:result[c]=std::max(x,y);break;case 0x202:result[c]=x<y;break;case 0x203:result[c]=x>=y;break;case 0x204:result[c]=x+y;break;case 0x205:result[c]=x*y;break;case 0x206:result[c]=std::atan2(x,y);break;case 0x208:result[c]=x/y;break;case 0x300:result[c]=x>=0?y:v[2];break;case 0x500:result[0]+=x*y;break;default:throw std::invalid_argument("FX2 preshader 運算未支援");}}
        for(unsigned c=0;c<(op==0x500?1:n);++c)write(operands[inputs].table,operands[inputs].offset+c,result[c]);
    }
    if(at!=instructions.size())throw std::invalid_argument("FX2 preshader 尾端資料無效");
    const double selected=read(6,0);if(!std::isfinite(selected) || selected<0 || selected>=static_cast<double>(fx.nodes[root].elements.size()))throw std::invalid_argument("FX2 陣列索引越界");
    return fx.nodes[root].elements[static_cast<unsigned>(selected)];
}
}
