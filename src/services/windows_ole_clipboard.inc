// OLE 剪貼簿共用實作：FORMATETC、延遲資料、媒介序列化與 apartment 引用。
namespace {
constexpr std::uint32_t clip_pointer=0x80004003U,clip_invalid=0x80070057U,clip_memory=0x8007000eU;
constexpr std::uint32_t clip_open=0x800401d0U,clip_empty=0x800401d1U,clip_set=0x800401d2U;
constexpr std::uint32_t clip_format=0x80040064U,clip_device=0x80040065U,clip_index=0x80040068U,clip_tymed=0x80040069U,clip_aspect=0x8004006bU;
constexpr std::uint64_t clip_limit=64U*1024U*1024U,clip_chunk=65536,clip_scratch=clip_chunk+1024;
bool clip_failed(std::uint64_t result){return (result&0x80000000U)!=0;}
std::uint64_t clip_station(const GuiState& gui){const auto h=gui.user_handles.find(gui.station_handle);return h==gui.user_handles.end()?1:h->second.object;}
GuiClipboardFormat clip_read_format(const GuestMemory& memory,std::uint64_t pointer,bool x86){
    const unsigned unit=x86?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!pointer)throw MemoryFault(GuestAddress{},Access::Read,x86?20:32,"FORMATETC 為空");
    memory.check(GuestAddress{pointer},x86?20:32,Access::Read);
    GuiClipboardFormat format;format.format=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer},16));
    format.aspect=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit*2},32));
    format.index=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit*2+4},32)));
    format.tymed=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit*2+8},32));
    const auto target=memory.load(GuestAddress{pointer+unit},bits);
    if(target){
        const auto size=memory.load(GuestAddress{target},32);if(size<12 || size>65536)throw HostCallFault("DVTARGETDEVICE 大小不合法");
        format.device.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{target},format.device);
        for(unsigned i=4;i<12;i+=2){const auto offset=memory.load(GuestAddress{target+i},16);if(offset && (offset<12 || offset>=size))throw HostCallFault("DVTARGETDEVICE 偏移不合法");}
    }
    return format;
}
void clip_write_format(GuestMemory& memory,std::uint64_t pointer,const GuiClipboardFormat& format,std::uint64_t device,bool x86){
    const unsigned unit=x86?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    memory.write(GuestAddress{pointer},std::vector<std::uint8_t>(x86?20:32));
    memory.store(GuestAddress{pointer},16,format.format);memory.store(GuestAddress{pointer+unit},bits,device);
    memory.store(GuestAddress{pointer+unit*2},32,format.aspect);memory.store(GuestAddress{pointer+unit*2+4},32,static_cast<std::uint32_t>(format.index));
    memory.store(GuestAddress{pointer+unit*2+8},32,format.tymed);
}
bool clip_same_format(const GuiClipboardFormat& a,const GuiClipboardFormat& b){return a.format==b.format && a.aspect==b.aspect && a.index==b.index && a.device==b.device;}
std::uint32_t clip_media(const GuiClipboardEntry& entry){
    // OLE 的預設資料物件讓平面資料可經 HGLOBAL／IStream 取得；storage 另保留結構化媒介。
    return entry.type==1 || entry.type==4?5U:entry.type==8?13U:entry.type?entry.type:entry.format.tymed;
}
std::vector<GuiClipboardEntry> clip_formats(const std::vector<GuiClipboardEntry>& entries){
    std::vector<GuiClipboardEntry> result;
    for(const auto& entry:entries){const auto old=std::find_if(result.begin(),result.end(),[&](const auto& e){return clip_same_format(e.format,entry.format);});
        if(old==result.end()){GuiClipboardEntry e;e.format=entry.format;e.format.tymed=clip_media(entry);result.push_back(std::move(e));}else old->format.tymed|=clip_media(entry);}
    return result;
}
std::optional<std::u16string> clip_text(const GuiClipboardEntry& entry){
    if((entry.type!=1 && entry.type!=4) || (entry.format.format!=1 && entry.format.format!=13))return {};
    if(entry.format.format==13){std::u16string text;for(std::size_t i=0;i+1<entry.bytes.size();i+=2){const auto c=static_cast<char16_t>(entry.bytes[i]|(entry.bytes[i+1]<<8));if(!c)return text;text.push_back(c);}}
    else{const auto end=std::find(entry.bytes.begin(),entry.bytes.end(),0);if(end!=entry.bytes.end())return utf8_to_utf16_lossy(std::string(entry.bytes.begin(),end));}
    return {};
}
std::uint64_t clip_payload(const std::vector<GuiClipboardEntry>& entries){
    std::uint64_t total=0;for(const auto& entry:entries){const auto size=entry.bytes.size()+(entry.bitmap?entry.bitmap->pixels.size()*4:0)+(entry.metafile?entry.metafile->bytes.size():0);
        if(size>clip_limit-total)return clip_limit+1;total+=size;}return total;
}
}

std::uint64_t WindowsServices::create_clipboard_object(GuestMemory& memory,GuiClipboardObject object){
    if(gui_.clipboard_objects.size()>=65536)return 0;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    auto& table=object.enumerator?gui_.clipboard_enum_vtable:gui_.clipboard_data_vtable;
    if(!table){const auto family=object.enumerator?"internal-clipboard-enum":"internal-clipboard-data";
        const auto address=runtime_allocate(memory,{},(object.enumerator?7:12)*unit);unsigned i=0;
        for(const auto& descriptor:ole_service_descriptors())if(descriptor.family==family)memory.store(GuestAddress{address+unit*i++},bits,resolve(family,descriptor.symbol).value);
        table=address;
    }
    const auto header=allocate_heap(memory,unit);if(!header)return 0;
    try{memory.store(GuestAddress{header},bits,table);gui_.clipboard_objects.emplace(header,std::move(object));}
    catch(...){(void)free_heap(memory,header,GuestState{});throw;}return header;
}

