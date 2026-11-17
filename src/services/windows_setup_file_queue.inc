        // SetupAPI 檔案佇列（HSPFILEQ）：複製、更名、刪除三個子佇列，以及掃描、提交、預設佇列回呼與錯誤／換片提示對話框。
        // 提交依刪除→更名→複製的順序通知回呼；複製依來源媒體（根目錄、描述、標記檔）分組，每組先送 SPFILENOTIFY_NEEDMEDIA。
        // FILEPATHS／SOURCE_MEDIA 與字串依 A／W 版本編碼（無字尾的 SetupCommitFileQueue 為 W）；回呼與對話框以協程呼叫客體入口。
        constexpr std::uint32_t copy_delete_source=0x1,copy_replace_only=0x2,copy_newer=0x4,copy_no_overwrite=0x8,copy_source_absolute=0x40,copy_sourcepath_absolute=0x80,
            copy_force_no_overwrite=0x1000,copy_force_newer=0x2000,copy_noskip=0x400,copy_nobrowse=0x8000,copy_newer_only=0x10000;
        constexpr std::uint32_t op_abort=0,op_doit=1,op_skip=2,op_newpath=4,prompt_success=0,prompt_cancel=1,prompt_skip=2,prompt_small=3;
        constexpr std::uint32_t max_path=260,cancelled=1223,non_interactive=1459;
        auto& queues=setup_inf_->file_queues;
        const auto queue_of=[&](std::uint64_t value)->State::FileQueue&{const auto found=queues.find(value);if(found==queues.end())throw Failure{6};return found->second;};
        const auto join=[](std::u16string left,std::u16string_view right){
            while(!right.empty() && (right.front()==u'\\' || right.front()==u'/'))right.remove_prefix(1);
            if(right.empty())return left;if(left.empty())return std::u16string(right);
            if(left.back()!=u'\\' && left.back()!=u'/')left+=u'\\';left+=right;return left;
        };
        const auto text_arg=[&](unsigned i){return read_text(arg(i));};
        // [SourceDisksFiles] 的 diskid 與子目錄對應 [SourceDisksNames] 的描述、標記檔與磁碟內路徑；未列出的檔案位於來源根目錄。
        const auto source_disk=[&](State::Handle& handle,std::uint64_t inf,const std::u16string& file,State::QueuedCopy& copy){
            const auto entry=first_line(handle,inf,u"SourceDisksFiles",file,true);if(!entry)return;
            std::u16string path;
            if(const auto id=driver_store::number(line_field(*entry,0)))
                if(const auto names=first_line(handle,inf,u"SourceDisksNames",utf8_to_utf16(std::to_string(*id)),true)){
                    if(copy.description.empty())copy.description=line_field(*names,0);if(copy.tagfile.empty())copy.tagfile=line_field(*names,1);path=line_field(*names,3);}
            copy.path=join(path,line_field(*entry,1));
        };
        // 區段各行的欄位（跨附加檔案）；區段不存在為 ERROR_SECTION_NOT_FOUND。
        const auto section_rows=[&](State::Handle& handle,const std::u16string& section){
            const auto id=section_id(handle,section);if(!section_exists(handle,id))throw Failure{section_not_found};
            std::vector<std::vector<std::u16string>> rows;
            for(std::size_t file=0;file<handle.files.size();++file)if(const auto* lines=lines_of(handle,file,id))for(const auto& line:*lines){
                std::vector<std::u16string> row;if(line.keyed)row.push_back(key_text(line));
                for(const auto& field:line.fields)row.push_back(field_text(handle,handle.files[file],field));
                if(!row.empty() && !(row.size()==1 && row.front().empty()))rows.push_back(std::move(row));
            }
            return rows;
        };
        const auto add_copy=[&](std::uint64_t queue,State::QueuedCopy copy){
            if(copy.file.empty() || copy.target_directory.empty())throw Failure{87};
            copy.target_directory=full_path(copy.target_directory);queue_of(queue).copies.push_back(std::move(copy));
        };
        // INF 複製行的 COPYFLG_* 轉成 SP_COPY_*（NOVERSIONCHECK 取消版本比較）。
        const auto copy_flags=[](std::uint32_t style,std::uint32_t flags){
            static constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,10> map{{{0x1,0x4000},{0x2,0x400},{0x8,0x200},{0x10,0x1000},{0x20,0x2000},{0x40,0x10000},{0x400,0x2},{0x800,0x10},{0x1000,0x80000},{0x2000,0x100000}}};
            for(const auto& [from,to]:map)if(flags&from)style|=to;if(flags&0x4)style&=~0x4U;return style;};
        // 複製區段（SetupQueueCopySection 與 SetupInstallFilesFromInfSection 共用）：每行 目的檔[,來源檔][,暫存名][,COPYFLG]；目的目錄依 [DestinationDirs]。
        const auto queue_copy_section=[&](std::uint64_t queue,const std::u16string& root,State::Handle& handle,std::uint64_t inf,State::Handle& layout,std::uint64_t layout_inf,const std::u16string& section,std::uint32_t style){
            (void)queue_of(queue);const auto target=destination_of(handle,inf,section);
            for(const auto& row:section_rows(handle,section)){
                State::QueuedCopy copy;copy.root=root;copy.target_file=row[0];copy.file=row.size()>1 && !row[1].empty()?row[1]:row[0];copy.target_directory=target;copy.style=style;
                if(row.size()>3 && !row[3].empty()){const auto flags=driver_store::number(row[3]);if(!flags)throw Failure{13};copy.style=copy_flags(style,static_cast<std::uint32_t>(*flags));}
                source_disk(layout,layout_inf,copy.file,copy);add_copy(queue,std::move(copy));
            }
        };
        // 刪除／更名區段：刪除行為「檔名」，更名行為「新名稱,舊名稱」，都在 [DestinationDirs] 指定的目錄。
        const auto queue_path_section=[&](std::uint64_t queue,State::Handle& handle,std::uint64_t inf,const std::u16string& section,bool rename){
            auto& target=queue_of(queue);const auto directory=destination_of(handle,inf,section);
            for(const auto& row:section_rows(handle,section)){
                if(rename){if(row.size()<2 || row[1].empty())throw Failure{13};target.renames.push_back({full_path(join(directory,row[1])),full_path(join(directory,row[0]))});}
                else target.deletes.push_back({{},full_path(join(directory,row[0]))});
            }
        };
        // 安裝區段的 CopyFiles／DelFiles／RenFiles（逗號分隔的區段名稱；CopyFiles 的 @檔名 為單一檔案，目的為 DefaultDestDir）排入佇列。
        const auto queue_install_section=[&](std::uint64_t queue,State::Handle& handle,std::uint64_t inf,State::Handle& layout,std::uint64_t layout_inf,const std::u16string& section,const std::u16string& root,std::uint32_t style){
            for(const auto& row:section_rows(handle,section)){
                if(row.empty())continue;const auto key=fold(row[0]);
                for(std::size_t i=1;i<row.size();++i){
                    const auto& value=row[i];if(value.empty())continue;
                    if(key==u"copyfiles"){
                        if(value.front()==u'@'){
                            State::QueuedCopy copy;copy.root=root;copy.file=value.substr(1);copy.target_file=copy.file;copy.target_directory=destination_of(handle,inf,u"");copy.style=style;
                            source_disk(layout,layout_inf,copy.file,copy);add_copy(queue,std::move(copy));
                        }else queue_copy_section(queue,root,handle,inf,layout,layout_inf,value,style);
                    }else if(key==u"delfiles")queue_path_section(queue,handle,inf,value,false);
                    else if(key==u"renfiles")queue_path_section(queue,handle,inf,value,true);
                }
            }
        };
        // 以下宿主端檔案操作只捕獲 this，可複製進協程（提交與 SetupInstallFile 共用）。
        const auto host_path=[](const std::u16string& path){return utf16_to_utf8(path);};
        const auto file_present=[this](const std::u16string& path){const auto info=filesystem_.information(utf16_to_utf8(path));return !info.error && !(info.metadata.attributes&0x10U);};
        // 版本資源（VS_FIXEDFILEINFO）比較；任一方沒有版本時改比最後寫入時間。來源較新 >0、相同 0、較舊 <0。
        const auto version_compare=[this](const std::u16string& source,const std::u16string& target)->int{
            const auto version=[&](const std::u16string& path)->std::optional<std::uint64_t>{
                try{const auto bytes=driver_store::read_file(filesystem_,path);static constexpr std::array<std::uint8_t,4> signature{0xbd,0x04,0xef,0xfe};
                    const auto found=std::search(bytes->begin(),bytes->end(),signature.begin(),signature.end());if(found==bytes->end() || bytes->end()-found<16)return std::nullopt;
                    std::uint64_t value=0;for(unsigned i=0;i<4;++i){value|=std::uint64_t{found[8+i]}<<(32+8*i);value|=std::uint64_t{found[12+i]}<<(8*i);}return value;}
                catch(const driver_store::Error&){return std::nullopt;}};
            const auto a=version(source),b=version(target);if(a && b)return *a>*b?1:*a<*b?-1:0;
            const auto ta=filesystem_.information(utf16_to_utf8(source)).metadata.modified,tb=filesystem_.information(utf16_to_utf8(target)).metadata.modified;return ta>tb?1:ta<tb?-1:0;};
        // 來源檔：本身不存在時依序改找最後一字元換成 '_'、'$' 的壓縮名稱。
        const auto actual_source=[file_present](const std::u16string& path)->std::optional<std::u16string>{
            if(file_present(path))return path;if(path.empty())return std::nullopt;
            const auto slash=path.find_last_of(u"\\/");const auto dot=path.find_last_of(u'.');
            for(const auto mark:{u'_',u'$'}){auto candidate=path;if(dot!=std::u16string::npos && (slash==std::u16string::npos || dot>slash) && dot+1<path.size())candidate.back()=mark;else candidate+=std::u16string(u".")+mark;if(file_present(candidate))return candidate;}
            return std::nullopt;};
        // 壓縮類型（FILE_COMPRESSION_NONE 0、WINLZA 1、MSZIP 2、NTCAB 3）與展開後大小；Cabinet 取名稱與目的檔名相同的成員，只有一個成員時取它。
        struct Expanded {std::uint32_t type{},error{};std::uint64_t source_size{},target_size{};std::optional<std::vector<std::uint8_t>> bytes;};
        const auto expand_source=[this](const std::u16string& path,const std::u16string& wanted,bool decode)->Expanded{
            Expanded result;std::shared_ptr<const std::vector<std::uint8_t>> bytes;
            try{bytes=driver_store::read_file(filesystem_,path);}catch(const driver_store::Error& error){result.error=error.code;return result;}
            result.source_size=bytes->size();result.target_size=bytes->size();
            if(bytes->size()>=14 && std::equal(szdd_magic.begin(),szdd_magic.end(),bytes->begin())){
                result.type=1;result.target_size=(*bytes)[10]|(std::uint32_t{(*bytes)[11]}<<8)|(std::uint32_t{(*bytes)[12]}<<16)|(std::uint32_t{(*bytes)[13]}<<24);
                if(decode){result.bytes=lz_expand_file(*bytes);if(!result.bytes)result.error=13;}
                return result;
            }
            if(const auto archive=cabinet::parse(*bytes)){
                result.type=!archive->folders.empty() && (archive->folders.front().compression&0xfU)==1?2:3;
                const cabinet::File* member=nullptr;const auto key=fold(wanted);
                for(const auto& file:archive->files){const auto name=utf8_to_utf16_lossy(file.name);const auto slash=name.find_last_of(u"\\/");if(fold(slash==std::u16string::npos?name:name.substr(slash+1))==key){member=&file;break;}}
                if(!member && (archive->files.size()==1 || (!decode && !archive->files.empty())))member=&archive->files.front();
                if(!member){result.error=2;return result;}
                result.target_size=member->size;
                if(decode){auto data=cabinet::file_data(*bytes,*archive,*member);if(data.error)result.error=data.error;else result.bytes=std::move(data.bytes);}
                return result;
            }
            if(decode)result.bytes=*bytes;return result;};
        // 建立目的檔所在的各層目錄。
        const auto make_target_directories=[this](const std::u16string& file){
            const auto slash=file.find_last_of(u'\\');if(slash==std::u16string::npos)return;const auto directory=file.substr(0,slash);
            for(std::size_t at=directory.find(u'\\',3);;at=directory.find(u'\\',at+1)){
                const auto part=at==std::u16string::npos?directory:directory.substr(0,at);(void)filesystem_.create_directory(utf16_to_utf8(part));if(at==std::u16string::npos)break;}};
        // 複製單一檔案：未指定 SP_COPY_NODECOMP 時 LZ／Cabinet 來源先解壓再寫入目的檔；directories 為真時先建立目的目錄。
        const auto install_copy=[this,expand_source,make_target_directories,actual_source](const std::u16string& source,const std::u16string& target,std::uint32_t style,bool directories)->std::uint32_t{
            if(style&0x10U){if(directories)make_target_directories(target);return filesystem_.copy(utf16_to_utf8(source),utf16_to_utf8(target),false);}
            const auto found=actual_source(source);if(!found)return 2;if(directories)make_target_directories(target);
            const auto slash=target.find_last_of(u"\\/");auto expanded=expand_source(*found,slash==std::u16string::npos?target:target.substr(slash+1),true);
            if(expanded.error)return expanded.error;
            if(!expanded.type)return filesystem_.copy(utf16_to_utf8(*found),utf16_to_utf8(target),false);
            auto opened=filesystem_.open_host(utf16_to_utf8(target),0x40000000U,2,0x80,3);if(opened.error)return opened.error;
            const auto written=opened.file->write(*expanded.bytes,0);if(written.error)return written.error;return opened.file->truncate(expanded.bytes->size());};
        if(name.find("FileQueue")!=std::string_view::npos || name.starts_with("SetupQueue") || name.find("QueueCallback")!=std::string_view::npos ||
           name.starts_with("SetupPromptForDisk") || name.starts_with("SetupCopyError") || name.starts_with("SetupDeleteError") || name.starts_with("SetupRenameError") ||
           name=="pSetupGetQueueFlags" || name=="pSetupSetQueueFlags"){
            if(name=="SetupOpenFileQueue"){
                std::uint64_t value=0;while(!value || queues.contains(value)){value=allocate_heap(memory,16);if(!value)return fail(8,invalid_handle);}
                queues.emplace(value,State::FileQueue{});return succeed(value);
            }
            if(name=="SetupCloseFileQueue"){
                const auto found=queues.find(arg(0));if(found==queues.end())return fail(6);queues.erase(found);(void)free_heap(memory,arg(0),state);return succeed();
            }
            if(name=="SetupQueueCopyA" || name=="SetupQueueCopyW"){
                // (Queue, SourceRootPath, SourcePath, SourceFilename, SourceDescription, SourceTagfile, TargetDirectory, TargetFilename, CopyStyle)
                (void)queue_of(arg(0));State::QueuedCopy copy{text_arg(1),text_arg(2),text_arg(3),text_arg(4),text_arg(5),text_arg(6),text_arg(7),{},static_cast<std::uint32_t>(arg(8))};
                add_copy(arg(0),std::move(copy));return succeed();
            }
            if(name.starts_with("SetupQueueCopyIndirect")){
                // SP_FILE_COPY_PARAMS：cbSize、QueueHandle、七個字串、CopyStyle、LayoutInf、SecurityDescriptor（SDDL，複製後套用到目的檔）。
                if(!arg(0))return fail(87);const auto at=arg(0);if(memory.load(GuestAddress{at},32)!=12U*unit)return fail(87);
                const auto pointer=[&](unsigned slot){return memory.load(GuestAddress{at+slot*unit},width);};
                const auto queue=pointer(1);(void)queue_of(queue);
                State::QueuedCopy copy{read_text(pointer(2)),read_text(pointer(3)),read_text(pointer(4)),read_text(pointer(5)),read_text(pointer(6)),read_text(pointer(7)),read_text(pointer(8)),
                    read_text(pointer(11)),static_cast<std::uint32_t>(memory.load(GuestAddress{at+9*unit},32))};
                if(const auto layout=pointer(10);layout && !copy.file.empty()){auto& handle=find_handle(layout);auto located=copy;source_disk(handle,layout,copy.file,located);if(copy.path.empty())copy.path=located.path;copy.description=located.description;copy.tagfile=located.tagfile;}
                add_copy(queue,std::move(copy));return succeed();
            }
            if(name.starts_with("SetupQueueDefaultCopy")){
                // (Queue, InfHandle, SourceRootPath, SourceFilename, TargetFilename, CopyStyle)：來源依 [SourceDisksFiles]，目的為 DefaultDestDir。
                (void)queue_of(arg(0));auto& handle=find_handle(arg(1));if(!arg(3))return fail(87);
                State::QueuedCopy copy;copy.root=text_arg(2);copy.file=text_arg(3);copy.target_file=arg(4)?text_arg(4):copy.file;copy.style=static_cast<std::uint32_t>(arg(5));
                source_disk(handle,arg(1),copy.file,copy);copy.target_directory=destination_of(handle,arg(1),u"");add_copy(arg(0),std::move(copy));return succeed();
            }
            if(name.starts_with("SetupQueueCopySection")){
                // (Queue, SourceRootPath, InfHandle, ListInfHandle, Section, CopyStyle)
                auto& handle=find_handle(arg(2));auto& layout=arg(3)?find_handle(arg(3)):handle;if(!arg(4))return fail(87);
                queue_copy_section(arg(0),text_arg(1),handle,arg(2),layout,arg(3)?arg(3):arg(2),text_arg(4),static_cast<std::uint32_t>(arg(5)));return succeed();
            }
            if(name=="SetupQueueDeleteA" || name=="SetupQueueDeleteW"){
                // (Queue, PathPart1, PathPart2)：PathPart2 為 NULL 時 PathPart1 是完整路徑。
                auto& queue=queue_of(arg(0));if(!arg(1))return fail(87);queue.deletes.push_back({{},full_path(arg(2)?join(text_arg(1),text_arg(2)):text_arg(1))});return succeed();
            }
            if(name=="SetupQueueRenameA" || name=="SetupQueueRenameW"){
                // (Queue, SourcePath, SourceFilename, TargetPath, TargetFilename)：TargetPath 為 NULL 時留在來源目錄。
                auto& queue=queue_of(arg(0));if(!arg(2) || !arg(4))return fail(87);const auto directory=text_arg(1);
                queue.renames.push_back({full_path(join(directory,text_arg(2))),full_path(join(arg(3)?text_arg(3):directory,text_arg(4)))});return succeed();
            }
            if(name.starts_with("SetupQueueDeleteSection") || name.starts_with("SetupQueueRenameSection")){
                // (Queue, InfHandle, ListInfHandle, Section)
                auto& handle=find_handle(arg(1));if(!arg(3))return fail(87);queue_path_section(arg(0),handle,arg(1),text_arg(3),name.starts_with("SetupQueueRenameSection"));return succeed();
            }
            if(name=="SetupGetFileQueueCount"){
                // (Queue, SubQueueFileOp, NumOperations)：FILEOP_COPY 0、FILEOP_RENAME 1、FILEOP_DELETE 2。
                const auto& queue=queue_of(arg(0));const auto op=static_cast<std::uint32_t>(arg(1));if(op>2 || !arg(2))return fail(87);
                memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,op==0?queue.copies.size():op==1?queue.renames.size():queue.deletes.size());return succeed();
            }
            if(name=="SetupGetFileQueueFlags"){const auto& queue=queue_of(arg(0));if(!arg(1))return fail(87);memory.check(GuestAddress{arg(1)},4,Access::Write);memory.store(GuestAddress{arg(1)},32,queue.flags);return succeed();}
            if(name=="SetupSetFileQueueFlags"){
                // (Queue, FlagMask, Flags)：SPQ_FLAG_BACKUP_AWARE、ABORT_IF_UNSIGNED、FILES_MODIFIED、DO_SHUFFLEMOVE。
                auto& queue=queue_of(arg(0));const auto mask=static_cast<std::uint32_t>(arg(1)),flags=static_cast<std::uint32_t>(arg(2));if(mask&~0xfU)return fail(87);
                queue.flags=(queue.flags&~mask)|(flags&mask);return succeed();
            }
            if(name=="pSetupGetQueueFlags"){const auto& queue=queue_of(arg(0));last_error_=0;return finish(queue.private_flags);}
            if(name=="pSetupSetQueueFlags"){queue_of(arg(0)).private_flags=static_cast<std::uint32_t>(arg(1));return succeed();}
            if(name.starts_with("SetupSetFileQueueAlternatePlatform")){
                // (Queue, SP_ALTPLATFORM_INFO, AlternateDefaultCatalogFile)：資訊為 NULL 時恢復原生平台；V1 20、V2 28 位元組，Platform 須為 VER_PLATFORM_WIN32_NT。
                auto& queue=queue_of(arg(0));
                if(!arg(1)){if(arg(2))return fail(87);queue.platform.clear();queue.catalog.clear();return succeed();}
                const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));if(size!=20 && size!=28)return fail(1784);
                if(memory.load(GuestAddress{arg(1)+4},32)!=2)return fail(87);
                queue.platform.resize(size);memory.read(GuestAddress{arg(1)},std::span<std::uint8_t>(queue.platform));queue.catalog=text_arg(2);return succeed();
            }
            if(name=="SetupInitDefaultQueueCallback" || name=="SetupInitDefaultQueueCallbackEx"){
                // Ex：(Owner, AlternateProgressWindow, ProgressMessage, Reserved1, Reserved2)；進度視窗為 INVALID_HANDLE_VALUE 時不送進度訊息。
                const bool extended=name.ends_with("Ex");State::QueueCallback callback{arg(0),extended?arg(1):0,extended?static_cast<std::uint32_t>(arg(2)):0,extended && arg(1)==invalid_handle};
                if(callback.quiet)callback.progress=0;
                std::uint64_t value=0;while(!value || setup_inf_->queue_callbacks.contains(value)){value=allocate_heap(memory,16);if(!value)return fail(8);}
                setup_inf_->queue_callbacks.emplace(value,callback);return succeed(value);
            }
            if(name=="SetupTermDefaultQueueCallback"){
                if(const auto found=setup_inf_->queue_callbacks.find(arg(0));found!=setup_inf_->queue_callbacks.end()){setup_inf_->queue_callbacks.erase(found);(void)free_heap(memory,arg(0),state);}
                return finish(0);
            }
            const bool unicode=!name.ends_with('A');
            const auto encode=[unicode](const std::u16string& value){
                std::vector<std::uint8_t> bytes;if(unicode)bytes=utf16_bytes(value);else{const auto narrow=utf16_to_utf8(value);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}return bytes;};
            const auto interactive=!(setup_inf_->global_flags&4U);
            if(name.starts_with("SetupPromptForDisk") || name.starts_with("SetupCopyError") || name.starts_with("SetupDeleteError") || name.starts_with("SetupRenameError")){
                // 提示與錯誤對話框以 MessageBoxW 呈現（中止／重試／略過）；回傳 DPROMPT_*。非互動模式回 DPROMPT_CANCEL 與 ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION。
                // PromptForDisk：(hwnd, Title, DiskName, PathToSource, FileSought, TagFile, Style, PathBuffer, Size, Required)；IDF_CHECKFIRST 先找檔案（含壓縮名稱），找到即不顯示。
                // CopyError：(hwnd, Title, DiskName, PathToSource, SourceFile, TargetPathFile, Win32Error, Style, PathBuffer, Size, Required)。
                // DeleteError：(hwnd, Title, File, Win32Error, Style)；RenameError：(hwnd, Title, SourceFile, TargetFile, Win32Error, Style)。
                const bool prompt=name.starts_with("SetupPromptForDisk"),copy_error=name.starts_with("SetupCopyError"),rename_error=name.starts_with("SetupRenameError");
                const auto owner=arg(0);const auto title=arg(1)?text_arg(1):std::u16string(prompt?u"Files Needed":copy_error?u"Copy Error":rename_error?u"Rename Error":u"Delete Error");
                const unsigned style_at=prompt?6:copy_error?7:rename_error?5:4;const auto style=static_cast<std::uint32_t>(arg(style_at));
                const auto path_source=prompt || copy_error?text_arg(3):std::u16string{};
                const unsigned buffer_at=prompt?7:8;const auto buffer=prompt || copy_error?arg(buffer_at):0,size=prompt || copy_error?arg(buffer_at+1):0,required_at=prompt || copy_error?arg(buffer_at+2):0;
                if(prompt && !arg(4))return fail(87,prompt_cancel);
                const auto sought=prompt?text_arg(4):std::u16string{},tagfile=prompt?text_arg(5):std::u16string{};
                // 輸出路徑：所需長度含 NUL；緩衝區不足回 DPROMPT_BUFFERTOOSMALL。
                const auto deliver=[buffer,size,required_at,encode,unicode](GuestMemory& m,const std::u16string& path)->std::uint32_t{
                    const auto bytes=encode(path);const auto required=unicode?bytes.size()/2:bytes.size();
                    if(required_at){m.check(GuestAddress{required_at},4,Access::Write);m.store(GuestAddress{required_at},32,required);}
                    if(!buffer)return prompt_success;if(static_cast<std::uint32_t>(size)<required)return prompt_small;m.write(GuestAddress{buffer},bytes);return prompt_success;};
                const auto present=[this](const std::u16string& directory,const std::u16string& file){
                    std::vector<std::u16string> names{file};
                    if(const auto dot=file.find_last_of(u'.');dot!=std::u16string::npos && dot+1<file.size()){auto compressed=file;compressed.back()=u'_';names.push_back(compressed);}else names.push_back(file+u"._");
                    for(const auto& candidate:names){
                        const auto full=full_windows_environment_path(directory.empty()?candidate:directory+(directory.back()==u'\\'?u"":u"\\")+candidate,runtime_.current_directory,runtime_.environment);
                        const auto info=filesystem_.information(utf16_to_utf8(full));if(!info.error && !(info.metadata.attributes&0x10U))return true;}
                    return false;};
                if(prompt && (style&0x100U) && (present(path_source,sought) || (!tagfile.empty() && present(path_source,tagfile)))){
                    const auto result=deliver(memory,path_source);last_error_=0;return finish(result);
                }
                if(!interactive){last_error_=non_interactive;return finish(prompt_cancel);}
                std::u16string message;
                const auto error_text=[&](unsigned at){return u"\n\nError "+utf8_to_utf16(std::to_string(static_cast<std::uint32_t>(arg(at))));};
                if(prompt){message=u"Insert the disk \""+(arg(2)?text_arg(2):std::u16string(u"source media"))+u"\" and click Retry.\n\nFile needed: "+sought+u"\nCopy files from: "+path_source;}
                else if(copy_error)message=u"The file \""+join(path_source,text_arg(4))+u"\" could not be copied to \""+text_arg(5)+u"\"."+error_text(6);
                else if(rename_error)message=u"The file \""+text_arg(2)+u"\" could not be renamed to \""+text_arg(3)+u"\"."+error_text(4);
                else message=u"The file \""+text_arg(2)+u"\" could not be deleted."+error_text(3);
                const auto box=resolve("user32.dll","MessageBoxW").value;const bool skippable=!(style&0x2U);
                return nested_continuation(state,memory,"SetupPromptDialog",
                    [this,service,box,owner,title,message,skippable,prompt,copy_error,path_source,sought,tagfile,deliver,present](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);std::uint32_t result=prompt_cancel;
                        for(;;){
                            std::uint32_t pressed=0;
                            {GuestScratch scratch(*this,m,s);const auto text=scratch.text(message),caption=scratch.text(title);if(!text || !caption)throw std::bad_alloc{};
                             pressed=co_await service_callback::call(box,{owner,text,caption,(skippable?2U:5U)|0x30U});}
                            if(pressed==4){ // IDRETRY
                                if(prompt && !present(path_source,sought) && (tagfile.empty() || !present(path_source,tagfile)))continue;
                                result=prompt || copy_error?deliver(m,path_source):prompt_success;break;
                            }
                            result=pressed==5?prompt_skip:prompt_cancel;break; // IDIGNORE；IDABORT／IDCANCEL
                        }
                        last_error_=result==prompt_cancel?cancelled:0;inner.return_to(s,result);co_return std::optional<std::uint32_t>{};
                    });
            }
            if(name=="SetupDefaultQueueCallbackA" || name=="SetupDefaultQueueCallbackW"){
                // (Context, Notification, Param1, Param2)：開始通知繼續、檔案操作照做、錯誤與換片轉交對應的提示函式；有進度視窗時以 SendMessage 回報。
                const auto notification=static_cast<std::uint32_t>(arg(1));const auto p1=arg(2),p2=arg(3);
                State::QueueCallback callback;if(const auto found=setup_inf_->queue_callbacks.find(arg(0));found!=setup_inf_->queue_callbacks.end())callback=found->second;
                const auto progress=[&](std::uint64_t wparam,std::uint64_t lparam,std::uint32_t value)->std::optional<std::uint32_t>{
                    if(!callback.progress){last_error_=0;return finish(value);}
                    const auto send=resolve("user32.dll","SendMessageW").value;
                    return nested_continuation(state,memory,"SetupQueueProgress",[service,send,callback,wparam,lparam,value](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);(void)co_await service_callback::call_value(send,{callback.progress,callback.message,wparam,lparam});inner.return_to(s,value);co_return std::optional<std::uint32_t>{};});
                };
                switch(notification){
                case 1:case 4:last_error_=0;return finish(1);                                   // STARTQUEUE／ENDSUBQUEUE
                case 2:last_error_=0;return finish(0);                                          // ENDQUEUE
                case 3:return progress(0,p2,1);                                                 // STARTSUBQUEUE：檔案數
                case 5:case 8:case 0xb:case 0x19:last_error_=0;return finish(op_doit);          // START*／STARTREGISTRATION
                case 6:case 9:case 0xc:return progress(1,0,0);                                  // END*：每完成一個檔案
                case 0xf:case 0x18:case 0x20:last_error_=0;return finish(0);                    // QUEUESCAN／QUEUESCAN_EX／ENDREGISTRATION
                default:break;
                }
                // 檔案已存在／目的較新／語言不符：互動模式詢問是否覆寫，非互動模式保留目的檔。
                if(notification&0x70000U){
                    if(!interactive){last_error_=0;return finish(0);}
                    const auto box=resolve("user32.dll","MessageBoxW").value;const auto paths=p1;const auto owner=callback.owner;
                    return nested_continuation(state,memory,"SetupQueueOverwrite",[this,service,box,owner,paths,unicode](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);const auto target_at=m.load(GuestAddress{paths},x86_?32:64);
                        const auto target=target_at?(unicode?read_utf16(m,GuestAddress{target_at},32768):utf8_to_utf16(read_ansi(m,target_at,32768))):std::u16string{};
                        std::uint32_t pressed=0;
                        {GuestScratch scratch(*this,m,s);const auto text=scratch.text(u"The file \""+target+u"\" already exists or is newer than the file being copied.\n\nOverwrite it?"),caption=scratch.text(u"Confirm File Replace");
                         if(!text || !caption)throw std::bad_alloc{};pressed=co_await service_callback::call(box,{owner,text,caption,0x34});}
                        inner.return_to(s,pressed==6?1:0);co_return std::optional<std::uint32_t>{};});
                }
                if(notification!=0x7 && notification!=0xa && notification!=0xd && notification!=0xe){last_error_=0;return finish(0);}
                // 錯誤與換片：以 A／W 對應的提示函式（客體入口）詢問，DPROMPT_SUCCESS 對應重試或新路徑。
                const auto read_string=[&](std::uint64_t at){return at?read_text(at):std::u16string{};};
                const auto bits=width;const auto load_pointer=[&](std::uint64_t base,unsigned slot){return memory.load(GuestAddress{base+slot*unit},bits);};
                std::vector<std::u16string> strings;std::vector<std::uint64_t> values;std::string target_name;
                const auto owner=callback.owner;
                if(notification==0xe){ // NEEDMEDIA：SOURCE_MEDIA {Reserved, Tagfile, Description, SourcePath, SourceFile, Flags}；Param2 為 MAX_PATH 新路徑緩衝區
                    const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p1+5*unit},32));
                    strings={read_string(load_pointer(p1,2)),read_string(load_pointer(p1,3)),read_string(load_pointer(p1,4)),read_string(load_pointer(p1,1))};
                    values={0x100U|((flags&copy_noskip)?2U:0U)|((flags&copy_nobrowse)?1U:0U)};target_name=unicode?"SetupPromptForDiskW":"SetupPromptForDiskA";
                }else{ // FILEPATHS {Target, Source, Win32Error, Flags}
                    const auto target=read_string(load_pointer(p1,0)),source=read_string(load_pointer(p1,1));const auto error=memory.load(GuestAddress{p1+2*unit},32),flags=memory.load(GuestAddress{p1+2*unit+4},32);
                    if(notification==0xd){
                        const auto slash=source.find_last_of(u"\\/");strings={slash==std::u16string::npos?std::u16string{}:source.substr(0,slash),slash==std::u16string::npos?source:source.substr(slash+1),target};
                        values={error,(flags&copy_noskip)?2U:0U};target_name=unicode?"SetupCopyErrorW":"SetupCopyErrorA";
                    }else if(notification==0xa){strings={source,target};values={error,0};target_name=unicode?"SetupRenameErrorW":"SetupRenameErrorA";}
                    else {strings={target};values={error,0};target_name=unicode?"SetupDeleteErrorW":"SetupDeleteErrorA";}
                }
                const auto dialog=resolve("setupapi.dll",target_name).value;
                return nested_continuation(state,memory,"SetupQueueDialog",
                    [this,service,dialog,notification,owner,strings,values,p2,encode,unicode](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);std::vector<std::uint64_t> blocks;
                        const auto put=[&](const std::u16string& value)->std::uint64_t{const auto bytes=encode(value);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},bytes);return at;};
                        std::uint32_t answer=0;std::u16string chosen;
                        try{
                            if(notification==0xe){
                                const auto path=put(strings[1]);const auto out=allocate_heap(m,max_path*(unicode?2:1));if(!out)throw std::bad_alloc{};blocks.push_back(out);
                                const auto result=co_await service_callback::call(dialog,{owner,0,strings[0].empty()?0:put(strings[0]),path,put(strings[2]),strings[3].empty()?0:put(strings[3]),values[0],out,max_path,0});
                                if(result==prompt_success){chosen=unicode?read_utf16(m,GuestAddress{out},max_path):utf8_to_utf16(read_ansi(m,out,max_path));answer=chosen==strings[1]?op_doit:op_newpath;}
                                else answer=result==prompt_skip?op_skip:op_abort;
                            }else if(notification==0xd){
                                const auto out=allocate_heap(m,max_path*(unicode?2:1));if(!out)throw std::bad_alloc{};blocks.push_back(out);
                                const auto result=co_await service_callback::call(dialog,{owner,0,0,put(strings[0]),put(strings[1]),put(strings[2]),values[0],values[1],out,max_path,0});
                                if(result==prompt_success){chosen=unicode?read_utf16(m,GuestAddress{out},max_path):utf8_to_utf16(read_ansi(m,out,max_path));answer=chosen==strings[0]?op_doit:op_newpath;}
                                else answer=result==prompt_skip?op_skip:op_abort;
                            }else{
                                std::uint32_t result=0;
                                if(notification==0xa)result=co_await service_callback::call(dialog,{owner,0,put(strings[0]),put(strings[1]),values[0],0});
                                else result=co_await service_callback::call(dialog,{owner,0,put(strings[0]),values[0],0});
                                answer=result==prompt_success?op_doit:result==prompt_skip?op_skip:op_abort;
                            }
                            if(answer==op_newpath && p2){const auto bytes=encode(chosen);if(bytes.size()>max_path*(unicode?2U:1U))answer=op_abort;else m.write(GuestAddress{p2},bytes);}
                        }catch(...){for(const auto block:blocks)(void)free_heap(m,block,s);throw;}
                        for(const auto block:blocks)(void)free_heap(m,block,s);
                        last_error_=answer==op_abort?cancelled:0;inner.return_to(s,answer);co_return std::optional<std::uint32_t>{};
                    });
            }
            // 提交與掃描的步驟：組合完整路徑與來源媒體（根目錄＋相對路徑）。
            struct Step {std::u16string source,target,media,relative,description,tagfile,security;std::uint32_t style{};};
            const auto copy_steps=[&](const State::FileQueue& queue){
                std::vector<Step> steps;std::vector<std::u16string> order;std::map<std::u16string,std::vector<Step>> groups;
                for(const auto& copy:queue.copies){
                    Step step;step.style=copy.style;step.description=copy.description;step.tagfile=copy.tagfile;step.security=copy.security;
                    if(copy.style&copy_source_absolute){step.source=full_path(copy.file);const auto slash=step.source.find_last_of(u'\\');step.media=step.source.substr(0,slash==std::u16string::npos?0:slash);step.relative=slash==std::u16string::npos?step.source:step.source.substr(slash+1);}
                    else if(copy.style&copy_sourcepath_absolute){step.media=full_path(copy.path);step.relative=copy.file;step.source=join(step.media,step.relative);}
                    else {step.media=copy.root.empty()?copy.root:full_path(copy.root);step.relative=join(copy.path,copy.file);step.source=step.media.empty()?full_path(step.relative):join(step.media,step.relative);}
                    while(!step.relative.empty() && (step.relative.front()==u'\\' || step.relative.front()==u'/'))step.relative.erase(0,1);
                    const auto leaf=copy.file.substr(copy.file.find_last_of(u"\\/")==std::u16string::npos?0:copy.file.find_last_of(u"\\/")+1);
                    step.target=join(copy.target_directory,copy.target_file.empty()?leaf:copy.target_file);
                    const auto key=fold(step.media)+u'\n'+step.description+u'\n'+step.tagfile;
                    if(!groups.contains(key))order.push_back(key);groups[key].push_back(std::move(step));
                }
                for(const auto& key:order)for(auto& step:groups[key])steps.push_back(std::move(step));
                return steps;
            };
            if(name.starts_with("SetupScanFileQueue")){
                // (Queue, Flags, Window, CallbackRoutine, CallbackContext, Result)：PRESENCE／VALIDITY 掃描目的檔（可修剪佇列），USE_CALLBACK(EX) 逐項通知。
                // 沙箱沒有驅動程式目錄簽章資料庫：VALIDITY 以目的檔存在且內容與佇列來源相同判定；SPQ_SCAN_INFORM_USER 不另外提示。
                auto& queue=queue_of(arg(0));const auto flags=static_cast<std::uint32_t>(arg(1));const auto mode=flags&0x4fU;
                if(!arg(5) || std::popcount(mode)!=1 || (flags&~0xffU) || mode==0x40)return fail(87);
                memory.check(GuestAddress{arg(5)},4,Access::Write);
                if(mode==1 || mode==2){
                    const auto steps=copy_steps(queue);bool all=true;std::set<std::u16string> satisfied;
                    for(const auto& step:steps){
                        bool ok=exists(step.target);
                        if(ok && mode==2){try{ok=*driver_store::read_file(filesystem_,step.target)==*driver_store::read_file(filesystem_,step.source);}catch(const driver_store::Error&){ok=false;}}
                        if(ok)satisfied.insert(fold(step.target));else all=false;
                    }
                    if(flags&0x20U)std::erase_if(queue.copies,[&](const State::QueuedCopy& copy){
                        const auto leaf=copy.file.substr(copy.file.find_last_of(u"\\/")==std::u16string::npos?0:copy.file.find_last_of(u"\\/")+1);
                        return !(copy.style&0x100000U) && satisfied.contains(fold(join(copy.target_directory,copy.target_file.empty()?leaf:copy.target_file)));});
                    if(flags&0x80U){std::erase_if(queue.deletes,[&](const State::QueuedPath& item){return !exists(item.target);});std::erase_if(queue.renames,[&](const State::QueuedPath& item){return !exists(item.source);});}
                    memory.store(GuestAddress{arg(5)},32,all?1:0);return succeed();
                }
                if(!arg(3))return fail(87);
                const auto steps=copy_steps(queue);const auto callback=arg(3),context=arg(4),result_at=arg(5);const bool extended=mode==8;
                return nested_continuation(state,memory,"SetupScanFileQueue",[this,service,steps,callback,context,result_at,extended,encode](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall inner(s,m,service);const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);std::uint32_t verdict=0;
                    for(const auto& step:steps){
                        std::vector<std::uint64_t> blocks;
                        const auto put=[&](const std::u16string& value)->std::uint64_t{const auto bytes=encode(value);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},bytes);return at;};
                        std::uint64_t param=put(step.target);
                        if(extended){const auto paths=allocate_heap(m,2*size+8);if(!paths)throw std::bad_alloc{};blocks.push_back(paths);m.store(GuestAddress{paths},bits,param);m.store(GuestAddress{paths+size},bits,put(step.source));
                            m.store(GuestAddress{paths+2*size},32,0);m.store(GuestAddress{paths+2*size+4},32,step.style);param=paths;}
                        verdict=co_await service_callback::call(callback,{context,extended?0x18U:0xfU,param,0});
                        for(const auto block:blocks)(void)free_heap(m,block,s);
                        if(verdict)break;
                    }
                    m.store(GuestAddress{result_at},32,0);last_error_=verdict;inner.return_to(s,verdict?0:1);co_return std::optional<std::uint32_t>{};
                });
            }
            if(name=="SetupCommitFileQueue" || name=="SetupCommitFileQueueA" || name=="SetupCommitFileQueueW"){
                // (Owner, Queue, MsgHandler, Context)：開始佇列 → 刪除、更名、複製子佇列（非空才通知）→ 結束佇列；回呼回傳 FILEOP_ABORT 時中止並回傳 FALSE。
                const auto& queue=queue_of(arg(1));if(!arg(2))return fail(87);
                const auto copies=copy_steps(queue);std::vector<Step> renames,deletes;
                for(const auto& item:queue.renames){Step step;step.source=item.source;step.target=item.target;renames.push_back(std::move(step));}
                for(const auto& item:queue.deletes){Step step;step.target=item.target;deletes.push_back(std::move(step));}
                const auto owner=arg(0),queue_handle=arg(1),handler=arg(2),context=arg(3);
                const auto convert=resolve("advapi32.dll","ConvertStringSecurityDescriptorToSecurityDescriptorW").value,set_security=resolve("advapi32.dll","SetFileSecurityW").value,local_free=resolve("kernel32.dll","LocalFree").value;
                return nested_continuation(state,memory,"SetupCommitFileQueue",
                    [this,service,owner,queue_handle,handler,context,copies,renames,deletes,encode,unicode,convert,set_security,local_free,join,host_path,file_present,version_compare,install_copy](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall inner(s,m,service);const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);
                        std::vector<std::uint64_t> blocks;
                        const auto release=[&]{for(const auto block:blocks)(void)free_heap(m,block,s);blocks.clear();};
                        const auto put=[&](const std::u16string& value)->std::uint64_t{if(value.empty())return 0;const auto bytes=encode(value);const auto at=allocate_heap(m,bytes.size());if(!at)throw std::bad_alloc{};blocks.push_back(at);m.write(GuestAddress{at},bytes);return at;};
                        const auto block=[&](std::size_t length)->std::uint64_t{const auto at=allocate_heap(m,length);if(!at)throw std::bad_alloc{};blocks.push_back(at);std::vector<std::uint8_t> zero(length);m.write(GuestAddress{at},zero);return at;};
                        const auto paths=[&](const Step& step,std::uint32_t error,std::uint32_t flags){const auto at=block(2*size+8);m.store(GuestAddress{at},bits,put(step.target));m.store(GuestAddress{at+size},bits,put(step.source));
                            m.store(GuestAddress{at+2*size},32,error);m.store(GuestAddress{at+2*size+4},32,flags);return at;};
                        const auto read_back=[&](std::uint64_t at){return unicode?read_utf16(m,GuestAddress{at},max_path):utf8_to_utf16(read_ansi(m,at,max_path));};
                        const auto host=host_path;const auto exists_file=file_present;const auto compare=version_compare;
                        bool aborted=false,modified=false;std::uint32_t failure=0;
                        const auto notify=[&](std::uint32_t code,std::uint64_t p1,std::uint64_t p2){return service_callback::call(handler,{context,code,p1,p2});};
                        if(!(co_await notify(1,owner,0))){aborted=true;failure=cancelled;}
                        for(unsigned phase=0;phase<3 && !aborted;++phase){
                            const auto& list=phase==0?deletes:phase==1?renames:copies;if(list.empty())continue;
                            const std::uint32_t op=phase==0?2U:phase==1?1U:0U;
                            if(!(co_await notify(3,op,list.size()))){aborted=true;failure=cancelled;break;}
                            std::optional<std::u16string> media_key;std::u16string media_root;bool media_skip=false;
                            for(const auto& original:list){
                                Step step=original;
                                if(op==0){
                                    const auto key=fold(step.media)+u'\n'+step.description+u'\n'+step.tagfile;
                                    if(media_key!=key){
                                        media_key=key;media_skip=false;media_root=step.media;
                                        const auto media=block(5*size+8);m.store(GuestAddress{media+size},bits,put(step.tagfile));m.store(GuestAddress{media+2*size},bits,put(step.description));
                                        m.store(GuestAddress{media+3*size},bits,put(step.media));m.store(GuestAddress{media+4*size},bits,put(step.relative));m.store(GuestAddress{media+5*size},32,step.style);
                                        const auto out=block(max_path*(unicode?2:1));const auto answer=co_await notify(0xe,media,out);
                                        if(answer==op_newpath)media_root=full_windows_environment_path(read_back(out),runtime_.current_directory,runtime_.environment);
                                        release();
                                        if(answer==op_abort){aborted=true;failure=cancelled;break;}
                                        if(answer==op_skip)media_skip=true;
                                    }
                                    if(media_skip)continue;
                                    step.source=media_root.empty()?full_windows_environment_path(step.relative,runtime_.current_directory,runtime_.environment):join(media_root,step.relative);
                                }
                                const std::uint32_t start=op==0?0xbU:op==1?8U:5U,end=start+1,error_code=op==0?0xdU:op==1?0xaU:7U;
                                const auto answer=co_await notify(start,paths(step,0,step.style),op);release();
                                if(answer==op_abort){aborted=true;failure=cancelled;break;}
                                if(answer==op_skip)continue;
                                bool perform=true;
                                if(op==0 && exists_file(step.target)){
                                    if(step.style&copy_force_no_overwrite)perform=false;
                                    else if(step.style&copy_no_overwrite){perform=(co_await notify(0x20000,paths(step,0,step.style),0))!=0;release();}
                                    if(perform && (step.style&(copy_newer_only|copy_force_newer)))perform=compare(step.source,step.target)>0;
                                    else if(perform && (step.style&copy_newer) && compare(step.source,step.target)<=0){perform=(co_await notify(0x40000,paths(step,0,step.style),0))!=0;release();}
                                }else if(op==0 && (step.style&copy_replace_only))perform=false;
                                if(!perform)continue;
                                std::uint32_t error=0;
                                for(;;){
                                    if(op==2){error=filesystem_.remove(host(step.target),false);if(error==2 || error==3)error=0;}
                                    else if(op==1)error=filesystem_.rename(host(step.source),host(step.target),false);
                                    else error=install_copy(step.source,step.target,step.style,true);
                                    if(!error)break;
                                    const auto out=op==0?block(max_path*(unicode?2:1)):0;const auto reply=co_await notify(error_code,paths(step,error,step.style),out);
                                    std::u16string replacement;if(reply==op_newpath && out)replacement=read_back(out);release();
                                    if(reply==op_abort){aborted=true;failure=error;break;}
                                    if(reply==op_skip)break;
                                    if(reply==op_newpath && op==0){const auto slash=step.relative.find_last_of(u"\\/");step.source=join(full_windows_environment_path(replacement,runtime_.current_directory,runtime_.environment),slash==std::u16string::npos?step.relative:step.relative.substr(slash+1));}
                                }
                                if(aborted)break;
                                if(!error){
                                    modified=true;
                                    if(op==0 && (step.style&copy_delete_source))(void)filesystem_.remove(host(step.source),false);
                                    if(op==0 && !step.security.empty()){
                                        // SP_FILE_COPY_PARAMS 的 SDDL：轉成自相對描述元後套用 DACL（含擁有者與群組）到目的檔。
                                        GuestScratch scratch(*this,m,s);const auto sddl=scratch.text(step.security),out=scratch.allocate(size),file=scratch.text(step.target);if(!sddl || !out || !file)throw std::bad_alloc{};
                                        m.store(GuestAddress{out},bits,0);
                                        if(co_await service_callback::call(convert,{sddl,1,out,0})){const auto descriptor=m.load(GuestAddress{out},bits);(void)co_await service_callback::call(set_security,{file,0x7,descriptor});(void)co_await service_callback::call_value(local_free,{descriptor});}
                                    }
                                    (void)co_await notify(end,paths(step,0,step.style),0);release();
                                }
                            }
                            if(aborted)break;
                            (void)co_await notify(4,op,0);
                        }
                        (void)co_await notify(2,aborted?0:1,0);release();
                        if(modified)if(const auto found=setup_inf_->file_queues.find(queue_handle);found!=setup_inf_->file_queues.end())found->second.flags|=4U;
                        last_error_=aborted?failure:0;inner.return_to(s,aborted?0:1);co_return std::optional<std::uint32_t>{};
                    });
            }
        }
