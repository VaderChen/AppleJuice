#pragma once
#include "hlsl_linkage.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::dxbc {
// 所有 offset 相對於所屬 DXBC chunk；先做範圍檢查，不能直接解參照客體資料。
struct Reader {
    std::span<const std::uint8_t> bytes;
    void check(std::uint64_t at,std::uint64_t size)const {
        if(at>bytes.size() || size>bytes.size()-at)throw std::invalid_argument("DXBC reflection 範圍越界");
    }
    std::uint32_t u32(std::uint64_t at)const {
        check(at,4);std::uint32_t value{};for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[static_cast<std::size_t>(at+i)]}<<(8*i);return value;
    }
    std::uint32_t u16(std::uint64_t at)const {
        check(at,2);return bytes[static_cast<std::size_t>(at)]|(std::uint32_t{bytes[static_cast<std::size_t>(at+1)]}<<8);
    }
    std::string string(std::uint64_t at)const {
        check(at,1);const auto tail=bytes.subspan(static_cast<std::size_t>(at));
        const auto end=std::find(tail.begin(),tail.end(),0);
        if(end==tail.end())throw std::invalid_argument("DXBC reflection 字串缺少終止字元");
        return {reinterpret_cast<const char*>(tail.data()),static_cast<std::size_t>(end-tail.begin())};
    }
    Reader sub(std::uint64_t at,std::uint64_t size)const {
        check(at,size);return {bytes.subspan(static_cast<std::size_t>(at),static_cast<std::size_t>(size))};
    }
};
struct Type;
struct Member {std::string name;std::uint32_t offset{};std::shared_ptr<Type> type;};
struct Type {
    std::string name;
    std::uint32_t id{},classification{},base{},rows{},columns{},elements{};
    std::vector<Member> members;
    std::shared_ptr<Type> subtype,parent;
    std::vector<std::shared_ptr<Type>> interfaces;
};
struct Variable {
    std::string name;
    std::uint32_t offset{},size{},flags{},texture{UINT32_MAX},textures{},sampler{UINT32_MAX},samplers{};
    std::shared_ptr<Type> type;std::vector<std::uint8_t> initial;
};
struct Buffer {std::string name;std::uint32_t size{},flags{},type{};std::vector<Variable> variables;};
struct Binding {std::string name;std::uint32_t type{},format{},dimension{},samples{},slot{},count{},flags{},space{},id{};};
struct Signature {std::string name;std::uint32_t index{},system{},component{},reg{},mask{},used{},stream{},precision{};};
struct Reflection {
    std::optional<hlsl_linkage::Metadata> linkage;
    std::uint64_t requires_flags{};
    std::string creator;
    std::uint32_t version{},flags{},interface_slots{};
    std::vector<Buffer> buffers;
    std::vector<Binding> bindings;
    std::array<std::vector<Signature>,3> signatures;
    std::vector<std::uint32_t> statistics;
    std::array<std::uint32_t,3> threads{};