std::vector<GuestCallback> WindowsServices::retire_ole_clipboard(GuestState& state,GuestMemory& memory,std::uint64_t thread,bool callbacks){
    std::vector<GuestCallback> result;
    const auto retire=[&](GuiStationNamespace& station){
        if(!station.clipboard_ole_object || station.clipboard_ole_thread!=thread)return;
        const auto object=station.clipboard_ole_object;station.clipboard_ole_object=station.clipboard_ole_thread=0;++station.clipboard_ole_generation;
        if(callbacks)result.push_back({resolve("internal-com-proxy","ReleaseAt").value,{thread,object}});
        else {const auto retirement=release_com_proxy_owned(state,memory,{thread,object});if(retirement)queue_com_proxy_retirement(memory,retirement);}
        // 未 Flush 的延遲資料隨提供者 apartment 關閉；已 Flush 的資料沒有 provider。
        std::erase_if(station.clipboard,[&](const auto& p){return !p.second;});
        std::erase_if(station.clipboard_order,[&](auto id){return !station.clipboard.contains(id);});
        station.clipboard_ole_entries.clear();station.clipboard_owner=0;
    };
    retire(gui_);for(auto& [id,station]:gui_.station_namespaces){(void)id;retire(station);}
    for(auto it=gui_.clipboard_windows.begin();it!=gui_.clipboard_windows.end();){if(it->first.first!=thread){++it;continue;}
        if(callbacks && gui_.windows.contains(it->second))result.push_back({resolve("user32.dll","DestroyWindow").value,{it->second}});
        it=gui_.clipboard_windows.erase(it);
    }
    return result;
}

void WindowsServices::discard_ole_clipboard(GuestState& state,GuestMemory& memory,std::uint64_t token){
    const auto found=gui_.clipboard_operations.find(token);if(found==gui_.clipboard_operations.end())return;
    auto op=std::move(found->second);gui_.clipboard_operations.erase(found);
    if(op.acquired && !op.transferred && op.provider)op.releases.emplace_back(op.thread,op.provider);
    for(const auto object:{op.enumerator,op.storage,op.lock_bytes})if(object)op.releases.emplace_back(op.thread,object);
    if(op.medium_owned){const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto type=memory.load(GuestAddress{op.buffer+64},32),value=memory.load(GuestAddress{op.buffer+64+unit},bits),owner=memory.load(GuestAddress{op.buffer+64+2*unit},bits);
        if(type==4 || type==8)op.releases.emplace_back(op.thread,value);if(owner)op.releases.emplace_back(op.thread,owner);
        else if(type==1)op.resources.emplace_back(1,value);else if(type==16 || type==64)op.resources.emplace_back(static_cast<std::uint32_t>(type),value);
    }
    for(const auto& [thread,object]:op.releases)if(object){const auto retirement=release_com_proxy_owned(state,memory,{thread,object});if(retirement)queue_com_proxy_retirement(memory,retirement);}
    if(op.global)op.resources.emplace_back(1,op.global);
    for(const auto& [type,handle]:op.resources){
        if(type==16){gui_.bitmaps.erase(handle);rasters_.erase(handle);}else if(type==64)gui_.metafiles.erase(handle);
        else if(const auto global=gui_.global_memory.find(handle);global!=gui_.global_memory.end() && !global->second.clipboard_owned){if(global->second.address)(void)free_heap(memory,global->second.address,state);gui_.global_memory.erase(global);}
    }
    if(op.buffer)(void)free_heap(memory,op.buffer,state);
}

