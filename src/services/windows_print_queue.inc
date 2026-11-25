// 列印佇列控制（由 windows_printing.cpp 的 invoke_spooler 引入；回 Win32 BOOL 與 LastError）：
// EnumJobs（第 1～3 級）、SetJob（命令與第 1、2 級的文件名稱、優先權、資料型別）、AddJob／ScheduleJob（應用程式直接寫暫存檔再排程）、
// GetSpoolFileHandle／CommitSpoolData／CloseSpoolFileHandle（StartDocPrinter 後以檔案控制代碼寫入暫存資料）、
// SetPrinter（暫停、恢復、清除、設定狀態；第 2 級註解／位置／優先權／預設 DEVMODE、第 6 級狀態、第 8、9 級預設 DEVMODE）、ResetPrinter、
// 工作控制代碼的 ReadPrinter／SeekPrinter、FreePrinterNotifyInfo、SpoolerInit。暫停的印表機或工作讓完成的工作留在佇列，恢復後輸出 PDF。
if(name.starts_with("EnumJobs")){
    // (hPrinter, FirstJob, NoJobs, Level, pJob, cbBuf, pcbNeeded, pcReturned)
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);
    if(!arg(6) || !arg(7))return fail(87);memory.store(GuestAddress{arg(7)},32,0);
    const auto level=d(3);if(level<1 || level>3)return fail(124);
    const auto jobs=all_jobs();const auto first=std::min<std::size_t>(d(1),jobs.size());const auto count=std::min<std::size_t>(d(2),jobs.size()-first);
    const unsigned size=level==1?(x86_?64:96):level==2?(x86_?104:160):12;
    PrinterBuffer buffer(static_cast<unsigned>(size*count),arg(4),x86_,wide);
    for(std::size_t i=0;i<count;++i){
        const auto& device=*jobs[first+i];const auto at=static_cast<unsigned>(i*size);
        if(level==3){buffer.put(at,device.job);buffer.put(at+4,first+i+1<jobs.size()?jobs[first+i+1]->job:0U);}
        else write_job(buffer,at,device,level,static_cast<unsigned>(first+i+1));
    }
    if(d(5)>=buffer.bytes.size() && (arg(4) || buffer.bytes.empty()))memory.store(GuestAddress{arg(7)},32,count);
    if(buffer.bytes.empty()){memory.store(GuestAddress{arg(6)},32,0);return finish(1);}
    return copy(buffer,arg(4),d(5),arg(6));
}
if(name.starts_with("SetJob")){
    // (hPrinter, JobId, Level, pJob, Command)：命令 1 暫停、2 恢復、3 取消、4 重新開始、5 刪除、6 已送至印表機、7 最後一頁已退出、8 保留、9 釋放。
    if(!printing_->handles.contains(arg(0)))return fail(6);
    auto* device=print_job(d(1));if(!device)return fail(87);
    const auto level=d(2),command=d(4);
    if(level>2)return fail(124);
    if(level && !arg(3))return fail(87);
    if(level){
        // JOB_INFO_1／2：pDocument、pDatatype、Priority 可改；Position 只接受目前位置或 0（JOB_POSITION_UNSPECIFIED）。
        const auto pointer=[&](unsigned index){return memory.load(GuestAddress{arg(3)+index*unit},width);};
        const unsigned fields=level==1?unit*7:unit*13;const auto priority=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+fields+4},32));
        if(priority && (priority<1 || priority>99))return fail(87);
        const auto type=pointer(level==1?5:6)?datatype(string(pointer(level==1?5:6))):std::u16string{};if(!type.empty() && type!=u"RAW" && type!=u"TEXT")return fail(1804);
        if(pointer(4))device->title=string(pointer(4));if(!type.empty())device->datatype=type;if(priority)device->priority=priority;
    }
    switch(command){
        case 0:break;
        case 1:device->status|=1U;break;
        case 2:device->status&=~1U;break;
        case 3:case 5:
            if(device->active){device->cancelled=true;device->status|=4U;}
            else for(auto it=printing_->queue.begin();it!=printing_->queue.end();++it)if(&*it==device){printing_->queue.erase(it);break;}
            printing_changed(0x400);return finish(1);
        case 4:device->status|=0x800U;break;
        case 6:case 7:device->status|=command==6?0x80U:0x10U;break;
        case 8:device->status|=0x2000U;break;
        case 9:device->status&=~0x2000U;break;
        default:return fail(87);
    }
    printing_changed(0x200);
    if(command==2)try{if(const auto error=drain())return fail(error);}catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}catch(const std::domain_error&){return fail(50);}
    return finish(1);
}
const auto spool_directory=[&]()->std::u16string{
    // 暫存檔放在沙箱 C 槽的 spool\PRINTERS；目錄已存在時 CreateDirectoryW 失敗可忽略。
    GuestScratch block(*this,memory,state);
    for(const auto folder:{u"C:\\Windows",u"C:\\Windows\\System32",u"C:\\Windows\\System32\\spool",u"C:\\Windows\\System32\\spool\\PRINTERS"}){
        const auto text=block.text(folder);if(!text)throw std::bad_alloc{};const auto saved=last_error_;(void)nested_service(state,memory,"kernel32.dll","CreateDirectoryW",{text,0});last_error_=saved;}
    return u"C:\\Windows\\System32\\spool\\PRINTERS\\";
};
const auto spool_name=[&](unsigned job){auto digits=std::to_string(job);while(digits.size()<5)digits.insert(digits.begin(),'0');return std::u16string(digits.begin(),digits.end())+u".SPL";};
const auto read_host=[&](const std::u16string& path,std::uint64_t offset,std::uint64_t limit,std::vector<std::uint8_t>& out)->unsigned{
    auto file=filesystem_.open_host(utf16_to_utf8(path),0x80000000U,3,0,7);if(file.error)return file.error;
    std::array<std::uint8_t,65536> chunk{};
    while(out.size()<limit){const auto want=static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(),limit-out.size()));const auto read=file.file->read(std::span(chunk).first(want),offset);
        if(read.error)return read.error;if(!read.bytes)break;out.insert(out.end(),chunk.begin(),chunk.begin()+static_cast<std::ptrdiff_t>(read.bytes));offset+=read.bytes;}
    return 0;
};
if(name.starts_with("AddJob")){
    // (hPrinter, Level, pData, cbBuf, pcbNeeded)：ADDJOB_INFO_1 {Path, JobId}；應用程式寫入 Path 後呼叫 ScheduleJob。
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& device=found->second;
    if(!(device.access&8U) || device.read_job)return fail(5);if(d(1)!=1)return fail(124);if(!arg(4))return fail(87);
    if(device.active)return fail(1906);if(!filesystem_.writable())return fail(5);if(printing_->available_job()>INT32_MAX)return fail(8);
    const auto job=printing_->next_job;const auto path=spool_directory()+spool_name(job);
    PrinterBuffer buffer(x86_?8:16,arg(2),x86_,wide);buffer.text(0,path);buffer.put(unit,job);
    memory.store(GuestAddress{arg(4)},32,buffer.bytes.size());if(d(3)<buffer.bytes.size())return fail(122);if(!arg(2))return fail(87);
    ++printing_->next_job;
    device.job=job;device.spool_path=path;device.title=u"";device.output="C:\\x64arm-print-"+std::to_string(::getpid())+"-"+std::to_string(job)+".pdf";device.datatype=device.default_datatype;
    device.submitted=runtime_.system_filetime();device.active=true;device.page=device.cancelled=device.explicit_pages=false;device.spool.clear();device.page_ends.clear();device.bytes=0;device.status=0;
    printing_changed(0x100);return copy(buffer,arg(2),d(3),arg(4));
}
if(name=="ScheduleJob"){
    // (hPrinter, JobId)：讀入 AddJob 的暫存檔並刪除；印表機或工作暫停時留在佇列。
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& device=found->second;
    if(!device.active || device.job!=d(1) || device.spool_path.empty())return fail(87);
    const auto path=device.spool_path;device.spool_path.clear();
    const auto finish_job=[&]()->std::optional<std::uint32_t>{
        GuestScratch block(*this,memory,state);const auto text=block.text(path);const auto saved=last_error_;if(text)(void)nested_service(state,memory,"kernel32.dll","DeleteFileW",{text});last_error_=saved;
        device.active=device.page=device.explicit_pages=false;device.spool.clear();device.page_ends.clear();device.bytes=0;printing_changed(0x400);return finish(1);};
    if(device.cancelled)return finish_job();
    try{
        std::vector<std::uint8_t> data;if(const auto error=read_host(path,0,128*1024*1024,data))return fail(error==2?1:error);
        device.spool=std::move(data);device.bytes=device.spool.size();
        if(printing_->paused || (device.status&1U)){auto held=device;held.active=false;printing_->queue.push_back(std::move(held));return finish_job();}
        if(const auto error=emit(device))return fail(error);
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}catch(const std::domain_error&){return fail(50);}
    return finish_job();
}
if(name=="GetSpoolFileHandle" || name=="CommitSpoolData" || name=="CloseSpoolFileHandle"){
    // GetSpoolFileHandle(hPrinter) 回暫存檔控制代碼；CommitSpoolData(hPrinter, hSpoolFile, cbCommit) 把新寫入的位元組加入工作並回同一控制代碼；
    // CloseSpoolFileHandle(hPrinter, hSpoolFile) 關閉。需要 StartDocPrinter 進行中的工作；失敗時前兩者回 INVALID_HANDLE_VALUE。
    const auto invalid=[&](unsigned error){last_error_=error;return finish(name=="CloseSpoolFileHandle"?0:(x86_?UINT32_MAX:UINT64_MAX));};
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return invalid(6);auto& device=found->second;
    if(!device.active || !device.spool_path.empty() || device.read_job)return invalid(1906);
    const auto path=spool_directory()+spool_name(device.job);
    if(name=="GetSpoolFileHandle"){
        if(device.spool_handle)return finish(device.spool_handle);
        GuestScratch block(*this,memory,state);const auto text=block.text(path);if(!text)return invalid(8);
        const auto handle=nested_service(state,memory,"kernel32.dll","CreateFileW",{text,0xc0000000U,3,0,2,0x80,0});
        if(handle==(x86_?UINT32_MAX:UINT64_MAX))return invalid(last_error_);
        device.spool_handle=handle;device.committed=0;return finish(handle);
    }
    if(!device.spool_handle || arg(1)!=device.spool_handle)return invalid(6);
    if(name=="CloseSpoolFileHandle"){(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{device.spool_handle});device.spool_handle=0;return finish(1);}
    if(device.cancelled)return invalid(63);
    try{
        std::vector<std::uint8_t> data;if(const auto error=read_host(path,device.committed,d(2),data))return invalid(error);
        if(data.size()<d(2))return invalid(87);if(data.size()>128*1024*1024-device.spool.size())return invalid(8);
        device.spool.insert(device.spool.end(),data.begin(),data.end());device.committed+=data.size();device.bytes=device.spool.size();printing_changed(0x800);
    }catch(const std::bad_alloc&){return invalid(8);}
    return finish(device.spool_handle);
}
if(name.starts_with("SetPrinter")){
    // (hPrinter, Level, pPrinter, Command)：命令 1 暫停、2 恢復、3 清除全部工作、4 設定狀態（pPrinter 指向 DWORD）；需要 PRINTER_ACCESS_ADMINISTER，
    // 第 9 級（使用者預設 DEVMODE）只需要 PRINTER_ACCESS_USE。
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);const auto& handle=found->second;
    if(handle.read_job)return fail(6);
    const auto level=d(1),command=d(3);const bool administer=(handle.access&4U)!=0;
    if(command){
        if(level)return fail(124);if(command>4)return fail(87);if(!administer)return fail(5);
        if(command==4){if(!arg(2))return fail(87);printing_->status=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));}
        else if(command==1)printing_->paused=true;
        else if(command==2){printing_->paused=false;try{if(const auto error=drain())return fail(error);}catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(13);}catch(const std::domain_error&){return fail(50);}}
        else{printing_->queue.clear();for(auto* list:{&printing_->devices,&printing_->handles})for(auto& [id,device]:*list){(void)id;if(device.active)device.cancelled=true;}printing_changed(0x400);}
        printing_changed(0x2);return finish(1);
    }
    if(!arg(2))return fail(87);
    const auto read_devmode=[&](std::uint64_t pointer)->unsigned{
        PrinterSettings settings=PrintingState::Device{};if(printing_->defaults)settings=*printing_->defaults;
        if(!pointer)return 87;if(const auto error=read_printer_settings(memory,pointer,wide,settings))return error;printing_->defaults=settings;return 0;};
    const auto pointer=[&](unsigned index){return memory.load(GuestAddress{arg(2)+index*unit},width);};
    if(level==2){
        if(!administer)return fail(5);
        const unsigned fields=unit*13;const auto priority=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+fields+4},32)),default_priority=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+fields+8},32));
        if(priority>99 || default_priority>99)return fail(87);
        if(pointer(1) && !printer_name(pointer(1)))return fail(1801);
        if(pointer(7))if(const auto error=read_devmode(pointer(7)))return fail(error);
        if(pointer(5))printing_->comment=string(pointer(5));if(pointer(6))printing_->location=string(pointer(6));
        if(priority)printing_->priority=priority;if(default_priority)printing_->default_priority=default_priority;
    }else if(level==6){if(!administer)return fail(5);printing_->status=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));}
    else if(level==8 || level==9){if(level==8 && !administer)return fail(5);if(const auto error=read_devmode(pointer(0)))return fail(error);}
    else return fail(124);
    printing_changed(0x2);return finish(1);
}
if(name.starts_with("ResetPrinter")){
    // (hPrinter, PRINTER_DEFAULTS*)：pDatatype／pDevMode 為 NULL 時不變，(LPTSTR)-1 與 (LPDEVMODE)-1 回到印表機預設；之後的 StartDocPrinter 使用新值。
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& device=found->second;
    if(!arg(1))return finish(1);memory.check(GuestAddress{arg(1)},unit*2,Access::Read);
    const auto minus_one=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
    const auto type_at=memory.load(GuestAddress{arg(1)},width),mode_at=memory.load(GuestAddress{arg(1)+unit},width);
    std::u16string type=device.default_datatype;
    if(type_at==minus_one)type=u"RAW";else if(type_at){type=datatype(string(type_at));if(type!=u"RAW" && type!=u"TEXT")return fail(1804);}
    PrinterSettings settings=device;
    if(mode_at==minus_one){settings=PrintingState::Device{};if(printing_->defaults)settings=*printing_->defaults;}
    else if(mode_at)if(const auto error=read_printer_settings(memory,mode_at,wide,settings))return fail(error);
    static_cast<PrinterSettings&>(device)=settings;device.default_datatype=type;if(!device.active)device.datatype=type;
    return finish(1);
}
if(name=="ReadPrinter"){
    // (hPrinter, pBuf, cbBuf, pNoBytesRead)：工作控制代碼讀取該工作的暫存資料；印表機控制代碼的 FILE: 連接埠沒有雙向資料。
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& handle=found->second;
    if(!arg(3))return fail(87);memory.store(GuestAddress{arg(3)},32,0);
    if(!handle.read_job)return fail(50);
    const auto* job=print_job(handle.read_job);if(!job)return fail(87);
    const auto available_bytes=job->spool.size()>handle.read_offset?job->spool.size()-handle.read_offset:0;const auto count=std::min<std::uint64_t>(available_bytes,d(2));
    if(count){if(!arg(1))return fail(87);memory.write(GuestAddress{arg(1)},std::span(job->spool).subspan(static_cast<std::size_t>(handle.read_offset),static_cast<std::size_t>(count)));}
    handle.read_offset+=count;memory.store(GuestAddress{arg(3)},32,count);return finish(1);
}
if(name=="SeekPrinter"){
    // (hPrinter, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER pliNewPointer, dwMoveMethod, bWrite)：工作控制代碼的讀取位置。
    const auto found=printing_->handles.find(arg(0));if(found==printing_->handles.end())return fail(6);auto& handle=found->second;
    if(!handle.read_job || d(4))return fail(50);const auto* job=print_job(handle.read_job);if(!job)return fail(87);
    const auto distance=static_cast<std::int64_t>(arg(1));const auto method=d(3);
    const std::int64_t base=method==0?0:method==1?static_cast<std::int64_t>(handle.read_offset):method==2?static_cast<std::int64_t>(job->spool.size()):-1;
    if(base<0 && method>2)return fail(87);
    const auto target=base+distance;if(target<0)return fail(131);
    handle.read_offset=static_cast<std::uint64_t>(target);if(arg(2))memory.store(GuestAddress{arg(2)},64,handle.read_offset);return finish(1);
}
if(name=="FreePrinterNotifyInfo"){
    if(!arg(0))return fail(87);if(!free_heap(memory,arg(0),state))return fail(87);return finish(1);
}
if(name=="SpoolerInit")return finish(1);
