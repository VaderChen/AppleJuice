        // SetupAPI 的 INF 安裝與解壓：使用者 DIRID、壓縮資訊、解壓或複製、Cabinet 逐檔回呼、單檔安裝、安裝區段排入佇列、
        // 重新開機提示、安裝來源清單與磁碟空間清單。複製、解壓與版本比較沿用檔案佇列的 install_copy／expand_source／version_compare。
        if(name.starts_with("SetupSetDirectoryId") || name.starts_with("SetupGetFileCompressionInfo") || name.starts_with("SetupDecompressOrCopyFile") ||
           name.starts_with("SetupIterateCabinet") || name.starts_with("SetupInstallFile") || name=="SetupPromptReboot" ||
           name.starts_with("SetupAddToSourceList") || name.starts_with("SetupSetSourceList") || name.starts_with("SetupAddInstallSectionToDiskSpaceList")){
            const bool unicode=!name.ends_with('A');const unsigned chars=unicode?2U:1U;
            const auto encode=[unicode](const std::u16string& value){
                std::vector<std::uint8_t> bytes;if(unicode)bytes=utf16_bytes(value);else{const auto narrow=utf16_to_ansi(value);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}return bytes;};
            const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
            const auto leaf_of=[](const std::u16string& path){const auto slash=path.find_last_of(u"\\/");return slash==std::u16string::npos?path:path.substr(slash+1);};
            const auto store32=[&](std::uint64_t at,std::uint64_t value){memory.check(GuestAddress{at},4,Access::Write);memory.store(GuestAddress{at},32,value);};
            if(name.starts_with("SetupSetDirectoryId")){
                // (InfHandle, Id, Directory)：Id 0 且 Directory 為 NULL 清除全部使用者 DIRID；Directory 為 NULL 時移除該 Id；使用者 DIRID 介於 0x8000～0xfffe。
                auto& handle=find_handle(arg(0));const auto id=u32(1);
                if(!id){if(arg(2))return fail(87);handle.dirids.clear();return succeed();}
                if(id<0x8000 || id>0xfffe)return fail(87);
                if(!arg(2)){handle.dirids.erase(id);return succeed();}
                auto directory=full_path(text_arg(2));while(directory.size()>3 && (directory.back()==u'\\' || directory.back()==u'/'))directory.pop_back();
                handle.dirids[id]=directory;return succeed();
            }
            if(name.starts_with("SetupGetFileCompressionInfo")){
                // 非 Ex：(SourceFileName, ActualSourceFileName*, SourceFileSize, TargetFileSize, CompressionType) 回傳 Win32 錯誤碼，實際檔名以 LocalAlloc 配置。
                // Ex：(SourceFileName, Buffer, BufferLen, RequiredBufferLen, SourceFileSize, TargetFileSize, CompressionType) 回傳 BOOL。
                const bool extended=name.starts_with("SetupGetFileCompressionInfoEx");
                const auto error_code=[&](std::uint32_t code)->std::optional<std::uint32_t>{last_error_=code;return finish(code);};
                const auto report=[&](std::uint32_t code){return extended?(code?fail(code):succeed()):error_code(code);};
                if(!arg(0) || (extended?(!arg(4) || !arg(5) || !arg(6)):(!arg(1) || !arg(2) || !arg(3) || !arg(4))))return report(87);
                const auto source=full_path(text_arg(0));const auto actual=actual_source(source);if(!actual)return report(2);
                const auto expanded=expand_source(*actual,leaf_of(source),false);if(expanded.error)return report(expanded.error);
                const auto bytes=encode(*actual);const unsigned sizes=extended?4:2;
                if(extended){
                    const auto required=bytes.size()/chars;if(arg(3))store32(arg(3),required);
                    if(arg(1)){if(u32(2)<required)return fail(122);memory.write(GuestAddress{arg(1)},bytes);}
                }else{
                    const auto block=nested_service(state,memory,"kernel32.dll","LocalAlloc",{0x40,bytes.size()});if(!block)return error_code(8);
                    memory.write(GuestAddress{block},bytes);memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.store(GuestAddress{arg(1)},width,block);
                }
                store32(arg(sizes),std::min<std::uint64_t>(expanded.source_size,UINT32_MAX));store32(arg(sizes+1),std::min<std::uint64_t>(expanded.target_size,UINT32_MAX));store32(arg(sizes+2),expanded.type);
                return report(0);
            }
            if(name.starts_with("SetupDecompressOrCopyFile")){
                // (SourceFileName, TargetFileName, CompressionType)：回傳 Win32 錯誤碼；CompressionType 為 NULL 時自動判斷（含壓縮名稱），FILE_COMPRESSION_NONE 直接複製。
                const auto done=[&](std::uint32_t code)->std::optional<std::uint32_t>{last_error_=code;return finish(code);};
                if(!arg(0) || !arg(1))return done(87);const auto source=full_path(text_arg(0)),target=full_path(text_arg(1));
                if(arg(2)){const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));if(type>3)return done(87);
                    if(!type)return done(filesystem_.copy(host_path(source),host_path(target),false));}
                return done(install_copy(source,target,0,false));
            }
            if(name.starts_with("SetupIterateCabinet")){
                // (CabinetFile, Reserved, MsgHandler, Context)：每個成員先送 SPFILENOTIFY_FILEINCABINET（FILE_IN_CABINET_INFO，Param2 為 Cabinet 路徑），
                // 回呼填入 FullTargetName 並回 FILEOP_DOIT 才解出，接著送 SPFILENOTIFY_FILEEXTRACTED（FILEPATHS）；FILEOP_ABORT 或解出通知回傳錯誤時中止。
                if(!arg(0) || arg(1) || !arg(2))return fail(87);const auto path=full_path(text_arg(0));
                std::shared_ptr<const std::vector<std::uint8_t>> bytes;try{bytes=driver_store::read_file(filesystem_,path);}catch(const driver_store::Error& error){return fail(error.code);}
                auto archive=cabinet::parse(*bytes);if(!archive)return fail(13);
                const auto handler=arg(2),context=arg(3);
                return nested_continuation(state,memory,"SetupIterateCabinet",
                    [this,service,bytes,cab=std::move(*archive),path,handler,context,encode,unicode](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);
                        std::map<std::size_t,cabinet::Data> folders;bool aborted=false;std::uint32_t failure=0;
                        for(const auto& member:cab.files){
                            if(member.folder>=0xfffdU)continue; // 跨卷成員不在本卷完整
                            std::vector<std::uint64_t> blocks;
                            const auto put=[&](const std::u16string& value)->std::uint64_t{const auto data=encode(value);const auto at=allocate_heap(m,data.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},data);return at;};
                            const auto zero=[&](std::size_t length)->std::uint64_t{const auto at=allocate_heap(m,length);if(!at)throw std::bad_alloc{};blocks.push_back(at);std::vector<std::uint8_t> fill(length);m.write(GuestAddress{at},fill);return at;};
                            const auto release=[&]{for(const auto block:blocks)(void)free_heap(m,block,s);blocks.clear();};
                            const auto member_name=utf8_to_utf16_lossy(member.name);
                            const auto info=zero(size+14+260U*(unicode?2U:1U)+8);
                            m.store(GuestAddress{info},bits,put(member_name));m.store(GuestAddress{info+size},32,member.size);m.store(GuestAddress{info+size+8},16,member.date);
                            m.store(GuestAddress{info+size+10},16,member.time);m.store(GuestAddress{info+size+12},16,member.attributes&0x7fU);
                            const auto answer=co_await service_callback::call(handler,{context,0x11,info,put(path)});
                            if(answer==0){failure=static_cast<std::uint32_t>(m.load(GuestAddress{info+size+4},32));if(!failure)failure=1223;aborted=true;release();break;}
                            if(answer!=1){release();continue;}
                            const auto target_at=info+size+14;
                            const auto target=unicode?read_utf16(m,GuestAddress{target_at},260):ansi_to_utf16(read_ansi(m,target_at,260));
                            std::uint32_t error=0;
                            if(target.empty())error=87;
                            else{
                                if(!folders.contains(member.folder))folders.emplace(member.folder,cabinet::folder_data(*bytes,cab,member.folder));
                                const auto& folder=folders.at(member.folder);
                                if(folder.error)error=folder.error;
                                else if(std::size_t{member.offset}+member.size>folder.bytes.size())error=13;
                                else{
                                    auto opened=filesystem_.open_host(utf16_to_utf8(full_windows_environment_path(target,runtime_.current_directory,runtime_.environment)),0x40000000U,2,0x80,3);
                                    if(opened.error)error=opened.error;
                                    else{const auto written=opened.file->write(std::span(folder.bytes).subspan(member.offset,member.size),0);error=written.error?written.error:opened.file->truncate(member.size);}
                                }
                            }
                            const auto paths=zero(2*size+8);m.store(GuestAddress{paths},bits,put(target.empty()?member_name:target));m.store(GuestAddress{paths+size},bits,put(path));m.store(GuestAddress{paths+2*size},32,error);
                            const auto verdict=co_await service_callback::call(handler,{context,0x13,paths,0});release();
                            if(verdict){failure=verdict;aborted=true;break;}
                        }
                        last_error_=aborted?failure:0;inner.return_to(s,aborted?0:1);co_return std::optional<std::uint32_t>{};
                    });
            }
            if(name.starts_with("SetupInstallFile") && !name.starts_with("SetupInstallFilesFromInfSection")){
                // (InfHandle, InfContext, SourceFile, SourcePathRoot, DestinationName, CopyStyle, CopyMsgHandler, Context[, FileWasInUse])。
                // 有 INF 時來源依 [SourceDisksFiles]／[SourceDisksNames]，目的目錄依 InfContext 所在區段的 [DestinationDirs]；InfContext 的行為 目的[,來源]。
                // 沒有 INF 時 SourceFile（可接在 SourcePathRoot 後）與 DestinationName 都是完整路徑。因旗標或回呼否決而未複製時回 FALSE 且 GetLastError 為 NO_ERROR。
                const bool extended=name.starts_with("SetupInstallFileEx");const auto style=u32(5);
                if(extended && arg(8))store32(arg(8),0);
                std::u16string source,target;
                if(arg(0)){
                    auto& handle=find_handle(arg(0));std::u16string file,destination,section;
                    if(arg(1)){
                        const auto context=read_context(arg(1));auto [owner_handle,owner,line]=context_line(context);
                        if(line.fields.empty())return fail(87);destination=field_text(owner_handle,owner,line.fields[0]);
                        file=line.fields.size()>1?field_text(owner_handle,owner,line.fields[1]):std::u16string{};if(file.empty())file=destination;
                        section=owner_handle.section_names[context.section];
                    }else{if(!arg(2))return fail(87);file=text_arg(2);destination=arg(4)?text_arg(4):file;}
                    State::QueuedCopy copy;copy.file=file;source_disk(handle,arg(0),file,copy);
                    if(style&copy_source_absolute)source=full_path(file);
                    else if(style&copy_sourcepath_absolute)source=join(full_path(copy.path),file);
                    else source=full_path(join(join(arg(3)?text_arg(3):std::u16string{},copy.path),file));
                    target=join(destination_of(handle,arg(0),section),destination);
                }else{
                    if(!arg(2) || !arg(4))return fail(87);
                    source=full_path(arg(3) && !(style&copy_source_absolute)?join(text_arg(3),text_arg(2)):text_arg(2));target=full_path(text_arg(4));
                }
                const bool present=file_present(target);bool perform=true;std::uint32_t ask=0;
                if((style&copy_replace_only) && !present)perform=false;
                if(perform && present){
                    if(style&copy_force_no_overwrite)perform=false;
                    else if(style&copy_no_overwrite)ask=0x20000;
                    if(perform && !ask && (style&(copy_newer_only|copy_force_newer)))perform=version_compare(source,target)>0;
                    else if(perform && !ask && (style&copy_newer) && version_compare(source,target)<=0)ask=0x40000;
                }
                if(!perform || (ask && !arg(6))){last_error_=0;return finish(0);}
                const auto finish_copy=[this,install_copy,source,target,style]()->std::uint32_t{
                    const auto error=install_copy(source,target,style,true);if(!error && (style&0x1U))(void)filesystem_.remove(utf16_to_utf8(source),false);return error;};
                if(!ask){const auto error=finish_copy();return error?fail(error):succeed();}
                const auto handler=arg(6),context=arg(7);
                return nested_continuation(state,memory,"SetupInstallFile",[this,service,handler,context,ask,source,target,style,encode,finish_copy](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall inner(s,m,service);const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);std::vector<std::uint64_t> blocks;
                    const auto put=[&](const std::u16string& value)->std::uint64_t{const auto data=encode(value);const auto at=allocate_heap(m,data.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},data);return at;};
                    const auto paths=allocate_heap(m,2*size+8);if(!paths)throw std::bad_alloc{};blocks.push_back(paths);
                    m.store(GuestAddress{paths},bits,put(target));m.store(GuestAddress{paths+size},bits,put(source));m.store(GuestAddress{paths+2*size},32,0);m.store(GuestAddress{paths+2*size+4},32,style);
                    const auto allowed=co_await service_callback::call(handler,{context,ask,paths,0});
                    for(const auto block:blocks)(void)free_heap(m,block,s);
                    std::uint32_t error=0;if(allowed)error=finish_copy();
                    last_error_=error;inner.return_to(s,allowed && !error?1:0);co_return std::optional<std::uint32_t>{};});
            }
            if(name.starts_with("SetupInstallFilesFromInfSection")){
                // (InfHandle, LayoutInfHandle, FileQueue, SectionName, SourceRootPath, CopyFlags)
                auto& handle=find_handle(arg(0));auto& layout=arg(1)?find_handle(arg(1)):handle;(void)queue_of(arg(2));if(!arg(3))return fail(87);
                queue_install_section(arg(2),handle,arg(0),layout,arg(1)?arg(1):arg(0),text_arg(3),text_arg(4),u32(5));return succeed();
            }
            if(name=="SetupPromptReboot"){
                // (FileQueue, Owner, ScanOnly)：提交時不把檔案操作延後到重新開機（使用中的目的檔直接走錯誤通知），因此不需要重新開機，回傳 0。
                if(arg(0) && arg(0)!=invalid_handle && !queues.contains(arg(0)))return fail(6,UINT32_MAX);
                last_error_=0;return finish(0);
            }
            if(name.starts_with("SetupAddToSourceList") || name.starts_with("SetupSetSourceList")){
                // Add：(Flags, Source)；Set：(Flags, SourceList, SourceCount)。SRCLIST_TEMPORARY 只存在本程序；SRCLIST_SYSTEM（SYSIFADMIN 以系統管理員視同 SYSTEM）與 SRCLIST_USER
                // 存於 HKLM／HKCU 的 Software\Microsoft\Windows\CurrentVersion\Setup "Installation Sources"（REG_MULTI_SZ）。預設去掉結尾的平台子目錄，SRCLIST_NOSTRIPPLATFORM 保留；
                // Add 預設放到最前面、SRCLIST_APPEND 放到最後，相同路徑（不分大小寫）只保留一份；Set 整份取代。
                const bool add=name.starts_with("SetupAddToSourceList");const auto flags=u32(0);
                const bool temporary=flags&1U,machine_list=flags&0x50U,user_list=flags&0x20U;if(!temporary && !machine_list && !user_list)return fail(87);
                const auto strip=[&](std::u16string path){
                    while(path.size()>3 && (path.back()==u'\\' || path.back()==u'/'))path.pop_back();
                    if(!(flags&0x400U))for(const auto* suffix:{u"\\i386",u"\\amd64",u"\\arm64",u"\\ia64",u"\\x86",u"\\alpha",u"\\mips",u"\\ppc"}){
                        const std::u16string_view tail(suffix);if(path.size()>tail.size() && fold(path.substr(path.size()-tail.size()))==tail){path.resize(path.size()-tail.size());break;}}
                    return path;};
                std::vector<std::u16string> items;
                if(add){if(!arg(1))return fail(87);items.push_back(strip(text_arg(1)));}
                else{const auto count=u32(2);if(count && !arg(1))return fail(87);if(count>4096)return fail(87);
                    for(std::uint32_t i=0;i<count;++i){const auto at=memory.load(GuestAddress{arg(1)+i*unit},width);if(!at)return fail(87);items.push_back(strip(read_text(at)));}}
                const auto merge=[&](std::vector<std::u16string> list){
                    if(!add)return items;const auto key=fold(items.front());std::erase_if(list,[&](const std::u16string& item){return fold(item)==key;});
                    if(flags&0x200U)list.push_back(items.front());else list.insert(list.begin(),items.front());return list;};
                if(temporary){setup_inf_->temporary_sources=merge(setup_inf_->temporary_sources.value_or(std::vector<std::u16string>{}));return succeed();}
                GuestScratch scratch(*this,memory,state);
                const auto subkey=scratch.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\Setup"),value_name=scratch.text(u"Installation Sources"),key_at=scratch.allocate(unit),size_at=scratch.allocate(4);
                if(!subkey || !value_name || !key_at || !size_at)throw std::bad_alloc{};
                const auto host_call=[&](std::string_view symbol,std::initializer_list<std::uint64_t> values){return static_cast<std::uint32_t>(nested_service(state,memory,"advapi32.dll",symbol,values));};
                for(const bool machine:{true,false}){
                    if(machine?!machine_list:!user_list)continue;
                    const std::uint64_t root=machine?(x86_?0x80000002ULL:0xffffffff80000002ULL):(x86_?0x80000001ULL:0xffffffff80000001ULL);
                    memory.store(GuestAddress{key_at},width,0);
                    if(const auto error=host_call("RegCreateKeyExW",{root,subkey,0,0,0,0xf003f,0,key_at,0}))return fail(error);
                    const auto key=memory.load(GuestAddress{key_at},width);std::vector<std::u16string> list;
                    memory.store(GuestAddress{size_at},32,0);
                    if(!host_call("RegQueryValueExW",{key,value_name,0,0,0,size_at})){
                        const auto bytes=static_cast<std::uint32_t>(memory.load(GuestAddress{size_at},32));
                        if(bytes && bytes<=65536){const auto data=scratch.allocate(bytes+2);if(!data)throw std::bad_alloc{};
                            if(!host_call("RegQueryValueExW",{key,value_name,0,0,data,size_at})){
                                std::u16string text;for(std::uint32_t i=0;i+1<bytes;i+=2)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{data+i},16)));
                                for(std::size_t begin=0;begin<text.size();){const auto end=text.find(u'\0',begin);const auto item=text.substr(begin,end==std::u16string::npos?std::u16string::npos:end-begin);if(!item.empty())list.push_back(item);if(end==std::u16string::npos)break;begin=end+1;}
                            }}
                    }
                    list=merge(std::move(list));std::u16string joined;for(const auto& item:list){joined+=item;joined.push_back(0);}joined.push_back(0);
                    auto bytes=utf16_bytes(joined);bytes.resize(joined.size()*2);const auto data=scratch.allocate(bytes.size());if(!data)throw std::bad_alloc{};memory.write(GuestAddress{data},bytes);
                    const auto error=host_call("RegSetValueExW",{key,value_name,0,7,data,bytes.size()});(void)host_call("RegCloseKey",{key});if(error)return fail(error);
                }
                return succeed();
            }
            if(name.starts_with("SetupAddInstallSectionToDiskSpaceList")){
                // (DiskSpace, InfHandle, LayoutInfHandle, SectionName, Reserved1, Reserved2)：安裝區段的複製目的檔以 [SourceDisksFiles] 的大小欄計入，刪除以目前檔案大小計入。
                if(arg(4) || u32(5) || !arg(3))return fail(87);const auto list=setup_inf_->disk_spaces.find(arg(0));if(list==setup_inf_->disk_spaces.end())return fail(6);
                auto& handle=find_handle(arg(1));auto& layout=arg(2)?find_handle(arg(2)):handle;const auto layout_inf=arg(2)?arg(2):arg(1);
                // 借用暫時佇列收集目的檔（不配置 handle，結束即移除）。
                constexpr std::uint64_t scratch_queue=UINT64_MAX;queues[scratch_queue]={};
                try{queue_install_section(scratch_queue,handle,arg(1),layout,layout_inf,text_arg(3),{},0);}catch(...){queues.erase(scratch_queue);throw;}
                const auto collected=std::move(queues[scratch_queue]);queues.erase(scratch_queue);
                for(const auto& copy:collected.copies){
                    const auto target=join(copy.target_directory,copy.target_file.empty()?leaf_of(copy.file):copy.target_file);std::int64_t size=0;
                    if(const auto entry=first_line(layout,layout_inf,u"SourceDisksFiles",copy.file,true))if(const auto value=driver_store::number(line_field(*entry,2)))size=static_cast<std::int64_t>(*value);
                    list->second.files[fold(target)]={target,size,0};
                }
                for(const auto& item:collected.deletes){
                    const auto info=filesystem_.information(host_path(item.target));
                    list->second.files[fold(item.target)]={item.target,info.error?0:static_cast<std::int64_t>(info.metadata.size),2};
                }
                return succeed();
            }
        }