std::optional<std::uint32_t> WindowsServices::invoke_ole_clipboard(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using Kind=GuiClipboardOperation::Kind;
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
    const auto done=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    try{
    if(service.family=="internal-clipboard-data" || service.family=="internal-clipboard-enum"){
        const auto found=gui_.clipboard_objects.find(arg(0));if(found==gui_.clipboard_objects.end())return done(0x80010108U);
        auto& object=found->second;if(object.enumerator!=(service.family=="internal-clipboard-enum"))return done(0x80004002U);
        if(name=="AddRef"){if(object.references==UINT32_MAX)return done(UINT32_MAX);return done(++object.references);}
        if(name=="Release"){
            if(--object.references)return done(object.references);const auto provider=object.provider,thread=object.thread;
            gui_.clipboard_objects.erase(found);(void)free_heap(memory,arg(0),state);
            if(provider){start_callbacks(state,memory,{{resolve("internal-com-proxy","ReleaseAt").value,{thread,provider}}},Continuation::Return,0);return {};}
            return done(0);
        }
        if(name=="QueryInterface"){
            if(!arg(2))return done(clip_pointer);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(clip_pointer);
            OleGuid requested{};memory.read(GuestAddress{arg(1)},requested);
            constexpr OleGuid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            auto expected=unknown;expected[0]=object.enumerator?3:14;expected[1]=1;
            if(requested!=unknown && requested!=expected)return done(0x80004002U);
            if(object.references==UINT32_MAX)return done(clip_memory);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(0);
        }
        if(object.enumerator){
            if(name=="Reset"){object.cursor=0;return done(0);}
            if(name=="Skip"){const auto count=static_cast<std::uint32_t>(arg(1));const auto left=object.entries.size()-object.cursor;object.cursor+=std::min<std::size_t>(count,left);return done(count<=left?0:1);}
            if(name=="Clone"){if(!arg(1))return done(clip_pointer);memory.store(GuestAddress{arg(1)},bits,0);auto copy=object;copy.references=1;const auto clone=create_clipboard_object(memory,std::move(copy));if(!clone)return done(clip_memory);memory.store(GuestAddress{arg(1)},bits,clone);return done(0);}
            if(name!="Next")throw HostCallFault("未知的 IEnumFORMATETC 方法");
            const auto count=static_cast<std::uint32_t>(arg(1));if(count>4096 || (!arg(3) && count!=1))return done(clip_invalid);
            if(arg(3))memory.store(GuestAddress{arg(3)},32,0);if(count && !arg(2))return done(clip_pointer);
            const auto amount=std::min<std::size_t>(count,object.entries.size()-object.cursor);const auto stride=x86_?20U:32U;
            memory.check(GuestAddress{arg(2)},amount*stride,Access::Write);std::vector<std::uint64_t> targets;
            try{for(std::size_t i=0;i<amount;++i){const auto& format=object.entries[object.cursor+i].format;std::uint64_t target=0;
                if(!format.device.empty()){target=allocate_heap(memory,format.device.size());if(!target)throw std::bad_alloc();targets.push_back(target);memory.write(GuestAddress{target},format.device);gui_.task_allocations.insert(target);}
                clip_write_format(memory,arg(2)+i*stride,format,target,x86_);
            }}catch(...){for(const auto target:targets){gui_.task_allocations.erase(target);(void)free_heap(memory,target,state);}throw;}
            object.cursor+=amount;if(arg(3))memory.store(GuestAddress{arg(3)},32,amount);return done(amount==count?0:1);
        }
        if(object.provider && object.thread!=threads_.current)return done(0x8001010eU);
        if(name=="EnumFormatEtc" && static_cast<std::uint32_t>(arg(1))==1){
            if(!arg(2))return done(clip_pointer);memory.store(GuestAddress{arg(2)},bits,0);
            GuiClipboardObject enumeration;enumeration.enumerator=true;enumeration.entries=clip_formats(object.entries);
            const auto result=create_clipboard_object(memory,std::move(enumeration));if(!result)return done(clip_memory);memory.store(GuestAddress{arg(2)},bits,result);return done(0);
        }
        if(object.provider){
            static constexpr std::array<std::string_view,12> methods{"QueryInterface","AddRef","Release","GetData","GetDataHere","QueryGetData","GetCanonicalFormatEtc","SetData","EnumFormatEtc","DAdvise","DUnadvise","EnumDAdvise"};
            const auto method=std::find(methods.begin(),methods.end(),name);if(method==methods.end())throw HostCallFault("未知的 IDataObject 方法");
            std::vector<std::uint64_t> arguments{object.provider};for(unsigned i=1;i<service.arguments;++i)arguments.push_back(arg(i));
            start_callbacks(state,memory,{{com_method(memory,object.provider,static_cast<unsigned>(method-methods.begin())),std::move(arguments)}},Continuation::ReturnCallback);return {};
        }
        if(name=="DAdvise" || name=="DUnadvise" || name=="EnumDAdvise")return done(0x80040003U);
        if(name=="SetData" || name=="EnumFormatEtc")return done(0x80004001U);
        if(name=="GetCanonicalFormatEtc"){
            if(!arg(1) || !arg(2))return done(clip_pointer);const auto format=clip_read_format(memory,arg(1),x86_);clip_write_format(memory,arg(2),format,0,x86_);return done(0x40130U);
        }
        if(name!="GetData" && name!="GetDataHere" && name!="QueryGetData")throw HostCallFault("未知的唯讀 IDataObject 方法");
        if(!arg(1))return done(clip_pointer);const auto requested=clip_read_format(memory,arg(1),x86_);
        const GuiClipboardEntry* chosen=nullptr;std::uint32_t selected_type=0;bool format_match=false,aspect_match=false,index_match=false,device_match=false;
        for(const auto& entry:object.entries)if(entry.format.format==requested.format){format_match=true;if(entry.format.aspect!=requested.aspect)continue;aspect_match=true;if(entry.format.index!=requested.index)continue;index_match=true;if(entry.format.device!=requested.device)continue;device_match=true;
            auto media=clip_media(entry)&requested.tymed;
            // 非 OLE caller 放入的 compound bytes 也能要求 IStorage；列舉仍以原始平面格式描述。
            constexpr std::array<std::uint8_t,8> compound{0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};
            if(name!="QueryGetData" && (requested.tymed&8U) && entry.type==1 && entry.bytes.size()>=compound.size() && std::equal(compound.begin(),compound.end(),entry.bytes.begin()))media|=8U;
            if(media){chosen=&entry;selected_type=(entry.type&media)?entry.type:media&(~media+1U);break;}}
        if(!chosen)return done(!format_match?clip_format:!aspect_match?clip_aspect:!index_match?clip_index:!device_match?clip_device:clip_tymed);
        if(name=="QueryGetData")return done(0);if(!arg(2))return done(clip_pointer);memory.check(GuestAddress{arg(2)},3*unit,Access::Write);
        if(name=="GetDataHere" && (!requested.tymed || (requested.tymed&(requested.tymed-1U)) || (requested.tymed&~15U)))return done(clip_tymed);
        GuiClipboardOperation operation;operation.kind=Kind::Read;operation.thread=threads_.current;operation.output=arg(2);operation.type=selected_type;operation.entries.push_back(*chosen);operation.here=name=="GetDataHere";
        if(operation.here){memory.check(GuestAddress{arg(2)},3*unit,Access::Read);if(memory.load(GuestAddress{arg(2)},32)!=selected_type || memory.load(GuestAddress{arg(2)+2*unit},bits))return done(clip_tymed);operation.medium=memory.load(GuestAddress{arg(2)+unit},bits);if(!operation.medium)return done(clip_pointer);}
        else memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(3*unit));
        if(gui_.clipboard_operations.size()>=128)return done(clip_memory);operation.buffer=allocate_heap(memory,clip_scratch);if(!operation.buffer)return done(clip_memory);
        const auto token=gui_.next_object++;gui_.clipboard_operations.emplace(token,std::move(operation));return resume_ole_clipboard(state,memory,token,0);
    }
    if(name=="WindowProc"){
        if(static_cast<std::uint32_t>(arg(1))==0x305){
            if(arg(0)!=gui_.clipboard_owner || !gui_.clipboard_ole_object)return done(0);
            // 用獨立的 Render API frame，讓一般 Win32 caller 不必初始化 COM。
            start_callbacks(state,memory,{{resolve("internal-ole-clipboard","Render").value,{arg(2)}}},Continuation::ReturnCallback);return {};
        }
        return invoke_gui(state,memory,{"user32.dll","DefWindowProcW",4});
    }
    refresh_clipboard();if(gui_.clipboard_stale){
        if(gui_.clipboard_ole_object){const auto retirement=release_com_proxy_owned(state,memory,{gui_.clipboard_ole_thread,gui_.clipboard_ole_object});if(retirement)queue_com_proxy_retirement(memory,retirement);gui_.clipboard_ole_object=gui_.clipboard_ole_thread=0;}
        release_gui_clipboard(state,memory);
    }
    if(name=="OleIsCurrentClipboard")return done(arg(0) && arg(0)==gui_.clipboard_ole_object?0:1);
    const bool raw=name=="Render" || name=="Empty";
    if(!raw && !gui_.ole_initializations[threads_.current])return done(0x800401f0U);
    if((name=="OleGetClipboard" || name=="Render") && (job_ui_restrictions()&2U))return done(raw?0:0x80070005U);
    if((name=="OleSetClipboard" || name=="OleFlushClipboard" || name=="Empty") && (job_ui_restrictions()&4U))return done(raw?0:0x80070005U);
    if(raw){if(gui_.clipboard_open_thread!=threads_.current && !(name=="Render" && gui_.clipboard_ole_thread==threads_.current)){last_error_=1418;return done(0);}}
    else if(gui_.clipboard_open_thread)return done(clip_open);
    if(name=="Render" && gui_.clipboard_ole_object && gui_.clipboard_ole_thread!=threads_.current){
        if(!gui_.windows.contains(gui_.clipboard_owner)){last_error_=1400;return done(0);}
        start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{gui_.clipboard_owner,0x305,arg(0),0}}},Continuation::ReturnCallback);return {};
    }
    GuiClipboardOperation operation;operation.thread=threads_.current;operation.station=clip_station(gui_);operation.generation=gui_.clipboard_ole_generation;
    operation.source=gui_.clipboard_ole_object;operation.source_thread=gui_.clipboard_ole_thread;operation.window=gui_.clipboard_owner;
    if(name=="OleSetClipboard"){operation.kind=Kind::Set;operation.source=arg(0);operation.source_thread=threads_.current;}
    else if(name=="Empty")operation.kind=Kind::Empty;
    else if(name=="OleGetClipboard"){
        if(!arg(0))return done(clip_pointer);memory.store(GuestAddress{arg(0)},bits,0);operation.kind=Kind::Get;operation.output=arg(0);operation.entries=gui_.clipboard_ole_entries;
        if(!operation.source){
            for(const auto& [format,handle]:gui_.clipboard)if(handle && std::none_of(operation.entries.begin(),operation.entries.end(),[&](const auto& entry){return entry.format.format==format;})){
                GuiClipboardEntry entry;entry.format.format=format;entry.type=gui_.clipboard_types.contains(format)?gui_.clipboard_types.at(format):1U;entry.format.tymed=entry.type;
                if(entry.type==1){const auto global=gui_.global_memory.find(handle);if(global==gui_.global_memory.end())continue;if(global->second.size>clip_limit)return done(clip_memory);entry.bytes.resize(static_cast<std::size_t>(global->second.size));if(!entry.bytes.empty())memory.read(GuestAddress{global->second.address},entry.bytes);}
                else if(entry.type==16){sync_raster_bitmap(memory,handle);const auto bitmap=gui_.bitmaps.find(handle);if(bitmap==gui_.bitmaps.end())continue;entry.bitmap=bitmap->second;entry.bitmap->dib_address=entry.bitmap->dib_view=entry.bitmap->dib_section=0;}
                else if(entry.type==64){const auto metafile=gui_.metafiles.find(handle);if(metafile==gui_.metafiles.end())continue;entry.metafile=metafile->second;}
                else continue;operation.entries.push_back(std::move(entry));
            }
            if(gui_.clipboard_native_text)for(const auto format:{13U,1U})if(std::none_of(operation.entries.begin(),operation.entries.end(),[&](const auto& entry){return entry.format.format==format;})){
                GuiClipboardEntry entry;entry.format.format=format;entry.type=entry.format.tymed=1;if(format==13)entry.bytes=utf16_bytes(*gui_.clipboard_native_text);else{const auto text=utf16_to_utf8(*gui_.clipboard_native_text);entry.bytes.assign(text.begin(),text.end());entry.bytes.push_back(0);}operation.entries.push_back(std::move(entry));}
        }
    }else if(name=="OleFlushClipboard"){
        if(!operation.source)return done(0);operation.kind=Kind::Flush;
        for(const auto& entry:gui_.clipboard_ole_entries)if(entry.format.device.empty())for(const auto type:{1U,4U,8U,16U,32U,64U})if(entry.format.tymed&type){GuiClipboardEntry copy;copy.format=entry.format;copy.format.tymed=type;operation.entries.push_back(std::move(copy));}
    }else if(name=="Render"){
        operation.kind=Kind::Render;operation.requested=static_cast<std::uint32_t>(arg(0));
        if(!operation.source){
            const auto owner=gui_.windows.find(operation.window);if(owner==gui_.windows.end())return done(0);
            for(const auto format:{operation.requested,operation.requested==13?1U:operation.requested==1?13U:0U})if(format && gui_.clipboard.contains(format) && !gui_.clipboard.at(format)){operation.type=format;break;}
            if(!operation.type)return done(0);operation.source_thread=owner->second.thread;
            if(std::any_of(gui_.clipboard_operations.begin(),gui_.clipboard_operations.end(),[&](const auto& p){const auto& old=p.second;return old.kind==Kind::Render && !old.source && old.station==operation.station && old.type==operation.type;}))return done(0);
        }
        const GuiClipboardEntry* chosen=nullptr;for(const auto format:{operation.requested,operation.requested==13?1U:operation.requested==1?13U:0U}){
            for(const auto& entry:gui_.clipboard_ole_entries)if(entry.format.format==format && entry.format.aspect==1 && entry.format.index==-1 && entry.format.device.empty() && (entry.format.tymed&0x5dU)){chosen=&entry;if(entry.format.tymed&1U)break;}
            if(chosen)break;
        }
        if(operation.source){if(!chosen)return done(0);GuiClipboardEntry copy;copy.format=chosen->format;copy.format.tymed&=0x5dU;operation.entries.push_back(std::move(copy));}
    }else throw HostCallFault("未知的 OLE 剪貼簿服務");
    if(operation.entries.size()>4096 || gui_.clipboard_operations.size()>=128)return done(clip_memory);
    operation.buffer=allocate_heap(memory,clip_scratch);if(!operation.buffer)return done(clip_memory);memory.write(GuestAddress{operation.buffer},std::vector<std::uint8_t>(1024));
    const auto token=gui_.next_object++;gui_.clipboard_operations.emplace(token,std::move(operation));return resume_ole_clipboard(state,memory,token,0);
    }catch(const MemoryFault&){return done(clip_pointer);}catch(const std::bad_alloc&){return done(clip_memory);}
}

