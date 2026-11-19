// IoRing（kernelbase，api-ms-win-core-ioring）：使用者模式模擬（IORING_FEATURE_UM_EMULATION），由 windows_kernelbase_extras.cpp 引入。
// Build* 只把作業放進提交佇列；SubmitIoRing 依序同步執行（讀寫用檔案 handle 的定位 I/O，不改變檔案指標）並把結果放進完成佇列，
// 有完成項目且設定了完成事件時觸發事件。IORING_HANDLE_REF／IORING_BUFFER_REF 以值傳遞：x64 由呼叫者傳指向 16 位元組複本的指標；
// x86 分別佔 2／3 個堆疊格，描述元以 wide 位元湊出正確的清除格數，引數在 x86 改從堆疊直接解析。
if(service.symbol.find("IoRing")!=std::string_view::npos){
    const WindowsCall call(state,memory,service);const auto& name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(call(i));};
    const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    constexpr std::uint32_t invalid=0x80070057U,pointer=0x80004003U,required_unsupported=0x80460001U,queue_full=0x80460002U,version_unsupported=0x80460003U,
        submission_too_big=0x80460004U,completion_too_big=0x80460005U,max_submission=32768,max_completion=65536,version3=300;
    const auto slot=[&](unsigned index){return memory.load(GuestAddress{state.reg(Gpr::Rsp)+4+std::uint64_t{index}*4},32);};
    try{
    if(name=="QueryIoRingCapabilities"){
        // IORING_CAPABILITIES：最高版本 IORING_VERSION_3、佇列上限、使用者模式模擬與完成事件。
        if(!call(0))return done(pointer);memory.check(GuestAddress{call(0)},16,Access::Write);
        for(const auto [offset,value]:{std::pair{0U,version3},std::pair{4U,max_submission},std::pair{8U,max_completion},std::pair{12U,3U}})memory.store(GuestAddress{call(0)+offset},32,value);
        return done(0);
    }
    if(name=="CreateIoRing"){
        // (ioringVersion, IORING_CREATE_FLAGS {Required, Advisory}（8 位元組，以值傳遞）, submissionQueueSize, completionQueueSize, HIORING*)：大小進位到 2 的冪次。
        if(!call(4))return done(pointer);memory.check(GuestAddress{call(4)},unit,Access::Write);memory.store(GuestAddress{call(4)},bits,0);
        const auto version=u32(0);const auto flags=call(1);
        if(version!=1 && version!=2 && version!=version3)return done(version_unsupported);if(static_cast<std::uint32_t>(flags))return done(required_unsupported);
        if(!u32(2) || u32(2)>max_submission)return done(submission_too_big);if(u32(3)>max_completion)return done(completion_too_big);
        const auto round=[](std::uint32_t value){std::uint32_t result=1;while(result<value)result<<=1;return result;};
        if(io_rings_.size()>=1024)return done(0x8007000eU);
        IoRing ring;ring.version=version;ring.advisory=static_cast<std::uint32_t>(flags>>32);ring.submission=round(u32(2));ring.completion=round(std::max(u32(3),u32(2)));
        const auto handle=gui_.next_object++;io_rings_.emplace(handle,std::move(ring));memory.store(GuestAddress{call(4)},bits,handle);return done(0);
    }
    const auto found=io_rings_.find(call(0));
    if(name=="IsIoRingOpSupported"){
        // NOP、READ、REGISTER_FILES、REGISTER_BUFFERS、CANCEL；IORING_VERSION_3 起加上 WRITE、FLUSH；分散／集中讀寫（版本 4）不支援。
        if(found==io_rings_.end())return done(0);const auto op=u32(1);return done(op<=4 || ((op==5 || op==6) && found->second.version>=version3)?1:0);
    }
    if(found==io_rings_.end())return done(invalid);
    auto& ring=found->second;
    if(name=="CloseIoRing"){io_rings_.erase(found);return done(0);}
    if(name=="GetIoRingInfo"){
        if(!call(1))return done(pointer);memory.check(GuestAddress{call(1)},20,Access::Write);
        for(const auto [offset,value]:{std::pair{0U,ring.version},std::pair{4U,ring.required},std::pair{8U,ring.advisory},std::pair{12U,ring.submission},std::pair{16U,ring.completion}})
            memory.store(GuestAddress{call(1)+offset},32,value);
        return done(0);
    }
    if(name=="SetIoRingCompletionEvent"){ring.event=call(1);return done(0);}
    if(name=="PopIoRingCompletion"){
        // IORING_CQE {UserData, ResultCode, Information}；沒有完成項目時回 S_FALSE。
        if(!call(1))return done(pointer);memory.check(GuestAddress{call(1)},unit*3,Access::Write);if(ring.completions.empty())return done(1);
        const auto item=ring.completions.front();ring.completions.erase(ring.completions.begin());
        memory.store(GuestAddress{call(1)},bits,item.user_data);memory.store(GuestAddress{call(1)+unit},32,item.result);memory.store(GuestAddress{call(1)+unit*2},bits,item.information);return done(0);
    }
    if(name.starts_with("Build")){
        // 該版本不支援的作業（寫入、清除需要 IORING_VERSION_3）屬參數錯誤，先於佇列是否已滿判斷。
        if((name=="BuildIoRingWriteFile" || name=="BuildIoRingFlushFile") && ring.version<version3)return done(invalid);
        if(ring.pending.size()>=ring.submission)return done(queue_full);
        IoRingEntry entry;std::uint32_t sqe_flags=0;const bool check=!(ring.advisory&1U);
        const auto handle_ref=[&](unsigned index,unsigned x86_slot){
            if(x86_){entry.file_kind=static_cast<std::uint32_t>(slot(x86_slot));entry.file=slot(x86_slot+1);}
            else{const auto at=call(index);entry.file_kind=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));entry.file=memory.load(GuestAddress{at+8},64);}};
        const auto buffer_ref=[&](unsigned index,unsigned x86_slot){
            if(x86_){entry.buffer_kind=static_cast<std::uint32_t>(slot(x86_slot));entry.buffer=slot(x86_slot+1);entry.buffer_index=static_cast<std::uint32_t>(slot(x86_slot+1));entry.buffer_offset=static_cast<std::uint32_t>(slot(x86_slot+2));}
            else{const auto at=call(index);entry.buffer_kind=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));entry.buffer=memory.load(GuestAddress{at+8},64);
                entry.buffer_index=static_cast<std::uint32_t>(memory.load(GuestAddress{at+8},32));entry.buffer_offset=static_cast<std::uint32_t>(memory.load(GuestAddress{at+12},32));}};
        if(name=="BuildIoRingReadFile" || name=="BuildIoRingWriteFile"){
            const bool write=name=="BuildIoRingWriteFile";if(write && ring.version<version3)return done(invalid);
            entry.op=write?5U:1U;handle_ref(1,1);buffer_ref(2,3);
            if(x86_){entry.bytes=static_cast<std::uint32_t>(slot(6));entry.offset=slot(7)|(slot(8)<<32);
                if(write){entry.flags=static_cast<std::uint32_t>(slot(9));entry.user_data=slot(10);sqe_flags=static_cast<std::uint32_t>(slot(11));}else{entry.user_data=slot(9);sqe_flags=static_cast<std::uint32_t>(slot(10));}}
            else{entry.bytes=u32(3);entry.offset=call(4);if(write){entry.flags=u32(5);entry.user_data=call(6);sqe_flags=u32(7);}else{entry.user_data=call(5);sqe_flags=u32(6);}}
            if(check && (entry.buffer_kind>1 || (!entry.buffer_kind && !entry.buffer)))return done(invalid);
        }else if(name=="BuildIoRingFlushFile"){
            if(ring.version<version3)return done(invalid);entry.op=6;
            if(x86_){entry.file_kind=static_cast<std::uint32_t>(call(1));entry.file=call(1)>>32;}else handle_ref(1,0);
            entry.flags=u32(2);entry.user_data=call(3);sqe_flags=u32(4);
        }else if(name=="BuildIoRingCancelRequest"){
            entry.op=4;if(x86_){entry.file_kind=static_cast<std::uint32_t>(call(1));entry.file=call(1)>>32;}else handle_ref(1,0);entry.target=call(2);entry.user_data=call(3);
        }else{
            // RegisterFileHandles(ring, count, HANDLE[], userData)／RegisterBuffers(ring, count, IORING_BUFFER_INFO[], userData)：陣列在提交時讀取。
            entry.op=name=="BuildIoRingRegisterFileHandles"?2U:3U;entry.count=u32(1);entry.buffer=call(2);entry.user_data=call(3);
            if(check && (entry.count>65536 || (entry.count && !entry.buffer)))return done(invalid);
        }
        if(check && (entry.file_kind>1 || (sqe_flags&~1U)))return done(invalid);
        ring.pending.push_back(entry);return done(0);
    }
    // SubmitIoRing(ring, waitOperations, milliseconds, submittedEntries)：全部同步完成，等待條件在返回前即已滿足。
    // 清除作業與完成事件經客體入口（FlushFileBuffers／SetEvent），因此以協程執行。
    // 協程之前只做純解析：nested_continuation 完成後會再進入一次這段程式碼，不能有副作用。
    const auto ring_handle=call(0);
    return nested_continuation(state,memory,"SubmitIoRing",[this,service,ring_handle](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall frame(s,m,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto hr=[](std::uint32_t error){return error?0x80070000U|(error&0xffffU):0U;};constexpr std::uint32_t invalid=0x80070057U;
        if(frame(3)){m.check(GuestAddress{frame(3)},4,Access::Write);m.store(GuestAddress{frame(3)},32,0);}
        const auto kernel=[&](std::string_view symbol){return resolve("kernel32.dll",symbol).value;};
        const auto ring=[&]()->IoRing*{const auto found=io_rings_.find(ring_handle);return found==io_rings_.end()?nullptr:&found->second;};
        // 檔案 handle 與緩衝區位址（原始或註冊參考）。
        const auto resolve_file=[&](const IoRingEntry& entry,std::uint64_t& handle)->std::uint32_t{
            handle=entry.file;if(entry.file_kind==1){if(entry.file>=ring()->files.size())return invalid;handle=ring()->files[entry.file];}else if(entry.file_kind)return invalid;return 0;};
        const auto execute=[&](const IoRingEntry& entry,std::uint64_t& information)->std::uint32_t{
            if(entry.op==2 || entry.op==3){
                const unsigned stride=entry.op==2?unit:unit*2;m.check(GuestAddress{entry.buffer},std::size_t{entry.count}*stride,Access::Read);auto& target=*ring();
                if(entry.op==2){target.files.clear();for(std::uint32_t i=0;i<entry.count;++i)target.files.push_back(m.load(GuestAddress{entry.buffer+std::uint64_t{i}*stride},bits));}
                else{target.buffers.clear();for(std::uint32_t i=0;i<entry.count;++i){const auto at=entry.buffer+std::uint64_t{i}*stride;target.buffers.emplace_back(m.load(GuestAddress{at},bits),static_cast<std::uint32_t>(m.load(GuestAddress{at+unit},32)));}}
                return 0;
            }
            if(entry.op==4)return hr(1168); // 先前的作業都已同步完成，沒有可取消的項目。
            std::uint64_t handle=0;if(const auto error=resolve_file(entry,handle))return error;
            std::uint64_t address=entry.buffer;
            if(entry.buffer_kind==1){if(entry.buffer_index>=ring()->buffers.size())return invalid;const auto& buffer=ring()->buffers[entry.buffer_index];
                if(std::uint64_t{entry.buffer_offset}+entry.bytes>buffer.second)return invalid;address=buffer.first+entry.buffer_offset;}else if(entry.buffer_kind)return invalid;
            const auto file=files_.find(handle);if(file==files_.end())return hr(6);const auto& target=file->second;
            if(target.pipe || target.serial || target.hid || target.usb)return hr(50);if(target.metadata.attributes&0x10)return hr(5);
            constexpr std::uint64_t chunk=1U<<20;std::vector<std::uint8_t> data;std::uint64_t moved=0;
            if(entry.op==1){
                if(!(target.access&(0x80000000U|1U)))return hr(5);m.check(GuestAddress{address},entry.bytes,Access::Write);
                while(moved<entry.bytes){data.resize(static_cast<std::size_t>(std::min<std::uint64_t>(chunk,entry.bytes-moved)));std::uint64_t count=0;
                    if(target.native){const auto result=target.native->read(data,entry.offset+moved);if(result.error)return hr(result.error);count=result.bytes;}
                    else if(target.bytes){const auto size=target.bytes->size();if(entry.offset+moved<size){count=std::min<std::uint64_t>(data.size(),size-(entry.offset+moved));
                        std::copy_n(target.bytes->begin()+static_cast<std::ptrdiff_t>(entry.offset+moved),static_cast<std::ptrdiff_t>(count),data.begin());}}
                    if(!count)break;m.write(GuestAddress{address+moved},std::span(data).first(static_cast<std::size_t>(count)));moved+=count;if(count<data.size())break;}
                if(!moved && entry.bytes)return hr(38); // ERROR_HANDLE_EOF
                information=moved;return 0;
            }
            if(!(target.access&(0x40000000U|2U|4U)) || !target.native)return hr(5);m.check(GuestAddress{address},entry.bytes,Access::Read);
            while(moved<entry.bytes){data.resize(static_cast<std::size_t>(std::min<std::uint64_t>(chunk,entry.bytes-moved)));m.read(GuestAddress{address+moved},data);
                const auto result=target.native->write(data,entry.offset+moved);if(result.error)return hr(result.error);moved+=result.bytes;if(result.bytes<data.size())break;}
            information=moved;return 0;
        };
        std::size_t processed=0;
        while(ring() && processed<ring()->pending.size() && ring()->completions.size()<ring()->completion){
            const auto entry=ring()->pending[processed];IoRingCompletion completion{entry.user_data,0,0};
            try{completion.result=entry.op==6?0U:execute(entry,completion.information);}catch(const MemoryFault&){completion.result=hr(998);completion.information=0;}
            // 清除作業，以及寫入帶 FILE_WRITE_FLAGS_WRITE_THROUGH 時，經 FlushFileBuffers 寫回儲存裝置。
            if((entry.op==6 || (entry.op==5 && (entry.flags&1U) && !completion.result))){std::uint64_t handle=0;
                if(const auto error=resolve_file(entry,handle))completion.result=error;
                else{const auto saved=last_error_;const auto ok=co_await service_callback::call(kernel("FlushFileBuffers"),{handle});const auto flush_error=last_error_;last_error_=saved;
                    if(!ok && entry.op==6)completion.result=hr(flush_error);}}
            if(!ring())break;ring()->completions.push_back(completion);++processed;
        }
        if(auto* target=ring()){target->pending.erase(target->pending.begin(),target->pending.begin()+static_cast<std::ptrdiff_t>(processed));
            if(target->event && processed){const auto saved=last_error_;(void)co_await service_callback::call(kernel("SetEvent"),{target->event});last_error_=saved;}}
        if(frame(3))m.store(GuestAddress{frame(3)},32,processed);
        frame.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
    }catch(const MemoryFault&){return done(pointer);}catch(const std::bad_alloc&){return done(0x8007000eU);}
}
