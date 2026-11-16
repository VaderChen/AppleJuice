// 分散／集中 I/O（由 windows_nt_native.cpp 引入）：ReadFileScatter／WriteFileGather 與 NtReadFileScatter／NtWriteFileGather。
// 檔案須以 FILE_FLAG_NO_BUFFERING 開啟（Win32 版本另須 FILE_FLAG_OVERLAPPED），長度與位移須對齊磁區大小；FILE_SEGMENT_ELEMENT 陣列每項指向一個
// 4 KiB 對齊的頁面，依序讀寫整頁（最後一頁可不滿）。結果與 ReadFile／NtReadFile 共用 complete_file_io 發布：OVERLAPPED 或 IO_STATUS_BLOCK、
// 事件、APC 與完成埠封包；讀取位移已在檔尾時回報 ERROR_HANDLE_EOF／STATUS_END_OF_FILE。
std::optional<std::uint32_t> WindowsServices::invoke_file_segments(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const bool native=name.starts_with("Nt") || name.starts_with("Zw"),writing=name.ends_with("WriteFileGather");
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);constexpr std::uint64_t page=4096;
    // Win32：(File, Segments, Length, Reserved, OVERLAPPED*)；NT：(File, Event, ApcRoutine, ApcContext, IoStatusBlock, Segments, Length, ByteOffset*, Key*)。
    const auto handle=arg(0),segments=arg(native?5:1);const auto length=static_cast<std::uint32_t>(arg(native?6:2));
    const auto finish=[&](std::uint32_t error)->std::optional<std::uint32_t>{
        if(native)arg.return_to(state,error?file_io_status(error):0);else{last_error_=error;arg.return_to(state,error?0:1);}return std::nullopt;};
    const auto found=files_.find(handle);
    if(found==files_.end() || !found->second.native || found->second.pipe || found->second.serial || found->second.hid || found->second.usb)return finish(6);
    auto& file=found->second;
    if(!(file.mode&8U) || (!native && (file.mode&0x30U)))return finish(87);
    if(writing?!(file.access&(0x40000000U|2U|4U)):!(file.access&(0x80000000U|1U)))return finish(5);
    std::uint64_t offset=0,event=0,overlapped=0,iosb=0;std::uint32_t key=0;
    if(native){
        iosb=arg(4);if(!iosb || !arg(7))return finish(87);memory.check(GuestAddress{iosb},unit*2,Access::Write);offset=memory.load(GuestAddress{arg(7)},64);
        if(arg(8))key=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(8)},32));event=arg(1);
    }else{
        overlapped=arg(4);if(!overlapped || arg(3))return finish(87);memory.check(GuestAddress{overlapped},x86_?20:32,Access::Write);
        offset=memory.load(GuestAddress{overlapped+2*unit},64);event=memory.load(GuestAddress{overlapped+(x86_?16:24)},bits);
    }
    if(event && (!runtime_.wait_objects.contains(event&~UINT64_C(1)) || runtime_.wait_objects.at(event&~UINT64_C(1)).kind!=WaitObject::Kind::Event))return finish(6);
    // 無緩衝 I/O 的長度與位移以磁區為單位。
    const auto space=filesystem_.volume_space(file.path);const std::uint64_t sector=space.error || !space.sector_size?512:space.sector_size;
    if(length%sector || offset%sector)return finish(87);
    const auto pages=static_cast<std::size_t>((std::uint64_t{length}+page-1)/page);std::vector<std::uint64_t> buffers(pages);
    for(std::size_t i=0;i<pages;++i){buffers[i]=memory.load(GuestAddress{segments+i*8},64);if(!buffers[i] || buffers[i]%page)return finish(87);
        memory.check(GuestAddress{buffers[i]},static_cast<std::size_t>(std::min<std::uint64_t>(page,length-i*page)),writing?Access::Read:Access::Write);}
    if(native && event)runtime_.wait_objects.at(event).set_signal(false);
    set_file_io_signaled(handle,false);
    PipeResult result;
    if(const auto invalid=file.native->validate_io(offset,length,writing,key))result.error=invalid;
    else try{
        std::vector<std::uint8_t> buffer(page);
        for(std::size_t i=0;i<pages;++i){
            const auto chunk=static_cast<std::size_t>(std::min<std::uint64_t>(page,length-i*page));auto part=std::span(buffer).first(chunk);
            if(writing)memory.read(GuestAddress{buffers[i]},part);
            const auto io=writing?file.native->write(part,offset+result.bytes,key):file.native->read(part,offset+result.bytes,key);
            if(!writing && io.bytes)memory.write(GuestAddress{buffers[i]},part.first(static_cast<std::size_t>(io.bytes)));
            result.bytes+=static_cast<std::uint32_t>(io.bytes);result.error=io.error;if(io.error || io.bytes<chunk)break;
        }
        if(writing && result.bytes)file.metadata=file.native->information().metadata;
    }catch(const std::bad_alloc&){result.error=8;}catch(const MemoryFault&){result.error=998;}
    if(!writing && !result.error && !result.bytes && length)result.error=38;
    PendingFileIo io;io.kind=writing?PendingFileIo::Kind::Write:PendingFileIo::Kind::Read;io.handle=handle;io.thread=threads_.current;io.event=event;io.result=result;
    if(native){io.native_iosb=iosb;io.native_context=arg(3);io.routine=arg(2);}else io.overlapped=overlapped;
    complete_file_io(memory,io,file_io_status(result.error));
    if(native && io.routine && (!result.error || result.error==234))threads_.yield=true;
    return finish(result.error);
}
