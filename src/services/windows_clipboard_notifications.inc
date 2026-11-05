// 與視窗表共用映射命名空間及檔案生命期；不改既有 SharedWindowTable ABI。
GuiClipboardNoticeResult WindowsServices::clipboard_notice(ClipboardNoticeAction action,std::uint64_t window,
    std::uint64_t next,std::optional<std::uint64_t> native) {
    initialize_user_objects();initialize_shared_windows();
    const auto station=gui_.user_handles.at(gui_.station_handle).object;
    const auto apply=[&](GuiClipboardNoticeState& value){
        GuiClipboardNoticeResult result;
        const auto live_window=[&](std::uint64_t hwnd){return gui_.windows.contains(hwnd) || (shared_windows_ && shared_gui_window(hwnd,false).has_value());};
        if(value.viewer && !live_window(value.viewer))value.viewer=0;
        if(shared_windows_ && value.open_process && process_identity::query(value.open_process).gone({value.open_process,value.open_birth}))
            value.open_process=0,value.open_birth=value.open_thread=value.open_window=0;
        const bool owner=value.open_process==process_id() && value.open_thread==(action==ClipboardNoticeAction::Close && next?next:threads_.current);
        const auto observe=[&]{if(native){value.native_sequence=*native;value.native_known=true;}};
        switch(action){
        case ClipboardNoticeAction::Viewer:break;
        case ClipboardNoticeAction::AddViewer:result.previous=value.viewer;value.viewer=window;break;
        case ClipboardNoticeAction::RemoveViewer:
            if(value.viewer==window)value.viewer=next;
            else result.forward=value.viewer;
            break;
        case ClipboardNoticeAction::Open:
            if(value.open_process)result.error=5;
            else{value.open_process=process_id();value.open_birth=process_identity_birth();value.open_thread=threads_.current;value.open_window=window;}
            break;
        case ClipboardNoticeAction::Close:
            if(!owner)result.error=1418;
            else value.open_process=0,value.open_birth=value.open_thread=value.open_window=0;
            break;
        case ClipboardNoticeAction::Mutation:observe();++value.sequence;if(!value.sequence)++value.sequence;break;
        case ClipboardNoticeAction::Publish:observe();++value.publication;result.notify=true;break;
        case ClipboardNoticeAction::Poll:
            // 客體持有剪貼簿期間，原生 clear/set 只是一次交易中的中間狀態。
            if(!value.open_process && native){
                if(value.native_known && value.native_sequence!=*native){++value.sequence;if(!value.sequence)++value.sequence;++value.publication;result.notify=true;}
                observe();
            }
            break;
        }
        result.state=value;return result;
    };
#if !defined(_WIN32)
    if(shared_windows_){
        auto name=gui_.user_objects.at(station).name;std::uint64_t hash=UINT64_C(14695981039346656037);
        for(auto& ch:name){if(ch>=u'A' && ch<=u'Z')ch=static_cast<char16_t>(ch+32);hash^=ch;hash*=UINT64_C(1099511628211);}
        struct Record {std::uint32_t magic{},version{};std::array<char16_t,256> station{};GuiClipboardNoticeState state;};
        SharedFd file{open_window_file(shared_windows_->directory+"/clipboard-notices-"+std::to_string(hash))};FileLock lock(file.fd);
        struct stat info{};if(::fstat(file.fd,&info))throw HostCallFault("無法讀取剪貼簿通知狀態");
        Record record{};
        if(!info.st_size){record.magic=0x4e434a41;record.version=1;std::copy(name.begin(),name.end(),record.station.begin());}
        else{
            ssize_t count;do{count=::pread(file.fd,&record,sizeof(record),0);}while(count<0 && errno==EINTR);
            if(info.st_size!=sizeof(record) || count!=sizeof(record) || record.magic!=0x4e434a41 || record.version!=1 ||
                !std::equal(name.begin(),name.end(),record.station.begin()) || record.station[name.size()])throw HostCallFault("剪貼簿通知版本或 window station 不符");
        }
        const auto result=apply(record.state);ssize_t count;
        do{count=::pwrite(file.fd,&record,sizeof(record),0);}while(count<0 && errno==EINTR);
        if(count!=sizeof(record))throw HostCallFault("無法保存剪貼簿通知狀態");
        gui_.clipboard_notices=result.state;return result;
    }
#endif
    return apply(gui_.clipboard_notices);
}
