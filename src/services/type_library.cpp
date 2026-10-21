#include "type_library.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <set>

namespace x64arm::typelib {
namespace {
constexpr std::uint32_t absent=UINT32_MAX;
[[noreturn]] void bad(){throw Error(invalid_data,"型別庫資料損毀或參照超出範圍");}
[[noreturn]] void unsupported(){throw Error(unsupported_format,"尚未支援的型別庫格式或常數型別");}
std::size_t aligned(std::size_t value){return (value+3U)&~std::size_t{3};}
struct Bytes {
    std::span<const std::uint8_t> data;
    Bytes slice(std::size_t offset,std::size_t count)const{
        if(offset>data.size() || count>data.size()-offset)bad();
        return {data.subspan(offset,count)};
    }
    std::uint64_t number(std::size_t offset,unsigned width)const{
        const auto part=slice(offset,width);std::uint64_t value{};
        for(unsigned i=0;i<width;++i)value|=std::uint64_t{part.data[i]}<<(8*i);
        return value;
    }
    std::uint32_t u32(std::size_t offset)const{return static_cast<std::uint32_t>(number(offset,4));}
    std::uint16_t u16(std::size_t offset)const{return static_cast<std::uint16_t>(number(offset,2));}
    std::string text(std::size_t offset,std::size_t count)const{
        const auto part=slice(offset,count);return {part.data.begin(),part.data.end()};
    }
};
class Reader {
    Bytes file;
    std::array<Bytes,15> segments{};
    std::map<std::uint32_t,std::string> names,strings;
    std::map<std::uint32_t,Type> descriptors;
    std::set<std::uint32_t> reading;
    std::size_t work{},text_bytes{};
    void account_text(std::size_t count){
        constexpr std::size_t limit=64U*1024U*1024U;
        if(count>limit-text_bytes)bad();text_bytes+=count;
    }
    void consume(std::size_t count=1){
        // 共用資料可以被多處參照；限制展開量，避免小檔案造成無界配置。
        constexpr std::size_t limit=1U<<20;
        if(count>limit-work)bad();work+=count;
    }
    Guid guid(std::uint32_t offset)const{
        Guid value{};if(offset==absent)return value;
        if(offset%24)bad();const auto bytes=segments[5].slice(offset,24);
        std::copy_n(bytes.data.begin(),16,value.begin());return value;
    }
    const std::string& text(const std::map<std::uint32_t,std::string>& table,std::uint32_t offset)const{
        static const std::string empty;
        if(offset==absent)return empty;
        const auto found=table.find(offset);if(found==table.end())bad();return found->second;
    }
    std::string name(std::uint32_t offset){const auto& value=text(names,offset);account_text(value.size());return value;}
    std::string string(std::uint32_t offset){const auto& value=text(strings,offset);account_text(value.size());return value;}
    void read_text(unsigned index,bool identifier){
        const auto bytes=segments[index];
        for(std::size_t offset=0;offset<bytes.data.size();){
            consume();const std::size_t prefix=identifier?12U:2U;
            const std::size_t length=identifier?(bytes.u32(offset+8)&255U):bytes.u16(offset);
            const auto size=std::max(identifier?12U:8U,static_cast<unsigned>(aligned(prefix+length)));
            (void)bytes.slice(offset,size);
            account_text(length);
            (identifier?names:strings).emplace(static_cast<std::uint32_t>(offset),bytes.text(offset+prefix,length));
            offset+=size;
        }
    }
    Constant constant(std::uint32_t offset){
        consume();Constant value;
        if(offset&0x80000000U){
            value.vartype=static_cast<std::uint16_t>((offset>>26)&31U);value.bits=offset&0x03ffffffU;
        }else{
            const auto bytes=segments[11];value.vartype=bytes.u16(offset);unsigned width{};
            switch(value.vartype){
                case 0:case 1:case 2:case 3:case 4:case 10:case 11:case 16:case 17:case 18:case 19:case 22:case 23:case 24:case 25:width=4;break;
                case 5:case 6:case 7:case 20:case 21:case 64:width=8;break;
                case 8:{const auto size=bytes.u32(std::size_t{offset}+2);if(size!=absent){account_text(size);value.text=bytes.text(std::size_t{offset}+6,size);}return value;}
                default:unsupported();
            }
            value.bits=bytes.number(std::size_t{offset}+2,width);
        }
        // 以 Windows VARIANT 數值位元保存；不把宿主 long／double 布局帶入磁碟解析。
        switch(value.vartype){
            case 0:case 1:case 24:value.bits=0;break;
            case 16:case 17:value.bits&=0xff;break;
            case 2:case 11:case 18:value.bits&=0xffff;break;
            case 3:case 4:case 10:case 19:case 22:case 23:case 25:value.bits&=0xffffffff;break;
            case 5:case 6:case 7:case 20:case 21:case 64:break;
            default:unsupported();
        }
        return value;
    }
    std::vector<CustomData> custom(std::uint32_t offset){
        std::vector<CustomData> values;std::set<std::uint32_t> seen;
        while(offset!=absent){
            consume();if(offset%12 || !seen.insert(offset).second)bad();const auto row=segments[12].slice(offset,12);
            values.push_back({guid(row.u32(0)),constant(row.u32(4))});offset=row.u32(8);
        }return values;
    }
    Type type(std::uint32_t encoded,unsigned depth=0){
        consume();if(depth>64)bad();
        if(encoded&0x80000000U){
            const auto vt=static_cast<std::uint16_t>(encoded&0xffffU);
            if(vt>31 || (vt>=26 && vt<=29))bad();return {vt,{},0,{}};
        }
        if(encoded%8)bad();
        if(auto found=descriptors.find(encoded);found!=descriptors.end()){consume(found->second.bounds.size());return found->second;}
        if(!reading.insert(encoded).second)bad();
        const auto row=segments[9].slice(encoded,8);Type value;value.vartype=row.u16(0)&0xfffU;
        switch(value.vartype){
            case 26:case 27:value.element=std::make_shared<Type>(type(row.u32(4),depth+1));break;
            case 28:{
                const auto offset=row.u32(4);const auto array=segments[10].slice(offset,8);const auto count=array.u16(4);
                if(!count || count>64)bad();consume(count);
                value.element=std::make_shared<Type>(type(array.u32(0),depth+1));
                const auto bounds=segments[10].slice(std::size_t{offset}+8,std::size_t{count}*8);
                for(unsigned i=0;i<count;++i)value.bounds.push_back({bounds.u32(i*8),std::bit_cast<std::int32_t>(bounds.u32(i*8+4))});
                break;
            }
            case 29:value.reference=row.u32(4);break;
            default:if(value.vartype>31)unsupported();break;
        }
        reading.erase(encoded);descriptors.emplace(encoded,value);return value;
    }
    Function function(Bytes row,std::int32_t id,std::uint32_t name_offset){
        (void)row.slice(0,24);Function value;value.id=id;value.doc.name=name(name_offset);
        value.result=type(row.u32(4));value.flags=row.u16(8);value.vtable_offset=row.u16(12)&0xfffeU;
        const auto attrs=row.u32(16);value.kind=attrs&7U;value.invocation=(attrs>>3)&15U;value.calling_convention=(attrs>>8)&15U;
        const auto count=row.u16(20);value.optional_parameters=std::bit_cast<std::int16_t>(row.u16(22));
        if(value.kind>4 || !value.invocation || value.invocation>8 || (value.invocation&(value.invocation-1)) || value.calling_convention>8)bad();
        const std::size_t parameter_bytes=std::size_t{count}*((attrs&0x1000U)?16U:12U);
        if(parameter_bytes>row.data.size()-24)bad();
        const auto optional=row.data.size()-parameter_bytes;
        if(optional%4)bad();consume(count);
        if(optional>=28)value.doc.context=row.u32(24);
        if(optional>=32)value.doc.help=string(row.u32(28));
        if(optional>=36 && row.u32(32)!=absent){
            if(attrs&0x2000U){if(row.u32(32)>0xffff)bad();value.entry_ordinal=row.u16(32);}
            else value.entry_name=string(row.u32(32));
        }
        if(optional>=48)value.doc.string_context=row.u32(44);
        if(optional>=52 && (attrs&0x80U))value.custom=custom(row.u32(48));
        for(unsigned i=0;i<count;++i){
            const auto p=row.slice(row.data.size()-std::size_t{count-i}*12,12);Parameter parameter;
            parameter.type=type(p.u32(0));parameter.name=name(p.u32(4));parameter.flags=p.u16(8);
            if(parameter.flags&0x20U){if(!(attrs&0x1000U))bad();parameter.default_value=constant(row.u32(optional+std::size_t{i}*4));}
            if((attrs&0x80U) && optional>=56+std::size_t{i}*4)parameter.custom=custom(row.u32(52+std::size_t{i}*4));
            value.parameters.push_back(std::move(parameter));
        }
        return value;
    }
    Variable variable(Bytes row,std::int32_t id,std::uint32_t name_offset){
        (void)row.slice(0,20);Variable value;value.id=id;value.doc.name=name(name_offset);value.type=type(row.u32(4));
        value.flags=row.u16(8);value.kind=row.u16(12);if(value.kind>3)bad();
        if(value.kind==2)value.value=constant(row.u32(16));else value.instance_offset=row.u32(16);
        if(row.data.size()>=24)value.doc.context=row.u32(20);
        if(row.data.size()>=28)value.doc.help=string(row.u32(24));
        if(row.data.size()>=36)value.custom=custom(row.u32(32));
        if(row.data.size()>=40)value.doc.string_context=row.u32(36);
        return value;
    }
    TypeInfo info(std::uint32_t offset){
        const auto row=segments[0].slice(offset,100);TypeInfo value;
        value.reference=offset;value.kind=row.u16(0)&15U;if(value.kind>7)bad();
        value.alignment=(row.u16(0)>>11)&31U;value.guid=guid(row.u32(44));value.flags=row.u16(48);
        value.doc.name=name(row.u32(52));value.major=row.u16(56);value.minor=row.u16(58);
        value.doc.help=string(row.u32(60));value.doc.string_context=row.u32(64);value.doc.context=row.u32(68);
        value.custom=custom(row.u32(72));value.vtable_size=row.u16(78);value.size=row.u32(80);
        if(value.kind==6)value.alias=type(row.u32(84));
        if(value.kind==2)value.dll=string(row.u32(84));
        const auto functions=row.u16(24),variables=row.u16(26),implementations=row.u16(76);
        const std::size_t total=std::size_t{functions}+variables;consume(total+implementations);
        if(total){
            const std::size_t location=row.u32(4);const std::size_t length=file.u32(location);
            const auto records=file.slice(location+4,length);
            const auto table=file.slice(location+4+length,total*12);
            std::vector<std::pair<std::size_t,std::size_t>> ranges;
            for(std::size_t i=0;i<total;++i){
                const auto at=table.u32(8*total+4*i);const auto size=records.u16(at);
                if(size%4 || size<(i<functions?24:20))bad();
                const auto member=records.slice(at,size);ranges.emplace_back(at,std::size_t{at}+size);
                const auto id=std::bit_cast<std::int32_t>(table.u32(4*i));const auto n=table.u32(4*total+4*i);
                if(i<functions){
                    auto f=function(member,id,n);
                    if(n==absent && !value.functions.empty() && (f.invocation&14U) && (value.functions.back().invocation&14U))f.doc.name=value.functions.back().doc.name;
                    value.functions.push_back(std::move(f));
                }else value.variables.push_back(variable(member,id,n));
            }
            std::sort(ranges.begin(),ranges.end());
            for(std::size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)bad();
        }
        if(value.kind==5){
            auto current=row.u32(84);std::set<std::uint32_t> seen;
            for(unsigned i=0;i<implementations;++i){
                if(current%16 || !seen.insert(current).second)bad();const auto item=segments[3].slice(current,16);
                value.implementations.push_back({item.u32(0),item.u32(4),custom(item.u32(8))});current=item.u32(12);
            }
            if(implementations && current!=absent)bad();
        }else if(implementations && row.u32(84)!=absent){
            if(implementations!=1)bad();value.implementations.push_back({row.u32(84),0,{}});
        }
        return value;
    }
public:
    explicit Reader(std::span<const std::uint8_t> data):file{data}{}
    Library run(){
        if(file.data.size()<4)bad();if(file.u32(0)!=0x5446534dU)unsupported();
        (void)file.slice(0,84);if(file.u32(4)!=0x10002U)unsupported();
        const std::size_t count=file.u32(32);consume(count);
        const std::size_t offsets=84+((file.u32(20)&0x100U)?4U:0U);
        const std::size_t directory=offsets+count*4;
        const auto table=file.slice(directory,240);
        std::vector<std::pair<std::size_t,std::size_t>> ranges;
        for(unsigned i=0;i<15;++i){
            const auto start=table.u32(i*16),length=table.u32(i*16+4);
            if(length){if(start<directory+240 || start%4)bad();segments[i]=file.slice(start,length);ranges.emplace_back(start,std::size_t{start}+length);}
        }
        std::sort(ranges.begin(),ranges.end());
        for(std::size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)bad();
        if(segments[5].data.size()%24 || segments[9].data.size()%8 || segments[1].data.size()%12)bad();
        read_text(7,true);read_text(8,false);
        Library result;result.guid=guid(file.u32(8));result.lcid=file.u32(12);result.locale=file.u32(16);
        result.system=file.u16(20)&15U;if(result.system>3)unsupported();
        result.major=file.u16(24);result.minor=file.u16(26);result.flags=file.u16(28)|8U;
        result.doc.name=name(file.u32(56));result.doc.help=string(file.u32(36));result.doc.context=file.u32(44);result.doc.string_context=file.u32(40);
        result.help_file=string(file.u32(60));if(offsets!=84)result.help_dll=string(file.u32(84));
        result.custom=custom(file.u32(64));result.dispatch_reference=file.u32(76);
        for(std::size_t at=0;at<segments[2].data.size();){
            consume();const auto bytes=segments[2];Import item;item.offset=static_cast<std::uint32_t>(at);
            item.guid=guid(bytes.u32(at));item.lcid=bytes.u32(at+4);item.major=bytes.u16(at+8);item.minor=bytes.u16(at+10);
            const auto size=bytes.u16(at+12)>>2;account_text(static_cast<unsigned>(size));item.file=bytes.text(at+14,static_cast<unsigned>(size));
            const auto next=aligned(at+14+static_cast<unsigned>(size));(void)bytes.slice(at,next-at);at=next;result.imports.push_back(std::move(item));
        }
        for(std::size_t at=0;at<segments[1].data.size();at+=12){
            consume();const auto row=segments[1].slice(at,12);ExternalReference ref;ref.reference=static_cast<std::uint32_t>(at)|1U;
            ref.kind=static_cast<std::uint8_t>(row.u32(0)>>24);if(ref.kind>7)bad();
            const auto found=std::find_if(result.imports.begin(),result.imports.end(),[&](const Import& item){return item.offset==row.u32(4);});
            if(found==result.imports.end())bad();ref.import_index=static_cast<std::uint32_t>(found-result.imports.begin());
            if(row.u32(0)&0x10000U)ref.guid=guid(row.u32(8));else ref.type_index=row.u32(8);
            result.external_references.push_back(std::move(ref));
        }
        std::set<std::uint32_t> seen;
        for(std::size_t i=0;i<count;++i){const auto offset=file.u32(offsets+i*4);if(offset%100 || !seen.insert(offset).second)bad();result.types.push_back(info(offset));}
        const auto reference=[&](std::uint32_t ref){
            if(seen.contains(ref))return;
            if(std::none_of(result.external_references.begin(),result.external_references.end(),[&](const auto& item){return item.reference==ref;}))bad();
        };
        const auto validate=[&](const auto& self,const Type& value)->void{
            consume();if(value.vartype==29)reference(value.reference);
            if(value.element)self(self,*value.element);
        };
        for(const auto& value:result.types){
            if(value.alias)validate(validate,*value.alias);
            for(const auto& f:value.functions){validate(validate,f.result);for(const auto& p:f.parameters)validate(validate,p.type);}
            for(const auto& v:value.variables)validate(validate,v.type);
            for(const auto& impl:value.implementations)reference(impl.reference);
        }
        if(result.dispatch_reference!=absent)reference(result.dispatch_reference);
        return result;
    }
};
}
Library read(std::span<const std::uint8_t> bytes){return Reader(bytes).run();}
}
