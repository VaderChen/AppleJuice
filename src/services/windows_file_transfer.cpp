#include "x64arm/windows_services.hpp"
#include <algorithm>
#if defined(__APPLE__)
#include <fcntl.h>
#endif
namespace x64arm {
namespace {
constexpr std::uint64_t resume_magic=UINT64_C(0x3250434d52413458);
std::uint64_t copy_hash(std::span<const std::uint8_t> bytes,std::uint64_t seed=UINT64_C(14695981039346656037)){
    for(auto byte:bytes)seed=(seed^byte)*UINT64_C(1099511628211);return seed;
}
std::uint64_t copy_key(const std::string& source,const std::string& target){
    auto seed=copy_hash({reinterpret_cast<const std::uint8_t*>(source.data()),source.size()});seed=(seed^0)*UINT64_C(1099511628211);
    return copy_hash({reinterpret_cast<const std::uint8_t*>(target.data()),target.size()},seed);
}
std::array<std::uint8_t,64> restart_header(const FileMetadata& source,std::uint64_t position,std::uint64_t key){
    std::array<std::uint8_t,64> bytes{};const std::array values{resume_magic,source.id,source.device,source.modified,source.size,position,key};
    for(unsigned i=0;i<values.size();++i)for(unsigned b=0;b<8;++b)bytes[i*8+b]=static_cast<std::uint8_t>(values[i]>>(b*8));
    const auto checksum=copy_hash(std::span(bytes).first(56));for(unsigned b=0;b<8;++b)bytes[56+b]=static_cast<std::uint8_t>(checksum>>(b*8));return bytes;
}
std::optional<std::uint64_t> restart_position(const HostFile& target,const HostFile& source,const FileMetadata& metadata,std::uint64_t key){
    const auto info=target.information();if(info.error || info.metadata.size<64)return {};
    std::array<std::uint8_t,64> bytes{};const auto at=info.metadata.size-64;const auto read=target.read(bytes,at);if(read.error || read.bytes!=bytes.size())return {};
    std::uint64_t position=0;for(unsigned b=0;b<8;++b)position|=std::uint64_t(bytes[40+b])<<(b*8);
    if(position!=at || position>metadata.size || bytes!=restart_header(metadata,position,key))return {};
    // 不信任可由呼叫者修改的 header：已複製的 prefix 必須與來源實際位元組相同。
    std::array<std::uint8_t,65536> first{},second{};
    for(std::uint64_t offset=0;offset<position;){const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(first.size(),position-offset));
        const auto a=source.read(std::span(first).first(count),offset),b=target.read(std::span(second).first(count),offset);
        if(a.error || b.error || a.bytes!=count || b.bytes!=count || !std::equal(first.begin(),first.begin()+count,second.begin()))return {};
        offset+=count;
    }
    return position;
}
std::uint32_t save_restart(const HostFile& target,const FileMetadata& source,std::uint64_t position,std::uint64_t key){
    const auto bytes=restart_header(source,position,key);const auto written=target.write(bytes,position);
    if(written.error || written.bytes!=bytes.size())return written.error?written.error:112;
    if(const auto error=target.truncate(position+bytes.size()))return error;return target.flush();
}
std::uint32_t copy_hresult(std::uint32_t error){return error?0x80070000U|(error&0xffffU):0U;}
}
std::optional<std::uint32_t> WindowsServices::start_file_transfer(GuestState& state,GuestMemory& memory,std::string source,std::string destination,std::uint32_t flags,std::uint64_t callback,std::uint64_t parameter,std::uint64_t cancel,bool move,bool flush,bool copy2){
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint32_t>{last_error_=error;complete_call(state,memory,copy2?copy_hresult(error):0);return std::nullopt;};
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許宿主檔案複製");
    const auto supported=copy2?0x1U|0x2U|0x4U|0x8U|0x1000U|0x4000U|0x8000U|0x40000U|0x4000000U|0x8000000U|0x10000000U:0x900bU;
    if(flags&~supported)return fail(50);
    if(cancel && memory.load(GuestAddress{cancel},32))return fail(1235);
    if(callback && !handles(GuestAddress{callback}))memory.check(GuestAddress{callback},1,Access::Execute);
    if(file_transfers_.size()>=128 || files_.size()>4094)return fail(8);
    const auto input_access=0x80000000U|((flags&4U)?0x40000000U:0U);
    const auto input=filesystem_.open_host(source,input_access,3,0,7);if(input.error)return fail(input.error);
    if(input.metadata.attributes&0x10U)return fail(5);
    const auto target=filesystem_.information(destination);
    if(!target.error && target.metadata.id==input.metadata.id && target.metadata.device==input.metadata.device)return fail(5);
    const bool resumable=copy2 && (flags&0x4002U);const auto output_access=0x40000100U|(resumable?0x80000000U:0U);
    const auto output=filesystem_.open_host(destination,output_access,resumable?4U:(flags&1U)?1U:2U,0,3);if(output.error)return fail(output.error==183?80:output.error);
    const auto key=copy_key(input.key,output.key);const auto resumed=resumable?restart_position(*output.file,*input.file,input.metadata,key):std::optional<std::uint64_t>{};
    if(resumable && !resumed){if(!target.error && (flags&1U))return fail(80);if(const auto error=output.file->truncate(0))return fail(error);}
    if(flags&0x1000U){
#if defined(__APPLE__)
        if(::fcntl(input.file->transfer_descriptor(),F_NOCACHE,1) || ::fcntl(output.file->transfer_descriptor(),F_NOCACHE,1))return fail(50);
#else
        return fail(50);
#endif
    }
    const auto source_handle=runtime_.allocate_wait_handle(),target_handle=runtime_.allocate_wait_handle();
    const auto install=[&](const OpenHostFile& opened,const std::string& path,std::uint64_t handle,std::uint32_t access,std::uint32_t sharing){FileHandle file;file.key=opened.key;file.path=path;file.metadata=opened.metadata;file.native=opened.file;file.bytes=std::make_shared<const std::vector<std::uint8_t>>();file.object=handle;file.access=access;file.sharing=sharing;files_.emplace(handle,std::move(file));runtime_.win_file_positions[handle]=0;};
    install(input,source,source_handle,input_access,7);install(output,destination,target_handle,output_access,3);
    FileTransfer operation;operation.source=std::move(source);operation.destination=std::move(destination);operation.input=input.file;operation.output=output.file;operation.metadata=input.metadata;operation.callback=callback;operation.parameter=parameter;operation.cancel=cancel;operation.source_handle=source_handle;operation.target_handle=target_handle;operation.move=move;operation.flush=flush;
    operation.copy2=copy2;operation.flags=flags;operation.owner=threads_.current;operation.position=resumed.value_or(0);operation.resume_key=key;
    file_transfers_.emplace(source_handle,std::move(operation));return advance_file_transfer(state,memory,source_handle,0);
}
std::optional<std::uint32_t> WindowsServices::advance_file_transfer(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t callback_result){
    const auto found=file_transfers_.find(token);if(found==file_transfers_.end())throw HostCallFault("檔案複製 continuation 遺失");auto& operation=found->second;
    const auto finish=[&](std::uint32_t error,bool remove_destination)->std::optional<std::uint32_t>{
        const auto destination=operation.destination,source=operation.source;const bool move=operation.move,copy2=operation.copy2;
        const auto identity=operation.output->information().metadata;
        if(operation.message)(void)free_heap(memory,operation.message,state);
        for(auto handle:{operation.source_handle,operation.target_handle})close_transfer_handle(memory,handle);
        file_transfers_.erase(found);
        if(remove_destination){const auto current=filesystem_.information(destination);if(!current.error && current.metadata.id==identity.id && current.metadata.device==identity.device)(void)filesystem_.remove(destination,false);}
        if(!error && move)(void)filesystem_.remove(source,false);
        if(error)last_error_=error;complete_call(state,memory,copy2?copy_hresult(error):error?0:1);return std::nullopt;
    };
    if(operation.copy2){
#include "windows_copyfile2_progress.inc"
    }
    if(operation.pending){operation.pending=false;if(callback_result==1 || callback_result==2)return finish(1235,callback_result==1);if(callback_result==3)operation.callback=0;}
    for(;;){
        if(operation.cancel && memory.load(GuestAddress{operation.cancel},32))return finish(1235,true);
        const bool stream_switch=!operation.started;operation.started=true;
        if(!stream_switch){
            if(operation.position>=operation.metadata.size){
                if(const auto error=operation.output->set_times(operation.metadata.created,operation.metadata.accessed,operation.metadata.modified))return finish(error,false);
                if(operation.flush)if(const auto error=operation.output->flush())return finish(error,false);
                // 新檔案沿用可攜的 DOS 屬性；宿主權限由受限檔案層處理。
                if(const auto error=filesystem_.set_attributes(operation.destination,operation.metadata.attributes&0x27U))return finish(error,false);
                return finish(0,false);
            }
            std::array<std::uint8_t,65536> buffer{};const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),operation.metadata.size-operation.position));
            const auto read=operation.input->read(std::span(buffer).first(count),operation.position);if(read.error || !read.bytes)return finish(read.error?read.error:38,false);
            std::uint64_t written=0;while(written<read.bytes){const auto write=operation.output->write(std::span(buffer).subspan(static_cast<std::size_t>(written),static_cast<std::size_t>(read.bytes-written)),operation.position+written);if(write.error || !write.bytes)return finish(write.error?write.error:112,false);written+=write.bytes;}
            operation.position+=read.bytes;runtime_.win_file_positions[operation.source_handle]=operation.position;runtime_.win_file_positions[operation.target_handle]=operation.position;
        }
        if(operation.callback){operation.pending=true;GuestCallback progress{operation.callback,{operation.metadata.size,operation.position,operation.metadata.size,operation.position,1,stream_switch?1U:0U,operation.source_handle,operation.target_handle,operation.parameter}};progress.wide_arguments=0xf;start_callbacks(state,memory,{std::move(progress)},Continuation::FileTransfer,token);return std::nullopt;}
    }
}
void WindowsServices::discard_file_transfers(GuestState& state,GuestMemory& memory,std::uint64_t owner){
    for(auto it=file_transfers_.begin();it!=file_transfers_.end();){
        if(it->second.owner!=owner){++it;continue;}auto operation=std::move(it->second);it=file_transfers_.erase(it);
        if(operation.message)(void)free_heap(memory,operation.message,state);
        for(auto handle:{operation.source_handle,operation.target_handle})close_transfer_handle(memory,handle);
    }
}
}
