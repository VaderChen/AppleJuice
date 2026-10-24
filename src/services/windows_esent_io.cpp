#include "windows_esent_state.hpp"

namespace x64arm {
void WindowsServices::discard_esent(GuestState& state,GuestMemory& memory,std::uint64_t token){
    auto& bridge=esent_bridge();const auto it=bridge.operations.find(token);if(it==bridge.operations.end())return;
    auto op=std::move(it->second);bridge.operations.erase(it);if(auto s=bridge.sessions.find(op.session);s!=bridge.sessions.end())s->second.busy=0;
    if(op.file)close_transfer_handle(memory,op.file);if(op.scratch)(void)free_heap(memory,op.scratch,state);if(op.buffer)(void)free_heap(memory,op.buffer,state);
}
std::optional<std::uint32_t> WindowsServices::resume_esent(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& bridge=esent_bridge();const auto found=bridge.operations.find(token);if(found==bridge.operations.end())throw HostCallFault("找不到 ESENT 續接操作");auto& op=found->second;using Phase=EsentState::Phase;
    const auto finish=[&](std::int32_t error){const auto saved=op.saved_error;discard_esent(state,memory,token);last_error_=saved;complete_call(state,memory,static_cast<std::uint32_t>(error));return std::optional<std::uint32_t>{};};
    const auto file_error=[&](){if(last_error_==2 || last_error_==3)return -1811;if(last_error_==5)return -1032;if(last_error_==32 || last_error_==33)return -1202;return -1022;};
    const auto callback=[&](Phase phase,std::string_view name,std::vector<std::uint64_t> values){
        op.phase=phase;GuestCallback call;call.target=resolve("kernel32.dll",name).value;call.arguments=std::move(values);start_callbacks(state,memory,{std::move(call)},Continuation::EsentOperation,token);return std::optional<std::uint32_t>{};
    };
    const auto read=[&](){op.requested=static_cast<std::uint32_t>(std::min<std::size_t>(65536,op.bytes.size()-op.offset));memory.store(GuestAddress{op.scratch+16},32,0);return callback(Phase::Read,"ReadFile",{op.file,op.buffer,op.requested,op.scratch+16,0});};
    const auto loaded=[&](){
        EsentState::Document document;document.data=esent::parse(std::move(op.bytes));
        if(document.data.page_size!=bridge.page_size)return finish(-1213);
        // 尚未支援日誌復原時，不把髒關閉資料庫當成已復原的乾淨快照。
        if(document.data.state!=3)return finish(-550);
        document.instance=op.instance;document.file=op.file;document.path=op.path;document.key=op.key;document.flags=op.flags;document.maximum_pages=op.maximum_pages;
        ese_api::require(bridge.next_database!=UINT32_MAX,-1027);const auto id=bridge.next_database++;
        bridge.documents.emplace(id,std::move(document));op.file=0;return finish(0);
    };
    try{
        if(op.phase==Phase::Start){
            op.scratch=allocate_heap(memory,128+(op.path.size()+1)*2);if(!op.scratch)throw std::bad_alloc();op.buffer=allocate_heap(memory,65536);if(!op.buffer)throw std::bad_alloc();
            memory.write(GuestAddress{op.scratch+128},utf16_bytes(op.path));
            return callback(Phase::Opened,"CreateFileW",{op.scratch+128,(op.flags&1)?0x80000000U:0xc0000000U,(op.flags&1)?1U:0U,0,3,0x80,0});
        }
        if(op.phase==Phase::Opened){if(result==(x86_?UINT32_MAX:UINT64_MAX))return finish(file_error());op.file=result;return callback(Phase::Size,"GetFileSizeEx",{op.file,op.scratch});}
        if(op.phase==Phase::Size){if(!result)return finish(file_error());const auto size=memory.load(GuestAddress{op.scratch},64);if(size>esent::default_limit)return finish(-1011);op.bytes.resize(static_cast<std::size_t>(size));return size?read():loaded();}
        if(op.phase==Phase::Read){
            if(!result)return finish(file_error());const auto count=memory.load(GuestAddress{op.scratch+16},32);if(!count || count>op.requested)return finish(-1022);
            memory.read(GuestAddress{op.buffer},std::span(op.bytes).subspan(op.offset,static_cast<std::size_t>(count)));op.offset+=static_cast<std::size_t>(count);return op.offset<op.bytes.size()?read():loaded();
        }
        return finish(-1003);
    }catch(const ese_api::Error& error){return finish(error.code);}catch(const esent::Error& error){return finish(ese_api::format_error(error));}
    catch(const MemoryFault&){return finish(-1003);}catch(const std::bad_alloc&){return finish(-1011);}
}
}