std::optional<std::uint32_t> WindowsServices::resume_ole_clipboard(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    using Kind=GuiClipboardOperation::Kind;using Stage=GuiClipboardOperation::Stage;
    auto found=gui_.clipboard_operations.find(token);if(found==gui_.clipboard_operations.end()){complete_call(state,memory,0x800703e3U);return {};}
    auto& op=found->second;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);const auto buffer=op.buffer;
    const auto callback=[&](Stage stage,GuestCallback call){op.stage=stage;start_callbacks(state,memory,{std::move(call)},Continuation::OleClipboardOperation,token);};
    const auto method=[&](Stage stage,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> args,std::uint32_t wide=0){GuestCallback call{com_method(memory,object,slot),std::move(args)};call.wide_arguments=wide;callback(stage,std::move(call));};
    const auto api=[&](Stage stage,std::string_view family,std::string_view symbol,std::vector<std::uint64_t> args){callback(stage,{resolve(family,symbol).value,std::move(args)});};
    const auto finish=[&](std::uint32_t status)->std::optional<std::uint32_t>{
        op.status=status;op.stage=Stage::Cleanup;std::vector<GuestCallback> releases;
        if(op.medium_owned){releases.push_back({resolve("ole32.dll","ReleaseStgMedium").value,{buffer+64}});op.medium_owned=false;}
        if(op.acquired && !op.transferred && op.provider){op.releases.emplace_back(op.thread,op.provider);op.acquired=false;}
        for(auto* object:{&op.enumerator,&op.storage,&op.lock_bytes})if(*object){op.releases.emplace_back(op.thread,*object);*object=0;}
        for(const auto& [thread,object]:op.releases)if(object)releases.push_back({resolve("internal-com-proxy","ReleaseAt").value,{thread,object}});op.releases.clear();
        if(!releases.empty()){start_callbacks(state,memory,std::move(releases),Continuation::OleClipboardOperation,token);return {};}
        const auto value=op.kind==Kind::Render?(status?0:op.prepared.at(op.requested).second):op.kind==Kind::Empty?(status?0:1):status;
        if(status && (op.kind==Kind::Render || op.kind==Kind::Empty))last_error_=5;
        discard_ole_clipboard(state,memory,token);complete_call(state,memory,value);return {};
    };
    try{
    if(op.stage==Stage::Cleanup){const auto status=op.status;const auto value=op.kind==Kind::Render?(status?0:op.prepared.at(op.requested).second):op.kind==Kind::Empty?(status?0:1):status;
        if(status && (op.kind==Kind::Render || op.kind==Kind::Empty))last_error_=5;discard_ole_clipboard(state,memory,token);complete_call(state,memory,value);return {};}
    if(op.kind!=Kind::Read)refresh_clipboard();
    const auto valid=[&]{return op.kind==Kind::Read || (!gui_.clipboard_stale && clip_station(gui_)==op.station && gui_.clipboard_ole_generation==op.generation);};
    const auto global_bytes=[&](std::uint64_t handle,std::vector<std::uint8_t>& bytes){const auto global=gui_.global_memory.find(handle);if(global==gui_.global_memory.end() || global->second.size>clip_limit || (global->second.size && !global->second.address))return false;
        bytes.resize(static_cast<std::size_t>(global->second.size));if(!bytes.empty())memory.read(GuestAddress{global->second.address},bytes);return true;};
    if(op.kind==Kind::Render && !op.source){
        if(!valid())return finish(0x800703e3U);
        if(op.stage==Stage::Start){api(Stage::Window,"user32.dll","SendMessageW",{op.window,0x305,op.type,0});return {};}
        if(op.stage!=Stage::Window)throw HostCallFault("Win32 剪貼簿 rendering 階段不合法");
        auto data=gui_.clipboard.find(op.requested);
        if((data==gui_.clipboard.end() || !data->second) && op.type!=op.requested){
            const auto rendered=gui_.clipboard.find(op.type);GuiClipboardEntry text;text.format.format=op.type;text.type=1;
            if(rendered!=gui_.clipboard.end() && rendered->second && global_bytes(rendered->second,text.bytes))if(const auto value=clip_text(text)){
                std::vector<std::uint8_t> bytes;if(op.requested==13)bytes=utf16_bytes(*value);else{const auto ansi=utf16_to_utf8(*value);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}
                const auto handle=allocate_gui_global(memory,bytes.size(),2);if(!handle)return finish(clip_memory);
                auto& block=gui_.global_memory.at(handle);memory.write(GuestAddress{block.address},bytes);block.clipboard_owned=true;gui_.clipboard[op.requested]=handle;gui_.clipboard_types[op.requested]=1;gui_.clipboard_order.push_back(op.requested);data=gui_.clipboard.find(op.requested);
            }
        }
        if(data==gui_.clipboard.end() || !data->second)return finish(clip_format);
        op.prepared.emplace(op.requested,std::make_pair(gui_.clipboard_types.contains(op.requested)?gui_.clipboard_types.at(op.requested):1U,data->second));return finish(0);
    }
    const auto prepare=[&](const GuiClipboardEntry& entry)->bool{
        const auto format=entry.format.format;if(op.prepared.contains(format) || entry.format.aspect!=1 || entry.format.index!=-1 || !entry.format.device.empty())return true;
        std::uint64_t handle{};std::uint32_t type=entry.type;
        if(type==1 || type==4 || type==8){type=1;handle=allocate_gui_global(memory,entry.bytes.size(),2);if(handle && !entry.bytes.empty())memory.write(GuestAddress{gui_.global_memory.at(handle).address},entry.bytes);}
        else if(type==16 && entry.bitmap){handle=gui_.next_object++;gui_.bitmaps.emplace(handle,*entry.bitmap);}
        else if(type==64 && entry.metafile){handle=gui_.next_object++;gui_.metafiles.emplace(handle,*entry.metafile);}
        if(!handle)return false;op.resources.emplace_back(type,handle);op.prepared.emplace(format,std::make_pair(type,handle));return true;
    };
    const auto publish=[&]()->std::uint32_t{
        if(!valid())return 0x800703e3U;
        if(clip_payload(op.entries)>clip_limit)return clip_memory;
        for(const auto& entry:op.entries)if(!prepare(entry))return clip_memory;
        std::optional<std::u16string> text;for(const auto format:{13U,1U})for(const auto& entry:op.entries)if(entry.format.format==format && !text)text=clip_text(entry);
        if(op.kind==Kind::Render && (op.requested==1 || op.requested==13) && !op.prepared.contains(op.requested) && text){
            GuiClipboardEntry converted;converted.format.format=op.requested;converted.type=converted.format.tymed=1;
            if(op.requested==13)converted.bytes=utf16_bytes(*text);else{const auto ansi=utf16_to_utf8(*text);converted.bytes.assign(ansi.begin(),ansi.end());converted.bytes.push_back(0);}
            if(!prepare(converted))return clip_memory;
        }
        if(native_gui_ && interactive_station() && text && !native_gui_->set_clipboard_text(*text))return clip_set;
        if(op.kind==Kind::Flush){const auto original=gui_.clipboard_ole_object,thread=gui_.clipboard_ole_thread;release_gui_clipboard(state,memory);gui_.clipboard_ole_object=gui_.clipboard_ole_thread=0;op.releases.emplace_back(thread,original);gui_.clipboard_ole_entries=op.entries;}
        for(const auto& [format,resource]:op.prepared){gui_.clipboard[format]=resource.second;gui_.clipboard_types[format]=resource.first;if(resource.first==1)gui_.global_memory.at(resource.second).clipboard_owned=true;if(std::find(gui_.clipboard_order.begin(),gui_.clipboard_order.end(),format)==gui_.clipboard_order.end())gui_.clipboard_order.push_back(format);}
        if(op.kind==Kind::Render)for(const auto& entry:op.entries)for(auto& old:gui_.clipboard_ole_entries)if(clip_same_format(old.format,entry.format)){old.bytes=entry.bytes;old.type=entry.type;old.bitmap=entry.bitmap;old.metafile=entry.metafile;break;}
        op.resources.clear();gui_.clipboard_native_text=std::move(text);refresh_clipboard(true);notify_clipboard();return 0;
    };
    const auto next_medium=[&]()->std::optional<std::uint32_t>{
        if(!valid())return finish(0x800703e3U);
        if(op.index>=op.entries.size())return finish(publish());
        memory.write(GuestAddress{buffer+64},std::vector<std::uint8_t>(3*unit));clip_write_format(memory,buffer,op.entries[op.index].format,0,x86_);
        method(Stage::Medium,op.provider,3,{op.provider,buffer,buffer+64});return {};
    };
    if(op.kind==Kind::Read){
        const auto& entry=op.entries.front();
        const auto output_medium=[&](std::uint64_t value){if(!op.here){memory.check(GuestAddress{op.output},3*unit,Access::Write);memory.store(GuestAddress{op.output},32,op.type);memory.store(GuestAddress{op.output+unit},bits,value);memory.store(GuestAddress{op.output+2*unit},bits,0);op.global=0;op.storage=0;}return finish(0);};
        if(op.stage==Stage::Start){
            if(op.type==16 || op.type==64){if(op.here)return finish(clip_tymed);const auto handle=gui_.next_object++;
                if(op.type==16 && entry.bitmap)gui_.bitmaps.emplace(handle,*entry.bitmap);else if(op.type==64 && entry.metafile)gui_.metafiles.emplace(handle,*entry.metafile);else return finish(clip_tymed);return output_medium(handle);}
            if(op.type==1 && op.here){const auto global=gui_.global_memory.find(op.medium);if(global==gui_.global_memory.end() || global->second.size<entry.bytes.size() || (entry.bytes.size() && !global->second.address))return finish(0x80030070U);
                if(!entry.bytes.empty())memory.write(GuestAddress{global->second.address},entry.bytes);return finish(0);}
            op.global=allocate_gui_global(memory,entry.bytes.size(),2);if(!op.global)return finish(clip_memory);if(!entry.bytes.empty())memory.write(GuestAddress{gui_.global_memory.at(op.global).address},entry.bytes);
            if(op.type==1)return output_medium(op.global);
            memory.store(GuestAddress{buffer+128},bits,0);
            if(op.type==4)api(Stage::ReadStream,"ole32.dll","CreateStreamOnHGlobal",{op.global,1,buffer+128});
            else if(op.type==8)api(Stage::ReadLock,"ole32.dll","CreateILockBytesOnHGlobal",{op.global,1,buffer+128});
            else return finish(clip_tymed);return {};
        }
        if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));
        if(op.stage==Stage::ReadStream){op.global=0;const auto stream=memory.load(GuestAddress{buffer+128},bits);if(!stream)return finish(0x8000ffffU);op.storage=stream;
            if(!op.here){method(Stage::StreamSeek,stream,5,{stream,entry.bytes.size(),0,0},2);return {};}
            memory.store(GuestAddress{buffer+136},64,0);memory.store(GuestAddress{buffer+144},64,0);
            method(Stage::ReadCopy,stream,7,{stream,op.medium,entry.bytes.size(),buffer+136,buffer+144},4);return {};}
        if(op.stage==Stage::StreamSeek)return output_medium(op.storage);
        if(op.stage==Stage::ReadLock){op.global=0;op.lock_bytes=memory.load(GuestAddress{buffer+128},bits);if(!op.lock_bytes)return finish(0x8000ffffU);memory.store(GuestAddress{buffer+152},bits,0);
            api(Stage::ReadStorage,"ole32.dll","StgOpenStorageOnILockBytes",{op.lock_bytes,0,0x10,0,0,buffer+152});return {};}
        if(op.stage==Stage::ReadStorage){const auto storage=memory.load(GuestAddress{buffer+152},bits);if(!storage)return finish(0x8000ffffU);op.storage=storage;if(!op.here)return output_medium(storage);
            method(Stage::ReadCopy,storage,7,{storage,0,0,0,op.medium});return {};}
        if(op.stage==Stage::ReadCopy){if(op.type==4 && (memory.load(GuestAddress{buffer+136},64)!=entry.bytes.size() || memory.load(GuestAddress{buffer+144},64)!=entry.bytes.size()))return finish(0x80030070U);return finish(0);}
        throw HostCallFault("剪貼簿資料媒介讀取階段不合法");
    }
    if(op.stage==Stage::Start){
        if(op.kind==Kind::Empty)op.stage=Stage::Window;
        else if(op.source){
            if(op.kind==Kind::Set){op.provider=op.source;method(Stage::Acquire,op.source,1,{op.source});}
            else{memory.store(GuestAddress{buffer+96},32,0);api(Stage::Acquire,"internal-com-proxy","Marshal",{op.source,op.source_thread,op.thread,static_cast<std::uint64_t>(com_proxy::Kind::DataObject),buffer+96});}
            return {};
        }
        if(op.kind==Kind::Get){GuiClipboardObject object;object.entries=op.entries;const auto pointer=create_clipboard_object(memory,std::move(object));if(!pointer)return finish(clip_memory);memory.store(GuestAddress{op.output},bits,pointer);return finish(0);}
        if(op.kind==Kind::Set || op.kind==Kind::Empty)op.stage=Stage::Window;
    }
    if(op.stage==Stage::Acquire){
        op.acquired=true;
        if(op.kind==Kind::Set){memory.store(GuestAddress{buffer+96},bits,0);method(Stage::Enumerate,op.provider,8,{op.provider,1,buffer+96});return {};}
        op.provider=result;const auto error=memory.load(GuestAddress{buffer+96},32);if(error || !op.provider){op.acquired=false;return finish(error?static_cast<std::uint32_t>(error):0x8000ffffU);}
        if(op.kind==Kind::Get){if(!valid())return finish(0x800703e3U);GuiClipboardObject object;object.provider=op.provider;object.thread=op.thread;object.entries=op.entries;
            const auto pointer=create_clipboard_object(memory,std::move(object));if(!pointer)return finish(clip_memory);op.transferred=true;memory.store(GuestAddress{op.output},bits,pointer);return finish(0);}
        return next_medium();
    }
    if(op.stage==Stage::Enumerate){
        if(clip_failed(result) && static_cast<std::uint32_t>(result)!=0x80004001U)return finish(static_cast<std::uint32_t>(result));
        op.enumerator=clip_failed(result)?0:memory.load(GuestAddress{buffer+96},bits);
        if(op.enumerator){memory.store(GuestAddress{buffer+100},32,0);method(Stage::Next,op.enumerator,3,{op.enumerator,1,buffer,buffer+100});return {};}
        if(!clip_failed(result))return finish(0x8000ffffU);op.stage=Stage::ReleaseEnumerator;
    }
    if(op.stage==Stage::Next){
        const auto fetched=memory.load(GuestAddress{buffer+100},32);if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));
        if(fetched>1 || (result==0 && fetched!=1))return finish(0x8000ffffU);
        if(fetched){if(op.entries.size()>=4096)return finish(clip_memory);GuiClipboardEntry entry;entry.format=clip_read_format(memory,buffer,x86_);
            if(!entry.format.format || !entry.format.tymed || (entry.format.tymed&~0x7fU))return finish(clip_format);op.entries.push_back(std::move(entry));
            const auto device=memory.load(GuestAddress{buffer+unit},bits);if(device){api(Stage::FreeDevice,"ole32.dll","CoTaskMemFree",{device});return {};}
            memory.store(GuestAddress{buffer+100},32,0);method(Stage::Next,op.enumerator,3,{op.enumerator,1,buffer,buffer+100});return {};
        }
        const auto enumerator=op.enumerator;op.enumerator=0;method(Stage::ReleaseEnumerator,enumerator,2,{enumerator});return {};
    }
    if(op.stage==Stage::FreeDevice){memory.store(GuestAddress{buffer+100},32,0);method(Stage::Next,op.enumerator,3,{op.enumerator,1,buffer,buffer+100});return {};}
    if(op.stage==Stage::ReleaseEnumerator){
        if(!valid())return finish(0x800703e3U);const auto window=gui_.clipboard_windows.find({op.thread,op.station});
        if(window!=gui_.clipboard_windows.end() && gui_.windows.contains(window->second)){result=window->second;op.stage=Stage::Window;}
        else{memory.write(GuestAddress{buffer+192},utf16_bytes(u"Static"));api(Stage::Window,"user32.dll","CreateWindowExW",{0,buffer+192,0,0x80000000U,0,0,0,0,static_cast<std::uint64_t>(static_cast<std::int64_t>(-3)),0,0,0});return {};}
    }
    if(op.stage==Stage::Window){
        if(!valid())return finish(0x800703e3U);
        if(op.source && op.kind==Kind::Set){op.window=result;const auto window=gui_.windows.find(op.window);if(window==gui_.windows.end())return finish(clip_memory);window->second.procedure=resolve("internal-ole-clipboard","WindowProc").value;window->second.unicode=true;gui_.clipboard_windows[{op.thread,op.station}]=op.window;}
        if(native_gui_ && interactive_station() && !native_gui_->clear_clipboard())return finish(clip_empty);
        const auto old=gui_.clipboard_ole_object,old_thread=gui_.clipboard_ole_thread;
        release_gui_clipboard(state,memory);gui_.clipboard_ole_object=gui_.clipboard_ole_thread=0;gui_.clipboard_native_text.reset();
        if(old)op.releases.emplace_back(old_thread,old);
        if(op.kind==Kind::Set && op.source){gui_.clipboard_ole_object=op.provider;gui_.clipboard_ole_thread=op.thread;gui_.clipboard_ole_entries=op.entries;gui_.clipboard_owner=op.window;op.transferred=true;
            for(const auto& entry:op.entries)if(entry.format.device.empty() && entry.format.aspect==1 && entry.format.index==-1 && (entry.format.tymed&0x5dU)){gui_.clipboard.try_emplace(entry.format.format,0);if(std::find(gui_.clipboard_order.begin(),gui_.clipboard_order.end(),entry.format.format)==gui_.clipboard_order.end())gui_.clipboard_order.push_back(entry.format.format);}
        }else gui_.clipboard_owner=op.kind==Kind::Empty?gui_.clipboard_open_window:0;
        refresh_clipboard(true);notify_clipboard();return finish(0);
    }
    if(op.stage==Stage::Medium){
        if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));op.medium_owned=true;
        auto& entry=op.entries.at(op.index);op.type=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+64},32));op.medium=memory.load(GuestAddress{buffer+64+unit},bits);
        if(!(entry.format.tymed&op.type) || !op.medium || (op.type!=1 && op.type!=4 && op.type!=8 && op.type!=16 && op.type!=64))return finish(clip_tymed);
        entry.type=op.type;entry.format.tymed=op.type;
        if(op.type==1){if(!global_bytes(op.medium,entry.bytes))return finish(clip_tymed);}
        else if(op.type==16){sync_raster_bitmap(memory,op.medium);const auto bitmap=gui_.bitmaps.find(op.medium);if(bitmap==gui_.bitmaps.end())return finish(clip_tymed);entry.bitmap=bitmap->second;entry.bitmap->dib_address=entry.bitmap->dib_view=entry.bitmap->dib_section=0;}
        else if(op.type==64){const auto metafile=gui_.metafiles.find(op.medium);if(metafile==gui_.metafiles.end())return finish(clip_tymed);entry.metafile=metafile->second;}
        else if(op.type==4){memory.write(GuestAddress{buffer+160},std::vector<std::uint8_t>(80));method(Stage::StreamStat,op.medium,12,{op.medium,buffer+160,1});return {};}
        else {op.global=allocate_gui_global(memory,0,2);if(!op.global)return finish(clip_memory);memory.store(GuestAddress{buffer+128},bits,0);api(Stage::StorageLock,"ole32.dll","CreateILockBytesOnHGlobal",{op.global,1,buffer+128});return {};}
        if(clip_payload(op.entries)>clip_limit)return finish(clip_memory);op.medium_owned=false;api(Stage::ReleaseMedium,"ole32.dll","ReleaseStgMedium",{buffer+64});return {};
    }
    if(op.stage==Stage::StreamStat){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));op.length=memory.load(GuestAddress{buffer+160+(x86_?8:16)},64);
        memory.store(GuestAddress{buffer+136},64,0);method(Stage::StreamTell,op.medium,5,{op.medium,0,1,buffer+136},2);return {};}
    if(op.stage==Stage::StreamTell){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));op.position=memory.load(GuestAddress{buffer+136},64);if(op.position>op.length)return finish(0x8003001eU);
        const auto used=clip_payload(op.entries);if(used>clip_limit || op.position>clip_limit-used)return finish(clip_memory);op.length=op.position;op.entries.at(op.index).bytes.resize(static_cast<std::size_t>(op.length));
        op.position_valid=true;op.offset=0;method(Stage::StreamSeek,op.medium,5,{op.medium,0,0,0},2);return {};}
    if(op.stage==Stage::StreamRead){
        const auto count=memory.load(GuestAddress{buffer+100},32),expected=std::min<std::uint64_t>(clip_chunk,op.length-op.offset);
        if(clip_failed(result) || count!=expected){op.status=clip_failed(result)?static_cast<std::uint32_t>(result):0x8003001eU;method(Stage::StreamRestore,op.medium,5,{op.medium,op.position,0,0},2);return {};}
        memory.read(GuestAddress{buffer+1024},std::span(op.entries.at(op.index).bytes).subspan(static_cast<std::size_t>(op.offset),count));op.offset+=count;op.stage=Stage::StreamSeek;
    }
    if(op.stage==Stage::StreamSeek){
        if(clip_failed(result)){op.status=static_cast<std::uint32_t>(result);method(Stage::StreamRestore,op.medium,5,{op.medium,op.position,0,0},2);return {};}
        if(op.offset>=op.length){method(Stage::StreamRestore,op.medium,5,{op.medium,op.position,0,0},2);return {};}
        memory.store(GuestAddress{buffer+100},32,0);method(Stage::StreamRead,op.medium,3,{op.medium,buffer+1024,std::min<std::uint64_t>(clip_chunk,op.length-op.offset),buffer+100});return {};
    }
    if(op.stage==Stage::StreamRestore){op.position_valid=false;if(op.status || clip_failed(result))return finish(op.status?op.status:static_cast<std::uint32_t>(result));op.medium_owned=false;api(Stage::ReleaseMedium,"ole32.dll","ReleaseStgMedium",{buffer+64});return {};}
    if(op.stage==Stage::StorageLock){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));op.global=0;op.lock_bytes=memory.load(GuestAddress{buffer+128},bits);if(!op.lock_bytes)return finish(0x8000ffffU);
        memory.store(GuestAddress{buffer+152},bits,0);api(Stage::StorageCreate,"ole32.dll","StgCreateDocfileOnILockBytes",{op.lock_bytes,0x1012,0,buffer+152});return {};}
    if(op.stage==Stage::StorageCreate){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));op.storage=memory.load(GuestAddress{buffer+152},bits);if(!op.storage)return finish(0x8000ffffU);method(Stage::StorageCopy,op.medium,7,{op.medium,0,0,0,op.storage});return {};}
    if(op.stage==Stage::StorageCopy){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));method(Stage::StorageCommit,op.storage,9,{op.storage,0});return {};}
    if(op.stage==Stage::StorageCommit){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));memory.store(GuestAddress{buffer+128},bits,0);api(Stage::StorageGlobal,"ole32.dll","GetHGlobalFromILockBytes",{op.lock_bytes,buffer+128});return {};}
    if(op.stage==Stage::StorageGlobal){if(clip_failed(result))return finish(static_cast<std::uint32_t>(result));if(!global_bytes(memory.load(GuestAddress{buffer+128},bits),op.entries.at(op.index).bytes))return finish(clip_tymed);
        std::vector<GuestCallback> releases{{resolve("ole32.dll","ReleaseStgMedium").value,{buffer+64}},{com_method(memory,op.storage,2),{op.storage}},{com_method(memory,op.lock_bytes,2),{op.lock_bytes}}};op.medium_owned=false;op.storage=op.lock_bytes=0;op.stage=Stage::ReleaseMedium;start_callbacks(state,memory,std::move(releases),Continuation::OleClipboardOperation,token);return {};}
    if(op.stage==Stage::ReleaseMedium){++op.index;op.type=0;op.medium=0;return next_medium();}
    throw HostCallFault("OLE 剪貼簿操作階段不合法");
    }catch(const MemoryFault&){return finish(clip_pointer);}catch(const std::bad_alloc&){return finish(clip_memory);}
}