    explicit Reflection(std::span<const std::uint8_t> data){
        Reader container{data};
        if(container.u32(0)!=0x43425844 || container.u32(24)!=data.size())throw std::invalid_argument("DXBC reflection 容器不合法");
        const auto count=container.u32(28);container.check(32,std::uint64_t{count}*4);
        if(count>64)throw std::invalid_argument("DXBC reflection 區塊過多");
        for(unsigned i=0;i<count;++i){
            const auto at=container.u32(32+4*i),tag=container.u32(at),size=container.u32(std::uint64_t{at}+4);
            const auto chunk=container.sub(std::uint64_t{at}+8,size);
            if(tag==hlsl_linkage::tag)linkage=hlsl_linkage::Metadata::decode(chunk.bytes);
            else if(tag==0x46454452)read_rdef(chunk);
            else if(tag==0x54415453){if(size%4)throw std::invalid_argument("DXBC STAT 長度不合法");for(unsigned j=0;j<size/4;++j)statistics.push_back(chunk.u32(4*j));}
            else if(tag==0x4e475349 || tag==0x31475349)read_signature(chunk,0,tag==0x31475349?2U:0U);
            else if(tag==0x4e47534f || tag==0x3147534f || tag==0x3547534f)read_signature(chunk,1,tag==0x3147534f?2U:tag==0x3547534f?1U:0U);
            else if(tag==0x47534350 || tag==0x31475350)read_signature(chunk,2,tag==0x31475350?2U:0U);
            else if(tag==0x30494653){requires_flags=chunk.u32(0)|(std::uint64_t{chunk.u32(4)}<<32);}
            else if(tag==0x45434649){interface_slots=chunk.u32(12);}
            else if(tag==0x58454853 || tag==0x52444853){
                version=chunk.u32(0);const auto words=chunk.u32(4);
                if(words<2 || std::uint64_t{words}*4!=size)throw std::invalid_argument("DXBC reflection shader 長度不合法");
                for(unsigned j=2;j<words;){
                    const auto token=chunk.u32(4*j),opcode=token&0x7ffU;
                    const auto length=opcode==53?chunk.u32(4ULL*j+4):(token>>24)&127U;
                    if(!length || length>words-j)throw std::invalid_argument("DXBC reflection 指令長度不合法");
                    if(opcode==155 && length==4)threads={chunk.u32(4ULL*j+4),chunk.u32(4ULL*j+8),chunk.u32(4ULL*j+12)};
                    j+=length;
                }
            }
        }
        if(linkage){
            std::erase_if(bindings,[](const auto& b){return b.name=="__x64_Linkage";});
            std::erase_if(buffers,[](const auto& b){return b.name=="__x64_Linkage";});
            Buffer pointers;pointers.name="$Interfaces";pointers.type=2;
            for(const auto& entry:linkage->interfaces){
                Variable variable;variable.name=entry.name;variable.offset=entry.slot;variable.size=entry.count;variable.flags=6|(entry.parameter?8U:0U);
                variable.type=std::make_shared<Type>();variable.type->name=entry.type;variable.type->classification=7;variable.type->base=37;variable.type->elements=entry.count>1?entry.count:0;
                pointers.variables.push_back(std::move(variable));interface_slots=std::max(interface_slots,entry.slot+entry.count);
            }
            pointers.size=interface_slots;buffers.push_back(std::move(pointers));
        }
    }
private:
    void read_signature(Reader data,unsigned kind,unsigned extended){
        const auto count=data.u32(0),start=data.u32(4),stride=24U+(extended?4U:0U)+(extended==2?4U:0U);
        data.check(start,std::uint64_t{count}*stride);
        for(unsigned i=0;i<count;++i){
            std::uint64_t at=std::uint64_t{start}+i*stride;Signature s;
            if(extended){s.stream=data.u32(at);at+=4;}
            s.name=data.string(data.u32(at));s.index=data.u32(at+4);s.system=data.u32(at+8);
            s.component=data.u32(at+12);s.reg=data.u32(at+16);const auto masks=data.u32(at+20);
            s.mask=masks&255U;s.used=(masks>>8)&255U;if(extended==2)s.precision=data.u32(at+24);
            signatures[kind].push_back(std::move(s));
        }
    }
    void read_rdef(Reader data){
        const auto count=data.u32(0),start=data.u32(4),binding_count=data.u32(8),binding_start=data.u32(12);
        const unsigned major=(data.u32(16)>>8)&255U;
        flags=data.u32(20);creator=data.string(data.u32(24));
        unsigned variable_stride=24,type_stride=16,binding_stride=32;
        if(major>=5){
            const auto magic=data.u32(28);
            if((magic!=0x31314452 && magic!=0x52443131) || data.u32(32)!=60 || data.u32(36)!=24 || data.u32(52)!=12)
                throw std::invalid_argument("DXBC RD11 格式不合法");
            binding_stride=data.u32(40);variable_stride=data.u32(44);type_stride=data.u32(48);interface_slots=data.u32(56);
            if((binding_stride!=32 && binding_stride!=40) || variable_stride!=40 || type_stride!=36)throw std::invalid_argument("DXBC RD11 結構長度不合法");
        }
        std::map<std::uint32_t,std::shared_ptr<Type>> types;
        std::set<std::uint32_t> visiting;
        const auto type=[&](auto&& self,std::uint32_t at,unsigned depth)->std::shared_ptr<Type>{
            if(depth>64 || visiting.contains(at))throw std::invalid_argument("DXBC 型別遞迴不合法");
            if(const auto found=types.find(at);found!=types.end())return found->second;
            if(types.size()>65536)throw std::invalid_argument("DXBC 型別數量超過上限");
            data.check(at,type_stride);auto t=std::make_shared<Type>();t->id=at;
            t->classification=data.u16(at);t->base=data.u16(std::uint64_t{at}+2);t->rows=data.u16(std::uint64_t{at}+4);
            t->columns=data.u16(std::uint64_t{at}+6);t->elements=data.u16(std::uint64_t{at}+8);
            const auto members=data.u16(std::uint64_t{at}+10),offset=data.u32(std::uint64_t{at}+12);
            if(type_stride>=36 && data.u32(std::uint64_t{at}+32))t->name=data.string(data.u32(std::uint64_t{at}+32));
            data.check(offset,std::uint64_t{members}*12);visiting.insert(at);
            if(type_stride>=36){
                if(const auto sub=data.u32(std::uint64_t{at}+16))t->subtype=self(self,sub,depth+1);
                if(const auto parent=data.u32(std::uint64_t{at}+20))t->parent=self(self,parent,depth+1);
                const auto count=data.u32(std::uint64_t{at}+24),start=data.u32(std::uint64_t{at}+28);
                data.check(start,std::uint64_t{count}*4);
                for(unsigned i=0;i<count;++i)t->interfaces.push_back(self(self,data.u32(std::uint64_t{start}+i*4),depth+1));
            }
            for(unsigned j=0;j<members;++j){
                const auto row=std::uint64_t{offset}+12*j;
                t->members.push_back({data.string(data.u32(row)),data.u32(row+8),self(self,data.u32(row+4),depth+1)});
            }
            visiting.erase(at);types.emplace(at,t);return t;
        };
        data.check(start,std::uint64_t{count}*24);
        for(unsigned i=0;i<count;++i){
            const auto at=std::uint64_t{start}+24*i;Buffer b;
            b.name=data.string(data.u32(at));const auto n=data.u32(at+4),vars=data.u32(at+8);
            b.size=data.u32(at+12);b.flags=data.u32(at+16);b.type=data.u32(at+20);
            data.check(vars,std::uint64_t{n}*variable_stride);
            for(unsigned j=0;j<n;++j){
                const auto row=std::uint64_t{vars}+j*variable_stride;Variable v;
                v.name=data.string(data.u32(row));v.offset=data.u32(row+4);v.size=data.u32(row+8);v.flags=data.u32(row+12);
                v.type=type(type,data.u32(row+16),0);const auto initial=data.u32(row+20);
                if(initial){const auto bytes=data.sub(initial,v.size).bytes;v.initial.assign(bytes.begin(),bytes.end());}
                if(variable_stride>=40){v.texture=data.u32(row+24);v.textures=data.u32(row+28);v.sampler=data.u32(row+32);v.samplers=data.u32(row+36);}
                b.variables.push_back(std::move(v));
            }
            buffers.push_back(std::move(b));
        }
        data.check(binding_start,std::uint64_t{binding_count}*binding_stride);
        for(unsigned i=0;i<binding_count;++i){
            const auto at=std::uint64_t{binding_start}+i*binding_stride;Binding b;
            b.name=data.string(data.u32(at));b.type=data.u32(at+4);b.format=data.u32(at+8);b.dimension=data.u32(at+12);
            b.samples=data.u32(at+16);b.slot=data.u32(at+20);b.count=data.u32(at+24);b.flags=data.u32(at+28);
            if(binding_stride>=40){b.space=data.u32(at+32);b.id=data.u32(at+36);}bindings.push_back(std::move(b));
        }
    }
};
}
