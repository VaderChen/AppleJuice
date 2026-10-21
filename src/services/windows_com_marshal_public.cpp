#include "x64arm/windows_services.hpp"
#include <array>

namespace x64arm {
namespace {
constexpr std::uint32_t pointer_error=0x80004003U,no_interface=0x80004002U,
    invalid=0x80070057U,no_memory=0x8007000eU,read_fault=0x8003001eU,
    write_fault=0x8003001dU,disconnected=0x800401fdU;
WindowsGuid marshal_iid(std::uint32_t value){
    WindowsGuid id{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    for(unsigned i=0;i<4;++i)id[i]=static_cast<std::uint8_t>(value>>(8*i));return id;
}
bool failed(std::uint64_t result){return (result&0x80000000U)!=0;}
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const auto bits=static_cast<std::uint8_t>(memory.execution_mode()==ExecutionMode::X86?32:64);
    if(!object)throw MemoryFault(GuestAddress{object},Access::Read,bits/8,"COM 物件為空");
    const auto table=memory.load(GuestAddress{object},bits);
    return memory.load(GuestAddress{table+slot*(bits/8)},bits);
}
}

std::optional<std::uint32_t> WindowsServices::resume_com_marshal_public(
    GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t returned){
    using Kind=ComMarshalOperation::Kind;
    auto& op=runtime_.com_marshal.operations.at(id);
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto cell=[&](unsigned offset){return memory.load(GuestAddress{op.scratch+offset},bits);};
    const auto callback=[&](GuestCallback call,unsigned next){
        op.step=next;start_callbacks(state,memory,{std::move(call)},Continuation::ComMarshalOperation,id);
        return std::optional<std::uint32_t>{};
    };
    const auto api=[&](std::string_view name,std::vector<std::uint64_t> args,unsigned next){
        return callback({resolve("ole32.dll",name).value,std::move(args)},next);
    };
    const auto cleanup=[&](std::uint64_t result){
        op.result=result;
        if(op.kind==Kind::InterThread && op.stream_owned && op.context_pointer){
            op.context_pointer=0;
            GuestCallback seek{method(memory,op.stream,5),{op.stream,0,0,0}};
            seek.wide_arguments=2;return callback(std::move(seek),81);
        }
        // 每個引用先轉交給真正的 Release，再進入下一個清理階段。
        for(auto pair:{std::pair{&op.delivered_owned,op.delivered},
                       std::pair{&op.interface_owned,op.interface},
                       std::pair{&op.temporary_owned,op.temporary},
                       std::pair{&op.stream_owned,op.stream}}){
            if(*pair.first){const auto target=method(memory,pair.second,2);*pair.first=false;
                return callback({target,{pair.second}},90);}
        }
        discard_com_marshal(state,memory,id);complete_call(state,memory,result);
        return std::optional<std::uint32_t>{};
    };
    try {
        if(op.step==90)return cleanup(op.result);
        if(op.step==81){if(failed(returned))return cleanup(op.result);
            return api("CoReleaseMarshalData",{op.stream},82);}
        if(op.step==82)return cleanup(op.result);
        if(op.symbol=="DisconnectPublic"){
            if(op.step==0){memory.write(GuestAddress{op.scratch+96},marshal_iid(3));memory.write(GuestAddress{op.scratch+112},marshal_iid(0));
                return callback({method(memory,op.object,0),{op.object,op.scratch+96,op.scratch+80}},1);}
            if(op.step==1 && returned==no_interface)return api("CoGetStandardMarshal",{op.scratch+112,op.object,0,0,0,op.scratch+80},2);
            if(op.step==1 || op.step==2){
                op.temporary=cell(80);op.temporary_owned=op.temporary!=0;
                if(failed(returned))return cleanup(returned);if(!op.temporary)return cleanup(no_interface);
                return callback({method(memory,op.temporary,8),{op.temporary,0}},3);
            }
            return cleanup(returned);
        }
        if(op.symbol=="StandardDelegate"){
            if(op.step==0){memory.write(GuestAddress{op.scratch+96},op.iid);
                return api("CoGetStandardMarshal",{op.scratch+96,op.reference,op.context,0,op.flags,op.scratch+80},1);}
            if(op.step==1){if(failed(returned))return cleanup(returned);
                op.temporary=cell(80);if(!op.temporary)return cleanup(no_interface);op.temporary_owned=true;
                if(op.context_pointer==5)return callback({method(memory,op.temporary,5),
                    {op.temporary,op.stream,op.scratch+96,op.reference,op.context,0,op.flags}},2);
                return callback({method(memory,op.temporary,static_cast<unsigned>(op.context_pointer)),
                    {op.temporary,op.scratch+96,op.reference,op.context,0,op.flags,op.output}},2);
            }
            return cleanup(returned);
        }
        if(op.kind==Kind::PublicMarshal){
            const bool size=op.symbol=="CoGetMarshalSizeMax";
            if(op.step==0){memory.write(GuestAddress{op.scratch+96},marshal_iid(3));
                memory.write(GuestAddress{op.scratch+112},op.iid);
                return callback({method(memory,op.object,0),{op.object,op.scratch+96,op.scratch+80}},1);}
            if(op.step==1){
                if(returned==no_interface)return api("CoGetStandardMarshal",
                    {op.scratch+112,op.object,op.context,0,op.flags,op.scratch+80},2);
                if(failed(returned))return cleanup(returned);
            }
            if(op.step==1 || op.step==2){
                if(failed(returned))return cleanup(returned);
                op.temporary=cell(80);if(!op.temporary)return cleanup(no_interface);op.temporary_owned=true;
                return callback({method(memory,op.temporary,size?4:3),
                    {op.temporary,op.scratch+112,op.object,op.context,0,op.flags,op.scratch+(size?144:24)}},3);
            }
            if(op.step==3){
                if(failed(returned))return cleanup(returned);
                if(size){const auto count=memory.load(GuestAddress{op.scratch+144},32);
                    if(count>UINT32_MAX-48)return cleanup(invalid);
                    memory.store(GuestAddress{op.output},32,count+48);return cleanup(0);}
                // OBJREF_CUSTOM：IID 與 CLSID 後的兩個 DWORD 是 extension／reserved，非資料長度。
                memory.store(GuestAddress{op.scratch},32,0x574f454dU);
                memory.store(GuestAddress{op.scratch+4},32,4);
                memory.write(GuestAddress{op.scratch+8},op.iid);
                memory.store(GuestAddress{op.scratch+40},32,0);
                memory.store(GuestAddress{op.scratch+44},32,0);
                memory.store(GuestAddress{op.scratch+64},32,0);
                return callback({method(memory,op.stream,4),{op.stream,op.scratch,48,op.scratch+64}},4);
            }
            if(op.step==4){
                if(failed(returned))return cleanup(returned);
                if(memory.load(GuestAddress{op.scratch+64},32)!=48)return cleanup(write_fault);
                return callback({method(memory,op.temporary,5),
                    {op.temporary,op.stream,op.scratch+112,op.object,op.context,0,op.flags}},5);
            }
            return cleanup(returned);
        }
        if(op.kind==Kind::PublicUnmarshal || op.kind==Kind::PublicRelease){
            const bool releasing=op.kind==Kind::PublicRelease;
            if(op.step==0){memory.store(GuestAddress{op.scratch+64},32,0);
                return callback({method(memory,op.stream,3),{op.stream,op.scratch,48,op.scratch+64}},1);}
            if(op.step==1){
                if(failed(returned))return cleanup(returned);
                if(memory.load(GuestAddress{op.scratch+64},32)!=48)return cleanup(read_fault);
                if(memory.load(GuestAddress{op.scratch},32)!=0x574f454dU ||
                   memory.load(GuestAddress{op.scratch+4},32)!=4)return cleanup(invalid);
                WindowsGuid cls{};memory.read(GuestAddress{op.scratch+24},cls);
                if(cls==marshal_iid(0x33a) || cls==marshal_iid(0x17)){
                    const auto root=create_com_marshaler(state,memory,0,cls==marshal_iid(0x17));
                    op.temporary=root+unit;op.temporary_owned=true;
                }else{
                    memory.write(GuestAddress{op.scratch+96},marshal_iid(3));
                    return api("CoCreateInstance",{op.scratch+24,0,1,op.scratch+96,op.scratch+80},2);
                }
            }
            if(op.step==2){if(failed(returned))return cleanup(returned);
                op.temporary=cell(80);if(!op.temporary)return cleanup(no_interface);op.temporary_owned=true;}
            if(op.step==1 || op.step==2){
                if(releasing)return callback({method(memory,op.temporary,7),{op.temporary,op.stream}},3);
                return callback({method(memory,op.temporary,6),
                    {op.temporary,op.stream,op.scratch+8,op.scratch+152}},3);
            }
            if(op.step==3){
                if(releasing || failed(returned))return cleanup(returned);
                op.interface=cell(152);if(!op.interface)return cleanup(disconnected);op.interface_owned=true;
                WindowsGuid header{};memory.read(GuestAddress{op.scratch+8},header);
                if(op.iid==WindowsGuid{} || op.iid==header){
                    memory.store(GuestAddress{op.output},bits,op.interface);op.interface_owned=false;return cleanup(0);}
                memory.write(GuestAddress{op.scratch+112},op.iid);
                return callback({method(memory,op.interface,0),
                    {op.interface,op.scratch+112,op.scratch+160}},4);
            }
            if(failed(returned))return cleanup(returned);
            op.delivered=cell(160);if(!op.delivered)return cleanup(no_interface);op.delivered_owned=true;
            memory.store(GuestAddress{op.output},bits,op.delivered);op.delivered_owned=false;return cleanup(0);
        }
        if(op.kind==Kind::InterThread){
            if(op.step==0){memory.write(GuestAddress{op.scratch+96},op.iid);
                return api("CreateStreamOnHGlobal",{0,1,op.scratch+80},1);}
            if(op.step==1){if(failed(returned))return cleanup(returned);
                op.stream=cell(80);if(!op.stream)return cleanup(no_memory);op.stream_owned=true;
                return api("CoMarshalInterface",{op.stream,op.scratch+96,op.object,3,0,0},2);}
            if(op.step==2){if(failed(returned))return cleanup(returned);op.context_pointer=1;
                GuestCallback seek{method(memory,op.stream,5),{op.stream,0,0,0}};
                seek.wide_arguments=2;return callback(std::move(seek),3);}
            if(failed(returned))return cleanup(returned);
            memory.store(GuestAddress{op.output},bits,op.stream);op.stream_owned=false;op.context_pointer=0;return cleanup(0);
        }
        if(op.kind==Kind::GetAndRelease){
            if(op.step==0)return api("CoUnmarshalInterface",{op.stream,op.context_pointer,op.output},1);
            // 呼叫者把 stream 所有權交給此 API；包含失敗路徑也必須 Release。
            return cleanup(returned);
        }
        return cleanup(invalid);
    }catch(const MemoryFault&){op.error=pointer_error;}
     catch(const std::bad_alloc&){op.error=no_memory;}
     catch(const std::invalid_argument&){op.error=invalid;}
    return cleanup(op.error);
}
}
