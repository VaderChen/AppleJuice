#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "automation_value.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace x64arm {
namespace {
using namespace automation;
constexpr unsigned scratch_size=640,source_offset=32,output_offset=56,result_offset=192;
constexpr unsigned maximum_dispatch=16;
enum Phase {Convert,Copy,Dispatch,Query,Commit,Cleanup};
Value read_value(const GuestMemory& memory,std::uint64_t pointer){Value v{};memory.read(GuestAddress{pointer},std::span{v}.first(memory.execution_mode()==ExecutionMode::X86?16:24));return v;}
std::uint32_t indirect(const GuestMemory& memory,Value& v){
    const auto width=memory.execution_mode()==ExecutionMode::X86?4U:8U;std::set<std::uint64_t> visited;
    for(unsigned depth=0;depth<64;++depth){const auto t=static_cast<std::uint16_t>(get(v,0,2));if(!valid_type(t))return badtype;if(!(t&0x4000U))return 0;
        const auto pointer=get(v,8,width);if(!pointer)return invalid;if(!visited.insert(pointer).second)return invalid;
        const auto base=t&0xfffU;if(base==12 && !(t&0x2000U)){v=read_value(memory,pointer);continue;}
        const unsigned size=(t&0x2000U) || base==8 || base==9 || base==13?width:base==14?16:base==16 || base==17?1:base==2 || base==11 || base==18?2:base==3 || base==4 || base==10 || base==19 || base==22 || base==23?4:8;
        v={};memory.read(GuestAddress{pointer},std::span{v}.subspan(base==14?0:8,size));put(v,0,2,t&~0x4000U);return 0;
    }return invalid;
}
std::u16string string_value(const GuestMemory& memory,const Value& v){
    const auto pointer=get(v,8,memory.execution_mode()==ExecutionMode::X86?4:8);if(!pointer)return {};
    if(pointer<4)throw MemoryFault(GuestAddress{pointer},Access::Read,4,"BSTR 前綴不合法");const auto bytes=memory.load(GuestAddress{pointer-4},32);if(bytes>65534 || (bytes&1U))throw std::invalid_argument("字串轉換超過長度或含有非 UTF-16 尾端");
    std::u16string text(static_cast<std::size_t>(bytes/2),0);for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<char16_t>(memory.load(GuestAddress{pointer+2*i},16));return text;
}
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){const auto bits=memory.execution_mode()==ExecutionMode::X86?32U:64U;return memory.load(GuestAddress{memory.load(GuestAddress{object},static_cast<std::uint8_t>(bits))+slot*(bits/8)},static_cast<std::uint8_t>(bits));}
bool boolean_word(std::u16string_view text,std::u16string_view word){
    if(text.size()!=word.size())return false;for(std::size_t i=0;i<text.size();++i){auto a=text[i],b=word[i];if(a>=u'A' && a<=u'Z')a+=32;if(b>=u'A' && b<=u'Z')b+=32;if(a!=b)return false;}return true;
}
std::uint32_t comparison(const GuestMemory& memory,Value left,Value right,std::uint32_t lcid,std::uint32_t flags){
    const bool lh=get(left,0,2)&0x8000U,rh=get(right,0,2)&0x8000U;put(left,0,2,get(left,0,2)&~0x8000U);put(right,0,2,get(right,0,2)&~0x8000U);
    const auto l=get(left,0,2),r=get(right,0,2);if(!valid_type(static_cast<std::uint16_t>(l),false) || !valid_type(static_cast<std::uint16_t>(r),false))return badtype;
    if(flags&~0x4004fU)return invalid;if(l==1 || r==1)return 3;if(l==10 || r==10)return l==10 && r==10?1:mismatch;
    if(l==8 && r==8)return compare_strings(string_value(memory,left),string_value(memory,right),lcid,flags);
    if(l==8 || r==8){
        const bool swap=r==8;auto text=string_value(memory,swap?right:left);auto numeric=swap?left:right;const auto hard_string=swap?rh:lh,hard_numeric=swap?lh:rh;
        std::uint32_t result=2;
        if(get(numeric,0,2)==0)result=text.empty()?1:2;
        else if(!hard_string && !hard_numeric)result=2;
        else if(hard_string && !hard_numeric){const auto formatted=format_number(number_string(numeric),lcid);if(!formatted)return invalid;result=compare_strings(text,*formatted,lcid,flags);}
        else {std::uint32_t status=0;const auto parsed=parse_number(text,lcid,status);if(parsed){Value string_number{};put(string_number,0,2,8);if(hard_string)numeric={};result=number_compare(string_number,numeric,*parsed);}else if(status!=mismatch)return status;}
        return swap && result<=2?2-result:result;
    }
    return number_compare(left,right);
}
}
std::optional<std::uint32_t> WindowsServices::invoke_automation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace automation;const WindowsCall arg(state,memory,service);const auto done=[&](std::uint32_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(!arg(0) || !arg(1))return done(invalid);const auto size=x86_?16U:24U;
    if(service.symbol=="VarCmp"){
        try{return done(comparison(memory,read_value(memory,arg(0)),read_value(memory,arg(1)),static_cast<std::uint32_t>(arg(2)),static_cast<std::uint32_t>(arg(3))));}catch(const std::invalid_argument&){return done(mismatch);}catch(const std::range_error&){return done(overflow);}catch(const std::bad_alloc&){return done(nomemory);}
    }
    memory.check(GuestAddress{arg(0)},size,Access::Read);memory.check(GuestAddress{arg(0)},size,Access::Write);
    const bool copy=service.symbol=="VariantCopyInd",extended=service.symbol=="VariantChangeTypeEx";
    const auto lcid=extended?static_cast<std::uint32_t>(arg(2)):0x400U;
    const auto flags=copy?0U:static_cast<unsigned>(static_cast<std::uint16_t>(arg(extended?3:2)));
    if(flags&~0xffU)return done(invalid);
    Value source;try{source=read_value(memory,arg(1));if(const auto status=indirect(memory,source))return done(status);}catch(const std::bad_alloc&){return done(nomemory);}
    const auto target=copy?static_cast<std::uint16_t>(get(source,0,2)):static_cast<std::uint16_t>(arg(extended?4:3));
    if(!valid_type(target) || (target&0x4000U))return done(badtype);
    if((target&0x2000U) && target!=get(source,0,2))return done(mismatch);
    const auto scratch=allocate_heap(memory,scratch_size);if(!scratch)return done(nomemory);
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,scratch_size>{});memory.store(GuestAddress{scratch+8},64,arg(0));memory.store(GuestAddress{scratch+16},32,lcid);memory.store(GuestAddress{scratch+20},16,flags);memory.store(GuestAddress{scratch+22},16,target);memory.write(GuestAddress{scratch+source_offset},source);
    try{return resume_automation(state,memory,scratch,0);}catch(...){discard_automation(state,memory,scratch);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_automation(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint32_t result){
    using namespace automation;const auto bits=x86_?32U:64U,width=bits/8,size=x86_?16U:24U;
    const auto load=[&](unsigned offset,unsigned n=32){return memory.load(GuestAddress{scratch+offset},static_cast<std::uint8_t>(n));};
    const auto store=[&](unsigned offset,std::uint64_t value,unsigned n=32){memory.store(GuestAddress{scratch+offset},static_cast<std::uint8_t>(n),value);};
    const auto callback=[&](GuestCallback call,Phase phase){store(0,phase);start_callbacks(state,memory,{std::move(call)},Continuation::AutomationOperation,scratch);return std::optional<std::uint32_t>{};};
    const auto builtin=[&](std::string_view name,std::initializer_list<std::uint64_t> args,Phase phase){return callback({resolve("oleaut32.dll",name).value,args},phase);};
    const auto cleanup=[&](std::uint32_t status){store(4,status);store(24,load(28)+1);store(0,Cleanup);};
    const auto commit=[&](){return builtin("VariantClear",{load(8,64)},Commit);};
    const auto phase=static_cast<Phase>(load(0));
    if(phase==Copy){if(result)cleanup(result);else return commit();}
    else if(phase==Query){
        const auto pointer=load(144,bits);Value out{};put(out,0,2,load(22,16));put(out,8,width,pointer);memory.write(GuestAddress{scratch+output_offset},out);
        if(result || !pointer)cleanup(result?result:0x80004002U);else return commit();
    }
    else if(phase==Dispatch){
        if(result)cleanup(result);
        else {auto source=read_value(memory,scratch+result_offset+24*(load(28)-1));const auto status=indirect(memory,source);if(status)cleanup(status);else {memory.write(GuestAddress{scratch+source_offset},source);store(0,Convert);}}
    }
    else if(phase==Commit){
        if(!result){const auto out=read_value(memory,scratch+output_offset);memory.write(GuestAddress{load(8,64)},std::span{out}.first(size));store(output_offset,0,16);}cleanup(result);
    }
    else if(phase==Cleanup && result && !load(4))store(4,result);
    if(load(0)==Convert){
        auto source=read_value(memory,scratch+source_offset);const auto from=static_cast<std::uint16_t>(get(source,0,2)),target=static_cast<std::uint16_t>(load(22,16)),flags=static_cast<std::uint16_t>(load(20,16));const auto lcid=static_cast<std::uint32_t>(load(16));
        if(from==target)return builtin("VariantCopy",{scratch+output_offset,scratch+source_offset},Copy);
        Value out{};put(out,0,2,target);std::uint32_t status=0;
        if((from&0x2000U) || (target&0x2000U))status=mismatch;
        else if(target<=1){if(from==10 || (target==0 && from==1))status=mismatch;}
        else if((from==9 || from==13) && (target==9 || target==13)){
            const auto object=get(source,8,width);if(!object)status=mismatch;
            else {std::array<std::uint8_t,16> iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};if(target==9){iid[0]=0; iid[1]=4;iid[2]=2;}memory.write(GuestAddress{scratch+104},iid);store(144,0,bits);return callback({method(memory,object,0),{object,scratch+104,scratch+144}},Query);}
        }
        else if(from==9){
            const auto object=get(source,8,width),depth=load(28);if((flags&1) || !object || depth>=maximum_dispatch)status=mismatch;
            else {store(28,depth+1);memory.write(GuestAddress{scratch+104},std::array<std::uint8_t,40>{});return callback({method(memory,object,6),{object,0,scratch+104,lcid,2,scratch+120,scratch+result_offset+24*depth,0,0}},Dispatch);}
        }
        else try{
            std::string decimal;
            if(target==8){
                std::optional<std::u16string> text;
                if(from==0)text=std::u16string{};
                else if(from==11 && (flags&0x12U))text=bool_string(get(source,8,2)!=0,lcid,flags&0x10U);
                else if(from==7)text=format_date(std::bit_cast<double>(get(source,8,8)),lcid,flags);
                else text=format_number(number_string(source),lcid);
                if(!text)status=invalid;
                else {auto bytes=utf16_bytes(*text);bytes.resize(bytes.size()-2);const auto base=allocate_heap(memory,bytes.size()+10);if(!base)status=nomemory;else {memory.store(GuestAddress{base},32,0x52545342U);memory.store(GuestAddress{base+4},32,bytes.size());memory.write(GuestAddress{base+8},bytes);memory.store(GuestAddress{base+8+bytes.size()},16,0);put(out,8,width,base+8);}}
            }
            else if(from==8){
                auto text=string_value(memory,source);
                if(target==7){const auto date=parse_date(text,lcid,flags);if(!date)status=mismatch;else put(out,8,8,std::bit_cast<std::uint64_t>(*date));}
                else if(target==11 && (boolean_word(text,u"True") || boolean_word(text,bool_string(true,lcid,flags&0x10U))))put(out,8,2,0xffff);
                else if(target==11 && (boolean_word(text,u"False") || boolean_word(text,bool_string(false,lcid,flags&0x10U))))put(out,8,2,0);
                else {const auto parsed=parse_number(std::move(text),lcid,status);if(parsed)status=number_convert(source,*parsed,target,out);}
            }
            else status=number_convert(source,{},target,out);
        }catch(const std::invalid_argument&){status=mismatch;}catch(const std::range_error&){status=overflow;}catch(const std::bad_alloc&){status=nomemory;}
        if(status)cleanup(status);else {memory.write(GuestAddress{scratch+output_offset},out);return commit();}
    }
    // 所有臨時 VARIANT 都經既有的 Clear／SafeArray／COM callback 路徑釋放。
    while(load(24)){
        const auto index=load(24)-1;store(24,index);const auto pointer=scratch+(index?result_offset+24*(index-1):output_offset);
        if(memory.load(GuestAddress{pointer},16))return builtin("VariantClear",{pointer},Cleanup);
    }
    const auto status=static_cast<std::uint32_t>(load(4));(void)free_heap(memory,scratch,state);complete_call(state,memory,status);return std::nullopt;
}
void WindowsServices::discard_automation(GuestState& state,GuestMemory& memory,std::uint64_t scratch){
    // SEH 拋離回呼時不能執行新的 guest COM 方法；先回收自有字串與工作區。
    const auto count=memory.load(GuestAddress{scratch+28},32);
    for(std::uint64_t i=0;i<=count;++i){const auto pointer=scratch+(i?result_offset+24*(i-1):output_offset);if(memory.load(GuestAddress{pointer},16)!=8)continue;const auto bstr=memory.load(GuestAddress{pointer+8},x86_?32:64);if(bstr>=8 && allocations_.contains(bstr-8))(void)free_heap(memory,bstr-8,state);}
    (void)free_heap(memory,scratch,state);
}
}
