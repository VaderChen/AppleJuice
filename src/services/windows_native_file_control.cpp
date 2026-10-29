#include "x64arm/windows_services.hpp"
#include "windows_file_copy.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>
#include <system_error>
#include <tuple>

namespace x64arm {
namespace {
FileIoResult copy_range(const std::shared_ptr<HostFile>& source,const std::shared_ptr<const std::vector<std::uint8_t>>& snapshot,
                        const std::shared_ptr<HostFile>& destination,std::uint64_t from,std::uint64_t to,std::uint32_t length,
                        std::uint32_t source_key,std::uint32_t destination_key,const std::atomic<bool>& cancel) {
    FileIoResult result;
    try {
        // 暫存大小不限制合法 DWORD 複製長度；每段仍通過共用檔案存取與範圍鎖。
        std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(length,1024U*1024U));
        while(result.bytes<length){
            if(cancel.load(std::memory_order_relaxed)){result.error=995;break;}
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),length-result.bytes));auto part=std::span(bytes).first(count);
            FileIoResult read;
            if(source)read=source->read(part,from+result.bytes,source_key);
            else {const auto offset=std::min<std::uint64_t>(from+result.bytes,snapshot->size());read.bytes=std::min<std::uint64_t>(count,snapshot->size()-offset);std::copy_n(snapshot->begin()+static_cast<std::ptrdiff_t>(offset),static_cast<std::size_t>(read.bytes),part.begin());process_io::record(process_io::Kind::Read,read.bytes);}
            if(read.error){result.error=read.error;break;}
            if(!read.bytes){if(!result.bytes)result.error=38;break;}
            if(cancel.load(std::memory_order_relaxed)){result.error=995;break;}
            const auto written=destination->write(part.first(static_cast<std::size_t>(read.bytes)),to+result.bytes,destination_key);
            result.bytes+=written.bytes;result.error=written.error;
            if(result.error || written.bytes!=read.bytes){if(!result.error)result.error=112;break;}
            if(read.bytes<count)break;
        }
    }catch(const std::bad_alloc&){result.error=8;}catch(const std::exception&){result.error=1117;}
    return result;
}
}
FileVolumeLocation WindowsServices::file_volume_location(std::uint64_t handle) const {
    const auto file=files_.find(handle);if(file==files_.end())return {{},{},{},6};
    const auto& value=file->second;
    if(value.pipe || value.serial || value.hid || value.usb || (value.native && value.native->null_device()))return {{},{},{},1};
    const auto identity=value.native?value.native:value.identity;
    // 已開啟物件的卷身分不受後續磁碟別名／路徑異動影響。
    if(identity && identity->volume_id())return {identity->volume_id(),{},{},0};
    if(identity){
        // 繼承的宿主 fd 由實際物件重新定位；不拿可能已改指向的開檔名稱推測卷。
        const auto current=filesystem_.final_path(*identity,value.path.empty()?value.key:value.path);
        if(!current.error){const auto location=filesystem_.volume_location(current.key,true);if(!location.error)return location;}
        return {0,{},{},0}; // 沒有持久卷的唯讀來源仍保留既有查詢資訊。
    }
    return filesystem_.volume_location(value.path.empty()?value.key:value.path,true);
}
std::optional<std::uint32_t> WindowsServices::invoke_set_volume(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t status){threads_.items.at(threads_.current).last_native_status=status;arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    try {
        memory.check(GuestAddress{arg(1)},2*unit,Access::Write);
        const auto found=files_.find(arg(0));if(found==files_.end())return finish(0xc0000008U);
        const auto kind=static_cast<std::uint32_t>(arg(4)),size=static_cast<std::uint32_t>(arg(3));
        if(kind!=2 && kind!=6 && kind!=8)return finish(0xc0000003U);
        if(size<(kind==2?8U:kind==6?48U:64U))return finish(0xc0000004U);
        if(!(found->second.access&0x40000002U))return finish(0xc0000022U);
        const auto location=file_volume_location(arg(0));if(location.error)return finish(file_io_status(location.error));
        if(!location.volume)return finish(0xc0000010U);
        // 此檔案系統未宣告 FILE_VOLUME_QUOTAS；不能只保存旗標冒充配額強制執行。
        if(kind==6)return finish(0xc0000010U);
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許卷屬性宿主寫入");
        std::uint32_t error=0;
        if(kind==2){
            const auto length=memory.load(GuestAddress{arg(2)},32);
            if(length>64)return finish(0xc0000086U);
            if((length&1U) || length>size-4)return finish(0xc000000dU);
            std::u16string label;for(std::uint64_t i=0;i<length;i+=2)label.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(2)+4+i},16)));
            error=filesystem_.set_volume_label(location.volume,std::move(label));if(error==123)error=154;
        }else {std::array<std::uint8_t,64> object{};memory.read(GuestAddress{arg(2)},object);error=filesystem_.set_volume_object_id(location.volume,object);}
        const auto status=file_io_status(error);memory.store(GuestAddress{arg(1)},bits,status);memory.store(GuestAddress{arg(1)+unit},bits,0);return finish(status);
    }catch(const MemoryFault&){return finish(0xc0000005U);}catch(const std::bad_alloc&){return finish(0xc0000017U);}
}
std::optional<std::uint32_t> WindowsServices::invoke_file_copy_chunk(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t status){threads_.items.at(threads_.current).last_native_status=status;arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    try {
        const auto previous=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.copy && io.blocking && io.thread==threads_.current && io.native_iosb==arg(3);});
        if(previous!=pending_file_io_.end()){
            if(!previous->done)throw HostCallFault("檔案複製尚未完成卻恢復執行緒");
            const auto status=file_io_status(previous->result.error);pending_file_io_.erase(previous);threads_.items.at(threads_.current).wait={};return finish(status);
        }
        memory.check(GuestAddress{arg(3)},2*unit,Access::Write);
        if(static_cast<std::uint32_t>(arg(9)))return finish(0xc000000dU);
        const auto source=files_.find(arg(0)),destination=files_.find(arg(1));
        if(source==files_.end() || destination==files_.end())return finish(0xc0000008U);
        const auto disk=[](const FileHandle& f){return !f.pipe && !f.serial && !f.hid && !f.usb && !(f.metadata.attributes&0x10U) && !(f.native && f.native->null_device());};
        if(!disk(source->second) || !disk(destination->second))return finish(0xc0000010U);
        if(!(source->second.access&0x80000001U) || !(destination->second.access&0x40000002U) || !destination->second.native)return finish(0xc0000022U);
        const auto from=memory.load(GuestAddress{arg(5)},64),to=memory.load(GuestAddress{arg(6)},64);const auto length=static_cast<std::uint32_t>(arg(4));
        if(from>INT64_MAX || to>INT64_MAX || length>INT64_MAX-from || length>INT64_MAX-to)return finish(0xc000000dU);
        const auto source_key=arg(7)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(7)},32)):0U;
        const auto destination_key=arg(8)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(8)},32)):0U;
        if(arg(2) && (!runtime_.wait_objects.contains(arg(2)) || runtime_.wait_objects.at(arg(2)).kind!=WaitObject::Kind::Event))return finish(0xc0000008U);
        const auto& a=source->second;const auto& b=destination->second;
        for(const auto& [file,offset]:{std::pair{&a,from},std::pair{&b,to}})if(file->mode&8U){
            const auto space=file->native?file->native->volume_space():FileVolumeSpace{};
            if(space.error)return finish(file_io_status(space.error));
            if(!space.sector_size || offset%space.sector_size || length%space.sector_size)return finish(0xc000000dU);
        }
        if(length && a.metadata.device==b.metadata.device && a.metadata.id==b.metadata.id && from<to+length && to<from+length)return finish(0xc000000dU);
        for(const auto& [file,offset,writing,key]:{std::tuple{a.native,from,false,source_key},std::tuple{b.native,to,true,destination_key}}){
            if(file){const auto error=file->validate_io(offset,length,writing,key);if(error)return finish(file_io_status(error));}
        }
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許檔案區塊複製宿主寫入");
        if(pending_file_io_.size()>=1024)return finish(0xc0000017U);
        if(std::any_of(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.native_iosb==arg(3) && !io.done;}))return finish(0xc000000dU);
        pending_file_io_.reserve(pending_file_io_.size()+1);
        PendingFileIo io;io.kind=PendingFileIo::Kind::Copy;io.handle=arg(1);io.source_handle=arg(0);io.thread=threads_.current;io.native_iosb=arg(3);io.event=arg(2);io.offset=to;io.source_offset=from;
        io.blocking=(b.mode&0x30U)!=0;
        auto task=std::make_shared<AsyncFileCopy>();io.copy=task;
        task->completion_port=b.completion_port;task->completion_key=b.completion_key;task->completion_modes=b.completion_modes;
        auto run=[task=task.get(),native=a.native,snapshot=a.bytes,target=b.native,from,to,length,source_key,destination_key,wake=mapping_wake()]{
            process_io::Scope accounting(task->accounting);process_io::Batch reads(process_io::Kind::Read),writes(process_io::Kind::Write);
            task->result=copy_range(native,snapshot,target,from,to,length,source_key,destination_key,task->cancel);
            task->done.store(true,std::memory_order_release);if(wake)wake->notify();
        };
        // 同步來源可在呼叫內完成；非同步來源交給宿主要求，目的同步模式決定是否等待。
        // 兩種完成方式都只發布一次整筆複製的事件與目的完成埠。
        if(a.mode&0x30U)run();else task->worker=std::thread(std::move(run));
        memory.store(GuestAddress{arg(3)},bits,0x103U);memory.store(GuestAddress{arg(3)+unit},bits,0);
        if(io.event)runtime_.wait_objects.at(io.event).set_signal(false);set_file_io_signaled(io.handle,false);
        pending_file_io_.push_back(io);poll_file_io(memory);
        auto current=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& p){return p.copy==task;});
        if(current==pending_file_io_.end())throw HostCallFault("檔案複製缺少共用完成要求");
        if(current->done){const auto status=file_io_status(current->result.error);pending_file_io_.erase(current);return finish(status);}
        if(!io.blocking){current->notify=true;return finish(0x103U);}
        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::FileIo;thread.wait.handles={io.handle};schedule(state,memory);return std::nullopt;
    }catch(const MemoryFault&){return finish(0xc0000005U);}catch(const std::bad_alloc&){return finish(0xc0000017U);}catch(const std::system_error&){return finish(0xc000009aU);}
}
std::optional<std::uint32_t> WindowsServices::invoke_fs_control(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const bool native=service.symbol=="NtFsControlFile";
    const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto code=static_cast<std::uint32_t>(arg(native?5:1)),input_size=static_cast<std::uint32_t>(arg(native?7:3)),output_size=static_cast<std::uint32_t>(arg(native?9:5));
    const auto input=arg(native?6:2),output=arg(native?8:4),iosb=native?arg(4):0,overlap=native?0:arg(7);
    const auto finish=[&](std::uint32_t status,std::uint32_t error){
        if(native)threads_.items.at(threads_.current).last_native_status=status;else if(error)last_error_=error;
        arg.return_to(state,native?status:error?0U:1U);return std::optional<std::uint32_t>{};
    };
    const auto fail=[&](std::uint32_t error){return finish(file_io_status(error),error);};
    try {
        const auto previous=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.blocking && io.thread==threads_.current && io.handle==arg(0) && io.native_iosb==iosb && io.overlapped==overlap && (io.kind==PendingFileIo::Kind::Connect || io.kind==PendingFileIo::Kind::Transact);});
        if(previous!=pending_file_io_.end()){
            if(!previous->done)throw HostCallFault("檔案系統控制尚未完成卻恢復執行緒");
            const auto error=previous->result.error;pending_file_io_.erase(previous);threads_.items.at(threads_.current).wait={};return fail(error);
        }
        if(native)memory.check(GuestAddress{iosb},unit*2,Access::Write);
        else {if(!arg(6) && !overlap)return fail(87);if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);if(overlap)memory.check(GuestAddress{overlap},x86_?20U:32U,Access::Write);}
        const auto found=files_.find(arg(0));if(found==files_.end())return fail(6);auto& file=found->second;
        const auto access=(code>>14)&3U;
        if(((access&1U) && !(file.access&0x80000001U)) || ((access&2U) && !(file.access&0x40000002U)))return fail(5);
        PendingFileIo io;io.handle=arg(0);io.thread=threads_.current;io.native_iosb=iosb;io.overlapped=overlap;
        io.count_pointer=native?0:arg(6);io.event=native?arg(1):overlap?memory.load(GuestAddress{overlap+(x86_?16U:24U)},bits):0;
        io.routine=native?arg(2):0;io.native_context=native?arg(3):0;io.blocking=native?(file.mode&0x30U)!=0:!overlap;
        const auto event=native?io.event:io.event&~UINT64_C(1);
        if(event && (!runtime_.wait_objects.contains(event) || runtime_.wait_objects.at(event).kind!=WaitObject::Kind::Event))return fail(6);
        if(io.routine && file.completion_port)return fail(87);
        if(io.routine && threads_.items.at(threads_.current).apcs.size()>=1024)return fail(8);
        if(std::any_of(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& p){return !p.done && ((iosb && p.native_iosb==iosb) || (overlap && p.overlapped==overlap));}))return fail(87);
        if(output_size)memory.check(GuestAddress{output},output_size,Access::Write);
        if(input_size)memory.check(GuestAddress{input},input_size,Access::Read);
        if(file.pipe && (code==0x110008U || code==0x11c017U)){
            if(pending_file_io_.size()>=1024)return fail(8);
            if(code==0x11c017U && (!(file.pipe->information().mode&4U) || (file.pipe->server() && !(file.pipe_mode&2U))))return fail(87);
            io.kind=code==0x110008U?PendingFileIo::Kind::Connect:PendingFileIo::Kind::Transact;io.input=input;io.input_size=input_size;io.output=output;io.output_size=output_size;
            io.immediate=(file.pipe_mode&1U)!=0;
            if(event)runtime_.wait_objects.at(event).set_signal(false);set_file_io_signaled(io.handle,false);
            const auto block=native?iosb:overlap;if(block){memory.store(GuestAddress{block},bits,0x103U);memory.store(GuestAddress{block+unit},bits,0);}
            if(io.count_pointer)memory.store(GuestAddress{io.count_pointer},32,0);
            pending_file_io_.push_back(io);poll_file_io(memory);
            const auto current=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& p){return p.handle==io.handle && p.thread==io.thread && p.native_iosb==iosb && p.overlapped==overlap && p.kind==io.kind;});
            if(current==pending_file_io_.end())throw HostCallFault("檔案系統控制缺少共用完成要求");
            if(current->done){const auto error=current->result.error;pending_file_io_.erase(current);return fail(error);}
            if(!io.blocking){current->notify=true;return finish(0x103U,997);}
            auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::FileIo;thread.wait.handles={io.handle};schedule(state,memory);return std::nullopt;
        }
        std::vector<std::uint8_t> bytes;std::uint32_t error=0;bool truncated=false;std::uint32_t required=0;
        if(file.pipe){
            if(code==0x110004U){error=file.pipe->disconnect();if(!error)cancel_file_io(memory,arg(0));}
            else if(code==0x11400cU){
                if(output_size<16)error=122;
                else {bytes.resize(output_size);PipePeekInfo info;const auto result=file.pipe->peek(std::span(bytes).subspan(16),info);error=result.error;
                    if(error)bytes.clear();else {const std::array values{info.state,info.available,info.messages,info.message_length};for(unsigned i=0;i<4;++i)for(unsigned n=0;n<4;++n)bytes[4*i+n]=static_cast<std::uint8_t>(values[i]>>(8*n));bytes.resize(16+result.bytes);truncated=(info.messages?info.message_length:info.available)>result.bytes;}
                }
            }
            else error=1;
        }else if(file.serial || file.hid || file.usb || (file.native && file.native->null_device()))error=1;
        else if(code==0x900a8U){ // FSCTL_GET_REPARSE_POINT
            const auto identity=file.native?file.native:file.identity;
            const auto current=identity?filesystem_.final_path(*identity,file.path.empty()?file.key:file.path):FileInformation{{},{},6};
            const auto point=current.error?FileReparse{}:filesystem_.reparse(current.key);error=current.error?current.error:point.error;
            if(!error && (point.device!=file.metadata.device || point.id!=file.metadata.id))error=5;
            if(!error){
                auto print=utf8_to_utf16(point.target);std::replace(print.begin(),print.end(),u'/',u'\\');auto substitute=print;
                const bool relative=print.find(u':')==print.npos && !print.starts_with(u"\\\\");
                if(print.starts_with(u"\\\\?\\"))substitute=u"\\??\\"+print.substr(4);
                else if(!relative)substitute=print.starts_with(u"\\\\")?u"\\??\\UNC\\"+print.substr(2):u"\\??\\"+print;
                const auto sub=utf16_bytes(substitute),display=utf16_bytes(print);const unsigned header=point.kind==3?16U:20U;
                bytes.resize(header);const auto put=[&](unsigned at,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                put(0,4,point.kind==3?0xa0000003U:0xa000000cU);put(4,2,header-8+sub.size()+display.size());
                put(10,2,sub.size()-2);put(12,2,sub.size());put(14,2,display.size()-2);if(point.kind!=3)put(16,4,relative?1U:0U);
                bytes.insert(bytes.end(),sub.begin(),sub.end());bytes.insert(bytes.end(),display.begin(),display.end());
                if(output_size<header){required=static_cast<std::uint32_t>(bytes.size());bytes.clear();error=122;}
                else if(output_size<bytes.size()){bytes.resize(output_size);truncated=true;}
            }
        }else if(code==0x900a4U || code==0x900acU){ // 掛載點設定／刪除共用現有卷掛載交易。
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許重新解析點宿主寫入");
            if(!(file.access&0x40000002U))error=5;
            else if(input_size<8)error=4393;
            else {
                const auto tag=memory.load(GuestAddress{input},32),length=memory.load(GuestAddress{input+4},16);
                const auto current=file.native?filesystem_.final_path(*file.native,file.path.empty()?file.key:file.path):FileInformation{{},{},5};const auto& path=current.key;
                if(current.error)error=current.error;
                else if(tag!=0xa0000003U)error=50;
                else if(code==0x900acU){const auto point=filesystem_.reparse(path);error=point.error;if(!error && point.kind!=3)error=4394;if(!error && length)error=4393;if(!error)error=filesystem_.delete_volume_mount(path);}
                else if(input_size<16 || length<8 || length>input_size-8)error=4393;
                else {
                    const auto offset=memory.load(GuestAddress{input+8},16),size=memory.load(GuestAddress{input+10},16);
                    const auto print_offset=memory.load(GuestAddress{input+12},16),print_size=memory.load(GuestAddress{input+14},16);
                    if((offset|size|print_offset|print_size)&1U || !size || offset>length-8 || size>length-8-offset || print_offset>length-8 || print_size>length-8-print_offset)error=4393;
                    else {std::u16string target;for(std::uint64_t i=0;i<size;i+=2)target.push_back(static_cast<char16_t>(memory.load(GuestAddress{input+16+offset+i},16)));
                        if(target.starts_with(u"\\??\\"))target=u"\\\\?\\"+target.substr(4);
                        const auto volume=windows_volume_letter(std::u16string_view{target});
                        if(!volume)error=4393;else error=filesystem_.set_volume_mount(path,volume);
                    }
                }
            }
        }else if(code==0x9003cU){ // 檔案系統未宣告壓縮；查詢目前實際未壓縮的狀態。
            if(output_size<2)error=122;else bytes.resize(2);
        }else error=1;
        if(event)runtime_.wait_objects.at(event).set_signal(false);set_file_io_signaled(io.handle,false);
        if(!bytes.empty())memory.write(GuestAddress{output},bytes);
        io.result={required?required:static_cast<std::uint32_t>(bytes.size()),truncated?234U:error};
        const auto status=file_io_status(io.result.error);complete_file_io(memory,io,status);return finish(status,io.result.error);
    }catch(const MemoryFault&){return fail(998);}catch(const std::invalid_argument&){return fail(87);}catch(const std::bad_alloc&){return fail(8);}
}
}
