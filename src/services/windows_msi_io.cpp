#include "windows_msi_state.hpp"
#include <algorithm>

namespace x64arm {
void WindowsServices::discard_msi(GuestState& state,GuestMemory& memory,std::uint64_t token){
    auto& bridge=msi_bridge();const auto it=bridge.operations.find(token);if(it==bridge.operations.end())return;auto op=std::move(it->second);bridge.operations.erase(it);
    if(op.document && bridge.documents.contains(op.document)){auto& doc=bridge.documents.at(op.document);doc.busy=0;if(op.modified && !op.published)doc.failed=true;}
    if(op.owned && op.file)close_transfer_handle(memory,op.file);
    if(op.registry_key)runtime_.registry.handles.erase(op.registry_key);
    if(op.scratch)(void)free_heap(memory,op.scratch,state);if(op.buffer)(void)free_heap(memory,op.buffer,state);collect_msi(memory);
}
std::optional<std::uint32_t> WindowsServices::resume_msi(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& bridge=msi_bridge();const auto it=bridge.operations.find(token);if(it==bridge.operations.end())throw HostCallFault("找不到 MSI 續接操作");auto& op=it->second;
    using Kind=MsiState::Kind;using Phase=MsiState::Phase;
    if(op.kind==Kind::Product || op.kind==Kind::Component)return resume_msi_registry(state,memory,token,result);
    const auto callback=[&](Phase phase,std::string_view name,std::vector<std::uint64_t> arguments,std::uint32_t wide=0){
        op.phase=phase;GuestCallback call;call.target=resolve("kernel32.dll",name).value;call.arguments=std::move(arguments);call.wide_arguments=wide;
        start_callbacks(state,memory,{std::move(call)},Continuation::MsiOperation,token);return std::optional<std::uint32_t>{};
    };
    const auto finish=[&](){
        std::uint32_t status=op.error;std::uint64_t installed_document{};std::uint32_t installed_handle{};
        try{
            if(!status && op.kind==Kind::Open){
                MsiState::Document document;document.data=std::move(op.initial);document.committed=std::move(op.bytes);document.path=op.destination.empty()?op.path:op.destination;document.file=op.file;document.mode=op.mode;
                installed_document=bridge.next_document++;bridge.documents.emplace(installed_document,std::move(document));
                MsiState::Object object;object.type=MsiState::Type::Database;object.owner=op.owner;object.document=installed_document;installed_handle=bridge.add(std::move(object));
                memory.store(GuestAddress{op.output},32,installed_handle);op.owned=false;
            }else if(!status && op.kind==Kind::Stream){
                auto& record=bridge.get(op.record,MsiState::Type::Record);if(op.field>=record.fields.size())throw msi::Error{87,"MSI 串流欄位無效"};
                record.fields[op.field]=std::move(op.bytes);record.positions.erase(op.field);
            }
        }catch(const MemoryFault&){status=87;}catch(const msi::Error& error){status=error.code;}catch(const std::bad_alloc&){status=8;}
        if(status && installed_document){bridge.objects.erase(installed_handle);bridge.documents.erase(installed_document);}
        if(op.kind==Kind::Commit){auto& document=bridge.documents.at(op.document);document.busy=0;document.failed|=op.restore_failed;
            if(op.published){document.committed=std::move(op.bytes);++document.data.storage.transaction;}}
        if(op.owned && op.file)close_transfer_handle(memory,op.file);
        const auto saved=op.saved_error;if(op.scratch)(void)free_heap(memory,op.scratch,state);if(op.buffer)(void)free_heap(memory,op.buffer,state);
        bridge.operations.erase(token);collect_msi(memory);last_error_=saved;complete_call(state,memory,status);return std::optional<std::uint32_t>{};
    };
    const auto seek=[&](Phase phase,std::uint64_t offset){return callback(phase,"SetFilePointerEx",{op.file,offset,0,0},2);};
    const auto failure=[&](std::uint32_t error){
        if(!op.error)op.error=error;
        if(op.modified && op.kind==Kind::Commit && !op.restoring){op.restoring=true;op.offset=0;return seek(Phase::RestoreSeek,0);}
        if(op.restoring)op.restore_failed=true;return finish();
    };
    const auto write=[&](Phase phase,const std::vector<std::uint8_t>& bytes,std::size_t end){
        if(op.offset>end || end>bytes.size())throw msi::Error{1627,"MSI 寫入範圍無效"};op.requested=static_cast<std::uint32_t>(std::min<std::size_t>(65536,end-op.offset));
        memory.write(GuestAddress{op.buffer},std::span(bytes).subspan(op.offset,op.requested));memory.store(GuestAddress{op.scratch+16},32,0);
        return callback(phase,"WriteFile",{op.file,op.buffer,op.requested,op.scratch+16,0});
    };
    const auto read=[&](){op.requested=static_cast<std::uint32_t>(std::min<std::size_t>(65536,op.bytes.size()-op.offset));memory.store(GuestAddress{op.scratch+16},32,0);return callback(Phase::Read,"ReadFile",{op.file,op.buffer,op.requested,op.scratch+16,0});};
    const auto loaded=[&](){
        if(op.kind==Kind::Stream)return finish();
        try{op.initial=msi::parse(op.bytes,65001);const bool patch=op.initial.storage.nodes.at(0).clsid[0]==0x86;if(patch!=op.patch)return failure(110);}
        catch(const msi::Error&){return failure(110);}catch(const compound::Error&){return failure(110);}
        if(op.destination.empty())return finish();
        // 複製輸出模式仍由同一組檔案服務處理權限／分享，不把輸入檔當輸出改寫。
        close_transfer_handle(memory,op.file);op.file=0;op.owned=false;memory.write(GuestAddress{op.scratch+128},utf16_bytes(op.destination));
        return callback(Phase::Destination,"CreateFileW",{op.scratch+128,0xc0000000U,1,0,2,0x80,0});
    };
    try{
        if(op.phase==Phase::Start){
            const auto path_size=std::max(op.path.size(),op.destination.size());op.scratch=allocate_heap(memory,128+(path_size+1)*2);if(!op.scratch)throw std::bad_alloc();op.buffer=allocate_heap(memory,65536);if(!op.buffer)throw std::bad_alloc();
            if(op.kind==Kind::Commit){bridge.documents.at(op.document).busy=token;op.offset=op.previous.size();op.header_size=512;return seek(Phase::SeekBody,op.offset);}
            memory.write(GuestAddress{op.scratch+128},utf16_bytes(op.path));const bool create=op.kind==Kind::Open && op.mode>=3;
            const auto access=op.kind==Kind::Open && op.mode && op.destination.empty()?0xc0000000U:0x80000000U;
            return callback(Phase::Opened,"CreateFileW",{op.scratch+128,access,1,0,create?2U:3U,0x80,0});
        }
        if(op.phase==Phase::Opened || op.phase==Phase::Destination){
            if(result==(x86_?UINT32_MAX:UINT64_MAX))return failure(110);op.file=result;op.owned=true;
            if(op.phase==Phase::Destination || (op.kind==Kind::Open && op.mode>=3)){
                if(op.phase!=Phase::Destination){op.initial=msi::create(0,65001);if(op.patch)op.initial.storage.nodes.at(0).clsid[0]=0x86;op.bytes=msi::serialize(op.initial);}
                op.offset=0;op.modified=true;return write(Phase::WriteBody,op.bytes,op.bytes.size());
            }
            return callback(Phase::Size,"GetFileSizeEx",{op.file,op.scratch});
        }
        if(op.phase==Phase::Size){
            if(!result)return failure(last_error_?last_error_:30);const auto size=memory.load(GuestAddress{op.scratch},64);if(size>compound::default_limit)return failure(8);
            op.bytes.resize(static_cast<std::size_t>(size));op.offset=0;return size?read():loaded();
        }
        if(op.phase==Phase::Read){
            if(!result)return failure(last_error_?last_error_:30);const auto count=memory.load(GuestAddress{op.scratch+16},32);if(!count || count>op.requested)return failure(30);
            memory.read(GuestAddress{op.buffer},std::span(op.bytes).subspan(op.offset,static_cast<std::size_t>(count)));op.offset+=static_cast<std::size_t>(count);return op.offset<op.bytes.size()?read():loaded();
        }
        if(op.phase==Phase::SeekBody){if(!result)return failure(last_error_?last_error_:29);op.modified=true;return write(Phase::WriteBody,op.bytes,op.bytes.size());}
        if(op.phase==Phase::WriteBody || op.phase==Phase::WriteHeader || op.phase==Phase::RestoreWrite){
            if(!result)return failure(last_error_?last_error_:29);const auto count=memory.load(GuestAddress{op.scratch+16},32);if(!count || count>op.requested)return failure(29);op.offset+=static_cast<std::size_t>(count);
            if(op.phase==Phase::WriteBody){if(op.offset<op.bytes.size())return write(Phase::WriteBody,op.bytes,op.bytes.size());return callback(Phase::FlushBody,"FlushFileBuffers",{op.file});}
            if(op.phase==Phase::WriteHeader){if(op.offset<op.header_size)return write(Phase::WriteHeader,op.bytes,op.header_size);return callback(Phase::Flush,"FlushFileBuffers",{op.file});}
            if(op.offset<op.header_size)return write(Phase::RestoreWrite,op.previous,op.header_size);return seek(Phase::RestoreEndSeek,op.previous.size());
        }
        if(op.phase==Phase::FlushBody){if(!result)return failure(last_error_?last_error_:29);if(op.kind==Kind::Open)return finish();op.offset=0;return seek(Phase::SeekHeader,0);}
        if(op.phase==Phase::SeekHeader){if(!result)return failure(last_error_?last_error_:29);return write(Phase::WriteHeader,op.bytes,op.header_size);}
        if(op.phase==Phase::Flush){if(!result)return failure(last_error_?last_error_:29);op.published=true;return finish();}
        if(op.phase==Phase::RestoreSeek){if(!result)return failure(last_error_?last_error_:29);return write(Phase::RestoreWrite,op.previous,op.header_size);}
        if(op.phase==Phase::RestoreEndSeek){if(!result)return failure(last_error_?last_error_:29);return callback(Phase::RestoreEnd,"SetEndOfFile",{op.file});}
        if(op.phase==Phase::RestoreEnd){if(!result)return failure(last_error_?last_error_:29);return callback(Phase::RestoreFlush,"FlushFileBuffers",{op.file});}
        if(op.phase==Phase::RestoreFlush){if(!result)op.restore_failed=true;return finish();}
        return failure(1627);
    }catch(const msi::Error& error){return failure(error.code);}catch(const compound::Error&){return failure(1627);}catch(const MemoryFault&){return failure(87);}catch(const std::bad_alloc&){return failure(8);}
}
}
