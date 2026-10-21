#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "type_library.hpp"
#include "automation_value.hpp"
#include <algorithm>
#include <array>
#include <functional>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,unsupported=0x80004001U,nomemory=0x8007000eU,
    missing_member=0x80020003U,missing_parameter=0x80020004U,mismatch=0x80020005U,bad_count=0x8002000eU;
constexpr unsigned header=64,stride=96,pending_fastcall=3;
enum Phase : std::uint32_t { Prepare,Converted,Resolved,Invoked,Cleanup,Dispatched,Queried,Writeback,WritebackConverted,WritebackReady,WritebackReleased };
[[noreturn]] void fail(std::uint32_t status){throw typelib::Error(status,"型別庫方法呼叫未成功");}
struct NativeType {std::uint16_t variant{};bool indirect{};std::optional<typelib::Guid> interface_id{};bool interface_value{};};
const typelib::TypeInfo* reference(const typelib::Library& library,std::uint32_t value){
    for(const auto& type:library.types)if(type.reference==value)return &type;return nullptr;
}
struct ReferencedType {const typelib::Library* library{};const typelib::TypeInfo* info{};};
using ResolveReference=std::function<ReferencedType(const typelib::Library&,std::uint32_t)>;
struct ReferenceLeases {
    std::vector<std::uint64_t> owners;
    std::function<void(std::uint64_t)> release;
    ~ReferenceLeases(){for(auto i=owners.rbegin();i!=owners.rend();++i)release(*i);}
};
NativeType native_type(const typelib::Library& library,const typelib::Type& type,const ResolveReference& resolve,unsigned depth=0){
    if(depth>64)fail(typelib::invalid_data);
    if(type.vartype==26 || type.vartype==27){
        if(!type.element)fail(typelib::invalid_data);auto inner=native_type(library,*type.element,resolve,depth+1);if(inner.indirect)fail(unsupported);
        if(type.vartype==26){if(inner.interface_value)inner.interface_value=false;else inner.indirect=true;}
        else{if(inner.interface_id || inner.variant==0 || inner.variant>=24 || (inner.variant&0x2000U))fail(unsupported);inner.variant|=0x2000U;}
        return inner;
    }
    if(type.vartype==29){
        const auto* info=reference(library,type.reference);const auto* source=&library;
        if(!info){
            const auto external=std::find_if(library.external_references.begin(),library.external_references.end(),[&](const auto& value){return value.reference==type.reference;});
            if(external!=library.external_references.end() && external->guid && (external->kind==3 || external->kind==4))return {13,false,*external->guid,true};
            const auto target=resolve(library,type.reference);info=target.info;source=target.library;
        }
        if(info->kind==3 || info->kind==4)return {13,false,info->guid,true};
        if(info->kind==0)return {3,false};if(info->kind==6 && info->alias)return native_type(*source,*info->alias,resolve,depth+1);fail(unsupported);
    }
    switch(type.vartype){
        case 0:case 2:case 3:case 4:case 5:case 6:case 7:case 8:case 9:case 10:case 11:case 12:case 13:case 14:
        case 16:case 17:case 18:case 19:case 20:case 21:case 24:case 25:return {type.vartype,false};
        case 22:return {3,false};case 23:return {19,false};default:fail(unsupported);
    }
}
unsigned native_size(std::uint16_t type,unsigned unit){
    if((type&0x2000U) || type==8 || type==9 || type==13)return unit;
    if(type==12)return unit==4?16:24;if(type==14)return 16;
    if(type==16 || type==17)return 1;if(type==2 || type==11 || type==18)return 2;
    return type==5 || type==6 || type==7 || type==20 || type==21?8:4;
}
struct Method {const typelib::Library* library{};const typelib::TypeInfo* type{};const typelib::Function* function{};const typelib::Variable* variable{};};
Method find_method(const typelib::Library& lib,const typelib::TypeInfo& type,std::int32_t id,unsigned flags,const ResolveReference& resolve,unsigned depth=0){
    if(depth>64)fail(typelib::invalid_data);
    for(const auto& f:type.functions)if(f.id==id && (f.invocation&flags))return {&lib,&type,&f};
    for(const auto& v:type.variables)if(v.id==id && v.kind==3 && (flags&14U)){
        if((v.flags&1U) && (flags&12U))fail(missing_member);return {&lib,&type,nullptr,&v};
    }
    if(type.kind==3 || type.kind==4 || type.kind==5)for(const auto& impl:type.implementations){
        if(type.kind==5 && (!(impl.flags&1) || (impl.flags&2)))continue;
        auto base=ReferencedType{&lib,reference(lib,impl.reference)};if(!base.info)base=resolve(lib,impl.reference);
        const auto found=find_method(*base.library,*base.info,id,flags,resolve,depth+1);if(found.type)return found;
    }
    return {};
}
}
std::optional<std::uint32_t> WindowsServices::invoke_type_library_member(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto& bridge=runtime_.type_libraries;auto& view=bridge.views.at(arg(0));
    const auto& described=view.data->types.at(static_cast<std::size_t>(view.index));
    const auto& library=(described.flags&0x40U)?*bridge.views.at(view.library).data:*view.data;const unsigned unit=x86_?4:8,variant_size=x86_?16:24;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto flags=static_cast<std::uint16_t>(arg(3));if(!flags || (flags&~15U) || !arg(4))fail(invalid);
    const auto error=[&](std::uint32_t status,std::uint32_t index=UINT32_MAX){if(arg(7) && index!=UINT32_MAX)memory.store(GuestAddress{arg(7)},32,index);fail(status);};
    if(arg(7))memory.check(GuestAddress{arg(7)},4,Access::Write);
    const auto result=(flags&12U)?0:arg(5);if(result)memory.check(GuestAddress{result},variant_size,Access::Write);
    if(arg(6))memory.check(GuestAddress{arg(6)},x86_?32:64,Access::Write);
    ReferenceLeases dependencies{{},[&](std::uint64_t owner){(void)release_type_library(state,memory,owner);}};
    const ResolveReference resolve_reference=[&](const typelib::Library& data,std::uint32_t ref){
        auto owner=arg(0);
        if(&data!=&library){
            const auto found=std::find_if(dependencies.owners.begin(),dependencies.owners.end(),[&](auto value){return bridge.views.at(value).data.get()==&data || bridge.views.at(bridge.views.at(value).library).data.get()==&data;});
            if(found==dependencies.owners.end())fail(typelib::invalid_data);owner=*found;
        }
        const auto target=reference_type_library(state,memory,owner,ref);
        try{dependencies.owners.push_back(target);}catch(...){(void)release_type_library(state,memory,target);throw;}
        const auto& target_view=bridge.views.at(target);const auto index=static_cast<std::size_t>(target_view.index);const auto& native_data=(target_view.data->types.at(index).flags&0x40U)?bridge.views.at(target_view.library).data:target_view.data;return ReferencedType{native_data.get(),&native_data->types.at(index)};
    };
    const auto method=find_method(library,library.types.at(static_cast<std::size_t>(view.index)),static_cast<std::int32_t>(arg(2)),flags,resolve_reference);
    if(!method.type)fail(missing_member);const auto& method_library=*method.library;
    memory.check(GuestAddress{arg(4)},2*unit+8,Access::Read);
    const auto arguments=memory.load(GuestAddress{arg(4)},bits),names=memory.load(GuestAddress{arg(4)+unit},bits);
    const auto count=memory.load(GuestAddress{arg(4)+2*unit},32),named=memory.load(GuestAddress{arg(4)+2*unit+4},32);
    if(named>count)fail(invalid);if(count>4096)fail(bad_count);
    if(count)memory.check(GuestAddress{arguments},count*variant_size,Access::Read);if(named)memory.check(GuestAddress{names},named*4,Access::Read);
    TypeLibraryInvocation operation;operation.owner=arg(0);operation.result=result;operation.exception=arg(6);operation.argument_error=arg(7);operation.lcid=method_library.lcid;
    if(method.variable || method.function->kind==4){
        if(!arg(1))fail(invalid);operation.phase=Dispatched;
        operation.call.target=memory.load(GuestAddress{memory.load(GuestAddress{arg(1)},bits)+6*unit},bits);
    }else{
        const auto& function=*method.function;
        if(function.kind>3 || function.parameters.size()>15 || (function.calling_convention!=0 && function.calling_convention!=1 && function.calling_convention!=4))fail(unsupported);
        const auto returned=native_type(method_library,function.result,resolve_reference);if(returned.indirect || returned.interface_value)fail(unsupported);operation.return_type=returned.variant;
        if(x86_ && function.calling_convention==0)operation.call.x86_register_arguments=pending_fastcall;
        operation.call.floating_result=returned.variant==4?32:returned.variant==5 || returned.variant==7?64:0;
        operation.call.wide_result=returned.variant==6 || returned.variant==20 || returned.variant==21;
        if(function.kind!=3){
            if(!arg(1))fail(invalid);operation.call.arguments.push_back(arg(1));
            if(function.kind<2){if(function.vtable_offset%unit)fail(typelib::invalid_data);operation.call.target=memory.load(GuestAddress{memory.load(GuestAddress{arg(1)},bits)+function.vtable_offset},bits);}
        }
        std::vector<unsigned> visible;
        for(std::size_t i=0;i<function.parameters.size();++i){const auto& param=function.parameters[i];const auto type=native_type(method_library,param.type,resolve_reference);
            if(type.interface_value || !type.variant || type.variant==24 || type.variant==25)fail(unsupported);
            operation.parameters.push_back({type.variant,param.flags,type.indirect,0,UINT32_MAX,type.interface_id});
            if(param.flags&8){if(!type.indirect || operation.retval_type)fail(typelib::invalid_data);operation.retval_type=type.variant;}
            else if(param.flags&4){if(type.indirect || (type.variant!=3 && type.variant!=19))fail(unsupported);}
            else visible.push_back(static_cast<unsigned>(i));
        }
        if(count>visible.size())fail(bad_count);
        bool property_value=false;
        for(std::uint32_t i=0;i<named;++i){const auto id=static_cast<std::int32_t>(memory.load(GuestAddress{names+i*4},32));
            if(id==-3 && (flags&12U))property_value=true;
            const auto parameter=id==-3 && (flags&12U) && !visible.empty()?visible.back():
                described.kind==4 && (described.flags&0x40U)?
                    (id>=0 && static_cast<std::size_t>(id)<visible.size()?visible[static_cast<std::size_t>(id)]:UINT32_MAX):static_cast<unsigned>(id);
            if(std::find(visible.begin(),visible.end(),parameter)==visible.end() || operation.parameters[parameter].source)error(missing_parameter,i);
            operation.parameters[parameter].source=arguments+i*variant_size;operation.parameters[parameter].argument=i;
        }
        if((flags&12U) && !property_value)fail(missing_parameter);
        auto remaining=count;for(const auto index:visible)if(!operation.parameters[index].source && remaining>named){
            --remaining;operation.parameters[index].source=arguments+remaining*variant_size;operation.parameters[index].argument=static_cast<std::uint32_t>(remaining);
        }
        for(const auto index:visible){auto& param=operation.parameters[index];
            if(param.source && memory.load(GuestAddress{param.source},16)==10 && memory.load(GuestAddress{param.source+8},32)==missing_parameter)param.source=0;
            if(!param.source && !(param.flags&0x30U))error(bad_count,param.argument);
            if(!param.source && !function.parameters[index].default_value && param.type!=12)error(0x8002000fU,param.argument);
        }
        operation.phase=function.kind>=2?Resolved:Prepare;
    }
    if(bridge.invocations.size()>=128 || view.references==UINT32_MAX)fail(nomemory);
    const auto scratch=allocate_heap(memory,header+operation.parameters.size()*stride);if(!scratch)fail(nomemory);
    try{memory.write(GuestAddress{scratch},std::vector<std::uint8_t>(header+operation.parameters.size()*stride));bridge.invocations.emplace(scratch,std::move(operation));}
    catch(...){(void)free_heap(memory,scratch,state);throw;}++view.references;
    try{
        auto& op=bridge.invocations.at(scratch);
        if(op.return_type==12 || op.return_type==14){if(op.retval_type)fail(typelib::invalid_data);op.call.arguments.push_back(scratch+32);}
        for(std::size_t i=0;i<op.parameters.size();++i){auto& param=op.parameters[i];if(param.source || (param.flags&12U))continue;
            const auto at=scratch+header+i*stride;param.source=at;const auto& value=method.function->parameters[i].default_value;
            memory.store(GuestAddress{at},16,value?value->vartype:10);memory.store(GuestAddress{at+8},64,value?value->bits:missing_parameter);
            if(value && value->text){auto bytes=utf16_bytes(utf8_to_utf16_lossy(*value->text));bytes.resize(bytes.size()-2);const auto bstr=allocate_bstr(memory,bytes.size(),bytes);if(!bstr)fail(nomemory);memory.store(GuestAddress{at+8},bits,bstr);}
        }
        if(op.phase==Dispatched){op.call.arguments={arg(1),arg(2),scratch+16,op.lcid,flags,arg(4),result,arg(6),arg(7)};start_callbacks(state,memory,{op.call},Continuation::TypeLibraryInvoke,scratch);return std::nullopt;}
        if(op.phase==Resolved){start_callbacks(state,memory,{{resolve("internal-type-info","AddressOfMember").value,{arg(0),arg(2),flags,scratch}}},Continuation::TypeLibraryInvoke,scratch);return std::nullopt;}
        return resume_type_library_invoke(state,memory,scratch,0);
    }catch(...){discard_type_library_invoke(state,memory,scratch);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_type_library_invoke(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint64_t returned){
    auto& bridge=runtime_.type_libraries;auto& op=bridge.invocations.at(scratch);const unsigned unit=x86_?4:8,variant_size=x86_?16:24;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto cleanup=[&](std::uint32_t status){op.status=status;op.phase=Cleanup;op.cleanup=static_cast<std::uint32_t>(op.parameters.size()*3+1);};
    const auto completed=[&](std::uint32_t status){op.status=status;op.index=0;op.phase=Writeback;};
    const auto builtin=[&](std::string_view name,std::vector<std::uint64_t> args){start_callbacks(state,memory,{{resolve("oleaut32.dll",name).value,std::move(args)}},Continuation::TypeLibraryInvoke,scratch);return std::optional<std::uint32_t>{};};
    const auto append=[&](std::uint16_t type,std::uint64_t value,bool pointer){
        const auto index=op.call.arguments.size();if(index>=16)fail(unsupported);
        op.call.arguments.push_back(value);if(!pointer){
            if(type==4 || type==5 || type==7)op.call.floating_arguments|=1U<<index;
            if(native_size(type,unit)==8)op.call.wide_arguments|=1U<<index;
        }
    };
    try{
        if(op.phase==Resolved){if(returned&0x80000000U)cleanup(static_cast<std::uint32_t>(returned));else{op.call.target=memory.load(GuestAddress{scratch},bits);op.phase=Prepare;}}
        else if(op.phase==Converted){
            if(returned){if(op.argument_error && op.parameters[op.index].argument!=UINT32_MAX)memory.store(GuestAddress{op.argument_error},32,op.parameters[op.index].argument);cleanup(static_cast<std::uint32_t>(returned));}
            else{const auto type=op.parameters[op.index].type;const auto at=scratch+header+op.index*stride+32;
                if(op.parameters[op.index].interface_id){
                    const auto vt=memory.load(GuestAddress{at},16);
                    if(vt!=9 && vt!=13){
                        if(op.argument_error && op.parameters[op.index].argument!=UINT32_MAX)memory.store(GuestAddress{op.argument_error},32,op.parameters[op.index].argument);
                        fail(mismatch);
                    }
                    const auto object=memory.load(GuestAddress{at+8},bits);
                    memory.store(GuestAddress{at+32},16,13);
                    if(object){
                        memory.write(GuestAddress{scratch+16},*op.parameters[op.index].interface_id);
                        op.phase=Queried;
                        start_callbacks(state,memory,{{memory.load(GuestAddress{memory.load(GuestAddress{object},bits)},bits),{object,scratch+16,at+40}}},Continuation::TypeLibraryInvoke,scratch);return std::nullopt;
                    }
                    append(type,op.parameters[op.index].indirect?at+40:0,true);
                }
                else if(type==14){
                    std::array<std::uint8_t,16> decimal{};memory.read(GuestAddress{at},decimal);decimal[0]=decimal[1]=0;
                    memory.write(GuestAddress{at+32},decimal);
                    if(x86_)for(unsigned offset=0;offset<16;offset+=4)append(19,memory.load(GuestAddress{at+32+offset},32),false);
                    else append(type,at+32,true);
                }
                else if(type==12 && x86_)for(unsigned offset=0;offset<16;offset+=4)append(19,memory.load(GuestAddress{at+offset},32),false);
                else if(type==12)append(type,at,true);
                else append(type,memory.load(GuestAddress{at+8},static_cast<std::uint8_t>(native_size(type,unit)*8)),false);
                ++op.index;op.phase=Prepare;
            }
        }else if(op.phase==Queried){
            const auto object=memory.load(GuestAddress{scratch+header+op.index*stride+72},bits);
            if((returned&0x80000000U) || !object){
                if(op.argument_error && op.parameters[op.index].argument!=UINT32_MAX)memory.store(GuestAddress{op.argument_error},32,op.parameters[op.index].argument);
                cleanup(static_cast<std::uint32_t>(returned)==nomemory?nomemory:mismatch);
            }else{append(13,op.parameters[op.index].indirect?scratch+header+op.index*stride+72:object,true);++op.index;op.phase=Prepare;}
        }else if(op.phase==Dispatched)cleanup(static_cast<std::uint32_t>(returned));
        else if(op.phase==Invoked){
            if(op.retval_type==14 || op.return_type==14)memory.store(GuestAddress{scratch+32},16,14);
            if(op.return_type==25 && (returned&0x80000000U)){
                if(op.exception){memory.write(GuestAddress{op.exception},std::vector<std::uint8_t>(x86_?32:64));memory.store(GuestAddress{op.exception+(x86_?28:56)},32,returned);}
                completed(0x80020009U);
            }else{
                if(!op.retval_type && op.return_type!=25 && op.return_type!=24 && op.return_type!=12 && op.return_type!=14 && op.return_type){memory.store(GuestAddress{scratch+32},16,op.return_type);memory.store(GuestAddress{scratch+40},64,returned);}
                completed(0);
            }
        }
        if(op.phase==WritebackConverted){
            if(returned){
                const auto& param=op.parameters[op.index];if(op.argument_error && param.argument!=UINT32_MAX)memory.store(GuestAddress{op.argument_error},32,param.argument);
                cleanup(static_cast<std::uint32_t>(returned)==0x80004002U?mismatch:static_cast<std::uint32_t>(returned));
            }else op.phase=WritebackReady;
        }else if(op.phase==WritebackReleased)op.phase=Writeback;
        while(op.phase==Writeback || op.phase==WritebackReady){
            if(op.index==op.parameters.size()){
                if(!op.status && op.result){std::array<std::uint8_t,24> value{};memory.read(GuestAddress{scratch+32},std::span{value}.first(variant_size));memory.write(GuestAddress{op.result},std::span{value}.first(variant_size));memory.store(GuestAddress{scratch+32},16,0);}
                cleanup(op.status);break;
            }
            const auto& param=op.parameters[op.index];if(!param.writeback){++op.index;continue;}
            const auto at=scratch+header+op.index*stride;
            if(op.phase==Writeback){
                if(memory.load(GuestAddress{at+72},bits)){op.phase=WritebackConverted;return builtin("VariantChangeTypeEx",{at,at+64,op.lcid,0,param.writeback_type});}
                memory.store(GuestAddress{at},16,param.writeback_type);memory.store(GuestAddress{at+8},bits,0);
            }
            const auto previous=memory.load(GuestAddress{param.writeback},bits),value=memory.load(GuestAddress{at+8},bits);
            memory.store(GuestAddress{param.writeback},bits,value);memory.store(GuestAddress{at},16,0);++op.index;op.phase=Writeback;
            if(previous){
                op.phase=WritebackReleased;
                start_callbacks(state,memory,{{memory.load(GuestAddress{memory.load(GuestAddress{previous},bits)+2*unit},bits),{previous}}},Continuation::TypeLibraryInvoke,scratch);return std::nullopt;
            }
        }
        while(op.phase==Prepare && op.index<op.parameters.size()){
            auto& param=op.parameters[op.index];
            if(param.flags&8){const bool whole=param.type==12 || param.type==14;memory.store(GuestAddress{scratch+32},16,whole?0:param.type);append(param.type,scratch+32+(whole?0:8),true);}
            else if(param.flags&4)append(param.type,op.lcid,false);
            else if(param.indirect){
                const auto vt=memory.load(GuestAddress{param.source},16),address=memory.load(GuestAddress{param.source+8},bits);
                if(param.interface_id && (param.flags&1)){
                    if((vt!=0x400d && vt!=0x4009) || !address){if(op.argument_error && param.argument!=UINT32_MAX)memory.store(GuestAddress{op.argument_error},32,param.argument);cleanup(mismatch);break;}
                    memory.check(GuestAddress{address},unit,Access::Read);if(param.flags&2)memory.check(GuestAddress{address},unit,Access::Write);
                    param.writeback=(param.flags&2)?address:0;param.writeback_type=static_cast<std::uint16_t>(vt&0xfffU);op.phase=Converted;
                    return builtin("VariantCopyInd",{scratch+header+op.index*stride+32,param.source});
                }
                if(vt!=(param.type|0x4000U) || !address){if(op.argument_error && param.argument!=UINT32_MAX)memory.store(GuestAddress{op.argument_error},32,param.argument);cleanup(mismatch);break;}
                const auto size=native_size(param.type,unit);if(param.flags&2)memory.check(GuestAddress{address},size,Access::Write);if((param.flags&1) || !(param.flags&2))memory.check(GuestAddress{address},size,Access::Read);append(param.type,address,true);
            }else{
                if(param.flags&2)fail(unsupported);op.phase=Converted;const auto destination=scratch+header+op.index*stride+32;
                if(param.type==12 || param.interface_id)return builtin("VariantCopyInd",{destination,param.source});
                return builtin("VariantChangeTypeEx",{destination,param.source,op.lcid,0,param.type});
            }
            ++op.index;
        }
        if(op.phase==Prepare){
            if(op.call.x86_register_arguments==pending_fastcall){
                // CC_FASTCALL：挑選最左邊兩個 DWORD 純量／指標；浮點、64 位元與值結構留在堆疊。
                // 建構期間的 3 是尚未配置標記，不能傳入 enter_callback。
                unsigned slots{};for(const auto& p:op.parameters)slots+=!(p.flags&12U) && !p.indirect && (p.type==12 || p.type==14)?4:1;
                const auto prefix=op.call.arguments.size()-slots;std::uint32_t eligible{};
                const bool aggregate=op.return_type==12 || op.return_type==14;
                for(unsigned i=0;i<prefix;++i)if(!aggregate || prefix==2)eligible|=1U<<i;
                unsigned at=static_cast<unsigned>(prefix);
                for(const auto& p:op.parameters){
                    const bool expanded=!(p.flags&12U) && !p.indirect && (p.type==12 || p.type==14);
                    if(!expanded && !((op.call.wide_arguments|op.call.floating_arguments)&(1U<<at)))eligible|=1U<<at;
                    at+=expanded?4:1;
                }
                std::vector<unsigned> order;for(unsigned i=0;i<op.call.arguments.size() && order.size()<2;++i)if(eligible&(1U<<i))order.push_back(i);
                op.call.x86_register_arguments=static_cast<unsigned>(order.size());
                for(unsigned i=0;i<op.call.arguments.size();++i)if(std::find(order.begin(),order.end(),i)==order.end())order.push_back(i);
                auto args=op.call.arguments;const auto wide=op.call.wide_arguments,floating=op.call.floating_arguments;op.call.wide_arguments=op.call.floating_arguments=0;
                for(unsigned i=0;i<order.size();++i){op.call.arguments[i]=args[order[i]];if(wide&(1U<<order[i]))op.call.wide_arguments|=1U<<i;if(floating&(1U<<order[i]))op.call.floating_arguments|=1U<<i;}
            }
            op.phase=Invoked;start_callbacks(state,memory,{op.call},Continuation::TypeLibraryInvoke,scratch);return std::nullopt;
        }
    }catch(const typelib::Error& error){cleanup(error.code);}
    catch(const MemoryFault&){cleanup(0x80004003U);}
    catch(const std::bad_alloc&){cleanup(nomemory);}
    while(op.cleanup){
        const auto index=--op.cleanup;const auto at=index?scratch+header+((index-1)/3)*stride+((index-1)%3)*32:scratch+32;
        if(memory.load(GuestAddress{at},16))return builtin("VariantClear",{at});
    }
    const auto status=op.status;discard_type_library_invoke(state,memory,scratch);return finish_type_library_call(state,memory,status);
}
void WindowsServices::discard_type_library_invoke(GuestState& state,GuestMemory& memory,std::uint64_t scratch){
    auto& bridge=runtime_.type_libraries;const auto found=bridge.invocations.find(scratch);if(found==bridge.invocations.end())return;
    // 中止回呼時不能再次進入 COM；與 Automation 共用既有 BSTR 配置，先收回本地資源。
    for(std::size_t i=0;i<found->second.parameters.size()*3+1;++i){const auto at=i?scratch+header+((i-1)/3)*stride+((i-1)%3)*32:scratch+32;
        if(memory.load(GuestAddress{at},16)==8)free_bstr(state,memory,memory.load(GuestAddress{at+8},x86_?32:64));
    }
    const auto owner=found->second.owner;bridge.invocations.erase(found);(void)release_type_library(state,memory,owner);(void)free_heap(memory,scratch,state);
}
}
