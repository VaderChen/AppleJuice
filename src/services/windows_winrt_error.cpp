#include "x64arm/windows_services.hpp"
#include "x64arm/windows_error_support.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_memory=0x8007000eU;
constexpr std::uint32_t originate_exception=0x40080201U;
using winrt_error_detail::method;
}
std::span<const WindowsServiceDescriptor> winrt_error_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"combase.dll","RoOriginateError",2},{"combase.dll","RoOriginateErrorW",3},
        {"combase.dll","RoGetErrorReportingFlags",1},{"combase.dll","RoSetErrorReportingFlags",1},
        {"combase.dll","GetRestrictedErrorInfo",1},{"combase.dll","SetRestrictedErrorInfo",1},
        {"internal-restricted-error","QueryInterface",3},{"internal-restricted-error","AddRef",1},{"internal-restricted-error","Release",1},
        {"internal-restricted-error","GetErrorDetails",5},{"internal-restricted-error","GetReference",2},
        {"internal-winrt-error","Raise",1}
    };return table;
}
void WindowsServices::discard_restricted_error(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& operations=runtime_.com_error_bridge.restricted_operations;const auto found=operations.find(id);if(found==operations.end())return;
    auto op=found->second;operations.erase(found);
    if(op.phase==RestrictedErrorOperation::Phase::Query && !op.queried)op.queried=memory.load(GuestAddress{op.scratch},x86_?32:64);
    for(auto object:{op.setting?0:op.original,op.queried})if(runtime_.com_error_bridge.views.contains(object))(void)release_com_error(state,memory,object);
    // 異常離開不得再排入外部 guest callback；自有引用與 scratch 仍會清理。
    if(op.scratch)(void)free_heap(memory,op.scratch,state);
}
std::optional<std::uint32_t> WindowsServices::resume_restricted_error(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint32_t result){
    auto& bridge=runtime_.com_error_bridge;auto& op=bridge.restricted_operations.at(id);const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    if(op.phase==RestrictedErrorOperation::Phase::Cleanup){const auto status=op.result;discard_restricted_error(state,memory,id);complete_call(state,memory,status);return std::nullopt;}
    if(op.phase==RestrictedErrorOperation::Phase::Query){
        op.result=op.setting?result:1;
        if(!(result&0x80000000U)){
            op.queried=memory.load(GuestAddress{op.scratch},bits);
            const auto view=bridge.views.find(op.queried);
            const bool valid=view!=bridge.views.end() && runtime_.com_errors.find(view->second.object)->restricted &&
                view->second.kind==(op.setting?ComErrorViewKind::Info:ComErrorViewKind::Restricted);
            if(!valid)op.result=invalid;
            else if(op.setting){
                op.phase=RestrictedErrorOperation::Phase::Set;
                start_callbacks(state,memory,{{resolve("combase.dll","SetErrorInfo").value,{0,op.queried}}},Continuation::RestrictedError,id);return std::nullopt;
            }else{
                try{memory.store(GuestAddress{op.output},bits,op.queried);op.queried=0;op.result=0;}
                catch(const MemoryFault&){op.result=pointer_error;}
            }
        }
    }else op.result=result;
    std::vector<GuestCallback> callbacks;
    if(!op.setting && op.original)callbacks.push_back({method(memory,op.original,2),{op.original}});
    if(op.queried)callbacks.push_back({method(memory,op.queried,2),{op.queried}});
    op.original=op.queried=0;op.phase=RestrictedErrorOperation::Phase::Cleanup;
    if(!callbacks.empty())start_callbacks(state,memory,std::move(callbacks),Continuation::RestrictedError,id);
    else return resume_restricted_error(state,memory,id,0);return std::nullopt;
}
void WindowsServices::discard_winrt_origin(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& origins=runtime_.com_error_bridge.origins;const auto found=origins.find(id);if(found==origins.end())return;
    const auto op=found->second;origins.erase(found);
    if(op.object && runtime_.com_error_bridge.views.contains(op.object))(void)release_com_error(state,memory,op.object);
    if(op.payload)(void)free_heap(memory,op.payload,state);
}
std::optional<std::uint32_t> WindowsServices::resume_winrt_origin(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint32_t result,bool setup){
    auto& op=runtime_.com_error_bridge.origins.at(id);
    if(setup){
        if(op.object){(void)release_com_error(state,memory,op.object);op.object=0;}
        if(result&0x80000000U){discard_winrt_origin(state,memory,id);complete_call(state,memory,0);return std::nullopt;}
        if(op.exception){start_callbacks(state,memory,{{resolve("internal-winrt-error","Raise").value,{id}}},Continuation::WinrtOriginFinish,id);return std::nullopt;}
    }
    discard_winrt_origin(state,memory,id);complete_call(state,memory,1);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_winrt_error(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;auto& bridge=runtime_.com_error_bridge;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto pointer=[&](std::uint64_t at,std::size_t size){if(!at)throw HostCallFault("WinRT 錯誤輸出指標為空");memory.check(GuestAddress{at},size,Access::Write);};
    const auto next_id=[&](){auto id=bridge.next_operation;while(!id || bridge.restricted_operations.contains(id) || bridge.origins.contains(id))++id;bridge.next_operation=id+1;return id;};
    try{
        if(service.family=="internal-restricted-error"){
            if(name=="QueryInterface" || name=="AddRef" || name=="Release")return invoke_com_error(state,memory,service);
            const auto view=bridge.views.find(arg(0));if(view==bridge.views.end() || view->second.kind!=ComErrorViewKind::Restricted)return done(invalid);
            const auto& value=*runtime_.com_errors.find(view->second.object);
            if(name=="GetReference"){pointer(arg(1),unit);memory.store(GuestAddress{arg(1)},bits,0);return done(0);}
            if(name=="GetErrorDetails"){
                pointer(arg(1),unit);pointer(arg(2),4);pointer(arg(3),unit);pointer(arg(4),unit);
                std::array<std::uint64_t,3> strings{};
                try{
                    const std::array<std::u16string_view,3> texts{value.description,value.restricted_description,value.capability_sid};
                    for(unsigned i=0;i<3;++i){if(i==2 && texts[i].empty())continue;
                        std::vector<std::uint8_t> bytes(texts[i].size()*2);for(std::size_t j=0;j<texts[i].size();++j){bytes[2*j]=static_cast<std::uint8_t>(texts[i][j]);bytes[2*j+1]=static_cast<std::uint8_t>(texts[i][j]>>8);}
                        strings[i]=allocate_bstr(memory,bytes.size(),bytes);if(!strings[i])throw std::bad_alloc();
                    }
                }catch(...){for(auto string:strings)if(string)free_bstr(state,memory,string);throw;}
                memory.store(GuestAddress{arg(1)},bits,strings[0]);memory.store(GuestAddress{arg(2)},32,value.hresult);
                memory.store(GuestAddress{arg(3)},bits,strings[1]);memory.store(GuestAddress{arg(4)},bits,strings[2]);return done(0);
            }
        }
        if(name=="RoGetErrorReportingFlags"){pointer(arg(0),4);memory.store(GuestAddress{arg(0)},32,runtime_.com_errors.flags());return done(0);}
        if(name=="RoSetErrorReportingFlags")return done(runtime_.com_errors.set_flags(static_cast<std::uint32_t>(arg(0)))==com_error::Status::Ok?0:invalid);
        if(name=="GetRestrictedErrorInfo" || name=="SetRestrictedErrorInfo"){
            const bool setting=name=="SetRestrictedErrorInfo";
            if(!setting){pointer(arg(0),unit);memory.store(GuestAddress{arg(0)},bits,0);if(!runtime_.com_errors.current(threads_.current))return done(1);}
            else if(arg(0)){
                const auto view=bridge.views.find(arg(0));if(view==bridge.views.end() || view->second.kind!=ComErrorViewKind::Restricted || !runtime_.com_errors.find(view->second.object)->restricted)return done(invalid);
            }
            if(bridge.restricted_operations.size()>=4096)return done(no_memory);
            const auto id=next_id(),scratch=allocate_heap(memory,unit+16);if(!scratch)return done(no_memory);
            RestrictedErrorOperation op;op.setting=setting;op.scratch=scratch;op.output=setting?0:arg(0);
            memory.store(GuestAddress{scratch},bits,0);memory.write(GuestAddress{scratch+unit},setting?winrt_error_detail::error_iid:winrt_error_detail::restricted_iid);
            try{bridge.restricted_operations.emplace(id,op);}catch(...){(void)free_heap(memory,scratch,state);throw;}
            auto& stored=bridge.restricted_operations.at(id);stored.original=setting?arg(0):runtime_.com_errors.take(threads_.current);
            try{
                if(stored.original)start_callbacks(state,memory,{{method(memory,stored.original,0),{stored.original,scratch+unit,scratch}}},Continuation::RestrictedError,id);
                else{stored.phase=RestrictedErrorOperation::Phase::Set;start_callbacks(state,memory,{{resolve("combase.dll","SetErrorInfo").value,{0,0}}},Continuation::RestrictedError,id);}
            }catch(...){discard_restricted_error(state,memory,id);throw;}return std::nullopt;
        }
        if(name=="Raise"){
            const auto& op=bridge.origins.at(arg(0));GuestState caller=state;arg.return_to(caller,0);
            const std::array<std::uint64_t,3> parameters{op.hresult,op.length,op.payload};
            begin_exception(caller,memory,originate_exception,0,parameters);exceptions_.stack.back().winrt_origination=true;state=caller;return std::nullopt;
        }
        if(name=="RoOriginateError" || name=="RoOriginateErrorW"){
            const auto hr=static_cast<std::uint32_t>(arg(0));if(!(hr&0x80000000U))return done(0);
            auto generic=windows_error_message(hr);if(generic.empty())generic=windows_error_message(0x80004005U);
            std::u16string message;
            const auto source=arg(name=="RoOriginateErrorW"?2:1);
            if(!source)message=com_error::reported_message(hr,std::nullopt,generic);
            else if(name=="RoOriginateError")message=com_error::reported_message(hr,winrt_content(memory,winrt_string(memory,source),com_error::maximum_message_units),generic);
            else{
                const auto count=static_cast<std::uint32_t>(arg(1)),limit=std::min(count?count:com_error::maximum_message_units,com_error::maximum_message_units);
                for(unsigned i=0;i<limit;++i){const auto offset=std::uint64_t{i}*2;if(source>UINT64_MAX-offset)throw HostCallFault("WinRT 訊息位址溢位");const auto c=static_cast<char16_t>(memory.load(GuestAddress{source+offset},16));if(!c)break;message+=c;}
            }
            // 文件的 empty return 表與段落互有差異；採用 FALSE 且不報告、不改 channel。
            if(message.empty())return done(0);
            bool initialized=com_apartment(threads_.current).has_value();
            if(!initialized)initialized=std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),[](const auto& row){return row.second>=16 && !(row.second&2U);});
            const auto reporting=com_error::reporting(runtime_.com_errors.flags(),false,initialized);
            if(!reporting.channel && !reporting.exception)return done(1);
            if(bridge.origins.size()>=4096)return done(0);
            const auto id=next_id();WinrtOriginOperation op;op.hresult=hr;op.length=static_cast<std::uint32_t>(message.size());op.exception=reporting.exception;
            if(reporting.exception){op.payload=allocate_heap(memory,(message.size()+1)*2);if(!op.payload)return done(0);
                for(std::size_t i=0;i<message.size();++i)memory.store(GuestAddress{op.payload+i*2},16,message[i]);memory.store(GuestAddress{op.payload+message.size()*2},16,0);}
            try{
                if(reporting.channel){com_error::Object value;value.hresult=hr;value.description=generic;value.description_present=true;value.restricted_description=message;value.restricted=true;op.object=create_com_error(state,memory,std::move(value));}
                bridge.origins.emplace(id,op);
            }catch(...){if(op.object)(void)release_com_error(state,memory,op.object);if(op.payload)(void)free_heap(memory,op.payload,state);throw;}
            try{
                if(op.object)start_callbacks(state,memory,{{resolve("combase.dll","SetErrorInfo").value,{0,op.object}}},Continuation::WinrtOriginSetup,id);
                else start_callbacks(state,memory,{{resolve("internal-winrt-error","Raise").value,{id}}},Continuation::WinrtOriginFinish,id);
            }catch(...){discard_winrt_origin(state,memory,id);throw;}return std::nullopt;
        }
    }catch(const MemoryFault&){return done(name.starts_with("RoOriginateError")?0:pointer_error);}
    catch(const std::bad_alloc&){return done(name.starts_with("RoOriginateError")?0:no_memory);}
    catch(const HostCallFault&){return done(name.starts_with("RoOriginateError")?0:pointer_error);}
    throw HostCallFault("未知的 WinRT 錯誤資訊服務");
}
}
