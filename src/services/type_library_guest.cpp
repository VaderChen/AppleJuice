#include "type_library_guest.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm::typelib {
namespace {
struct Writer {
    Description result;
    unsigned unit;
    Writer(unsigned width,std::size_t size):unit(width){if(unit!=4 && unit!=8)throw std::invalid_argument("無效客體指標寬度");result.bytes.resize(size);}
    std::size_t allocate(std::size_t count,unsigned alignment=8){
        const auto at=(result.bytes.size()+alignment-1)&~std::size_t{alignment-1};
        if(count>16U*1024U*1024U || at>16U*1024U*1024U-count)throw std::bad_alloc();
        result.bytes.resize(at+count);return at;
    }
    void put(std::size_t at,std::uint64_t value,unsigned size){
        if(at>result.bytes.size() || size>result.bytes.size()-at)throw std::logic_error("描述欄位超出區塊");
        for(unsigned i=0;i<size;++i)result.bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
    }
    void pointer(std::size_t at,std::size_t target){put(at,target,unit);result.pointers.push_back(at);}
    void type(std::size_t at,const Type& value){
        put(at+unit,value.vartype,2);
        if(value.vartype==26 || value.vartype==27){
            if(!value.element)throw Error(invalid_data,"缺少指標／陣列元素型別");
            const auto target=allocate(unit*2);pointer(at,target);type(target,*value.element);
        }else if(value.vartype==28){
            if(!value.element || value.bounds.empty())throw Error(invalid_data,"缺少固定陣列元素型別");
            const auto target=allocate(unit*2+4+value.bounds.size()*8);pointer(at,target);type(target,*value.element);
            put(target+unit*2,value.bounds.size(),2);
            for(std::size_t i=0;i<value.bounds.size();++i){put(target+unit*2+4+i*8,value.bounds[i].count,4);put(target+unit*2+8+i*8,static_cast<std::uint32_t>(value.bounds[i].lower),4);}
        }else if(value.vartype==29)put(at,value.reference,4);
    }
    void constant(std::size_t at,const Constant& value){
        put(at,value.vartype,2);
        if(value.vartype==8){if(value.text)result.strings.emplace_back(at+8,utf8_to_utf16_lossy(*value.text));}
        else put(at+8,value.bits,8);
    }
};
}
Description library_description(const Library& lib){
    Writer w(4,32);std::copy(lib.guid.begin(),lib.guid.end(),w.result.bytes.begin());
    w.put(16,lib.lcid,4);w.put(20,lib.system,4);w.put(24,lib.major,2);w.put(26,lib.minor,2);w.put(28,lib.flags,2);return std::move(w.result);
}
Description type_description(const Library& lib,std::size_t index,unsigned unit){
    const auto& t=lib.types.at(index);Writer w(unit,unit==4?76:96);
    std::copy(t.guid.begin(),t.guid.end(),w.result.bytes.begin());w.put(16,lib.locale,4);w.put(24,UINT32_MAX,4);w.put(28,UINT32_MAX,4);
    const unsigned instance=unit==4?36:40;w.put(instance,t.size,4);w.put(instance+4,t.kind,4);
    const std::array<std::uint64_t,8> words{t.functions.size(),t.variables.size(),t.implementations.size(),t.vtable_size,t.alignment,t.flags,t.major,t.minor};
    for(unsigned i=0;i<words.size();++i)w.put(instance+8+i*2,words[i],2);
    if(t.alias)w.type(instance+24,*t.alias);return std::move(w.result);
}
Description function_description(const Function& f,unsigned unit){
    Writer w(unit,unit==4?52:88);w.put(0,static_cast<std::uint32_t>(f.id),4);
    const unsigned params=unit==4?8:16,kind=unit==4?12:24,count=unit==4?24:36,element=unit==4?32:48,flags=unit==4?48:80;
    w.put(kind,f.kind,4);w.put(kind+4,f.invocation,4);w.put(kind+8,f.calling_convention,4);
    w.put(count,f.parameters.size(),2);w.put(count+2,static_cast<std::uint16_t>(f.optional_parameters),2);w.put(count+4,f.kind==4?0:f.vtable_offset,2);
    w.type(element,f.result);w.put(flags,f.flags,2);
    if(!f.parameters.empty()){
        const auto base=w.allocate(f.parameters.size()*unit*4);w.pointer(params,base);
        for(std::size_t i=0;i<f.parameters.size();++i){
            const auto at=base+i*unit*4;const auto& p=f.parameters[i];w.type(at,p.type);w.put(at+unit*3,p.flags,2);
            if(p.default_value){const auto ext=w.allocate(unit==4?24:32);w.pointer(at+unit*2,ext);w.put(ext,unit==4?24:32,4);w.constant(ext+8,*p.default_value);}
        }
    }
    return std::move(w.result);
}
Description variable_description(const Variable& v,unsigned unit){
    Writer w(unit,unit==4?36:64);w.put(0,static_cast<std::uint32_t>(v.id),4);
    const unsigned data=unit==4?8:16,element=unit==4?12:24,flags=unit==4?28:56,kind=unit==4?32:60;
    w.type(element,v.type);w.put(flags,v.flags,2);w.put(kind,v.kind,4);
    if(v.value){const auto at=w.allocate(unit==4?16:24);w.pointer(data,at);w.constant(at,*v.value);}else w.put(data,v.instance_offset,4);
    return std::move(w.result);
}
}
